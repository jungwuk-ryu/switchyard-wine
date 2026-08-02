/*
 * Implementation of VERSION.DLL
 *
 * Copyright 1996,1997 Marcus Meissner
 * Copyright 1997 David Cuthbert
 * Copyright 1999 Ulrich Weigand
 * Copyright 2005 Paul Vriens
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
 *
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winver.h"
#include "winuser.h"
#include "winnls.h"
#include "winternl.h"
#include "winerror.h"
#include "appmodel.h"

#include "kernelbase.h"
#include "../appxsvc/query.h"
#include "wine/appx_package_graph.h"
#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(ver);

typedef struct
{
    WORD offset;
    WORD length;
    WORD flags;
    WORD id;
    WORD handle;
    WORD usage;
} NE_NAMEINFO;

typedef struct
{
    WORD  type_id;
    WORD  count;
    DWORD resloader;
} NE_TYPEINFO;

struct version_info
{
    DWORD major;
    DWORD minor;
    DWORD build;
};

/***********************************************************************
 * Version Info Structure
 */

typedef struct
{
    WORD  wLength;
    WORD  wValueLength;
    CHAR  szKey[1];
#if 0   /* variable length structure */
    /* DWORD aligned */
    BYTE  Value[];
    /* DWORD aligned */
    VS_VERSION_INFO_STRUCT16 Children[];
#endif
} VS_VERSION_INFO_STRUCT16;

typedef struct
{
    WORD  wLength;
    WORD  wValueLength;
    WORD  wType; /* 1:Text, 0:Binary */
    WCHAR szKey[1];
#if 0   /* variable length structure */
    /* DWORD aligned */
    BYTE  Value[];
    /* DWORD aligned */
    VS_VERSION_INFO_STRUCT32 Children[];
#endif
} VS_VERSION_INFO_STRUCT32;

#define VersionInfoIs16( ver ) \
    ( ((const VS_VERSION_INFO_STRUCT16 *)ver)->szKey[0] >= ' ' )

#define DWORD_ALIGN( base, ptr ) \
    ( (LPBYTE)(base) + ((((LPBYTE)(ptr) - (LPBYTE)(base)) + 3) & ~3) )

#define VersionInfo16_Value( ver )  \
    DWORD_ALIGN( (ver), (ver)->szKey + strlen((ver)->szKey) + 1 )
#define VersionInfo32_Value( ver )  \
    DWORD_ALIGN( (ver), (ver)->szKey + lstrlenW((ver)->szKey) + 1 )

#define VersionInfo16_Children( ver )  \
    (const VS_VERSION_INFO_STRUCT16 *)( VersionInfo16_Value( ver ) + \
                           ( ( (ver)->wValueLength + 3 ) & ~3 ) )
#define VersionInfo32_Children( ver )  \
    (const VS_VERSION_INFO_STRUCT32 *)( VersionInfo32_Value( ver ) + \
                           ( ( (ver)->wValueLength * \
                               ((ver)->wType? 2 : 1) + 3 ) & ~3 ) )

#define VersionInfo16_Next( ver ) \
    (VS_VERSION_INFO_STRUCT16 *)( (LPBYTE)ver + (((ver)->wLength + 3) & ~3) )
#define VersionInfo32_Next( ver ) \
    (VS_VERSION_INFO_STRUCT32 *)( (LPBYTE)ver + (((ver)->wLength + 3) & ~3) )


/***********************************************************************
 * Win8 info, reported if the app doesn't provide compat GUID in the manifest and
 * doesn't have higher OS version in PE header.
 */
static const struct version_info windows8_version_info = { 6, 2, 9200 };

/***********************************************************************
 * Win8.1 info, reported if the app doesn't provide compat GUID in the manifest and
 * OS version in PE header is 8.1 or higher but below 10.
 */
static const struct version_info windows8_1_version_info = { 6, 3, 9600 };


/***********************************************************************
 * Windows versions that need compatibility GUID specified in manifest
 * in order to be reported by the APIs.
 */
static const struct
{
    struct version_info info;
    GUID guid;
} version_data[] =
{
    /* Windows 8.1 */
    {
        { 6, 3, 9600 },
        {0x1f676c76,0x80e1,0x4239,{0x95,0xbb,0x83,0xd0,0xf6,0xd0,0xda,0x78}}
    },
    /* Windows 10 */
    {
        { 10, 0, 19045 },
        {0x8e0f7a12,0xbfb3,0x4fe8,{0xb9,0xa5,0x48,0xfd,0x50,0xa1,0x5a,0x9a}}
    }
};


/******************************************************************************
 *  init_current_version
 *
 * Initialize the current_version variable.
 *
 * For compatibility, Windows 8.1 and later report Win8 version unless the app
 * has a manifest or higher OS version in the PE optional header
 * that confirms its compatibility with newer versions of Windows.
 *
 */
static RTL_OSVERSIONINFOEXW current_version;

static BOOL CALLBACK init_current_version(PINIT_ONCE init_once, PVOID parameter, PVOID *context)
{
    struct acci
    {
        DWORD ElementCount;
        COMPATIBILITY_CONTEXT_ELEMENT Elements[1];
    } *acci;
    BOOL have_os_compat_elements = FALSE;
    const struct version_info *ver;
    IMAGE_NT_HEADERS *nt;
    SIZE_T req;
    int idx;

    current_version.dwOSVersionInfoSize = sizeof(current_version);
    if (!set_ntstatus( RtlGetVersion(&current_version) )) return FALSE;

    for (idx = ARRAY_SIZE(version_data); idx--;)
        if ( current_version.dwMajorVersion >  version_data[idx].info.major ||
            (current_version.dwMajorVersion == version_data[idx].info.major &&
             current_version.dwMinorVersion >= version_data[idx].info.minor))
            break;

    if (idx < 0) return TRUE;
    ver = &windows8_version_info;

    if (RtlQueryInformationActivationContext(0, NtCurrentTeb()->Peb->ActivationContextData, NULL,
            CompatibilityInformationInActivationContext, NULL, 0, &req) != STATUS_BUFFER_TOO_SMALL
        || !req)
        goto done;

    if (!(acci = HeapAlloc(GetProcessHeap(), 0, req)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (RtlQueryInformationActivationContext(0, NtCurrentTeb()->Peb->ActivationContextData, NULL,
            CompatibilityInformationInActivationContext, acci, req, &req) == STATUS_SUCCESS)
    {
        do
        {
            DWORD i;

            for (i = 0; i < acci->ElementCount; i++)
            {
                if (acci->Elements[i].Type != ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS)
                    continue;

                have_os_compat_elements = TRUE;

                if (IsEqualGUID(&acci->Elements[i].Id, &version_data[idx].guid))
                {
                    ver = &version_data[idx].info;

                    if (ver->major == current_version.dwMajorVersion &&
                        ver->minor == current_version.dwMinorVersion)
                        ver = NULL;

                    idx = 0;  /* break from outer loop */
                    break;
                }
            }
        } while (idx--);
    }
    HeapFree(GetProcessHeap(), 0, acci);

done:
    if (!have_os_compat_elements && current_version.dwMajorVersion >= 10
            && (nt = RtlImageNtHeader(NtCurrentTeb()->Peb->ImageBaseAddress))
            && (nt->OptionalHeader.MajorOperatingSystemVersion > 6
            || (nt->OptionalHeader.MajorOperatingSystemVersion == 6
            && nt->OptionalHeader.MinorOperatingSystemVersion >= 3)))
    {
        if (current_version.dwMajorVersion > 10)
            FIXME("Unsupported current_version.dwMajorVersion %lu.\n", current_version.dwMajorVersion);

        ver = nt->OptionalHeader.MajorOperatingSystemVersion >= 10 ? NULL : &windows8_1_version_info;
    }

    if (ver)
    {
        current_version.dwMajorVersion = ver->major;
        current_version.dwMinorVersion = ver->minor;
        current_version.dwBuildNumber  = ver->build;
    }
    return TRUE;
}


/**********************************************************************
 *  find_entry_by_id
 *
 * Find an entry by id in a resource directory
 * Copied from loader/pe_resource.c
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_by_id( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                         WORD id, const void *root,
                                                         DWORD root_size )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;
    int min, max, pos;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    min = dir->NumberOfNamedEntries;
    max = min + dir->NumberOfIdEntries - 1;

    if (max >= (root_size - ((INT_PTR)dir - (INT_PTR)root) - sizeof(*dir)) / sizeof(*entry))
        return NULL;

    while (min <= max)
    {
        pos = (min + max) / 2;
        if (entry[pos].Id == id)
        {
            DWORD offset = entry[pos].OffsetToDirectory;
            if (offset > root_size - sizeof(*dir)) return NULL;
            return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + offset);
        }
        if (entry[pos].Id > id) max = pos - 1;
        else min = pos + 1;
    }
    return NULL;
}


/**********************************************************************
 *  find_entry_default
 *
 * Find a default entry in a resource directory
 * Copied from loader/pe_resource.c
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_default( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                           const void *root )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + entry->OffsetToDirectory);
}


/**********************************************************************
 *  push_language
 *
 * push a language onto the list of languages to try
 */
static inline int push_language( WORD *list, int pos, WORD lang )
{
    int i;
    for (i = 0; i < pos; i++) if (list[i] == lang) return pos;
    list[pos++] = lang;
    return pos;
}


/**********************************************************************
 *  find_entry_language
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_language( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                            const void *root, DWORD root_size,
                                                            DWORD flags )
{
    const IMAGE_RESOURCE_DIRECTORY *ret;
    WORD list[9];
    int i, pos = 0;

    if (flags & FILE_VER_GET_LOCALISED)
    {
        /* cf. LdrFindResource_U */
        pos = push_language( list, pos, MAKELANGID( LANG_NEUTRAL, SUBLANG_NEUTRAL ) );
        pos = push_language( list, pos, LANGIDFROMLCID( NtCurrentTeb()->CurrentLocale ) );
        pos = push_language( list, pos, GetUserDefaultLangID() );
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetUserDefaultLangID()), SUBLANG_NEUTRAL ));
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetUserDefaultLangID()), SUBLANG_DEFAULT ));
        pos = push_language( list, pos, GetSystemDefaultLangID() );
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_NEUTRAL ));
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_DEFAULT ));
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }
    else
    {
        /* FIXME: resolve LN file here */
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }

    for (i = 0; i < pos; i++) if ((ret = find_entry_by_id( dir, list[i], root, root_size ))) return ret;
    return find_entry_default( dir, root );
}


static DWORD read_data( HANDLE handle, DWORD offset, void *data, DWORD len )
{
    DWORD res;

    SetFilePointer( handle, offset, NULL, FILE_BEGIN );
    if (!ReadFile( handle, data, len, &res, NULL )) res = 0;
    return res;
}

/***********************************************************************
 *           find_ne_resource         [internal]
 */
static BOOL find_ne_resource( HANDLE handle, DWORD *resLen, DWORD *resOff )
{
    const WORD typeid = VS_FILE_INFO | 0x8000;
    const WORD resid = VS_VERSION_INFO | 0x8000;
    IMAGE_OS2_HEADER nehd;
    NE_TYPEINFO *typeInfo;
    NE_NAMEINFO *nameInfo;
    DWORD nehdoffset = *resOff;
    LPBYTE resTab;
    DWORD resTabSize;
    int count;

    /* Read in NE header */
    if (read_data( handle, nehdoffset, &nehd, sizeof(nehd) ) != sizeof(nehd)) return FALSE;

    resTabSize = nehd.ne_restab - nehd.ne_rsrctab;
    if ( !resTabSize )
    {
        TRACE("No resources in NE dll\n" );
        return FALSE;
    }

    /* Read in resource table */
    resTab = HeapAlloc( GetProcessHeap(), 0, resTabSize );
    if ( !resTab ) return FALSE;

    if (read_data( handle, nehd.ne_rsrctab + nehdoffset, resTab, resTabSize ) != resTabSize)
    {
        HeapFree( GetProcessHeap(), 0, resTab );
        return FALSE;
    }

    /* Find resource */
    typeInfo = (NE_TYPEINFO *)(resTab + 2);
    while (typeInfo->type_id)
    {
        if (typeInfo->type_id == typeid) goto found_type;
        typeInfo = (NE_TYPEINFO *)((char *)(typeInfo + 1) +
                                   typeInfo->count * sizeof(NE_NAMEINFO));
    }
    TRACE("No typeid entry found\n" );
    HeapFree( GetProcessHeap(), 0, resTab );
    return FALSE;

 found_type:
    nameInfo = (NE_NAMEINFO *)(typeInfo + 1);

    for (count = typeInfo->count; count > 0; count--, nameInfo++)
        if (nameInfo->id == resid) goto found_name;

    TRACE("No resid entry found\n" );
    HeapFree( GetProcessHeap(), 0, resTab );
    return FALSE;

 found_name:
    /* Return resource data */
    *resLen = nameInfo->length << *(WORD *)resTab;
    *resOff = nameInfo->offset << *(WORD *)resTab;

    HeapFree( GetProcessHeap(), 0, resTab );
    return TRUE;
}

/***********************************************************************
 *           find_pe_resource         [internal]
 */
