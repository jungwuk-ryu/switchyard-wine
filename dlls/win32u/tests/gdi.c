/*
 * WoW64 GDI syscall tests
 *
 * Copyright 2026 Switchyard contributors
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
#include <setjmp.h>
#include <limits.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "ntgdi.h"

#include "wine/test.h"

static jmp_buf fault_jmpbuf;
static DWORD fault_code;
static DWORD fault_last_error;

static LONG WINAPI fault_handler( EXCEPTION_POINTERS *eptr )
{
    fault_code = eptr->ExceptionRecord->ExceptionCode;
    longjmp( fault_jmpbuf, 1 );
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD call_ext_get_object_catching_fault( HGDIOBJ handle, INT count, void *buffer,
                                                  INT *result )
{
    void *handler;

    fault_code = 0;
    fault_last_error = 0;
    handler = AddVectoredExceptionHandler( TRUE, fault_handler );
    ok( !!handler, "AddVectoredExceptionHandler failed, error %lu\n", GetLastError() );
    if (!handler) return 0;

    SetLastError( 0xdeadbeef );
    if (!setjmp( fault_jmpbuf ))
    {
        *result = NtGdiExtGetObjectW( handle, count, buffer );
        fault_last_error = GetLastError();
    }
    RemoveVectoredExceptionHandler( handler );
    return fault_code;
}

static DWORD call_outline_metrics_catching_fault( HDC hdc, UINT size,
                                                   OUTLINETEXTMETRICW *metrics,
                                                   UINT *result )
{
    void *handler;

    fault_code = 0;
    handler = AddVectoredExceptionHandler( TRUE, fault_handler );
    ok( !!handler, "AddVectoredExceptionHandler failed, error %lu\n", GetLastError() );
    if (!handler) return 0;

    if (!setjmp( fault_jmpbuf ))
        *result = NtGdiGetOutlineTextMetricsInternalW( hdc, size, metrics, 0 );
    RemoveVectoredExceptionHandler( handler );
    return fault_code;
}

static BOOL set_page_protection( void *page, DWORD protection )
{
    DWORD old_protection;
    BOOL ret = VirtualProtect( page, 0x1000, protection, &old_protection );

    ok( ret, "VirtualProtect(%p, %#lx) failed, error %lu\n",
        page, protection, GetLastError() );
    return ret;
}

static void check_outline_metric_string( const BYTE *buffer, UINT size, const char *name,
                                         ULONG_PTR offset, BOOL nonempty )
{
    BOOL terminated = FALSE;
    UINT pos;
    WCHAR ch;

    ok( offset >= sizeof(OUTLINETEXTMETRICW) && offset <= size &&
        size - offset >= sizeof(WCHAR), "%s offset %#Ix is outside %u bytes\n",
        name, offset, size );
    if (offset < sizeof(OUTLINETEXTMETRICW) || offset > size ||
        size - offset < sizeof(WCHAR))
        return;

    ok( !(offset & (sizeof(WCHAR) - 1)), "%s offset %#Ix is misaligned\n", name, offset );
    memcpy( &ch, buffer + offset, sizeof(ch) );
    if (nonempty) ok( ch, "%s string is empty\n", name );

    for (pos = offset;; pos += sizeof(WCHAR))
    {
        memcpy( &ch, buffer + pos, sizeof(ch) );
        if (!ch)
        {
            terminated = TRUE;
            break;
        }
        if (size - pos < 2 * sizeof(WCHAR)) break;
    }
    ok( terminated, "%s string is not terminated within %u bytes\n", name, size );
}

static void test_wow64_ext_get_object( BYTE *pages )
{
    static const DWORD styles[] = {3, 5};
    BYTE snapshot[256], buffer[256];
    LOGBRUSH brush = {BS_SOLID, RGB(0x12, 0x34, 0x56), 0};
    EXTLOGPEN *pen_data = (EXTLOGPEN *)buffer;
    BYTE *cross;
    HPEN pen, null_pen;
    INT needed, result;
    DWORD exception;
    SIZE_T first_size;

    pen = ExtCreatePen( PS_GEOMETRIC | PS_USERSTYLE, 3, &brush,
                        ARRAY_SIZE(styles), styles );
    ok( !!pen, "ExtCreatePen failed, error %lu\n", GetLastError() );
    if (!pen) return;

    needed = NtGdiExtGetObjectW( pen, 0, NULL );
    ok( needed == FIELD_OFFSET( EXTLOGPEN, elpStyleEntry[ARRAY_SIZE(styles)] ),
        "unexpected EXTLOGPEN size %d\n", needed );
    if (needed <= 0 || needed > sizeof(buffer)) goto done;

    memset( buffer, 0xcc, sizeof(buffer) );
    result = NtGdiExtGetObjectW( pen, needed - 1, buffer );
    ok( !result, "undersized EXTLOGPEN returned %d\n", result );
    ok( buffer[0] == 0xcc && buffer[needed - 2] == 0xcc,
        "undersized EXTLOGPEN modified its output\n" );

    memset( buffer, 0xcc, sizeof(buffer) );
    result = NtGdiExtGetObjectW( pen, INT_MAX, buffer );
    ok( result == needed, "INT_MAX EXTLOGPEN returned %d, expected %d\n", result, needed );
    ok( pen_data->elpNumEntries == ARRAY_SIZE(styles), "got %lu style entries\n",
        pen_data->elpNumEntries );
    ok( pen_data->elpStyleEntry[0] == styles[0] && pen_data->elpStyleEntry[1] == styles[1],
        "got styles %lu, %lu\n", pen_data->elpStyleEntry[0], pen_data->elpStyleEntry[1] );

    memset( buffer, 0xcc, sizeof(buffer) );
    result = NtGdiExtGetObjectW( pen, (INT)UINT_MAX, buffer );
    ok( !result, "UINT_MAX EXTLOGPEN returned %d\n", result );
    ok( buffer[0] == 0xcc, "UINT_MAX EXTLOGPEN modified its output\n" );

    null_pen = CreatePen( PS_NULL, 1, 0 );
    ok( !!null_pen, "CreatePen failed, error %lu\n", GetLastError() );
    if (null_pen)
    {
        memset( buffer, 0xcc, sizeof(buffer) );
        result = NtGdiExtGetObjectW( null_pen, sizeof(EXTLOGPEN), buffer );
        ok( result == sizeof(EXTLOGPEN), "null EXTLOGPEN returned %d\n", result );
        ok( pen_data->elpPenStyle == PS_NULL && !pen_data->elpWidth,
            "got null pen style %#lx, width %lu\n",
            pen_data->elpPenStyle, pen_data->elpWidth );
        DeleteObject( null_pen );
    }

    cross = pages + 0x1000 - needed / 2;
    first_size = pages + 0x1000 - cross;
    memset( cross, 0xcc, needed );
    memcpy( snapshot, cross, needed );
    if (!set_page_protection( pages + 0x1000, PAGE_NOACCESS )) goto done;
    result = 0x13572468;
    exception = call_ext_get_object_catching_fault( pen, needed, cross, &result );
    ok( exception == STATUS_ACCESS_VIOLATION, "NOACCESS EXTLOGPEN raised %#lx\n", exception );
    ok( !memcmp( cross, snapshot, first_size ),
        "NOACCESS EXTLOGPEN partially published %Iu accessible bytes\n", first_size );
    if (!set_page_protection( pages + 0x1000, PAGE_READWRITE )) goto done;

    result = NtGdiExtGetObjectW( pen, needed, cross );
    ok( result == needed, "valid follow-up EXTLOGPEN returned %d\n", result );
    ok( ((EXTLOGPEN *)cross)->elpStyleEntry[1] == styles[1],
        "valid follow-up got style %lu\n", ((EXTLOGPEN *)cross)->elpStyleEntry[1] );

    result = 0x13572468;
    exception = call_ext_get_object_catching_fault( pen, needed,
                    ULongToPtr( UINT_MAX - needed / 2 ), &result );
    ok( exception == STATUS_ACCESS_VIOLATION,
        "4GiB-crossing EXTLOGPEN raised %#lx\n", exception );

    memset( cross, 0xcc, needed );
    memcpy( snapshot, cross, needed );
    if (!set_page_protection( pages + 0x1000, PAGE_READONLY )) goto done;
    result = 0x13572468;
    exception = call_ext_get_object_catching_fault( pen, needed, cross, &result );
    ok( exception == STATUS_ACCESS_VIOLATION, "READONLY EXTLOGPEN raised %#lx\n", exception );
    ok( !memcmp( cross, snapshot, needed ), "READONLY EXTLOGPEN partially published output\n" );
    if (!set_page_protection( pages + 0x1000, PAGE_READWRITE )) goto done;

    memset( pages + 0x1000, 0xcc, needed );
    if (!set_page_protection( pages + 0x1000, PAGE_NOACCESS )) goto done;
    result = 0x13572468;
    exception = call_ext_get_object_catching_fault( pen, needed - 1,
                                                    pages + 0x1000, &result );
    ok( !exception, "undersized protected EXTLOGPEN raised %#lx\n", exception );
    ok( !result, "undersized protected EXTLOGPEN returned %d\n", result );

    DeleteObject( pen );
    exception = call_ext_get_object_catching_fault( pen, INT_MAX, pages + 0x1000, &result );
    ok( !exception, "invalid-handle EXTLOGPEN raised %#lx\n", exception );
    ok( !result, "invalid-handle EXTLOGPEN returned %d\n", result );
    ok( fault_last_error == 0xdeadbeef, "invalid handle changed last error to %lu\n",
        fault_last_error );
    pen = NULL;
    set_page_protection( pages + 0x1000, PAGE_READWRITE );

done:
    set_page_protection( pages + 0x1000, PAGE_READWRITE );
    if (pen) DeleteObject( pen );
}

static void test_wow64_outline_metrics( BYTE *pages )
{
    BYTE snapshot[512], *buffer, *cross;
    OUTLINETEXTMETRICW *metrics;
    HFONT font, old_font;
    HDC hdc, stale_hdc;
    UINT needed, result;
    DWORD exception;
    SIZE_T first_size;

    hdc = CreateCompatibleDC( NULL );
    ok( !!hdc, "CreateCompatibleDC failed, error %lu\n", GetLastError() );
    if (!hdc) return;

    font = CreateFontW( -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                        DEFAULT_PITCH, L"Arial" );
    ok( !!font, "CreateFontW failed, error %lu\n", GetLastError() );
    old_font = font ? SelectObject( hdc, font ) : NULL;

    needed = NtGdiGetOutlineTextMetricsInternalW( hdc, 0, NULL, 0 );
    if (!needed)
    {
        win_skip( "selected font has no outline metrics\n" );
        goto done;
    }
    ok( needed >= sizeof(OUTLINETEXTMETRICW), "unexpected OTM size %u\n", needed );

    buffer = malloc( (SIZE_T)needed + 16 );
    ok( !!buffer, "failed to allocate %u OTM bytes\n", needed );
    if (!buffer) goto done;

    memset( buffer, 0xcc, (SIZE_T)needed + 16 );
    result = NtGdiGetOutlineTextMetricsInternalW( hdc, UINT_MAX,
                                                  (OUTLINETEXTMETRICW *)buffer, 0 );
    ok( result == needed, "UINT_MAX OTM returned %u, expected %u\n", result, needed );
    metrics = (OUTLINETEXTMETRICW *)buffer;
    ok( metrics->otmSize == needed, "UINT_MAX OTM reported size %u\n", metrics->otmSize );
    check_outline_metric_string( buffer, needed, "family",
                                 (ULONG_PTR)metrics->otmpFamilyName, TRUE );
    check_outline_metric_string( buffer, needed, "face",
                                 (ULONG_PTR)metrics->otmpFaceName, TRUE );
    check_outline_metric_string( buffer, needed, "style",
                                 (ULONG_PTR)metrics->otmpStyleName, FALSE );
    check_outline_metric_string( buffer, needed, "full",
                                 (ULONG_PTR)metrics->otmpFullName, TRUE );
    ok( buffer[needed] == 0xcc && buffer[needed + 15] == 0xcc,
        "UINT_MAX OTM wrote past its result\n" );

    memset( buffer, 0xcc, (SIZE_T)needed + 16 );
    result = NtGdiGetOutlineTextMetricsInternalW( hdc, sizeof(*metrics) + 1, metrics, 0 );
    ok( result == sizeof(*metrics) + 1, "header-plus-one OTM returned %u\n", result );
    ok( buffer[sizeof(*metrics) + 1] == 0xcc,
        "header-plus-one OTM wrote past its requested boundary\n" );

    cross = pages + 0x1000 - sizeof(*metrics) / 2;
    first_size = pages + 0x1000 - cross;
    memset( cross, 0xcc, sizeof(*metrics) );
    memcpy( snapshot, cross, sizeof(*metrics) );
    if (!set_page_protection( pages + 0x1000, PAGE_NOACCESS )) goto free_buffer;
    result = 0x13572468;
    exception = call_outline_metrics_catching_fault( hdc, sizeof(*metrics),
                                                     (OUTLINETEXTMETRICW *)cross, &result );
    ok( exception == STATUS_ACCESS_VIOLATION, "NOACCESS OTM raised %#lx\n", exception );
    ok( !memcmp( cross, snapshot, first_size ),
        "NOACCESS OTM partially published %Iu accessible bytes\n", first_size );
    if (!set_page_protection( pages + 0x1000, PAGE_READWRITE )) goto free_buffer;

    result = NtGdiGetOutlineTextMetricsInternalW( hdc, sizeof(*metrics),
                                                  (OUTLINETEXTMETRICW *)cross, 0 );
    ok( result == sizeof(*metrics), "valid follow-up OTM returned %u\n", result );

    result = 0x13572468;
    exception = call_outline_metrics_catching_fault( hdc, sizeof(*metrics),
                    ULongToPtr( UINT_MAX - sizeof(*metrics) / 2 ), &result );
    ok( exception == STATUS_ACCESS_VIOLATION, "4GiB-crossing OTM raised %#lx\n", exception );

    memset( cross, 0xcc, sizeof(*metrics) );
    memcpy( snapshot, cross, sizeof(*metrics) );
    if (!set_page_protection( pages + 0x1000, PAGE_READONLY )) goto free_buffer;
    result = 0x13572468;
    exception = call_outline_metrics_catching_fault( hdc, sizeof(*metrics),
                                                     (OUTLINETEXTMETRICW *)cross, &result );
    ok( exception == STATUS_ACCESS_VIOLATION, "READONLY OTM raised %#lx\n", exception );
    ok( !memcmp( cross, snapshot, sizeof(*metrics) ), "READONLY OTM partially published output\n" );
    if (!set_page_protection( pages + 0x1000, PAGE_READWRITE )) goto free_buffer;

    if (!set_page_protection( pages + 0x1000, PAGE_NOACCESS )) goto free_buffer;
    result = 0x13572468;
    exception = call_outline_metrics_catching_fault( hdc, 0,
                                                     (OUTLINETEXTMETRICW *)(pages + 0x1000),
                                                     &result );
    ok( !exception, "zero-length protected OTM raised %#lx\n", exception );
    ok( result == sizeof(*metrics), "zero-length protected OTM returned %u\n", result );

    stale_hdc = hdc;
    if (font && old_font) SelectObject( hdc, old_font );
    if (font) DeleteObject( font );
    font = NULL;
    DeleteDC( hdc );
    hdc = NULL;
    result = 0x13572468;
    exception = call_outline_metrics_catching_fault( stale_hdc, UINT_MAX,
                                                     (OUTLINETEXTMETRICW *)(pages + 0x1000),
                                                     &result );
    ok( !exception, "invalid-HDC OTM raised %#lx\n", exception );
    ok( !result, "invalid-HDC OTM returned %u\n", result );
    set_page_protection( pages + 0x1000, PAGE_READWRITE );

free_buffer:
    set_page_protection( pages + 0x1000, PAGE_READWRITE );
    free( buffer );
done:
    if (hdc && font && old_font) SelectObject( hdc, old_font );
    if (font) DeleteObject( font );
    if (hdc) DeleteDC( hdc );
}

START_TEST(gdi)
{
    SYSTEM_INFO info;
    BOOL is_wow64;
    BYTE *pages;

    if (sizeof(void *) != 4 || !IsWow64Process( GetCurrentProcess(), &is_wow64 ) || !is_wow64)
    {
        win_skip( "WoW64 GDI marshalling tests require a 32-bit process on 64-bit Windows\n" );
        return;
    }

    GetSystemInfo( &info );
    if (info.dwPageSize != 0x1000)
    {
        win_skip( "expected 4K WoW64 pages, got %#lx\n", info.dwPageSize );
        return;
    }

    pages = VirtualAlloc( NULL, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !!pages, "VirtualAlloc failed, error %lu\n", GetLastError() );
    if (!pages) return;

    test_wow64_ext_get_object( pages );
    test_wow64_outline_metrics( pages );
    VirtualFree( pages, 0, MEM_RELEASE );
}
