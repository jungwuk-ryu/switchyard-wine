/*
 * Module loader
 *
 * Copyright 1993 Robert J. Amstadt
 * Copyright 2006 Mike McCormack
 * Copyright 1995, 2003, 2019 Alexandre Julliard
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

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winternl.h"
#include "ddk/ntddk.h"
#include "kernelbase.h"
#include "wine/appx_package_graph.h"
#include "wine/list.h"
#include "wine/asm.h"
#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);


/* to keep track of LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE file handles */
struct exclusive_datafile
{
    struct list entry;
    HMODULE     module;
    HANDLE      file;
};
static struct list exclusive_datafile_list = LIST_INIT( exclusive_datafile_list );

static CRITICAL_SECTION exclusive_datafile_list_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &exclusive_datafile_list_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": exclusive_datafile_list_section") }
};
static CRITICAL_SECTION exclusive_datafile_list_section = { &critsect_debug, -1, 0, 0, 0, 0 };

/***********************************************************************
 * Modules
 ***********************************************************************/


/******************************************************************
 *      get_proc_address
 */
FARPROC WINAPI get_proc_address( HMODULE module, LPCSTR function )
{
    FARPROC proc;
    ANSI_STRING str;

    if (!module) module = NtCurrentTeb()->Peb->ImageBaseAddress;

    if ((ULONG_PTR)function >> 16)
    {
        RtlInitAnsiString( &str, function );
        if (!set_ntstatus( LdrGetProcedureAddress( module, &str, 0, (void**)&proc ))) return NULL;
    }
    else if (!set_ntstatus( LdrGetProcedureAddress( module, NULL, LOWORD(function), (void**)&proc )))
        return NULL;

    return proc;
}


/******************************************************************
 *      load_library_as_datafile
 */
static BOOL load_library_as_datafile( LPCWSTR load_path, DWORD flags, LPCWSTR name, HMODULE *mod_ret )
{
    WCHAR filenameW[MAX_PATH];
    HANDLE mapping, file = INVALID_HANDLE_VALUE;
    HMODULE module = 0;
    DWORD protect = PAGE_READONLY;

    *mod_ret = 0;

    if (flags & LOAD_LIBRARY_AS_IMAGE_RESOURCE) protect |= SEC_IMAGE;

    if (SearchPathW( NULL, name, L".dll", ARRAY_SIZE( filenameW ), filenameW, NULL ))
    {
        file = CreateFileW( filenameW, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, 0, 0 );
    }
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    mapping = CreateFileMappingW( file, NULL, protect, 0, 0, NULL );
    if (!mapping) goto failed;

    module = MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, 0 );
    CloseHandle( mapping );
    if (!module) goto failed;

    if (!(flags & LOAD_LIBRARY_AS_IMAGE_RESOURCE))
    {
        /* make sure it's a valid PE file */
        if (!RtlImageNtHeader( module ))
        {
            SetLastError( ERROR_BAD_EXE_FORMAT );
            goto failed;
        }
        *mod_ret = (HMODULE)((char *)module + 1); /* set bit 0 for data file module */

        if (flags & LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)
        {
            struct exclusive_datafile *datafile = HeapAlloc( GetProcessHeap(), 0, sizeof(*datafile) );
            if (!datafile) goto failed;
            datafile->module = *mod_ret;
            datafile->file   = file;
            RtlEnterCriticalSection( &exclusive_datafile_list_section );
            list_add_head( &exclusive_datafile_list, &datafile->entry );
            RtlLeaveCriticalSection( &exclusive_datafile_list_section );
            TRACE( "delaying close %p for module %p\n", datafile->file, datafile->module );
            return TRUE;
        }
    }
    else *mod_ret = (HMODULE)((char *)module + 2); /* set bit 1 for image resource module */

    CloseHandle( file );
    return TRUE;

failed:
    if (module) UnmapViewOfFile( module );
    CloseHandle( file );
    return FALSE;
}


/******************************************************************
 *      load_library
 */
static HMODULE load_library( const UNICODE_STRING *libname, DWORD flags )
{
    const DWORD unsupported_flags = LOAD_IGNORE_CODE_AUTHZ_LEVEL | LOAD_LIBRARY_REQUIRE_SIGNED_TARGET;
    const ULONG load_library_search_flags = LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS
                | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    NTSTATUS status;
    HMODULE module;
    WCHAR *load_path, *dummy;
    DWORD load_flags = 0, search_flags;

    if (flags & unsupported_flags) FIXME( "unsupported flag(s) used %#08lx\n", flags );

    if (flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                 LOAD_LIBRARY_AS_IMAGE_RESOURCE))
    {
        if (!set_ntstatus( LdrGetDllPath( libname->Buffer, flags, &load_path, &dummy ))) return 0;
        if (LdrGetDllHandleEx( 0, load_path, NULL, libname, &module ))
            load_library_as_datafile( load_path, flags, libname->Buffer, &module );
        RtlReleasePath( load_path );
    }
    else
    {
        search_flags = flags & load_library_search_flags;
        if (flags & DONT_RESOLVE_DLL_REFERENCES) load_flags |= LDR_DONT_RESOLVE_REFS;
        status = LdrLoadDll( (void *)((ULONG_PTR)search_flags | 1), &load_flags, libname, &module );
        if (!set_ntstatus( status ))
        {
            module = 0;
            if (status == STATUS_DLL_NOT_FOUND && (GetVersion() & 0x80000000))
                SetLastError( ERROR_DLL_NOT_FOUND );
        }
    }
    return module;
}


/****************************************************************************
 *	AddDllDirectory   (kernelbase.@)
 */
DLL_DIRECTORY_COOKIE WINAPI DECLSPEC_HOTPATCH AddDllDirectory( const WCHAR *dir )
{
    UNICODE_STRING str;
    void *cookie;

    RtlInitUnicodeString( &str, dir );
    if (!set_ntstatus( LdrAddDllDirectory( &str, &cookie ))) return NULL;
    return cookie;
}


/***********************************************************************
 *	DelayLoadFailureHook   (kernelbase.@)
 */
FARPROC WINAPI DECLSPEC_HOTPATCH DelayLoadFailureHook( LPCSTR name, LPCSTR function )
{
    ULONG_PTR args[2];

    if ((ULONG_PTR)function >> 16)
        ERR( "failed to delay load %s.%s\n", name, function );
    else
        ERR( "failed to delay load %s.%u\n", name, LOWORD(function) );
    args[0] = (ULONG_PTR)name;
    args[1] = (ULONG_PTR)function;
    RaiseException( EXCEPTION_WINE_STUB, EXCEPTION_NONCONTINUABLE, 2, args );
    return NULL;
}


/****************************************************************************
 *	DisableThreadLibraryCalls   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH DisableThreadLibraryCalls( HMODULE module )
{
    return set_ntstatus( LdrDisableThreadCalloutsForDll( module ));
}


/***********************************************************************
 *	FreeLibrary   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FreeLibrary( HINSTANCE module )
{
    if (!module)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }

    if ((ULONG_PTR)module & 3) /* this is a datafile module */
    {
        void *ptr = (void *)((ULONG_PTR)module & ~3);
        if (!RtlImageNtHeader( ptr ))
        {
            SetLastError( ERROR_BAD_EXE_FORMAT );
            return FALSE;
        }
        if ((ULONG_PTR)module & 1)
        {
            struct exclusive_datafile *file;

            RtlEnterCriticalSection( &exclusive_datafile_list_section );
            LIST_FOR_EACH_ENTRY( file, &exclusive_datafile_list, struct exclusive_datafile, entry )
            {
                if (file->module != module) continue;
                TRACE( "closing %p for module %p\n", file->file, file->module );
                CloseHandle( file->file );
                list_remove( &file->entry );
                HeapFree( GetProcessHeap(), 0, file );
                break;
            }
            RtlLeaveCriticalSection( &exclusive_datafile_list_section );
        }
        return UnmapViewOfFile( ptr );
    }

    return set_ntstatus( LdrUnloadDll( module ));
}


