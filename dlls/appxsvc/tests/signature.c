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
static HRESULT (WINAPI *p_appx_signature_decode_digest_set)(
    const BYTE *, SIZE_T, struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_decode_indirect_data)(
    const BYTE *, SIZE_T, struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_compare_digest_sets)(
    const struct appx_signature_digest_set *,
    const struct appx_signature_digest_set * );
static HRESULT (WINAPI *p_appx_signature_verify_digest_set)(
    const APPX_SIGNATURE *, const struct appx_signature_digest_set * );

/*
 * AppxSignature.p7x from microsoft/msix-packaging
 * src/test/testData/unpack/SignedUntrustedCert-CERT_E_CHAINING.appx at
 * efeb9dad695a200c2beaddcba54a52c8320bd135.  The upstream repository is
 * MIT-licensed.  SHA-256:
 * 333d06e0f658b35b85a6b4b289b59ea45ce7aa7ecf55fd29491b78ee8d29a020.
 */
static const char signed_untrusted_signature_base64[] =
    "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";

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static BOOL decode_fixture_base64(const char *text, BYTE *output,
                                  SIZE_T capacity, SIZE_T *output_size)
{
    UINT32 accumulator = 0, bits = 0;
    SIZE_T size = 0;
    int value;

    while (*text && *text != '=')
    {
        if ((value = base64_value(*text++)) < 0) return FALSE;
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

static void put_u32(BYTE *data, DWORD value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static void make_digest_blob(BYTE blob[FULL_DIGEST_SIZE], BOOL code_integrity)
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

    memset(blob, 0xcc, FULL_DIGEST_SIZE);
    put_u32(blob, 0x58505041); /* APPX */
    for (i = 0; i < count; i++)
    {
        BYTE *record = blob + 4 + i * DIGEST_RECORD_SIZE;

        put_u32(record, tags[i]);
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

static void append_fixture_bytes(struct indirect_fixture *fixture,
                                 const void *data, SIZE_T size)
{
    memcpy(fixture->data + fixture->size, data, size);
    fixture->size += size;
}

static void append_fixture_byte(struct indirect_fixture *fixture, BYTE value)
{
    fixture->data[fixture->size++] = value;
}

static void make_indirect_fixture(struct indirect_fixture *fixture,
                                  BOOL code_integrity)
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

    memset(fixture, 0, sizeof(*fixture));
    make_digest_blob(digest, code_integrity);

    append_fixture_byte(fixture, 0x30);
    if (code_integrity)
    {
        append_fixture_byte(fixture, 0x82);
        append_fixture_byte(fixture, 0x01);
        append_fixture_byte(fixture, 0x04);
    }
    else
    {
        append_fixture_byte(fixture, 0x81);
        append_fixture_byte(fixture, 0xe0);
    }
    append_fixture_byte(fixture, 0x30);
    append_fixture_byte(fixture, 0x35);
    append_fixture_byte(fixture, 0x06);
    append_fixture_byte(fixture, sizeof(sip_oid));
    fixture->sip_oid = fixture->size;
    append_fixture_bytes(fixture, sip_oid, sizeof(sip_oid));
    append_fixture_byte(fixture, 0x30);
    append_fixture_byte(fixture, 0x27);
    append_fixture_byte(fixture, 0x02);
    append_fixture_byte(fixture, sizeof(version));
    fixture->version = fixture->size;
    append_fixture_bytes(fixture, version, sizeof(version));
    append_fixture_byte(fixture, 0x04);
    append_fixture_byte(fixture, sizeof(sip_guid));
    fixture->guid = fixture->size;
    append_fixture_bytes(fixture, sip_guid, sizeof(sip_guid));
    fixture->reserved = fixture->size + 2;
    for (i = 0; i < 5; i++)
    {
        append_fixture_byte(fixture, 0x02);
        append_fixture_byte(fixture, 0x01);
        append_fixture_byte(fixture, 0x00);
    }
    append_fixture_byte(fixture, 0x30);
    append_fixture_byte(fixture, 0x81);
    append_fixture_byte(fixture, code_integrity ? 0xca : 0xa6);
    append_fixture_byte(fixture, 0x30);
    append_fixture_byte(fixture, 0x0d);
    append_fixture_byte(fixture, 0x06);
    append_fixture_byte(fixture, sizeof(sha_oid));
    fixture->sha_oid = fixture->size;
    append_fixture_bytes(fixture, sha_oid, sizeof(sha_oid));
    fixture->null_tag = fixture->size;
    append_fixture_byte(fixture, 0x05);
    append_fixture_byte(fixture, 0x00);
    fixture->digest_tag = fixture->size;
    append_fixture_byte(fixture, 0x04);
    append_fixture_byte(fixture, 0x81);
    append_fixture_byte(fixture, code_integrity ? FULL_DIGEST_SIZE : BASE_DIGEST_SIZE);
    append_fixture_bytes(fixture, digest,
                         code_integrity ? FULL_DIGEST_SIZE : BASE_DIGEST_SIZE);
}

static void test_digest_sets(void)
{
    struct appx_signature_digest_set base, full, copy;
    BYTE blob[FULL_DIGEST_SIZE], changed[FULL_DIGEST_SIZE];
    HRESULT hr;
    UINT32 i;

    make_digest_blob(blob, FALSE);
    memset(&base, 0xcc, sizeof(base));
    hr = p_appx_signature_decode_digest_set(blob, BASE_DIGEST_SIZE, &base);
    ok(hr == S_OK, "got %#lx.\n", hr);
    ok(base.flags == APPX_SIGNATURE_DIGEST_REQUIRED, "got flags %#x.\n", base.flags);
    for (i = 0; i < APPX_SIGNATURE_SHA256_SIZE; i++)
    {
        ok(base.package_contents[i] == i, "AXPC byte %u is %#x.\n",
           i, base.package_contents[i]);
        ok(base.central_directory[i] == 32 + i, "AXCD byte %u is %#x.\n",
           i, base.central_directory[i]);
        ok(base.content_types[i] == 64 + i, "AXCT byte %u is %#x.\n",
           i, base.content_types[i]);
        ok(base.block_map[i] == 96 + i, "AXBM byte %u is %#x.\n",
           i, base.block_map[i]);
        ok(!base.code_integrity[i], "AXCI byte %u is %#x.\n",
           i, base.code_integrity[i]);
    }

    make_digest_blob(blob, TRUE);
    memset(&full, 0xcc, sizeof(full));
    hr = p_appx_signature_decode_digest_set(blob, FULL_DIGEST_SIZE, &full);
    ok(hr == S_OK, "got %#lx.\n", hr);
    ok(full.flags == APPX_SIGNATURE_DIGEST_ALL, "got flags %#x.\n", full.flags);
    for (i = 0; i < APPX_SIGNATURE_SHA256_SIZE; i++)
        ok(full.code_integrity[i] == 128 + i, "AXCI byte %u is %#x.\n",
           i, full.code_integrity[i]);

    copy = full;
    hr = p_appx_signature_compare_digest_sets(&full, &copy);
    ok(hr == S_OK, "got %#lx.\n", hr);
    hr = p_appx_signature_compare_digest_sets(&base, &full);
    ok(hr == APPX_E_DIGEST_MISMATCH, "AXCI presence returned %#lx.\n", hr);
    for (i = 0; i < sizeof(copy); i++)
    {
        BYTE *bytes = (BYTE *)&copy;

        bytes[i] ^= 0x80;
        hr = p_appx_signature_compare_digest_sets(&full, &copy);
        ok(hr == APPX_E_DIGEST_MISMATCH, "byte %u returned %#lx.\n", i, hr);
        bytes[i] ^= 0x80;
    }
    copy.flags |= 0x80000000;
    hr = p_appx_signature_compare_digest_sets(&full, &copy);
    ok(hr == APPX_E_DIGEST_MISMATCH, "unknown flag returned %#lx.\n", hr);
    ok(p_appx_signature_compare_digest_sets(NULL, &copy) == E_INVALIDARG,
       "accepted a NULL signed set.\n");
    ok(p_appx_signature_compare_digest_sets(&full, NULL) == E_INVALIDARG,
       "accepted a NULL recalculated set.\n");

    memcpy(changed, blob, sizeof(changed));
    put_u32(changed, 0x11111111);
    memset(&copy, 0xcc, sizeof(copy));
    hr = p_appx_signature_decode_digest_set(changed, FULL_DIGEST_SIZE, &copy);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "bad APPX tag returned %#lx.\n", hr);
    ok(!copy.flags, "failure left flags %#x.\n", copy.flags);

    for (i = 0; i < 5; i++)
    {
        memcpy(changed, blob, sizeof(changed));
        put_u32(changed + 4 + i * DIGEST_RECORD_SIZE, 0x43505841);
        if (!i) put_u32(changed + 4, 0x44435841);
        hr = p_appx_signature_decode_digest_set(changed, FULL_DIGEST_SIZE, &copy);
        ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA,
           "wrong/duplicate tag %u returned %#lx.\n", i, hr);
    }

    memcpy(changed, blob, sizeof(changed));
    put_u32(changed + 4 + 2 * DIGEST_RECORD_SIZE, 0x11111111);
    hr = p_appx_signature_decode_digest_set(changed, FULL_DIGEST_SIZE, &copy);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "unknown digest tag returned %#lx.\n", hr);

    memcpy(changed, blob, sizeof(changed));
    memcpy(changed + 4, blob + 4 + DIGEST_RECORD_SIZE, DIGEST_RECORD_SIZE);
    memcpy(changed + 4 + DIGEST_RECORD_SIZE, blob + 4, DIGEST_RECORD_SIZE);
    hr = p_appx_signature_decode_digest_set(changed, FULL_DIGEST_SIZE, &copy);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "reordered tags returned %#lx.\n", hr);

    hr = p_appx_signature_decode_digest_set(blob, BASE_DIGEST_SIZE - 1, &copy);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "short digest returned %#lx.\n", hr);
    hr = p_appx_signature_decode_digest_set(blob, BASE_DIGEST_SIZE + 1, &copy);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "odd digest returned %#lx.\n", hr);
    hr = p_appx_signature_decode_digest_set(blob, FULL_DIGEST_SIZE - 1, &copy);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "short AXCI digest returned %#lx.\n", hr);
    ok(p_appx_signature_decode_digest_set(NULL, FULL_DIGEST_SIZE, &copy) == E_INVALIDARG,
       "accepted NULL data.\n");
    ok(p_appx_signature_decode_digest_set(blob, FULL_DIGEST_SIZE, NULL) == E_INVALIDARG,
       "accepted NULL output.\n");
}

