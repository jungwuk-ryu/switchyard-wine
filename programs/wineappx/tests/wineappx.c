/*
 * wineappx command-line tests
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

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "wine/test.h"

#define COMMAND_TIMEOUT 15000

static BOOL append_path( WCHAR *output, SIZE_T capacity, const WCHAR *root,
                         const WCHAR *relative )
{
    SIZE_T root_length = lstrlenW( root );
    SIZE_T relative_length = lstrlenW( relative );
    BOOL separator = root_length && root[root_length - 1] != '\\';

    if (!capacity || !root_length ||
        root_length + separator + relative_length >= capacity)
    {
        if (capacity) output[0] = 0;
        return FALSE;
    }
    memcpy( output, root, root_length * sizeof(*output) );
    if (separator) output[root_length++] = '\\';
    memcpy( output + root_length, relative,
            (relative_length + 1) * sizeof(*output) );
    return TRUE;
}

static void remove_tree( const WCHAR *path )
{
    WIN32_FIND_DATAW data;
    WCHAR pattern[MAX_PATH * 2], child[MAX_PATH * 2];
    DWORD attributes = GetFileAttributesW( path );
    HANDLE find;

    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        SetFileAttributesW( path, FILE_ATTRIBUTE_NORMAL );
        DeleteFileW( path );
        return;
    }
    if (!append_path( pattern, ARRAY_SIZE(pattern), path, L"*" )) return;
    find = FindFirstFileW( pattern, &data );
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!lstrcmpW( data.cFileName, L"." ) ||
                !lstrcmpW( data.cFileName, L".." ))
                continue;
            if (!append_path( child, ARRAY_SIZE(child), path,
                              data.cFileName ))
                continue;
            remove_tree( child );
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    RemoveDirectoryW( path );
}

static BOOL make_test_root( WCHAR *root )
{
    WCHAR temp[MAX_PATH];

    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"wax", 0, root ))
        return FALSE;
    return DeleteFileW( root );
}

static DWORD run_wineappx_capture( const WCHAR *arguments, char *output,
                                   DWORD output_capacity )
{
    PROCESS_INFORMATION process = {0};
    STARTUPINFOW startup = {sizeof(startup)};
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    WCHAR temp[MAX_PATH], output_path[MAX_PATH];
    WCHAR command[1024];
    HANDLE null_file = INVALID_HANDLE_VALUE;
    HANDLE output_file = INVALID_HANDLE_VALUE;
    DWORD exit_code = ~0u, wait, read = 0;
    LARGE_INTEGER size = {0};
    BOOL ret, got_exit;

    if (output && output_capacity) output[0] = 0;
    ret = GetTempPathW( ARRAY_SIZE(temp), temp ) &&
          GetTempFileNameW( temp, L"wax", 0, output_path );
    ok( ret, "Failed to reserve an output file, error %lu.\n",
        GetLastError() );
    if (!ret) return ~0u;
    output_file = CreateFileW(
        output_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        NULL );
    if (output_file == INVALID_HANDLE_VALUE) DeleteFileW( output_path );
    ok( output_file != INVALID_HANDLE_VALUE,
        "Failed to open the output file, error %lu.\n", GetLastError() );
    if (output_file == INVALID_HANDLE_VALUE) return ~0u;
    null_file = CreateFileW( L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( null_file != INVALID_HANDLE_VALUE,
        "Failed to open NUL, error %lu.\n", GetLastError() );
    if (null_file == INVALID_HANDLE_VALUE)
    {
        CloseHandle( output_file );
        return ~0u;
    }
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_file;
    startup.hStdOutput = output_file;
    startup.hStdError = null_file;
    swprintf( command, ARRAY_SIZE(command), L"wineappx.exe %s", arguments );
    ret = CreateProcessW( NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                          NULL, NULL,
                          &startup, &process );
    CloseHandle( null_file );
    ok( ret, "CreateProcessW failed, error %lu.\n", GetLastError() );
    if (!ret)
    {
        CloseHandle( output_file );
        return ~0u;
    }
    wait = WaitForSingleObject( process.hProcess, COMMAND_TIMEOUT );
    ok( wait == WAIT_OBJECT_0, "wineappx.exe did not exit promptly.\n" );
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess( process.hProcess, 255 );
        WaitForSingleObject( process.hProcess, COMMAND_TIMEOUT );
    }
    got_exit = GetExitCodeProcess( process.hProcess, &exit_code );
    ok( got_exit, "GetExitCodeProcess failed, error %lu.\n", GetLastError() );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );

    if (output && output_capacity)
    {
        ret = GetFileSizeEx( output_file, &size );
        ok( ret, "GetFileSizeEx failed, error %lu.\n", GetLastError() );
        ok( !ret || size.QuadPart < output_capacity,
            "wineappx.exe output is unexpectedly large, size %s.\n",
            wine_dbgstr_longlong(size.QuadPart) );
        if (ret && size.QuadPart < output_capacity)
        {
            SetFilePointer( output_file, 0, NULL, FILE_BEGIN );
            ret = ReadFile( output_file, output, (DWORD)size.QuadPart,
                            &read, NULL );
            ok( ret && read == size.QuadPart,
                "Failed to read command output, error %lu, read %lu.\n",
                GetLastError(), read );
            output[ret ? read : 0] = 0;
        }
    }
    CloseHandle( output_file );
    return got_exit ? exit_code : ~0u;
}

static DWORD run_wineappx( const WCHAR *arguments )
{
    return run_wineappx_capture( arguments, NULL, 0 );
}

static void test_command_dispatch( void )
{
    static const struct
    {
        const WCHAR *command;
        DWORD expected;
        const char *output;
    } tests[] =
    {
        {L"list", 0, "package_count=0\r\n"},
        {L"recover", 0, "catalog_epoch="},
        {L"gc", 0, "reclaimed_entries="},
        {L"inspect Z:\\this-file-must-not-exist.msix", 1, NULL},
        {L"unpack Z:\\this-file-must-not-exist.msix "
         L"Z:\\this-directory-must-not-exist", 1, NULL},
        {L"install Z:\\this-file-must-not-exist.msix", 1, NULL},
        {L"update Z:\\this-file-must-not-exist.msix", 1, NULL},
        {L"remove Contoso.Missing_1.0.0.0_x64__8wekyb3d8bbwe", 1, NULL},
        {L"query Contoso.Missing_1.0.0.0_x64__8wekyb3d8bbwe", 1, NULL},
        {L"launch Contoso.Missing_1.0.0.0_x64__8wekyb3d8bbwe App", 1, NULL},
    };
    WCHAR root[MAX_PATH], arguments[1024];
    char output[32768];
    DWORD exit_code;
    unsigned int i;

    if (!make_test_root( root ))
    {
        skip( "Failed to reserve a deployment store path.\n" );
        return;
    }
    swprintf( arguments, ARRAY_SIZE(arguments),
              L"--store \"%s\" --accept-weak-durability "
              L"--free-space-floor 1 initialize", root );
    exit_code = run_wineappx_capture(
        arguments, output, ARRAY_SIZE(output) );
    if (exit_code == 3)
    {
        skip( "The appxsvc private API is not installed for wineappx.\n" );
        remove_tree( root );
        return;
    }
    ok( exit_code == 0, "initialize returned exit code %lu.\n", exit_code );
    ok( strstr( output, "catalog_epoch=0\r\n" ) != NULL,
        "initialize output is missing the initial epoch: %s\n", output );
    ok( strstr( output, "durability=" ) != NULL,
        "initialize output is missing durability: %s\n", output );

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        winetest_push_context( "%s", wine_dbgstr_w(tests[i].command) );
        swprintf( arguments, ARRAY_SIZE(arguments),
                  L"--store \"%s\" --accept-weak-durability "
                  L"--free-space-floor 1 %s", root, tests[i].command );
        exit_code = run_wineappx_capture(
            arguments, output, ARRAY_SIZE(output) );
        ok( exit_code == tests[i].expected,
            "Got exit code %lu, expected %lu.\n",
            exit_code, tests[i].expected );
        if (tests[i].output)
            ok( strstr( output, tests[i].output ) != NULL,
                "Output is missing %s: %s\n", tests[i].output, output );
        winetest_pop_context();
    }
    remove_tree( root );
    ok( GetFileAttributesW( root ) == INVALID_FILE_ATTRIBUTES,
        "Deployment store %s was not removed.\n", wine_dbgstr_w(root) );
}

START_TEST(wineappx)
{
    static const struct
    {
        const WCHAR *arguments;
        DWORD expected;
    } tests[] =
    {
        {L"--help", 0},
        {L"-h", 0},
        {L"--help ignored", 0},
        {L"", 2},
        {L"--", 2},
        {L"unknown", 2},
        {L"inspect", 2},
        {L"inspect one two", 2},
        {L"unpack", 2},
        {L"unpack one", 2},
        {L"unpack one two three", 2},
        {L"initialize extra", 2},
        {L"install", 2},
        {L"install one two", 2},
        {L"update", 2},
        {L"update one two", 2},
        {L"remove", 2},
        {L"remove one two", 2},
        {L"query", 2},
        {L"query one two", 2},
        {L"list extra", 2},
        {L"launch", 2},
        {L"launch package", 2},
        {L"launch package app extra", 2},
        {L"recover extra", 2},
        {L"gc extra", 2},
        {L"--wait list", 2},
        {L"--allow-downgrade list", 2},
        {L"--unknown list", 2},
        {L"--HELP", 2},
        {L"--wait=1 list", 2},
        {L"--allow-downgrade=1 list", 2},
        {L"--store", 2},
        {L"--store= list", 2},
        {L"--storehouse path list", 2},
        {L"--arch", 2},
        {L"--arch= list", 2},
        {L"--arch mips list", 2},
        {L"--max-archive", 2},
        {L"--max-archive= list", 2},
        {L"--max-archive 0 list", 2},
        {L"--max-archive -1 list", 2},
        {L"--max-archive 129G list", 2},
        {L"--max-archive 1.5G list", 2},
        {L"--max-archive 1KBjunk list", 2},
        {L"--max-expanded 65G list", 2},
        {L"--max-expanded 18446744073709551616 list", 2},
        {L"--free-space-floor 0 list", 2},
        {L"--free-space-floor 18446744073709551616 list", 2},
        {L"--free-space-floor 1XB list", 2},
        {L"list --wait", 2},
        {L"-- --help", 2},
        {L"-- list extra", 2},
        {L"--store C:\\test --help", 0},
        {L"--arch=neutral --help", 0},
        {L"--arch=x86 --help", 0},
        {L"--arch=x64 --help", 0},
        {L"--arch=arm --help", 0},
        {L"--arch=arm64 --help", 0},
        {L"--arch=x86a64 --help", 0},
        {L"--max-archive 128G --help", 0},
        {L"--max-expanded=64GiB --help", 0},
        {L"--free-space-floor 1KiB --help", 0},
        {L"--free-space-floor=1kb --help", 0},
        {L"--accept-weak-durability --help", 0},
        {L"--wait --wait --help", 0},
        {L"LiSt extra", 2},
    };
    DWORD exit_code;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        winetest_push_context( "%s", wine_dbgstr_w(tests[i].arguments) );
        exit_code = run_wineappx( tests[i].arguments );
        ok( exit_code == tests[i].expected, "Got exit code %lu, expected %lu.\n",
            exit_code, tests[i].expected );
        winetest_pop_context();
    }
    test_command_dispatch();
}