/***********************************************************************
 *	GetModuleFileNameA   (kernelbase.@)
 */
DWORD WINAPI DECLSPEC_HOTPATCH GetModuleFileNameA( HMODULE module, LPSTR filename, DWORD size )
{
    LPWSTR filenameW = HeapAlloc( GetProcessHeap(), 0, size * sizeof(WCHAR) );
    DWORD len;

    if (!filenameW)
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return 0;
    }
    if ((len = GetModuleFileNameW( module, filenameW, size )))
    {
    	len = file_name_WtoA( filenameW, len, filename, size );
        if (len < size)
            filename[len] = 0;
        else
            SetLastError( ERROR_INSUFFICIENT_BUFFER );
    }
    HeapFree( GetProcessHeap(), 0, filenameW );
    return len;
}


/***********************************************************************
 *	GetModuleFileNameW   (kernelbase.@)
 */
DWORD WINAPI DECLSPEC_HOTPATCH GetModuleFileNameW( HMODULE module, LPWSTR filename, DWORD size )
{
    ULONG len = 0;
    WIN16_SUBSYSTEM_TIB *win16_tib;
    UNICODE_STRING name;
    NTSTATUS status;

    if (!module && ((win16_tib = NtCurrentTeb()->Tib.SubSystemTib)) && win16_tib->exe_name)
    {
        len = min( size, win16_tib->exe_name->Length / sizeof(WCHAR) );
        memcpy( filename, win16_tib->exe_name->Buffer, len * sizeof(WCHAR) );
        if (len < size) filename[len] = 0;
        goto done;
    }

    name.Buffer = filename;
    name.MaximumLength = min( size, UNICODE_STRING_MAX_CHARS ) * sizeof(WCHAR);
    status = LdrGetDllFullName( module, &name );
    if (!status || status == STATUS_BUFFER_TOO_SMALL)
    {
        len = name.Length / sizeof(WCHAR);
        /* LdrGetDllFullName calls RtlCopyUnicodeString which should terminate
           if there's space, otherwise: */
        if (status == STATUS_BUFFER_TOO_SMALL && size > 0)
            filename[size - 1] = 0;
    }
    SetLastError( RtlNtStatusToDosError( status ));
done:
    TRACE( "%s\n", debugstr_wn(filename, len) );
    return len;
}


/***********************************************************************
 *	GetModuleHandleA   (kernelbase.@)
 */
HMODULE WINAPI DECLSPEC_HOTPATCH GetModuleHandleA( LPCSTR module )
{
    HMODULE ret;

    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, module, &ret );
    return ret;
}


/***********************************************************************
 *	GetModuleHandleW   (kernelbase.@)
 */
HMODULE WINAPI DECLSPEC_HOTPATCH GetModuleHandleW( LPCWSTR module )
{
    HMODULE ret;

    GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, module, &ret );
    return ret;
}


/***********************************************************************
 *	GetModuleHandleExA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetModuleHandleExA( DWORD flags, LPCSTR name, HMODULE *module )
{
    WCHAR *nameW;

    if (!module)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    if (!name || (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
        return GetModuleHandleExW( flags, (LPCWSTR)name, module );

    if (!(nameW = file_name_AtoW( name, FALSE )))
    {
        *module = NULL;
        SetLastError( ERROR_MOD_NOT_FOUND );
        return FALSE;
    }
    return GetModuleHandleExW( flags, nameW, module );
}


/***********************************************************************
 *	GetModuleHandleExW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetModuleHandleExW( DWORD flags, LPCWSTR name, HMODULE *module )
{
    HMODULE ret = NULL;
    NTSTATUS status;
    void *dummy;

    if (!module)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    if ((flags & ~(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
                  | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
                  || (flags & (GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
                  == (GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
    {
        *module = NULL;
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    if (name && !(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
    {
        UNICODE_STRING wstr;
        ULONG ldr_flags = 0;

        if (flags & GET_MODULE_HANDLE_EX_FLAG_PIN)
            ldr_flags |= LDR_GET_DLL_HANDLE_EX_FLAG_PIN;
        if (flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)
            ldr_flags |= LDR_GET_DLL_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;

        RtlInitUnicodeString( &wstr, name );
        status = LdrGetDllHandleEx( ldr_flags, NULL, NULL, &wstr, &ret );
    }
    else
    {
        ret = name ? RtlPcToFileHeader( (void *)name, &dummy ) : NtCurrentTeb()->Peb->ImageBaseAddress;

        if (ret)
        {
            if (!(flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
                status = LdrAddRefDll( flags & GET_MODULE_HANDLE_EX_FLAG_PIN ? LDR_ADDREF_DLL_PIN : 0, ret );
            else
                status = STATUS_SUCCESS;
        } else status = STATUS_DLL_NOT_FOUND;
    }

    *module = ret;
    return set_ntstatus( status );
}


/***********************************************************************
 *	GetProcAddress   (kernelbase.@)
 */

/*
 * Work around a Delphi bug on x86_64.  When delay loading a symbol,
 * Delphi saves rcx, rdx, r8 and r9 to the stack.  It then calls
 * GetProcAddress(), pops the saved registers and calls the function.
 * This works fine if all of the parameters are ints.  However, since
 * it does not save xmm0 - 3, it relies on GetProcAddress() preserving
 * these registers if the function takes floating point parameters.
 * This wrapper saves xmm0 - 3 to the stack.
 */
#ifdef __arm64ec__
FARPROC WINAPI __attribute__((naked)) GetProcAddress( HMODULE module, LPCSTR function )
{
    asm( ".seh_proc \"#GetProcAddress\"\n\t"
         "stp x29, x30, [sp, #-48]!\n\t"
         ".seh_save_fplr_x 48\n\t"
         ".seh_endprologue\n\t"
         "stp d0, d1, [sp, #16]\n\t"
         "stp d2, d3, [sp, #32]\n\t"
         "bl \"#get_proc_address\"\n\t"
         "ldp d0, d1, [sp, #16]\n\t"
         "ldp d2, d3, [sp, #32]\n\t"
         "ldp x29, x30, [sp], #48\n\t"
         "ret\n\t"
         ".seh_endproc" );
}
#elif defined(__x86_64__)
__ASM_GLOBAL_FUNC( GetProcAddress,
                   ".byte 0x48\n\t"  /* hotpatch prolog */
                   "pushq %rbp\n\t"
                   __ASM_SEH(".seh_pushreg %rbp\n\t")
                   __ASM_CFI(".cfi_adjust_cfa_offset 8\n\t")
                   __ASM_CFI(".cfi_rel_offset %rbp,0\n\t")
                   "movq %rsp,%rbp\n\t"
                   __ASM_SEH(".seh_setframe %rbp,0\n\t")
                   __ASM_CFI(".cfi_def_cfa_register %rbp\n\t")
                   __ASM_SEH(".seh_endprologue\n\t")
                   "subq $0x60,%rsp\n\t"
                   "andq $~15,%rsp\n\t"
                   "movaps %xmm0,0x20(%rsp)\n\t"
                   "movaps %xmm1,0x30(%rsp)\n\t"
                   "movaps %xmm2,0x40(%rsp)\n\t"
                   "movaps %xmm3,0x50(%rsp)\n\t"
                   "call " __ASM_NAME("get_proc_address") "\n\t"
                   "movaps 0x50(%rsp), %xmm3\n\t"
                   "movaps 0x40(%rsp), %xmm2\n\t"
                   "movaps 0x30(%rsp), %xmm1\n\t"
                   "movaps 0x20(%rsp), %xmm0\n\t"
                   "leaq 0(%rbp),%rsp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register %rsp\n\t")
                   "popq %rbp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset -8\n\t")
                   __ASM_CFI(".cfi_same_value %rbp\n\t")
                   "ret" )