static void test_indirect_data(void)
{
    struct appx_signature_digest_set set;
    struct indirect_fixture fixture;
    HRESULT hr;

    make_indirect_fixture(&fixture, FALSE);
    ok(fixture.size == 227, "base indirect data has size %Iu.\n", fixture.size);
    hr = p_appx_signature_decode_indirect_data(fixture.data, fixture.size, &set);
    ok(hr == S_OK, "base indirect data returned %#lx.\n", hr);
    ok(set.flags == APPX_SIGNATURE_DIGEST_REQUIRED, "got flags %#x.\n", set.flags);

    make_indirect_fixture(&fixture, TRUE);
    ok(fixture.size == 264, "AXCI indirect data has size %Iu.\n", fixture.size);
    hr = p_appx_signature_decode_indirect_data(fixture.data, fixture.size, &set);
    ok(hr == S_OK, "AXCI indirect data returned %#lx.\n", hr);
    ok(set.flags == APPX_SIGNATURE_DIGEST_ALL, "got flags %#x.\n", set.flags);

#define CHECK_MUTATION(member, description) do                                      \
    {                                                                                \
        fixture.data[fixture.member] ^= 1;                                            \
        memset(&set, 0xcc, sizeof(set));                                              \
        hr = p_appx_signature_decode_indirect_data(fixture.data, fixture.size, &set); \
        ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "%s returned %#lx.\n",               \
           description, hr);                                                          \
        ok(!set.flags, "%s left flags %#x.\n", description, set.flags);                \
        fixture.data[fixture.member] ^= 1;                                            \
    } while (0)

    CHECK_MUTATION(sip_oid, "wrong SIP OID");
    CHECK_MUTATION(version, "wrong SIP version");
    CHECK_MUTATION(guid, "wrong SIP GUID");
    CHECK_MUTATION(reserved, "nonzero SIP reserved value");
    CHECK_MUTATION(sha_oid, "wrong indirect digest algorithm");
    CHECK_MUTATION(null_tag, "wrong indirect algorithm parameters");
    CHECK_MUTATION(digest_tag, "wrong indirect digest tag");