static BOOL find_pe_resource( HANDLE handle, DWORD *resLen, DWORD *resOff, DWORD flags )
{
    union
    {
        IMAGE_NT_HEADERS32 nt32;
        IMAGE_NT_HEADERS64 nt64;
    } pehd;
    DWORD pehdoffset = *resOff;
    PIMAGE_DATA_DIRECTORY resDataDir;
    PIMAGE_SECTION_HEADER sections;
    LPBYTE resSection;
    DWORD len, section_size, data_size, resDirSize;
    const void *resDir;
    const IMAGE_RESOURCE_DIRECTORY *resPtr;
    const IMAGE_RESOURCE_DATA_ENTRY *resData;
    int i, nSections;
    BOOL ret = FALSE;

    /* Read in PE header */
    len = read_data( handle, pehdoffset, &pehd, sizeof(pehd) );
    if (len < sizeof(pehd.nt32.FileHeader)) return FALSE;
    if (len < sizeof(pehd)) memset( (char *)&pehd + len, 0, sizeof(pehd) - len );

    switch (pehd.nt32.OptionalHeader.Magic)
    {
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        resDataDir = pehd.nt32.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        resDataDir = pehd.nt64.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    default:
        return FALSE;
    }

    if ( !resDataDir->Size )
    {
        TRACE("No resources in PE dll\n" );
        return FALSE;
    }

    /* Read in section table */
    nSections = pehd.nt32.FileHeader.NumberOfSections;
    sections = HeapAlloc( GetProcessHeap(), 0,
                          nSections * sizeof(IMAGE_SECTION_HEADER) );
    if ( !sections ) return FALSE;

    len = FIELD_OFFSET( IMAGE_NT_HEADERS32, OptionalHeader ) + pehd.nt32.FileHeader.SizeOfOptionalHeader;
    if (read_data( handle, pehdoffset + len, sections, nSections * sizeof(IMAGE_SECTION_HEADER) ) !=
        nSections * sizeof(IMAGE_SECTION_HEADER))
    {
        HeapFree( GetProcessHeap(), 0, sections );
        return FALSE;
    }

    /* Find resource section */
    for ( i = 0; i < nSections; i++ )
        if (    resDataDir->VirtualAddress >= sections[i].VirtualAddress
             && resDataDir->VirtualAddress <  sections[i].VirtualAddress +
                                              sections[i].SizeOfRawData )
            break;

    if ( i == nSections )
    {
        HeapFree( GetProcessHeap(), 0, sections );
        TRACE("Couldn't find resource section\n" );
        return FALSE;
    }

    /* Read in resource section */
    data_size = sections[i].SizeOfRawData;
    section_size = max( data_size, sections[i].Misc.VirtualSize );
    resSection = HeapAlloc( GetProcessHeap(), 0, section_size );
    if ( !resSection )
    {
        HeapFree( GetProcessHeap(), 0, sections );
        return FALSE;
    }

    if (read_data( handle, sections[i].PointerToRawData, resSection, data_size ) != data_size) goto done;
    if (data_size < section_size) memset( (char *)resSection + data_size, 0, section_size - data_size );

    /* Find resource */
    resDir = resSection + (resDataDir->VirtualAddress - sections[i].VirtualAddress);
    resDirSize = section_size - (resDataDir->VirtualAddress - sections[i].VirtualAddress);

    resPtr = resDir;
    resPtr = find_entry_by_id( resPtr, VS_FILE_INFO, resDir, resDirSize );
    if ( !resPtr )
    {
        TRACE("No typeid entry found\n" );
        goto done;
    }
    resPtr = find_entry_by_id( resPtr, VS_VERSION_INFO, resDir, resDirSize );
    if ( !resPtr )
    {
        TRACE("No resid entry found\n" );
        goto done;
    }
    resPtr = find_entry_language( resPtr, resDir, resDirSize, flags );
    if ( !resPtr )
    {
        TRACE("No default language entry found\n" );
        goto done;
    }

    /* Find resource data section */
    resData = (const IMAGE_RESOURCE_DATA_ENTRY*)resPtr;
    for ( i = 0; i < nSections; i++ )
        if (    resData->OffsetToData >= sections[i].VirtualAddress
             && resData->OffsetToData <  sections[i].VirtualAddress +
                                         sections[i].SizeOfRawData )
            break;

    if ( i == nSections )
    {
        TRACE("Couldn't find resource data section\n" );
        goto done;
    }

    /* Return resource data */
    *resLen = resData->Size;
    *resOff = resData->OffsetToData - sections[i].VirtualAddress + sections[i].PointerToRawData;
    ret = TRUE;

 done:
    HeapFree( GetProcessHeap(), 0, resSection );
    HeapFree( GetProcessHeap(), 0, sections );
    return ret;
}


/***********************************************************************
 *           find_version_resource         [internal]
 */
static DWORD find_version_resource( HANDLE handle, DWORD *reslen, DWORD *offset, DWORD flags )
{
    IMAGE_DOS_HEADER mzh;
    WORD magic;

    if (read_data( handle, 0, &mzh, sizeof(mzh) ) != sizeof(mzh)) return 0;
    if (mzh.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    if (read_data( handle, mzh.e_lfanew, &magic, sizeof(magic) ) != sizeof(magic)) return 0;
    *offset = mzh.e_lfanew;

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        if (!find_ne_resource( handle, reslen, offset )) magic = 0;
        break;
    case IMAGE_NT_SIGNATURE:
        if (!find_pe_resource( handle, reslen, offset, flags )) magic = 0;
        break;
    }
    WARN( "Can't handle %04x files.\n", magic );
    return magic;
}

/******************************************************************************
 *   This function will print via standard TRACE, debug info regarding
 *   the file info structure vffi.
 */
static void print_vffi_debug(const VS_FIXEDFILEINFO *vffi)
{
    BOOL    versioned_printer = FALSE;

    if((vffi->dwFileType == VFT_DLL) || (vffi->dwFileType == VFT_DRV))
    {
        if(vffi->dwFileSubtype == VFT2_DRV_VERSIONED_PRINTER)
            /* this is documented for newer w2k Drivers and up */
            versioned_printer = TRUE;
        else if( (vffi->dwFileSubtype == VFT2_DRV_PRINTER) &&
                 (vffi->dwFileVersionMS != vffi->dwProductVersionMS) &&
                 (vffi->dwFileVersionMS > 0) &&
                 (vffi->dwFileVersionMS <= 3) )
            /* found this on NT 3.51, NT4.0 and old w2k Drivers */
            versioned_printer = TRUE;
    }

    TRACE("structversion=%u.%u, ",
            HIWORD(vffi->dwStrucVersion),LOWORD(vffi->dwStrucVersion));
    if(versioned_printer)
    {
        WORD mode = LOWORD(vffi->dwFileVersionMS);
        WORD ver_rev = HIWORD(vffi->dwFileVersionLS);
        TRACE("fileversion=%lu.%u.%u.%u (%s.major.minor.release), ",
            (vffi->dwFileVersionMS),
            HIBYTE(ver_rev), LOBYTE(ver_rev), LOWORD(vffi->dwFileVersionLS),
            (mode == 3) ? "Usermode" : ((mode <= 2) ? "Kernelmode" : "?") );
    }
    else
    {
        TRACE("fileversion=%u.%u.%u.%u, ",
            HIWORD(vffi->dwFileVersionMS),LOWORD(vffi->dwFileVersionMS),
            HIWORD(vffi->dwFileVersionLS),LOWORD(vffi->dwFileVersionLS));
    }
    TRACE("productversion=%u.%u.%u.%u\n",
          HIWORD(vffi->dwProductVersionMS),LOWORD(vffi->dwProductVersionMS),
          HIWORD(vffi->dwProductVersionLS),LOWORD(vffi->dwProductVersionLS));

    TRACE("flagmask=0x%lx, flags=0x%lx %s%s%s%s%s%s\n",
          vffi->dwFileFlagsMask, vffi->dwFileFlags,
          (vffi->dwFileFlags & VS_FF_DEBUG) ? "DEBUG," : "",
          (vffi->dwFileFlags & VS_FF_PRERELEASE) ? "PRERELEASE," : "",
          (vffi->dwFileFlags & VS_FF_PATCHED) ? "PATCHED," : "",
          (vffi->dwFileFlags & VS_FF_PRIVATEBUILD) ? "PRIVATEBUILD," : "",
          (vffi->dwFileFlags & VS_FF_INFOINFERRED) ? "INFOINFERRED," : "",
          (vffi->dwFileFlags & VS_FF_SPECIALBUILD) ? "SPECIALBUILD," : "");

    TRACE("(");

    TRACE("OS=0x%x.0x%x ", HIWORD(vffi->dwFileOS), LOWORD(vffi->dwFileOS));

    switch (vffi->dwFileOS&0xFFFF0000)
    {
    case VOS_DOS:TRACE("DOS,");break;
    case VOS_OS216:TRACE("OS/2-16,");break;
    case VOS_OS232:TRACE("OS/2-32,");break;
    case VOS_NT:TRACE("NT,");break;
    case VOS_UNKNOWN:
    default:
        TRACE("UNKNOWN(0x%lx),",vffi->dwFileOS&0xFFFF0000);break;
    }

    switch (LOWORD(vffi->dwFileOS))
    {
    case VOS__BASE:TRACE("BASE");break;
    case VOS__WINDOWS16:TRACE("WIN16");break;
    case VOS__WINDOWS32:TRACE("WIN32");break;
    case VOS__PM16:TRACE("PM16");break;
    case VOS__PM32:TRACE("PM32");break;
    default:
        TRACE("UNKNOWN(0x%x)",LOWORD(vffi->dwFileOS));break;
    }

    TRACE(")\n");

    switch (vffi->dwFileType)
    {
    case VFT_APP:TRACE("filetype=APP");break;
    case VFT_DLL:
        TRACE("filetype=DLL");
        if(vffi->dwFileSubtype != 0)
        {
            if(versioned_printer) /* NT3.x/NT4.0 or old w2k Driver  */
                TRACE(",PRINTER");
            TRACE(" (subtype=0x%lx)", vffi->dwFileSubtype);
        }
        break;
    case VFT_DRV:
        TRACE("filetype=DRV,");
        switch(vffi->dwFileSubtype)
        {
        case VFT2_DRV_PRINTER:TRACE("PRINTER");break;
        case VFT2_DRV_KEYBOARD:TRACE("KEYBOARD");break;
        case VFT2_DRV_LANGUAGE:TRACE("LANGUAGE");break;
        case VFT2_DRV_DISPLAY:TRACE("DISPLAY");break;
        case VFT2_DRV_MOUSE:TRACE("MOUSE");break;
        case VFT2_DRV_NETWORK:TRACE("NETWORK");break;
        case VFT2_DRV_SYSTEM:TRACE("SYSTEM");break;
        case VFT2_DRV_INSTALLABLE:TRACE("INSTALLABLE");break;
        case VFT2_DRV_SOUND:TRACE("SOUND");break;
        case VFT2_DRV_COMM:TRACE("COMM");break;
        case VFT2_DRV_INPUTMETHOD:TRACE("INPUTMETHOD");break;
        case VFT2_DRV_VERSIONED_PRINTER:TRACE("VERSIONED_PRINTER");break;
        case VFT2_UNKNOWN:
        default:
            TRACE("UNKNOWN(0x%lx)",vffi->dwFileSubtype);break;
        }
        break;
    case VFT_FONT:
        TRACE("filetype=FONT,");
        switch (vffi->dwFileSubtype)
        {
        case VFT2_FONT_RASTER:TRACE("RASTER");break;
        case VFT2_FONT_VECTOR:TRACE("VECTOR");break;
        case VFT2_FONT_TRUETYPE:TRACE("TRUETYPE");break;
        default:TRACE("UNKNOWN(0x%lx)",vffi->dwFileSubtype);break;
        }
        break;
    case VFT_VXD:TRACE("filetype=VXD");break;
    case VFT_STATIC_LIB:TRACE("filetype=STATIC_LIB");break;
    case VFT_UNKNOWN:
    default:
        TRACE("filetype=Unknown(0x%lx)",vffi->dwFileType);break;
    }

    TRACE("\n");
    TRACE("filedate=0x%lx.0x%lx\n",vffi->dwFileDateMS,vffi->dwFileDateLS);
}

/***********************************************************************
 *           GetFileVersionInfoSizeW         (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeW( LPCWSTR filename, LPDWORD handle )
{
    return GetFileVersionInfoSizeExW( FILE_VER_GET_LOCALISED, filename, handle );
}

/***********************************************************************
 *           GetFileVersionInfoSizeA         (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeA( LPCSTR filename, LPDWORD handle )
{
    return GetFileVersionInfoSizeExA( FILE_VER_GET_LOCALISED, filename, handle );
}

/******************************************************************************
 *           GetFileVersionInfoSizeExW       (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeExW( DWORD flags, LPCWSTR filename, LPDWORD ret_handle )
{
    DWORD len, offset, magic = 1;
    HMODULE hModule;

    TRACE("(0x%lx,%s,%p)\n", flags, debugstr_w(filename), ret_handle );

    if (ret_handle) *ret_handle = 0;

    if (!filename)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!*filename)
    {
        SetLastError(ERROR_BAD_PATHNAME);
        return 0;
    }
    if (flags & ~FILE_VER_GET_LOCALISED)
        FIXME("flags 0x%lx ignored\n", flags & ~FILE_VER_GET_LOCALISED);

    if ((hModule = LoadLibraryExW( filename, 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE )))
    {
        HRSRC hRsrc = NULL;
        if (!(flags & FILE_VER_GET_LOCALISED))
        {
            LANGID english = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
            hRsrc = FindResourceExW( hModule, (LPWSTR)VS_FILE_INFO,
                                     MAKEINTRESOURCEW(VS_VERSION_INFO), english );
        }
        if (!hRsrc)
            hRsrc = FindResourceW( hModule, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                   (LPWSTR)VS_FILE_INFO );
        if (hRsrc)
        {
            magic = IMAGE_NT_SIGNATURE;
            len = SizeofResource( hModule, hRsrc );
        }
        FreeLibrary( hModule );
    }
    else
    {
        HANDLE handle = CreateFileW( filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) return 0;
        magic = find_version_resource( handle, &len, &offset, flags );
        CloseHandle( handle );
    }

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        /* We have a 16bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is more than the actual needed space:
         *
         * (info->wLength - sizeof(VS_FIXEDFILEINFO)) * 4
         *
         * This extra buffer is used for ANSI to Unicode conversions in W-Calls.
         * info->wLength should be the same as len. Currently it isn't but that
         * doesn't seem to be a problem (len is bigger than info->wLength).
         */
        SetLastError(0);
        return (len - sizeof(VS_FIXEDFILEINFO)) * 4;

    case IMAGE_NT_SIGNATURE:
        /* We have a 32bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is 2 times the actual needed space + 4 bytes "FE2X"
         * This extra buffer is used for Unicode to ANSI conversions in A-Calls
         */
        SetLastError(0);
        return (len * 2) + 4;

    default:
        if (GetVersion() & 0x80000000) /* Windows 95/98 */
            SetLastError(ERROR_FILE_NOT_FOUND);
        else
            SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
}

