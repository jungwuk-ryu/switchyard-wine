/*
 * AppX package signature verification tests
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
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "wintrust.h"

#include "wine/test.h"

#include "../signature.h"

#define DIGEST_RECORD_SIZE (4 + APPX_SIGNATURE_SHA256_SIZE)
#define BASE_DIGEST_SIZE (4 + 4 * DIGEST_RECORD_SIZE)
#define FULL_DIGEST_SIZE (BASE_DIGEST_SIZE + DIGEST_RECORD_SIZE)

static HRESULT (WINAPI *p_appx_signature_parse_and_verify)(
    const BYTE *, SIZE_T, UINT32, APPX_SIGNATURE ** );
static void (WINAPI *p_appx_signature_free)( APPX_SIGNATURE * );
static const struct appx_signature_digest_set *(WINAPI
    *p_appx_signature_get_digest_set)( const APPX_SIGNATURE * );
static const WCHAR *(WINAPI *p_appx_signature_get_signer_subject)(
    const APPX_SIGNATURE * );
static HRESULT (WINAPI *p_appx_signature_get_signer_certificate_id)(
    const APPX_SIGNATURE *, BYTE *, UINT32 );
static HRESULT (WINAPI *p_appx_signature_check_publisher)(
    const APPX_SIGNATURE *, const WCHAR * );
static HRESULT (WINAPI *p_appx_signature_decode_digest_set)(
    const BYTE *, SIZE_T, struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_decode_indirect_data)(
    const BYTE *, SIZE_T, struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_decode_indirect_data_ex)(
    const BYTE *, SIZE_T, UINT32, struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_compare_digest_sets)(
    const struct appx_signature_digest_set *,
    const struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_verify_digest_set)(
    const APPX_SIGNATURE *, const struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_validate_leaf_extensions)(
    const CERT_EXTENSION *, UINT32, BOOL );
static HRESULT (WINAPI *p_appx_signature_select_ess_attribute)(
    UINT32, UINT32, UINT32 * );
static HRESULT (WINAPI *p_appx_signature_validate_ess_certificate)(
    const BYTE *, UINT32, UINT32, const BYTE *, UINT32 );
static HRESULT (WINAPI *p_appx_signature_get_chain_policy)(
    UINT32, BOOL, DWORD, DWORD *, DWORD * );
static HRESULT (WINAPI *p_appx_signature_evaluate_chain_status)(
    UINT32, BOOL, HRESULT, DWORD );

/*
 * AppxSignature.p7x from microsoft/msix-packaging
 * src/test/testData/unpack/SignedUntrustedCert-CERT_E_CHAINING.appx at
 * efeb9dad695a200c2beaddcba54a52c8320bd135.  The upstream repository is
 * MIT-licensed.  SHA-256:
 * 333d06e0f658b35b85a6b4b289b59ea45ce7aa7ecf55fd29491b78ee8d29a020.
 */
static const char signed_untrusted_signature_base64[] =
    "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";

static int base64_value( char ch )
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static BOOL decode_fixture_base64( const char *text, BYTE *output,
                                   SIZE_T capacity, SIZE_T *output_size )
{
    UINT32 accumulator = 0, bits = 0;
    SIZE_T size = 0;
    int value;

    while (*text && *text != '=')
    {
        if ((value = base64_value( *text++ )) < 0) return FALSE;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (size >= capacity) return FALSE;
            output[size++] = accumulator >> bits;
            accumulator &= (1u << bits) - 1;
        }
    }
    while (*text == '=') text++;
    if (*text || bits >= 6 || accumulator) return FALSE;
    *output_size = size;
    return TRUE;
}

static void put_u32( BYTE *data, DWORD value )
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static void make_digest_blob( BYTE blob[FULL_DIGEST_SIZE], BOOL code_integrity )
{
    static const DWORD tags[] =
    {
        0x43505841, /* AXPC */
        0x44435841, /* AXCD */
        0x54435841, /* AXCT */
        0x4d425841, /* AXBM */
        0x49435841  /* AXCI */
    };
    UINT32 count = code_integrity ? 5 : 4;
    UINT32 i, j;

    memset( blob, 0xcc, FULL_DIGEST_SIZE );
    put_u32( blob, 0x58505041 ); /* APPX */
    for (i = 0; i < count; i++)
    {
        BYTE *record = blob + 4 + i * DIGEST_RECORD_SIZE;

        put_u32( record, tags[i] );
        for (j = 0; j < APPX_SIGNATURE_SHA256_SIZE; j++)
            record[4 + j] = i * APPX_SIGNATURE_SHA256_SIZE + j;
    }
}

struct indirect_fixture
{
    BYTE data[265];
    SIZE_T size;
    SIZE_T sip_oid;
    SIZE_T version;
    SIZE_T guid;
    SIZE_T reserved;
    SIZE_T sha_oid;
    SIZE_T null_tag;
    SIZE_T digest_tag;
};

static void append_fixture_bytes( struct indirect_fixture *fixture,
                                  const void *data, SIZE_T size )
{
    memcpy( fixture->data + fixture->size, data, size );
    fixture->size += size;
}

static void append_fixture_byte( struct indirect_fixture *fixture, BYTE value )
{
    fixture->data[fixture->size++] = value;
}

