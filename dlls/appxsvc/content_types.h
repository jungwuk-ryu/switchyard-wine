/*
 * AppX OPC content-types parser interfaces
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

#ifndef __APPXSVC_CONTENT_TYPES_H
#define __APPXSVC_CONTENT_TYPES_H

#include "windef.h"

#define APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE      (16u * 1024 * 1024)
#define APPX_CONTENT_TYPES_MAX_DEPTH              16
#define APPX_CONTENT_TYPES_MAX_ENTRIES            65536
#define APPX_CONTENT_TYPES_MAX_NODES              262144
#define APPX_CONTENT_TYPES_MAX_ATTRIBUTES         131104
#define APPX_CONTENT_TYPES_MAX_ATTRIBUTES_PER_NODE 8
#define APPX_CONTENT_TYPES_MAX_NAMESPACES         32
#define APPX_CONTENT_TYPES_MAX_XML_NAME_BYTES     128
#define APPX_CONTENT_TYPES_MAX_XML_VALUE_BYTES    32767
#define APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS    255
#define APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS 255

#define APPX_CONTENT_TYPE_MANIFEST \
    L"application/vnd.ms-appx.manifest+xml"
#define APPX_CONTENT_TYPE_BLOCK_MAP \
    L"application/vnd.ms-appx.blockmap+xml"
#define APPX_CONTENT_TYPE_SIGNATURE \
    L"application/vnd.ms-appx.signature"
#define APPX_CONTENT_TYPE_BUNDLE_MANIFEST \
    L"application/vnd.ms-appx.bundlemanifest+xml"

typedef struct appx_content_types APPX_CONTENT_TYPES;

enum appx_content_types_mode
{
    APPX_CONTENT_TYPES_MODE_PACKAGE = 1,
    APPX_CONTENT_TYPES_MODE_BUNDLE = 2
};

typedef struct
{
    const WCHAR *extension;
    const WCHAR *content_type;
    UINT32 extension_length;
    UINT32 content_type_length;
} APPX_CONTENT_TYPE_DEFAULT;

typedef struct
{
    /*
     * This is an absolute, decoded and NFC-normalized OPC part name using
     * forward slashes.  Percent escapes are decoded exactly once.
     */
    const WCHAR *part_name;
    const WCHAR *content_type;
    UINT32 part_name_length;
    UINT32 content_type_length;
} APPX_CONTENT_TYPE_OVERRIDE;

/*
 * Parsing also validates the mode-specific required metadata mappings.
 * Successful parsing returns a sorted immutable table; all returned views and
 * strings remain valid until appx_content_types_free().
 */
HRESULT WINAPI appx_content_types_parse( const BYTE *document, UINT32 size,
                                         enum appx_content_types_mode mode,
                                         APPX_CONTENT_TYPES **result );
void WINAPI appx_content_types_free( APPX_CONTENT_TYPES *types );

enum appx_content_types_mode WINAPI appx_content_types_get_mode(
    const APPX_CONTENT_TYPES *types );
UINT32 WINAPI appx_content_types_get_default_count( const APPX_CONTENT_TYPES *types );
const APPX_CONTENT_TYPE_DEFAULT *WINAPI appx_content_types_get_default(
    const APPX_CONTENT_TYPES *types, UINT32 index );
UINT32 WINAPI appx_content_types_get_override_count( const APPX_CONTENT_TYPES *types );
const APPX_CONTENT_TYPE_OVERRIDE *WINAPI appx_content_types_get_override(
    const APPX_CONTENT_TYPES *types, UINT32 index );

/*
 * part_name must use the decoded absolute syntax returned by the override
 * accessor.  An Override takes precedence over a Default, as required by OPC.
 */
const WCHAR *WINAPI appx_content_types_get_content_type(
    const APPX_CONTENT_TYPES *types, const WCHAR *part_name );

#endif /* __APPXSVC_CONTENT_TYPES_H */