#undef CHECK_MUTATION

    hr = p_appx_signature_decode_indirect_data(fixture.data, fixture.size - 1, &set);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "truncated indirect data returned %#lx.\n", hr);
    fixture.data[fixture.size] = 0;
    hr = p_appx_signature_decode_indirect_data(fixture.data, fixture.size + 1, &set);
    ok(hr == APPX_E_INVALID_SIP_CLIENT_DATA, "trailing indirect data returned %#lx.\n", hr);
    ok(p_appx_signature_decode_indirect_data(NULL, fixture.size, &set) == E_INVALIDARG,
       "accepted NULL indirect data.\n");
    ok(p_appx_signature_decode_indirect_data(fixture.data, fixture.size, NULL) == E_INVALIDARG,
       "accepted NULL indirect output.\n");
}

/*
 * A deliberately certificate-free SignedData envelope.  Mutations exercise
 * the strict CMS/DER pre-parser without depending on a machine certificate
 * store or a private key.
 */
static const BYTE cms_shape[] =
{
    0x30,0x39,
      0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x07,0x02,
      0xa0,0x2c,
        0x30,0x2a,
          0x02,0x01,0x01,
          0x31,0x0f,
            0x30,0x0d,
              0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
              0x05,0x00,
          0x30,0x10,
            0x06,0x0a,0x2b,0x06,0x01,0x04,0x01,0x82,0x37,0x02,0x01,0x04,
            0xa0,0x02,0x30,0x00,
          0x31,0x02,0x30,0x00
};

