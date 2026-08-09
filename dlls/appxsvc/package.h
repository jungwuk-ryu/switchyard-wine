/*
 * AppX package inspection interfaces
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

#ifndef __WINE_APPXSVC_PACKAGE_H
#define __WINE_APPXSVC_PACKAGE_H

#include "windef.h"
#include "wine/appxsvc.h"

#include "manifest.h"

/*
 * The untrusted-chain flag exists for conformance fixtures and explicitly
 * configured development installs; production deployment never passes it.
 * Production may defer bulk payload hashes until extraction, but that never
 * relaxes the CMS signature, signed digest, publisher, block-map structure,
 * CRC, or package-layout checks.
 */
#define APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN 0x00000001
#define APPX_PACKAGE_INSPECT_DEFER_PAYLOAD_VALIDATION 0x00000002
#define APPX_PACKAGE_MAX_CODE_INTEGRITY_SIZE       (256u * 1024 * 1024)
#define APPX_PACKAGE_CONTENT_ID_SIZE               32
#define APPX_PACKAGE_SIGNER_ID_SIZE                32

typedef struct appx_package_inspection APPX_PACKAGE_INSPECTION;
typedef struct appx_package_validation APPX_PACKAGE_VALIDATION;

typedef struct
{
    const WCHAR *path;
    UINT32 archive_index;
    UINT16 compression_method;
    UINT16 reserved;
    UINT64 compressed_size;
    UINT64 uncompressed_size;
} APPX_PACKAGE_FILE;

/*
 * The input handle must identify a regular, seekable file and remain open
 * until this call returns.  The archive layer reopens and byte-range-locks the
 * file.  A successful inspection retains that pinned archive until
 * appx_package_inspection_free().
 * The _ex form accepts an optional cancellation event that must remain valid
 * until the call returns.  The legacy form behaves as if that event were NULL.
 */
HRESULT WINAPI appx_package_inspect( HANDLE file,
                                     const WINE_APPX_ARCHIVE_LIMITS *limits,
                                     UINT32 flags,
                                     APPX_PACKAGE_INSPECTION **result );
HRESULT WINAPI appx_package_inspect_ex( HANDLE file,
                                        const WINE_APPX_ARCHIVE_LIMITS *limits,
                                        UINT32 flags, HANDLE cancel_event,
                                        APPX_PACKAGE_INSPECTION **result );
void WINAPI appx_package_inspection_free( APPX_PACKAGE_INSPECTION *inspection );

const APPX_MANIFEST *WINAPI appx_package_inspection_get_manifest(
    const APPX_PACKAGE_INSPECTION *inspection );
UINT32 WINAPI appx_package_inspection_get_file_count(
    const APPX_PACKAGE_INSPECTION *inspection );
const APPX_PACKAGE_FILE *WINAPI appx_package_inspection_get_file(
    const APPX_PACKAGE_INSPECTION *inspection, UINT32 index );
UINT64 WINAPI appx_package_inspection_get_expanded_size(
    const APPX_PACKAGE_INSPECTION *inspection );
UINT64 WINAPI appx_package_inspection_get_archive_expanded_size(
    const APPX_PACKAGE_INSPECTION *inspection );
HRESULT WINAPI appx_package_inspection_get_content_id(
    const APPX_PACKAGE_INSPECTION *inspection, BYTE *content_id, UINT32 size );
HRESULT WINAPI appx_package_inspection_get_signer_id(
    const APPX_PACKAGE_INSPECTION *inspection, BYTE *signer_id, UINT32 size );

/*
 * Ordinary streams are available only after inspection has validated every
 * payload hash.  They own an independent archive handle and therefore remain
 * valid if the inspection object is freed after this call returns.
 */
HRESULT WINAPI appx_package_inspection_open_stream(
    const APPX_PACKAGE_INSPECTION *inspection, UINT32 file_index,
    WINE_APPX_ARCHIVE_STREAM **stream );

/*
 * Deployment may defer bulk payload block-hash validation so extraction can
 * hash the same bytes it writes instead of inflating every file twice.  The
 * signed block map, content types, package byte image, and manifest are still
 * authenticated before a deferred inspection is returned.  Such an inspection
 * must be consumed only by appx_package_extract(); ordinary stream access is
 * rejected until validation has already completed during inspection.
 *
 * These helpers are the private bridge used by the extractor.  A validation
 * object owns one reusable SHA-256 engine for the whole extraction and accepts
 * one file at a time.
 */
HRESULT WINAPI appx_package_inspection_open_validation(
    const APPX_PACKAGE_INSPECTION *inspection,
    APPX_PACKAGE_VALIDATION **validation );
HRESULT WINAPI appx_package_validation_begin_file(
    APPX_PACKAGE_VALIDATION *validation, UINT32 file_index );
HRESULT WINAPI appx_package_validation_update(
    APPX_PACKAGE_VALIDATION *validation, const void *data, UINT32 size );
HRESULT WINAPI appx_package_validation_finish_file(
    APPX_PACKAGE_VALIDATION *validation );
void WINAPI appx_package_validation_close(
    APPX_PACKAGE_VALIDATION *validation );
HRESULT WINAPI appx_package_inspection_open_validation_stream(
    const APPX_PACKAGE_INSPECTION *inspection, UINT32 file_index,
    WINE_APPX_ARCHIVE_STREAM **stream );

#endif /* __WINE_APPXSVC_PACKAGE_H */