static void make_indirect_fixture( struct indirect_fixture *fixture,
                                   BOOL code_integrity )
{
    static const BYTE sip_oid[] =
        {0x2b,0x06,0x01,0x04,0x01,0x82,0x37,0x02,0x01,0x1e};
    static const BYTE sha_oid[] =
        {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01};
    static const BYTE sip_guid[] =
        {0x4b,0xdf,0xc5,0x0a,0x07,0xce,0xe2,0x4d,
         0xb7,0x6e,0x23,0xc8,0x39,0xa0,0x9f,0xd1};
    static const BYTE version[] = {0x01,0x01,0x00,0x00};
    BYTE digest[FULL_DIGEST_SIZE];
    UINT32 i;

    memset( fixture, 0, sizeof(*fixture) );
    make_digest_blob( digest, code_integrity );

    append_fixture_byte( fixture, 0x30 );
    if (code_integrity)
    {
        append_fixture_byte( fixture, 0x82 );
        append_fixture_byte( fixture, 0x01 );
        append_fixture_byte( fixture, 0x04 );
    }
    else
    {
        append_fixture_byte( fixture, 0x81 );
        append_fixture_byte( fixture, 0xe0 );
    }
    append_fixture_byte( fixture, 0x30 );
    append_fixture_byte( fixture, 0x35 );
    append_fixture_byte( fixture, 0x06 );
    append_fixture_byte( fixture, sizeof(sip_oid) );
    fixture->sip_oid = fixture->size;
    append_fixture_bytes( fixture, sip_oid, sizeof(sip_oid) );
    append_fixture_byte( fixture, 0x30 );
    append_fixture_byte( fixture, 0x27 );
    append_fixture_byte( fixture, 0x02 );
    append_fixture_byte( fixture, sizeof(version) );
    fixture->version = fixture->size;
    append_fixture_bytes( fixture, version, sizeof(version) );
    append_fixture_byte( fixture, 0x04 );
    append_fixture_byte( fixture, sizeof(sip_guid) );
    fixture->guid = fixture->size;
    append_fixture_bytes( fixture, sip_guid, sizeof(sip_guid) );
    fixture->reserved = fixture->size + 2;
    for (i = 0; i < 5; i++)
    {
        append_fixture_byte( fixture, 0x02 );
        append_fixture_byte( fixture, 0x01 );
        append_fixture_byte( fixture, 0x00 );
    }
    append_fixture_byte( fixture, 0x30 );
    append_fixture_byte( fixture, 0x81 );
    append_fixture_byte( fixture, code_integrity ? 0xca : 0xa6 );
    append_fixture_byte( fixture, 0x30 );
    append_fixture_byte( fixture, 0x0d );
    append_fixture_byte( fixture, 0x06 );
    append_fixture_byte( fixture, sizeof(sha_oid) );
    fixture->sha_oid = fixture->size;
    append_fixture_bytes( fixture, sha_oid, sizeof(sha_oid) );
    fixture->null_tag = fixture->size;
    append_fixture_byte( fixture, 0x05 );
    append_fixture_byte( fixture, 0x00 );
    fixture->digest_tag = fixture->size;
    append_fixture_byte( fixture, 0x04 );
    append_fixture_byte( fixture, 0x81 );
    append_fixture_byte( fixture, code_integrity ? FULL_DIGEST_SIZE : BASE_DIGEST_SIZE );
    append_fixture_bytes( fixture, digest,
                          code_integrity ? FULL_DIGEST_SIZE : BASE_DIGEST_SIZE );
}

static void test_digest_sets( void )
{
    struct appx_signature_digest_set base, full, copy;
    BYTE blob[FULL_DIGEST_SIZE], changed[FULL_DIGEST_SIZE];
    HRESULT hr;
    UINT32 i;

    make_digest_blob( blob, FALSE );
    memset( &base, 0xcc, sizeof(base) );
    hr = p_appx_signature_decode_digest_set( blob, BASE_DIGEST_SIZE, &base );
    ok( hr == S_OK, "got %#lx.\n", hr );
    ok( base.flags == APPX_SIGNATURE_DIGEST_REQUIRED, "got flags %#x.\n", base.flags );
    for (i = 0; i < APPX_SIGNATURE_SHA256_SIZE; i++)
    {
        ok( base.package_contents[i] == i, "AXPC byte %u is %#x.\n",
            i, base.package_contents[i] );
        ok( base.central_directory[i] == 32 + i, "AXCD byte %u is %#x.\n",
            i, base.central_directory[i] );
        ok( base.content_types[i] == 64 + i, "AXCT byte %u is %#x.\n",
            i, base.content_types[i] );
        ok( base.block_map[i] == 96 + i, "AXBM byte %u is %#x.\n",
            i, base.block_map[i] );
        ok( !base.code_integrity[i], "AXCI byte %u is %#x.\n",
            i, base.code_integrity[i] );
    }

    make_digest_blob( blob, TRUE );
    memset( &full, 0xcc, sizeof(full) );
    hr = p_appx_signature_decode_digest_set( blob, FULL_DIGEST_SIZE, &full );
    ok( hr == S_OK, "got %#lx.\n", hr );
    ok( full.flags == APPX_SIGNATURE_DIGEST_ALL, "got flags %#x.\n", full.flags );
    for (i = 0; i < APPX_SIGNATURE_SHA256_SIZE; i++)
        ok( full.code_integrity[i] == 128 + i, "AXCI byte %u is %#x.\n",
            i, full.code_integrity[i] );

    copy = full;
    hr = p_appx_signature_compare_digest_sets( &full, &copy );
    ok( hr == S_OK, "got %#lx.\n", hr );
    hr = p_appx_signature_compare_digest_sets( &base, &full );
    ok( hr == APPX_E_DIGEST_MISMATCH, "AXCI presence returned %#lx.\n", hr );
    for (i = 0; i < sizeof(copy); i++)
    {
        BYTE *bytes = (BYTE *)&copy;

        bytes[i] ^= 0x80;
        hr = p_appx_signature_compare_digest_sets( &full, &copy );
        ok( hr == APPX_E_DIGEST_MISMATCH, "byte %u returned %#lx.\n", i, hr );
        bytes[i] ^= 0x80;
    }
    copy.flags |= 0x80000000;
    hr = p_appx_signature_compare_digest_sets( &full, &copy );
    ok( hr == APPX_E_DIGEST_MISMATCH, "unknown flag returned %#lx.\n", hr );
    ok( p_appx_signature_compare_digest_sets( NULL, &copy ) == E_INVALIDARG,
        "accepted a NULL signed set.\n" );
    ok( p_appx_signature_compare_digest_sets( &full, NULL ) == E_INVALIDARG,
        "accepted a NULL recalculated set.\n" );

    memcpy( changed, blob, sizeof(changed) );
    put_u32( changed, 0x11111111 );
    memset( &copy, 0xcc, sizeof(copy) );
    hr = p_appx_signature_decode_digest_set( changed, FULL_DIGEST_SIZE, &copy );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "bad APPX tag returned %#lx.\n", hr );
    ok( !copy.flags, "failure left flags %#x.\n", copy.flags );

    for (i = 0; i < 5; i++)
    {
        memcpy( changed, blob, sizeof(changed) );
        put_u32( changed + 4 + i * DIGEST_RECORD_SIZE, 0x43505841 );
        if (!i) put_u32( changed + 4, 0x44435841 );
        hr = p_appx_signature_decode_digest_set( changed, FULL_DIGEST_SIZE, &copy );
        ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA,
            "wrong/duplicate tag %u returned %#lx.\n", i, hr );
    }

    memcpy( changed, blob, sizeof(changed) );
    put_u32( changed + 4 + 2 * DIGEST_RECORD_SIZE, 0x11111111 );
    hr = p_appx_signature_decode_digest_set( changed, FULL_DIGEST_SIZE, &copy );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "unknown digest tag returned %#lx.\n", hr );

    memcpy( changed, blob, sizeof(changed) );
    memcpy( changed + 4, blob + 4 + DIGEST_RECORD_SIZE, DIGEST_RECORD_SIZE );
    memcpy( changed + 4 + DIGEST_RECORD_SIZE, blob + 4, DIGEST_RECORD_SIZE );
    hr = p_appx_signature_decode_digest_set( changed, FULL_DIGEST_SIZE, &copy );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "reordered tags returned %#lx.\n", hr );

    hr = p_appx_signature_decode_digest_set( blob, BASE_DIGEST_SIZE - 1, &copy );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "short digest returned %#lx.\n", hr );
    hr = p_appx_signature_decode_digest_set( blob, BASE_DIGEST_SIZE + 1, &copy );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "odd digest returned %#lx.\n", hr );
    hr = p_appx_signature_decode_digest_set( blob, FULL_DIGEST_SIZE - 1, &copy );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "short AXCI digest returned %#lx.\n", hr );
    ok( p_appx_signature_decode_digest_set( NULL, FULL_DIGEST_SIZE, &copy ) == E_INVALIDARG,
        "accepted NULL data.\n" );
    ok( p_appx_signature_decode_digest_set( blob, FULL_DIGEST_SIZE, NULL ) == E_INVALIDARG,
        "accepted NULL output.\n" );
}

