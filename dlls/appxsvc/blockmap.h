/*
 * AppX block map parser
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

#ifndef __APPXSVC_BLOCKMAP_H
#define __APPXSVC_BLOCKMAP_H

#include "windef.h"

#define APPX_BLOCK_MAP_BLOCK_SIZE             65536
#define APPX_BLOCK_MAP_HASH_SIZE              32

/*
 * These are parser limits, not hints.  Keep them independent of libxml2's
 * implementation limits so accepting a document never depends on a library
 * build option.
 */
#define APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE       (64u * 1024 * 1024)
#define APPX_BLOCK_MAP_MAX_DEPTH               32
#define APPX_BLOCK_MAP_MAX_FILES               65536
/* 64 GiB / 64 KiB plus at most one partially filled block per file. */
#define APPX_BLOCK_MAP_MAX_BLOCKS              1114112
/* Root, files, blocks, and one optional 2021 FileHash per file. */
#define APPX_BLOCK_MAP_MAX_ELEMENTS            1245185
#define APPX_BLOCK_MAP_MAX_NODES               4000000
#define APPX_BLOCK_MAP_MAX_ATTRIBUTES          2500000
#define APPX_BLOCK_MAP_MAX_ATTRIBUTES_PER_NODE 8
#define APPX_BLOCK_MAP_MAX_XML_NAME_BYTES      128
#define APPX_BLOCK_MAP_MAX_XML_VALUE_BYTES     2048
#define APPX_BLOCK_MAP_MAX_TOTAL_NAME_BYTES    (128u * 1024 * 1024)
#define APPX_BLOCK_MAP_MAX_TOTAL_VALUE_BYTES   (128u * 1024 * 1024)
#define APPX_BLOCK_MAP_MAX_FILE_NAME_CHARS     260
#define APPX_BLOCK_MAP_MAX_FILE_SIZE           (16ULL * 1024 * 1024 * 1024)
#define APPX_BLOCK_MAP_MAX_TOTAL_FILE_SIZE     (64ULL * 1024 * 1024 * 1024)

typedef struct appx_block_map APPX_BLOCK_MAP;

typedef struct
{
    /* UTF-16 length excludes the terminator; separators in name are backslashes. */
    const WCHAR *name;
    UINT32 name_length;
    UINT64 size;
    UINT32 local_file_header_size;
    UINT32 block_count;
    /* Optional 2021 b4:FileHash over the whole uncompressed file. */
    BYTE file_hash[APPX_BLOCK_MAP_HASH_SIZE];
    BOOL has_file_hash;
} APPX_BLOCK_MAP_FILE;

typedef struct
{
    BYTE hash[APPX_BLOCK_MAP_HASH_SIZE];
    /* logical_size is 64 KiB except for the last non-empty file block. */
    UINT32 logical_size;
    /* ZIP reconciliation decides whether Size was required for this entry. */
    UINT32 compressed_size;
    BOOL has_compressed_size;
} APPX_BLOCK_MAP_BLOCK;

/*
 * Successful parsing returns an immutable map.  Views returned by the accessors
 * remain valid until appx_block_map_free() and preserve document block order.
 */
HRESULT WINAPI appx_block_map_parse( const BYTE *document, UINT32 size, APPX_BLOCK_MAP **result );
void WINAPI appx_block_map_free( APPX_BLOCK_MAP *map );

UINT32 WINAPI appx_block_map_get_file_count( const APPX_BLOCK_MAP *map );
UINT32 WINAPI appx_block_map_get_block_count( const APPX_BLOCK_MAP *map );
const APPX_BLOCK_MAP_FILE *WINAPI appx_block_map_get_file( const APPX_BLOCK_MAP *map,
                                                          UINT32 index );
const APPX_BLOCK_MAP_BLOCK *WINAPI appx_block_map_get_block( const APPX_BLOCK_MAP *map,
                                                            UINT32 file_index,
                                                            UINT32 block_index );

#endif /* __APPXSVC_BLOCKMAP_H */