/******************************************************************************
 *           GetFileVersionInfoSizeExA       (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeExA( DWORD flags, LPCSTR filename, LPDWORD handle )
{
    UNICODE_STRING filenameW;
    DWORD retval;

    TRACE("(0x%lx,%s,%p)\n", flags, debugstr_a(filename), handle );

    if(filename)
        RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else
        filenameW.Buffer = NULL;

    retval = GetFileVersionInfoSizeExW(flags, filenameW.Buffer, handle);

    RtlFreeUnicodeString(&filenameW);

    return retval;
}

/***********************************************************************
 *           GetFileVersionInfoExW           (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoExW( DWORD flags, LPCWSTR filename, DWORD ignored, DWORD datasize, LPVOID data )
{
    static const char signature[4] = "FE2X";
    DWORD len, offset, magic = 1;
    HMODULE hModule;
    VS_VERSION_INFO_STRUCT32* vvis = data;

    TRACE("(0x%lx,%s,%ld,size=%ld,data=%p)\n",
          flags, debugstr_w(filename), ignored, datasize, data );

    if (!data)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (flags & ~FILE_VER_GET_LOCALISED)
        FIXME("flags 0x%lx ignored\n", flags & ~FILE_VER_GET_LOCALISED);

    if ((hModule = LoadLibraryExW( filename, 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE )))
    {
        HRSRC hRsrc = NULL;
        if (!(flags & FILE_VER_GET_LOCALISED))
        {
            LANGID english = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
            hRsrc = FindResourceExW( hModule, (LPWSTR)VS_FILE_INFO,
                                     MAKEINTRESOURCEW(VS_VERSION_INFO), english );
        }
        if (!hRsrc)
            hRsrc = FindResourceW( hModule, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                   (LPWSTR)VS_FILE_INFO );
        if (hRsrc)
        {
            HGLOBAL hMem = LoadResource( hModule, hRsrc );
            magic = IMAGE_NT_SIGNATURE;
            len = min( SizeofResource(hModule, hRsrc), datasize );
            memcpy( data, LockResource( hMem ), len );
            FreeResource( hMem );
        }
        FreeLibrary( hModule );
    }
    else
    {
        HANDLE handle = CreateFileW( filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) return 0;
        if ((magic = find_version_resource( handle, &len, &offset, flags )))
            len = read_data( handle, offset, data, min( len, datasize ));
        CloseHandle( handle );
    }

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        /* We have a 16bit resource. */
        if (TRACE_ON(ver))
            print_vffi_debug( (VS_FIXEDFILEINFO *)VersionInfo16_Value( (VS_VERSION_INFO_STRUCT16 *)data ));
        SetLastError(0);
        return TRUE;

    case IMAGE_NT_SIGNATURE:
        /* We have a 32bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is 2 times the actual needed space + 4 bytes "FE2X"
         * This extra buffer is used for Unicode to ANSI conversions in A-Calls
         */
        len = vvis->wLength + sizeof(signature);
        if (datasize >= len) memcpy( (char*)data + vvis->wLength, signature, sizeof(signature) );
        if (TRACE_ON(ver))
            print_vffi_debug( (VS_FIXEDFILEINFO *)VersionInfo32_Value( vvis ));
        SetLastError(0);
        return TRUE;

    default:
        SetLastError( ERROR_RESOURCE_DATA_NOT_FOUND );
        return FALSE;
    }
}

/***********************************************************************
 *           GetFileVersionInfoExA           (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoExA( DWORD flags, LPCSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    UNICODE_STRING filenameW;
    BOOL retval;

    TRACE("(0x%lx,%s,%ld,size=%ld,data=%p)\n",
          flags, debugstr_a(filename), handle, datasize, data );

    if(filename)
        RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else
        filenameW.Buffer = NULL;

    retval = GetFileVersionInfoExW(flags, filenameW.Buffer, handle, datasize, data);

    RtlFreeUnicodeString(&filenameW);

    return retval;
}

/***********************************************************************
 *           GetFileVersionInfoW             (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoW( LPCWSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    return GetFileVersionInfoExW(FILE_VER_GET_LOCALISED, filename, handle, datasize, data);
}

/***********************************************************************
 *           GetFileVersionInfoA             (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoA( LPCSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    return GetFileVersionInfoExA(FILE_VER_GET_LOCALISED, filename, handle, datasize, data);
}

/***********************************************************************
 *           VersionInfo16_FindChild             [internal]
 */
static const VS_VERSION_INFO_STRUCT16 *VersionInfo16_FindChild( const VS_VERSION_INFO_STRUCT16 *info,
                                                                LPCSTR key, UINT len )
{
    const VS_VERSION_INFO_STRUCT16 *child = VersionInfo16_Children( info );

    while ((char *)child < (char *)info + info->wLength )
    {
        if (!strnicmp( child->szKey, key, len ) && !child->szKey[len])
            return child;

        if (!(child->wLength)) return NULL;
        child = VersionInfo16_Next( child );
    }

    return NULL;
}

/***********************************************************************
 *           VersionInfo32_FindChild             [internal]
 */
static const VS_VERSION_INFO_STRUCT32 *VersionInfo32_FindChild( const VS_VERSION_INFO_STRUCT32 *info,
                                                                LPCWSTR key, UINT len )
{
    const VS_VERSION_INFO_STRUCT32 *child = VersionInfo32_Children( info );

    while ((char *)child < (char *)info + info->wLength )
    {
        if (!wcsnicmp( child->szKey, key, len ) && !child->szKey[len])
            return child;

        if (!(child->wLength)) return NULL;
        child = VersionInfo32_Next( child );
    }

    return NULL;
}

/***********************************************************************
 *           VersionInfo16_QueryValue              [internal]
 *
 *    Gets a value from a 16-bit NE resource
 */
static BOOL VersionInfo16_QueryValue( const VS_VERSION_INFO_STRUCT16 *info, LPCSTR lpSubBlock,
                               LPVOID *lplpBuffer, UINT *puLen )
{
    while ( *lpSubBlock )
    {
        /* Find next path component */
        LPCSTR lpNextSlash;
        for ( lpNextSlash = lpSubBlock; *lpNextSlash; lpNextSlash++ )
            if ( *lpNextSlash == '\\' )
                break;

        /* Skip empty components */
        if ( lpNextSlash == lpSubBlock )
        {
            lpSubBlock++;
            continue;
        }

        /* We have a non-empty component: search info for key */
        info = VersionInfo16_FindChild( info, lpSubBlock, lpNextSlash-lpSubBlock );
        if ( !info )
        {
            if (puLen) *puLen = 0 ;
            SetLastError( ERROR_RESOURCE_TYPE_NOT_FOUND );
            return FALSE;
        }

        /* Skip path component */
        lpSubBlock = lpNextSlash;
    }

    /* Return value */
    *lplpBuffer = VersionInfo16_Value( info );
    if (puLen)
        *puLen = info->wValueLength;

    return TRUE;
}

/***********************************************************************
 *           VersionInfo32_QueryValue              [internal]
 *
 *    Gets a value from a 32-bit PE resource
 */
static BOOL VersionInfo32_QueryValue( const VS_VERSION_INFO_STRUCT32 *info, LPCWSTR lpSubBlock,
                                      LPVOID *lplpBuffer, UINT *puLen, BOOL *pbText )
{
    PVOID ptr;
    TRACE("lpSubBlock : (%s)\n", debugstr_w(lpSubBlock));

    while ( *lpSubBlock )
    {
        /* Find next path component */
        LPCWSTR lpNextSlash;
        for ( lpNextSlash = lpSubBlock; *lpNextSlash; lpNextSlash++ )
            if ( *lpNextSlash == '\\' )
                break;

        /* Skip empty components */
        if ( lpNextSlash == lpSubBlock )
        {
            lpSubBlock++;
            continue;
        }

        /* We have a non-empty component: search info for key */
        info = VersionInfo32_FindChild( info, lpSubBlock, lpNextSlash-lpSubBlock );
        if ( !info )
        {
            if (puLen) *puLen = 0 ;
            SetLastError( ERROR_RESOURCE_TYPE_NOT_FOUND );
            return FALSE;
        }

        /* Skip path component */
        lpSubBlock = lpNextSlash;
    }

    /* Return value */
    ptr = VersionInfo32_Value(info);
    if ((PBYTE)ptr >= ((PBYTE)info + info->wLength))  /* empty value */
        ptr = (WCHAR*)info->szKey + wcslen(info->szKey);

    *lplpBuffer = ptr;
    if (puLen)
        *puLen = info->wValueLength;
    if (pbText)
        *pbText = info->wType;

    return TRUE;
}

/***********************************************************************
 *           VerQueryValueA              (kernelbase.@)
 */
BOOL WINAPI VerQueryValueA( LPCVOID pBlock, LPCSTR lpSubBlock,
                               LPVOID *lplpBuffer, PUINT puLen )
{
    static const char rootA[] = "\\";
    const VS_VERSION_INFO_STRUCT16 *info = pBlock;

    TRACE("(%p,%s,%p,%p)\n",
                pBlock, debugstr_a(lpSubBlock), lplpBuffer, puLen );

     if (!pBlock)
        return FALSE;

    if (lpSubBlock == NULL || lpSubBlock[0] == '\0')
        lpSubBlock = rootA;

    if ( !VersionInfoIs16( info ) )
    {
        BOOL ret, isText;
        INT len;
        LPWSTR lpSubBlockW;
        UINT value_len;

        len  = MultiByteToWideChar(CP_ACP, 0, lpSubBlock, -1, NULL, 0);
        lpSubBlockW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));

        if (!lpSubBlockW)
            return FALSE;

        MultiByteToWideChar(CP_ACP, 0, lpSubBlock, -1, lpSubBlockW, len);

        ret = VersionInfo32_QueryValue(pBlock, lpSubBlockW, lplpBuffer, &value_len, &isText);
        if (puLen) *puLen = value_len;

        HeapFree(GetProcessHeap(), 0, lpSubBlockW);

        if (ret && isText)
        {
            /* Set lpBuffer so it points to the 'empty' area where we store
             * the converted strings
             */
            LPSTR lpBufferA = (LPSTR)pBlock + info->wLength + 4;
            DWORD pos = (LPCSTR)*lplpBuffer - (LPCSTR)pBlock;
            len = WideCharToMultiByte(CP_ACP, 0, *lplpBuffer, value_len,
                                      lpBufferA + pos, info->wLength - pos, NULL, NULL);
            *lplpBuffer = lpBufferA + pos;
            if (puLen) *puLen = len;
        }
        return ret;
    }

    return VersionInfo16_QueryValue(info, lpSubBlock, lplpBuffer, puLen);
}

/***********************************************************************
 *           VerQueryValueW              (kernelbase.@)
 */
BOOL WINAPI VerQueryValueW( LPCVOID pBlock, LPCWSTR lpSubBlock,
                               LPVOID *lplpBuffer, PUINT puLen )
{
    const VS_VERSION_INFO_STRUCT32 *info = pBlock;

    TRACE("(%p,%s,%p,%p)\n",
                pBlock, debugstr_w(lpSubBlock), lplpBuffer, puLen );

    if (!pBlock)
        return FALSE;

    if (!lpSubBlock || !lpSubBlock[0])
        lpSubBlock = L"\\";

    if ( VersionInfoIs16( info ) )
    {
        BOOL ret;
        int len;
        LPSTR lpSubBlockA;

        len = WideCharToMultiByte(CP_ACP, 0, lpSubBlock, -1, NULL, 0, NULL, NULL);
        lpSubBlockA = HeapAlloc(GetProcessHeap(), 0, len * sizeof(char));

        if (!lpSubBlockA)
            return FALSE;

        WideCharToMultiByte(CP_ACP, 0, lpSubBlock, -1, lpSubBlockA, len, NULL, NULL);

        ret = VersionInfo16_QueryValue(pBlock, lpSubBlockA, lplpBuffer, puLen);

        HeapFree(GetProcessHeap(), 0, lpSubBlockA);

        if (ret && wcscmp( lpSubBlock, L"\\" ) && wcsicmp( lpSubBlock, L"\\VarFileInfo\\Translation" ))
        {
            /* Set lpBuffer so it points to the 'empty' area where we store
             * the converted strings
             */
            LPWSTR lpBufferW = (LPWSTR)((LPSTR)pBlock + info->wLength);
            DWORD pos = (LPCSTR)*lplpBuffer - (LPCSTR)pBlock;
            DWORD max = (info->wLength - sizeof(VS_FIXEDFILEINFO)) * 4 - info->wLength;

            len = MultiByteToWideChar(CP_ACP, 0, *lplpBuffer, -1,
                                      lpBufferW + pos, max/sizeof(WCHAR) - pos );
            *lplpBuffer = lpBufferW + pos;
            if (puLen) *puLen = len;
        }
        return ret;
    }

    return VersionInfo32_QueryValue(info, lpSubBlock, lplpBuffer, puLen, NULL);
}


