/*
 * AppX package inspection tests
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
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/test.h"

#include "../manifest.h"
#include "../package.h"

static HRESULT (WINAPI *p_appx_package_inspect)(
    HANDLE, const WINE_APPX_ARCHIVE_LIMITS *, UINT32, APPX_PACKAGE_INSPECTION ** );
static HRESULT (WINAPI *p_appx_package_inspect_ex)(
    HANDLE, const WINE_APPX_ARCHIVE_LIMITS *, UINT32, HANDLE,
    APPX_PACKAGE_INSPECTION ** );
static void (WINAPI *p_appx_package_inspection_free)( APPX_PACKAGE_INSPECTION * );
static const APPX_MANIFEST *(WINAPI *p_appx_package_inspection_get_manifest)(
    const APPX_PACKAGE_INSPECTION * );
static const struct appx_manifest_identity *(WINAPI *p_appx_manifest_get_identity)(
    const APPX_MANIFEST * );
static UINT32 (WINAPI *p_appx_package_inspection_get_file_count)(
    const APPX_PACKAGE_INSPECTION * );
static const APPX_PACKAGE_FILE *(WINAPI *p_appx_package_inspection_get_file)(
    const APPX_PACKAGE_INSPECTION *, UINT32 );
static UINT64 (WINAPI *p_appx_package_inspection_get_expanded_size)(
    const APPX_PACKAGE_INSPECTION * );
static UINT64 (WINAPI *p_appx_package_inspection_get_archive_expanded_size)(
    const APPX_PACKAGE_INSPECTION * );
static HRESULT (WINAPI *p_appx_package_inspection_get_content_id)(
    const APPX_PACKAGE_INSPECTION *, BYTE *, UINT32 );
static HRESULT (WINAPI *p_appx_package_inspection_get_signer_id)(
    const APPX_PACKAGE_INSPECTION *, BYTE *, UINT32 );
static HRESULT (WINAPI *p_appx_package_inspection_open_stream)(
    const APPX_PACKAGE_INSPECTION *, UINT32, WINE_APPX_ARCHIVE_STREAM ** );
static HRESULT (WINAPI *p_wine_appx_archive_stream_read)(
    WINE_APPX_ARCHIVE_STREAM *, void *, UINT32, UINT32 * );
static void (WINAPI *p_wine_appx_archive_stream_close)(
    WINE_APPX_ARCHIVE_STREAM * );

/*
 * microsoft/msix-packaging
 * src/test/testData/unpack/SignedUntrustedCert-CERT_E_CHAINING.appx at
 * efeb9dad695a200c2beaddcba54a52c8320bd135.  The upstream repository is
 * MIT-licensed.  SHA-256:
 * e0f94c16eab4d7acfb41b1dc23acd8c65d23e71a1805061343e4f4ac7018e34e.
 */