static void test_indirect_data( void )
{
    static const BYTE package_sip_guid[] =
        {0x4b,0xdf,0xc5,0x0a,0x07,0xce,0xe2,0x4d,
         0xb7,0x6e,0x23,0xc8,0x39,0xa0,0x9f,0xd1};
    static const BYTE bundle_sip_guid[] =
        {0xb3,0x58,0x5f,0x0f,0xde,0xaa,0x9a,0x4b,
         0xa4,0x34,0x95,0x74,0x2d,0x92,0xec,0xeb};
    struct appx_signature_digest_set set;
    struct indirect_fixture fixture;
    HRESULT hr;

    make_indirect_fixture( &fixture, FALSE );
    ok( fixture.size == 227, "base indirect data has size %Iu.\n", fixture.size );
    hr = p_appx_signature_decode_indirect_data( fixture.data, fixture.size, &set );
    ok( hr == S_OK, "base indirect data returned %#lx.\n", hr );
    ok( set.flags == APPX_SIGNATURE_DIGEST_REQUIRED, "got flags %#x.\n", set.flags );
    hr = p_appx_signature_decode_indirect_data_ex(
        fixture.data, fixture.size, 0, &set );
    ok( hr == S_OK, "typed package indirect data returned %#lx.\n", hr );

    memcpy( fixture.data + fixture.guid, bundle_sip_guid,
            sizeof(bundle_sip_guid) );
    hr = p_appx_signature_decode_indirect_data(
        fixture.data, fixture.size, &set );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA,
        "package decoder accepted bundle SIP GUID, got %#lx.\n", hr );
    hr = p_appx_signature_decode_indirect_data_ex(
        fixture.data, fixture.size, APPX_SIGNATURE_VERIFY_BUNDLE, &set );
    ok( hr == S_OK, "typed bundle indirect data returned %#lx.\n", hr );
    hr = p_appx_signature_decode_indirect_data_ex(
        fixture.data, fixture.size, 0, &set );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA,
        "typed package decoder accepted bundle SIP GUID, got %#lx.\n", hr );
    hr = p_appx_signature_decode_indirect_data_ex(
        fixture.data, fixture.size, 0x80000000, &set );
    ok( hr == E_INVALIDARG, "unknown SIP type flag returned %#lx.\n", hr );
    memcpy( fixture.data + fixture.guid, package_sip_guid,
            sizeof(package_sip_guid) );
    hr = p_appx_signature_decode_indirect_data_ex(
        fixture.data, fixture.size, APPX_SIGNATURE_VERIFY_BUNDLE, &set );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA,
        "bundle decoder accepted package SIP GUID, got %#lx.\n", hr );

    make_indirect_fixture( &fixture, TRUE );
    ok( fixture.size == 264, "AXCI indirect data has size %Iu.\n", fixture.size );
    hr = p_appx_signature_decode_indirect_data( fixture.data, fixture.size, &set );
    ok( hr == S_OK, "AXCI indirect data returned %#lx.\n", hr );
    ok( set.flags == APPX_SIGNATURE_DIGEST_ALL, "got flags %#x.\n", set.flags );

#define CHECK_MUTATION(member, description) do                                           \
    {                                                                                     \
        fixture.data[fixture.member] ^= 1;                                                 \
        memset( &set, 0xcc, sizeof(set) );                                                 \
        hr = p_appx_signature_decode_indirect_data( fixture.data, fixture.size, &set );     \
        ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "%s returned %#lx.\n",                   \
            description, hr );                                                             \
        ok( !set.flags, "%s left flags %#x.\n", description, set.flags );                  \
        fixture.data[fixture.member] ^= 1;                                                 \
    } while (0)

    CHECK_MUTATION( sip_oid, "wrong SIP OID" );
    CHECK_MUTATION( version, "wrong SIP version" );
    CHECK_MUTATION( guid, "wrong SIP GUID" );
    CHECK_MUTATION( reserved, "nonzero SIP reserved value" );
    CHECK_MUTATION( sha_oid, "wrong indirect digest algorithm" );
    CHECK_MUTATION( null_tag, "wrong indirect algorithm parameters" );
    CHECK_MUTATION( digest_tag, "wrong indirect digest tag" );
