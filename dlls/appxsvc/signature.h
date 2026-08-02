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
#include "wincrypt.h"
#include "winerror.h"
#include "wine/appxsvc.h"

#define APPX_SIGNATURE_MAX_SIZE                    (2 * 1024 * 1024)
#define APPX_SIGNATURE_SHA256_SIZE                 32
#define APPX_SIGNATURE_CERTIFICATE_ID_SIZE          APPX_SIGNATURE_SHA256_SIZE

#define APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN 0x00000001
#define APPX_SIGNATURE_VERIFY_BUNDLE                0x00000002

#define APPX_SIGNATURE_ESS_CERT_ID_V1               1
#define APPX_SIGNATURE_ESS_CERT_ID_V2               2

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
 * verify the signer's certificate chain.  The normal policy first establishes
 * a complete trusted, time-valid, usage-valid chain without URL retrieval.
 * Only that trusted chain may enter a second, ten-second-bounded online
 * revocation pass, which checks every non-root certificate and fails closed
 * when revocation status is unavailable.
 *
 * ALLOW_UNTRUSTED_CHAIN is a developer/test policy, not an installation trust
 * policy.  It relaxes only untrusted-root/incomplete-chain errors in the first
 * no-network pass and never starts the online pass.  It does not relax malformed
 * certificates, unsupported critical extensions, time, usage, KeyUsage, TSA
 * EKU, CMS signature, timestamp signer binding, or content-digest failures.
 * Since no revocation network access is permitted in this mode, callers must
 * not treat its success as a revocation assertion.
 */
HRESULT WINAPI appx_signature_parse_and_verify( const BYTE *data, SIZE_T size,
                                                UINT32 flags,
                                                APPX_SIGNATURE **signature );
void WINAPI appx_signature_free( APPX_SIGNATURE *signature );

const struct appx_signature_digest_set * WINAPI appx_signature_get_digest_set(
    const APPX_SIGNATURE *signature );
const WCHAR * WINAPI appx_signature_get_signer_subject( const APPX_SIGNATURE *signature );
/*
 * Return the SHA-256 digest of the exact DER signer certificate selected by
 * the CMS signer identifier.  Publisher names alone are not a signer identity:
 * bundle and inner-package trust binding must compare this value.
 */
HRESULT WINAPI appx_signature_get_signer_certificate_id(
    const APPX_SIGNATURE *signature, BYTE *certificate_id, UINT32 size );
/* Bind a manifest Identity Publisher DN to the verified signer subject. */
HRESULT WINAPI appx_signature_check_publisher( const APPX_SIGNATURE *signature,
                                               const WCHAR *publisher );

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
 * Select the SIP subject type while retaining the same strict DER grammar.
 * The default form accepts package signatures only; the _ex form accepts
 * APPX_SIGNATURE_VERIFY_BUNDLE to require the bundle SIP GUID instead.
 */
HRESULT WINAPI appx_signature_decode_indirect_data_ex(
    const BYTE *data, SIZE_T size, UINT32 flags,
    struct appx_signature_digest_set *set );

/*
 * Strict leaf and RFC3161 helpers used by the verifier.  They are private DLL
 * exports so their hostile-input and policy decisions can be tested without a
 * machine certificate store or network dependency.
 */
HRESULT WINAPI appx_signature_validate_leaf_extensions(
    const CERT_EXTENSION *extensions, UINT32 count, BOOL timestamp_signer );
HRESULT WINAPI appx_signature_select_ess_attribute(
    UINT32 version1_count, UINT32 version2_count, UINT32 *version );
HRESULT WINAPI appx_signature_validate_ess_certificate(
    const BYTE *attribute, UINT32 attribute_size, UINT32 version,
    const BYTE *certificate, UINT32 certificate_size );
HRESULT WINAPI appx_signature_get_chain_policy(
    UINT32 flags, BOOL online, DWORD time_flags, DWORD *chain_flags,
    DWORD *url_timeout );
HRESULT WINAPI appx_signature_evaluate_chain_status(
    UINT32 flags, BOOL online, HRESULT policy_error, DWORD trust_errors );

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

/*
 * Recalculate the package digests that are signed by AppxSignature.p7x.
 * AXPC and AXCD are computed from the raw ZIP file-record and central-directory
 * byte image; AXCT, AXBM, and optional AXCI are computed from the verified
 * uncompressed part streams.
 * The _ex form accepts an optional cancellation event that must remain valid
 * until the call returns.  The legacy form behaves as if that event were NULL.
 */
HRESULT WINAPI appx_archive_calculate_digest_set(
    WINE_APPX_ARCHIVE *archive, struct appx_signature_digest_set *set );
HRESULT WINAPI appx_archive_calculate_digest_set_ex(
    WINE_APPX_ARCHIVE *archive, HANDLE cancel_event,
    struct appx_signature_digest_set *set );

#endif /* __WINE_APPXSVC_SIGNATURE_H */
