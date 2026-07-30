/*
 * AppX package signature verification interfaces
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

#ifndef __WINE_APPXSVC_SIGNATURE_H
#define __WINE_APPXSVC_SIGNATURE_H

#include "windef.h"
#include "winerror.h"

#define APPX_SIGNATURE_MAX_SIZE                    (2 * 1024 * 1024)
#define APPX_SIGNATURE_SHA256_SIZE                 32

#define APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN 0x00000001

#define APPX_SIGNATURE_DIGEST_PACKAGE_CONTENTS     0x00000001
#define APPX_SIGNATURE_DIGEST_CENTRAL_DIRECTORY    0x00000002
#define APPX_SIGNATURE_DIGEST_CONTENT_TYPES        0x00000004
#define APPX_SIGNATURE_DIGEST_BLOCK_MAP            0x00000008
#define APPX_SIGNATURE_DIGEST_CODE_INTEGRITY       0x00000010
#define APPX_SIGNATURE_DIGEST_REQUIRED             \
    (APPX_SIGNATURE_DIGEST_PACKAGE_CONTENTS |      \
     APPX_SIGNATURE_DIGEST_CENTRAL_DIRECTORY |    \
     APPX_SIGNATURE_DIGEST_CONTENT_TYPES |         \
     APPX_SIGNATURE_DIGEST_BLOCK_MAP)
#define APPX_SIGNATURE_DIGEST_ALL                  \
    (APPX_SIGNATURE_DIGEST_REQUIRED | APPX_SIGNATURE_DIGEST_CODE_INTEGRITY)

typedef struct appx_signature APPX_SIGNATURE;

/*
 * These hashes are the package digest records embedded in
 * SpcIndirectDataContent.  A caller must set exactly the flags for the hashes
 * it recalculated.  In particular, the AXCI flag and hash are present only
 * when AppxMetadata/CodeIntegrity.cat is part of the package.
 */
struct appx_signature_digest_set
{
    UINT32 flags;
    BYTE package_contents[APPX_SIGNATURE_SHA256_SIZE];  /* AXPC */
    BYTE central_directory[APPX_SIGNATURE_SHA256_SIZE]; /* AXCD */
    BYTE content_types[APPX_SIGNATURE_SHA256_SIZE];     /* AXCT */
    BYTE block_map[APPX_SIGNATURE_SHA256_SIZE];         /* AXBM */
    BYTE code_integrity[APPX_SIGNATURE_SHA256_SIZE];    /* AXCI */
};

/*
 * Parse AppxSignature.p7x, verify its single CMS signer, bind the
 * authenticated message digest to the embedded SpcIndirectDataContent, and
 * verify the signer's certificate chain.  ALLOW_UNTRUSTED_CHAIN relaxes only
 * untrusted-root/incomplete-chain policy errors; it never bypasses CMS
 * signature or content-digest verification.
 */
HRESULT WINAPI appx_signature_parse_and_verify( const BYTE *data, SIZE_T size,
                                                UINT32 flags,
                                                APPX_SIGNATURE **signature );
void WINAPI appx_signature_free( APPX_SIGNATURE *signature );

const struct appx_signature_digest_set * WINAPI appx_signature_get_digest_set(
    const APPX_SIGNATURE *signature );
const WCHAR * WINAPI appx_signature_get_signer_subject( const APPX_SIGNATURE *signature );

/*
 * Decode the fixed APPX/AXP* digest stream.  This helper is intentionally
 * exposed only inside appxsvc so the hostile-input grammar can be tested
 * independently of certificate fixtures.
 */
HRESULT WINAPI appx_signature_decode_digest_set( const BYTE *data, SIZE_T size,
                                                 struct appx_signature_digest_set *set );
/* Strictly decode the complete DER SpcIndirectDataContent value. */
HRESULT WINAPI appx_signature_decode_indirect_data(
    const BYTE *data, SIZE_T size, struct appx_signature_digest_set *set );

/*
 * Compare a caller-recalculated package hash set with the signed set.  The
 * comparison always consumes every flag and all five 32-byte slots.
 */
HRESULT WINAPI appx_signature_verify_digest_set(
    const APPX_SIGNATURE *signature,
    const struct appx_signature_digest_set *recalculated );
HRESULT WINAPI appx_signature_compare_digest_sets(
    const struct appx_signature_digest_set *signed_set,
    const struct appx_signature_digest_set *recalculated );

#endif /* __WINE_APPXSVC_SIGNATURE_H */