#undef CHECK_MUTATION

    hr = p_appx_signature_decode_indirect_data( fixture.data, fixture.size - 1, &set );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "truncated indirect data returned %#lx.\n", hr );
    fixture.data[fixture.size] = 0;
    hr = p_appx_signature_decode_indirect_data( fixture.data, fixture.size + 1, &set );
    ok( hr == APPX_E_INVALID_SIP_CLIENT_DATA, "trailing indirect data returned %#lx.\n", hr );
    ok( p_appx_signature_decode_indirect_data( NULL, fixture.size, &set ) == E_INVALIDARG,
        "accepted NULL indirect data.\n" );
    ok( p_appx_signature_decode_indirect_data( fixture.data, fixture.size, NULL ) == E_INVALIDARG,
        "accepted NULL indirect output.\n" );
}

static void set_extension( CERT_EXTENSION *extension, const char *oid,
                           BOOL critical, BYTE *value, DWORD size )
{
    memset( extension, 0, sizeof(*extension) );
    extension->pszObjId = (char *)oid;
    extension->fCritical = critical;
    extension->Value.pbData = value;
    extension->Value.cbData = size;
}

static void test_leaf_extensions( void )
{
    static BYTE digital_signature[] = {0x03,0x02,0x07,0x80};
    static BYTE non_repudiation[] = {0x03,0x02,0x06,0x40};
    static BYTE key_encipherment[] = {0x03,0x02,0x05,0x20};
    static BYTE malformed_unused_bits[] = {0x03,0x02,0x07,0x81};
    static BYTE noncanonical_named_bits[] = {0x03,0x03,0x07,0x80,0x00};
    static BYTE timestamp_eku[] =
        {0x30,0x0a,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x08};
    static BYTE code_signing_eku[] =
        {0x30,0x0a,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x03};
    static BYTE multiple_eku[] =
    {
        0x30,0x14,
          0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x08,
          0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x03
    };
    static BYTE malformed_eku[] = {0x30,0x03,0x06,0x01,0x80};
    CERT_EXTENSION extensions[2];
    HRESULT hr;

    hr = p_appx_signature_validate_leaf_extensions( NULL, 0, FALSE );
    ok( hr == S_OK, "absent package extensions returned %#lx.\n", hr );
    hr = p_appx_signature_validate_leaf_extensions( NULL, 0, TRUE );
    ok( hr == CERT_E_WRONG_USAGE,
        "absent timestamp EKU returned %#lx.\n", hr );

    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   digital_signature, sizeof(digital_signature) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, FALSE );
    ok( hr == S_OK, "digitalSignature KeyUsage returned %#lx.\n", hr );
    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   non_repudiation, sizeof(non_repudiation) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, FALSE );
    ok( hr == S_OK, "nonRepudiation KeyUsage returned %#lx.\n", hr );
    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   key_encipherment, sizeof(key_encipherment) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, FALSE );
    ok( hr == CERT_E_WRONG_USAGE,
        "encipherment-only KeyUsage returned %#lx.\n", hr );
    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   malformed_unused_bits, sizeof(malformed_unused_bits) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, FALSE );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "malformed KeyUsage returned %#lx.\n", hr );
    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   noncanonical_named_bits,
                   sizeof(noncanonical_named_bits) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, FALSE );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "noncanonical KeyUsage returned %#lx.\n", hr );

    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   digital_signature, sizeof(digital_signature) );
    extensions[1] = extensions[0];
    hr = p_appx_signature_validate_leaf_extensions( extensions, 2, FALSE );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "duplicate KeyUsage returned %#lx.\n", hr );

    set_extension( extensions, szOID_ENHANCED_KEY_USAGE, TRUE,
                   timestamp_eku, sizeof(timestamp_eku) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, TRUE );
    ok( hr == S_OK, "strict timestamp EKU returned %#lx.\n", hr );
    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   digital_signature, sizeof(digital_signature) );
    set_extension( extensions + 1, szOID_ENHANCED_KEY_USAGE, TRUE,
                   timestamp_eku, sizeof(timestamp_eku) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 2, TRUE );
    ok( hr == S_OK, "timestamp signing KeyUsage returned %#lx.\n", hr );
    set_extension( extensions, szOID_KEY_USAGE, FALSE,
                   key_encipherment, sizeof(key_encipherment) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 2, TRUE );
    ok( hr == CERT_E_WRONG_USAGE,
        "timestamp encipherment-only KeyUsage returned %#lx.\n", hr );

    set_extension( extensions, szOID_ENHANCED_KEY_USAGE, TRUE,
                   timestamp_eku, sizeof(timestamp_eku) );
    extensions[0].fCritical = FALSE;
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, TRUE );
    ok( hr == CERT_E_WRONG_USAGE,
        "noncritical timestamp EKU returned %#lx.\n", hr );
    set_extension( extensions, szOID_ENHANCED_KEY_USAGE, TRUE,
                   code_signing_eku, sizeof(code_signing_eku) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, TRUE );
    ok( hr == CERT_E_WRONG_USAGE,
        "wrong timestamp EKU returned %#lx.\n", hr );
    set_extension( extensions, szOID_ENHANCED_KEY_USAGE, TRUE,
                   multiple_eku, sizeof(multiple_eku) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, TRUE );
    ok( hr == CERT_E_WRONG_USAGE,
        "multi-purpose timestamp EKU returned %#lx.\n", hr );
    set_extension( extensions, szOID_ENHANCED_KEY_USAGE, TRUE,
                   malformed_eku, sizeof(malformed_eku) );
    hr = p_appx_signature_validate_leaf_extensions( extensions, 1, TRUE );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "malformed timestamp EKU returned %#lx.\n", hr );
    set_extension( extensions, szOID_ENHANCED_KEY_USAGE, TRUE,
                   timestamp_eku, sizeof(timestamp_eku) );
    extensions[1] = extensions[0];
    hr = p_appx_signature_validate_leaf_extensions( extensions, 2, TRUE );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "duplicate timestamp EKU returned %#lx.\n", hr );
}

