/*
 * Wine AppX deployment service interfaces
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

#ifndef __WINE_APPXSVC_H
#define __WINE_APPXSVC_H

#include "windef.h"

#define WINE_APPX_PATH_DIRECTORY       0x00000001
#define WINE_APPX_MAX_ENTRY_NAME_BYTES 32767
#define WINE_APPX_MAX_PATH_CHARS       32767
#define WINE_APPX_MAX_COMPONENT_CHARS  255

#define WINE_APPX_ARCHIVE_OPEN_PACKAGE 0x00000001
#define WINE_APPX_ARCHIVE_OPEN_BUNDLE  0x00000002

#define WINE_APPX_ENTRY_DIRECTORY      0x00000001
#define WINE_APPX_ENTRY_DATA_DESCRIPTOR 0x00000002

typedef struct wine_appx_archive WINE_APPX_ARCHIVE;

typedef struct
{
    UINT32 size;
    UINT32 max_entries;
    UINT64 max_archive_size;
    UINT64 max_central_directory_size;
    UINT64 max_entry_compressed_size;
    UINT64 max_entry_uncompressed_size;
    UINT64 max_total_uncompressed_size;
    UINT32 max_compression_ratio;
    UINT32 compression_ratio_slack;
} WINE_APPX_ARCHIVE_LIMITS;

typedef struct
{
    UINT32 size;
    UINT32 flags;
    UINT32 crc32;
    UINT16 compression_method;
    UINT16 reserved;
    UINT64 compressed_size;
    UINT64 uncompressed_size;
    UINT64 local_header_offset;
    UINT64 data_offset;
} WINE_APPX_ARCHIVE_ENTRY;

HRESULT WINAPI wine_appx_validate_archive_path( const BYTE *utf8, UINT32 utf8_length, UINT32 flags,
                                                UINT32 *path_length, WCHAR *path );
/*
 * The archive handle must grant read access and permit a second read open.
 * The service owns that sharing contract; reopening keeps archive I/O from
 * changing or racing the caller's file position.
 */
HRESULT WINAPI wine_appx_archive_open( HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits,
                                      UINT32 flags, WINE_APPX_ARCHIVE **archive );
void WINAPI wine_appx_archive_close( WINE_APPX_ARCHIVE *archive );
HRESULT WINAPI wine_appx_archive_get_count( WINE_APPX_ARCHIVE *archive, UINT32 *count );
HRESULT WINAPI wine_appx_archive_get_entry( WINE_APPX_ARCHIVE *archive, UINT32 index,
                                            WINE_APPX_ARCHIVE_ENTRY *entry,
                                            UINT32 *path_length, WCHAR *path );

#endif /* __WINE_APPXSVC_H */