static const char signed_package_base64[] =
    "UEsDBC0ACAAIAPJ8sj4AAAAAAAAAAAAAAAAFAAAAMi50eHTszAc6ggEAANCr/MmeGVklZKRtZCah7GyRyNk7h+977wAvCPWEe/v6"
    "BwaHhkdGx8YnIpNT0zPR2bn5hcVYfCmxvLKaXFvf2ExtpTPZXL5Q3N7Z3SvtHxweHZ+UTytn1fOLy1r96vrm9u7+ofH49Pzy+vb+"
    "0fz8an23f347f4Fer9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1e"
    "r9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6"
    "vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr"
    "9Xq9Xq/X6/9H3wUAAP//7MwHNgIAAADQq8gIUUhISRpC9soKycoqI6Rw9s7Re/8f4L+9fzQ/v75bP+3O799/X6B/YHAoODwyGhob"
    "D0cmJqei07GZ2bn4fCK5kFpML2WyufxyYWV1rbi+sbm1vbO7t39wWDo6Pjk9K59fXFauqtc3t3f3tYfHp+eXeuNVr9fr9Xq9Xq/X"
    "6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1e"
    "r9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6"
    "vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/U90ncBAAD//+zMBzYC"
    "AAAA0KvICFFISEkaQvbKCsnKKiOkcPbO0Xv/H+Bf39ze3dceHp+eX+qN17f3j+bn13frp935/fvvC/QPDA4Fh0dGQ2Pj4cjE5FR0"
    "OjYzOxefTyQXUovppUw2l18urKyuFdc3Nre2d3b39g8OS0fHJ6dn5fOLy8pVVa/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v"
    "1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9"
    "Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1"
    "er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1PdJ3AQAA///szAciAgAAAMCvyAhRSEhJGkL2ygrJyioj"
    "pPD2PtAT7h5wh6Wj45PTs/L5xWXlqnp9c3t3X3t4fHp+qTde394/mp9f362fduf3778v0D8wOBQcHhkNjY2HIxOTU9Hp2MzsXHw+"
    "kVxILaaXMtlcfrmwsrpWXN/Y3Nre2d3bP9Dr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X"
    "6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1er9fr9Xq9Xq/X6/V6vV6v1+v1er1e"
    "r9fr9T37LgAAAP//AwBQSwcII7gsbjEFAAAAAAAAmpQDAAAAAABQSwMELQAIAAgA8nyyPgAAAAAAAAAAAAAAAAcAAABhLzMudHh0"
    "CkktLsnMSwcAAAD//wMAUEsHCJpjTicPAAAAAAAAAAcAAAAAAAAAUEsDBC0ACAAAAPJ8sj4AAAAAAAAAAAAAAAAJAAAARW1wdHku"
    "dHh0UEsHCAAAAAAAAAAAAAAAAAAAAAAAAAAAUEsDBC0ACAAAAPJ8sj4AAAAAAAAAAAAAAAAIAAAAVGlsZS5wbmeJUE5HDQoaCgAA"
    "AA1JSERSAAAAyAAAAMgIBgAAAK1Yrp4AAAAZdEVYdFNvZnR3YXJlAEFkb2JlIEltYWdlUmVhZHlxyWU8AAADZGlUWHRYTUw6Y29t"
    "LmFkb2JlLnhtcAAAAAAAPD94cGFja2V0IGJlZ2luPSLvu78iIGlkPSJXNU0wTXBDZWhpSHpyZVN6TlRjemtjOWQiPz4gPHg6eG1w"
    "bWV0YSB4bWxuczp4PSJhZG9iZTpuczptZXRhLyIgeDp4bXB0az0iQWRvYmUgWE1QIENvcmUgNS4wLWMwNjAgNjEuMTM0Nzc3LCAy"
    "MDEwLzAyLzEyLTE3OjMyOjAwICAgICAgICAiPiA8cmRmOlJERiB4bWxuczpyZGY9Imh0dHA6Ly93d3cudzMub3JnLzE5OTkvMDIv"
    "MjItcmRmLXN5bnRheC1ucyMiPiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0iIiB4bWxuczp4bXBNTT0iaHR0cDovL25zLmFk"
    "b2JlLmNvbS94YXAvMS4wL21tLyIgeG1sbnM6c3RSZWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9zVHlwZS9SZXNvdXJj"
    "ZVJlZiMiIHhtbG5zOnhtcD0iaHR0cDovL25zLmFkb2JlLmNvbS94YXAvMS4wLyIgeG1wTU06T3JpZ2luYWxEb2N1bWVudElEPSJ4"
    "bXAuZGlkOkQwMUIyQjQ0RjY5OERGMTE4NjE5QTk5QjQ3OENDQ0ExIiB4bXBNTTpEb2N1bWVudElEPSJ4bXAuZGlkOjk4RkVGRUEy"
    "OThGNjExREZCQTU1Q0FEREQzMzk5OUYwIiB4bXBNTTpJbnN0YW5jZUlEPSJ4bXAuaWlkOjk4RkVGRUExOThGNjExREZCQTU1Q0FE"
    "REQzMzk5OUYwIiB4bXA6Q3JlYXRvclRvb2w9IkFkb2JlIFBob3Rvc2hvcCBDUzUgV2luZG93cyI+IDx4bXBNTTpEZXJpdmVkRnJv"
    "bSBzdFJlZjppbnN0YW5jZUlEPSJ4bXAuaWlkOkQxMUIyQjQ0RjY5OERGMTE4NjE5QTk5QjQ3OENDQ0ExIiBzdFJlZjpkb2N1bWVu"
    "dElEPSJ4bXAuZGlkOkQwMUIyQjQ0RjY5OERGMTE4NjE5QTk5QjQ3OENDQ0ExIi8+IDwvcmRmOkRlc2NyaXB0aW9uPiA8L3JkZjpS"
    "REY+IDwveDp4bXBtZXRhPiA8P3hwYWNrZXQgZW5kPSJyIj8+cycsBAAADKRJREFUeNrsnQ+IHUcdxyemLYk9vDQ0GggEA2dPDgLR"
    "SEskkiNBSblgUVIskUiKUlEqKS2KRUVEJaUQSTBUEioJDQaLweKf0GDpoTQYDI0ED4KHB5FAIBpJEzm99ML1nK/7W315eS+3s2/f"
    "e7s7nw/86Mt2Z29n9vfd2Znf/Fk0Pz/vAKA176IIABAIAAIBQCAACAQAgQAgEIAqcxdFUAiLva1rOjbsbaDN+TPezrc4fsHbVfu9"
    "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"
    "/////wAA";