static void test_ess_certificate_binding( void )
{
    static const BYTE certificate[] = {'a','b','c'};
    static const BYTE sha1[] =
        {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
         0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    static const BYTE sha256[] =
        {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
         0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
         0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
         0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    BYTE version1[28] = {0x30,0x1a,0x30,0x18,0x30,0x16,0x04,0x14};
    BYTE version2_default[40] =
        {0x30,0x26,0x30,0x24,0x30,0x22,0x04,0x20};
    BYTE version2_explicit[53] =
    {
        0x30,0x33,0x30,0x31,0x30,0x2f,
          0x30,0x0b,0x06,0x09,
            0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
          0x04,0x20
    };
    BYTE version2_explicit_null[55] =
    {
        0x30,0x35,0x30,0x33,0x30,0x31,
          0x30,0x0d,0x06,0x09,
            0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
            0x05,0x00,
          0x04,0x20
    };
    UINT32 version;
    HRESULT hr;

    memcpy( version1 + 8, sha1, sizeof(sha1) );
    memcpy( version2_default + 8, sha256, sizeof(sha256) );
    memcpy( version2_explicit + 21, sha256, sizeof(sha256) );
    memcpy( version2_explicit_null + 23, sha256, sizeof(sha256) );

    version = 0xdeadbeef;
    hr = p_appx_signature_select_ess_attribute( 1, 0, &version );
    ok( hr == S_OK && version == APPX_SIGNATURE_ESS_CERT_ID_V1,
        "ESSCertID selection returned %#lx, version %u.\n", hr, version );
    hr = p_appx_signature_select_ess_attribute( 0, 1, &version );
    ok( hr == S_OK && version == APPX_SIGNATURE_ESS_CERT_ID_V2,
        "ESSCertIDv2 selection returned %#lx, version %u.\n", hr, version );
    hr = p_appx_signature_select_ess_attribute( 0, 0, &version );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE && !version,
        "missing ESS attribute returned %#lx, version %u.\n", hr, version );
    hr = p_appx_signature_select_ess_attribute( 2, 0, &version );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE && !version,
        "duplicate ESSCertID returned %#lx, version %u.\n", hr, version );
    hr = p_appx_signature_select_ess_attribute( 1, 1, &version );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE && !version,
        "mixed ESS attributes returned %#lx, version %u.\n", hr, version );

    hr = p_appx_signature_validate_ess_certificate(
        version1, sizeof(version1), APPX_SIGNATURE_ESS_CERT_ID_V1,
        certificate, sizeof(certificate) );
    ok( hr == S_OK, "ESSCertID binding returned %#lx.\n", hr );
    hr = p_appx_signature_validate_ess_certificate(
        version2_default, sizeof(version2_default),
        APPX_SIGNATURE_ESS_CERT_ID_V2, certificate, sizeof(certificate) );
    ok( hr == S_OK, "default ESSCertIDv2 binding returned %#lx.\n", hr );
    hr = p_appx_signature_validate_ess_certificate(
        version2_explicit, sizeof(version2_explicit),
        APPX_SIGNATURE_ESS_CERT_ID_V2, certificate, sizeof(certificate) );
    ok( hr == S_OK, "explicit SHA-256 ESSCertIDv2 returned %#lx.\n", hr );
    hr = p_appx_signature_validate_ess_certificate(
        version2_explicit_null, sizeof(version2_explicit_null),
        APPX_SIGNATURE_ESS_CERT_ID_V2, certificate, sizeof(certificate) );
    ok( hr == S_OK,
        "explicit SHA-256 NULL ESSCertIDv2 returned %#lx.\n", hr );

    version1[8] ^= 1;
    hr = p_appx_signature_validate_ess_certificate(
        version1, sizeof(version1), APPX_SIGNATURE_ESS_CERT_ID_V1,
        certificate, sizeof(certificate) );
    ok( hr == TRUST_E_SUBJECT_NOT_TRUSTED,
        "mismatched ESSCertID returned %#lx.\n", hr );
    version1[8] ^= 1;
    hr = p_appx_signature_validate_ess_certificate(
        version1, sizeof(version1) - 1, APPX_SIGNATURE_ESS_CERT_ID_V1,
        certificate, sizeof(certificate) );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "truncated ESSCertID returned %#lx.\n", hr );

    version2_explicit[18] ^= 1;
    hr = p_appx_signature_validate_ess_certificate(
        version2_explicit, sizeof(version2_explicit),
        APPX_SIGNATURE_ESS_CERT_ID_V2, certificate, sizeof(certificate) );
    ok( hr == TRUST_E_MALFORMED_SIGNATURE,
        "unsupported ESSCertIDv2 algorithm returned %#lx.\n", hr );
    version2_explicit[18] ^= 1;
    hr = p_appx_signature_validate_ess_certificate(
        version2_default, sizeof(version2_default), 0,
        certificate, sizeof(certificate) );
    ok( hr == E_INVALIDARG, "invalid ESS version returned %#lx.\n", hr );
}