/******************************************************************************
 *   file_existsA
 */
static BOOL file_existsA( char const * path, char const * file, BOOL excl )
{
    DWORD sharing = excl ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE;
    char filename[MAX_PATH];
    int len;
    HANDLE handle;

    if (path)
    {
        strcpy( filename, path );
        len = strlen(filename);
        if (len && filename[len - 1] != '\\') strcat( filename, "\\" );
        strcat( filename, file );
    }
    else if (!SearchPathA( NULL, file, NULL, MAX_PATH, filename, NULL )) return FALSE;

    handle = CreateFileA( filename, 0, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( handle );
    return TRUE;
}

/******************************************************************************
 *   file_existsW
 */
static BOOL file_existsW( const WCHAR *path, const WCHAR *file, BOOL excl )
{
    DWORD sharing = excl ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE;
    WCHAR filename[MAX_PATH];
    int len;
    HANDLE handle;

    if (path)
    {
        lstrcpyW( filename, path );
        len = lstrlenW(filename);
        if (len && filename[len - 1] != '\\') lstrcatW( filename, L"\\" );
        lstrcatW( filename, file );
    }
    else if (!SearchPathW( NULL, file, NULL, MAX_PATH, filename, NULL )) return FALSE;

    handle = CreateFileW( filename, 0, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( handle );
    return TRUE;
}

/*****************************************************************************
 *   VerFindFileA (kernelbase.@)
 *
 *   Determines where to install a file based on whether it locates another
 *   version of the file in the system.  The values VerFindFile returns are
 *   used in a subsequent call to the VerInstallFile function.
 */
DWORD WINAPI VerFindFileA( DWORD flags, LPCSTR filename, LPCSTR win_dir, LPCSTR app_dir,
                           LPSTR cur_dir, PUINT curdir_len, LPSTR dest, PUINT dest_len )
{
    DWORD  retval = 0;
    const char *curDir;
    const char *destDir;
    char winDir[MAX_PATH], systemDir[MAX_PATH];

    TRACE("flags = %lx filename=%s windir=%s appdir=%s curdirlen=%p(%u) destdirlen=%p(%u)\n",
          flags, debugstr_a(filename), debugstr_a(win_dir), debugstr_a(app_dir),
          curdir_len, curdir_len ? *curdir_len : 0, dest_len, dest_len ? *dest_len : 0 );

    /* Figure out where the file should go; shared files default to the
       system directory */

    GetSystemDirectoryA(systemDir, sizeof(systemDir));
    curDir = "";

    if(flags & VFFF_ISSHAREDFILE)
    {
        destDir = systemDir;
        /* Were we given a filename?  If so, try to find the file. */
        if(filename)
        {
            if(file_existsA(destDir, filename, FALSE)) curDir = destDir;
            else if(app_dir && file_existsA(app_dir, filename, FALSE))
                curDir = app_dir;

            if(!file_existsA(systemDir, filename, FALSE))
                retval |= VFF_CURNEDEST;
        }
    }
    else /* not a shared file */
    {
        destDir = app_dir ? app_dir : "";
        if(filename)
        {
            GetWindowsDirectoryA( winDir, MAX_PATH );
            if(file_existsA(destDir, filename, FALSE)) curDir = destDir;
            else if(file_existsA(winDir, filename, FALSE))
                curDir = winDir;
            else if(file_existsA(systemDir, filename, FALSE))
                curDir = systemDir;

            if (app_dir && app_dir[0])
            {
                if(!file_existsA(app_dir, filename, FALSE))
                    retval |= VFF_CURNEDEST;
            }
            else if(file_existsA(NULL, filename, FALSE))
                retval |= VFF_CURNEDEST;
        }
    }

    /* Check to see if the file exists and is in use by another application */
    if (filename && file_existsA(curDir, filename, FALSE))
    {
        if (filename && !file_existsA(curDir, filename, TRUE))
           retval |= VFF_FILEINUSE;
    }

    if (dest_len && dest)
    {
        UINT len = strlen(destDir) + 1;
        if (*dest_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynA(dest, destDir, *dest_len);
        *dest_len = len;
    }
    if (curdir_len && cur_dir)
    {
        UINT len = strlen(curDir) + 1;
        if (*curdir_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynA(cur_dir, curDir, *curdir_len);
        *curdir_len = len;
    }

    TRACE("ret = %lu (%s%s%s) curdir=%s destdir=%s\n", retval,
          (retval & VFF_CURNEDEST) ? "VFF_CURNEDEST " : "",
          (retval & VFF_FILEINUSE) ? "VFF_FILEINUSE " : "",
          (retval & VFF_BUFFTOOSMALL) ? "VFF_BUFFTOOSMALL " : "",
          debugstr_a(cur_dir), debugstr_a(dest));

    return retval;
}

/*****************************************************************************
 * VerFindFileW (kernelbase.@)
 */
DWORD WINAPI VerFindFileW( DWORD flags, LPCWSTR filename, LPCWSTR win_dir, LPCWSTR app_dir,
                           LPWSTR cur_dir, PUINT curdir_len, LPWSTR dest, PUINT dest_len )
{
    DWORD retval = 0;
    const WCHAR *curDir;
    const WCHAR *destDir;

    TRACE("flags = %lx filename=%s windir=%s appdir=%s curdirlen=%p(%u) destdirlen=%p(%u)\n",
          flags, debugstr_w(filename), debugstr_w(win_dir), debugstr_w(app_dir),
          curdir_len, curdir_len ? *curdir_len : 0, dest_len, dest_len ? *dest_len : 0 );

    /* Figure out where the file should go; shared files default to the
       system directory */

    curDir = L"";

    if(flags & VFFF_ISSHAREDFILE)
    {
        destDir = system_dir;
        /* Were we given a filename?  If so, try to find the file. */
        if(filename)
        {
            if(file_existsW(destDir, filename, FALSE)) curDir = destDir;
            else if(app_dir && file_existsW(app_dir, filename, FALSE))
            {
                curDir = app_dir;
                retval |= VFF_CURNEDEST;
            }
        }
    }
    else /* not a shared file */
    {
        destDir = app_dir ? app_dir : L"";
        if(filename)
        {
            if(file_existsW(destDir, filename, FALSE)) curDir = destDir;
            else if(file_existsW(windows_dir, filename, FALSE))
            {
                curDir = windows_dir;
                retval |= VFF_CURNEDEST;
            }
            else if (file_existsW(system_dir, filename, FALSE))
            {
                curDir = system_dir;
                retval |= VFF_CURNEDEST;
            }
        }
    }

    if (filename && !file_existsW(curDir, filename, TRUE))
        retval |= VFF_FILEINUSE;

    if (dest_len && dest)
    {
        UINT len = lstrlenW(destDir) + 1;
        if (*dest_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynW(dest, destDir, *dest_len);
        *dest_len = len;
    }
    if (curdir_len && cur_dir)
    {
        UINT len = lstrlenW(curDir) + 1;
        if (*curdir_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynW(cur_dir, curDir, *curdir_len);
        *curdir_len = len;
    }

    TRACE("ret = %lu (%s%s%s) curdir=%s destdir=%s\n", retval,
          (retval & VFF_CURNEDEST) ? "VFF_CURNEDEST " : "",
          (retval & VFF_FILEINUSE) ? "VFF_FILEINUSE " : "",
          (retval & VFF_BUFFTOOSMALL) ? "VFF_BUFFTOOSMALL " : "",
          debugstr_w(cur_dir), debugstr_w(dest));
    return retval;
}


/***********************************************************************
 *         GetProductInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetProductInfo( DWORD os_major, DWORD os_minor,
                                              DWORD sp_major, DWORD sp_minor, DWORD *type )
{
    return RtlGetProductInfo( os_major, os_minor, sp_major, sp_minor, type );
}


/***********************************************************************
 *         GetVersion   (kernelbase.@)
 */
DWORD WINAPI GetVersion(void)
{
    OSVERSIONINFOEXW info;
    DWORD result;

    info.dwOSVersionInfoSize = sizeof(info);
    if (!GetVersionExW( (OSVERSIONINFOW *)&info )) return 0;

    result = MAKELONG( MAKEWORD( info.dwMajorVersion, info.dwMinorVersion ),
                       (info.dwPlatformId ^ 2) << 14 );

    if (info.dwPlatformId == VER_PLATFORM_WIN32_NT)
        result |= LOWORD(info.dwBuildNumber) << 16;
    return result;
}


/***********************************************************************
 *         GetVersionExA   (kernelbase.@)
 */
BOOL WINAPI GetVersionExA( OSVERSIONINFOA *info )
{
    OSVERSIONINFOEXW infoW;

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA))
    {
        WARN( "wrong OSVERSIONINFO size from app (got: %ld)\n", info->dwOSVersionInfoSize );
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return FALSE;
    }

    infoW.dwOSVersionInfoSize = sizeof(infoW);
    if (!GetVersionExW( (OSVERSIONINFOW *)&infoW )) return FALSE;

    info->dwMajorVersion = infoW.dwMajorVersion;
    info->dwMinorVersion = infoW.dwMinorVersion;
    info->dwBuildNumber  = infoW.dwBuildNumber;
    info->dwPlatformId   = infoW.dwPlatformId;
    WideCharToMultiByte( CP_ACP, 0, infoW.szCSDVersion, -1,
                         info->szCSDVersion, sizeof(info->szCSDVersion), NULL, NULL );

    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
    {
        OSVERSIONINFOEXA *vex = (OSVERSIONINFOEXA *)info;
        vex->wServicePackMajor = infoW.wServicePackMajor;
        vex->wServicePackMinor = infoW.wServicePackMinor;
        vex->wSuiteMask        = infoW.wSuiteMask;
        vex->wProductType      = infoW.wProductType;
    }
    return TRUE;
}


/***********************************************************************
 *         GetVersionExW   (kernelbase.@)
 */
BOOL WINAPI GetVersionExW( OSVERSIONINFOW *info )
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))
    {
        WARN( "wrong OSVERSIONINFO size from app (got: %ld)\n", info->dwOSVersionInfoSize );
        return FALSE;
    }

    if (!InitOnceExecuteOnce(&init_once, init_current_version, NULL, NULL)) return FALSE;

    info->dwMajorVersion = current_version.dwMajorVersion;
    info->dwMinorVersion = current_version.dwMinorVersion;
    info->dwBuildNumber  = current_version.dwBuildNumber;
    info->dwPlatformId   = current_version.dwPlatformId;
    wcscpy( info->szCSDVersion, current_version.szCSDVersion );

    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW))
    {
        OSVERSIONINFOEXW *vex = (OSVERSIONINFOEXW *)info;
        vex->wServicePackMajor = current_version.wServicePackMajor;
        vex->wServicePackMinor = current_version.wServicePackMinor;
        vex->wSuiteMask        = current_version.wSuiteMask;
        vex->wProductType      = current_version.wProductType;
    }
    return TRUE;
}

/*
 * PackageDependencyData is a pointer to an immutable, pointer-free package
 * graph installed by ntdll during process startup.  Keep the wire offsets in
 * one block until the shared graph header grows named field accessors.
 */
enum appx_graph_wire_offset
{
    GRAPH_HEADER_VERSION_OFFSET             = 8,
    GRAPH_HEADER_PACKAGE_COUNT_OFFSET       = 44,
    GRAPH_HEADER_PACKAGES_OFFSET            = 48,
    GRAPH_HEADER_AUMID_REF_OFFSET           = 80,

    GRAPH_PACKAGE_VERSION_OFFSET            = 0,
    GRAPH_PACKAGE_ARCHITECTURE_OFFSET       = 8,
    GRAPH_PACKAGE_FLAGS_OFFSET              = 12,
    GRAPH_PACKAGE_NAME_REF_OFFSET           = 56,
    GRAPH_PACKAGE_PUBLISHER_REF_OFFSET      = 64,
    GRAPH_PACKAGE_RESOURCE_ID_REF_OFFSET    = 72,
    GRAPH_PACKAGE_PUBLISHER_ID_REF_OFFSET   = 80,
    GRAPH_PACKAGE_FULL_NAME_REF_OFFSET      = 88,
    GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET    = 96,
    GRAPH_PACKAGE_ROOT_REF_OFFSET           = 104,
};

#define GRAPH_PACKAGE_ACTIVE                  0x00000001
#define GRAPH_PACKAGE_FRAMEWORK               0x00000002
#define GRAPH_PACKAGE_RESOURCE                0x00000004
#define GRAPH_PACKAGE_SIGNED                  0x00000008
#define GRAPH_PACKAGE_DIRECT                  0x00000010

#define PACKAGE_FILTER_HEAD                   0x00000010
#define PACKAGE_FILTER_DIRECT                 0x00000020
#define PACKAGE_FILTER_RESOURCE               0x00000040
#define PACKAGE_FILTER_BUNDLE                 0x00000080
#define PACKAGE_FILTER_OPTIONAL               0x00020000
#define PACKAGE_FILTER_IS_IN_RELATED_SET      0x00040000
#define PACKAGE_FILTER_STATIC                 0x00080000
#define PACKAGE_FILTER_DYNAMIC                0x00100000
#define PACKAGE_FILTER_HOSTRUNTIME            0x00200000
#define PACKAGE_FILTER_KNOWN                  \
    (PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT | PACKAGE_FILTER_RESOURCE | \
     PACKAGE_FILTER_BUNDLE | PACKAGE_FILTER_OPTIONAL | \
     PACKAGE_FILTER_IS_IN_RELATED_SET | PACKAGE_FILTER_STATIC | \
     PACKAGE_FILTER_DYNAMIC | PACKAGE_FILTER_HOSTRUNTIME)

#define PACKAGE_PROPERTY_FRAMEWORK            0x00000001
#define PACKAGE_PROPERTY_RESOURCE             0x00000002
#define PACKAGE_PROPERTY_STATIC               0x00080000