static int base64_value( char ch )
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static BYTE *decode_base64( const char *text, SIZE_T *output_size )
{
    SIZE_T capacity = (strlen( text ) / 4 + 1) * 3, size = 0;
    UINT32 accumulator = 0, bits = 0;
    BYTE *output;
    int value;

    *output_size = 0;
    if (!(output = HeapAlloc( GetProcessHeap(), 0, capacity ))) return NULL;
    while (*text && *text != '=')
    {
        if ((value = base64_value( *text++ )) < 0) goto failed;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (size == capacity) goto failed;
            output[size++] = accumulator >> bits;
            accumulator &= (1u << bits) - 1;
        }
    }
    while (*text == '=') text++;
    if (*text || bits >= 6 || accumulator) goto failed;
    *output_size = size;
    return output;

failed:
    HeapFree( GetProcessHeap(), 0, output );
    return NULL;
}

static BOOL create_fixture( WCHAR path[MAX_PATH], HANDLE *file )
{
    WCHAR directory[MAX_PATH];
    BYTE *data;
    SIZE_T size;
    DWORD written;
    BOOL success = FALSE;

    *file = INVALID_HANDLE_VALUE;
    if (!(data = decode_base64( signed_package_base64, &size ))) return FALSE;
    if (!GetTempPathW( ARRAY_SIZE(directory), directory ) ||
        !GetTempFileNameW( directory, L"apx", 0, path ))
        goto done;
    *file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, TRUNCATE_EXISTING,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_RANDOM_ACCESS, NULL );
    if (*file == INVALID_HANDLE_VALUE) goto done;
    if (size > MAXDWORD ||
        !WriteFile( *file, data, (DWORD)size, &written, NULL ) ||
        written != size || !FlushFileBuffers( *file ))
        goto done;
    success = TRUE;

done:
    HeapFree( GetProcessHeap(), 0, data );
    if (!success && *file != INVALID_HANDLE_VALUE)
    {
        CloseHandle( *file );
        *file = INVALID_HANDLE_VALUE;
    }
    if (!success && path[0]) DeleteFileW( path );
    return success;
}

