/*
 * Switchyard Mesa OpenGL loader path probe.
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#ifdef _WIN64
static const WCHAR mesa_dll_suffix[] = L"\\x86_64-windows\\opengl32.dll";
#else
static const WCHAR mesa_dll_suffix[] = L"\\i386-windows\\opengl32.dll";
#endif

static int fail( const char *message )
{
    fprintf( stderr, "%s (error %lu)\n", message, GetLastError() );
    return 1;
}

static WCHAR *get_environment_value( const WCHAR *name )
{
    DWORD length = GetEnvironmentVariableW( name, NULL, 0 );
    WCHAR *value;

    if (!length) return NULL;
    if (!(value = HeapAlloc( GetProcessHeap(), 0, length * sizeof(*value) ))) return NULL;
    if (GetEnvironmentVariableW( name, value, length ) != length - 1)
    {
        HeapFree( GetProcessHeap(), 0, value );
        return NULL;
    }
    return value;
}

static WCHAR *strip_nt_prefix( WCHAR *path )
{
    if (!wcsncmp( path, L"\\??\\", 4 )) return path + 4;
    return path;
}

int main(void)
{
    static const WCHAR hostile_path[] = L"Z:\\switchyard-attacker";
    WCHAR module_path[32768];
    WCHAR *expected_root, *expected_buffer, *expected_path, *internal_path, *loaded_path;
    DWORD length;
    HMODULE module;
    size_t expected_length;

    if (!(expected_root = get_environment_value( L"SWITCHYARD_EXPECTED_MESA_DLL_PATH" )))
        return fail( "expected Mesa path is missing" );
    if (!(internal_path = get_environment_value( L"SWITCHYARD_MESA_DLL_NT_PATH" )))
        return fail( "Wine-owned Mesa path is missing" );
    if (_wcsicmp( strip_nt_prefix( internal_path ), strip_nt_prefix( expected_root ) ))
    {
        fwprintf( stderr, L"unexpected Wine-owned Mesa path: %ls (expected %ls)\n",
                  internal_path, expected_root );
        return 1;
    }
    expected_length = wcslen( expected_root ) + ARRAY_SIZE(mesa_dll_suffix);
    if (!(expected_buffer = HeapAlloc( GetProcessHeap(), 0,
                                       expected_length * sizeof(*expected_buffer) )))
        return fail( "could not allocate expected Mesa path" );
    wcscpy( expected_buffer, expected_root );
    wcscat( expected_buffer, mesa_dll_suffix );

    if (!SetEnvironmentVariableW( L"SWITCHYARD_OPENGL_DLL_PATH", hostile_path ) ||
        !SetEnvironmentVariableW( L"SWITCHYARD_MESA_DLL_NT_PATH", hostile_path ))
        return fail( "could not replace Mesa loader environment" );
    if (!(module = LoadLibraryW( L"opengl32.dll" )))
        return fail( "could not load opengl32.dll after replacing the environment" );
    length = GetModuleFileNameW( module, module_path, ARRAY_SIZE(module_path) );
    if (!length || length == ARRAY_SIZE(module_path))
        return fail( "could not query the loaded opengl32.dll path" );

    loaded_path = strip_nt_prefix( module_path );
    expected_path = strip_nt_prefix( expected_buffer );
    if (_wcsicmp( loaded_path, expected_path ))
    {
        fwprintf( stderr, L"loaded unexpected opengl32.dll: %ls (expected %ls)\n",
                  loaded_path, expected_path );
        return 1;
    }

    wprintf( L"loaded trusted Mesa module: %ls\n", loaded_path );
    FreeLibrary( module );
    HeapFree( GetProcessHeap(), 0, expected_buffer );
    HeapFree( GetProcessHeap(), 0, internal_path );
    HeapFree( GetProcessHeap(), 0, expected_root );
    return 0;
}