struct current_package_graph
{
    const BYTE *data;
    UINT32 size;
    UINT32 version;
    UINT32 package_count;
    UINT32 packages_offset;
};

struct current_package
{
    struct wine_appx_graph_string_ref name;
    struct wine_appx_graph_string_ref publisher;
    struct wine_appx_graph_string_ref resource_id;
    struct wine_appx_graph_string_ref publisher_id;
    struct wine_appx_graph_string_ref full_name;
    struct wine_appx_graph_string_ref family_name;
    struct wine_appx_graph_string_ref root;
    UINT64 version;
    UINT32 architecture;
    UINT32 flags;
};

struct package_info_native
{
    UINT32 reserved;
    UINT32 flags;
    WCHAR *path;
    WCHAR *package_full_name;
    WCHAR *package_family_name;
    PACKAGE_ID package_id;
};

enum current_graph_status
{
    CURRENT_GRAPH_NONE,
    CURRENT_GRAPH_VALID,
    CURRENT_GRAPH_CORRUPT,
};

struct current_package_graph_cache
{
    const BYTE *data;
    struct current_package_graph graph;
};

static struct current_package_graph_cache *current_graph_cache;

static UINT64 appx_graph_read_u64( const BYTE *data )
{
    return wine_appx_graph_read_u32( data ) |
           ((UINT64)wine_appx_graph_read_u32( data + 4 ) << 32);
}

static BOOL appx_ref_length_between( struct wine_appx_graph_string_ref ref,
                                     UINT32 minimum, UINT32 maximum )
{
    UINT32 length = ref.chars - 1;
    return length >= minimum && length <= maximum;
}

static BOOL appx_validate_primary_identity( const struct current_package_graph *graph )
{
    const BYTE *record = graph->data + graph->packages_offset;
    struct wine_appx_graph_string_ref application_id, aumid, family;
    UINT32 family_length, application_length, i;

    application_id = wine_appx_graph_get_ref( graph->data, 72 );
    aumid = wine_appx_graph_get_ref( graph->data, GRAPH_HEADER_AUMID_REF_OFFSET );
    family = wine_appx_graph_get_ref( record, GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET );
    if (application_id.chars < PACKAGE_RELATIVE_APPLICATION_ID_MIN_LENGTH ||
        application_id.chars > PACKAGE_RELATIVE_APPLICATION_ID_MAX_LENGTH ||
        aumid.chars < APPLICATION_USER_MODEL_ID_MIN_LENGTH ||
        aumid.chars > APPLICATION_USER_MODEL_ID_MAX_LENGTH ||
        !appx_ref_length_between( family, PACKAGE_FAMILY_NAME_MIN_LENGTH,
                                 PACKAGE_FAMILY_NAME_MAX_LENGTH ))
        return FALSE;

    family_length = family.chars - 1;
    application_length = application_id.chars - 1;
    if (aumid.chars != family_length + 1 + application_length + 1)
        return FALSE;
    for (i = 0; i < family_length; i++)
        if (wine_appx_graph_read_u16( graph->data + family.offset + i * 2 ) !=
            wine_appx_graph_read_u16( graph->data + aumid.offset + i * 2 ))
            return FALSE;
    if (wine_appx_graph_read_u16( graph->data + aumid.offset + family_length * 2 ) != '!')
        return FALSE;
    for (i = 0; i < application_length; i++)
        if (wine_appx_graph_read_u16( graph->data + application_id.offset + i * 2 ) !=
            wine_appx_graph_read_u16( graph->data + aumid.offset +
                                      (family_length + 1 + i) * 2 ))
            return FALSE;
    return TRUE;
}