static void test_signed_package( void )
{
    static const WCHAR expected_name[] = L"AppxPackaging.Signing.UnitTests.Data";
    APPX_PACKAGE_INSPECTION *inspection = (void *)0xdeadbeef;
    const struct appx_manifest_identity *identity;
    const APPX_PACKAGE_FILE *file_info;
    const APPX_MANIFEST *manifest;
    WINE_APPX_ARCHIVE_STREAM *stream;
    WCHAR path[MAX_PATH] = {0};
    BYTE buffer[4096], content_id[APPX_PACKAGE_CONTENT_ID_SIZE];
    BYTE signer_id[APPX_PACKAGE_SIGNER_ID_SIZE], byte;
    LARGE_INTEGER offset;
    UINT64 streamed;
    UINT32 count, i;
    DWORD transferred;
    HANDLE cancel, file;
    HRESULT hr;
    BOOL found_manifest = FALSE;

    ok( create_fixture( path, &file ), "failed to create signed fixture.\n" );
    if (file == INVALID_HANDLE_VALUE) return;

    cancel = CreateEventW( NULL, TRUE, TRUE, NULL );
    ok( !!cancel, "failed to create manual cancellation event, error %lu.\n",
        GetLastError() );
    if (cancel)
    {
        inspection = (void *)0xdeadbeef;
        hr = p_appx_package_inspect_ex(
            file, NULL, APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN,
            cancel, &inspection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "pre-cancelled inspection returned %#lx.\n", hr );
        ok( !inspection, "pre-cancelled inspection returned %p.\n",
            inspection );
        CloseHandle( cancel );
    }

    cancel = CreateEventW( NULL, FALSE, TRUE, NULL );
    ok( !!cancel, "failed to create auto-reset cancellation event, error %lu.\n",
        GetLastError() );
    if (cancel)
    {
        inspection = (void *)0xdeadbeef;
        hr = p_appx_package_inspect_ex(
            file, NULL, APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN,
            cancel, &inspection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "auto-reset cancelled inspection returned %#lx.\n", hr );
        ok( !inspection, "auto-reset cancelled inspection returned %p.\n",
            inspection );
        CloseHandle( cancel );
    }

    cancel = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!cancel, "failed to create invalid cancellation event, error %lu.\n",
        GetLastError() );
    if (cancel)
    {
        CloseHandle( cancel );
        inspection = (void *)0xdeadbeef;
        hr = p_appx_package_inspect_ex(
            file, NULL, APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN,
            cancel, &inspection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
            "invalid cancellation event returned %#lx.\n", hr );
        ok( !inspection, "invalid cancellation event returned %p.\n",
            inspection );
    }

    hr = p_appx_package_inspect( file, NULL, 0, &inspection );
    ok( FAILED(hr), "untrusted package returned %#lx.\n", hr );
    ok( !inspection, "untrusted package returned inspection %p.\n", inspection );

    hr = p_appx_package_inspect(
        file, NULL, APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN, &inspection );
    ok( hr == S_OK, "signed package returned %#lx.\n", hr );
    ok( !!inspection, "signed package returned no inspection.\n" );
    if (!inspection) goto done;

    manifest = p_appx_package_inspection_get_manifest( inspection );
    ok( !!manifest, "inspection returned no manifest.\n" );
    identity = manifest ? p_appx_manifest_get_identity( manifest ) : NULL;
    ok( !!identity, "manifest returned no identity.\n" );
    if (identity)
        ok( !lstrcmpW( identity->name, expected_name ),
            "got package name %s.\n", wine_dbgstr_w(identity->name) );

    count = p_appx_package_inspection_get_file_count( inspection );
    ok( count == 5, "got %u block-mapped files.\n", count );
    ok( p_appx_package_inspection_get_expanded_size( inspection ) != 0,
        "got zero expanded size.\n" );
    ok( p_appx_package_inspection_get_archive_expanded_size( inspection ) >=
        p_appx_package_inspection_get_expanded_size( inspection ),
        "archive expansion is smaller than its block-mapped payload.\n" );
    memset( content_id, 0, sizeof(content_id) );
    hr = p_appx_package_inspection_get_content_id(
        inspection, content_id, sizeof(content_id) );
    ok( hr == S_OK, "content id returned %#lx.\n", hr );
    for (i = 0; i < sizeof(content_id); i++)
        if (content_id[i]) break;
    ok( i != sizeof(content_id), "content id is all zero.\n" );
    memset( signer_id, 0, sizeof(signer_id) );
    hr = p_appx_package_inspection_get_signer_id(
        inspection, signer_id, sizeof(signer_id) );
    ok( hr == S_OK, "signer id returned %#lx.\n", hr );
    for (i = 0; i < sizeof(signer_id); i++)
        if (signer_id[i]) break;
    ok( i != sizeof(signer_id), "signer id is all zero.\n" );
    for (i = 0; i < count; i++)
    {
        file_info = p_appx_package_inspection_get_file( inspection, i );
        ok( !!file_info, "file %u is missing.\n", i );
        if (!file_info) continue;
        if (!lstrcmpW( file_info->path, L"AppxManifest.xml" ))
            found_manifest = TRUE;

        stream = NULL;
        hr = p_appx_package_inspection_open_stream( inspection, i, &stream );
        ok( hr == S_OK, "open stream %u returned %#lx.\n", i, hr );
        streamed = 0;
        while (stream)
        {
            UINT32 read = 0;

            hr = p_wine_appx_archive_stream_read(
                stream, buffer, sizeof(buffer), &read );
            if (hr == S_FALSE) break;
            ok( hr == S_OK && read, "stream %u returned %#lx, %u.\n",
                i, hr, read );
            if (hr != S_OK || !read) break;
            streamed += read;
        }
        ok( streamed == file_info->uncompressed_size,
            "stream %u returned %s bytes, expected %s.\n", i,
            wine_dbgstr_longlong(streamed),
            wine_dbgstr_longlong(file_info->uncompressed_size) );
        if (stream) p_wine_appx_archive_stream_close( stream );
    }
    ok( found_manifest, "block map did not contain AppxManifest.xml.\n" );
    p_appx_package_inspection_free( inspection );
    inspection = NULL;

    offset.QuadPart = 64;
    ok( SetFilePointerEx( file, offset, NULL, FILE_BEGIN ),
        "failed to seek fixture, error %lu.\n", GetLastError() );
    ok( ReadFile( file, &byte, 1, &transferred, NULL ) && transferred == 1,
        "failed to read fixture, error %lu.\n", GetLastError() );
    byte ^= 1;
    offset.QuadPart = 64;
    ok( SetFilePointerEx( file, offset, NULL, FILE_BEGIN ),
        "failed to seek fixture, error %lu.\n", GetLastError() );
    ok( WriteFile( file, &byte, 1, &transferred, NULL ) && transferred == 1 &&
        FlushFileBuffers( file ), "failed to tamper fixture, error %lu.\n",
        GetLastError() );
    hr = p_appx_package_inspect(
        file, NULL, APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN, &inspection );
    ok( FAILED(hr), "tampered package returned %#lx.\n", hr );
    ok( !inspection, "tampered package returned inspection %p.\n", inspection );

