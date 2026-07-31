/*
 * AppX archive entry streams
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <zlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/appxsvc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define ZIP_METHOD_STORE                  0
#define ZIP_METHOD_DEFLATE                8
#define STREAM_INPUT_SIZE                 (64 * 1024)
#define STREAM_CANCEL_ATTEMPTS             16
#define STREAM_CANCEL_RETRY_MS              1

enum stream_io_state
{
    /* No ReadFile call or registered overlapped request exists. */
    STREAM_IO_IDLE,
    /* OVERLAPPED and io_thread_id are published; ReadFile has not returned. */
    STREAM_IO_STARTING,
    /* ReadFile returned ERROR_IO_PENDING; CancelIoEx can target the request. */
    STREAM_IO_PENDING,
};

/*
 * archive.c provides this private ownership boundary.  It validates index,
 * copies the immutable public entry metadata, and reopens the archive's locked
 * file as an independent FILE_FLAG_OVERLAPPED handle.  On success the caller
 * owns *file and the handle remains valid independently of the archive
 * object's lifetime.
 */
extern HRESULT appx_archive_acquire_entry_source( WINE_APPX_ARCHIVE *archive, UINT32 index,
                                                  WINE_APPX_ARCHIVE_ENTRY *entry,
                                                  HANDLE *file );

struct wine_appx_archive_stream
{
    HANDLE file;
    HANDLE event;
    HANDLE idle_event;
    OVERLAPPED io;
    CRITICAL_SECTION lifecycle_cs;
    CRITICAL_SECTION io_cs;
    z_stream zstream;
    HRESULT status;
    LONG cancelled;
    LONG read_active;
    LONG active_calls;
    UINT32 expected_crc32;
    UINT32 crc32;
    UINT16 method;
    UINT64 data_offset;
    UINT64 compressed_size;
    UINT64 uncompressed_size;
    UINT64 compressed_loaded;
    UINT64 compressed_consumed;
    UINT64 uncompressed_read;
    DWORD io_thread_id;
    enum stream_io_state io_state;
    BOOL inflate_initialized;
    BOOL completed;
    BOOL closing;
    BYTE input[STREAM_INPUT_SIZE];
};

static void *stream_zalloc( void *opaque, unsigned int items, unsigned int size )
{
    SIZE_T bytes;

    if (size && items > ~(SIZE_T)0 / size) return NULL;
    bytes = (SIZE_T)items * size;
    return HeapAlloc( GetProcessHeap(), 0, bytes ? bytes : 1 );
}

static void stream_zfree( void *opaque, void *ptr )
{
    HeapFree( GetProcessHeap(), 0, ptr );
}

/*
 * A successful read returns S_OK and at least one byte.  S_FALSE is returned
 * only after the compressed stream, exact byte counts, and CRC have all been
 * verified.  Callers must therefore keep output in transaction staging until
 * they observe S_FALSE; a later integrity failure makes all earlier chunks
 * untrusted.  Failures are sticky and report zero bytes.  read() is
 * single-consumer.  cancel() and close() may race a read already in progress;
 * close() cancels and drains calls which entered before it marked the stream
 * closing.  As with any released object, callers must not start an operation
 * after close() begins.
 */
static BOOL stream_enter( WINE_APPX_ARCHIVE_STREAM *stream )
{
    BOOL entered = FALSE;

    EnterCriticalSection( &stream->lifecycle_cs );
    if (!stream->closing)
    {
        if (!stream->active_calls) ResetEvent( stream->idle_event );
        stream->active_calls++;
        entered = TRUE;
    }
    LeaveCriticalSection( &stream->lifecycle_cs );
    return entered;
}

static void stream_leave( WINE_APPX_ARCHIVE_STREAM *stream )
{
    EnterCriticalSection( &stream->lifecycle_cs );
    if (!--stream->active_calls) SetEvent( stream->idle_event );
    LeaveCriticalSection( &stream->lifecycle_cs );
}

static HRESULT stream_fail( WINE_APPX_ARCHIVE_STREAM *stream, HRESULT hr )
{
    if (SUCCEEDED(stream->status)) stream->status = hr;
    return stream->status;
}

static BOOL stream_is_cancelled( WINE_APPX_ARCHIVE_STREAM *stream )
{
    return InterlockedCompareExchange( &stream->cancelled, 0, 0 ) != 0;
}

