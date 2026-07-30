/*
 * AppX archive path validation
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

#include <limits.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"

#include "wine/appxsvc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

static BOOL equal_name( const WCHAR *name, UINT32 length, const WCHAR *expected )
{
    UINT32 expected_length = lstrlenW( expected );

    return length == expected_length &&
           CompareStringOrdinal( name, length, expected, expected_length, TRUE ) == CSTR_EQUAL;
}

static BOOL is_reserved_component( const WCHAR *component, UINT32 length )
{
    UINT32 basename_length = 0;
    WCHAR digit;

    while (basename_length < length && component[basename_length] != '.') basename_length++;
    while (basename_length && component[basename_length - 1] == ' ') basename_length--;

    if (equal_name( component, basename_length, L"CON" ) ||
        equal_name( component, basename_length, L"PRN" ) ||
        equal_name( component, basename_length, L"AUX" ) ||
        equal_name( component, basename_length, L"NUL" ) ||
        equal_name( component, basename_length, L"CLOCK$" ) ||
        equal_name( component, basename_length, L"CONIN$" ) ||
        equal_name( component, basename_length, L"CONOUT$" ))
        return TRUE;

    if (basename_length != 4) return FALSE;
    if (!equal_name( component, 3, L"COM" ) && !equal_name( component, 3, L"LPT" )) return FALSE;

    digit = component[3];
    return (digit >= '1' && digit <= '9') || digit == 0x00b9 || digit == 0x00b2 || digit == 0x00b3;
}

static BOOL validate_component( const WCHAR *component, UINT32 length )
{
    UINT32 i;

    if (!length || length > WINE_APPX_MAX_COMPONENT_CHARS) return FALSE;
    if ((length == 1 && component[0] == '.') ||
        (length == 2 && component[0] == '.' && component[1] == '.'))
        return FALSE;
    if (component[length - 1] == '.' || component[length - 1] == ' ') return FALSE;
    if (is_reserved_component( component, length )) return FALSE;

    for (i = 0; i < length; i++)
    {
        WCHAR ch = component[i];

        if (ch < 0x20 || ch == 0x7f || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '\\' || ch == '|' || ch == '?' || ch == '*' || ch == 0)
            return FALSE;
    }

    return TRUE;
}

HRESULT WINAPI wine_appx_validate_archive_path( const BYTE *utf8, UINT32 utf8_length, UINT32 flags,
                                                UINT32 *path_length, WCHAR *path )
{
    UINT32 component_start, i, input_length, required, capacity;
    WCHAR *decoded;
    int count;

    TRACE( "utf8 %p, utf8_length %u, flags %#x, path_length %p, path %p.\n",
           utf8, utf8_length, flags, path_length, path );

    if (!path_length || !utf8 || !utf8_length || (flags & ~WINE_APPX_PATH_DIRECTORY))
        return E_INVALIDARG;

    capacity = *path_length;
    *path_length = 0;

    if (utf8_length > WINE_APPX_MAX_ENTRY_NAME_BYTES || utf8_length > INT_MAX)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    input_length = utf8_length;
    if (flags & WINE_APPX_PATH_DIRECTORY)
    {
        if (utf8[input_length - 1] != '/') return APPX_E_INVALID_PACKAGING_LAYOUT;
        input_length--;
    }
    else if (utf8[input_length - 1] == '/')
    {
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    if (!input_length || utf8[0] == '/' || utf8[0] == '\\')
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)utf8, input_length, NULL, 0 );
    if (!count || count >= WINE_APPX_MAX_PATH_CHARS) return APPX_E_INVALID_PACKAGING_LAYOUT;

    if (!(decoded = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*decoded) ))) return E_OUTOFMEMORY;
    if (MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)utf8, input_length,
                             decoded, count ) != count)
    {
        HeapFree( GetProcessHeap(), 0, decoded );
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    if (!IsNormalizedString( NormalizationC, decoded, count ))
    {
        HeapFree( GetProcessHeap(), 0, decoded );
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    component_start = 0;
    for (i = 0; i <= count; i++)
    {
        if (i != count && decoded[i] != '/')
        {
            if (decoded[i] == '\\')
            {
                HeapFree( GetProcessHeap(), 0, decoded );
                return APPX_E_INVALID_PACKAGING_LAYOUT;
            }
            continue;
        }

        if (!validate_component( decoded + component_start, i - component_start ))
        {
            HeapFree( GetProcessHeap(), 0, decoded );
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        }
        component_start = i + 1;
    }

    required = count + 1;
    *path_length = required;
    if (!path || capacity < required)
    {
        HeapFree( GetProcessHeap(), 0, decoded );
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    }

    for (i = 0; i < count; i++) path[i] = decoded[i] == '/' ? '\\' : decoded[i];
    path[count] = 0;
    HeapFree( GetProcessHeap(), 0, decoded );
    return S_OK;
}
