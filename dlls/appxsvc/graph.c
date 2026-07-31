/*
 * AppX packaged process graph
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
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"
#include "winternl.h"

#include "graph.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define GRAPH_HEADER_MAGIC_OFFSET                  0
#define GRAPH_HEADER_VERSION_OFFSET                8
#define GRAPH_HEADER_SIZE_OFFSET                   12
#define GRAPH_HEADER_TOTAL_SIZE_OFFSET             16
#define GRAPH_HEADER_FLAGS_OFFSET                  20
#define GRAPH_HEADER_EPOCH_OFFSET                  24
#define GRAPH_HEADER_REVISION_OFFSET               32
#define GRAPH_HEADER_TARGET_ARCHITECTURE_OFFSET    40
#define GRAPH_HEADER_PACKAGE_COUNT_OFFSET          44
#define GRAPH_HEADER_PACKAGES_OFFSET               48
#define GRAPH_HEADER_LOADER_COUNT_OFFSET           52
#define GRAPH_HEADER_LOADERS_OFFSET                56
#define GRAPH_HEADER_STRINGS_OFFSET                60
#define GRAPH_HEADER_STRINGS_SIZE_OFFSET           64
#define GRAPH_HEADER_ACTIVATION_KIND_OFFSET        68
#define GRAPH_HEADER_APPLICATION_ID_REF_OFFSET     72
#define GRAPH_HEADER_AUMID_REF_OFFSET              80
#define GRAPH_HEADER_EXECUTABLE_REF_OFFSET         88
#define GRAPH_HEADER_ENTRY_POINT_REF_OFFSET        96
#define GRAPH_HEADER_RESERVED_OFFSET               104

#define GRAPH_PACKAGE_VERSION_OFFSET               0
#define GRAPH_PACKAGE_ARCHITECTURE_OFFSET          8
#define GRAPH_PACKAGE_FLAGS_OFFSET                 12
#define GRAPH_PACKAGE_RANK_OFFSET                  16
#define GRAPH_PACKAGE_RESERVED_OFFSET              20
#define GRAPH_PACKAGE_CONTENT_ID_OFFSET            24
#define GRAPH_PACKAGE_NAME_REF_OFFSET              56
#define GRAPH_PACKAGE_PUBLISHER_REF_OFFSET         64
#define GRAPH_PACKAGE_RESOURCE_ID_REF_OFFSET       72
#define GRAPH_PACKAGE_PUBLISHER_ID_REF_OFFSET      80
#define GRAPH_PACKAGE_FULL_NAME_REF_OFFSET         88
#define GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET       96
#define GRAPH_PACKAGE_ROOT_REF_OFFSET              104

#define GRAPH_LOADER_PACKAGE_INDEX_OFFSET          0
#define GRAPH_LOADER_RESERVED_OFFSET               4
#define GRAPH_LOADER_BASENAME_REF_OFFSET           8
#define GRAPH_LOADER_PATH_REF_OFFSET               16

#define GRAPH_STRING_REF_SIZE                      8
#define GRAPH_HEADER_RESERVED_SIZE                 \
    (APPX_GRAPH_BLOB_HEADER_SIZE - GRAPH_HEADER_RESERVED_OFFSET)
#define GRAPH_MAX_EDGES                            \
    (APPX_GRAPH_MAX_PACKAGES * APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE)

static const BYTE graph_magic[8] = {'S','W','X','G','R','A','P','H'};

struct appx_package_graph
{
    UINT32 size;
    BYTE data[1];
};

struct graph_buffer
{
    BYTE *data;
    UINT32 size;
    UINT32 capacity;
};

struct candidate
{
    const struct appx_catalog_package *package;
};

struct selected_package
{
    const struct appx_catalog_package *package;
    UINT32 edge_start;
    UINT32 edge_count;
};

struct graph_edge
{
    UINT32 from;
    UINT32 to;
};

struct loader_build
{
    const WCHAR *relative_path;
    const WCHAR *basename;
    UINT32 package_index;
};

struct dependency_build
{
    const struct appx_catalog_dependency *dependency;
};

struct application_build
{
    const struct appx_catalog_application *application;
};

struct blob_string_ref
{
    UINT32 offset;
    UINT32 chars;
};

static UINT16 read_uint16( const BYTE *data )
{
    return data[0] | ((UINT16)data[1] << 8);
}

static UINT32 read_uint32( const BYTE *data )
{
    return read_uint16( data ) | ((UINT32)read_uint16( data + 2 ) << 16);
}

static UINT64 read_uint64( const BYTE *data )
{
    return read_uint32( data ) | ((UINT64)read_uint32( data + 4 ) << 32);
}

static void write_uint16( BYTE *data, UINT16 value )
{
    data[0] = value;
    data[1] = value >> 8;
}

static void write_uint32( BYTE *data, UINT32 value )
{
    write_uint16( data, value );
    write_uint16( data + 2, value >> 16 );
}

static void write_uint64( BYTE *data, UINT64 value )
{
    write_uint32( data, value );
    write_uint32( data + 4, value >> 32 );
}

static BOOL add_uint32( UINT32 left, UINT32 right, UINT32 *result )
{
    if (MAXDWORD - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL multiply_uint32( UINT32 left, UINT32 right, UINT32 *result )
{
    if (left && right > MAXDWORD / left) return FALSE;
    *result = left * right;
    return TRUE;
}

static HRESULT malformed_graph( void )
{
    return APPX_E_INVALID_PACKAGING_LAYOUT;
}

static BOOL is_valid_architecture( enum appx_catalog_architecture architecture )
{
    return architecture == APPX_CATALOG_ARCHITECTURE_NEUTRAL ||
           architecture == APPX_CATALOG_ARCHITECTURE_X86 ||
           architecture == APPX_CATALOG_ARCHITECTURE_X64 ||
           architecture == APPX_CATALOG_ARCHITECTURE_ARM ||
           architecture == APPX_CATALOG_ARCHITECTURE_ARM64 ||
           architecture == APPX_CATALOG_ARCHITECTURE_X86A64;
}

static BOOL architecture_is_compatible(
    enum appx_catalog_architecture package_architecture,
    enum appx_catalog_architecture target_architecture )
{
    return package_architecture == APPX_CATALOG_ARCHITECTURE_NEUTRAL ||
           package_architecture == target_architecture;
}

static BOOL is_valid_activation_kind( enum appx_catalog_activation_kind kind )
{
    return kind == APPX_CATALOG_ACTIVATION_UNSUPPORTED ||
           kind == APPX_CATALOG_ACTIVATION_FULL_TRUST ||
           kind == APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC ||
           kind == APPX_CATALOG_ACTIVATION_WIN32;
}

static BOOL is_launchable_activation_kind( enum appx_catalog_activation_kind kind )
{
    return kind == APPX_CATALOG_ACTIVATION_FULL_TRUST ||
           kind == APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC ||
           kind == APPX_CATALOG_ACTIVATION_WIN32;
}

static HRESULT bounded_string_length( const WCHAR *string, BOOL allow_empty,
                                      UINT32 *chars )
{
    UINT32 i;

    if (!string) return E_INVALIDARG;
    for (i = 0; i <= APPX_GRAPH_MAX_STRING_CHARS; i++)
    {
        WCHAR ch = string[i];

        if (!ch)
        {
            if (!allow_empty && !i) return malformed_graph();
            *chars = i + 1;
            return S_OK;
        }
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            WCHAR low;

            if (i == APPX_GRAPH_MAX_STRING_CHARS) return malformed_graph();
            low = string[++i];
            if (low < 0xdc00 || low > 0xdfff) return malformed_graph();
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return malformed_graph();
    }
    return malformed_graph();
}

static INT compare_string_ci( const WCHAR *left, const WCHAR *right )
{
    INT result = CompareStringOrdinal( left, -1, right, -1, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static INT compare_string_exact( const WCHAR *left, const WCHAR *right )
{
    INT result = CompareStringOrdinal( left, -1, right, -1, FALSE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static INT compare_string_canonical( const WCHAR *left, const WCHAR *right )
{
    INT result = compare_string_ci( left, right );

    return result ? result : compare_string_exact( left, right );
}

static INT compare_version( const struct appx_catalog_version *left,
                            const struct appx_catalog_version *right )
{
    if (left->major != right->major)
        return left->major < right->major ? -1 : 1;
    if (left->minor != right->minor)
        return left->minor < right->minor ? -1 : 1;
    if (left->build != right->build)
        return left->build < right->build ? -1 : 1;
    if (left->revision != right->revision)
        return left->revision < right->revision ? -1 : 1;
    return 0;
}

static UINT64 pack_version( const struct appx_catalog_version *version )
{
    return version->major | ((UINT64)version->minor << 16) |
           ((UINT64)version->build << 32) | ((UINT64)version->revision << 48);
}

static struct appx_catalog_version unpack_version( UINT64 value )
{
    struct appx_catalog_version version;

    version.major = value;
    version.minor = value >> 16;
    version.build = value >> 32;
    version.revision = value >> 48;
    return version;
}

static WCHAR ascii_upper( WCHAR ch )
{
    if (ch >= 'a' && ch <= 'z') return ch - ('a' - 'A');
    return ch;
}

static BOOL reserved_component_name( const WCHAR *string, UINT32 start, UINT32 end )
{
    WCHAR name[5] = {0};
    UINT32 i, base_end = start;

    while (base_end < end && string[base_end] != '.') base_end++;
    if (base_end - start < 3 || base_end - start > 4) return FALSE;
    for (i = start; i < base_end; i++) name[i - start] = ascii_upper( string[i] );
    if (base_end - start == 3)
        return (!memcmp( name, L"CON", 3 * sizeof(WCHAR) ) ||
                !memcmp( name, L"PRN", 3 * sizeof(WCHAR) ) ||
                !memcmp( name, L"AUX", 3 * sizeof(WCHAR) ) ||
                !memcmp( name, L"NUL", 3 * sizeof(WCHAR) ));
    return ((!memcmp( name, L"COM", 3 * sizeof(WCHAR) ) ||
             !memcmp( name, L"LPT", 3 * sizeof(WCHAR) )) &&
            name[3] >= '1' && name[3] <= '9');
}

static HRESULT validate_relative_path( const WCHAR *path, BOOL basename_only )
{
    UINT32 chars, length, component_start = 0, i;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( path, FALSE, &chars ))) return hr;
    length = chars - 1;
    if (path[0] == '\\' || path[0] == '/') return malformed_graph();
    for (i = 0; i <= length; i++)
    {
        WCHAR ch = i == length ? '\\' : path[i];

        if (ch == '/')
            return malformed_graph();
        if (ch != '\\')
        {
            if (ch < 0x20 || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
                ch == '|' || ch == '?' || ch == '*')
                return malformed_graph();
            continue;
        }
        if (basename_only && i != length) return malformed_graph();
        if (i == component_start || path[i - 1] == ' ' || path[i - 1] == '.')
            return malformed_graph();
        if (i - component_start > 255) return malformed_graph();
        if ((i - component_start == 1 && path[component_start] == '.') ||
            (i - component_start == 2 && path[component_start] == '.' &&
             path[component_start + 1] == '.') ||
            reserved_component_name( path, component_start, i ))
            return malformed_graph();
        component_start = i + 1;
    }
    return S_OK;
}

static HRESULT validate_absolute_store_root( const WCHAR *path )
{
    UINT32 chars, length, component_start, i;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( path, FALSE, &chars ))) return hr;
    length = chars - 1;
    if (length < 3 ||
        !((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) ||
        path[1] != ':' || path[2] != '\\' ||
        (length > 3 && path[length - 1] == '\\'))
        return malformed_graph();
    if (length == 3) return S_OK;
    component_start = 3;
    for (i = 3; i <= length; i++)
    {
        WCHAR ch = i == length ? '\\' : path[i];

        if (ch == '/') return malformed_graph();
        if (ch != '\\')
        {
            if (ch < 0x20 || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
                ch == '|' || ch == '?' || ch == '*')
                return malformed_graph();
            continue;
        }
        if (i == component_start || path[i - 1] == ' ' || path[i - 1] == '.' ||
            i - component_start > 255 ||
            (i - component_start == 1 && path[component_start] == '.') ||
            (i - component_start == 2 && path[component_start] == '.' &&
             path[component_start + 1] == '.') ||
            reserved_component_name( path, component_start, i ))
            return malformed_graph();
        component_start = i + 1;
    }
    return S_OK;
}

static HRESULT combine_package_root( const WCHAR *store_root,
                                     const WCHAR *payload_path,
                                     WCHAR **package_root )
{
    UINT32 store_chars, payload_chars, total_chars, position;
    BOOL add_separator;
    HRESULT hr;

    *package_root = NULL;
    if (FAILED(hr = validate_absolute_store_root( store_root )) ||
        FAILED(hr = bounded_string_length( store_root, FALSE, &store_chars )) ||
        FAILED(hr = validate_relative_path( payload_path, FALSE )) ||
        FAILED(hr = bounded_string_length( payload_path, FALSE, &payload_chars )))
        return hr;
    add_separator = store_root[store_chars - 2] != '\\';
    if (!add_uint32( store_chars - 1, payload_chars, &total_chars ) ||
        (add_separator && !add_uint32( total_chars, 1, &total_chars )) ||
        total_chars - 1 > APPX_GRAPH_MAX_STRING_CHARS)
        return malformed_graph();
    if (!(*package_root = HeapAlloc( GetProcessHeap(), 0,
                                     total_chars * sizeof(**package_root) )))
        return E_OUTOFMEMORY;
    memcpy( *package_root, store_root,
            (store_chars - 1) * sizeof(**package_root) );
    position = store_chars - 1;
    if (add_separator) (*package_root)[position++] = '\\';
    memcpy( *package_root + position, payload_path,
            payload_chars * sizeof(**package_root) );
    return S_OK;
}

static const WCHAR *path_basename( const WCHAR *path )
{
    const WCHAR *cursor, *result = path;

    for (cursor = path; *cursor; cursor++)
        if (*cursor == '\\') result = cursor + 1;
    return result;
}

static HRESULT validate_catalog_package_shape(
    const struct appx_catalog_package *package )
{
    UINT32 chars, i;
    HRESULT hr;

    if (!package || (package->flags & ~APPX_CATALOG_PACKAGE_KNOWN_FLAGS) ||
        !is_valid_architecture( package->architecture ) ||
        package->application_count > APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE ||
        package->dependency_count > APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE ||
        (package->application_count && !package->applications) ||
        (package->dependency_count && !package->dependencies))
        return malformed_graph();

    if (FAILED(hr = bounded_string_length( package->name, FALSE, &chars )) ||
        FAILED(hr = bounded_string_length( package->publisher, FALSE, &chars )) ||
        FAILED(hr = bounded_string_length( package->resource_id, TRUE, &chars )) ||
        FAILED(hr = bounded_string_length( package->publisher_id, FALSE, &chars )) ||
        FAILED(hr = bounded_string_length( package->full_name, FALSE, &chars )) ||
        FAILED(hr = bounded_string_length( package->family_name, FALSE, &chars )) ||
        FAILED(hr = validate_relative_path( package->payload_path, FALSE )))
        return FAILED(hr) ? hr : malformed_graph();

    for (i = 0; i < package->application_count; i++)
    {
        const struct appx_catalog_application *application =
            package->applications + i;

        if (!is_valid_activation_kind( application->activation_kind ) ||
            FAILED(hr = bounded_string_length( application->id, FALSE, &chars )) ||
            FAILED(hr = validate_relative_path( application->executable, FALSE )) ||
            FAILED(hr = bounded_string_length( application->entry_point, TRUE, &chars )))
            return FAILED(hr) ? hr : malformed_graph();
    }
    for (i = 0; i < package->dependency_count; i++)
    {
        const struct appx_catalog_dependency *dependency =
            package->dependencies + i;

        if (FAILED(hr = bounded_string_length( dependency->name, FALSE, &chars )) ||
            FAILED(hr = bounded_string_length( dependency->publisher, FALSE, &chars )))
            return FAILED(hr) ? hr : malformed_graph();
    }
    return S_OK;
}

static INT __cdecl compare_candidate( const void *left_ptr, const void *right_ptr )
{
    const struct candidate *left = left_ptr, *right = right_ptr;
    INT result;

    if ((result = compare_string_ci( left->package->name, right->package->name )))
        return result;
    if ((result = compare_string_exact( left->package->publisher,
                                        right->package->publisher )))
        return result;
    if ((result = compare_version( &left->package->version,
                                   &right->package->version )))
        return result;
    if (left->package->architecture != right->package->architecture)
        return left->package->architecture < right->package->architecture ? -1 : 1;
    return compare_string_canonical( left->package->full_name,
                                     right->package->full_name );
}

static INT compare_candidate_key( const struct appx_catalog_package *package,
                                  const WCHAR *name, const WCHAR *publisher )
{
    INT result;

    if ((result = compare_string_ci( package->name, name ))) return result;
    return compare_string_exact( package->publisher, publisher );
}

static HRESULT resolve_dependency( const struct candidate *candidates,
                                   UINT32 candidate_count,
                                   const struct appx_catalog_dependency *dependency,
                                   enum appx_catalog_architecture target_architecture,
                                   const struct appx_catalog_package **package )
{
    const struct appx_catalog_package *best = NULL;
    UINT32 low = 0, high = candidate_count, i;

    *package = NULL;
    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;

        if (compare_candidate_key( candidates[middle].package, dependency->name,
                                   dependency->publisher ) < 0)
            low = middle + 1;
        else
            high = middle;
    }

    for (i = low; i < candidate_count; i++)
    {
        const struct appx_catalog_package *candidate = candidates[i].package;
        INT version_result;

        if (compare_candidate_key( candidate, dependency->name,
                                   dependency->publisher ))
            break;
        if (!(candidate->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
            !architecture_is_compatible( candidate->architecture,
                                         target_architecture ) ||
            compare_version( &candidate->version,
                             &dependency->min_version ) < 0)
            continue;
        version_result = best ? compare_version( &candidate->version,
                                                 &best->version ) : 1;
        if (version_result > 0)
            best = candidate;
        else if (!version_result &&
                 compare_string_ci( candidate->full_name, best->full_name ))
            return APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED;
    }
    if (!best) return APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED;
    if (!(best->flags & APPX_CATALOG_PACKAGE_FRAMEWORK) ||
        !(best->flags & APPX_CATALOG_PACKAGE_SIGNED) ||
        (best->flags & APPX_CATALOG_PACKAGE_RESOURCE))
        return APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED;
    *package = best;
    return S_OK;
}

static HRESULT append_edge( struct graph_edge **edges, UINT32 *count,
                            UINT32 *capacity, UINT32 from, UINT32 to )
{
    struct graph_edge *replacement;
    UINT32 new_capacity;

    if (*count >= GRAPH_MAX_EDGES) return APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED;
    if (*count == *capacity)
    {
        new_capacity = *capacity ? *capacity * 2 : 64;
        if (new_capacity > GRAPH_MAX_EDGES) new_capacity = GRAPH_MAX_EDGES;
        if (!(replacement = HeapReAlloc( GetProcessHeap(), 0, *edges,
                                         new_capacity * sizeof(**edges) )))
        {
            if (*edges) return E_OUTOFMEMORY;
            if (!(replacement = HeapAlloc( GetProcessHeap(), 0,
                                           new_capacity * sizeof(**edges) )))
                return E_OUTOFMEMORY;
        }
        *edges = replacement;
        *capacity = new_capacity;
    }
    (*edges)[*count].from = from;
    (*edges)[*count].to = to;
    (*count)++;
    return S_OK;
}

static BOOL dependency_keys_equal( const struct appx_catalog_dependency *left,
                                   const struct appx_catalog_dependency *right )
{
    return !compare_string_ci( left->name, right->name ) &&
           !compare_string_exact( left->publisher, right->publisher );
}

static INT __cdecl compare_dependency_build( const void *left_ptr,
                                             const void *right_ptr )
{
    const struct dependency_build *left = left_ptr, *right = right_ptr;
    INT result;

    if ((result = compare_string_ci( left->dependency->name,
                                     right->dependency->name )))
        return result;
    return compare_string_exact( left->dependency->publisher,
                                 right->dependency->publisher );
}

static INT __cdecl compare_application_build( const void *left_ptr,
                                              const void *right_ptr )
{
    const struct application_build *left = left_ptr, *right = right_ptr;

    return compare_string_exact( left->application->id,
                                 right->application->id );
}

static INT find_selected_package( const struct selected_package *selected,
                                  UINT32 count,
                                  const struct appx_catalog_package *package )
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (!compare_string_ci( selected[i].package->full_name,
                                package->full_name ))
            return i;
    return -1;
}

static BOOL graph_visit_cycle( UINT32 index,
                               const struct selected_package *selected,
                               const struct graph_edge *edges, BYTE *colors )
{
    UINT32 i;

    if (colors[index] == 1) return TRUE;
    if (colors[index] == 2) return FALSE;
    colors[index] = 1;
    for (i = 0; i < selected[index].edge_count; i++)
        if (graph_visit_cycle( edges[selected[index].edge_start + i].to,
                               selected, edges, colors ))
            return TRUE;
    colors[index] = 2;
    return FALSE;
}

static HRESULT build_selection(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *package_full_name,
    const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture,
    struct selected_package selected[APPX_GRAPH_MAX_PACKAGES],
    UINT32 *selected_count, const struct appx_catalog_application **application,
    struct graph_edge **edges, UINT32 *edge_count )
{
    const struct appx_catalog_package *main_package = NULL;
    struct candidate *candidates = NULL;
    struct dependency_build dependency_index[
        APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE];
    struct application_build application_index[
        APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE];
    BYTE colors[APPX_GRAPH_MAX_PACKAGES] = {0};
    UINT32 candidate_count, edge_capacity = 0, i, cursor;
    HRESULT hr = S_OK;

    *selected_count = 0;
    *application = NULL;
    *edges = NULL;
    *edge_count = 0;

    candidate_count = appx_catalog_snapshot_get_package_count( snapshot );
    if (candidate_count > APPX_CATALOG_MAX_PACKAGES)
        return malformed_graph();
    if (candidate_count &&
        !(candidates = HeapAlloc( GetProcessHeap(), 0,
                                  candidate_count * sizeof(*candidates) )))
        return E_OUTOFMEMORY;

    for (i = 0; i < candidate_count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( snapshot, i );

        if (FAILED(hr = validate_catalog_package_shape( package ))) goto done;
        candidates[i].package = package;
        if (i && compare_string_canonical( candidates[i - 1].package->full_name,
                                           package->full_name ) >= 0)
        {
            hr = malformed_graph();
            goto done;
        }
        if (!compare_string_ci( package->full_name, package_full_name ))
        {
            if (main_package)
            {
                hr = malformed_graph();
                goto done;
            }
            main_package = package;
        }
    }
    if (!main_package)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
        goto done;
    }
    if (!(main_package->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
        !(main_package->flags & APPX_CATALOG_PACKAGE_SIGNED) ||
        (main_package->flags & (APPX_CATALOG_PACKAGE_FRAMEWORK |
                                APPX_CATALOG_PACKAGE_RESOURCE)))
    {
        hr = HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
        goto done;
    }
    if (!architecture_is_compatible( main_package->architecture,
                                     target_architecture ))
    {
        hr = HRESULT_FROM_WIN32( ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
        goto done;
    }

    for (i = 0; i < main_package->application_count; i++)
    {
        const struct appx_catalog_application *candidate =
            main_package->applications + i;

        application_index[i].application = candidate;
        if (!compare_string_exact( candidate->id, application_id ))
        {
            if (*application)
            {
                hr = malformed_graph();
                goto done;
            }
            *application = candidate;
        }
    }
    qsort( application_index, main_package->application_count,
           sizeof(*application_index), compare_application_build );
    for (i = 1; i < main_package->application_count; i++)
        if (!compare_string_exact( application_index[i - 1].application->id,
                                   application_index[i].application->id ))
        {
            hr = malformed_graph();
            goto done;
        }
    if (!*application)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
        goto done;
    }
    if (!is_launchable_activation_kind( (*application)->activation_kind ))
    {
        hr = HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        goto done;
    }

    qsort( candidates, candidate_count, sizeof(*candidates), compare_candidate );
    memset( selected, 0,
            APPX_GRAPH_MAX_PACKAGES * sizeof(*selected) );
    selected[0].package = main_package;
    *selected_count = 1;

    for (cursor = 0; cursor < *selected_count; cursor++)
    {
        const struct appx_catalog_package *package = selected[cursor].package;

        selected[cursor].edge_start = *edge_count;
        for (i = 0; i < package->dependency_count; i++)
            dependency_index[i].dependency = package->dependencies + i;
        qsort( dependency_index, package->dependency_count,
               sizeof(*dependency_index), compare_dependency_build );
        for (i = 1; i < package->dependency_count; i++)
            if (dependency_keys_equal( dependency_index[i - 1].dependency,
                                       dependency_index[i].dependency ))
            {
                hr = malformed_graph();
                goto done;
            }
        for (i = 0; i < package->dependency_count; i++)
        {
            const struct appx_catalog_package *resolved;
            INT selected_index;

            if (FAILED(hr = resolve_dependency(
                    candidates, candidate_count, package->dependencies + i,
                    target_architecture, &resolved )))
                goto done;
            selected_index = find_selected_package( selected, *selected_count,
                                                    resolved );
            if (selected_index < 0)
            {
                if (*selected_count >= APPX_GRAPH_MAX_PACKAGES)
                {
                    hr = APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED;
                    goto done;
                }
                selected_index = (*selected_count)++;
                selected[selected_index].package = resolved;
            }
            if (FAILED(hr = append_edge( edges, edge_count, &edge_capacity,
                                         cursor, selected_index )))
                goto done;
        }
        selected[cursor].edge_count = *edge_count - selected[cursor].edge_start;
    }

    if (graph_visit_cycle( 0, selected, *edges, colors ))
        hr = APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED;

done:
    HeapFree( GetProcessHeap(), 0, candidates );
    if (FAILED(hr))
    {
        HeapFree( GetProcessHeap(), 0, *edges );
        *edges = NULL;
        *edge_count = 0;
        *selected_count = 0;
        *application = NULL;
    }
    return hr;
}

static INT __cdecl compare_loader_build( const void *left_ptr,
                                         const void *right_ptr )
{
    const struct loader_build *left = left_ptr, *right = right_ptr;
    INT result;

    if ((result = compare_string_ci( left->basename, right->basename )))
        return result;
    if (left->package_index != right->package_index)
        return left->package_index < right->package_index ? -1 : 1;
    if ((result = compare_string_exact( left->basename, right->basename )))
        return result;
    return compare_string_canonical( left->relative_path,
                                     right->relative_path );
}

static HRESULT build_loader_index(
    const struct selected_package *selected, UINT32 selected_count,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_file_count,
    struct loader_build **loaders )
{
    struct loader_build *result = NULL;
    UINT32 i, j;
    HRESULT hr;

    *loaders = NULL;
    if (loader_file_count > APPX_GRAPH_MAX_LOADER_FILES ||
        (loader_file_count && !loader_files))
        return E_INVALIDARG;
    if (loader_file_count &&
        !(result = HeapAlloc( GetProcessHeap(), 0,
                              loader_file_count * sizeof(*result) )))
        return E_OUTOFMEMORY;

    for (i = 0; i < loader_file_count; i++)
    {
        UINT32 chars;
        INT package_index = -1;

        if (FAILED(hr = bounded_string_length(
                loader_files[i].package_full_name, FALSE, &chars )) ||
            FAILED(hr = validate_relative_path(
                loader_files[i].relative_path, FALSE )))
            goto failed;
        for (j = 0; j < selected_count; j++)
            if (!compare_string_ci( selected[j].package->full_name,
                                    loader_files[i].package_full_name ))
            {
                if (package_index >= 0)
                {
                    hr = malformed_graph();
                    goto failed;
                }
                package_index = j;
            }
        if (package_index < 0)
        {
            hr = E_INVALIDARG;
            goto failed;
        }
        result[i].package_index = package_index;
        result[i].relative_path = loader_files[i].relative_path;
        result[i].basename = path_basename( loader_files[i].relative_path );
    }
    qsort( result, loader_file_count, sizeof(*result), compare_loader_build );
    for (i = 1; i < loader_file_count; i++)
        if (result[i - 1].package_index == result[i].package_index &&
            !compare_string_ci( result[i - 1].basename, result[i].basename ))
        {
            hr = malformed_graph();
            goto failed;
        }
    *loaders = result;
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, result );
    return hr;
}

static HRESULT buffer_reserve( struct graph_buffer *buffer, UINT32 additional )
{
    BYTE *data;
    UINT32 needed, capacity;

    if (!add_uint32( buffer->size, additional, &needed ) ||
        needed > APPX_GRAPH_MAX_BLOB_SIZE)
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    if (needed <= buffer->capacity) return S_OK;
    capacity = buffer->capacity ? buffer->capacity : 4096;
    while (capacity < needed)
    {
        if (capacity > APPX_GRAPH_MAX_BLOB_SIZE / 2)
        {
            capacity = APPX_GRAPH_MAX_BLOB_SIZE;
            break;
        }
        capacity *= 2;
    }
    if (buffer->data)
        data = HeapReAlloc( GetProcessHeap(), 0, buffer->data, capacity );
    else
        data = HeapAlloc( GetProcessHeap(), 0, capacity );
    if (!data) return E_OUTOFMEMORY;
    buffer->data = data;
    buffer->capacity = capacity;
    return S_OK;
}

static HRESULT buffer_append_zero( struct graph_buffer *buffer, UINT32 size,
                                   UINT32 *offset )
{
    HRESULT hr;

    if (FAILED(hr = buffer_reserve( buffer, size ))) return hr;
    if (offset) *offset = buffer->size;
    memset( buffer->data + buffer->size, 0, size );
    buffer->size += size;
    return S_OK;
}

static void write_string_ref( BYTE *record, UINT32 offset, UINT32 chars )
{
    write_uint32( record, offset );
    write_uint32( record + 4, chars );
}

static HRESULT buffer_append_string( struct graph_buffer *buffer,
                                     const WCHAR *string, BOOL allow_empty,
                                     UINT32 ref_offset )
{
    UINT32 chars, bytes, offset, i;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( string, allow_empty, &chars )))
        return hr;
    if (!multiply_uint32( chars, sizeof(WCHAR), &bytes ))
        return malformed_graph();
    if (FAILED(hr = buffer_append_zero( buffer, bytes, &offset ))) return hr;
    for (i = 0; i < chars; i++)
        write_uint16( buffer->data + offset + i * sizeof(WCHAR), string[i] );
    write_string_ref( buffer->data + ref_offset, offset, chars );
    return S_OK;
}

static HRESULT create_aumid( const WCHAR *family_name, const WCHAR *application_id,
                             WCHAR **aumid )
{
    UINT32 family_chars, application_chars, total_chars;
    HRESULT hr;

    *aumid = NULL;
    if (FAILED(hr = bounded_string_length( family_name, FALSE, &family_chars )) ||
        FAILED(hr = bounded_string_length( application_id, FALSE,
                                            &application_chars )))
        return hr;
    if (!add_uint32( family_chars, application_chars, &total_chars ) ||
        total_chars > APPX_GRAPH_MAX_STRING_CHARS + 1)
        return malformed_graph();
    if (!(*aumid = HeapAlloc( GetProcessHeap(), 0,
                              total_chars * sizeof(**aumid) )))
        return E_OUTOFMEMORY;
    memcpy( *aumid, family_name, (family_chars - 1) * sizeof(**aumid) );
    (*aumid)[family_chars - 1] = '!';
    memcpy( *aumid + family_chars, application_id,
            application_chars * sizeof(**aumid) );
    return S_OK;
}

static HRESULT serialize_graph(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *store_root,
    const struct selected_package *selected, UINT32 selected_count,
    const struct appx_catalog_application *application,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct loader_build *loaders, UINT32 loader_count,
    BYTE **blob, UINT32 *blob_size )
{
    struct graph_buffer buffer = {0};
    WCHAR *aumid = NULL;
    UINT32 package_bytes, loader_bytes, fixed_size, i, record_offset;
    HRESULT hr;

    *blob = NULL;
    *blob_size = 0;
    if (!multiply_uint32( selected_count,
                          APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE,
                          &package_bytes ) ||
        !multiply_uint32( loader_count,
                          APPX_GRAPH_BLOB_LOADER_RECORD_SIZE,
                          &loader_bytes ) ||
        !add_uint32( APPX_GRAPH_BLOB_HEADER_SIZE, package_bytes, &fixed_size ) ||
        !add_uint32( fixed_size, loader_bytes, &fixed_size ))
        return malformed_graph();
    if (FAILED(hr = buffer_append_zero( &buffer, fixed_size, NULL ))) goto done;
    if (FAILED(hr = create_aumid( selected[0].package->family_name,
                                  application->id, &aumid )))
        goto done;

    if (FAILED(hr = buffer_append_string(
            &buffer, application->id, FALSE,
            GRAPH_HEADER_APPLICATION_ID_REF_OFFSET )) ||
        FAILED(hr = buffer_append_string(
            &buffer, aumid, FALSE, GRAPH_HEADER_AUMID_REF_OFFSET )) ||
        FAILED(hr = buffer_append_string(
            &buffer, application->executable, FALSE,
            GRAPH_HEADER_EXECUTABLE_REF_OFFSET )) ||
        FAILED(hr = buffer_append_string(
            &buffer, application->entry_point, TRUE,
            GRAPH_HEADER_ENTRY_POINT_REF_OFFSET )))
        goto done;

    for (i = 0; i < selected_count; i++)
    {
        const struct appx_catalog_package *package = selected[i].package;
        WCHAR *absolute_root = NULL;
        BYTE *record;

        record_offset = APPX_GRAPH_BLOB_HEADER_SIZE +
                        i * APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
        record = buffer.data + record_offset;
        write_uint64( record + GRAPH_PACKAGE_VERSION_OFFSET,
                      pack_version( &package->version ) );
        write_uint32( record + GRAPH_PACKAGE_ARCHITECTURE_OFFSET,
                      package->architecture );
        write_uint32( record + GRAPH_PACKAGE_FLAGS_OFFSET, package->flags );
        write_uint32( record + GRAPH_PACKAGE_RANK_OFFSET, i );
        memcpy( record + GRAPH_PACKAGE_CONTENT_ID_OFFSET, package->content_id,
                APPX_CATALOG_CONTENT_ID_SIZE );
        if (FAILED(hr = combine_package_root(
                store_root, package->payload_path, &absolute_root )))
            goto done;
        if (FAILED(hr = buffer_append_string(
                &buffer, package->name, FALSE,
                record_offset + GRAPH_PACKAGE_NAME_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, package->publisher, FALSE,
                record_offset + GRAPH_PACKAGE_PUBLISHER_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, package->resource_id, TRUE,
                record_offset + GRAPH_PACKAGE_RESOURCE_ID_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, package->publisher_id, FALSE,
                record_offset + GRAPH_PACKAGE_PUBLISHER_ID_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, package->full_name, FALSE,
                record_offset + GRAPH_PACKAGE_FULL_NAME_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, package->family_name, FALSE,
                record_offset + GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, absolute_root, FALSE,
                record_offset + GRAPH_PACKAGE_ROOT_REF_OFFSET )))
        {
            HeapFree( GetProcessHeap(), 0, absolute_root );
            goto done;
        }
        HeapFree( GetProcessHeap(), 0, absolute_root );
    }

    for (i = 0; i < loader_count; i++)
    {
        UINT32 loader_offset = APPX_GRAPH_BLOB_HEADER_SIZE + package_bytes +
                               i * APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;

        write_uint32( buffer.data + loader_offset +
                      GRAPH_LOADER_PACKAGE_INDEX_OFFSET,
                      loaders[i].package_index );
        if (FAILED(hr = buffer_append_string(
                &buffer, loaders[i].basename, FALSE,
                loader_offset + GRAPH_LOADER_BASENAME_REF_OFFSET )) ||
            FAILED(hr = buffer_append_string(
                &buffer, loaders[i].relative_path, FALSE,
                loader_offset + GRAPH_LOADER_PATH_REF_OFFSET )))
            goto done;
    }

    memcpy( buffer.data + GRAPH_HEADER_MAGIC_OFFSET, graph_magic,
            sizeof(graph_magic) );
    write_uint32( buffer.data + GRAPH_HEADER_VERSION_OFFSET,
                  APPX_GRAPH_BLOB_VERSION );
    write_uint32( buffer.data + GRAPH_HEADER_SIZE_OFFSET,
                  APPX_GRAPH_BLOB_HEADER_SIZE );
    write_uint32( buffer.data + GRAPH_HEADER_TOTAL_SIZE_OFFSET, buffer.size );
    write_uint64( buffer.data + GRAPH_HEADER_EPOCH_OFFSET,
                  appx_catalog_snapshot_get_epoch( snapshot ) );
    write_uint64( buffer.data + GRAPH_HEADER_REVISION_OFFSET, revision );
    write_uint32( buffer.data + GRAPH_HEADER_TARGET_ARCHITECTURE_OFFSET,
                  target_architecture );
    write_uint32( buffer.data + GRAPH_HEADER_PACKAGE_COUNT_OFFSET,
                  selected_count );
    write_uint32( buffer.data + GRAPH_HEADER_PACKAGES_OFFSET,
                  APPX_GRAPH_BLOB_HEADER_SIZE );
    write_uint32( buffer.data + GRAPH_HEADER_LOADER_COUNT_OFFSET, loader_count );
    write_uint32( buffer.data + GRAPH_HEADER_LOADERS_OFFSET,
                  APPX_GRAPH_BLOB_HEADER_SIZE + package_bytes );
    write_uint32( buffer.data + GRAPH_HEADER_STRINGS_OFFSET, fixed_size );
    write_uint32( buffer.data + GRAPH_HEADER_STRINGS_SIZE_OFFSET,
                  buffer.size - fixed_size );
    write_uint32( buffer.data + GRAPH_HEADER_ACTIVATION_KIND_OFFSET,
                  application->activation_kind );

    *blob = buffer.data;
    *blob_size = buffer.size;
    buffer.data = NULL;
    hr = S_OK;

done:
    HeapFree( GetProcessHeap(), 0, aumid );
    HeapFree( GetProcessHeap(), 0, buffer.data );
    return hr;
}

static BOOL range_inside( UINT32 offset, UINT32 count, UINT32 element_size,
                          UINT32 limit, UINT32 *end )
{
    UINT32 bytes;

    return multiply_uint32( count, element_size, &bytes ) &&
           add_uint32( offset, bytes, end ) &&
           offset <= limit && *end <= limit;
}

static struct blob_string_ref get_blob_string_ref( const BYTE *record,
                                                   UINT32 offset )
{
    struct blob_string_ref ref;

    ref.offset = read_uint32( record + offset );
    ref.chars = read_uint32( record + offset + 4 );
    return ref;
}

static HRESULT validate_blob_string( const BYTE *data, UINT32 size,
                                     UINT32 strings_offset,
                                     struct blob_string_ref ref,
                                     BOOL allow_empty, UINT32 *expected_offset )
{
    UINT32 bytes, end, i;

    if (!ref.chars || ref.chars > APPX_GRAPH_MAX_STRING_CHARS + 1 ||
        (ref.offset & 1) || ref.offset != *expected_offset ||
        !multiply_uint32( ref.chars, sizeof(WCHAR), &bytes ) ||
        !add_uint32( ref.offset, bytes, &end ) ||
        ref.offset < strings_offset || end > size ||
        (!allow_empty && ref.chars == 1) ||
        read_uint16( data + end - sizeof(WCHAR) ))
        return malformed_graph();
    for (i = 0; i + 1 < ref.chars; i++)
    {
        UINT16 ch = read_uint16( data + ref.offset + i * sizeof(WCHAR) );

        if (!ch) return malformed_graph();
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            UINT16 low;

            if (++i + 1 >= ref.chars) return malformed_graph();
            low = read_uint16( data + ref.offset + i * sizeof(WCHAR) );
            if (low < 0xdc00 || low > 0xdfff) return malformed_graph();
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return malformed_graph();
    }
    *expected_offset = end;
    return S_OK;
}

static INT compare_blob_strings( const BYTE *data,
                                 struct blob_string_ref left,
                                 struct blob_string_ref right, BOOL ignore_case )
{
    UINT32 left_length = left.chars - 1, right_length = right.chars - 1;
    UINT32 length = left_length < right_length ? left_length : right_length, i;

    for (i = 0; i < length; i++)
    {
        WCHAR left_ch = read_uint16( data + left.offset + i * sizeof(WCHAR) );
        WCHAR right_ch = read_uint16( data + right.offset + i * sizeof(WCHAR) );

        if (ignore_case)
        {
            left_ch = RtlUpcaseUnicodeChar( left_ch );
            right_ch = RtlUpcaseUnicodeChar( right_ch );
        }
        if (left_ch != right_ch) return left_ch < right_ch ? -1 : 1;
    }
    if (left_length == right_length) return 0;
    return left_length < right_length ? -1 : 1;
}

static BOOL blob_strings_are_unique_ci(
    const BYTE *data, struct blob_string_ref *refs,
    struct blob_string_ref *scratch, UINT32 count )
{
    struct blob_string_ref *source = refs, *destination = scratch, *swap;
    UINT32 width, left, middle, right, i, j, output;

    for (width = 1; width < count; width *= 2)
    {
        for (left = 0; left < count; left += width * 2)
        {
            middle = left + width < count ? left + width : count;
            right = middle + width < count ? middle + width : count;
            i = left;
            j = middle;
            output = left;
            while (i < middle && j < right)
            {
                if (compare_blob_strings( data, source[i], source[j], TRUE ) <= 0)
                    destination[output++] = source[i++];
                else
                    destination[output++] = source[j++];
            }
            while (i < middle) destination[output++] = source[i++];
            while (j < right) destination[output++] = source[j++];
        }
        swap = source;
        source = destination;
        destination = swap;
    }
    for (i = 1; i < count; i++)
        if (!compare_blob_strings( data, source[i - 1], source[i], TRUE ))
            return FALSE;
    return TRUE;
}

static BOOL blob_string_equals_path_basename(
    const BYTE *data, struct blob_string_ref basename,
    struct blob_string_ref path )
{
    UINT32 start = 0, i, basename_length = basename.chars - 1;
    UINT32 path_length = path.chars - 1;

    for (i = 0; i < path_length; i++)
        if (read_uint16( data + path.offset + i * sizeof(WCHAR) ) == '\\')
            start = i + 1;
    if (path_length - start != basename_length) return FALSE;
    for (i = 0; i < basename_length; i++)
        if (read_uint16( data + basename.offset + i * sizeof(WCHAR) ) !=
            read_uint16( data + path.offset + (start + i) * sizeof(WCHAR) ))
            return FALSE;
    return TRUE;
}

static HRESULT copy_blob_string( const BYTE *data, struct blob_string_ref ref,
                                 WCHAR **string )
{
    UINT32 i;

    *string = HeapAlloc( GetProcessHeap(), 0, ref.chars * sizeof(**string) );
    if (!*string) return E_OUTOFMEMORY;
    for (i = 0; i < ref.chars; i++)
        (*string)[i] = read_uint16( data + ref.offset + i * sizeof(WCHAR) );
    return S_OK;
}

static HRESULT validate_blob_path( const BYTE *data,
                                   struct blob_string_ref ref,
                                   BOOL basename_only )
{
    WCHAR *path;
    HRESULT hr;

    if (FAILED(hr = copy_blob_string( data, ref, &path ))) return hr;
    hr = validate_relative_path( path, basename_only );
    HeapFree( GetProcessHeap(), 0, path );
    return hr;
}

static BOOL blob_aumid_matches( const BYTE *data,
                                struct blob_string_ref family,
                                struct blob_string_ref application_id,
                                struct blob_string_ref aumid )
{
    UINT32 family_length = family.chars - 1;
    UINT32 application_length = application_id.chars - 1, i;

    if (aumid.chars != family_length + application_length + 2) return FALSE;
    for (i = 0; i < family_length; i++)
        if (read_uint16( data + family.offset + i * sizeof(WCHAR) ) !=
            read_uint16( data + aumid.offset + i * sizeof(WCHAR) ))
            return FALSE;
    if (read_uint16( data + aumid.offset +
                     family_length * sizeof(WCHAR) ) != '!')
        return FALSE;
    for (i = 0; i < application_length; i++)
        if (read_uint16( data + application_id.offset + i * sizeof(WCHAR) ) !=
            read_uint16( data + aumid.offset +
                         (family_length + 1 + i) * sizeof(WCHAR) ))
            return FALSE;
    return TRUE;
}

HRESULT WINAPI appx_package_graph_validate_blob( const void *blob, SIZE_T size )
{
    const BYTE *data = blob;
    struct blob_string_ref application_id, aumid, executable, entry_point;
    struct blob_string_ref previous_basename = {0};
    struct blob_string_ref package_full_names[APPX_GRAPH_MAX_PACKAGES];
    struct blob_string_ref package_sort_scratch[APPX_GRAPH_MAX_PACKAGES];
    UINT32 package_count, package_offset, package_end;
    UINT32 loader_count, loader_offset, loader_end;
    UINT32 strings_offset, strings_size, strings_end;
    UINT32 expected_string, activation_kind, target_architecture;
    UINT32 i, j;
    HRESULT hr;

    if (!data || size < APPX_GRAPH_BLOB_HEADER_SIZE ||
        size > APPX_GRAPH_MAX_BLOB_SIZE)
        return E_INVALIDARG;
    if (memcmp( data + GRAPH_HEADER_MAGIC_OFFSET, graph_magic,
                sizeof(graph_magic) ) ||
        read_uint32( data + GRAPH_HEADER_VERSION_OFFSET ) !=
            APPX_GRAPH_BLOB_VERSION ||
        read_uint32( data + GRAPH_HEADER_SIZE_OFFSET ) !=
            APPX_GRAPH_BLOB_HEADER_SIZE ||
        read_uint32( data + GRAPH_HEADER_TOTAL_SIZE_OFFSET ) != size ||
        read_uint32( data + GRAPH_HEADER_FLAGS_OFFSET ))
        return malformed_graph();
    for (i = 0; i < GRAPH_HEADER_RESERVED_SIZE; i++)
        if (data[GRAPH_HEADER_RESERVED_OFFSET + i]) return malformed_graph();

    target_architecture =
        read_uint32( data + GRAPH_HEADER_TARGET_ARCHITECTURE_OFFSET );
    activation_kind = read_uint32( data + GRAPH_HEADER_ACTIVATION_KIND_OFFSET );
    package_count = read_uint32( data + GRAPH_HEADER_PACKAGE_COUNT_OFFSET );
    package_offset = read_uint32( data + GRAPH_HEADER_PACKAGES_OFFSET );
    loader_count = read_uint32( data + GRAPH_HEADER_LOADER_COUNT_OFFSET );
    loader_offset = read_uint32( data + GRAPH_HEADER_LOADERS_OFFSET );
    strings_offset = read_uint32( data + GRAPH_HEADER_STRINGS_OFFSET );
    strings_size = read_uint32( data + GRAPH_HEADER_STRINGS_SIZE_OFFSET );

    if (!is_valid_architecture( target_architecture ) ||
        !is_launchable_activation_kind( activation_kind ) ||
        !package_count || package_count > APPX_GRAPH_MAX_PACKAGES ||
        loader_count > APPX_GRAPH_MAX_LOADER_FILES ||
        package_offset != APPX_GRAPH_BLOB_HEADER_SIZE ||
        !range_inside( package_offset, package_count,
                       APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE,
                       size, &package_end ) ||
        loader_offset != package_end ||
        !range_inside( loader_offset, loader_count,
                       APPX_GRAPH_BLOB_LOADER_RECORD_SIZE,
                       size, &loader_end ) ||
        strings_offset != loader_end ||
        !add_uint32( strings_offset, strings_size, &strings_end ) ||
        strings_end != size)
        return malformed_graph();

    expected_string = strings_offset;
    application_id = get_blob_string_ref(
        data, GRAPH_HEADER_APPLICATION_ID_REF_OFFSET );
    aumid = get_blob_string_ref( data, GRAPH_HEADER_AUMID_REF_OFFSET );
    executable = get_blob_string_ref( data, GRAPH_HEADER_EXECUTABLE_REF_OFFSET );
    entry_point = get_blob_string_ref(
        data, GRAPH_HEADER_ENTRY_POINT_REF_OFFSET );
    if (FAILED(hr = validate_blob_string( data, size, strings_offset,
                                          application_id, FALSE,
                                          &expected_string )) ||
        FAILED(hr = validate_blob_string( data, size, strings_offset,
                                          aumid, FALSE, &expected_string )) ||
        FAILED(hr = validate_blob_string( data, size, strings_offset,
                                          executable, FALSE,
                                          &expected_string )) ||
        FAILED(hr = validate_blob_string( data, size, strings_offset,
                                          entry_point, TRUE,
                                          &expected_string )) ||
        FAILED(hr = validate_blob_path( data, executable, FALSE )))
        return hr;

    for (i = 0; i < package_count; i++)
    {
        const BYTE *record = data + package_offset +
                             i * APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
        struct blob_string_ref refs[7];
        UINT32 flags = read_uint32( record + GRAPH_PACKAGE_FLAGS_OFFSET );
        UINT32 architecture =
            read_uint32( record + GRAPH_PACKAGE_ARCHITECTURE_OFFSET );

        refs[0] = get_blob_string_ref( record, GRAPH_PACKAGE_NAME_REF_OFFSET );
        refs[1] = get_blob_string_ref( record, GRAPH_PACKAGE_PUBLISHER_REF_OFFSET );
        refs[2] = get_blob_string_ref( record, GRAPH_PACKAGE_RESOURCE_ID_REF_OFFSET );
        refs[3] = get_blob_string_ref( record, GRAPH_PACKAGE_PUBLISHER_ID_REF_OFFSET );
        refs[4] = get_blob_string_ref( record, GRAPH_PACKAGE_FULL_NAME_REF_OFFSET );
        refs[5] = get_blob_string_ref( record, GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET );
        refs[6] = get_blob_string_ref( record, GRAPH_PACKAGE_ROOT_REF_OFFSET );
        if (read_uint32( record + GRAPH_PACKAGE_RESERVED_OFFSET ) ||
            read_uint32( record + GRAPH_PACKAGE_RANK_OFFSET ) != i ||
            (flags & ~APPX_CATALOG_PACKAGE_KNOWN_FLAGS) ||
            !(flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
            !(flags & APPX_CATALOG_PACKAGE_SIGNED) ||
            !is_valid_architecture( architecture ) ||
            !architecture_is_compatible( architecture, target_architecture ) ||
            (i == 0 && (flags & (APPX_CATALOG_PACKAGE_FRAMEWORK |
                                 APPX_CATALOG_PACKAGE_RESOURCE))) ||
            (i != 0 && (!(flags & APPX_CATALOG_PACKAGE_FRAMEWORK) ||
                        (flags & APPX_CATALOG_PACKAGE_RESOURCE))))
            return malformed_graph();
        for (j = 0; j < ARRAY_SIZE(refs); j++)
            if (FAILED(hr = validate_blob_string(
                    data, size, strings_offset, refs[j], j == 2,
                    &expected_string )))
                return hr;
        {
            WCHAR *root;

            if (FAILED(hr = copy_blob_string( data, refs[6], &root )))
                return hr;
            hr = validate_absolute_store_root( root );
            HeapFree( GetProcessHeap(), 0, root );
            if (FAILED(hr)) return hr;
        }
        package_full_names[i] = refs[4];
        if (!i && !blob_aumid_matches( data, refs[5],
                                       application_id, aumid ))
            return malformed_graph();
    }
    if (!blob_strings_are_unique_ci( data, package_full_names,
                                     package_sort_scratch, package_count ))
        return malformed_graph();

    for (i = 0; i < loader_count; i++)
    {
        const BYTE *record = data + loader_offset +
                             i * APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        struct blob_string_ref basename = get_blob_string_ref(
            record, GRAPH_LOADER_BASENAME_REF_OFFSET );
        struct blob_string_ref path = get_blob_string_ref(
            record, GRAPH_LOADER_PATH_REF_OFFSET );
        UINT32 package_index =
            read_uint32( record + GRAPH_LOADER_PACKAGE_INDEX_OFFSET );

        if (package_index >= package_count ||
            read_uint32( record + GRAPH_LOADER_RESERVED_OFFSET ))
            return malformed_graph();
        if (FAILED(hr = validate_blob_string( data, size, strings_offset,
                                              basename, FALSE,
                                              &expected_string )) ||
            FAILED(hr = validate_blob_string( data, size, strings_offset,
                                              path, FALSE,
                                              &expected_string )) ||
            FAILED(hr = validate_blob_path( data, basename, TRUE )) ||
            FAILED(hr = validate_blob_path( data, path, FALSE )))
            return hr;
        if (!blob_string_equals_path_basename( data, basename, path ))
            return malformed_graph();
        if (i)
        {
            const BYTE *previous = data + loader_offset +
                (i - 1) * APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
            UINT32 previous_package = read_uint32(
                previous + GRAPH_LOADER_PACKAGE_INDEX_OFFSET );
            INT result = compare_blob_strings( data, previous_basename,
                                               basename, TRUE );

            if (result > 0 ||
                (!result && previous_package > package_index) ||
                (!result && previous_package == package_index))
                return malformed_graph();
        }
        previous_basename = basename;
    }
    if (expected_string != size) return malformed_graph();
    return S_OK;
}

static HRESULT allocate_graph_from_blob( const void *data, UINT32 size,
                                         APPX_PACKAGE_GRAPH **graph )
{
    APPX_PACKAGE_GRAPH *object;
    SIZE_T allocation_size;

    allocation_size = offsetof( APPX_PACKAGE_GRAPH, data ) + size;
    if (!(object = HeapAlloc( GetProcessHeap(), 0, allocation_size )))
        return E_OUTOFMEMORY;
    object->size = size;
    memcpy( object->data, data, size );
    *graph = object;
    return S_OK;
}

HRESULT WINAPI appx_package_graph_from_blob(
    const void *data, SIZE_T size, APPX_PACKAGE_GRAPH **graph )
{
    HRESULT hr;

    if (!graph) return E_INVALIDARG;
    *graph = NULL;
    if (FAILED(hr = appx_package_graph_validate_blob( data, size ))) return hr;
    return allocate_graph_from_blob( data, size, graph );
}

HRESULT WINAPI appx_package_graph_clone(
    const APPX_PACKAGE_GRAPH *source, APPX_PACKAGE_GRAPH **graph )
{
    if (!graph) return E_INVALIDARG;
    *graph = NULL;
    if (!source) return E_INVALIDARG;
    return appx_package_graph_from_blob( source->data, source->size, graph );
}

HRESULT WINAPI appx_package_graph_create(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *store_root,
    const WCHAR *package_full_name, const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_file_count,
    APPX_PACKAGE_GRAPH **graph )
{
    struct selected_package selected[APPX_GRAPH_MAX_PACKAGES];
    const struct appx_catalog_application *application;
    struct loader_build *loaders = NULL;
    struct graph_edge *edges = NULL;
    BYTE *blob = NULL;
    UINT32 selected_count, edge_count, blob_size, chars;
    HRESULT hr;

    TRACE( "snapshot %p, store root %s, package %s, application %s, "
           "architecture %u, revision %s, loader_files %p, count %u, graph %p.\n",
           snapshot, debugstr_w(store_root), debugstr_w(package_full_name),
           debugstr_w(application_id), target_architecture,
           wine_dbgstr_longlong(revision), loader_files, loader_file_count, graph );

    if (!graph) return E_INVALIDARG;
    *graph = NULL;
    if (!snapshot || !store_root || !package_full_name || !application_id ||
        !is_valid_architecture( target_architecture ) ||
        loader_file_count > APPX_GRAPH_MAX_LOADER_FILES ||
        (loader_file_count && !loader_files))
        return E_INVALIDARG;
    if (FAILED(hr = validate_absolute_store_root( store_root )) ||
        FAILED(hr = bounded_string_length( package_full_name, FALSE, &chars )) ||
        FAILED(hr = bounded_string_length( application_id, FALSE, &chars )))
        return hr;

    if (FAILED(hr = build_selection(
            snapshot, package_full_name, application_id, target_architecture,
            selected, &selected_count, &application, &edges, &edge_count )))
        goto done;
    if (FAILED(hr = build_loader_index(
            selected, selected_count, loader_files, loader_file_count,
            &loaders )))
        goto done;
    if (FAILED(hr = serialize_graph(
            snapshot, store_root, selected, selected_count, application,
            target_architecture, revision, loaders, loader_file_count,
            &blob, &blob_size )))
        goto done;
    if (FAILED(hr = appx_package_graph_validate_blob( blob, blob_size )))
        goto done;
    hr = allocate_graph_from_blob( blob, blob_size, graph );

done:
    HeapFree( GetProcessHeap(), 0, blob );
    HeapFree( GetProcessHeap(), 0, loaders );
    HeapFree( GetProcessHeap(), 0, edges );
    return hr;
}

void WINAPI appx_package_graph_free( APPX_PACKAGE_GRAPH *graph )
{
    if (!graph) return;
    memset( graph, 0, offsetof(APPX_PACKAGE_GRAPH, data) + graph->size );
    HeapFree( GetProcessHeap(), 0, graph );
}

const BYTE * WINAPI appx_package_graph_get_blob(
    const APPX_PACKAGE_GRAPH *graph, UINT32 *size )
{
    if (size) *size = graph ? graph->size : 0;
    return graph ? graph->data : NULL;
}

UINT64 WINAPI appx_package_graph_get_epoch( const APPX_PACKAGE_GRAPH *graph )
{
    return graph ? read_uint64( graph->data + GRAPH_HEADER_EPOCH_OFFSET ) : 0;
}

UINT64 WINAPI appx_package_graph_get_revision( const APPX_PACKAGE_GRAPH *graph )
{
    return graph ? read_uint64( graph->data + GRAPH_HEADER_REVISION_OFFSET ) : 0;
}

enum appx_catalog_architecture WINAPI
appx_package_graph_get_target_architecture( const APPX_PACKAGE_GRAPH *graph )
{
    if (!graph) return APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    return read_uint32( graph->data +
                        GRAPH_HEADER_TARGET_ARCHITECTURE_OFFSET );
}

UINT32 WINAPI appx_package_graph_get_package_count(
    const APPX_PACKAGE_GRAPH *graph )
{
    if (!graph) return 0;
    return read_uint32( graph->data + GRAPH_HEADER_PACKAGE_COUNT_OFFSET );
}

static const WCHAR *graph_string( const BYTE *record, UINT32 ref_offset,
                                  const BYTE *data )
{
    return (const WCHAR *)(data + read_uint32( record + ref_offset ));
}

HRESULT WINAPI appx_package_graph_get_package(
    const APPX_PACKAGE_GRAPH *graph, UINT32 index,
    struct appx_graph_package *package )
{
    const BYTE *record;
    UINT32 count;

    if (!graph || !package) return E_INVALIDARG;
    count = read_uint32( graph->data + GRAPH_HEADER_PACKAGE_COUNT_OFFSET );
    if (index >= count) return E_INVALIDARG;
    record = graph->data + APPX_GRAPH_BLOB_HEADER_SIZE +
             index * APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    memset( package, 0, sizeof(*package) );
    package->version = unpack_version(
        read_uint64( record + GRAPH_PACKAGE_VERSION_OFFSET ) );
    package->architecture =
        read_uint32( record + GRAPH_PACKAGE_ARCHITECTURE_OFFSET );
    package->flags = read_uint32( record + GRAPH_PACKAGE_FLAGS_OFFSET );
    package->rank = read_uint32( record + GRAPH_PACKAGE_RANK_OFFSET );
    memcpy( package->content_id, record + GRAPH_PACKAGE_CONTENT_ID_OFFSET,
            APPX_CATALOG_CONTENT_ID_SIZE );
    package->name = graph_string( record, GRAPH_PACKAGE_NAME_REF_OFFSET,
                                  graph->data );
    package->publisher = graph_string(
        record, GRAPH_PACKAGE_PUBLISHER_REF_OFFSET, graph->data );
    package->resource_id = graph_string(
        record, GRAPH_PACKAGE_RESOURCE_ID_REF_OFFSET, graph->data );
    package->publisher_id = graph_string(
        record, GRAPH_PACKAGE_PUBLISHER_ID_REF_OFFSET, graph->data );
    package->full_name = graph_string(
        record, GRAPH_PACKAGE_FULL_NAME_REF_OFFSET, graph->data );
    package->family_name = graph_string(
        record, GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET, graph->data );
    package->root = graph_string( record, GRAPH_PACKAGE_ROOT_REF_OFFSET,
                                  graph->data );
    return S_OK;
}

HRESULT WINAPI appx_package_graph_get_application(
    const APPX_PACKAGE_GRAPH *graph, struct appx_graph_application *application )
{
    if (!graph || !application) return E_INVALIDARG;
    application->id = graph_string(
        graph->data, GRAPH_HEADER_APPLICATION_ID_REF_OFFSET, graph->data );
    application->aumid = graph_string(
        graph->data, GRAPH_HEADER_AUMID_REF_OFFSET, graph->data );
    application->executable = graph_string(
        graph->data, GRAPH_HEADER_EXECUTABLE_REF_OFFSET, graph->data );
    application->entry_point = graph_string(
        graph->data, GRAPH_HEADER_ENTRY_POINT_REF_OFFSET, graph->data );
    application->activation_kind = read_uint32(
        graph->data + GRAPH_HEADER_ACTIVATION_KIND_OFFSET );
    return S_OK;
}

static INT compare_input_blob_string_ci( const WCHAR *input, const BYTE *data,
                                         struct blob_string_ref ref )
{
    UINT32 i, input_length = lstrlenW( input ), ref_length = ref.chars - 1;
    UINT32 length = input_length < ref_length ? input_length : ref_length;

    for (i = 0; i < length; i++)
    {
        WCHAR left = RtlUpcaseUnicodeChar( input[i] );
        WCHAR right = RtlUpcaseUnicodeChar(
            read_uint16( data + ref.offset + i * sizeof(WCHAR) ) );

        if (left != right) return left < right ? -1 : 1;
    }
    if (input_length == ref_length) return 0;
    return input_length < ref_length ? -1 : 1;
}

HRESULT WINAPI appx_package_graph_lookup_basename(
    const APPX_PACKAGE_GRAPH *graph, const WCHAR *basename,
    struct appx_graph_loader_match *match )
{
    const BYTE *data, *record, *package_record;
    struct blob_string_ref ref;
    UINT32 loader_count, loader_offset, package_offset, low = 0, high, middle;
    UINT32 package_index, chars;
    HRESULT hr;

    if (!graph || !basename || !match) return E_INVALIDARG;
    if (FAILED(hr = bounded_string_length( basename, FALSE, &chars )) ||
        FAILED(hr = validate_relative_path( basename, TRUE )))
        return E_INVALIDARG;
    data = graph->data;
    loader_count = read_uint32( data + GRAPH_HEADER_LOADER_COUNT_OFFSET );
    loader_offset = read_uint32( data + GRAPH_HEADER_LOADERS_OFFSET );
    high = loader_count;
    while (low < high)
    {
        middle = low + (high - low) / 2;
        record = data + loader_offset +
                 middle * APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        ref = get_blob_string_ref( record, GRAPH_LOADER_BASENAME_REF_OFFSET );
        if (compare_input_blob_string_ci( basename, data, ref ) > 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (low >= loader_count)
        return HRESULT_FROM_WIN32( ERROR_MOD_NOT_FOUND );
    record = data + loader_offset + low * APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
    ref = get_blob_string_ref( record, GRAPH_LOADER_BASENAME_REF_OFFSET );
    if (compare_input_blob_string_ci( basename, data, ref ))
        return HRESULT_FROM_WIN32( ERROR_MOD_NOT_FOUND );

    package_index = read_uint32(
        record + GRAPH_LOADER_PACKAGE_INDEX_OFFSET );
    package_offset = read_uint32( data + GRAPH_HEADER_PACKAGES_OFFSET );
    package_record = data + package_offset +
        package_index * APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    match->package_index = package_index;
    match->package_root = graph_string(
        package_record, GRAPH_PACKAGE_ROOT_REF_OFFSET, data );
    match->relative_path = graph_string(
        record, GRAPH_LOADER_PATH_REF_OFFSET, data );
    return S_OK;
}