static void make_p7x(BYTE *p7x)
{
    put_u32(p7x, 0x58434b50); /* PKCX */
    memcpy(p7x + 4, cms_shape, sizeof(cms_shape));
}

static void expect_rejected(const BYTE *data, SIZE_T size, const char *description)
{
    APPX_SIGNATURE *signature = (APPX_SIGNATURE *)0xdeadbeef;
    HRESULT hr = p_appx_signature_parse_and_verify(
        data, size, APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature);

    ok(FAILED(hr), "%s unexpectedly returned %#lx.\n", description, hr);
    ok(!signature, "%s returned signature %p.\n", description, signature);
}

static void test_cms_boundaries(void)
{
    BYTE p7x[4 + sizeof(cms_shape) + 2], changed[sizeof(p7x)];
    APPX_SIGNATURE *signature = (APPX_SIGNATURE *)0xdeadbeef;
    BYTE *oversized;
    HRESULT hr;
    SIZE_T i;

    make_p7x(p7x);
    expect_rejected(p7x, 4 + sizeof(cms_shape), "certificate-free CMS");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[0] ^= 1;
    expect_rejected(changed, 4 + sizeof(cms_shape), "bad PKCX");

    for (i = 0; i < 4 + sizeof(cms_shape); i++)
        expect_rejected(p7x, i, "truncated p7x");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[4 + sizeof(cms_shape)] = 0;
    expect_rejected(changed, sizeof(changed), "DER trailing byte");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[4 + 12] ^= 1; /* signedData OID */
    expect_rejected(changed, 4 + sizeof(cms_shape), "wrong outer OID");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[4 + 34] ^= 1; /* SHA-256 OID */
    expect_rejected(changed, 4 + sizeof(cms_shape), "wrong digest algorithm");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[4 + 50] ^= 1; /* SPC_INDIRECT_DATA_OBJID */
    expect_rejected(changed, 4 + sizeof(cms_shape), "wrong content OID");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[4 + 1] = 0x37;
    changed[4 + 14] = 0x2a;
    changed[4 + 16] = 0x28;
    changed[4 + 56] = 0; /* Empty signerInfos SET. */
    expect_rejected(changed, 4 + sizeof(cms_shape) - 2, "zero CMS signers");

    memcpy(changed, p7x, 4 + sizeof(cms_shape));
    changed[4 + 1] = 0x3b;
    changed[4 + 14] = 0x2e;
    changed[4 + 16] = 0x2c;
    changed[4 + 56] = 4;
    changed[4 + sizeof(cms_shape)] = 0x30;
    changed[4 + sizeof(cms_shape) + 1] = 0;
    expect_rejected(changed, 4 + sizeof(cms_shape) + 2, "two CMS signers");

    memcpy(changed, p7x, 4 + 55);
    changed[4 + 1] = 0x3b;
    changed[4 + 14] = 0x2e;
    changed[4 + 16] = 0x2c;
    changed[4 + 38] = 0x12;
    changed[4 + 52] = 4;
    changed[4 + 55] = 0x30;
    changed[4 + 56] = 0;
    memcpy(changed + 4 + 57, p7x + 4 + 55, sizeof(cms_shape) - 55);
    expect_rejected(changed, 4 + sizeof(cms_shape) + 2, "nested DER trailing element");

    hr = p_appx_signature_parse_and_verify(
        p7x, 4 + sizeof(cms_shape), 0x80000000, &signature);
    ok(hr == E_INVALIDARG, "unknown flag returned %#lx.\n", hr);
    ok(!signature, "unknown flag returned signature %p.\n", signature);
    ok(p_appx_signature_parse_and_verify(p7x, 4 + sizeof(cms_shape), 0, NULL) == E_POINTER,
       "accepted NULL output.\n");
    signature = (APPX_SIGNATURE *)0xdeadbeef;
    hr = p_appx_signature_parse_and_verify(NULL, 0, 0, &signature);
    ok(hr == E_INVALIDARG, "NULL data returned %#lx.\n", hr);
    ok(!signature, "NULL data returned signature %p.\n", signature);

    oversized = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                          APPX_SIGNATURE_MAX_SIZE + 1);
    ok(!!oversized, "failed to allocate oversized fixture.\n");
    if (oversized)
    {
        signature = (APPX_SIGNATURE *)0xdeadbeef;
        hr = p_appx_signature_parse_and_verify(
            oversized, APPX_SIGNATURE_MAX_SIZE + 1, 0, &signature);
        ok(hr == E_INVALIDARG, "oversized p7x returned %#lx.\n", hr);
        ok(!signature, "oversized p7x returned signature %p.\n", signature);
        HeapFree(GetProcessHeap(), 0, oversized);
    }
}