done:
    p_appx_package_inspection_free( inspection );
    CloseHandle( file );
    DeleteFileW( path );
}

START_TEST(package)
{
    HMODULE module = LoadLibraryA( "appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is unavailable, error %lu.\n", GetLastError() );
        return;
    }

#define LOAD(name) do {                                                         \
    p_##name = (void *)GetProcAddress( module, #name );                          \
    if (!p_##name)                                                              \
    {                                                                            \
        ok( 0, #name " is unavailable.\n" );                                    \
        FreeLibrary( module );                                                   \
        return;                                                                  \
    }                                                                            \
} while (0)

    LOAD(appx_package_inspect);
    LOAD(appx_package_inspect_ex);
    LOAD(appx_package_inspection_free);
    LOAD(appx_package_inspection_get_manifest);
    LOAD(appx_package_inspection_get_file_count);
    LOAD(appx_package_inspection_get_file);
    LOAD(appx_package_inspection_get_expanded_size);
    LOAD(appx_package_inspection_get_archive_expanded_size);
    LOAD(appx_package_inspection_get_content_id);
    LOAD(appx_package_inspection_get_signer_id);
    LOAD(appx_package_inspection_open_stream);
    LOAD(appx_manifest_get_identity);
    LOAD(wine_appx_archive_stream_read);
    LOAD(wine_appx_archive_stream_close);

#undef LOAD

    ok( p_appx_package_inspect( INVALID_HANDLE_VALUE, NULL, 0, NULL ) ==
        E_INVALIDARG, "invalid arguments were accepted.\n" );
    ok( p_appx_package_inspection_get_content_id( NULL, NULL, 0 ) ==
        E_INVALIDARG, "invalid content-id arguments were accepted.\n" );
    ok( p_appx_package_inspection_get_signer_id( NULL, NULL, 0 ) ==
        E_INVALIDARG, "invalid signer-id arguments were accepted.\n" );
    test_signed_package();
    FreeLibrary( module );
}