#else /* __x86_64__ */

FARPROC WINAPI DECLSPEC_HOTPATCH GetProcAddress( HMODULE module, LPCSTR function )
{
    return get_proc_address( module, function );
}

#endif /* __x86_64__ */


/***********************************************************************
 *	IsApiSetImplemented   (kernelbase.@)
 */
BOOL WINAPI IsApiSetImplemented( LPCSTR name )
{
    UNICODE_STRING str;
    NTSTATUS status;
    BOOLEAN in_schema, present;

    if (!RtlCreateUnicodeStringFromAsciiz( &str, name )) return FALSE;
    status = ApiSetQueryApiSetPresenceEx( &str, &in_schema, &present );
    RtlFreeUnicodeString( &str );
    return !status && present;
}


/***********************************************************************
 *	LoadLibraryA   (kernelbase.@)
 */
HMODULE WINAPI DECLSPEC_HOTPATCH LoadLibraryA( LPCSTR name )
{
    return LoadLibraryExA( name, 0, 0 );
}


/***********************************************************************
 *	LoadLibraryW   (kernelbase.@)
 */
HMODULE WINAPI DECLSPEC_HOTPATCH LoadLibraryW( LPCWSTR name )
{
    return LoadLibraryExW( name, 0, 0 );
}


/******************************************************************
 *	LoadLibraryExA   (kernelbase.@)
 */
HMODULE WINAPI DECLSPEC_HOTPATCH LoadLibraryExA( LPCSTR name, HANDLE file, DWORD flags )
{
    WCHAR *nameW;
    HMODULE module;

    /* A new allocation is necessary due to TP Shell Service
     * calling LoadLibraryExA from an LdrLoadDll hook */
    if (!(nameW = file_name_AtoW( name, TRUE ))) return 0;

    module = LoadLibraryExW( nameW, file, flags );

    HeapFree( GetProcessHeap(), 0, nameW );
    return module;
}


/***********************************************************************
 *	LoadLibraryExW   (kernelbase.@)
 */
HMODULE WINAPI DECLSPEC_HOTPATCH LoadLibraryExW( LPCWSTR name, HANDLE file, DWORD flags )
{
    UNICODE_STRING str;
    HMODULE module;

    if (!name)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return 0;
    }
    RtlInitUnicodeString( &str, name );
    if (str.Length && str.Buffer[str.Length/sizeof(WCHAR) - 1] != ' ') return load_library( &str, flags );

    /* library name has trailing spaces */
    RtlCreateUnicodeString( &str, name );
    while (str.Length > sizeof(WCHAR) && str.Buffer[str.Length/sizeof(WCHAR) - 1] == ' ')
        str.Length -= sizeof(WCHAR);

    str.Buffer[str.Length/sizeof(WCHAR)] = 0;
    module = load_library( &str, flags );
    RtlFreeUnicodeString( &str );
    return module;
}

enum appx_loader_wire_offset
{
    APPX_HEADER_PACKAGE_COUNT_OFFSET       = 44,
    APPX_HEADER_PACKAGES_OFFSET            = 48,
    APPX_HEADER_LOADER_COUNT_OFFSET        = 52,
    APPX_HEADER_LOADERS_OFFSET             = 56,

    APPX_PACKAGE_ROOT_REF_OFFSET           = 104,

    APPX_LOADER_PACKAGE_INDEX_OFFSET       = 0,
    APPX_LOADER_BASENAME_REF_OFFSET        = 8,
    APPX_LOADER_PATH_REF_OFFSET            = 16,
};

struct appx_loader_graph
{
    const BYTE *data;
    UINT32 size;
    UINT32 package_count;
    UINT32 packages_offset;
    UINT32 loader_count;
    UINT32 loaders_offset;
};

struct appx_loader_graph_cache
{
    const BYTE *data;
    struct appx_loader_graph graph;
};

static struct appx_loader_graph_cache *appx_loader_graph_cache;

static BOOL appx_validate_loader_graph_domain( const struct appx_loader_graph *graph );

