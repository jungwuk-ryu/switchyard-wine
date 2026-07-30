/*
 * AppX archive path tests
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

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/appxsvc.h"
#include "wine/test.h"

static HRESULT (WINAPI *p_wine_appx_validate_archive_path)( const BYTE *, UINT32, UINT32, UINT32 *, WCHAR * );

static void check_valid_path( const char *input, UINT32 flags, const WCHAR *expected )
{
    WCHAR buffer[512], untouched[2] = {0xcccc, 0xdddd};
    UINT32 input_length = strlen( input ), length, expected_length;
    HRESULT hr;

    expected_length = lstrlenW( expected ) + 1;

    length = 0;
    hr = p_wine_appx_validate_archive_path( (const BYTE *)input, input_length, flags, &length, NULL );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "got hr %#lx for %s.\n", hr, debugstr_a(input) );
    ok( length == expected_length, "got length %u, expected %u for %s.\n",
        length, expected_length, debugstr_a(input) );

    length = expected_length - 1;
    hr = p_wine_appx_validate_archive_path( (const BYTE *)input, input_length, flags, &length, untouched );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "got hr %#lx for %s.\n", hr, debugstr_a(input) );
    ok( length == expected_length, "got length %u, expected %u for %s.\n",
        length, expected_length, debugstr_a(input) );
    ok( untouched[0] == 0xcccc && untouched[1] == 0xdddd, "buffer was modified for %s.\n", debugstr_a(input) );

    memset( buffer, 0xcc, sizeof(buffer) );
    length = ARRAY_SIZE(buffer);
    hr = p_wine_appx_validate_archive_path( (const BYTE *)input, input_length, flags, &length, buffer );
    ok( hr == S_OK, "got hr %#lx for %s.\n", hr, debugstr_a(input) );
    ok( length == expected_length, "got length %u, expected %u for %s.\n",
        length, expected_length, debugstr_a(input) );
    ok( !lstrcmpW( buffer, expected ), "got path %s, expected %s.\n",
        debugstr_w(buffer), debugstr_w(expected) );
}

static void check_invalid_path( const BYTE *input, UINT32 input_length, UINT32 flags )
{
    WCHAR buffer[8];
    UINT32 length = ARRAY_SIZE(buffer);
    HRESULT hr;

    memset( buffer, 0xcc, sizeof(buffer) );
    hr = p_wine_appx_validate_archive_path( input, input_length, flags, &length, buffer );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx for %s.\n",
        hr, debugstr_an((const char *)input, input_length) );
    ok( !length, "got length %u for %s.\n", length, debugstr_an((const char *)input, input_length) );
    ok( buffer[0] == 0xcccc, "buffer was modified for %s.\n",
        debugstr_an((const char *)input, input_length) );
}

static void test_path_length_boundaries( void )
{
    const UINT32 valid_length = WINE_APPX_MAX_PATH_CHARS - 1;
    BYTE *input;
    WCHAR *output;
    UINT32 capacity, i;
    HRESULT hr;

    input = HeapAlloc( GetProcessHeap(), 0, WINE_APPX_MAX_PATH_CHARS );
    ok( !!input, "failed to allocate path input.\n" );
    if (!input) return;

    memset( input, 'a', WINE_APPX_MAX_PATH_CHARS );
    for (i = WINE_APPX_MAX_COMPONENT_CHARS; i < WINE_APPX_MAX_PATH_CHARS;
         i += WINE_APPX_MAX_COMPONENT_CHARS + 1)
        input[i] = '/';

    capacity = 0;
    hr = p_wine_appx_validate_archive_path( input, valid_length, 0, &capacity, NULL );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "got hr %#lx.\n", hr );
    ok( capacity == WINE_APPX_MAX_PATH_CHARS, "got capacity %u.\n", capacity );

    output = HeapAlloc( GetProcessHeap(), 0, capacity * sizeof(*output) );
    ok( !!output, "failed to allocate path output.\n" );
    if (output)
    {
        hr = p_wine_appx_validate_archive_path( input, valid_length, 0, &capacity, output );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( capacity == WINE_APPX_MAX_PATH_CHARS, "got capacity %u.\n", capacity );
        ok( output[WINE_APPX_MAX_COMPONENT_CHARS] == '\\', "separator was not converted.\n" );
        ok( !output[valid_length], "path was not terminated.\n" );
        HeapFree( GetProcessHeap(), 0, output );
    }

    capacity = 1;
    hr = p_wine_appx_validate_archive_path( input, WINE_APPX_MAX_PATH_CHARS, 0, &capacity, NULL );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx.\n", hr );
    ok( !capacity, "got capacity %u.\n", capacity );

    capacity = 1;
    hr = p_wine_appx_validate_archive_path( input, WINE_APPX_MAX_ENTRY_NAME_BYTES + 1, 0,
                                            &capacity, NULL );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx.\n", hr );
    ok( !capacity, "got capacity %u.\n", capacity );

    HeapFree( GetProcessHeap(), 0, input );
}

static void test_archive_path( void )
{
    static const char *invalid_paths[] =
    {
        "/absolute",
        "\\absolute",
        "C:/drive",
        "//server/share",
        "a\\b",
        ".",
        "..",
        "./a",
        "../a",
        "a/./b",
        "a/../b",
        "a//b",
        "a/",
        "a.",
        "a ",
        "a./b",
        "a /b",
        "a:b",
        "a<b",
        "a>b",
        "a\"b",
        "a|b",
        "a?b",
        "a*b",
        "CON",
        "con.txt",
        "CON  .txt",
        "PRN",
        "AUX.data",
        "NUL",
        "NUL .dat",
        "CLOCK$",
        "CONIN$",
        "CONOUT$.txt",
        "COM1",
        "COM1 .dll",
        "com9.dll",
        "LPT1",
        "LPT9 .x",
        "lpt9.txt",
    };
    static const BYTE embedded_nul[] = {'a', 0, 'b'};
    static const BYTE control[] = {'a', 0x1f, 'b'};
    static const BYTE invalid_utf8_1[] = {0xc0, 0xaf};
    static const BYTE invalid_utf8_2[] = {0xed, 0xa0, 0x80};
    static const BYTE decomposed[] = {'c', 'a', 'f', 'e', 0xcc, 0x81};
    static const BYTE superscript_com[] = {'C', 'O', 'M', 0xc2, 0xb9};
    char component[WINE_APPX_MAX_COMPONENT_CHARS + 2];
    WCHAR wide_component[WINE_APPX_MAX_COMPONENT_CHARS + 1];
    WCHAR buffer[8];
    UINT32 length;
    HRESULT hr;
    unsigned int i;

    check_valid_path( "AppxManifest.xml", 0, L"AppxManifest.xml" );
    check_valid_path( "VFS/ProgramFilesX64/Example/app.exe", 0,
                      L"VFS\\ProgramFilesX64\\Example\\app.exe" );
    check_valid_path( "caf\xc3\xa9.txt", 0, L"caf\u00e9.txt" );
    check_valid_path( "Assets/", WINE_APPX_PATH_DIRECTORY, L"Assets" );

    for (i = 0; i < ARRAY_SIZE(invalid_paths); i++)
        check_invalid_path( (const BYTE *)invalid_paths[i], strlen(invalid_paths[i]), 0 );
    check_invalid_path( embedded_nul, sizeof(embedded_nul), 0 );
    check_invalid_path( control, sizeof(control), 0 );
    check_invalid_path( invalid_utf8_1, sizeof(invalid_utf8_1), 0 );
    check_invalid_path( invalid_utf8_2, sizeof(invalid_utf8_2), 0 );
    check_invalid_path( decomposed, sizeof(decomposed), 0 );
    check_invalid_path( superscript_com, sizeof(superscript_com), 0 );
    check_invalid_path( (const BYTE *)"Assets", strlen("Assets"), WINE_APPX_PATH_DIRECTORY );
    check_invalid_path( (const BYTE *)"Assets//", strlen("Assets//"), WINE_APPX_PATH_DIRECTORY );

    memset( component, 'a', WINE_APPX_MAX_COMPONENT_CHARS );
    component[WINE_APPX_MAX_COMPONENT_CHARS] = 0;
    for (i = 0; i < WINE_APPX_MAX_COMPONENT_CHARS; i++) wide_component[i] = 'a';
    wide_component[WINE_APPX_MAX_COMPONENT_CHARS] = 0;
    check_valid_path( component, 0, wide_component );

    component[WINE_APPX_MAX_COMPONENT_CHARS] = 'a';
    component[WINE_APPX_MAX_COMPONENT_CHARS + 1] = 0;
    check_invalid_path( (const BYTE *)component, strlen(component), 0 );

    length = ARRAY_SIZE(buffer);
    hr = p_wine_appx_validate_archive_path( NULL, 1, 0, &length, buffer );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_wine_appx_validate_archive_path( (const BYTE *)"a", 0, 0, &length, buffer );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_wine_appx_validate_archive_path( (const BYTE *)"a", 1, 0, NULL, buffer );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    length = ARRAY_SIZE(buffer);
    hr = p_wine_appx_validate_archive_path( (const BYTE *)"a", 1, 2, &length, buffer );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    test_path_length_boundaries();
}

START_TEST(path)
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is not available, error %lu.\n", GetLastError() );
        return;
    }

    p_wine_appx_validate_archive_path =
        (void *)GetProcAddress( module, "wine_appx_validate_archive_path" );
    if (!p_wine_appx_validate_archive_path)
    {
        ok( 0, "wine_appx_validate_archive_path is not available, error %lu.\n", GetLastError() );
        FreeLibrary( module );
        return;
    }

    test_archive_path();
    FreeLibrary( module );
}