static enum current_graph_status validate_package_graph_data(
    const BYTE *data, struct current_package_graph *graph )
{
    enum current_graph_status status = CURRENT_GRAPH_CORRUPT;
    UINT32 size;

    if (!data) return CURRENT_GRAPH_NONE;
    __TRY
    {
        size = wine_appx_graph_read_u32(
            data + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET );
        if (wine_appx_graph_validate_blob( data, size ))
        {
            graph->data = data;
            graph->size = size;
            graph->version = wine_appx_graph_read_u32(
                data + GRAPH_HEADER_VERSION_OFFSET );
            graph->package_count = wine_appx_graph_read_u32(
                data + GRAPH_HEADER_PACKAGE_COUNT_OFFSET );
            graph->packages_offset = wine_appx_graph_read_u32(
                data + GRAPH_HEADER_PACKAGES_OFFSET );
            if (appx_validate_primary_identity( graph ))
                status = CURRENT_GRAPH_VALID;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        status = CURRENT_GRAPH_CORRUPT;
    }
    __ENDTRY
    return status;
}

static enum current_graph_status get_current_package_graph( struct current_package_graph *graph )
{
    const BYTE *data = NtCurrentTeb()->Peb->ProcessParameters->PackageDependencyData;
    struct current_package_graph_cache *cache, *new_cache;
    enum current_graph_status status;

    if (!data) return CURRENT_GRAPH_NONE;
    cache = InterlockedCompareExchangePointer( (void **)&current_graph_cache,
                                               NULL, NULL );
    if (cache && cache->data == data)
    {
        *graph = cache->graph;
        return CURRENT_GRAPH_VALID;
    }

    status = validate_package_graph_data( data, graph );
    if (status == CURRENT_GRAPH_VALID &&
        (new_cache = HeapAlloc( GetProcessHeap(), 0, sizeof(*new_cache) )))
    {
        new_cache->data = data;
        new_cache->graph = *graph;
        if (InterlockedCompareExchangePointer(
                (void **)&current_graph_cache, new_cache, NULL ))
            HeapFree( GetProcessHeap(), 0, new_cache );
    }
    return status;
}

static BOOL get_current_package( const struct current_package_graph *graph,
                                 UINT32 index, struct current_package *package )
{
    const BYTE *record;

    if (index >= graph->package_count) return FALSE;
    record = graph->data + graph->packages_offset +
             index * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    package->version = appx_graph_read_u64( record + GRAPH_PACKAGE_VERSION_OFFSET );
    package->architecture = wine_appx_graph_read_u32(
        record + GRAPH_PACKAGE_ARCHITECTURE_OFFSET );
    package->flags = wine_appx_graph_read_u32( record + GRAPH_PACKAGE_FLAGS_OFFSET );
    package->name = wine_appx_graph_get_ref( record, GRAPH_PACKAGE_NAME_REF_OFFSET );
    package->publisher = wine_appx_graph_get_ref(
        record, GRAPH_PACKAGE_PUBLISHER_REF_OFFSET );
    package->resource_id = wine_appx_graph_get_ref(
        record, GRAPH_PACKAGE_RESOURCE_ID_REF_OFFSET );
    package->publisher_id = wine_appx_graph_get_ref(
        record, GRAPH_PACKAGE_PUBLISHER_ID_REF_OFFSET );
    package->full_name = wine_appx_graph_get_ref(
        record, GRAPH_PACKAGE_FULL_NAME_REF_OFFSET );
    package->family_name = wine_appx_graph_get_ref(
        record, GRAPH_PACKAGE_FAMILY_NAME_REF_OFFSET );
    package->root = wine_appx_graph_get_ref( record, GRAPH_PACKAGE_ROOT_REF_OFFSET );

    return appx_ref_length_between( package->name, PACKAGE_NAME_MIN_LENGTH,
                                    PACKAGE_NAME_MAX_LENGTH ) &&
           appx_ref_length_between( package->publisher, PACKAGE_PUBLISHER_MIN_LENGTH,
                                    PACKAGE_PUBLISHER_MAX_LENGTH ) &&
           appx_ref_length_between( package->resource_id, PACKAGE_RESOURCEID_MIN_LENGTH,
                                    PACKAGE_RESOURCEID_MAX_LENGTH ) &&
           appx_ref_length_between( package->publisher_id,
                                    PACKAGE_PUBLISHERID_MIN_LENGTH,
                                    PACKAGE_PUBLISHERID_MAX_LENGTH ) &&
           appx_ref_length_between( package->full_name, PACKAGE_FULL_NAME_MIN_LENGTH,
                                    PACKAGE_FULL_NAME_MAX_LENGTH ) &&
           appx_ref_length_between( package->family_name, PACKAGE_FAMILY_NAME_MIN_LENGTH,
                                    PACKAGE_FAMILY_NAME_MAX_LENGTH ) &&
           package->architecture <= 5 &&
           !(package->flags & ~(GRAPH_PACKAGE_ACTIVE | GRAPH_PACKAGE_FRAMEWORK |
                                GRAPH_PACKAGE_RESOURCE | GRAPH_PACKAGE_SIGNED |
                                GRAPH_PACKAGE_DIRECT)) &&
           (package->flags & (GRAPH_PACKAGE_ACTIVE | GRAPH_PACKAGE_SIGNED)) ==
               (GRAPH_PACKAGE_ACTIVE | GRAPH_PACKAGE_SIGNED);
}

static LONG get_graph_or_error( struct current_package_graph *graph )
{
    switch (get_current_package_graph( graph ))
    {
    case CURRENT_GRAPH_NONE:
        return APPMODEL_ERROR_NO_PACKAGE;
    case CURRENT_GRAPH_CORRUPT:
        return APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT;
    default:
        return ERROR_SUCCESS;
    }
}

static LONG copy_graph_string( const struct current_package_graph *graph,
                               struct wine_appx_graph_string_ref ref,
                               UINT32 *length, WCHAR *buffer )
{
    UINT32 capacity;

    if (!length) return ERROR_INVALID_PARAMETER;
    capacity = *length;
    *length = ref.chars;
    if (!buffer || capacity < ref.chars) return ERROR_INSUFFICIENT_BUFFER;
    memcpy( buffer, graph->data + ref.offset, ref.chars * sizeof(WCHAR) );
    return ERROR_SUCCESS;
}

static UINT32 package_processor_architecture( UINT32 architecture )
{
    static const UINT32 architectures[] =
    {
        PROCESSOR_ARCHITECTURE_NEUTRAL,
        PROCESSOR_ARCHITECTURE_INTEL,
        PROCESSOR_ARCHITECTURE_AMD64,
        PROCESSOR_ARCHITECTURE_ARM,
        PROCESSOR_ARCHITECTURE_ARM64,
        PROCESSOR_ARCHITECTURE_IA32_ON_ARM64,
    };

    return architecture < ARRAY_SIZE(architectures) ?
           architectures[architecture] : PROCESSOR_ARCHITECTURE_UNKNOWN;
}

static BOOL add_package_size( UINT32 *size, UINT32 chars )
{
    UINT32 bytes;

    if (chars > MAXDWORD / sizeof(WCHAR)) return FALSE;
    bytes = chars * sizeof(WCHAR);
    if (*size > MAXDWORD - bytes) return FALSE;
    *size += bytes;
    return TRUE;
}

static WCHAR *copy_package_string( WCHAR *destination,
                                   const struct current_package_graph *graph,
                                   struct wine_appx_graph_string_ref ref )
{
    memcpy( destination, graph->data + ref.offset, ref.chars * sizeof(WCHAR) );
    return destination + ref.chars;
}

static void fill_package_id( PACKAGE_ID *id, WCHAR **strings,
                             const struct current_package_graph *graph,
                             const struct current_package *package )
{
    UINT64 version = package->version;

    memset( id, 0, sizeof(*id) );
    id->processorArchitecture = package_processor_architecture( package->architecture );
    id->version.Major = version;
    id->version.Minor = version >> 16;
    id->version.Build = version >> 32;
    id->version.Revision = version >> 48;
    id->name = *strings;
    *strings = copy_package_string( *strings, graph, package->name );
    id->publisher = *strings;
    *strings = copy_package_string( *strings, graph, package->publisher );
    id->resourceId = *strings;
    *strings = copy_package_string( *strings, graph, package->resource_id );
    id->publisherId = *strings;
    *strings = copy_package_string( *strings, graph, package->publisher_id );
}

/***********************************************************************
 *         GetCurrentApplicationUserModelId   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentApplicationUserModelId( UINT32 *length, WCHAR *id )
{
    struct current_package_graph graph;
    struct wine_appx_graph_string_ref aumid;
    enum current_graph_status status;

    status = get_current_package_graph( &graph );
    if (status == CURRENT_GRAPH_NONE) return APPMODEL_ERROR_NO_APPLICATION;
    if (status == CURRENT_GRAPH_CORRUPT)
        return APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT;
    aumid = wine_appx_graph_get_ref( graph.data, GRAPH_HEADER_AUMID_REF_OFFSET );
    return copy_graph_string( &graph, aumid, length, id );
}

/***********************************************************************
 *         GetCurrentPackageFamilyName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageFamilyName( UINT32 *length, WCHAR *name )
{
    struct current_package_graph graph;
    struct current_package package;
    LONG ret;

    if ((ret = get_graph_or_error( &graph ))) return ret;
    if (!get_current_package( &graph, 0, &package ))
        return APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
    return copy_graph_string( &graph, package.family_name, length, name );
}


/***********************************************************************
 *         GetCurrentPackageFullName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageFullName( UINT32 *length, WCHAR *name )
{
    struct current_package_graph graph;
    struct current_package package;
    LONG ret;

    if ((ret = get_graph_or_error( &graph ))) return ret;
    if (!get_current_package( &graph, 0, &package ))
        return APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
    return copy_graph_string( &graph, package.full_name, length, name );
}


/***********************************************************************
 *         GetCurrentPackageId   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageId( UINT32 *len, BYTE *buffer )
{
    struct current_package_graph graph;
    struct current_package package;
    UINT32 capacity, required = sizeof(PACKAGE_ID);
    PACKAGE_ID *id;
    WCHAR *strings;
    LONG ret;

    if ((ret = get_graph_or_error( &graph ))) return ret;
    if (!get_current_package( &graph, 0, &package ))
        return APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
    if (!len) return ERROR_INVALID_PARAMETER;
    if (!add_package_size( &required, package.name.chars ) ||
        !add_package_size( &required, package.publisher.chars ) ||
        !add_package_size( &required, package.resource_id.chars ) ||
        !add_package_size( &required, package.publisher_id.chars ))
        return ERROR_ARITHMETIC_OVERFLOW;

    capacity = *len;
    *len = required;
    if (!buffer || capacity < required) return ERROR_INSUFFICIENT_BUFFER;
    id = (PACKAGE_ID *)buffer;
    strings = (WCHAR *)(buffer + sizeof(*id));
    fill_package_id( id, &strings, &graph, &package );
    return ERROR_SUCCESS;
}

static LONG get_static_package_path_type_status( PackagePathType path_type )
{
    switch (path_type)
    {
    case PackagePathType_Install:
    case PackagePathType_Effective:
        return ERROR_SUCCESS;
    case PackagePathType_Mutable:
    case PackagePathType_MachineExternal:
    case PackagePathType_UserExternal:
    case PackagePathType_EffectiveExternal:
        return ERROR_NOT_FOUND;
    default:
        return ERROR_INVALID_PARAMETER;
    }
}

static LONG get_current_package_info( const UINT32 flags,
                                      PackagePathType path_type,
                                      UINT32 *buffer_size, BYTE *buffer,
                                      UINT32 *count )
{
    struct current_package packages[WINE_APPX_GRAPH_MAX_PACKAGES];
    struct current_package_graph graph;
    BYTE selected[WINE_APPX_GRAPH_MAX_PACKAGES];
    struct package_info_native *info;
    UINT32 capacity, required, selected_count = 0, type_filters, i;
    BOOL include_static;
    WCHAR *strings;
    LONG ret;

    if ((ret = get_graph_or_error( &graph ))) return ret;
    if (flags & ~PACKAGE_FILTER_KNOWN) return ERROR_INVALID_PARAMETER;
    if (!buffer_size) return ERROR_INVALID_PARAMETER;
    if ((ret = get_static_package_path_type_status( path_type ))) return ret;

    /*
     * GetCurrentPackageInfo is static-only unless DYNAMIC is explicitly
     * requested.  Unlike the zero-valued ALL_LOADED compatibility filter,
     * an explicit STATIC category with no type filter selects the complete
     * static graph.
     */
    include_static = !(flags & PACKAGE_FILTER_DYNAMIC) ||
                     (flags & PACKAGE_FILTER_STATIC);
    type_filters = flags & (PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT |
                            PACKAGE_FILTER_RESOURCE | PACKAGE_FILTER_BUNDLE |
                            PACKAGE_FILTER_OPTIONAL |
                            PACKAGE_FILTER_IS_IN_RELATED_SET |
                            PACKAGE_FILTER_HOSTRUNTIME);
    if (!flags) type_filters = PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT;

    /*
     * Graph v1 does not encode direct versus transitive dependencies.  Never
     * broaden a DIRECT query to all dependencies; fail closed until v2.
     */
    if (include_static && graph.version == 1 &&
        (type_filters & PACKAGE_FILTER_DIRECT))
        return ERROR_NOT_SUPPORTED;

    for (i = 0; i < graph.package_count; i++)
    {
        BOOL include = include_static;

        if (!get_current_package( &graph, i, &packages[i] ))
            return APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
        if (include && type_filters)
        {
            include = (!i && (type_filters & PACKAGE_FILTER_HEAD)) ||
                      (i && (type_filters & PACKAGE_FILTER_DIRECT) &&
                       (packages[i].flags & GRAPH_PACKAGE_DIRECT)) ||
                      ((type_filters & PACKAGE_FILTER_RESOURCE) &&
                       (packages[i].flags & GRAPH_PACKAGE_RESOURCE));
        }
        if (include) selected[selected_count++] = i;
    }

    required = selected_count * sizeof(*info);
    for (i = 0; i < selected_count; i++)
    {
        const struct current_package *package = &packages[selected[i]];

        if (!add_package_size( &required, package->root.chars ) ||
            !add_package_size( &required, package->full_name.chars ) ||
            !add_package_size( &required, package->family_name.chars ) ||
            !add_package_size( &required, package->name.chars ) ||
            !add_package_size( &required, package->publisher.chars ) ||
            !add_package_size( &required, package->resource_id.chars ) ||
            !add_package_size( &required, package->publisher_id.chars ))
            return ERROR_ARITHMETIC_OVERFLOW;
    }

    capacity = *buffer_size;
    *buffer_size = required;
    if (count) *count = selected_count;
    if (!required) return ERROR_SUCCESS;
    if (!buffer || capacity < required) return ERROR_INSUFFICIENT_BUFFER;

    info = (struct package_info_native *)buffer;
    strings = (WCHAR *)(buffer + selected_count * sizeof(*info));
    memset( info, 0, selected_count * sizeof(*info) );
    for (i = 0; i < selected_count; i++)
    {
        const struct current_package *package = &packages[selected[i]];

        if (package->flags & GRAPH_PACKAGE_FRAMEWORK)
            info[i].flags |= PACKAGE_PROPERTY_FRAMEWORK;
        if (package->flags & GRAPH_PACKAGE_RESOURCE)
            info[i].flags |= PACKAGE_PROPERTY_RESOURCE;
        if (selected[i]) info[i].flags |= PACKAGE_PROPERTY_STATIC;
        info[i].path = strings;
        strings = copy_package_string( strings, &graph, package->root );
        info[i].package_full_name = strings;
        strings = copy_package_string( strings, &graph, package->full_name );
        info[i].package_family_name = strings;
        strings = copy_package_string( strings, &graph, package->family_name );
        fill_package_id( &info[i].package_id, &strings, &graph, package );
    }
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetCurrentPackageInfo   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageInfo( const UINT32 flags, UINT32 *buffer_size,
                                   BYTE *buffer, UINT32 *count )
{
    return get_current_package_info( flags, PackagePathType_Install,
                                     buffer_size, buffer, count );
}

/***********************************************************************
 *         GetCurrentPackageInfo2   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageInfo2( const UINT32 flags,
                                    PackagePathType path_type,
                                    UINT32 *buffer_size, BYTE *buffer,
                                    UINT32 *count )
{
    return get_current_package_info( flags, path_type, buffer_size,
                                     buffer, count );
}

static LONG get_current_package_path( PackagePathType path_type,
                                      UINT32 *length, WCHAR *path )
{
    struct current_package_graph graph;
    struct current_package package;
    LONG ret;

    if ((ret = get_graph_or_error( &graph ))) return ret;
    if (!get_current_package( &graph, 0, &package ))
        return APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
    if ((ret = get_static_package_path_type_status( path_type ))) return ret;
    return copy_graph_string( &graph, package.root, length, path );
}

/***********************************************************************
 *         GetCurrentPackagePath   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackagePath(
    UINT32 *length, WCHAR *path )
{
    return get_current_package_path(
        PackagePathType_Install, length, path );
}

/***********************************************************************
 *         GetCurrentPackagePath2   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackagePath2( PackagePathType path_type,
                                    UINT32 *length, WCHAR *path )
{
    return get_current_package_path( path_type, length, path );
}


/***********************************************************************
 *         GetPackageFullName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFullName( HANDLE process, UINT32 *length, WCHAR *name )
{
    struct current_package_graph graph;
    struct current_package package;
    enum current_graph_status graph_status;
    SIZE_T free_size = 0;
    void *snapshot = NULL, *free_base;
    ULONG snapshot_size;
    NTSTATUS status, free_status;
    LONG ret;

    if (process == GetCurrentProcess())
        return GetCurrentPackageFullName( length, name );
    status = __wine_get_process_package_graph(
        process, &snapshot, &snapshot_size );
    if (status == STATUS_NOT_FOUND) return APPMODEL_ERROR_NO_PACKAGE;
    if (status) return RtlNtStatusToDosError( status );
    graph_status = validate_package_graph_data( snapshot, &graph );
    if (graph_status != CURRENT_GRAPH_VALID)
        ret = APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT;
    else if (!get_current_package( &graph, 0, &package ))
        ret = APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
    else
        ret = copy_graph_string( &graph, package.full_name, length, name );
    free_base = snapshot;
    free_status = NtFreeVirtualMemory(
        GetCurrentProcess(), &free_base, &free_size, MEM_RELEASE );
    if (!ret && free_status)
    {
        WARN( "failed to release remote package graph snapshot, status %#lx.\n",
              free_status );
        ret = RtlNtStatusToDosError( free_status );
    }
    return ret;
}


/***********************************************************************
 *         GetPackageFamilyName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFamilyName( HANDLE process, UINT32 *length, WCHAR *name )
{
    struct current_package_graph graph;
    struct current_package package;
    enum current_graph_status graph_status;
    SIZE_T free_size = 0;
    void *snapshot = NULL, *free_base;
    ULONG snapshot_size;
    NTSTATUS status, free_status;
    LONG ret;

    if (process == GetCurrentProcess())
        return GetCurrentPackageFamilyName( length, name );
    status = __wine_get_process_package_graph(
        process, &snapshot, &snapshot_size );
    if (status == STATUS_NOT_FOUND) return APPMODEL_ERROR_NO_PACKAGE;
    if (status) return RtlNtStatusToDosError( status );
    graph_status = validate_package_graph_data( snapshot, &graph );
    if (graph_status != CURRENT_GRAPH_VALID)
        ret = APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT;
    else if (!get_current_package( &graph, 0, &package ))
        ret = APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT;
    else
        ret = copy_graph_string( &graph, package.family_name, length, name );
    free_base = snapshot;
    free_status = NtFreeVirtualMemory(
        GetCurrentProcess(), &free_base, &free_size, MEM_RELEASE );
    if (!ret && free_status)
    {
        WARN( "failed to release remote package graph snapshot, status %#lx.\n",
              free_status );
        ret = RtlNtStatusToDosError( free_status );
    }
    return ret;
}

static const struct
{
    UINT32 code;
    const WCHAR *name;
}
arch_names[] =
{
    {PROCESSOR_ARCHITECTURE_INTEL,         L"x86"},
    {PROCESSOR_ARCHITECTURE_ARM,           L"arm"},
    {PROCESSOR_ARCHITECTURE_AMD64,         L"x64"},
    {PROCESSOR_ARCHITECTURE_NEUTRAL,       L"neutral"},
    {PROCESSOR_ARCHITECTURE_ARM64,         L"arm64"},
    {PROCESSOR_ARCHITECTURE_IA32_ON_ARM64, L"x86a64"},
};

struct parsed_package_full_name
{
    const WCHAR *name;
    const WCHAR *resource_id;
    const WCHAR *publisher_id;
    UINT32 name_length;
    UINT32 resource_id_length;
    UINT32 publisher_id_length;
    UINT32 processor_architecture;
    PACKAGE_VERSION version;
};

struct parsed_package_family_name
{
    const WCHAR *name;
    const WCHAR *publisher_id;
    UINT32 name_length;
    UINT32 publisher_id_length;
};

struct sha256_context
{
    UINT32 state[8];
    UINT64 bytes;
    BYTE block[64];
    UINT32 block_length;
};

struct appx_query_api
{
    HMODULE module;
    LONG (WINAPI *get_path)( const WCHAR *, UINT32 *, WCHAR * );
    LONG (WINAPI *get_family_packages)(
        const WCHAR *, UINT32 *, WCHAR **, UINT32 *, WCHAR * );
    LONG (WINAPI *get_publisher)( const WCHAR *, UINT32 *, WCHAR * );
};

static INIT_ONCE appx_query_init_once = INIT_ONCE_STATIC_INIT;
static struct appx_query_api appx_query_api;
static LONG appx_query_init_error = ERROR_GEN_FAILURE;

static WCHAR ascii_lower( WCHAR ch )
{
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static BOOL ascii_equal_slice( const WCHAR *value, UINT32 length,
                               const WCHAR *expected )
{
    UINT32 i;

    if (lstrlenW( expected ) != length) return FALSE;
    for (i = 0; i < length; i++)
        if (ascii_lower( value[i] ) != ascii_lower( expected[i] ))
            return FALSE;
    return TRUE;
}

static BOOL get_bounded_identity_length( const WCHAR *value, UINT32 minimum,
                                         UINT32 maximum, UINT32 *length )
{
    UINT32 i;

    if (!value) return FALSE;
    for (i = 0; i <= maximum; i++)
        if (!value[i])
        {
            if (i < minimum) return FALSE;
            *length = i;
            return TRUE;
        }
    return FALSE;
}

static BOOL is_identity_character( WCHAR ch )
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
}

static BOOL is_reserved_identity_name( const WCHAR *value, UINT32 length )
{
    UINT32 base_length = 0;

    while (base_length < length && value[base_length] != '.')
        base_length++;
    if (ascii_equal_slice( value, base_length, L"con" ) ||
        ascii_equal_slice( value, base_length, L"prn" ) ||
        ascii_equal_slice( value, base_length, L"aux" ) ||
        ascii_equal_slice( value, base_length, L"nul" ))
        return TRUE;
    if (base_length == 4 &&
        (ascii_equal_slice( value, 3, L"com" ) ||
         ascii_equal_slice( value, 3, L"lpt" )) &&
        value[3] >= '1' && value[3] <= '9')
        return TRUE;
    return FALSE;
}

static BOOL is_valid_identity_name( const WCHAR *value, UINT32 length,
                                    UINT32 minimum, UINT32 maximum,
                                    BOOL allow_empty )
{
    UINT32 i;

    if (!length) return allow_empty;
    if (!value || length < minimum || length > maximum ||
        value[length - 1] == '.' ||
        is_reserved_identity_name( value, length ))
        return FALSE;
    for (i = 0; i < length; i++)
    {
        if (!is_identity_character( value[i] )) return FALSE;
        if ((i == 0 || value[i - 1] == '.') && i + 4 <= length &&
            ascii_equal_slice( value + i, 4, L"xn--" ))
            return FALSE;
    }
    return TRUE;
}

static BOOL is_valid_publisher_id( const WCHAR *value, UINT32 length )
{
    UINT32 i;

    if (!value || length != PACKAGE_PUBLISHERID_MAX_LENGTH) return FALSE;
    for (i = 0; i < length; i++)
    {
        WCHAR ch = ascii_lower( value[i] );

        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'h') || ch == 'j' || ch == 'k' ||
            ch == 'm' || ch == 'n' || (ch >= 'p' && ch <= 't') ||
            (ch >= 'v' && ch <= 'z'))
            continue;
        return FALSE;
    }
    return TRUE;
}

static UINT32 processor_arch_from_string( const WCHAR *value, UINT32 length )
{
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(arch_names); i++)
        if (ascii_equal_slice( value, length, arch_names[i].name ))
            return arch_names[i].code;
    return ~0u;
}

static const WCHAR *processor_arch_to_string( UINT32 architecture )
{
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(arch_names); i++)
        if (arch_names[i].code == architecture) return arch_names[i].name;
    return NULL;
}

static BOOL parse_package_version( const WCHAR *value, UINT32 length,
                                   PACKAGE_VERSION *version )
{
    USHORT parts[4];
    UINT32 part, offset = 0;

    memset( parts, 0, sizeof(parts) );
    for (part = 0; part < ARRAY_SIZE(parts); part++)
    {
        UINT32 digits = 0, number = 0;

        while (offset < length && value[offset] >= '0' &&
               value[offset] <= '9')
        {
            number = number * 10 + value[offset++] - '0';
            if (++digits > 5 || number > 0xffff) return FALSE;
        }
        if (!digits ||
            (part + 1 < ARRAY_SIZE(parts) ?
             offset >= length || value[offset++] != '.' :
             offset != length))
            return FALSE;
        parts[part] = number;
    }
    version->Major = parts[0];
    version->Minor = parts[1];
    version->Build = parts[2];
    version->Revision = parts[3];
    return TRUE;
}

static BOOL parse_package_full_name( const WCHAR *full_name,
                                     struct parsed_package_full_name *parsed )
{
    UINT32 separators[4], separator_count = 0, length, i;
    UINT32 version_start, architecture_start, resource_start, publisher_start;

    memset( parsed, 0, sizeof(*parsed) );
    if (!get_bounded_identity_length(
            full_name, PACKAGE_FULL_NAME_MIN_LENGTH,
            PACKAGE_FULL_NAME_MAX_LENGTH, &length ))
        return FALSE;
    for (i = 0; i < length; i++)
        if (full_name[i] == '_')
        {
            if (separator_count == ARRAY_SIZE(separators)) return FALSE;
            separators[separator_count++] = i;
        }
    if (separator_count != ARRAY_SIZE(separators)) return FALSE;

    parsed->name = full_name;
    parsed->name_length = separators[0];
    version_start = separators[0] + 1;
    architecture_start = separators[1] + 1;
    resource_start = separators[2] + 1;
    publisher_start = separators[3] + 1;
    parsed->resource_id = full_name + resource_start;
    parsed->resource_id_length = separators[3] - resource_start;
    parsed->publisher_id = full_name + publisher_start;
    parsed->publisher_id_length = length - publisher_start;

    if (!is_valid_identity_name(
            parsed->name, parsed->name_length, PACKAGE_NAME_MIN_LENGTH,
            PACKAGE_NAME_MAX_LENGTH, FALSE ) ||
        !parse_package_version(
            full_name + version_start, separators[1] - version_start,
            &parsed->version ) ||
        (parsed->processor_architecture = processor_arch_from_string(
             full_name + architecture_start,
             separators[2] - architecture_start )) == ~0u ||
        !is_valid_identity_name(
            parsed->resource_id, parsed->resource_id_length, 1,
            PACKAGE_RESOURCEID_MAX_LENGTH, TRUE ) ||
        !is_valid_publisher_id(
            parsed->publisher_id, parsed->publisher_id_length ))
        return FALSE;
    return TRUE;
}

static BOOL parse_package_family_name(
    const WCHAR *family_name, struct parsed_package_family_name *parsed )
{
    UINT32 length, separator, i;

    memset( parsed, 0, sizeof(*parsed) );
    if (!get_bounded_identity_length(
            family_name, PACKAGE_FAMILY_NAME_MIN_LENGTH,
            PACKAGE_FAMILY_NAME_MAX_LENGTH, &length ))
        return FALSE;
    separator = length;
    for (i = 0; i < length; i++)
        if (family_name[i] == '_')
        {
            if (separator != length) return FALSE;
            separator = i;
        }
    if (separator == length) return FALSE;
    parsed->name = family_name;
    parsed->name_length = separator;
    parsed->publisher_id = family_name + separator + 1;
    parsed->publisher_id_length = length - separator - 1;
    return is_valid_identity_name(
               parsed->name, parsed->name_length, PACKAGE_NAME_MIN_LENGTH,
               PACKAGE_NAME_MAX_LENGTH, FALSE ) &&
           is_valid_publisher_id(
               parsed->publisher_id, parsed->publisher_id_length );
}

static UINT32 rotate_right( UINT32 value, UINT32 shift )
{
    return (value >> shift) | (value << (32 - shift));
}

static void sha256_transform( struct sha256_context *context,
                              const BYTE block[64] )
{
    static const UINT32 constants[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    UINT32 words[64], a, b, c, d, e, f, g, h, i;

    for (i = 0; i < 16; i++)
        words[i] = ((UINT32)block[i * 4] << 24) |
                   ((UINT32)block[i * 4 + 1] << 16) |
                   ((UINT32)block[i * 4 + 2] << 8) |
                   block[i * 4 + 3];
    for (; i < ARRAY_SIZE(words); i++)
    {
        UINT32 s0 = rotate_right( words[i - 15], 7 ) ^
                    rotate_right( words[i - 15], 18 ) ^
                    (words[i - 15] >> 3);
        UINT32 s1 = rotate_right( words[i - 2], 17 ) ^
                    rotate_right( words[i - 2], 19 ) ^
                    (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (i = 0; i < ARRAY_SIZE(words); i++)
    {
        UINT32 sum1 = rotate_right( e, 6 ) ^ rotate_right( e, 11 ) ^
                      rotate_right( e, 25 );
        UINT32 choose = (e & f) ^ (~e & g);
        UINT32 temporary1 = h + sum1 + choose + constants[i] + words[i];
        UINT32 sum0 = rotate_right( a, 2 ) ^ rotate_right( a, 13 ) ^
                      rotate_right( a, 22 );
        UINT32 majority = (a & b) ^ (a & c) ^ (b & c);
        UINT32 temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init( struct sha256_context *context )
{
    static const UINT32 initial_state[8] =
    {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    memset( context, 0, sizeof(*context) );
    memcpy( context->state, initial_state, sizeof(initial_state) );
}

static void sha256_update( struct sha256_context *context, const BYTE *data,
                           UINT32 length )
{
    UINT32 copy;

    context->bytes += length;
    while (length)
    {
        copy = min( length, 64 - context->block_length );
        memcpy( context->block + context->block_length, data, copy );
        context->block_length += copy;
        data += copy;
        length -= copy;
        if (context->block_length == 64)
        {
            sha256_transform( context, context->block );
            context->block_length = 0;
        }
    }
}

static void sha256_finish( struct sha256_context *context, BYTE digest[32] )
{
    BYTE padding[72] = {0x80};
    BYTE length_bytes[8];
    UINT64 bits = context->bytes * 8;
    UINT32 padding_length, i;

    for (i = 0; i < ARRAY_SIZE(length_bytes); i++)
        length_bytes[ARRAY_SIZE(length_bytes) - 1 - i] = bits >> (i * 8);
    padding_length = context->block_length < 56 ?
                     56 - context->block_length :
                     120 - context->block_length;
    sha256_update( context, padding, padding_length );
    sha256_update( context, length_bytes, sizeof(length_bytes) );
    for (i = 0; i < ARRAY_SIZE(context->state); i++)
    {
        digest[i * 4] = context->state[i] >> 24;
        digest[i * 4 + 1] = context->state[i] >> 16;
        digest[i * 4 + 2] = context->state[i] >> 8;
        digest[i * 4 + 3] = context->state[i];
    }
}

static void derive_publisher_id( const WCHAR *publisher, UINT32 length,
                                 WCHAR publisher_id[14] )
{
    static const WCHAR alphabet[] = L"0123456789abcdefghjkmnpqrstvwxyz";
    struct sha256_context hash;
    BYTE digest[32], encoded[2];
    UINT32 i, j;

    sha256_init( &hash );
    for (i = 0; i < length; i++)
    {
        encoded[0] = publisher[i];
        encoded[1] = publisher[i] >> 8;
        sha256_update( &hash, encoded, sizeof(encoded) );
    }
    sha256_finish( &hash, digest );
    for (i = 0; i < 13; i++)
    {
        UINT32 value = 0;

        for (j = 0; j < 5; j++)
        {
            UINT32 bit = i * 5 + j;

            value <<= 1;
            if (bit < 64)
                value |= (digest[bit / 8] >> (7 - bit % 8)) & 1;
        }
        publisher_id[i] = alphabet[value];
    }
    publisher_id[13] = 0;
}

static BOOL publisher_ids_equal( const WCHAR *left, const WCHAR *right )
{
    UINT32 i;

    for (i = 0; i < PACKAGE_PUBLISHERID_MAX_LENGTH; i++)
        if (ascii_lower( left[i] ) != ascii_lower( right[i] ))
            return FALSE;
    return TRUE;
}

static LONG get_package_id_publisher_id( const PACKAGE_ID *id,
                                         WCHAR publisher_id[14] )
{
    WCHAR derived[14];
    UINT32 publisher_length = 0, publisher_id_length = 0;
    BOOL has_publisher, has_publisher_id;

    has_publisher = id->publisher &&
        get_bounded_identity_length(
            id->publisher, PACKAGE_PUBLISHER_MIN_LENGTH,
            PACKAGE_PUBLISHER_MAX_LENGTH, &publisher_length );
    has_publisher_id = id->publisherId &&
        get_bounded_identity_length(
            id->publisherId, PACKAGE_PUBLISHERID_MIN_LENGTH,
            PACKAGE_PUBLISHERID_MAX_LENGTH, &publisher_id_length ) &&
        is_valid_publisher_id( id->publisherId, publisher_id_length );
    if ((id->publisher && !has_publisher) ||
        (id->publisherId && !has_publisher_id) ||
        (!has_publisher && !has_publisher_id))
        return ERROR_INVALID_PARAMETER;
    if (has_publisher)
    {
        derive_publisher_id( id->publisher, publisher_length, derived );
        if (has_publisher_id &&
            !publisher_ids_equal( derived, id->publisherId ))
            return ERROR_INVALID_PARAMETER;
        memcpy( publisher_id, derived, sizeof(derived) );
    }
    else
    {
        memcpy( publisher_id, id->publisherId,
                PACKAGE_PUBLISHERID_MAX_LENGTH * sizeof(*publisher_id) );
        publisher_id[PACKAGE_PUBLISHERID_MAX_LENGTH] = 0;
    }
    return ERROR_SUCCESS;
}

static WCHAR *append_identity_slice( WCHAR *cursor, const WCHAR *value,
                                     UINT32 length )
{
    memcpy( cursor, value, length * sizeof(*cursor) );
    return cursor + length;
}

static WCHAR *append_identity_uint16( WCHAR *cursor, USHORT value )
{
    WCHAR digits[5];
    UINT32 count = 0;

    do
    {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (count) *cursor++ = digits[--count];
    return cursor;
}

static LONG copy_identity_string( const WCHAR *value, UINT32 required,
                                  UINT32 *length, WCHAR *buffer )
{
    UINT32 capacity;

    if (!length) return ERROR_INVALID_PARAMETER;
    capacity = *length;
    *length = required;
    if (!buffer || capacity < required) return ERROR_INSUFFICIENT_BUFFER;
    memcpy( buffer, value, required * sizeof(*buffer) );
    return ERROR_SUCCESS;
}

static LONG load_appx_query_proc( HMODULE module, const char *name,
                                  void **function )
{
    ANSI_STRING name_string;
    NTSTATUS status;

    RtlInitAnsiString( &name_string, name );
    if ((status = LdrGetProcedureAddress(
            module, &name_string, 0, function )))
        return RtlNtStatusToDosError( status );
    return ERROR_SUCCESS;
}

static BOOL CALLBACK init_appx_query_api( INIT_ONCE *once, void *parameter,
                                          void **context )
{
    UNICODE_STRING module_name;
    ULONG flags = 0;
    NTSTATUS status;
    LONG error;

    RtlInitUnicodeString( &module_name, L"appxsvc.dll" );
    if ((status = LdrLoadDll(
            (void *)((ULONG_PTR)LOAD_LIBRARY_SEARCH_SYSTEM32 | 1),
            &flags, &module_name, &appx_query_api.module )))
    {
        appx_query_init_error = RtlNtStatusToDosError( status );
        return TRUE;
    }
    if ((error = load_appx_query_proc(
            appx_query_api.module,
            "wine_appx_get_package_path_by_full_name",
            (void **)&appx_query_api.get_path )) ||
        (error = load_appx_query_proc(
            appx_query_api.module, "wine_appx_get_packages_by_family",
            (void **)&appx_query_api.get_family_packages )) ||
        (error = load_appx_query_proc(
            appx_query_api.module,
            "wine_appx_get_package_publisher_by_full_name",
            (void **)&appx_query_api.get_publisher )))
    {
        appx_query_init_error = error;
        return TRUE;
    }
    appx_query_init_error = ERROR_SUCCESS;
    return TRUE;
}

static LONG get_appx_query_api( const struct appx_query_api **api )
{
    if (!InitOnceExecuteOnce(
            &appx_query_init_once, init_appx_query_api, NULL, NULL ))
        return GetLastError();
    if (appx_query_init_error) return appx_query_init_error;
    *api = &appx_query_api;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetPackagesByPackageFamily   (kernelbase.@)
 */
LONG WINAPI DECLSPEC_HOTPATCH GetPackagesByPackageFamily(
    const WCHAR *family_name, UINT32 *count, WCHAR **full_names,
    UINT32 *buffer_len, WCHAR *buffer )
{
    struct parsed_package_family_name parsed;
    const struct appx_query_api *api;
    LONG ret;

    TRACE( "family_name %s, count %p, full_names %p, buffer_len %p, "
           "buffer %p.\n", debugstr_w(family_name), count, full_names,
           buffer_len, buffer );

    if (!count || !buffer_len ||
        !parse_package_family_name( family_name, &parsed ))
        return ERROR_INVALID_PARAMETER;
    if ((ret = get_appx_query_api( &api ))) return ret;
    return api->get_family_packages(
        family_name, count, full_names, buffer_len, buffer );
}

/***********************************************************************
 *         GetPackagePathByFullName   (kernelbase.@)
 */
LONG WINAPI GetPackagePathByFullName(
    const WCHAR *name, UINT32 *len, WCHAR *path )
{
    struct parsed_package_full_name parsed;
    const struct appx_query_api *api;
    LONG ret;

    TRACE( "name %s, len %p, path %p.\n",
           debugstr_w(name), len, path );

    if (!len || !parse_package_full_name( name, &parsed ))
        return ERROR_INVALID_PARAMETER;
    if ((ret = get_appx_query_api( &api ))) return ret;
    return api->get_path( name, len, path );
}

/***********************************************************************
 *         GetPackagePathByFullName2   (kernelbase.@)
 */
LONG WINAPI GetPackagePathByFullName2(
    const WCHAR *name, PackagePathType path_type, UINT32 *len, WCHAR *path )
{
    struct parsed_package_full_name parsed;
    const struct appx_query_api *api;
    UINT32 probe_length = 0;
    LONG path_type_status, ret;

    TRACE( "name %s, path_type %u, len %p, path %p.\n",
           debugstr_w(name), path_type, len, path );

    if (!len || !parse_package_full_name( name, &parsed ))
        return ERROR_INVALID_PARAMETER;
    if ((path_type_status =
            get_static_package_path_type_status( path_type )) ==
        ERROR_INVALID_PARAMETER)
        return path_type_status;
    if ((ret = get_appx_query_api( &api ))) return ret;
    if (!path_type_status) return api->get_path( name, len, path );

    ret = api->get_path( name, &probe_length, NULL );
    return ret == ERROR_INSUFFICIENT_BUFFER ? path_type_status : ret;
}

/***********************************************************************
 *         PackageFamilyNameFromFullName   (kernelbase.@)
 */
LONG WINAPI PackageFamilyNameFromFullName(
    const WCHAR *full_name, UINT32 *family_name_length, WCHAR *family_name )
{
    struct parsed_package_full_name parsed;
    WCHAR result[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1], *cursor = result;

    TRACE( "full_name %s, family_name_length %p, family_name %p.\n",
           debugstr_w(full_name), family_name_length, family_name );

    if (!family_name_length ||
        !parse_package_full_name( full_name, &parsed ))
        return ERROR_INVALID_PARAMETER;
    cursor = append_identity_slice(
        cursor, parsed.name, parsed.name_length );
    *cursor++ = '_';
    cursor = append_identity_slice(
        cursor, parsed.publisher_id, parsed.publisher_id_length );
    *cursor++ = 0;
    return copy_identity_string(
        result, cursor - result, family_name_length, family_name );
}

/***********************************************************************
 *         PackageFamilyNameFromId   (kernelbase.@)
 */
LONG WINAPI PackageFamilyNameFromId(
    const PACKAGE_ID *id, UINT32 *family_name_length, WCHAR *family_name )
{
    WCHAR publisher_id[14];
    WCHAR result[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1], *cursor = result;
    UINT32 name_length, resource_length;
    LONG ret;

    TRACE( "id %p, family_name_length %p, family_name %p.\n",
           id, family_name_length, family_name );

    if (!id || !family_name_length || id->reserved ||
        !get_bounded_identity_length(
            id->name, PACKAGE_NAME_MIN_LENGTH, PACKAGE_NAME_MAX_LENGTH,
            &name_length ) ||
        !is_valid_identity_name(
            id->name, name_length, PACKAGE_NAME_MIN_LENGTH,
            PACKAGE_NAME_MAX_LENGTH, FALSE ) ||
        !processor_arch_to_string( id->processorArchitecture ) ||
        (id->resourceId &&
         (!get_bounded_identity_length(
              id->resourceId, PACKAGE_RESOURCEID_MIN_LENGTH,
              PACKAGE_RESOURCEID_MAX_LENGTH, &resource_length ) ||
          !is_valid_identity_name(
              id->resourceId, resource_length, 1,
              PACKAGE_RESOURCEID_MAX_LENGTH, TRUE ))))
        return ERROR_INVALID_PARAMETER;
    if ((ret = get_package_id_publisher_id( id, publisher_id ))) return ret;
    cursor = append_identity_slice( cursor, id->name, name_length );
    *cursor++ = '_';
    cursor = append_identity_slice(
        cursor, publisher_id, PACKAGE_PUBLISHERID_MAX_LENGTH );
    *cursor++ = 0;
    return copy_identity_string(
        result, cursor - result, family_name_length, family_name );
}

/***********************************************************************
 *         PackageFullNameFromId   (kernelbase.@)
 */
LONG WINAPI PackageFullNameFromId(
    const PACKAGE_ID *id, UINT32 *full_name_length, WCHAR *full_name )
{
    WCHAR publisher_id[14];
    WCHAR result[PACKAGE_FULL_NAME_MAX_LENGTH + 1], *cursor = result;
    const WCHAR *architecture;
    UINT32 name_length, resource_length = 0;
    LONG ret;

    TRACE( "id %p, full_name_length %p, full_name %p.\n",
           id, full_name_length, full_name );

    if (!id || !full_name_length || id->reserved ||
        !get_bounded_identity_length(
            id->name, PACKAGE_NAME_MIN_LENGTH, PACKAGE_NAME_MAX_LENGTH,
            &name_length ) ||
        !is_valid_identity_name(
            id->name, name_length, PACKAGE_NAME_MIN_LENGTH,
            PACKAGE_NAME_MAX_LENGTH, FALSE ) ||
        (id->resourceId &&
         (!get_bounded_identity_length(
              id->resourceId, PACKAGE_RESOURCEID_MIN_LENGTH,
              PACKAGE_RESOURCEID_MAX_LENGTH, &resource_length ) ||
          !is_valid_identity_name(
              id->resourceId, resource_length, 1,
              PACKAGE_RESOURCEID_MAX_LENGTH, TRUE ))) ||
        !(architecture = processor_arch_to_string(
              id->processorArchitecture )))
        return ERROR_INVALID_PARAMETER;
    if ((ret = get_package_id_publisher_id( id, publisher_id ))) return ret;

    cursor = append_identity_slice( cursor, id->name, name_length );
    *cursor++ = '_';
    cursor = append_identity_uint16( cursor, id->version.Major );
    *cursor++ = '.';
    cursor = append_identity_uint16( cursor, id->version.Minor );
    *cursor++ = '.';
    cursor = append_identity_uint16( cursor, id->version.Build );
    *cursor++ = '.';
    cursor = append_identity_uint16( cursor, id->version.Revision );
    *cursor++ = '_';
    cursor = append_identity_slice(
        cursor, architecture, lstrlenW(architecture) );
    *cursor++ = '_';
    if (resource_length)
        cursor = append_identity_slice(
            cursor, id->resourceId, resource_length );
    *cursor++ = '_';
    cursor = append_identity_slice(
        cursor, publisher_id, PACKAGE_PUBLISHERID_MAX_LENGTH );
    *cursor++ = 0;
    if (cursor - result > ARRAY_SIZE(result))
        return ERROR_INVALID_PARAMETER;
    return copy_identity_string(
        result, cursor - result, full_name_length, full_name );
}

/***********************************************************************
 *         PackageNameAndPublisherIdFromFamilyName   (kernelbase.@)
 */
LONG WINAPI PackageNameAndPublisherIdFromFamilyName(
    const WCHAR *family_name, UINT32 *name_length, WCHAR *name,
    UINT32 *publisher_id_length, WCHAR *publisher_id )
{
    struct parsed_package_family_name parsed;
    UINT32 name_capacity, publisher_capacity;
    UINT32 required_name, required_publisher;

    TRACE( "family_name %s, name_length %p, name %p, publisher_id_length "
           "%p, publisher_id %p.\n", debugstr_w(family_name), name_length,
           name, publisher_id_length, publisher_id );

    if (!name_length || !publisher_id_length ||
        !parse_package_family_name( family_name, &parsed ))
        return ERROR_INVALID_PARAMETER;
    name_capacity = *name_length;
    publisher_capacity = *publisher_id_length;
    required_name = parsed.name_length + 1;
    required_publisher = parsed.publisher_id_length + 1;
    *name_length = required_name;
    *publisher_id_length = required_publisher;
    if (!name || !publisher_id || name_capacity < required_name ||
        publisher_capacity < required_publisher)
        return ERROR_INSUFFICIENT_BUFFER;
    memcpy( name, parsed.name, parsed.name_length * sizeof(*name) );
    name[parsed.name_length] = 0;
    memcpy( publisher_id, parsed.publisher_id,
            parsed.publisher_id_length * sizeof(*publisher_id) );
    publisher_id[parsed.publisher_id_length] = 0;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         PackageIdFromFullName   (kernelbase.@)
 */
LONG WINAPI PackageIdFromFullName(
    const WCHAR *full_name, UINT32 flags, UINT32 *buffer_length, BYTE *buffer )
{
    struct parsed_package_full_name parsed;
    const struct appx_query_api *api;
    WCHAR derived_publisher_id[14];
    WCHAR *publisher = NULL, *strings;
    UINT32 publisher_length = 0, capacity, size;
    PACKAGE_ID id;
    LONG ret;

    TRACE( "full_name %s, flags %#x, buffer_length %p, buffer %p.\n",
           debugstr_w(full_name), flags, buffer_length, buffer );

    if (!buffer_length ||
        (flags != PACKAGE_INFORMATION_BASIC &&
         flags != PACKAGE_INFORMATION_FULL) ||
        !parse_package_full_name( full_name, &parsed ))
        return ERROR_INVALID_PARAMETER;
    if (!buffer && *buffer_length) return ERROR_INVALID_PARAMETER;

    if (flags == PACKAGE_INFORMATION_FULL)
    {
        if ((ret = get_appx_query_api( &api ))) return ret;
        publisher_length = PACKAGE_PUBLISHER_MAX_LENGTH + 1;
        if (!(publisher = HeapAlloc(
                GetProcessHeap(), 0,
                publisher_length * sizeof(*publisher) )))
            return ERROR_NOT_ENOUGH_MEMORY;
        ret = api->get_publisher(
            full_name, &publisher_length, publisher );
        if (ret)
        {
            HeapFree( GetProcessHeap(), 0, publisher );
            return ret;
        }
        if (!get_bounded_identity_length(
                publisher, PACKAGE_PUBLISHER_MIN_LENGTH,
                PACKAGE_PUBLISHER_MAX_LENGTH, &publisher_length ))
        {
            HeapFree( GetProcessHeap(), 0, publisher );
            return ERROR_BAD_FORMAT;
        }
        derive_publisher_id(
            publisher, publisher_length, derived_publisher_id );
        if (!publisher_ids_equal(
                derived_publisher_id, parsed.publisher_id ))
        {
            HeapFree( GetProcessHeap(), 0, publisher );
            return ERROR_BAD_FORMAT;
        }
        publisher_length++;
    }

    size = sizeof(id) +
           (parsed.name_length + 1 +
            parsed.resource_id_length + 1 +
            parsed.publisher_id_length + 1 +
            publisher_length) * sizeof(WCHAR);
    capacity = *buffer_length;
    *buffer_length = size;
    if (!buffer || capacity < size)
    {
        HeapFree( GetProcessHeap(), 0, publisher );
        return ERROR_INSUFFICIENT_BUFFER;
    }

    memset( &id, 0, sizeof(id) );
    id.processorArchitecture = parsed.processor_architecture;
    id.version = parsed.version;
    strings = (WCHAR *)(buffer + sizeof(id));
    id.name = strings;
    strings = append_identity_slice(
        strings, parsed.name, parsed.name_length );
    *strings++ = 0;
    if (publisher)
    {
        id.publisher = strings;
        strings = append_identity_slice(
            strings, publisher, publisher_length - 1 );
        *strings++ = 0;
    }
    id.resourceId = strings;
    strings = append_identity_slice(
        strings, parsed.resource_id, parsed.resource_id_length );
    *strings++ = 0;
    id.publisherId = strings;
    strings = append_identity_slice(
        strings, parsed.publisher_id, parsed.publisher_id_length );
    *strings = 0;
    memcpy( buffer, &id, sizeof(id) );
    HeapFree( GetProcessHeap(), 0, publisher );
    return ERROR_SUCCESS;
}