static BOOL get_appx_loader_graph( const BYTE *data, struct appx_loader_graph *graph )
{
    struct appx_loader_graph_cache *cache, *new_cache;
    BOOL valid = FALSE;
    UINT32 size;

    cache = InterlockedCompareExchangePointer( (void **)&appx_loader_graph_cache,
                                               NULL, NULL );
    if (cache && cache->data == data)
    {
        *graph = cache->graph;
        return TRUE;
    }

    __TRY
    {
        size = wine_appx_graph_read_u32( data + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET );
        if (wine_appx_graph_validate_blob( data, size ))
        {
            graph->data = data;
            graph->size = size;
            graph->package_count = wine_appx_graph_read_u32(
                data + APPX_HEADER_PACKAGE_COUNT_OFFSET );
            graph->packages_offset = wine_appx_graph_read_u32(
                data + APPX_HEADER_PACKAGES_OFFSET );
            graph->loader_count = wine_appx_graph_read_u32(
                data + APPX_HEADER_LOADER_COUNT_OFFSET );
            graph->loaders_offset = wine_appx_graph_read_u32(
                data + APPX_HEADER_LOADERS_OFFSET );
            if (appx_validate_loader_graph_domain( graph ))
            {
                if ((new_cache = HeapAlloc( GetProcessHeap(), 0,
                                            sizeof(*new_cache) )))
                {
                    new_cache->data = data;
                    new_cache->graph = *graph;
                    if (InterlockedCompareExchangePointer(
                            (void **)&appx_loader_graph_cache, new_cache, NULL ))
                        HeapFree( GetProcessHeap(), 0, new_cache );
                }
                valid = TRUE;
            }
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        valid = FALSE;
    }
    __ENDTRY
    return valid;
}

static BOOL appx_path_component_is_reserved( const WCHAR *component, UINT32 length )
{
    WCHAR name[5];
    UINT32 i, base_length = 0;

    while (base_length < length && component[base_length] != '.') base_length++;
    if (!base_length || base_length > 4) return FALSE;
    for (i = 0; i < base_length; i++) name[i] = RtlUpcaseUnicodeChar( component[i] );
    name[base_length] = 0;
    if ((base_length == 3 && (!wcscmp( name, L"CON" ) ||
                              !wcscmp( name, L"PRN" ) ||
                              !wcscmp( name, L"AUX" ) ||
                              !wcscmp( name, L"NUL" ))) ||
        (base_length == 4 && (!wcsncmp( name, L"COM", 3 ) ||
                              !wcsncmp( name, L"LPT", 3 )) &&
         name[3] >= '1' && name[3] <= '9'))
        return TRUE;
    return FALSE;
}

static BOOL appx_validate_path_components( const WCHAR *path, UINT32 start,
                                           UINT32 length )
{
    UINT32 component = start, i;

    if (start == length) return TRUE;
    for (i = start; i <= length; i++)
    {
        WCHAR ch = i == length ? '\\' : path[i];

        if (ch == '/' || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
            ch == '|' || ch == '?' || ch == '*' || ch < 0x20)
            return FALSE;
        if (ch != '\\') continue;
        if (i == component ||
            (i - component == 1 && path[component] == '.') ||
            (i - component == 2 && path[component] == '.' &&
             path[component + 1] == '.') ||
            path[i - 1] == ' ' || path[i - 1] == '.' ||
            i - component > 255 ||
            appx_path_component_is_reserved( path + component, i - component ))
            return FALSE;
        component = i + 1;
    }
    return TRUE;
}

static BOOL appx_validate_package_root( const WCHAR *root, UINT32 chars )
{
    UINT32 length;

    if (chars < 4 || chars > UNICODE_STRING_MAX_CHARS) return FALSE;
    length = chars - 1;
    if (!((root[0] >= 'A' && root[0] <= 'Z') ||
          (root[0] >= 'a' && root[0] <= 'z')) ||
        root[1] != ':' || root[2] != '\\')
        return FALSE;
    if (length > 3 && root[length - 1] == '\\') return FALSE;
    return appx_validate_path_components( root, 3, length );
}

static BOOL appx_validate_relative_path( const WCHAR *path, UINT32 chars )
{
    if (chars < 2 || chars > UNICODE_STRING_MAX_CHARS || path[0] == '\\')
        return FALSE;
    return appx_validate_path_components( path, 0, chars - 1 );
}

static INT appx_compare_string_ci( const WCHAR *left, UINT32 left_length,
                                   const BYTE *data,
                                   struct wine_appx_graph_string_ref right )
{
    UINT32 right_length = right.chars - 1, length;
    UINT32 i;

    length = left_length < right_length ? left_length : right_length;
    for (i = 0; i < length; i++)
    {
        WCHAR left_char = RtlUpcaseUnicodeChar( left[i] );
        WCHAR right_char = RtlUpcaseUnicodeChar(
            wine_appx_graph_read_u16( data + right.offset + i * sizeof(WCHAR) ) );

        if (left_char != right_char) return left_char < right_char ? -1 : 1;
    }
    if (left_length == right_length) return 0;
    return left_length < right_length ? -1 : 1;
}

static INT appx_compare_graph_refs_ci(
    const BYTE *data, struct wine_appx_graph_string_ref left,
    struct wine_appx_graph_string_ref right )
{
    return appx_compare_string_ci( (const WCHAR *)(data + left.offset),
                                   left.chars - 1, data, right );
}

static BOOL appx_validate_loader_graph_domain( const struct appx_loader_graph *graph )
{
    struct wine_appx_graph_string_ref previous_basename = {0};
    struct wine_appx_graph_string_ref previous_path = {0};
    UINT32 previous_package = 0, previous_rank = 0, i;

    for (i = 0; i < graph->package_count; i++)
    {
        const BYTE *record = graph->data + graph->packages_offset +
                             i * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
        struct wine_appx_graph_string_ref root = wine_appx_graph_get_ref(
            record, APPX_PACKAGE_ROOT_REF_OFFSET );

        if (!appx_validate_package_root(
                (const WCHAR *)(graph->data + root.offset), root.chars ))
            return FALSE;
    }

    for (i = 0; i < graph->loader_count; i++)
    {
        const BYTE *record = graph->data + graph->loaders_offset +
                             i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        struct wine_appx_graph_string_ref basename = wine_appx_graph_get_ref(
            record, APPX_LOADER_BASENAME_REF_OFFSET );
        struct wine_appx_graph_string_ref path = wine_appx_graph_get_ref(
            record, APPX_LOADER_PATH_REF_OFFSET );
        const WCHAR *basename_string = (const WCHAR *)(graph->data + basename.offset);
        const WCHAR *path_string = (const WCHAR *)(graph->data + path.offset);
        UINT32 package_index = wine_appx_graph_read_u32(
            record + APPX_LOADER_PACKAGE_INDEX_OFFSET );
        UINT32 search_rank = wine_appx_graph_read_u32(
            record + WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET );
        UINT64 change_time = wine_appx_graph_read_u64(
            record + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET );
        UINT64 file_size = wine_appx_graph_read_u64(
            record + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET );
        UINT32 path_basename = path.chars - 1, j;
        INT comparison;

        if (package_index >= graph->package_count ||
            (search_rank >= WINE_APPX_GRAPH_MAX_LOADER_SEARCH_PATHS &&
             search_rank != WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY) ||
            !change_time || (change_time >> 63) ||
            !file_size || (file_size >> 63) ||
            !appx_validate_relative_path( basename_string, basename.chars ) ||
            !appx_validate_relative_path( path_string, path.chars ))
            return FALSE;
        for (j = 0; j + 1 < basename.chars; j++)
            if (basename_string[j] == '\\') return FALSE;
        while (path_basename && path_string[path_basename - 1] != '\\')
            path_basename--;
        if (path.chars - 1 - path_basename != basename.chars - 1)
            return FALSE;
        for (j = 0; j + 1 < basename.chars; j++)
            if (path_string[path_basename + j] != basename_string[j])
                return FALSE;

        if (i)
        {
            comparison = appx_compare_graph_refs_ci(
                graph->data, previous_basename, basename );
            if (comparison > 0 ||
                (!comparison && previous_package > package_index) ||
                (!comparison && previous_package == package_index &&
                 previous_rank > search_rank) ||
                (!comparison && previous_package == package_index &&
                 previous_rank == search_rank &&
                 appx_compare_graph_refs_ci(
                     graph->data, previous_path, path ) >= 0))
                return FALSE;
        }
        previous_basename = basename;
        previous_path = path;
        previous_package = package_index;
        previous_rank = search_rank;
    }
    return TRUE;
}

static BOOL appx_normalize_module_name( const WCHAR *name, WCHAR **normalized,
                                        UINT32 *length, UINT32 *basename_offset,
                                        BOOL *has_path )
{
    UINT32 input_length, output_length, basename = 0, i;
    BOOL append_extension = TRUE;
    WCHAR *result;

    *normalized = NULL;
    for (input_length = 0; input_length < UNICODE_STRING_MAX_CHARS;
         input_length++)
        if (!name[input_length]) break;
    if (!input_length)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    if (input_length == UNICODE_STRING_MAX_CHARS)
    {
        SetLastError( ERROR_FILENAME_EXCED_RANGE );
        return FALSE;
    }
    for (i = 0; i < input_length; i++)
    {
        if (name[i] == '/')
        {
            SetLastError( ERROR_INVALID_PARAMETER );
            return FALSE;
        }
        if (name[i] == '\\') basename = i + 1;
    }
    if (!basename)
        *has_path = FALSE;
    else
        *has_path = TRUE;
    if (!appx_validate_relative_path( name, input_length + 1 ))
    {
        /*
         * A final dot is a LoadPackagedLibrary sentinel that suppresses the
         * implicit .dll extension.  Validate the path after removing it.
         */
        if (name[input_length - 1] != '.' || input_length == 1 ||
            !appx_validate_relative_path( name, input_length ))
        {
            SetLastError( ERROR_INVALID_PARAMETER );
            return FALSE;
        }
        input_length--;
        append_extension = FALSE;
    }
    else
    {
        for (i = basename; i < input_length; i++)
            if (name[i] == '.')
            {
                append_extension = FALSE;
                break;
            }
    }
    output_length = input_length + (append_extension ? 4 : 0);
    if (output_length >= UNICODE_STRING_MAX_CHARS)
    {
        SetLastError( ERROR_FILENAME_EXCED_RANGE );
        return FALSE;
    }
    if (!(result = HeapAlloc( GetProcessHeap(), 0,
                              (output_length + 1) * sizeof(*result) )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    memcpy( result, name, input_length * sizeof(*result) );
    if (append_extension) memcpy( result + input_length, L".dll", 5 * sizeof(WCHAR) );
    else result[output_length] = 0;
    *normalized = result;
    *length = output_length;
    *basename_offset = basename;
    return TRUE;
}

static const BYTE *appx_find_loader_record( const struct appx_loader_graph *graph,
                                            const WCHAR *name, UINT32 length,
                                            UINT32 basename_offset, BOOL has_path )
{
    struct wine_appx_graph_string_ref basename, relative_path;
    const BYTE *record;
    UINT32 low = 0, high = graph->loader_count, middle;
    INT comparison;

    while (low < high)
    {
        middle = low + (high - low) / 2;
        record = graph->data + graph->loaders_offset +
                 middle * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        basename = wine_appx_graph_get_ref( record, APPX_LOADER_BASENAME_REF_OFFSET );
        comparison = appx_compare_string_ci( name + basename_offset,
                                             length - basename_offset,
                                             graph->data, basename );
        if (comparison > 0) low = middle + 1;
        else high = middle;
    }

    while (low < graph->loader_count)
    {
        record = graph->data + graph->loaders_offset +
                 low * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        basename = wine_appx_graph_get_ref( record, APPX_LOADER_BASENAME_REF_OFFSET );
        if (appx_compare_string_ci( name + basename_offset, length - basename_offset,
                                    graph->data, basename ))
            break;
        relative_path = wine_appx_graph_get_ref( record, APPX_LOADER_PATH_REF_OFFSET );
        if ((!has_path &&
             wine_appx_graph_read_u32(
                 record + WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET ) !=
                 WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY) ||
            (has_path && !appx_compare_string_ci(
                name, length, graph->data, relative_path )))
            return record;
        low++;
    }
    return NULL;
}

static HMODULE load_appx_graph_module( const struct appx_loader_graph *graph,
                                      const BYTE *loader_record,
                                      const WCHAR *normalized_name,
                                      UINT32 normalized_length,
                                      UINT32 basename_offset )
{
    struct wine_appx_graph_string_ref root_ref, basename_ref, path_ref;
    const WCHAR *root, *relative_path;
    const BYTE *package_record;
    UINT32 package_index, root_length, path_length, path_basename, separator;
    UINT32 full_length;
    WCHAR *full_path = NULL, *loaded_path = NULL;
    HMODULE module = NULL;
    DWORD loaded_length, error;

    package_index = wine_appx_graph_read_u32(
        loader_record + APPX_LOADER_PACKAGE_INDEX_OFFSET );
    if (package_index >= graph->package_count)
    {
        SetLastError( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
        return NULL;
    }
    package_record = graph->data + graph->packages_offset +
                     package_index * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    root_ref = wine_appx_graph_get_ref( package_record, APPX_PACKAGE_ROOT_REF_OFFSET );
    basename_ref = wine_appx_graph_get_ref(
        loader_record, APPX_LOADER_BASENAME_REF_OFFSET );
    path_ref = wine_appx_graph_get_ref( loader_record, APPX_LOADER_PATH_REF_OFFSET );
    root = (const WCHAR *)(graph->data + root_ref.offset);
    relative_path = (const WCHAR *)(graph->data + path_ref.offset);
    path_basename = path_ref.chars - 1;
    while (path_basename && relative_path[path_basename - 1] != '\\')
        path_basename--;
    if (!appx_validate_package_root( root, root_ref.chars ) ||
        !appx_validate_relative_path( relative_path, path_ref.chars ) ||
        appx_compare_string_ci( normalized_name + basename_offset,
                                normalized_length - basename_offset,
                                graph->data, basename_ref ) ||
        appx_compare_string_ci( relative_path + path_basename,
                                path_ref.chars - 1 - path_basename,
                                graph->data, basename_ref ))
    {
        SetLastError( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
        return NULL;
    }

    path_length = path_ref.chars - 1;
    root_length = root_ref.chars - 1;
    separator = root[root_length - 1] != '\\';
    if (root_length >= UNICODE_STRING_MAX_CHARS - path_length - separator)
    {
        SetLastError( ERROR_FILENAME_EXCED_RANGE );
        return NULL;
    }
    full_length = root_length + separator + path_length;
    if (!(full_path = HeapAlloc( GetProcessHeap(), 0,
                                  (full_length + 1) * sizeof(*full_path) )) ||
        !(loaded_path = HeapAlloc( GetProcessHeap(), 0,
                                    (full_length + 2) * sizeof(*loaded_path) )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        goto done;
    }
    memcpy( full_path, root, root_length * sizeof(*full_path) );
    if (separator) full_path[root_length] = '\\';
    memcpy( full_path + root_length + separator, relative_path,
            (path_length + 1) * sizeof(*full_path) );

    if (!(module = LoadLibraryExW( full_path, 0, 0 ))) goto done;
    loaded_length = GetModuleFileNameW( module, loaded_path, full_length + 2 );
    if (loaded_length != full_length ||
        wcsnicmp( loaded_path, full_path, full_length ))
    {
        FreeLibrary( module );
        module = NULL;
        SetLastError( ERROR_INVALID_DATA );
    }

done:
    error = GetLastError();
    HeapFree( GetProcessHeap(), 0, loaded_path );
    HeapFree( GetProcessHeap(), 0, full_path );
    SetLastError( error );
    return module;
}


/***********************************************************************
 *      LoadPackagedLibrary    (kernelbase.@)
 */
HMODULE WINAPI /* DECLSPEC_HOTPATCH */ LoadPackagedLibrary( LPCWSTR name, DWORD reserved )
{
    const BYTE *data = NtCurrentTeb()->Peb->ProcessParameters->PackageDependencyData;
    struct appx_loader_graph graph;
    const BYTE *record;
    WCHAR *normalized = NULL;
    UINT32 length, basename_offset;
    BOOL has_path;
    HMODULE module;

    if (!data)
    {
        SetLastError( APPMODEL_ERROR_NO_PACKAGE );
        return NULL;
    }
    if (reserved || !name)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return NULL;
    }
    if (!get_appx_loader_graph( data, &graph ))
    {
        SetLastError( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
        return NULL;
    }

    __TRY
    {
        appx_normalize_module_name( name, &normalized, &length,
                                    &basename_offset, &has_path );
    }
    __EXCEPT_PAGE_FAULT
    {
        SetLastError( ERROR_INVALID_PARAMETER );
    }
    __ENDTRY
    if (!normalized) return NULL;

    record = appx_find_loader_record( &graph, normalized, length,
                                      basename_offset, has_path );
    if (!record)
    {
        HeapFree( GetProcessHeap(), 0, normalized );
        SetLastError( ERROR_MOD_NOT_FOUND );
        return NULL;
    }
    module = load_appx_graph_module( &graph, record, normalized, length,
                                     basename_offset );
    HeapFree( GetProcessHeap(), 0, normalized );
    return module;
}


/***********************************************************************
 *      LoadAppInitDlls    (kernelbase.@)
 */
void WINAPI LoadAppInitDlls(void)
{
    TRACE( "\n" );
}


/****************************************************************************
 *	RemoveDllDirectory   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH RemoveDllDirectory( DLL_DIRECTORY_COOKIE cookie )
{
    return set_ntstatus( LdrRemoveDllDirectory( cookie ));
}


/*************************************************************************
 *	SetDefaultDllDirectories   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetDefaultDllDirectories( DWORD flags )
{
    return set_ntstatus( LdrSetDefaultDllDirectories( flags ));
}


/***********************************************************************
 * Resources
 ***********************************************************************/


#define IS_INTRESOURCE(x)   (((ULONG_PTR)(x) >> 16) == 0)

/* retrieve the resource name to pass to the ntdll functions */
static NTSTATUS get_res_nameA( LPCSTR name, UNICODE_STRING *str )
{
    if (IS_INTRESOURCE(name))
    {
        str->Buffer = ULongToPtr( LOWORD(name) );
        return STATUS_SUCCESS;
    }
    if (name[0] == '#')
    {
        ULONG value;
        if (RtlCharToInteger( name + 1, 10, &value ) != STATUS_SUCCESS || HIWORD(value))
            return STATUS_INVALID_PARAMETER;
        str->Buffer = ULongToPtr(value);
        return STATUS_SUCCESS;
    }
    RtlCreateUnicodeStringFromAsciiz( str, name );
    RtlUpcaseUnicodeString( str, str, FALSE );
    return STATUS_SUCCESS;
}

/* retrieve the resource name to pass to the ntdll functions */
static NTSTATUS get_res_nameW( LPCWSTR name, UNICODE_STRING *str )
{
    if (IS_INTRESOURCE(name))
    {
        str->Buffer = ULongToPtr( LOWORD(name) );
        return STATUS_SUCCESS;
    }
    if (name[0] == '#')
    {
        ULONG value;
        RtlInitUnicodeString( str, name + 1 );
        if (RtlUnicodeStringToInteger( str, 10, &value ) != STATUS_SUCCESS || HIWORD(value))
            return STATUS_INVALID_PARAMETER;
        str->Buffer = ULongToPtr(value);
        return STATUS_SUCCESS;
    }
    RtlCreateUnicodeString( str, name );
    RtlUpcaseUnicodeString( str, str, FALSE );
    return STATUS_SUCCESS;
}


/**********************************************************************
 *	EnumResourceLanguagesExA	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceLanguagesExA( HMODULE module, LPCSTR type, LPCSTR name,
                                                        ENUMRESLANGPROCA func, LONG_PTR param,
                                                        DWORD flags, LANGID lang )
{
    int i;
    BOOL ret = FALSE;
    NTSTATUS status;
    UNICODE_STRING typeW, nameW;
    LDR_RESOURCE_INFO info;
    const IMAGE_RESOURCE_DIRECTORY *basedir, *resdir;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *et;

    TRACE( "%p %s %s %p %Ix %lx %d\n", module, debugstr_a(type), debugstr_a(name),
           func, param, flags, lang );

    if (flags & (RESOURCE_ENUM_MUI | RESOURCE_ENUM_MUI_SYSTEM | RESOURCE_ENUM_VALIDATE))
        FIXME( "unimplemented flags: %lx\n", flags );

    if (!flags) flags = RESOURCE_ENUM_LN | RESOURCE_ENUM_MUI;
    if (!(flags & RESOURCE_ENUM_LN)) return ret;

    if (!module) module = GetModuleHandleW( 0 );
    typeW.Buffer = nameW.Buffer = NULL;
    if ((status = LdrFindResourceDirectory_U( module, NULL, 0, &basedir )) != STATUS_SUCCESS)
        goto done;
    if ((status = get_res_nameA( type, &typeW )) != STATUS_SUCCESS)
        goto done;
    if ((status = get_res_nameA( name, &nameW )) != STATUS_SUCCESS)
        goto done;
    info.Type = (ULONG_PTR)typeW.Buffer;
    info.Name = (ULONG_PTR)nameW.Buffer;
    if ((status = LdrFindResourceDirectory_U( module, &info, 2, &resdir )) != STATUS_SUCCESS)
        goto done;

    et = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(resdir + 1);
    __TRY
    {
        for (i = 0; i < resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries; i++)
        {
            ret = func( module, type, name, et[i].Id, param );
            if (!ret) break;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        ret = FALSE;
        status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY
done:
    if (!IS_INTRESOURCE(typeW.Buffer)) HeapFree( GetProcessHeap(), 0, typeW.Buffer );
    if (!IS_INTRESOURCE(nameW.Buffer)) HeapFree( GetProcessHeap(), 0, nameW.Buffer );
    if (status != STATUS_SUCCESS) SetLastError( RtlNtStatusToDosError(status) );
    return ret;
}


/**********************************************************************
 *	EnumResourceLanguagesExW	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceLanguagesExW( HMODULE module, LPCWSTR type, LPCWSTR name,
                                                        ENUMRESLANGPROCW func, LONG_PTR param,
                                                        DWORD flags, LANGID lang )
{
    int i;
    BOOL ret = FALSE;
    NTSTATUS status;
    UNICODE_STRING typeW, nameW;
    LDR_RESOURCE_INFO info;
    const IMAGE_RESOURCE_DIRECTORY *basedir, *resdir;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *et;

    TRACE( "%p %s %s %p %Ix %lx %d\n", module, debugstr_w(type), debugstr_w(name),
           func, param, flags, lang );

    if (flags & (RESOURCE_ENUM_MUI | RESOURCE_ENUM_MUI_SYSTEM | RESOURCE_ENUM_VALIDATE))
        FIXME( "unimplemented flags: %lx\n", flags );

    if (!flags) flags = RESOURCE_ENUM_LN | RESOURCE_ENUM_MUI;
    if (!(flags & RESOURCE_ENUM_LN)) return ret;

    if (!module) module = GetModuleHandleW( 0 );
    typeW.Buffer = nameW.Buffer = NULL;
    if ((status = LdrFindResourceDirectory_U( module, NULL, 0, &basedir )) != STATUS_SUCCESS)
        goto done;
    if ((status = get_res_nameW( type, &typeW )) != STATUS_SUCCESS)
        goto done;
    if ((status = get_res_nameW( name, &nameW )) != STATUS_SUCCESS)
        goto done;
    info.Type = (ULONG_PTR)typeW.Buffer;
    info.Name = (ULONG_PTR)nameW.Buffer;
    if ((status = LdrFindResourceDirectory_U( module, &info, 2, &resdir )) != STATUS_SUCCESS)
        goto done;

    et = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(resdir + 1);
    __TRY
    {
        for (i = 0; i < resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries; i++)
        {
            ret = func( module, type, name, et[i].Id, param );
            if (!ret) break;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        ret = FALSE;
        status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY
done:
    if (!IS_INTRESOURCE(typeW.Buffer)) HeapFree( GetProcessHeap(), 0, typeW.Buffer );
    if (!IS_INTRESOURCE(nameW.Buffer)) HeapFree( GetProcessHeap(), 0, nameW.Buffer );
    if (status != STATUS_SUCCESS) SetLastError( RtlNtStatusToDosError(status) );
    return ret;
}


/**********************************************************************
 *	EnumResourceNamesExA	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceNamesExA( HMODULE module, LPCSTR type, ENUMRESNAMEPROCA func,
                                                    LONG_PTR param, DWORD flags, LANGID lang )
{
    int i;
    BOOL ret = FALSE;
    DWORD len = 0, newlen;
    LPSTR name = NULL;
    NTSTATUS status;
    UNICODE_STRING typeW;
    LDR_RESOURCE_INFO info;
    const IMAGE_RESOURCE_DIRECTORY *basedir, *resdir;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *et;
    const IMAGE_RESOURCE_DIR_STRING_U *str;

    TRACE( "%p %s %p %Ix\n", module, debugstr_a(type), func, param );

    if (flags & (RESOURCE_ENUM_MUI | RESOURCE_ENUM_MUI_SYSTEM | RESOURCE_ENUM_VALIDATE))
        FIXME( "unimplemented flags: %lx\n", flags );

    if (!flags) flags = RESOURCE_ENUM_LN | RESOURCE_ENUM_MUI;
    if (!(flags & RESOURCE_ENUM_LN)) return ret;

    if (!module) module = GetModuleHandleW( 0 );
    typeW.Buffer = NULL;
    if ((status = LdrFindResourceDirectory_U( module, NULL, 0, &basedir )) != STATUS_SUCCESS)
        goto done;
    if ((status = get_res_nameA( type, &typeW )) != STATUS_SUCCESS)
        goto done;
    info.Type = (ULONG_PTR)typeW.Buffer;
    if ((status = LdrFindResourceDirectory_U( module, &info, 1, &resdir )) != STATUS_SUCCESS)
        goto done;

    et = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(resdir + 1);
    __TRY
    {
        for (i = 0; i < resdir->NumberOfNamedEntries+resdir->NumberOfIdEntries; i++)
        {
            if (et[i].NameIsString)
            {
                str = (const IMAGE_RESOURCE_DIR_STRING_U *)((const BYTE *)basedir + et[i].NameOffset);
                newlen = WideCharToMultiByte(CP_ACP, 0, str->NameString, str->Length, NULL, 0, NULL, NULL);
                if (newlen + 1 > len)
                {
                    len = newlen + 1;
                    HeapFree( GetProcessHeap(), 0, name );
                    if (!(name = HeapAlloc( GetProcessHeap(), 0, len + 1 )))
                    {
                        ret = FALSE;
                        break;
                    }
                }
                WideCharToMultiByte( CP_ACP, 0, str->NameString, str->Length, name, len, NULL, NULL );
                name[newlen] = 0;
                ret = func( module, type, name, param );
            }
            else
            {
                ret = func( module, type, UIntToPtr(et[i].Id), param );
            }
            if (!ret) break;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        ret = FALSE;
        status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY

done:
    HeapFree( GetProcessHeap(), 0, name );
    if (!IS_INTRESOURCE(typeW.Buffer)) HeapFree( GetProcessHeap(), 0, typeW.Buffer );
    if (status != STATUS_SUCCESS) SetLastError( RtlNtStatusToDosError(status) );
    return ret;
}


/**********************************************************************
 *	EnumResourceNamesExW	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceNamesExW( HMODULE module, LPCWSTR type, ENUMRESNAMEPROCW func,
                                                    LONG_PTR param, DWORD flags, LANGID lang )
{
    int i, len = 0;
    BOOL ret = FALSE;
    LPWSTR name = NULL;
    NTSTATUS status;
    UNICODE_STRING typeW;
    LDR_RESOURCE_INFO info;
    const IMAGE_RESOURCE_DIRECTORY *basedir, *resdir;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *et;
    const IMAGE_RESOURCE_DIR_STRING_U *str;

    TRACE( "%p %s %p %Ix\n", module, debugstr_w(type), func, param );

    if (flags & (RESOURCE_ENUM_MUI | RESOURCE_ENUM_MUI_SYSTEM | RESOURCE_ENUM_VALIDATE))
        FIXME( "unimplemented flags: %lx\n", flags );

    if (!flags) flags = RESOURCE_ENUM_LN | RESOURCE_ENUM_MUI;
    if (!(flags & RESOURCE_ENUM_LN)) return ret;

    if (!module) module = GetModuleHandleW( 0 );
    typeW.Buffer = NULL;
    if ((status = LdrFindResourceDirectory_U( module, NULL, 0, &basedir )) != STATUS_SUCCESS)
        goto done;
    if ((status = get_res_nameW( type, &typeW )) != STATUS_SUCCESS)
        goto done;
    info.Type = (ULONG_PTR)typeW.Buffer;
    if ((status = LdrFindResourceDirectory_U( module, &info, 1, &resdir )) != STATUS_SUCCESS)
        goto done;

    et = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(resdir + 1);
    __TRY
    {
        for (i = 0; i < resdir->NumberOfNamedEntries+resdir->NumberOfIdEntries; i++)
        {
            if (et[i].NameIsString)
            {
                str = (const IMAGE_RESOURCE_DIR_STRING_U *)((const BYTE *)basedir + et[i].NameOffset);
                if (str->Length + 1 > len)
                {
                    len = str->Length + 1;
                    HeapFree( GetProcessHeap(), 0, name );
                    if (!(name = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) )))
                    {
                        ret = FALSE;
                        break;
                    }
                }
                memcpy(name, str->NameString, str->Length * sizeof (WCHAR));
                name[str->Length] = 0;
                ret = func( module, type, name, param );
            }
            else
            {
                ret = func( module, type, UIntToPtr(et[i].Id), param );
            }
            if (!ret) break;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        ret = FALSE;
        status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY
done:
    HeapFree( GetProcessHeap(), 0, name );
    if (!IS_INTRESOURCE(typeW.Buffer)) HeapFree( GetProcessHeap(), 0, typeW.Buffer );
    if (status != STATUS_SUCCESS) SetLastError( RtlNtStatusToDosError(status) );
    return ret;
}


/**********************************************************************
 *	EnumResourceNamesW	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceNamesW( HMODULE module, LPCWSTR type,
                                                  ENUMRESNAMEPROCW func, LONG_PTR param )
{
    return EnumResourceNamesExW( module, type, func, param, 0, 0 );
}


/**********************************************************************
 *	EnumResourceTypesExA	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceTypesExA( HMODULE module, ENUMRESTYPEPROCA func, LONG_PTR param,
                                                    DWORD flags, LANGID lang )
{
    int i;
    BOOL ret = FALSE;
    LPSTR type = NULL;
    DWORD len = 0, newlen;
    const IMAGE_RESOURCE_DIRECTORY *resdir;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *et;
    const IMAGE_RESOURCE_DIR_STRING_U *str;

    TRACE( "%p %p %Ix\n", module, func, param );

    if (flags & (RESOURCE_ENUM_MUI | RESOURCE_ENUM_MUI_SYSTEM | RESOURCE_ENUM_VALIDATE))
        FIXME( "unimplemented flags: %lx\n", flags );

    if (!flags) flags = RESOURCE_ENUM_LN | RESOURCE_ENUM_MUI;
    if (!(flags & RESOURCE_ENUM_LN)) return ret;

    if (!module) module = GetModuleHandleW( 0 );

    if (!set_ntstatus( LdrFindResourceDirectory_U( module, NULL, 0, &resdir ))) return FALSE;

    et = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(resdir + 1);
    for (i = 0; i < resdir->NumberOfNamedEntries+resdir->NumberOfIdEntries; i++)
    {
        if (et[i].NameIsString)
        {
            str = (const IMAGE_RESOURCE_DIR_STRING_U *)((const BYTE *)resdir + et[i].NameOffset);
            newlen = WideCharToMultiByte( CP_ACP, 0, str->NameString, str->Length, NULL, 0, NULL, NULL);
            if (newlen + 1 > len)
            {
                len = newlen + 1;
                HeapFree( GetProcessHeap(), 0, type );
                if (!(type = HeapAlloc( GetProcessHeap(), 0, len ))) return FALSE;
            }
            WideCharToMultiByte( CP_ACP, 0, str->NameString, str->Length, type, len, NULL, NULL);
            type[newlen] = 0;
            ret = func( module, type, param );
        }
        else
        {
            ret = func( module, UIntToPtr(et[i].Id), param );
        }
        if (!ret) break;
    }
    HeapFree( GetProcessHeap(), 0, type );
    return ret;
}


/**********************************************************************
 *	EnumResourceTypesExW	(kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH EnumResourceTypesExW( HMODULE module, ENUMRESTYPEPROCW func, LONG_PTR param,
                                                    DWORD flags, LANGID lang )
{
    int i, len = 0;
    BOOL ret = FALSE;
    LPWSTR type = NULL;
    const IMAGE_RESOURCE_DIRECTORY *resdir;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *et;
    const IMAGE_RESOURCE_DIR_STRING_U *str;

    TRACE( "%p %p %Ix\n", module, func, param );

    if (!flags) flags = RESOURCE_ENUM_LN | RESOURCE_ENUM_MUI;
    if (!(flags & RESOURCE_ENUM_LN)) return ret;

    if (!module) module = GetModuleHandleW( 0 );

    if (!set_ntstatus( LdrFindResourceDirectory_U( module, NULL, 0, &resdir ))) return FALSE;

    et = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(resdir + 1);
    for (i = 0; i < resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries; i++)
    {
        if (et[i].NameIsString)
        {
            str = (const IMAGE_RESOURCE_DIR_STRING_U *)((const BYTE *)resdir + et[i].NameOffset);
            if (str->Length + 1 > len)
            {
                len = str->Length + 1;
                HeapFree( GetProcessHeap(), 0, type );
                if (!(type = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) ))) return FALSE;
            }
            memcpy(type, str->NameString, str->Length * sizeof (WCHAR));
            type[str->Length] = 0;
            ret = func( module, type, param );
        }
        else
        {
            ret = func( module, UIntToPtr(et[i].Id), param );
        }
        if (!ret) break;
    }
    HeapFree( GetProcessHeap(), 0, type );
    return ret;
}


/**********************************************************************
 *	    FindResourceExW  (kernelbase.@)
 */
HRSRC WINAPI DECLSPEC_HOTPATCH FindResourceExW( HMODULE module, LPCWSTR type, LPCWSTR name, WORD lang )
{
    NTSTATUS status;
    UNICODE_STRING nameW, typeW;
    LDR_RESOURCE_INFO info;
    const IMAGE_RESOURCE_DATA_ENTRY *entry = NULL;

    TRACE( "%p %s %s %04x\n", module, debugstr_w(type), debugstr_w(name), lang );

    if (!module) module = GetModuleHandleW( 0 );
    nameW.Buffer = typeW.Buffer = NULL;

    __TRY
    {
        if ((status = get_res_nameW( name, &nameW )) != STATUS_SUCCESS) goto done;
        if ((status = get_res_nameW( type, &typeW )) != STATUS_SUCCESS) goto done;
        info.Type = (ULONG_PTR)typeW.Buffer;
        info.Name = (ULONG_PTR)nameW.Buffer;
        info.Language = lang;
        status = LdrFindResource_U( module, &info, 3, &entry );
    done:
        if (status != STATUS_SUCCESS) SetLastError( RtlNtStatusToDosError(status) );
    }
    __EXCEPT_PAGE_FAULT
    {
        SetLastError( ERROR_INVALID_PARAMETER );
    }
    __ENDTRY

    if (!IS_INTRESOURCE(nameW.Buffer)) HeapFree( GetProcessHeap(), 0, nameW.Buffer );
    if (!IS_INTRESOURCE(typeW.Buffer)) HeapFree( GetProcessHeap(), 0, typeW.Buffer );
    return (HRSRC)entry;
}


/**********************************************************************
 *	    FindResourceW    (kernelbase.@)
 */
HRSRC WINAPI DECLSPEC_HOTPATCH FindResourceW( HINSTANCE module, LPCWSTR name, LPCWSTR type )
{
    return FindResourceExW( module, type, name, MAKELANGID( LANG_NEUTRAL, SUBLANG_NEUTRAL ) );
}


/**********************************************************************
 *	    FreeResource     (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FreeResource( HGLOBAL handle )
{
    return FALSE;
}


/**********************************************************************
 *	    LoadResource     (kernelbase.@)
 */
HGLOBAL WINAPI DECLSPEC_HOTPATCH LoadResource( HINSTANCE module, HRSRC rsrc )
{
    void *ret;

    TRACE( "%p %p\n", module, rsrc );

    if (!rsrc) return 0;
    if (!module) module = GetModuleHandleW( 0 );
    if (!set_ntstatus( LdrAccessResource( module, (IMAGE_RESOURCE_DATA_ENTRY *)rsrc, &ret, NULL )))
        return 0;
    return ret;
}


/**********************************************************************
 *	    LockResource     (kernelbase.@)
 */
LPVOID WINAPI DECLSPEC_HOTPATCH LockResource( HGLOBAL handle )
{
    return handle;
}


/**********************************************************************
 *	    SizeofResource   (kernelbase.@)
 */
DWORD WINAPI DECLSPEC_HOTPATCH SizeofResource( HINSTANCE module, HRSRC rsrc )
{
    if (!rsrc) return 0;
    return ((IMAGE_RESOURCE_DATA_ENTRY *)rsrc)->Size;
}


/***********************************************************************
 * Activation contexts
 ***********************************************************************/


/***********************************************************************
 *          ActivateActCtx    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ActivateActCtx( HANDLE context, ULONG_PTR *cookie )
{
    return set_ntstatus( RtlActivateActivationContext( 0, context, cookie ));
}


/***********************************************************************
 *          AddRefActCtx    (kernelbase.@)
 */
void WINAPI DECLSPEC_HOTPATCH AddRefActCtx( HANDLE context )
{
    RtlAddRefActivationContext( context );
}


/***********************************************************************
 *          CreateActCtxW    (kernelbase.@)
 */
HANDLE WINAPI DECLSPEC_HOTPATCH CreateActCtxW( PCACTCTXW ctx )
{
    struct _ACTIVATION_CONTEXT *context;

    TRACE( "%p %08lx\n", ctx, ctx ? ctx->dwFlags : 0 );

    if (!set_ntstatus( RtlCreateActivationContext( &context, ctx ))) return INVALID_HANDLE_VALUE;
    return context;
}


/***********************************************************************
 *          DeactivateActCtx    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH DeactivateActCtx( DWORD flags, ULONG_PTR cookie )
{
    RtlDeactivateActivationContext( flags, cookie );
    return TRUE;
}


/***********************************************************************
 *          FindActCtxSectionGuid    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FindActCtxSectionGuid( DWORD flags, const GUID *ext_guid, ULONG id,
                                                     const GUID *guid, PACTCTX_SECTION_KEYED_DATA info )
{
    return set_ntstatus( RtlFindActivationContextSectionGuid( flags, ext_guid, id, guid, info ));
}


/***********************************************************************
 *          FindActCtxSectionStringW    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FindActCtxSectionStringW( DWORD flags, const GUID *ext_guid, ULONG id,
                                                        LPCWSTR str, PACTCTX_SECTION_KEYED_DATA info )
{
    UNICODE_STRING us;

    if (!info)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    RtlInitUnicodeString( &us, str );
    return set_ntstatus( RtlFindActivationContextSectionString( flags, ext_guid, id, &us, info ));
}


/***********************************************************************
 *          GetCurrentActCtx    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetCurrentActCtx( HANDLE *pcontext )
{
    return set_ntstatus( RtlGetActiveActivationContext( (struct _ACTIVATION_CONTEXT **)pcontext ));
}


/***********************************************************************
 *          QueryActCtxSettingsW    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH QueryActCtxSettingsW( DWORD flags, HANDLE ctx, const WCHAR *ns,
                                                    const WCHAR *settings, WCHAR *buffer, SIZE_T size,
                                                    SIZE_T *written )
{
    return set_ntstatus( RtlQueryActivationContextApplicationSettings( flags, ctx, ns, settings,
                                                                       buffer, size, written ));
}


/***********************************************************************
 *          QueryActCtxW    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH QueryActCtxW( DWORD flags, HANDLE context, PVOID inst, ULONG class,
                                            PVOID buffer, SIZE_T size, SIZE_T *written )
{
    return set_ntstatus( RtlQueryInformationActivationContext( flags, context, inst, class,
                                                               buffer, size, written ));
}


/***********************************************************************
 *          ReleaseActCtx    (kernelbase.@)
 */
void WINAPI DECLSPEC_HOTPATCH ReleaseActCtx( HANDLE context )
{
    RtlReleaseActivationContext( context );
}


/***********************************************************************
 *          ZombifyActCtx    (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ZombifyActCtx( HANDLE context )
{
    return set_ntstatus( RtlZombifyActivationContext( context ));
}