static void test_chain_policy( void )
{
    DWORD flags = 0xdeadbeef, timeout = 0xdeadbeef;
    HRESULT hr;

    hr = p_appx_signature_get_chain_policy( 0, FALSE, 0,
                                            &flags, &timeout );
    ok( hr == S_OK, "offline chain policy returned %#lx.\n", hr );
    ok( flags == (CERT_CHAIN_CACHE_END_CERT |
                  CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE |
                  CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL),
        "offline chain flags are %#lx.\n", flags );
    ok( !timeout, "offline chain timeout is %lu.\n", timeout );

    hr = p_appx_signature_get_chain_policy(
        0, TRUE, CERT_CHAIN_TIMESTAMP_TIME, &flags, &timeout );
    ok( hr == S_OK, "online chain policy returned %#lx.\n", hr );
    ok( flags == (CERT_CHAIN_CACHE_END_CERT |
                  CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE |
                  CERT_CHAIN_TIMESTAMP_TIME |
                  CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                  CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT),
        "online chain flags are %#lx.\n", flags );
    ok( timeout == 10000, "online chain timeout is %lu.\n", timeout );

    flags = timeout = 0xdeadbeef;
    hr = p_appx_signature_get_chain_policy(
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, TRUE, 0,
        &flags, &timeout );
    ok( hr == S_FALSE, "developer online policy returned %#lx.\n", hr );
    ok( !flags && !timeout,
        "developer online policy returned flags %#lx, timeout %lu.\n",
        flags, timeout );

    hr = p_appx_signature_evaluate_chain_status(
        0, FALSE, CERT_E_UNTRUSTEDROOT,
        CERT_TRUST_IS_UNTRUSTED_ROOT );
    ok( hr == CERT_E_UNTRUSTEDROOT,
        "normal untrusted gate returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, FALSE,
        CERT_E_UNTRUSTEDROOT,
        CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN );
    ok( hr == S_OK, "developer untrusted chain returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, FALSE, 0,
        CERT_TRUST_IS_UNTRUSTED_ROOT |
        CERT_TRUST_IS_NOT_VALID_FOR_USAGE );
    ok( hr == CERT_E_WRONG_USAGE,
        "developer wrong-usage chain returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, FALSE, 0,
        CERT_TRUST_IS_UNTRUSTED_ROOT |
        CERT_TRUST_HAS_NOT_SUPPORTED_CRITICAL_EXT );
    ok( hr == CERT_E_CRITICAL,
        "developer critical-extension chain returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, TRUE, 0, 0 );
    ok( hr == E_INVALIDARG,
        "developer online chain status returned %#lx.\n", hr );

    hr = p_appx_signature_evaluate_chain_status(
        0, TRUE, CRYPT_E_REVOKED, 0 );
    ok( hr == CERT_E_REVOKED, "revoked policy returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        0, TRUE, 0, CERT_TRUST_IS_REVOKED );
    ok( hr == CERT_E_REVOKED, "revoked status returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        0, TRUE, 0, CERT_TRUST_REVOCATION_STATUS_UNKNOWN );
    ok( hr == CRYPT_E_REVOCATION_OFFLINE,
        "unknown revocation status returned %#lx.\n", hr );
    hr = p_appx_signature_evaluate_chain_status(
        0, TRUE, CERT_E_REVOCATION_FAILURE,
        CERT_TRUST_IS_OFFLINE_REVOCATION );
    ok( hr == CRYPT_E_REVOCATION_OFFLINE,
        "offline revocation policy returned %#lx.\n", hr );
}

/*
 * A deliberately certificate-free SignedData envelope.  Mutations exercise
 * the strict CMS/DER pre-parser without depending on a machine certificate
 * store or a private key.
 */
static const BYTE cms_shape[] =
{
    0x30, 0x39,
      0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02,
      0xa0, 0x2c,
        0x30, 0x2a,
          0x02, 0x01, 0x01,
          0x31, 0x0f,
            0x30, 0x0d,
              0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
              0x05, 0x00,
          0x30, 0x10,
            0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04,
            0xa0, 0x02, 0x30, 0x00,
          0x31, 0x02, 0x30, 0x00
};

static void make_p7x( BYTE *p7x )
{
    put_u32( p7x, 0x58434b50 ); /* PKCX */
    memcpy( p7x + 4, cms_shape, sizeof(cms_shape) );
}

static void expect_rejected( const BYTE *data, SIZE_T size, const char *description )
{
    APPX_SIGNATURE *signature = (APPX_SIGNATURE *)0xdeadbeef;
    HRESULT hr = p_appx_signature_parse_and_verify(
        data, size, APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature );

    ok( FAILED( hr ), "%s unexpectedly returned %#lx.\n", description, hr );
    ok( !signature, "%s returned signature %p.\n", description, signature );
}

static void test_cms_boundaries( void )
{
    BYTE p7x[4 + sizeof(cms_shape) + 2], changed[sizeof(p7x)];
    APPX_SIGNATURE *signature = (APPX_SIGNATURE *)0xdeadbeef;
    BYTE *oversized;
    HRESULT hr;
    SIZE_T i;

    make_p7x( p7x );
    expect_rejected( p7x, 4 + sizeof(cms_shape), "certificate-free CMS" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[0] ^= 1;
    expect_rejected( changed, 4 + sizeof(cms_shape), "bad PKCX" );

    for (i = 0; i < 4 + sizeof(cms_shape); i++)
        expect_rejected( p7x, i, "truncated p7x" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[4 + sizeof(cms_shape)] = 0;
    expect_rejected( changed, sizeof(changed), "DER trailing byte" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[4 + 12] ^= 1; /* signedData OID */
    expect_rejected( changed, 4 + sizeof(cms_shape), "wrong outer OID" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[4 + 34] ^= 1; /* SHA-256 OID */
    expect_rejected( changed, 4 + sizeof(cms_shape), "wrong digest algorithm" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[4 + 50] ^= 1; /* SPC_INDIRECT_DATA_OBJID */
    expect_rejected( changed, 4 + sizeof(cms_shape), "wrong content OID" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[4 + 1] = 0x37;
    changed[4 + 14] = 0x2a;
    changed[4 + 16] = 0x28;
    changed[4 + 56] = 0; /* Empty signerInfos SET. */
    expect_rejected( changed, 4 + sizeof(cms_shape) - 2, "zero CMS signers" );

    memcpy( changed, p7x, 4 + sizeof(cms_shape) );
    changed[4 + 1] = 0x3b;
    changed[4 + 14] = 0x2e;
    changed[4 + 16] = 0x2c;
    changed[4 + 56] = 4;
    changed[4 + sizeof(cms_shape)] = 0x30;
    changed[4 + sizeof(cms_shape) + 1] = 0;
    expect_rejected( changed, 4 + sizeof(cms_shape) + 2, "two CMS signers" );

    memcpy( changed, p7x, 4 + 55 );
    changed[4 + 1] = 0x3b;
    changed[4 + 14] = 0x2e;
    changed[4 + 16] = 0x2c;
    changed[4 + 38] = 0x12;
    changed[4 + 52] = 4;
    changed[4 + 55] = 0x30;
    changed[4 + 56] = 0;
    memcpy( changed + 4 + 57, p7x + 4 + 55, sizeof(cms_shape) - 55 );
    expect_rejected( changed, 4 + sizeof(cms_shape) + 2, "nested DER trailing element" );

    hr = p_appx_signature_parse_and_verify(
        p7x, 4 + sizeof(cms_shape), 0x80000000, &signature );
    ok( hr == E_INVALIDARG, "unknown flag returned %#lx.\n", hr );
    ok( !signature, "unknown flag returned signature %p.\n", signature );
    ok( p_appx_signature_parse_and_verify( p7x, 4 + sizeof(cms_shape), 0, NULL ) == E_POINTER,
        "accepted NULL output.\n" );
    signature = (APPX_SIGNATURE *)0xdeadbeef;
    hr = p_appx_signature_parse_and_verify( NULL, 0, 0, &signature );
    ok( hr == E_INVALIDARG, "NULL data returned %#lx.\n", hr );
    ok( !signature, "NULL data returned signature %p.\n", signature );

    oversized = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                           APPX_SIGNATURE_MAX_SIZE + 1 );
    ok( !!oversized, "failed to allocate oversized fixture.\n" );
    if (oversized)
    {
        signature = (APPX_SIGNATURE *)0xdeadbeef;
        hr = p_appx_signature_parse_and_verify(
            oversized, APPX_SIGNATURE_MAX_SIZE + 1, 0, &signature );
        ok( hr == E_INVALIDARG, "oversized p7x returned %#lx.\n", hr );
        ok( !signature, "oversized p7x returned signature %p.\n", signature );
        HeapFree( GetProcessHeap(), 0, oversized );
    }
}

static void test_real_signature( void )
{
    static const BYTE digest_prefix[] = { 'A', 'P', 'P', 'X', 'A', 'X', 'P', 'C' };
    static const BYTE rsa_algorithm[] =
        { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
          0x01, 0x01, 0x01, 0x05, 0x00 };
    struct appx_signature_digest_set recalculated;
    const struct appx_signature_digest_set *signed_set;
    BYTE signature_bytes[1655], tampered[1655];
    BYTE signer_id[APPX_SIGNATURE_CERTIFICATE_ID_SIZE];
    const WCHAR *subject;
    APPX_SIGNATURE *signature = NULL;
    SIZE_T signature_size = 0, i;
    HRESULT hr;

    ok( decode_fixture_base64( signed_untrusted_signature_base64,
                             signature_bytes, sizeof(signature_bytes),
                             &signature_size ),
        "failed to decode the signed fixture.\n" );
    ok( signature_size == sizeof(signature_bytes), "decoded %Iu bytes.\n",
        signature_size );
    if (signature_size != sizeof(signature_bytes)) return;

    hr = p_appx_signature_parse_and_verify(
        signature_bytes, signature_size, 0, &signature );
    ok( hr == CERT_E_CHAINING || hr == CERT_E_UNTRUSTEDROOT ||
        hr == CERT_E_UNTRUSTEDTESTROOT,
        "untrusted real AppX signature returned %#lx.\n", hr );
    ok( !signature, "untrusted real AppX signature returned object %p.\n",
        signature );

    hr = p_appx_signature_parse_and_verify(
        signature_bytes, signature_size,
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature );
    ok( hr == S_OK, "real AppX signature returned %#lx.\n", hr );
    ok( signature != NULL, "real AppX signature returned no object.\n" );
    if (!signature) return;

    signed_set = p_appx_signature_get_digest_set( signature );
    ok( signed_set != NULL, "real AppX signature returned no digest set.\n" );
    if (signed_set)
    {
        ok( signed_set->flags == APPX_SIGNATURE_DIGEST_REQUIRED,
            "got digest flags %#x.\n", signed_set->flags );
        ok( signed_set->package_contents[0] == 0xe8 &&
            signed_set->central_directory[0] == 0xf7 &&
            signed_set->content_types[0] == 0x53 &&
            signed_set->block_map[0] == 0x67,
            "unexpected real AppX digests.\n" );
        recalculated = *signed_set;
        hr = p_appx_signature_verify_digest_set( signature, &recalculated );
        ok( hr == S_OK, "matching real digest set returned %#lx.\n", hr );
    }
    subject = p_appx_signature_get_signer_subject( signature );
    ok( subject && wcsstr( subject, L"CN=Microsoft Corporation" ),
        "got signer subject %s.\n", wine_dbgstr_w( subject ) );
    memset( signer_id, 0, sizeof(signer_id) );
    hr = p_appx_signature_get_signer_certificate_id(
        signature, signer_id, sizeof(signer_id) );
    ok( hr == S_OK, "signer certificate id returned %#lx.\n", hr );
    for (i = 0; i < sizeof(signer_id); i++)
        if (signer_id[i]) break;
    ok( i != sizeof(signer_id), "signer certificate id is all zero.\n" );
    ok( p_appx_signature_get_signer_certificate_id(
            signature, signer_id, sizeof(signer_id) - 1 ) == E_INVALIDARG,
        "accepted a short signer certificate id buffer.\n" );
    hr = p_appx_signature_check_publisher(
        signature,
        L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" );
    ok( hr == S_OK, "matching publisher returned %#lx.\n", hr );
    hr = p_appx_signature_check_publisher(
        signature,
        L"CN=Wrong Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" );
    ok( hr == TRUST_E_SUBJECT_NOT_TRUSTED,
        "wrong publisher returned %#lx.\n", hr );
    hr = p_appx_signature_check_publisher( signature, L"CN=\"unterminated" );
    ok( hr == TRUST_E_SUBJECT_FORM_UNKNOWN,
        "malformed publisher returned %#lx.\n", hr );
    hr = p_appx_signature_check_publisher( signature, L"CN=A+O=B" );
    ok( hr == TRUST_E_SUBJECT_FORM_UNKNOWN,
        "multi-valued RDN publisher returned %#lx.\n", hr );
    ok( p_appx_signature_check_publisher( NULL, L"CN=Microsoft Corporation" ) == E_INVALIDARG,
        "accepted NULL signature publisher comparison.\n" );
    ok( p_appx_signature_check_publisher( signature, NULL ) == E_INVALIDARG,
        "accepted NULL publisher.\n" );
    p_appx_signature_free( signature );
    signature = NULL;

    memcpy( tampered, signature_bytes, sizeof(tampered) );
    tampered[sizeof(tampered) - 1] ^= 1;
    hr = p_appx_signature_parse_and_verify(
        tampered, sizeof(tampered),
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature );
    ok( FAILED( hr ), "tampered CMS signature returned %#lx.\n", hr );
    ok( !signature, "tampered CMS signature returned object %p.\n", signature );

    memcpy( tampered, signature_bytes, sizeof(tampered) );
    for (i = 0; i + sizeof(digest_prefix) < sizeof(tampered); i++)
        if (!memcmp( tampered + i, digest_prefix, sizeof(digest_prefix) )) break;
    ok( i + sizeof(digest_prefix) < sizeof(tampered),
        "failed to locate signed digest content.\n" );
    if (i + sizeof(digest_prefix) < sizeof(tampered))
    {
        tampered[i + sizeof(digest_prefix)] ^= 1;
        hr = p_appx_signature_parse_and_verify(
            tampered, sizeof(tampered),
            APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature );
        ok( hr == TRUST_E_BAD_DIGEST,
            "tampered SpcIndirectDataContent returned %#lx.\n", hr );
        ok( !signature, "tampered content returned object %p.\n", signature );
    }

    memcpy( tampered, signature_bytes, sizeof(tampered) );
    for (i = sizeof(tampered) - sizeof(rsa_algorithm); i; i--)
        if (!memcmp( tampered + i, rsa_algorithm, sizeof(rsa_algorithm) )) break;
    ok( i != 0, "failed to locate signer RSA algorithm.\n" );
    if (i)
    {
        tampered[i + sizeof(rsa_algorithm) - 3] ^= 1;
        hr = p_appx_signature_parse_and_verify(
            tampered, sizeof(tampered),
            APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature );
        ok( hr == TRUST_E_MALFORMED_SIGNATURE,
            "non-RSA signer algorithm returned %#lx.\n", hr );
        ok( !signature, "non-RSA signer algorithm returned object %p.\n",
            signature );
    }
}

START_TEST( signature )
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is unavailable, error %lu.\n", GetLastError() );
        return;
    }
    p_appx_signature_parse_and_verify =
        (void *)GetProcAddress( module, "appx_signature_parse_and_verify" );
    p_appx_signature_free =
        (void *)GetProcAddress( module, "appx_signature_free" );
    p_appx_signature_get_digest_set =
        (void *)GetProcAddress( module, "appx_signature_get_digest_set" );
    p_appx_signature_get_signer_subject =
        (void *)GetProcAddress( module, "appx_signature_get_signer_subject" );
    p_appx_signature_get_signer_certificate_id =
        (void *)GetProcAddress( module,
                               "appx_signature_get_signer_certificate_id" );
    p_appx_signature_check_publisher =
        (void *)GetProcAddress( module, "appx_signature_check_publisher" );
    p_appx_signature_decode_digest_set =
        (void *)GetProcAddress( module, "appx_signature_decode_digest_set" );
    p_appx_signature_decode_indirect_data =
        (void *)GetProcAddress( module, "appx_signature_decode_indirect_data" );
    p_appx_signature_decode_indirect_data_ex =
        (void *)GetProcAddress( module, "appx_signature_decode_indirect_data_ex" );
    p_appx_signature_compare_digest_sets =
        (void *)GetProcAddress( module, "appx_signature_compare_digest_sets" );
    p_appx_signature_verify_digest_set =
        (void *)GetProcAddress( module, "appx_signature_verify_digest_set" );
    p_appx_signature_validate_leaf_extensions = (void *)GetProcAddress(
        module, "appx_signature_validate_leaf_extensions" );
    p_appx_signature_select_ess_attribute = (void *)GetProcAddress(
        module, "appx_signature_select_ess_attribute" );
    p_appx_signature_validate_ess_certificate = (void *)GetProcAddress(
        module, "appx_signature_validate_ess_certificate" );
    p_appx_signature_get_chain_policy = (void *)GetProcAddress(
        module, "appx_signature_get_chain_policy" );
    p_appx_signature_evaluate_chain_status = (void *)GetProcAddress(
        module, "appx_signature_evaluate_chain_status" );
    if (!p_appx_signature_parse_and_verify || !p_appx_signature_free ||
        !p_appx_signature_get_digest_set ||
        !p_appx_signature_get_signer_subject ||
        !p_appx_signature_get_signer_certificate_id ||
        !p_appx_signature_check_publisher ||
        !p_appx_signature_decode_digest_set ||
        !p_appx_signature_decode_indirect_data ||
        !p_appx_signature_decode_indirect_data_ex ||
        !p_appx_signature_compare_digest_sets ||
        !p_appx_signature_verify_digest_set ||
        !p_appx_signature_validate_leaf_extensions ||
        !p_appx_signature_select_ess_attribute ||
        !p_appx_signature_validate_ess_certificate ||
        !p_appx_signature_get_chain_policy ||
        !p_appx_signature_evaluate_chain_status)
    {
        ok( 0, "AppX signature exports are unavailable.\n" );
        FreeLibrary( module );
        return;
    }

    test_digest_sets();
    test_indirect_data();
    test_leaf_extensions();
    test_ess_certificate_binding();
    test_chain_policy();
    test_cms_boundaries();
    test_real_signature();

    p_appx_signature_free( NULL );
    ok( !p_appx_signature_get_digest_set( NULL ), "NULL signature returned digests.\n" );
    ok( !p_appx_signature_get_signer_subject( NULL ), "NULL signature returned a subject.\n" );
    ok( p_appx_signature_get_signer_certificate_id( NULL, NULL, 0 ) ==
        E_INVALIDARG, "accepted invalid signer certificate id arguments.\n" );
    ok( p_appx_signature_verify_digest_set( NULL, NULL ) == E_INVALIDARG,
        "accepted NULL signature digest comparison.\n" );
    FreeLibrary( module );
}