static HRESULT stream_read_at( WINE_APPX_ARCHIVE_STREAM *stream, UINT64 offset,
                               void *buffer, UINT32 size )
{
    BOOL pending = FALSE, success;
    DWORD error, read = 0;

    if (!size) return S_OK;

    EnterCriticalSection( &stream->io_cs );
    if (stream_is_cancelled( stream ))
    {
        LeaveCriticalSection( &stream->io_cs );
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    }
    memset( &stream->io, 0, sizeof(stream->io) );
    stream->io.Offset = offset;
    stream->io.OffsetHigh = offset >> 32;
    stream->io.hEvent = stream->event;
    ResetEvent( stream->event );
    stream->io_thread_id = GetCurrentThreadId();
    stream->io_state = STREAM_IO_STARTING;
    LeaveCriticalSection( &stream->io_cs );

    EnterCriticalSection( &stream->io_cs );
    if (stream_is_cancelled( stream ))
    {
        stream->io_thread_id = 0;
        stream->io_state = STREAM_IO_IDLE;
        LeaveCriticalSection( &stream->io_cs );
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    }
    LeaveCriticalSection( &stream->io_cs );
    success = ReadFile( stream->file, buffer, size, &read, &stream->io );

    EnterCriticalSection( &stream->io_cs );
    if (!success)
    {
        error = GetLastError();
        pending = error == ERROR_IO_PENDING;
        stream->io_state = pending ? STREAM_IO_PENDING : STREAM_IO_IDLE;
    }
    else
    {
        error = ERROR_SUCCESS;
        stream->io_state = STREAM_IO_IDLE;
    }
    stream->io_thread_id = 0;
    if (pending && stream_is_cancelled( stream ))
        CancelIoEx( stream->file, &stream->io );
    LeaveCriticalSection( &stream->io_cs );

    if (pending)
    {
        success = GetOverlappedResult( stream->file, &stream->io, &read, TRUE );
        error = success ? ERROR_SUCCESS : GetLastError();
        EnterCriticalSection( &stream->io_cs );
        stream->io_state = STREAM_IO_IDLE;
        LeaveCriticalSection( &stream->io_cs );
    }
    if (!success)
    {
        if (stream_is_cancelled( stream ) || error == ERROR_OPERATION_ABORTED)
            return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        if (error == ERROR_HANDLE_EOF) return APPX_E_CORRUPT_CONTENT;
        return HRESULT_FROM_WIN32( error );
    }
    if (stream_is_cancelled( stream ))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (read != size) return APPX_E_CORRUPT_CONTENT;
    return S_OK;
}

static void stream_cancel_io( WINE_APPX_ARCHIVE_STREAM *stream )
{
    enum stream_io_state state;
    HANDLE thread;
    UINT32 attempt;

    InterlockedExchange( &stream->cancelled, 1 );
    for (attempt = 0; attempt < STREAM_CANCEL_ATTEMPTS; attempt++)
    {
        EnterCriticalSection( &stream->io_cs );
        state = stream->io_state;
        if (state != STREAM_IO_IDLE)
            CancelIoEx( stream->file, &stream->io );
        if (state == STREAM_IO_STARTING && stream->io_thread_id)
        {
            /*
             * Keep io_cs held until the cancellation call returns.  Otherwise
             * the reader could leave stream_read_at() and begin unrelated
             * synchronous I/O before the thread handle is cancelled.
             */
            if ((thread = OpenThread( THREAD_TERMINATE, FALSE,
                                      stream->io_thread_id )))
            {
                CancelSynchronousIo( thread );
                CloseHandle( thread );
            }
        }
        LeaveCriticalSection( &stream->io_cs );

        if (state != STREAM_IO_STARTING) break;

        /*
         * CancelIoEx() can run just before ReadFile() registers its request,
         * and CancelSynchronousIo() can run just before the target enters the
         * syscall.  Give that transition a short finite handoff window.  The
         * reader also observes the sticky cancelled flag and cancels a newly
         * pending request after ReadFile() returns.  Public cancel therefore
         * remains bounded even if a host filesystem ignores both mechanisms.
         */
        Sleep( STREAM_CANCEL_RETRY_MS );
    }
}

static HRESULT stream_finish( WINE_APPX_ARCHIVE_STREAM *stream )
{
    if (stream_is_cancelled( stream ))
        return stream_fail( stream, HRESULT_FROM_WIN32(ERROR_CANCELLED) );
    if (stream->compressed_loaded != stream->compressed_size ||
        stream->compressed_consumed != stream->compressed_size ||
        stream->uncompressed_read != stream->uncompressed_size ||
        stream->crc32 != stream->expected_crc32)
        return stream_fail( stream, APPX_E_CORRUPT_CONTENT );

    stream->completed = TRUE;
    return S_OK;
}