static void test_real_signature(void)
{
    static const BYTE digest_prefix[] = {'A','P','P','X','A','X','P','C'};
    struct appx_signature_digest_set recalculated;
    const struct appx_signature_digest_set *signed_set;
    BYTE signature_bytes[1655], tampered[1655];
    const WCHAR *subject;
    APPX_SIGNATURE *signature = NULL;
    SIZE_T signature_size = 0, i;
    HRESULT hr;

    ok(decode_fixture_base64(signed_untrusted_signature_base64,
                             signature_bytes, sizeof(signature_bytes),
                             &signature_size),
       "failed to decode the signed fixture.\n");
    ok(signature_size == sizeof(signature_bytes), "decoded %Iu bytes.\n",
       signature_size);
    if (signature_size != sizeof(signature_bytes)) return;

    hr = p_appx_signature_parse_and_verify(
        signature_bytes, signature_size, 0, &signature);
    ok(hr == CERT_E_CHAINING || hr == CERT_E_UNTRUSTEDROOT ||
       hr == CERT_E_UNTRUSTEDTESTROOT,
       "untrusted real AppX signature returned %#lx.\n", hr);
    ok(!signature, "untrusted real AppX signature returned object %p.\n",
       signature);

    hr = p_appx_signature_parse_and_verify(
        signature_bytes, signature_size,
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature);
    ok(hr == S_OK, "real AppX signature returned %#lx.\n", hr);
    ok(signature != NULL, "real AppX signature returned no object.\n");
    if (!signature) return;

    signed_set = p_appx_signature_get_digest_set(signature);
    ok(signed_set != NULL, "real AppX signature returned no digest set.\n");
    if (signed_set)
    {
        ok(signed_set->flags == APPX_SIGNATURE_DIGEST_REQUIRED,
           "got digest flags %#x.\n", signed_set->flags);
        ok(signed_set->package_contents[0] == 0xe8 &&
           signed_set->central_directory[0] == 0xf7 &&
           signed_set->content_types[0] == 0x53 &&
           signed_set->block_map[0] == 0x67,
           "unexpected real AppX digests.\n");
        recalculated = *signed_set;
        hr = p_appx_signature_verify_digest_set(signature, &recalculated);
        ok(hr == S_OK, "matching real digest set returned %#lx.\n", hr);
    }
    subject = p_appx_signature_get_signer_subject(signature);
    ok(subject && wcsstr(subject, L"CN=Microsoft Corporation"),
       "got signer subject %s.\n", wine_dbgstr_w(subject));
    p_appx_signature_free(signature);
    signature = NULL;

    memcpy(tampered, signature_bytes, sizeof(tampered));
    tampered[sizeof(tampered) - 1] ^= 1;
    hr = p_appx_signature_parse_and_verify(
        tampered, sizeof(tampered),
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature);
    ok(FAILED(hr), "tampered CMS signature returned %#lx.\n", hr);
    ok(!signature, "tampered CMS signature returned object %p.\n", signature);

    memcpy(tampered, signature_bytes, sizeof(tampered));
    for (i = 0; i + sizeof(digest_prefix) < sizeof(tampered); i++)
        if (!memcmp(tampered + i, digest_prefix, sizeof(digest_prefix))) break;
    ok(i + sizeof(digest_prefix) < sizeof(tampered),
       "failed to locate signed digest content.\n");
    if (i + sizeof(digest_prefix) < sizeof(tampered))
    {
        tampered[i + sizeof(digest_prefix)] ^= 1;
        hr = p_appx_signature_parse_and_verify(
            tampered, sizeof(tampered),
            APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN, &signature);
        ok(hr == TRUST_E_BAD_DIGEST,
           "tampered SpcIndirectDataContent returned %#lx.\n", hr);
        ok(!signature, "tampered content returned object %p.\n", signature);
    }
}

