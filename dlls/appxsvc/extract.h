/*
 * AppX package payload extraction interfaces
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

#ifndef __WINE_APPXSVC_EXTRACT_H
#define __WINE_APPXSVC_EXTRACT_H

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "package.h"

#define APPX_EXTRACT_DEFAULT_BUFFER_SIZE (256u * 1024)
#define APPX_EXTRACT_MAX_BUFFER_SIZE     (1024u * 1024)

/*
 * Every successful file flush completed, but at least one containing-directory
 * flush was unavailable.  All names and bytes are present and verified; the
 * caller must not mistake this for full power-loss durability.
 */
#define APPX_EXTRACT_S_WEAK_DURABILITY S_FALSE

typedef struct
{
    UINT32 size;
    UINT32 flags;

    /*
     * Zero applies no limit in addition to the archive inspection limits.
     * A nonzero value is checked before any child is created.
     */
    UINT64 max_expanded_bytes;

    /*
     * Free bytes which must remain after extraction.  Both the initial
     * expanded-size reservation and the final remaining space are checked
     * through the staging-directory handle, never through a path.
     */
    UINT64 free_space_floor_bytes;

    /*
     * Optional waitable cancellation object.  Signalling is made sticky for
     * the duration of the call.  The object may be closed after this function
     * returns; the extractor owns a duplicate while it is running.
     */
    HANDLE cancel_event;

    /*
     * Zero selects APPX_EXTRACT_DEFAULT_BUFFER_SIZE.  Nonzero values through
     * APPX_EXTRACT_MAX_BUFFER_SIZE are accepted.  Memory use is independent
     * of the largest payload file.
     */
    UINT32 io_buffer_size;
    UINT32 reserved;
} APPX_EXTRACT_OPTIONS;

/*
 * The inspection must have returned successfully from appx_package_inspect()
 * and must remain alive until this call returns.
 *
 * staging_root must be a caller-opened handle to a newly created, empty,
 * private directory.  The caller must have opened and validated its complete
 * ancestor chain without following reparse points, and must prevent untrusted
 * principals from adding, renaming, linking, or deleting entries while this
 * call runs.  The handle must grant directory enumeration, traversal, child
 * creation, and attribute access.  The extractor duplicates and validates the
 * handle, rejects a reparse-point or non-directory leaf, and confines every
 * child operation with RootDirectory-relative NT I/O.  The handle must also
 * grant SYNCHRONIZE so the empty-directory check can wait for synchronous NT
 * enumeration completion.  Destination path strings are never used as a
 * confinement boundary.
 *
 * The directory must not be published or made executable until this function
 * returns S_OK (or the caller explicitly accepts
 * APPX_EXTRACT_S_WEAK_DURABILITY).  A failure leaves no intentionally
 * published payload; cleanup is handle-relative and best effort, but the
 * caller must discard the entire private staging directory after any failure.
 *
 * Archive attributes and Unix mode bits are not inherited.  Every payload,
 * including .exe and .dll files, is created as an ordinary non-reparse,
 * non-sparse, non-compressed staging file.  Windows PE execution does not
 * require granting a host filesystem execute bit.
 */
HRESULT WINAPI appx_package_extract( const APPX_PACKAGE_INSPECTION *inspection,
                                     HANDLE staging_root,
                                     const APPX_EXTRACT_OPTIONS *options );

/*
 * Private deterministic I/O seam used only through the private extraction-test
 * export below.  NULL callbacks use the production implementation; callbacks
 * must preserve the documented Win32 contracts.
 */
typedef BOOL (WINAPI *APPX_EXTRACT_WRITE_FILE_CALLBACK)(
    HANDLE file, const void *buffer, DWORD size, DWORD *written,
    OVERLAPPED *overlapped );
typedef HRESULT (WINAPI *APPX_EXTRACT_FLUSH_CALLBACK)( HANDLE handle );
typedef HRESULT (WINAPI *APPX_EXTRACT_QUERY_SPACE_CALLBACK)(
    HANDLE directory, UINT64 *available );

typedef struct
{
    UINT32 size;
    APPX_EXTRACT_WRITE_FILE_CALLBACK write_file;
    APPX_EXTRACT_FLUSH_CALLBACK flush_file;
    APPX_EXTRACT_FLUSH_CALLBACK flush_directory;
    APPX_EXTRACT_QUERY_SPACE_CALLBACK query_available_bytes;
} APPX_EXTRACT_TEST_IO;

/*
 * Private source seam for deterministic tests.  Production callers must use
 * appx_package_extract(); this interface is a private appxsvc export so the
 * tests exercise the DLL implementation without compiling production source
 * into the test executable.  A source stream is opaque to the extractor.
 */
typedef UINT32 (WINAPI *APPX_EXTRACT_SOURCE_GET_COUNT_CALLBACK)(
    const void *source );
typedef const APPX_PACKAGE_FILE *(WINAPI
    *APPX_EXTRACT_SOURCE_GET_FILE_CALLBACK)(
    const void *source, UINT32 index );
typedef UINT64 (WINAPI *APPX_EXTRACT_SOURCE_GET_EXPANDED_SIZE_CALLBACK)(
    const void *source );
typedef HRESULT (WINAPI *APPX_EXTRACT_SOURCE_OPEN_STREAM_CALLBACK)(
    const void *source, UINT32 index, void **stream );
typedef HRESULT (WINAPI *APPX_EXTRACT_SOURCE_READ_STREAM_CALLBACK)(
    void *stream, void *buffer, UINT32 capacity, UINT32 *read );
typedef void (WINAPI *APPX_EXTRACT_SOURCE_CANCEL_STREAM_CALLBACK)(
    void *stream );
typedef void (WINAPI *APPX_EXTRACT_SOURCE_CLOSE_STREAM_CALLBACK)(
    void *stream );

typedef struct
{
    UINT32 size;
    APPX_EXTRACT_SOURCE_GET_COUNT_CALLBACK get_count;
    APPX_EXTRACT_SOURCE_GET_FILE_CALLBACK get_file;
    APPX_EXTRACT_SOURCE_GET_EXPANDED_SIZE_CALLBACK get_expanded_size;
    APPX_EXTRACT_SOURCE_OPEN_STREAM_CALLBACK open_stream;
    APPX_EXTRACT_SOURCE_READ_STREAM_CALLBACK read_stream;
    APPX_EXTRACT_SOURCE_CANCEL_STREAM_CALLBACK cancel_stream;
    APPX_EXTRACT_SOURCE_CLOSE_STREAM_CALLBACK close_stream;
} APPX_EXTRACT_TEST_SOURCE;

HRESULT WINAPI appx_package_extract_with_test_source(
    const void *source, const APPX_EXTRACT_TEST_SOURCE *source_ops,
    HANDLE staging_root, const APPX_EXTRACT_OPTIONS *options,
    const APPX_EXTRACT_TEST_IO *test_io );

#endif /* __WINE_APPXSVC_EXTRACT_H */