static HRESULT stream_read_stored( WINE_APPX_ARCHIVE_STREAM *stream, BYTE *buffer,
                                   UINT32 capacity, UINT32 *read )
{
    UINT64 remaining = stream->uncompressed_size - stream->uncompressed_read;
    UINT32 count = remaining < capacity ? (UINT32)remaining : capacity;
    HRESULT hr;

    if (!count)
    {
        if (FAILED(hr = stream_finish( stream ))) return hr;
        return S_FALSE;
    }
    if (FAILED(hr = stream_read_at( stream, stream->data_offset + stream->compressed_loaded,
                                    buffer, count )))
        return stream_fail( stream, hr );

    stream->compressed_loaded += count;
    stream->compressed_consumed += count;
    stream->uncompressed_read += count;
    stream->crc32 = crc32( stream->crc32, buffer, count );

    if (stream->uncompressed_read == stream->uncompressed_size &&
        FAILED(hr = stream_finish( stream )))
        return hr;
    *read = count;
    return S_OK;
}

static HRESULT stream_fill_input( WINE_APPX_ARCHIVE_STREAM *stream )
{
    UINT64 remaining = stream->compressed_size - stream->compressed_loaded;
    UINT32 count = remaining < sizeof(stream->input) ? (UINT32)remaining :
                                                        sizeof(stream->input);
    HRESULT hr;

    if (!count) return S_FALSE;
    if (FAILED(hr = stream_read_at( stream, stream->data_offset + stream->compressed_loaded,
                                    stream->input, count )))
        return hr;

    stream->compressed_loaded += count;
    stream->zstream.next_in = stream->input;
    stream->zstream.avail_in = count;
    return S_OK;
}

static HRESULT stream_read_deflated( WINE_APPX_ARCHIVE_STREAM *stream, BYTE *buffer,
                                     UINT32 capacity, UINT32 *read )
{
    BYTE overflow;
    UINT32 produced = 0;
    HRESULT hr;

    while (!stream->completed && produced < capacity)
    {
        UINT64 remaining_output;
        UINT32 output_capacity, before_input, before_output, consumed, emitted;
        BYTE *output;
        int result;

        if (stream_is_cancelled( stream ))
            return stream_fail( stream, HRESULT_FROM_WIN32(ERROR_CANCELLED) );
        if (!stream->zstream.avail_in && stream->compressed_loaded < stream->compressed_size)
        {
            if (FAILED(hr = stream_fill_input( stream )))
                return stream_fail( stream, hr );
        }

        remaining_output = stream->uncompressed_size - stream->uncompressed_read;
        if (remaining_output)
        {
            output_capacity = capacity - produced;
            if (remaining_output < output_capacity) output_capacity = remaining_output;
            output = buffer + produced;
        }
        else
        {
            /*
             * Give inflate one private byte after the declared output length.
             * Emitting it proves that the central-directory size was false;
             * the byte is never returned to the caller.
             */
            output_capacity = 1;
            output = &overflow;
        }

        before_input = stream->zstream.avail_in;
        before_output = output_capacity;
        stream->zstream.next_out = output;
        stream->zstream.avail_out = output_capacity;
        result = inflate( &stream->zstream, Z_NO_FLUSH );
        consumed = before_input - stream->zstream.avail_in;
        emitted = before_output - stream->zstream.avail_out;
        stream->compressed_consumed += consumed;

        if (!remaining_output && emitted)
            return stream_fail( stream, APPX_E_CORRUPT_CONTENT );
        if (emitted)
        {
            stream->crc32 = crc32( stream->crc32, buffer + produced, emitted );
            stream->uncompressed_read += emitted;
            produced += emitted;
        }

        if (result == Z_STREAM_END)
        {
            if (stream->zstream.avail_in ||
                FAILED(hr = stream_finish( stream )))
                return stream_fail( stream, stream->zstream.avail_in ?
                                    APPX_E_CORRUPT_CONTENT : hr );
            break;
        }
        if (result != Z_OK || (!consumed && !emitted))
            return stream_fail( stream, APPX_E_CORRUPT_CONTENT );
    }

    if (!produced && stream->completed) return S_FALSE;
    *read = produced;
    return S_OK;
}