START_TEST(signature)
{
    HMODULE module = LoadLibraryW(L"appxsvc.dll");

    if (!module)
    {
        ok(0, "appxsvc.dll is unavailable, error %lu.\n", GetLastError());
        return;
    }
    p_appx_signature_parse_and_verify =
        (void *)GetProcAddress(module, "appx_signature_parse_and_verify");
    p_appx_signature_free =
        (void *)GetProcAddress(module, "appx_signature_free");
    p_appx_signature_get_digest_set =
        (void *)GetProcAddress(module, "appx_signature_get_digest_set");
    p_appx_signature_get_signer_subject =
        (void *)GetProcAddress(module, "appx_signature_get_signer_subject");
    p_appx_signature_decode_digest_set =
        (void *)GetProcAddress(module, "appx_signature_decode_digest_set");
    p_appx_signature_decode_indirect_data =
        (void *)GetProcAddress(module, "appx_signature_decode_indirect_data");
    p_appx_signature_compare_digest_sets =
        (void *)GetProcAddress(module, "appx_signature_compare_digest_sets");
    p_appx_signature_verify_digest_set =
        (void *)GetProcAddress(module, "appx_signature_verify_digest_set");
    if (!p_appx_signature_parse_and_verify || !p_appx_signature_free ||
        !p_appx_signature_get_digest_set ||
        !p_appx_signature_get_signer_subject ||
        !p_appx_signature_decode_digest_set ||
        !p_appx_signature_decode_indirect_data ||
        !p_appx_signature_compare_digest_sets ||
        !p_appx_signature_verify_digest_set)
    {
        ok(0, "AppX signature exports are unavailable.\n");
        FreeLibrary(module);
        return;
    }

    test_digest_sets();
    test_indirect_data();
    test_cms_boundaries();
    test_real_signature();

    p_appx_signature_free(NULL);
    ok(!p_appx_signature_get_digest_set(NULL), "NULL signature returned digests.\n");
    ok(!p_appx_signature_get_signer_subject(NULL), "NULL signature returned a subject.\n");
    ok(p_appx_signature_verify_digest_set(NULL, NULL) == E_INVALIDARG,
       "accepted NULL signature digest comparison.\n");
    FreeLibrary(module);
}