HRESULT WINAPI wine_appx_archive_stream_open( WINE_APPX_ARCHIVE *archive, UINT32 index,
                                              WINE_APPX_ARCHIVE_STREAM **result )
{
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE_ENTRY entry = {sizeof(entry)};
    LARGE_INTEGER file_size;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;
    int zresult;

    TRACE( "archive %p, index %u, result %p.\n", archive, index, result );

    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!archive) return E_INVALIDARG;
    if (FAILED(hr = appx_archive_acquire_entry_source( archive, index, &entry, &file )))
        return hr;
    if (entry.flags & WINE_APPX_ENTRY_DIRECTORY)
    {
        CloseHandle( file );
        return HRESULT_FROM_WIN32( ERROR_DIRECTORY );
    }
    if (entry.compression_method != ZIP_METHOD_STORE &&
        entry.compression_method != ZIP_METHOD_DEFLATE)
    {
        CloseHandle( file );
        return APPX_E_CORRUPT_CONTENT;
    }
    if (!GetFileSizeEx( file, &file_size ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( file );
        return hr;
    }
    if (file_size.QuadPart < 0 || entry.data_offset > (UINT64)file_size.QuadPart ||
        entry.compressed_size > (UINT64)file_size.QuadPart - entry.data_offset)
    {
        CloseHandle( file );
        return APPX_E_CORRUPT_CONTENT;
    }
    if (!(stream = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream) )))
    {
        CloseHandle( file );
        return E_OUTOFMEMORY;
    }
    if (!(stream->event = CreateEventW( NULL, TRUE, FALSE, NULL )) ||
        !(stream->idle_event = CreateEventW( NULL, TRUE, TRUE, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );

        if (stream->event) CloseHandle( stream->event );
        CloseHandle( file );
        HeapFree( GetProcessHeap(), 0, stream );
        return hr;
    }

    InitializeCriticalSection( &stream->lifecycle_cs );
    InitializeCriticalSection( &stream->io_cs );
    stream->file = file;
    stream->status = S_OK;
    stream->expected_crc32 = entry.crc32;
    stream->crc32 = crc32( 0, Z_NULL, 0 );
    stream->method = entry.compression_method;
    stream->data_offset = entry.data_offset;
    stream->compressed_size = entry.compressed_size;
    stream->uncompressed_size = entry.uncompressed_size;

    if (stream->method == ZIP_METHOD_DEFLATE)
    {
        stream->zstream.zalloc = stream_zalloc;
        stream->zstream.zfree = stream_zfree;
        zresult = inflateInit2( &stream->zstream, -MAX_WBITS );
        if (zresult != Z_OK)
        {
            DeleteCriticalSection( &stream->io_cs );
            DeleteCriticalSection( &stream->lifecycle_cs );
            CloseHandle( stream->idle_event );
            CloseHandle( stream->event );
            CloseHandle( stream->file );
            HeapFree( GetProcessHeap(), 0, stream );
            return zresult == Z_MEM_ERROR ? E_OUTOFMEMORY : APPX_E_CORRUPT_CONTENT;
        }
        stream->inflate_initialized = TRUE;
    }

    *result = stream;
    return S_OK;
}

HRESULT WINAPI wine_appx_archive_stream_read( WINE_APPX_ARCHIVE_STREAM *stream,
                                              void *buffer, UINT32 capacity, UINT32 *read )
{
    HRESULT hr;

    if (!read) return E_INVALIDARG;
    *read = 0;
    if (!stream || !buffer || !capacity) return E_INVALIDARG;
    if (!stream_enter( stream ))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (InterlockedCompareExchange( &stream->read_active, 1, 0 ))
    {
        stream_leave( stream );
        return HRESULT_FROM_WIN32( ERROR_BUSY );
    }

    if (FAILED(stream->status))
        hr = stream->status;
    else if (stream->completed)
        hr = S_FALSE;
    else if (stream_is_cancelled( stream ))
        hr = stream_fail( stream, HRESULT_FROM_WIN32(ERROR_CANCELLED) );
    else if (stream->method == ZIP_METHOD_STORE)
        hr = stream_read_stored( stream, buffer, capacity, read );
    else
        hr = stream_read_deflated( stream, buffer, capacity, read );

    InterlockedExchange( &stream->read_active, 0 );
    stream_leave( stream );
    return hr;
}

void WINAPI wine_appx_archive_stream_cancel( WINE_APPX_ARCHIVE_STREAM *stream )
{
    if (!stream) return;
    if (!stream_enter( stream )) return;
    stream_cancel_io( stream );
    stream_leave( stream );
}

void WINAPI wine_appx_archive_stream_close( WINE_APPX_ARCHIVE_STREAM *stream )
{
    if (!stream) return;

    EnterCriticalSection( &stream->lifecycle_cs );
    if (stream->closing)
    {
        LeaveCriticalSection( &stream->lifecycle_cs );
        return;
    }
    stream->closing = TRUE;
    LeaveCriticalSection( &stream->lifecycle_cs );
    stream_cancel_io( stream );
    WaitForSingleObject( stream->idle_event, INFINITE );
    /*
     * The last active caller signals idle_event while it still owns the
     * lifecycle lock.  Acquire it once more before destruction so no caller
     * can still be unwinding through stream_leave().
     */
    EnterCriticalSection( &stream->lifecycle_cs );
    LeaveCriticalSection( &stream->lifecycle_cs );

    if (stream->inflate_initialized) inflateEnd( &stream->zstream );
    DeleteCriticalSection( &stream->io_cs );
    DeleteCriticalSection( &stream->lifecycle_cs );
    CloseHandle( stream->idle_event );
    CloseHandle( stream->event );
    CloseHandle( stream->file );
    HeapFree( GetProcessHeap(), 0, stream );
}
