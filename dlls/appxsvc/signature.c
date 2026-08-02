/*
 * AppX package signature verification
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

#define CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "winerror.h"
#include "wintrust.h"

#include "wine/debug.h"

#include "signature.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define APPX_SIGNATURE_MAGIC             0x58434b50 /* PKCX */
#define APPX_DIGEST_MAGIC                0x58505041 /* APPX */
#define APPX_DIGEST_RECORD_SIZE          (4 + APPX_SIGNATURE_SHA256_SIZE)
#define APPX_DIGEST_BASE_SIZE            (4 + 4 * APPX_DIGEST_RECORD_SIZE)
#define APPX_DIGEST_CODE_INTEGRITY_SIZE  (APPX_DIGEST_BASE_SIZE + APPX_DIGEST_RECORD_SIZE)

#define MAX_DER_DEPTH                    32
#define MAX_DER_ELEMENTS                 32768
#define MAX_CMS_CONTENT_SIZE             (64 * 1024)
#define MAX_SIGNER_INFO_SIZE             (256 * 1024)
#define MAX_EMBEDDED_CERTIFICATES        64
#define MAX_EMBEDDED_CERTIFICATE_SIZE    (512 * 1024)
#define MAX_SIGNER_ATTRIBUTES            64
#define MAX_ATTRIBUTE_VALUES             16
#define MAX_ATTRIBUTE_OID_CHARS          128
#define MAX_SIGNER_SUBJECT_CHARS         4096
#define MAX_SIGNER_SUBJECT_DER_SIZE       (64 * 1024)
#define MAX_CERTIFICATE_EXTENSIONS       64
#define MAX_ESS_CERTIFICATE_IDS          64
#define APPX_CHAIN_URL_TIMEOUT_MS        10000

static const BYTE oid_pkcs7_signed_data[] =
    { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
static const BYTE oid_spc_indirect_data[] =
    { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04 };
static const BYTE oid_spc_siginfo[] =
    { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x1e };
static const BYTE oid_tst_info[] =
    { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x10, 0x01, 0x04 };
static const BYTE oid_sha256[] =
    { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const BYTE oid_timestamp_signing[] =
    { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x08 };
static const BYTE appx_sip_guid[] =
    { 0x4b, 0xdf, 0xc5, 0x0a, 0x07, 0xce, 0xe2, 0x4d,
      0xb7, 0x6e, 0x23, 0xc8, 0x39, 0xa0, 0x9f, 0xd1 };
static const BYTE appx_bundle_sip_guid[] =
    { 0xb3, 0x58, 0x5f, 0x0f, 0xde, 0xaa, 0x9a, 0x4b,
      0xa4, 0x34, 0x95, 0x74, 0x2d, 0x92, 0xec, 0xeb };
static const char oid_rfc3161_counter_signature[] = "1.3.6.1.4.1.311.3.3.1";
static const char oid_signing_certificate[] = "1.2.840.113549.1.9.16.2.12";
static const char oid_signing_certificate_v2[] = "1.2.840.113549.1.9.16.2.47";
static const char oid_tst_info_string[] = "1.2.840.113549.1.9.16.1.4";
static const WCHAR sha1_algorithm[] = { 'S', 'H', 'A', '1', 0 };
static const WCHAR sha256_algorithm[] = { 'S', 'H', 'A', '2', '5', '6', 0 };

struct appx_signature
{
    struct appx_signature_digest_set digests;
    BYTE signer_certificate_id[APPX_SIGNATURE_CERTIFICATE_ID_SIZE];
    WCHAR *signer_subject;
    CERT_NAME_BLOB signer_subject_name;
};

C_ASSERT( sizeof(struct appx_signature_digest_set) ==
          sizeof(UINT32) + 5 * APPX_SIGNATURE_SHA256_SIZE );

struct der_budget
{
    UINT32 elements;
};

struct der_cursor
{
    const BYTE *data;
    SIZE_T size;
    struct der_budget *budget;
};

struct der_element
{
    BYTE tag;
    const BYTE *encoded;
    SIZE_T encoded_size;
    const BYTE *value;
    SIZE_T value_size;
};

static HRESULT malformed_signature( void )
{
    return TRUST_E_MALFORMED_SIGNATURE;
}

static HRESULT crypto_error( HRESULT fallback )
{
    DWORD error = GetLastError();

    return error ? HRESULT_FROM_WIN32( error ) : fallback;
}

static DWORD read_u32( const BYTE *data )
{
    return data[0] | ((DWORD)data[1] << 8) | ((DWORD)data[2] << 16) |
           ((DWORD)data[3] << 24);
}

static BOOL oid_equal( const struct der_element *element, const BYTE *oid, SIZE_T size )
{
    return element->tag == 0x06 && element->value_size == size &&
           !memcmp( element->value, oid, size );
}

static BOOL der_oid_is_valid( const BYTE *data, SIZE_T size )
{
    SIZE_T i;

    if (!size) return FALSE;
    for (i = 0; i < size;)
    {
        if (data[i] == 0x80) return FALSE;
        do
        {
            if (i >= size) return FALSE;
        } while (data[i++] & 0x80);
    }
    return TRUE;
}

static BOOL der_universal_primitive_is_valid( BYTE tag, const BYTE *data, SIZE_T size )
{
    switch (tag)
    {
    case 0x01: /* BOOLEAN */
        return size == 1 && (!data[0] || data[0] == 0xff);
    case 0x02: /* INTEGER */
    case 0x0a: /* ENUMERATED */
        if (!size) return FALSE;
        if (size > 1 && data[0] == 0x00 && !(data[1] & 0x80)) return FALSE;
        if (size > 1 && data[0] == 0xff && (data[1] & 0x80)) return FALSE;
        return TRUE;
    case 0x03: /* BIT STRING */
        if (!size || data[0] > 7) return FALSE;
        if (size == 1) return !data[0];
        return !(data[size - 1] & ((1u << data[0]) - 1));
    case 0x05: /* NULL */
        return !size;
    case 0x06: /* OBJECT IDENTIFIER */
        return der_oid_is_valid( data, size );
    default:
        return TRUE;
    }
}

static BOOL der_read_element( struct der_cursor *cursor, struct der_element *element )
{
    const BYTE *start = cursor->data;
    SIZE_T header_size = 2, length = 0, count, i;
    BYTE tag, length_byte;

    if (cursor->size < 2 || cursor->budget->elements >= MAX_DER_ELEMENTS)
        return FALSE;
    cursor->budget->elements++;

    tag = start[0];
    if (!tag) return FALSE; /* DER never contains an EOC element. */
    if ((tag & 0x1f) == 0x1f)
    {
        if (cursor->size < 3 || start[1] == 0x80 ||
            (!(start[1] & 0x80) && start[1] < 0x1f))
            return FALSE;
        for (i = 1; i < cursor->size && i <= 5; i++)
        {
            if (!(start[i] & 0x80)) break;
        }
        if (i == cursor->size || i > 5) return FALSE;
        header_size = i + 2;
        if (cursor->size < header_size) return FALSE;
        length_byte = start[i + 1];
    }
    else
    {
        length_byte = start[1];
    }

    if (!(length_byte & 0x80))
    {
        length = length_byte;
    }
    else
    {
        count = length_byte & 0x7f;
        if (!count || count > sizeof(SIZE_T) || header_size > cursor->size ||
            count > cursor->size - header_size || !start[header_size])
            return FALSE;
        for (i = 0; i < count; i++)
        {
            if (length > (((SIZE_T)-1) >> 8)) return FALSE;
            length = (length << 8) | start[header_size + i];
        }
        if (length < 0x80) return FALSE;
        header_size += count;
    }

    if (header_size > cursor->size || length > cursor->size - header_size)
        return FALSE;

    element->tag = tag;
    element->encoded = start;
    element->encoded_size = header_size + length;
    element->value = start + header_size;
    element->value_size = length;
    cursor->data += element->encoded_size;
    cursor->size -= element->encoded_size;
    return TRUE;
}

static int der_lexicographic_compare( const BYTE *left, SIZE_T left_size,
                                      const BYTE *right, SIZE_T right_size )
{
    SIZE_T size = left_size < right_size ? left_size : right_size;
    int result = memcmp( left, right, size );

    if (result) return result;
    return left_size < right_size ? -1 : left_size != right_size;
}

static BOOL der_validate_contents( const BYTE *data, SIZE_T size, UINT32 depth,
                                   BOOL sorted_set, struct der_budget *budget )
{
    struct der_cursor cursor = {data, size, budget};
    struct der_element element;
    const BYTE *previous = NULL;
    SIZE_T previous_size = 0;

    if (depth > MAX_DER_DEPTH) return FALSE;

    while (cursor.size)
    {
        if (!der_read_element( &cursor, &element )) return FALSE;

        if (!(element.tag & 0xc0))
        {
            BYTE number = element.tag & 0x1f;

            if ((number == 16 || number == 17) && !(element.tag & 0x20))
                return FALSE;
            if (number != 16 && number != 17 && (element.tag & 0x20))
                return FALSE; /* DER has no constructed primitive strings. */
            if (!(element.tag & 0x20) &&
                !der_universal_primitive_is_valid( element.tag, element.value,
                                                   element.value_size ))
                return FALSE;
        }

        if (element.tag & 0x20)
        {
            if (!der_validate_contents( element.value, element.value_size,
                                        depth + 1, element.tag == 0x31, budget ))
                return FALSE;
        }

        if (previous && sorted_set &&
            der_lexicographic_compare( previous, previous_size, element.encoded,
                                       element.encoded_size ) > 0)
            return FALSE;
        previous = element.encoded;
        previous_size = element.encoded_size;
    }
    return TRUE;
}

static BOOL der_validate_document( const BYTE *data, SIZE_T size )
{
    struct der_budget budget = { 0 };
    struct der_cursor cursor = { data, size, &budget };
    struct der_element element;

    if (!der_read_element( &cursor, &element ) || cursor.size || element.tag != 0x30)
        return FALSE;
    return der_validate_contents( element.value, element.value_size, 1, FALSE, &budget );
}

static BOOL der_read_expected( struct der_cursor *cursor, BYTE tag,
                               struct der_element *element )
{
    return der_read_element( cursor, element ) && element->tag == tag;
}

static BOOL algorithm_identifier_is_sha256( const struct der_element *element,
                                            BOOL require_null )
{
    struct der_budget budget = { 0 };
    struct der_cursor cursor = { element->value, element->value_size, &budget };
    struct der_element oid, parameters;

    if (element->tag != 0x30 || !der_read_expected( &cursor, 0x06, &oid ) ||
        !oid_equal( &oid, oid_sha256, sizeof(oid_sha256) ))
        return FALSE;
    if (!cursor.size) return !require_null;
    return der_read_expected( &cursor, 0x05, &parameters ) &&
           !parameters.value_size && !cursor.size;
}

static BOOL bounded_oid_string( const char *value )
{
    SIZE_T i;

    if (!value) return FALSE;
    for (i = 0; i < MAX_ATTRIBUTE_OID_CHARS; i++)
        if (!value[i]) return i != 0;
    return FALSE;
}

static BOOL oid_string_equal( const char *value, const char *expected )
{
    SIZE_T i;

    if (!value) return FALSE;
    for (i = 0; i < MAX_ATTRIBUTE_OID_CHARS; i++)
    {
        if (value[i] != expected[i]) return FALSE;
        if (!value[i]) return TRUE;
    }
    return FALSE;
}

static HRESULT validate_key_usage_extension( const CRYPT_OBJID_BLOB *blob )
{
    struct der_budget budget = {0};
    struct der_cursor cursor;
    struct der_element bits;
    BYTE last, unused, trailing = 0;

    if (!blob->pbData || !blob->cbData ||
        blob->cbData > MAX_CMS_CONTENT_SIZE)
        return malformed_signature();
    cursor.data = blob->pbData;
    cursor.size = blob->cbData;
    cursor.budget = &budget;
    if (!der_read_expected( &cursor, 0x03, &bits ) || cursor.size ||
        !der_universal_primitive_is_valid( bits.tag, bits.value,
                                           bits.value_size ) ||
        bits.value_size < 2 || bits.value_size > 3)
        return malformed_signature();

    unused = bits.value[0];
    last = bits.value[bits.value_size - 1];
    if (!last || (bits.value_size == 3 && (last != 0x80 || unused != 7)))
        return malformed_signature();
    while (trailing < 8 && !(last & (1u << trailing))) trailing++;
    if (trailing != unused) return malformed_signature();

    /* X.509 bit zero is the high bit of the first content octet. */
    if (!(bits.value[1] &
          (CERT_DIGITAL_SIGNATURE_KEY_USAGE |
           CERT_NON_REPUDIATION_KEY_USAGE)))
        return CERT_E_WRONG_USAGE;
    return S_OK;
}

static HRESULT validate_timestamp_eku_extension( const CRYPT_OBJID_BLOB *blob )
{
    struct der_budget budget = {0};
    struct der_cursor document, usages;
    struct der_element sequence, usage;
    UINT32 count = 0;

    if (!blob->pbData || !blob->cbData ||
        blob->cbData > MAX_CMS_CONTENT_SIZE ||
        !der_validate_document( blob->pbData, blob->cbData ))
        return malformed_signature();
    document.data = blob->pbData;
    document.size = blob->cbData;
    document.budget = &budget;
    if (!der_read_expected( &document, 0x30, &sequence ) || document.size)
        return malformed_signature();
    usages.data = sequence.value;
    usages.size = sequence.value_size;
    usages.budget = &budget;
    while (usages.size)
    {
        if (!der_read_expected( &usages, 0x06, &usage ))
            return malformed_signature();
        if (++count > 1) return CERT_E_WRONG_USAGE;
    }
    if (count != 1 ||
        !oid_equal( &usage, oid_timestamp_signing,
                    sizeof(oid_timestamp_signing) ))
        return CERT_E_WRONG_USAGE;
    return S_OK;
}

HRESULT WINAPI appx_signature_validate_leaf_extensions(
    const CERT_EXTENSION *extensions, UINT32 count, BOOL timestamp_signer )
{
    const CERT_EXTENSION *key_usage = NULL, *enhanced_key_usage = NULL;
    UINT32 key_usage_count = 0, enhanced_key_usage_count = 0, i;
    HRESULT hr;

    if (count > MAX_CERTIFICATE_EXTENSIONS || (count && !extensions))
        return E_INVALIDARG;
    for (i = 0; i < count; i++)
    {
        const CERT_EXTENSION *extension = extensions + i;

        if (!bounded_oid_string( extension->pszObjId ))
            return malformed_signature();
        if (oid_string_equal( extension->pszObjId, szOID_KEY_USAGE ))
        {
            key_usage = extension;
            if (++key_usage_count > 1) return malformed_signature();
        }
        if (timestamp_signer &&
            oid_string_equal( extension->pszObjId,
                              szOID_ENHANCED_KEY_USAGE ))
        {
            enhanced_key_usage = extension;
            if (++enhanced_key_usage_count > 1)
                return malformed_signature();
        }
    }

    if (key_usage &&
        FAILED( hr = validate_key_usage_extension( &key_usage->Value ) ))
        return hr;
    if (!timestamp_signer) return S_OK;
    if (!enhanced_key_usage || !enhanced_key_usage->fCritical)
        return CERT_E_WRONG_USAGE;
    return validate_timestamp_eku_extension( &enhanced_key_usage->Value );
}

static BOOL allocation_contains( const void *allocation, SIZE_T allocation_size,
                                 const void *pointer, SIZE_T size )
{
    ULONG_PTR start = (ULONG_PTR)allocation, value = (ULONG_PTR)pointer;

    return pointer && value >= start && size <= allocation_size &&
           value - start <= allocation_size - size;
}

static BOOL allocation_contains_string( const void *allocation, SIZE_T allocation_size,
                                        const char *string, SIZE_T maximum )
{
    ULONG_PTR start = (ULONG_PTR)allocation, value = (ULONG_PTR)string;
    SIZE_T available;

    if (!string || value < start || value - start >= allocation_size) return FALSE;
    available = allocation_size - (value - start);
    if (available > maximum) available = maximum;
    return memchr( string, 0, available ) != NULL;
}

static BOOL crypt_algorithm_is_sha256( const CRYPT_ALGORITHM_IDENTIFIER *algorithm,
                                       const void *allocation, SIZE_T allocation_size )
{
    static const BYTE null_parameters[] = { 0x05, 0x00 };

    if (!allocation_contains_string( allocation, allocation_size,
                                     algorithm->pszObjId,
                                     MAX_ATTRIBUTE_OID_CHARS ) ||
        strcmp( algorithm->pszObjId, szOID_NIST_sha256 ))
        return FALSE;
    return !algorithm->Parameters.cbData ||
           (algorithm->Parameters.cbData == sizeof(null_parameters) &&
            allocation_contains( allocation, allocation_size,
                                 algorithm->Parameters.pbData,
                                 algorithm->Parameters.cbData ) &&
            !memcmp( algorithm->Parameters.pbData, null_parameters,
                     sizeof(null_parameters) ));
}

static BOOL crypt_algorithm_is_rsa( const CRYPT_ALGORITHM_IDENTIFIER *algorithm,
                                    const void *allocation, SIZE_T allocation_size )
{
    static const BYTE null_parameters[] = { 0x05, 0x00 };

    return allocation_contains_string( allocation, allocation_size,
                                       algorithm->pszObjId,
                                       MAX_ATTRIBUTE_OID_CHARS ) &&
           !strcmp( algorithm->pszObjId, szOID_RSA_RSA ) &&
           algorithm->Parameters.cbData == sizeof(null_parameters) &&
           allocation_contains( allocation, allocation_size,
                                algorithm->Parameters.pbData,
                                algorithm->Parameters.cbData ) &&
           !memcmp( algorithm->Parameters.pbData, null_parameters,
                    sizeof(null_parameters) );
}

static BOOL crypt_algorithm_is_sha256_rsa(
    const CRYPT_ALGORITHM_IDENTIFIER *algorithm,
    const void *allocation, SIZE_T allocation_size )
{
    static const BYTE null_parameters[] = { 0x05, 0x00 };

    return allocation_contains_string( allocation, allocation_size,
                                       algorithm->pszObjId,
                                       MAX_ATTRIBUTE_OID_CHARS ) &&
           !strcmp( algorithm->pszObjId, szOID_RSA_SHA256RSA ) &&
           algorithm->Parameters.cbData == sizeof(null_parameters) &&
           allocation_contains( allocation, allocation_size,
                                algorithm->Parameters.pbData,
                                algorithm->Parameters.cbData ) &&
           !memcmp( algorithm->Parameters.pbData, null_parameters,
                    sizeof(null_parameters) );
}

static BOOL der_integer_is_u32( const struct der_element *element, DWORD value )
{
    BYTE encoded[4];
    SIZE_T offset = 0, size;

    if (element->tag != 0x02) return FALSE;
    encoded[0] = value >> 24;
    encoded[1] = value >> 16;
    encoded[2] = value >> 8;
    encoded[3] = value;
    while (offset < 3 && !encoded[offset] && !(encoded[offset + 1] & 0x80))
        offset++;
    size = sizeof(encoded) - offset;
    if (encoded[offset] & 0x80) return FALSE; /* Values used here never need padding. */
    return element->value_size == size &&
           !memcmp( element->value, encoded + offset, size );
}

static HRESULT parse_sip_info( const struct der_element *wrapper,
                               const BYTE expected_guid[16] )
{
    struct der_element value;
    struct der_budget budget = { 0 };
    struct der_cursor cursor;
    UINT32 i;

    if (wrapper->tag != 0x30) return APPX_E_INVALID_SIP_CLIENT_DATA;
    cursor.data = wrapper->value;
    cursor.size = wrapper->value_size;
    cursor.budget = &budget;

    if (!der_read_expected( &cursor, 0x02, &value ) ||
        !der_integer_is_u32( &value, 0x01010000 ))
        return APPX_E_INVALID_SIP_CLIENT_DATA;
    if (!der_read_expected( &cursor, 0x04, &value ) ||
        value.value_size != sizeof(appx_sip_guid) ||
        memcmp( value.value, expected_guid, sizeof(appx_sip_guid) ))
        return APPX_E_INVALID_SIP_CLIENT_DATA;
    for (i = 0; i < 5; i++)
    {
        if (!der_read_expected( &cursor, 0x02, &value ) ||
            !der_integer_is_u32( &value, 0 ))
            return APPX_E_INVALID_SIP_CLIENT_DATA;
    }
    return cursor.size ? APPX_E_INVALID_SIP_CLIENT_DATA : S_OK;
}

HRESULT WINAPI appx_signature_decode_indirect_data_ex(
    const BYTE *data, SIZE_T size, UINT32 flags,
    struct appx_signature_digest_set *digests )
{
    const BYTE *expected_guid;
    struct der_budget budget = {0};
    struct der_cursor document = {data, size, &budget}, outer, attribute, digest_info;
    struct der_element sequence, data_part, digest_part, oid, value, algorithm, digest;
    HRESULT hr;

    if (!data || !digests || flags & ~APPX_SIGNATURE_VERIFY_BUNDLE)
        return E_INVALIDARG;
    memset( digests, 0, sizeof(*digests) );
    expected_guid = flags & APPX_SIGNATURE_VERIFY_BUNDLE ?
                    appx_bundle_sip_guid : appx_sip_guid;
    if (!size || size > MAX_CMS_CONTENT_SIZE ||
        !der_validate_document( data, size ) ||
        !der_read_expected( &document, 0x30, &sequence ) || document.size)
        return APPX_E_INVALID_SIP_CLIENT_DATA;
    outer.data = sequence.value;
    outer.size = sequence.value_size;
    outer.budget = &budget;

    if (!der_read_expected( &outer, 0x30, &data_part ) ||
        !der_read_expected( &outer, 0x30, &digest_part ) || outer.size)
        return APPX_E_INVALID_SIP_CLIENT_DATA;

    attribute.data = data_part.value;
    attribute.size = data_part.value_size;
    attribute.budget = &budget;
    if (!der_read_expected( &attribute, 0x06, &oid ) ||
        !oid_equal( &oid, oid_spc_siginfo, sizeof(oid_spc_siginfo) ) ||
        !der_read_expected( &attribute, 0x30, &value ) || attribute.size)
        return APPX_E_INVALID_SIP_CLIENT_DATA;
    if (FAILED( hr = parse_sip_info( &value, expected_guid ) )) return hr;

    digest_info.data = digest_part.value;
    digest_info.size = digest_part.value_size;
    digest_info.budget = &budget;
    if (!der_read_expected( &digest_info, 0x30, &algorithm ) ||
        !algorithm_identifier_is_sha256( &algorithm, TRUE ) ||
        !der_read_expected( &digest_info, 0x04, &digest ) || digest_info.size)
        return APPX_E_INVALID_SIP_CLIENT_DATA;

    return appx_signature_decode_digest_set( digest.value, digest.value_size, digests );
}

HRESULT WINAPI appx_signature_decode_indirect_data(
    const BYTE *data, SIZE_T size, struct appx_signature_digest_set *digests )
{
    return appx_signature_decode_indirect_data_ex( data, size, 0, digests );
}

static HRESULT validate_cms_shape( const BYTE *data, SIZE_T size,
                                   const BYTE **embedded_content,
                                   SIZE_T *embedded_content_size,
                                   const BYTE **authenticated_content,
                                   SIZE_T *authenticated_content_size )
{
    struct der_budget budget = {0};
    struct der_cursor document = {data, size, &budget}, outer, explicit_content;
    struct der_cursor signed_data, digest_algorithms, encapsulated, econtent, signers;
    struct der_cursor signer_fields;
    struct der_element sequence, oid, wrapper, signed_sequence, version;
    struct der_element digest_set, digest_algorithm, content_info, certificates;
    struct der_element signer_set, signer;

    if (!der_validate_document( data, size )) return malformed_signature();
    budget.elements = 0;
    if (!der_read_expected( &document, 0x30, &sequence ) || document.size)
        return malformed_signature();
    outer.data = sequence.value;
    outer.size = sequence.value_size;
    outer.budget = &budget;
    if (!der_read_expected( &outer, 0x06, &oid ) ||
        !oid_equal( &oid, oid_pkcs7_signed_data, sizeof(oid_pkcs7_signed_data) ) ||
        !der_read_expected( &outer, 0xa0, &wrapper ) || outer.size)
        return malformed_signature();
    explicit_content.data = wrapper.value;
    explicit_content.size = wrapper.value_size;
    explicit_content.budget = &budget;
    if (!der_read_expected( &explicit_content, 0x30, &signed_sequence ) ||
        explicit_content.size)
        return malformed_signature();

    signed_data.data = signed_sequence.value;
    signed_data.size = signed_sequence.value_size;
    signed_data.budget = &budget;
    if (!der_read_expected( &signed_data, 0x02, &version ) ||
        !der_integer_is_u32( &version, CMSG_SIGNED_DATA_V1 ) ||
        !der_read_expected( &signed_data, 0x31, &digest_set ))
        return malformed_signature();

    digest_algorithms.data = digest_set.value;
    digest_algorithms.size = digest_set.value_size;
    digest_algorithms.budget = &budget;
    if (!der_read_expected( &digest_algorithms, 0x30, &digest_algorithm ) ||
        digest_algorithms.size ||
        !algorithm_identifier_is_sha256( &digest_algorithm, FALSE ))
        return malformed_signature();

    if (!der_read_expected( &signed_data, 0x30, &content_info ))
        return malformed_signature();
    encapsulated.data = content_info.value;
    encapsulated.size = content_info.value_size;
    encapsulated.budget = &budget;
    if (!der_read_expected( &encapsulated, 0x06, &oid ) ||
        !oid_equal( &oid, oid_spc_indirect_data, sizeof(oid_spc_indirect_data) ) ||
        !der_read_expected( &encapsulated, 0xa0, &wrapper ) || encapsulated.size)
        return malformed_signature();
    econtent.data = wrapper.value;
    econtent.size = wrapper.value_size;
    econtent.budget = &budget;
    if (!der_read_expected( &econtent, 0x30, &content_info ) || econtent.size)
        return malformed_signature();
    *embedded_content = content_info.encoded;
    *embedded_content_size = content_info.encoded_size;
    /*
     * Authenticode encodes SpcIndirectDataContent directly below the
     * explicit eContent wrapper rather than inside an OCTET STRING.  The
     * authenticated messageDigest therefore covers the SEQUENCE contents,
     * while CMSG_CONTENT_PARAM exposes the complete encoded SEQUENCE.
     */
    *authenticated_content = content_info.value;
    *authenticated_content_size = content_info.value_size;

    /* Certificates and CRLs are optional CMS fields. */
    if (!signed_data.size) return malformed_signature();
    if (signed_data.data[0] == 0xa0 &&
        !der_read_expected( &signed_data, 0xa0, &certificates ))
        return malformed_signature();
    if (signed_data.size && signed_data.data[0] == 0xa1 &&
        !der_read_expected( &signed_data, 0xa1, &certificates ))
        return malformed_signature();
    if (!der_read_expected( &signed_data, 0x31, &signer_set ) || signed_data.size)
        return malformed_signature();
    signers.data = signer_set.value;
    signers.size = signer_set.value_size;
    signers.budget = &budget;
    if (!der_read_expected( &signers, 0x30, &signer ) || signers.size)
        return malformed_signature();
    signer_fields.data = signer.value;
    signer_fields.size = signer.value_size;
    signer_fields.budget = &budget;
    if (!der_read_expected( &signer_fields, 0x02, &version ) ||
        !der_integer_is_u32( &version, CMSG_SIGNER_INFO_V1 ) ||
        !der_read_expected( &signer_fields, 0x30, &content_info ))
        return malformed_signature();
    return S_OK;
}

static HRESULT validate_timestamp_cms_shape( const BYTE *data, SIZE_T size,
                                             const BYTE **embedded_content,
                                             SIZE_T *embedded_content_size )
{
    struct der_budget budget = {0};
    struct der_cursor document = {data, size, &budget}, outer, explicit_content;
    struct der_cursor signed_data, digest_algorithms, encapsulated, econtent, signers;
    struct der_cursor signer_fields;
    struct der_element sequence, oid, wrapper, signed_sequence, version;
    struct der_element digest_set, digest_algorithm, content_info, certificates;
    struct der_element signer_set, signer;

    if (!data || !size || size > MAX_CMS_CONTENT_SIZE ||
        !der_validate_document( data, size ))
        return malformed_signature();
    budget.elements = 0;
    if (!der_read_expected( &document, 0x30, &sequence ) || document.size)
        return malformed_signature();
    outer.data = sequence.value;
    outer.size = sequence.value_size;
    outer.budget = &budget;
    if (!der_read_expected( &outer, 0x06, &oid ) ||
        !oid_equal( &oid, oid_pkcs7_signed_data, sizeof(oid_pkcs7_signed_data) ) ||
        !der_read_expected( &outer, 0xa0, &wrapper ) || outer.size)
        return malformed_signature();
    explicit_content.data = wrapper.value;
    explicit_content.size = wrapper.value_size;
    explicit_content.budget = &budget;
    if (!der_read_expected( &explicit_content, 0x30, &signed_sequence ) ||
        explicit_content.size)
        return malformed_signature();

    signed_data.data = signed_sequence.value;
    signed_data.size = signed_sequence.value_size;
    signed_data.budget = &budget;
    if (!der_read_expected( &signed_data, 0x02, &version ) ||
        !der_integer_is_u32( &version, CMSG_SIGNED_DATA_V3 ) ||
        !der_read_expected( &signed_data, 0x31, &digest_set ))
        return malformed_signature();
    digest_algorithms.data = digest_set.value;
    digest_algorithms.size = digest_set.value_size;
    digest_algorithms.budget = &budget;
    if (!der_read_expected( &digest_algorithms, 0x30, &digest_algorithm ) ||
        digest_algorithms.size ||
        !algorithm_identifier_is_sha256( &digest_algorithm, FALSE ))
        return malformed_signature();

    if (!der_read_expected( &signed_data, 0x30, &content_info ))
        return malformed_signature();
    encapsulated.data = content_info.value;
    encapsulated.size = content_info.value_size;
    encapsulated.budget = &budget;
    if (!der_read_expected( &encapsulated, 0x06, &oid ) ||
        !oid_equal( &oid, oid_tst_info, sizeof(oid_tst_info) ) ||
        !der_read_expected( &encapsulated, 0xa0, &wrapper ) || encapsulated.size)
        return malformed_signature();
    econtent.data = wrapper.value;
    econtent.size = wrapper.value_size;
    econtent.budget = &budget;
    if (!der_read_expected( &econtent, 0x04, &content_info ) || econtent.size ||
        !content_info.value_size ||
        content_info.value_size > MAX_CMS_CONTENT_SIZE ||
        !der_validate_document( content_info.value, content_info.value_size ))
        return malformed_signature();
    *embedded_content = content_info.value;
    *embedded_content_size = content_info.value_size;

    if (!signed_data.size) return malformed_signature();
    if (signed_data.data[0] == 0xa0 &&
        !der_read_expected( &signed_data, 0xa0, &certificates ))
        return malformed_signature();
    if (signed_data.size && signed_data.data[0] == 0xa1 &&
        !der_read_expected( &signed_data, 0xa1, &certificates ))
        return malformed_signature();
    if (!der_read_expected( &signed_data, 0x31, &signer_set ) || signed_data.size)
        return malformed_signature();
    signers.data = signer_set.value;
    signers.size = signer_set.value_size;
    signers.budget = &budget;
    if (!der_read_expected( &signers, 0x30, &signer ) || signers.size)
        return malformed_signature();
    signer_fields.data = signer.value;
    signer_fields.size = signer.value_size;
    signer_fields.budget = &budget;
    if (!der_read_expected( &signer_fields, 0x02, &version ) ||
        !der_integer_is_u32( &version, CMSG_SIGNER_INFO_V1 ) ||
        !der_read_expected( &signer_fields, 0x30, &content_info ))
        return malformed_signature();
    return S_OK;
}

static HRESULT get_message_param( HCRYPTMSG message, DWORD parameter, DWORD index,
                                  SIZE_T maximum, void **result, DWORD *result_size )
{
    DWORD size = 0;
    void *buffer;

    *result = NULL;
    *result_size = 0;
    if (!CryptMsgGetParam( message, parameter, index, NULL, &size ))
    {
        TRACE( "message parameter %lu size query failed, error %#lx.\n",
               parameter, GetLastError() );
        return crypto_error( malformed_signature() );
    }
    if (!size || size > maximum) return malformed_signature();
    if (!(buffer = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size )))
        return E_OUTOFMEMORY;
    if (!CryptMsgGetParam( message, parameter, index, buffer, &size ))
    {
        HRESULT hr = crypto_error( malformed_signature() );
        TRACE( "message parameter %lu copy failed, error %#lx.\n",
               parameter, GetLastError() );
        HeapFree( GetProcessHeap(), 0, buffer );
        return hr;
    }
    *result = buffer;
    *result_size = size;
    return S_OK;
}

static BOOL decode_oid_attribute_value( const CRYPT_ATTR_BLOB *blob,
                                        const BYTE *oid, SIZE_T oid_size )
{
    struct der_budget budget = {0};
    struct der_cursor cursor;
    struct der_element element;

    if (!blob->pbData || !blob->cbData) return FALSE;
    cursor.data = blob->pbData;
    cursor.size = blob->cbData;
    cursor.budget = &budget;
    return der_read_expected( &cursor, 0x06, &element ) && !cursor.size &&
           oid_equal( &element, oid, oid_size );
}

static BOOL decode_digest_attribute_value( const CRYPT_ATTR_BLOB *blob,
                                           BYTE digest[APPX_SIGNATURE_SHA256_SIZE] )
{
    struct der_budget budget = {0};
    struct der_cursor cursor;
    struct der_element element;

    if (!blob->pbData || !blob->cbData) return FALSE;
    cursor.data = blob->pbData;
    cursor.size = blob->cbData;
    cursor.budget = &budget;
    if (!der_read_expected( &cursor, 0x04, &element ) || cursor.size ||
        element.value_size != APPX_SIGNATURE_SHA256_SIZE)
        return FALSE;
    memcpy( digest, element.value, APPX_SIGNATURE_SHA256_SIZE );
    return TRUE;
}

HRESULT WINAPI appx_signature_select_ess_attribute(
    UINT32 version1_count, UINT32 version2_count, UINT32 *version )
{
    if (!version) return E_POINTER;
    *version = 0;
    if (!((version1_count == 1 && !version2_count) ||
          (!version1_count && version2_count == 1)))
        return malformed_signature();
    *version = version1_count ? APPX_SIGNATURE_ESS_CERT_ID_V1 :
                               APPX_SIGNATURE_ESS_CERT_ID_V2;
    return S_OK;
}

static HRESULT validate_authenticated_attributes( const CMSG_SIGNER_INFO *signer,
                                                  DWORD signer_size,
                                                  const BYTE *content_type_oid,
                                                  SIZE_T content_type_oid_size,
                                                  const BYTE *content,
                                                  DWORD content_size,
                                                  BOOL timestamp_signer,
                                                  PCCERT_CONTEXT
                                                  timestamp_certificate )
{
    BYTE signed_digest[APPX_SIGNATURE_SHA256_SIZE], computed_digest[APPX_SIGNATURE_SHA256_SIZE];
    const CRYPT_ATTR_BLOB *ess_value = NULL;
    DWORD computed_size = sizeof(computed_digest);
    UINT32 content_type_count = 0, message_digest_count = 0;
    UINT32 ess_version1_count = 0, ess_version2_count = 0, ess_version;
    HRESULT hr;
    DWORD i;

    if (timestamp_signer != !!timestamp_certificate)
        return E_INVALIDARG;
    if (!signer->AuthAttrs.cAttr ||
        signer->AuthAttrs.cAttr > MAX_SIGNER_ATTRIBUTES ||
        signer->UnauthAttrs.cAttr > MAX_SIGNER_ATTRIBUTES ||
        !signer->EncryptedHash.cbData ||
        signer->EncryptedHash.cbData > MAX_SIGNER_INFO_SIZE ||
        !allocation_contains( signer, signer_size,
                              signer->EncryptedHash.pbData,
                              signer->EncryptedHash.cbData ) ||
        !allocation_contains( signer, signer_size, signer->AuthAttrs.rgAttr,
                              signer->AuthAttrs.cAttr *
                              sizeof(*signer->AuthAttrs.rgAttr) ) ||
        (signer->UnauthAttrs.cAttr &&
         !allocation_contains( signer, signer_size, signer->UnauthAttrs.rgAttr,
                               signer->UnauthAttrs.cAttr *
                               sizeof(*signer->UnauthAttrs.rgAttr) )))
        return malformed_signature();

    for (i = 0; i < signer->AuthAttrs.cAttr; i++)
    {
        const CRYPT_ATTRIBUTE *attribute = signer->AuthAttrs.rgAttr + i;

        if (!allocation_contains_string( signer, signer_size,
                                         attribute->pszObjId,
                                         MAX_ATTRIBUTE_OID_CHARS ) ||
            !attribute->cValue || attribute->cValue > MAX_ATTRIBUTE_VALUES ||
            !allocation_contains( signer, signer_size, attribute->rgValue,
                                  attribute->cValue * sizeof(*attribute->rgValue) ))
            return malformed_signature();
        {
            DWORD j;

            for (j = 0; j < attribute->cValue; j++)
            {
                const CRYPT_ATTR_BLOB *value = attribute->rgValue + j;

                if (value->cbData > MAX_CMS_CONTENT_SIZE ||
                    (value->cbData &&
                     !allocation_contains( signer, signer_size, value->pbData,
                                           value->cbData )))
                    return malformed_signature();
            }
        }
        if (!strcmp( attribute->pszObjId, szOID_RSA_contentType ))
        {
            if (++content_type_count != 1 || attribute->cValue != 1 ||
                !decode_oid_attribute_value( attribute->rgValue,
                                             content_type_oid,
                                             content_type_oid_size ))
                return malformed_signature();
        }
        else if (!strcmp( attribute->pszObjId, szOID_RSA_messageDigest ))
        {
            if (++message_digest_count != 1 || attribute->cValue != 1 ||
                !decode_digest_attribute_value( attribute->rgValue, signed_digest ))
                return malformed_signature();
        }
        else if (timestamp_signer &&
                 (!strcmp( attribute->pszObjId,
                           oid_signing_certificate ) ||
                  !strcmp( attribute->pszObjId,
                           oid_signing_certificate_v2 )))
        {
            if (!strcmp( attribute->pszObjId, oid_signing_certificate ))
                ess_version1_count++;
            else
                ess_version2_count++;
            if (attribute->cValue != 1) return malformed_signature();
            ess_value = attribute->rgValue;
        }
    }
    if (content_type_count != 1 || message_digest_count != 1)
        return malformed_signature();
    if (timestamp_signer)
    {
        if (FAILED( hr = appx_signature_select_ess_attribute(
                ess_version1_count, ess_version2_count, &ess_version ) ))
            return hr;
        if (!ess_value) return malformed_signature();
        if (FAILED( hr = appx_signature_validate_ess_certificate(
                ess_value->pbData, ess_value->cbData, ess_version,
                timestamp_certificate->pbCertEncoded,
                timestamp_certificate->cbCertEncoded ) ))
            return hr;
    }

    for (i = 0; i < signer->UnauthAttrs.cAttr; i++)
    {
        const CRYPT_ATTRIBUTE *attribute = signer->UnauthAttrs.rgAttr + i;

        if (!allocation_contains_string( signer, signer_size,
                                         attribute->pszObjId,
                                         MAX_ATTRIBUTE_OID_CHARS ) ||
            attribute->cValue > MAX_ATTRIBUTE_VALUES ||
            (attribute->cValue &&
             !allocation_contains( signer, signer_size, attribute->rgValue,
                                   attribute->cValue * sizeof(*attribute->rgValue) )))
            return malformed_signature();
        {
            DWORD j;

            for (j = 0; j < attribute->cValue; j++)
            {
                const CRYPT_ATTR_BLOB *value = attribute->rgValue + j;

                if (value->cbData > MAX_CMS_CONTENT_SIZE ||
                    (value->cbData &&
                     !allocation_contains( signer, signer_size, value->pbData,
                                           value->cbData )))
                    return malformed_signature();
            }
        }
        if (!strcmp( attribute->pszObjId, szOID_NESTED_SIGNATURE ) ||
            (timestamp_signer &&
             (!strcmp( attribute->pszObjId, szOID_RSA_counterSign ) ||
              !strcmp( attribute->pszObjId,
                       oid_rfc3161_counter_signature ) ||
              !strcmp( attribute->pszObjId,
                       oid_signing_certificate ) ||
              !strcmp( attribute->pszObjId,
                       oid_signing_certificate_v2 ))))
            return malformed_signature();
    }

    if (!CryptHashCertificate2( sha256_algorithm, 0, NULL, content, content_size,
                                computed_digest, &computed_size ) ||
        computed_size != sizeof(computed_digest))
        return crypto_error( TRUST_E_BAD_DIGEST );

    /* The CMS implementation verifies the signed attributes' signature.  This
     * explicit comparison additionally binds their messageDigest attribute to
     * the encapsulated SpcIndirectDataContent.
     */
    for (i = 0; i < APPX_SIGNATURE_SHA256_SIZE; i++)
        computed_digest[i] ^= signed_digest[i];
    for (i = 1; i < APPX_SIGNATURE_SHA256_SIZE; i++)
        computed_digest[0] |= computed_digest[i];
    return computed_digest[0] ? TRUST_E_BAD_DIGEST : S_OK;
}

static BOOL der_positive_integer( const struct der_element *element )
{
    if (element->tag != 0x02 || !element->value_size ||
        element->value[0] & 0x80)
        return FALSE;
    return element->value_size == 1 || element->value[0] ||
           element->value[1] & 0x80;
}

static BOOL validate_ess_general_names( const struct der_element *element,
                                        struct der_budget *budget )
{
    struct der_cursor names, contents;
    struct der_element name, value;
    UINT32 count = 0;
    SIZE_T i;

    if (element->tag != 0x30 || !element->value_size) return FALSE;
    names.data = element->value;
    names.size = element->value_size;
    names.budget = budget;
    while (names.size)
    {
        if (!der_read_element( &names, &name ) || !name.value_size ||
            ++count > MAX_ESS_CERTIFICATE_IDS)
            return FALSE;
        switch (name.tag)
        {
        case 0xa0: /* otherName */
            contents.data = name.value;
            contents.size = name.value_size;
            contents.budget = budget;
            if (!der_read_expected( &contents, 0x06, &value ) ||
                !der_read_expected( &contents, 0xa0, &value ) ||
                contents.size || !value.value_size)
                return FALSE;
            break;
        case 0x81: /* rfc822Name */
        case 0x82: /* dNSName */
        case 0x86: /* uniformResourceIdentifier */
            for (i = 0; i < name.value_size; i++)
                if (name.value[i] & 0x80) return FALSE;
            break;
        case 0xa3: /* x400Address */
        case 0xa5: /* ediPartyName */
            break;
        case 0xa4: /* directoryName */
            contents.data = name.value;
            contents.size = name.value_size;
            contents.budget = budget;
            if (!der_read_expected( &contents, 0x30, &value ) ||
                contents.size)
                return FALSE;
            break;
        case 0x87: /* iPAddress */
            if (name.value_size != 4 && name.value_size != 16)
                return FALSE;
            break;
        case 0x88: /* registeredID */
            if (!der_oid_is_valid( name.value, name.value_size ))
                return FALSE;
            break;
        default:
            return FALSE;
        }
    }
    return count != 0;
}

static BOOL validate_ess_issuer_serial( const struct der_element *element,
                                        struct der_budget *budget )
{
    struct der_cursor fields;
    struct der_element issuer, serial;

    if (element->tag != 0x30) return FALSE;
    fields.data = element->value;
    fields.size = element->value_size;
    fields.budget = budget;
    return der_read_expected( &fields, 0x30, &issuer ) &&
           der_read_expected( &fields, 0x02, &serial ) && !fields.size &&
           validate_ess_general_names( &issuer, budget ) &&
           der_positive_integer( &serial );
}

static BOOL validate_ess_policies( const struct der_element *element,
                                   struct der_budget *budget )
{
    struct der_cursor policies, fields, qualifiers, qualifier_fields;
    struct der_element policy, oid, qualifier_set, qualifier, value;
    UINT32 policy_count = 0, qualifier_count;

    if (element->tag != 0x30 || !element->value_size) return FALSE;
    policies.data = element->value;
    policies.size = element->value_size;
    policies.budget = budget;
    while (policies.size)
    {
        if (!der_read_expected( &policies, 0x30, &policy ) ||
            ++policy_count > MAX_SIGNER_ATTRIBUTES)
            return FALSE;
        fields.data = policy.value;
        fields.size = policy.value_size;
        fields.budget = budget;
        if (!der_read_expected( &fields, 0x06, &oid ))
            return FALSE;
        if (!fields.size) continue;
        if (!der_read_expected( &fields, 0x30, &qualifier_set ) ||
            fields.size || !qualifier_set.value_size)
            return FALSE;
        qualifiers.data = qualifier_set.value;
        qualifiers.size = qualifier_set.value_size;
        qualifiers.budget = budget;
        qualifier_count = 0;
        while (qualifiers.size)
        {
            if (!der_read_expected( &qualifiers, 0x30, &qualifier ) ||
                ++qualifier_count > MAX_SIGNER_ATTRIBUTES)
                return FALSE;
            qualifier_fields.data = qualifier.value;
            qualifier_fields.size = qualifier.value_size;
            qualifier_fields.budget = budget;
            if (!der_read_expected( &qualifier_fields, 0x06, &oid ) ||
                !der_read_element( &qualifier_fields, &value ) ||
                qualifier_fields.size)
                return FALSE;
        }
    }
    return policy_count != 0;
}

static BOOL parse_ess_certificate_id( const struct der_element *element,
                                      BOOL version2,
                                      struct der_budget *budget,
                                      const BYTE **hash )
{
    struct der_cursor fields;
    struct der_element value;
    SIZE_T expected_size = version2 ? APPX_SIGNATURE_SHA256_SIZE : 20;

    if (element->tag != 0x30) return FALSE;
    fields.data = element->value;
    fields.size = element->value_size;
    fields.budget = budget;
    if (version2 && fields.size && fields.data[0] == 0x30)
    {
        if (!der_read_expected( &fields, 0x30, &value ) ||
            !algorithm_identifier_is_sha256( &value, FALSE ))
            return FALSE;
    }
    if (!der_read_expected( &fields, 0x04, &value ) ||
        value.value_size != expected_size)
        return FALSE;
    if (!*hash) *hash = value.value;
    if (!fields.size) return TRUE;
    return der_read_expected( &fields, 0x30, &value ) && !fields.size &&
           validate_ess_issuer_serial( &value, budget );
}

static BOOL parse_ess_signing_certificate( const BYTE *data, SIZE_T size,
                                           BOOL version2,
                                           const BYTE **signer_hash )
{
    struct der_budget budget = {0};
    struct der_cursor document, contents, certificates;
    struct der_element sequence, certificate_ids, certificate_id, policies;
    UINT32 count = 0;

    *signer_hash = NULL;
    if (!data || !size || size > MAX_CMS_CONTENT_SIZE ||
        !der_validate_document( data, size ))
        return FALSE;
    document.data = data;
    document.size = size;
    document.budget = &budget;
    if (!der_read_expected( &document, 0x30, &sequence ) || document.size)
        return FALSE;
    contents.data = sequence.value;
    contents.size = sequence.value_size;
    contents.budget = &budget;
    if (!der_read_expected( &contents, 0x30, &certificate_ids ) ||
        !certificate_ids.value_size)
        return FALSE;
    certificates.data = certificate_ids.value;
    certificates.size = certificate_ids.value_size;
    certificates.budget = &budget;
    while (certificates.size)
    {
        if (!der_read_expected( &certificates, 0x30, &certificate_id ) ||
            ++count > MAX_ESS_CERTIFICATE_IDS ||
            !parse_ess_certificate_id( &certificate_id, version2, &budget,
                                       signer_hash ))
            return FALSE;
    }
    if (!count || !*signer_hash) return FALSE;
    if (contents.size)
    {
        if (!der_read_expected( &contents, 0x30, &policies ) ||
            !validate_ess_policies( &policies, &budget ))
            return FALSE;
    }
    return !contents.size;
}

static BOOL constant_time_equal( const BYTE *left, const BYTE *right,
                                 SIZE_T size )
{
    BYTE difference = 0;
    SIZE_T i;

    for (i = 0; i < size; i++) difference |= left[i] ^ right[i];
    return !difference;
}

HRESULT WINAPI appx_signature_validate_ess_certificate(
    const BYTE *attribute, UINT32 attribute_size, UINT32 version,
    const BYTE *certificate, UINT32 certificate_size )
{
    BYTE digest[APPX_SIGNATURE_SHA256_SIZE];
    const BYTE *signed_digest;
    const WCHAR *algorithm;
    DWORD digest_size;
    BOOL version2;

    if (!attribute || !attribute_size ||
        attribute_size > MAX_CMS_CONTENT_SIZE || !certificate ||
        !certificate_size ||
        certificate_size > MAX_EMBEDDED_CERTIFICATE_SIZE ||
        (version != APPX_SIGNATURE_ESS_CERT_ID_V1 &&
         version != APPX_SIGNATURE_ESS_CERT_ID_V2))
        return E_INVALIDARG;
    version2 = version == APPX_SIGNATURE_ESS_CERT_ID_V2;
    if (!parse_ess_signing_certificate( attribute, attribute_size, version2,
                                        &signed_digest ))
        return malformed_signature();

    algorithm = version2 ? sha256_algorithm : sha1_algorithm;
    digest_size = version2 ? APPX_SIGNATURE_SHA256_SIZE : 20;
    if (!CryptHashCertificate2( algorithm, 0, NULL, certificate,
                                certificate_size, digest, &digest_size ) ||
        digest_size != (version2 ? APPX_SIGNATURE_SHA256_SIZE : 20))
        return crypto_error( TRUST_E_BAD_DIGEST );
    return constant_time_equal( digest, signed_digest, digest_size ) ?
           S_OK : TRUST_E_SUBJECT_NOT_TRUSTED;
}

static BOOL der_implicit_u32( const struct der_element *element, BYTE tag,
                              DWORD maximum )
{
    DWORD value = 0;
    SIZE_T i;

    if (element->tag != tag || !element->value_size ||
        element->value_size > sizeof(value) || element->value[0] & 0x80 ||
        (element->value_size > 1 && !element->value[0] &&
         !(element->value[1] & 0x80)))
        return FALSE;
    for (i = 0; i < element->value_size; i++)
        value = (value << 8) | element->value[i];
    return value <= maximum;
}

static BOOL decimal_pair( const BYTE *value, WORD *result )
{
    if (value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9')
        return FALSE;
    *result = (value[0] - '0') * 10 + value[1] - '0';
    return TRUE;
}

static HRESULT decode_timestamp_time( const struct der_element *element,
                                      FILETIME *result )
{
    SYSTEMTIME system_time = {0};
    const BYTE *value = element->value;
    SIZE_T fraction_start = 0, fraction_size = 0, i;

    if (element->tag != 0x18 || element->value_size < 15 ||
        element->value_size > 32 || value[element->value_size - 1] != 'Z')
        return malformed_signature();
    for (i = 0; i < 14; i++)
        if (value[i] < '0' || value[i] > '9')
            return malformed_signature();
    if (element->value_size != 15)
    {
        if (value[14] != '.' || element->value_size < 17)
            return malformed_signature();
        fraction_start = 15;
        fraction_size = element->value_size - 16;
        if (fraction_size > 9 ||
            value[fraction_start + fraction_size - 1] == '0')
            return malformed_signature();
        for (i = 0; i < fraction_size; i++)
            if (value[fraction_start + i] < '0' ||
                value[fraction_start + i] > '9')
                return malformed_signature();
    }

    system_time.wYear = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
                        (value[2] - '0') * 10 + value[3] - '0';
    if (!decimal_pair( value + 4, &system_time.wMonth ) ||
        !decimal_pair( value + 6, &system_time.wDay ) ||
        !decimal_pair( value + 8, &system_time.wHour ) ||
        !decimal_pair( value + 10, &system_time.wMinute ) ||
        !decimal_pair( value + 12, &system_time.wSecond ))
        return malformed_signature();
    for (i = 0; i < 3; i++)
    {
        system_time.wMilliseconds *= 10;
        if (i < fraction_size)
            system_time.wMilliseconds += value[fraction_start + i] - '0';
    }
    if (!SystemTimeToFileTime( &system_time, result ))
        return malformed_signature();
    return S_OK;
}

static HRESULT parse_timestamp_info( const BYTE *data, SIZE_T size,
                                     const CMSG_SIGNER_INFO *outer_signer,
                                     FILETIME *timestamp )
{
    BYTE signed_imprint[APPX_SIGNATURE_SHA256_SIZE];
    BYTE computed_imprint[APPX_SIGNATURE_SHA256_SIZE];
    DWORD computed_size = sizeof(computed_imprint);
    struct der_budget budget = {0};
    struct der_cursor document = {data, size, &budget}, contents, imprint, accuracy;
    struct der_element sequence, value, algorithm, digest;
    FILETIME current_time;
    SIZE_T i;

    if (!der_validate_document( data, size ) ||
        !der_read_expected( &document, 0x30, &sequence ) || document.size)
        return malformed_signature();
    contents.data = sequence.value;
    contents.size = sequence.value_size;
    contents.budget = &budget;
    if (!der_read_expected( &contents, 0x02, &value ) ||
        !der_integer_is_u32( &value, 1 ) ||
        !der_read_expected( &contents, 0x06, &value ) || !value.value_size ||
        !der_read_expected( &contents, 0x30, &sequence ))
        return malformed_signature();

    imprint.data = sequence.value;
    imprint.size = sequence.value_size;
    imprint.budget = &budget;
    if (!der_read_expected( &imprint, 0x30, &algorithm ) ||
        !algorithm_identifier_is_sha256( &algorithm, FALSE ) ||
        !der_read_expected( &imprint, 0x04, &digest ) || imprint.size ||
        digest.value_size != sizeof(signed_imprint))
        return malformed_signature();
    memcpy( signed_imprint, digest.value, sizeof(signed_imprint) );
    if (!CryptHashCertificate2(
            sha256_algorithm, 0, NULL, outer_signer->EncryptedHash.pbData,
            outer_signer->EncryptedHash.cbData, computed_imprint,
            &computed_size ) ||
        computed_size != sizeof(computed_imprint))
        return crypto_error( TRUST_E_BAD_DIGEST );
    for (i = 0; i < sizeof(computed_imprint); i++)
        computed_imprint[i] ^= signed_imprint[i];
    for (i = 1; i < sizeof(computed_imprint); i++)
        computed_imprint[0] |= computed_imprint[i];
    if (computed_imprint[0])
    {
        TRACE( "RFC3161 message imprint does not bind the package signer value.\n" );
        return TRUST_E_BAD_DIGEST;
    }

    if (!der_read_expected( &contents, 0x02, &value ) ||
        !der_positive_integer( &value ) ||
        !der_read_expected( &contents, 0x18, &value ))
        return malformed_signature();
    if (FAILED( decode_timestamp_time( &value, timestamp ) ))
        return malformed_signature();

    if (contents.size && contents.data[0] == 0x30)
    {
        if (!der_read_expected( &contents, 0x30, &sequence ))
            return malformed_signature();
        accuracy.data = sequence.value;
        accuracy.size = sequence.value_size;
        accuracy.budget = &budget;
        if (accuracy.size && accuracy.data[0] == 0x02)
        {
            if (!der_read_expected( &accuracy, 0x02, &value ) ||
                !der_positive_integer( &value ))
                return malformed_signature();
        }
        if (accuracy.size && accuracy.data[0] == 0x80)
        {
            if (!der_read_expected( &accuracy, 0x80, &value ) ||
                !der_implicit_u32( &value, 0x80, 999 ))
                return malformed_signature();
        }
        if (accuracy.size && accuracy.data[0] == 0x81)
        {
            if (!der_read_expected( &accuracy, 0x81, &value ) ||
                !der_implicit_u32( &value, 0x81, 999 ))
                return malformed_signature();
        }
        if (accuracy.size) return malformed_signature();
    }
    if (contents.size && contents.data[0] == 0x01)
    {
        if (!der_read_expected( &contents, 0x01, &value ) ||
            value.value_size != 1 || value.value[0] != 0xff)
            return malformed_signature();
    }
    if (contents.size && contents.data[0] == 0x02)
    {
        if (!der_read_expected( &contents, 0x02, &value ) ||
            !der_positive_integer( &value ))
            return malformed_signature();
    }
    if (contents.size && contents.data[0] == 0xa0)
    {
        if (!der_read_expected( &contents, 0xa0, &value ) || !value.value_size)
            return malformed_signature();
    }
    if (contents.size && contents.data[0] == 0xa1)
    {
        if (!der_read_expected( &contents, 0xa1, &value ) || !value.value_size)
            return malformed_signature();
    }
    if (contents.size) return malformed_signature();

    GetSystemTimeAsFileTime( &current_time );
    if (CompareFileTime( timestamp, &current_time ) > 0)
        return CERT_E_EXPIRED;
    return S_OK;
}

static HRESULT load_certificate_store( HCRYPTMSG message, HCERTSTORE *result )
{
    HCERTSTORE store;
    DWORD count, size = sizeof(count), i;
    SIZE_T total = 0;
    BYTE *encoded = NULL;
    HRESULT hr = S_OK;

    *result = NULL;
    if (!CryptMsgGetParam( message, CMSG_CERT_COUNT_PARAM, 0, &count, &size ) ||
        size != sizeof(count) || !count || count > MAX_EMBEDDED_CERTIFICATES)
        return malformed_signature();
    if (!(store = CertOpenStore( CERT_STORE_PROV_MEMORY, 0, 0,
                               CERT_STORE_CREATE_NEW_FLAG, NULL )))
        return crypto_error( E_FAIL );

    for (i = 0; i < count; i++)
    {
        size = 0;
        if (!CryptMsgGetParam( message, CMSG_CERT_PARAM, i, NULL, &size ) ||
            !size || size > MAX_EMBEDDED_CERTIFICATE_SIZE ||
            total > APPX_SIGNATURE_MAX_SIZE - size)
        {
            hr = malformed_signature();
            break;
        }
        total += size;
        if (!(encoded = HeapAlloc( GetProcessHeap(), 0, size )))
        {
            hr = E_OUTOFMEMORY;
            break;
        }
        if (!CryptMsgGetParam( message, CMSG_CERT_PARAM, i, encoded, &size ) ||
            !CertAddEncodedCertificateToStore( store,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded, size,
                CERT_STORE_ADD_ALWAYS, NULL ))
            hr = crypto_error( malformed_signature() );
        HeapFree( GetProcessHeap(), 0, encoded );
        encoded = NULL;
        if (FAILED( hr )) break;
    }
    if (FAILED( hr ))
    {
        CertCloseStore( store, 0 );
        return hr;
    }
    *result = store;
    return S_OK;
}

static HRESULT get_signer_certificate( HCRYPTMSG message, HCERTSTORE store,
                                       PCCERT_CONTEXT *result )
{
    CERT_INFO *info = NULL;
    DWORD size;
    HRESULT hr;

    *result = NULL;
    if (FAILED( hr = get_message_param( message, CMSG_SIGNER_CERT_INFO_PARAM, 0,
                                      MAX_SIGNER_INFO_SIZE, (void **)&info, &size ) ))
        return hr;
    if (size < sizeof(*info))
    {
        HeapFree( GetProcessHeap(), 0, info );
        return malformed_signature();
    }
    *result = CertGetSubjectCertificateFromStore( store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, info );
    HeapFree( GetProcessHeap(), 0, info );
    return *result ? S_OK : crypto_error( TRUST_E_NO_SIGNER_CERT );
}

static BOOL is_relaxable_chain_error( HRESULT error )
{
    return error == CERT_E_UNTRUSTEDROOT || error == CERT_E_CHAINING ||
           error == CERT_E_UNTRUSTEDTESTROOT;
}

static BOOL trust_status_is_relaxable( DWORD errors )
{
    const DWORD relaxable = CERT_TRUST_IS_UNTRUSTED_ROOT |
                            CERT_TRUST_IS_PARTIAL_CHAIN;

    return errors && !(errors & ~relaxable);
}

static HRESULT trust_status_to_hresult( DWORD errors )
{
    if (errors & CERT_TRUST_IS_EXPLICIT_DISTRUST)
        return TRUST_E_EXPLICIT_DISTRUST;
    if (errors & CERT_TRUST_IS_REVOKED)
        return CERT_E_REVOKED;
    if (errors & (CERT_TRUST_REVOCATION_STATUS_UNKNOWN |
                  CERT_TRUST_IS_OFFLINE_REVOCATION))
        return CRYPT_E_REVOCATION_OFFLINE;
    if (errors & CERT_TRUST_IS_NOT_SIGNATURE_VALID)
        return TRUST_E_CERT_SIGNATURE;
    if (errors & CERT_TRUST_IS_NOT_TIME_VALID)
        return CERT_E_EXPIRED;
    if (errors & CERT_TRUST_IS_NOT_TIME_NESTED)
        return CERT_E_VALIDITYPERIODNESTING;
    if (errors & CERT_TRUST_IS_NOT_VALID_FOR_USAGE)
        return CERT_E_WRONG_USAGE;
    if (errors & (CERT_TRUST_HAS_NOT_SUPPORTED_CRITICAL_EXT |
                  CERT_TRUST_INVALID_EXTENSION))
        return CERT_E_CRITICAL;
    if (errors & CERT_TRUST_INVALID_BASIC_CONSTRAINTS)
        return TRUST_E_BASIC_CONSTRAINTS;
    if (errors & CERT_TRUST_INVALID_POLICY_CONSTRAINTS)
        return CERT_E_INVALID_POLICY;
    if (errors & CERT_TRUST_IS_CYCLIC)
        return CERT_E_CHAINING;
    if (errors & CERT_TRUST_IS_PARTIAL_CHAIN)
        return CERT_E_CHAINING;
    if (errors & CERT_TRUST_IS_UNTRUSTED_ROOT)
        return CERT_E_UNTRUSTEDROOT;
    return TRUST_E_SUBJECT_NOT_TRUSTED;
}

static HRESULT normalize_chain_policy_error( HRESULT error )
{
    if (error && SUCCEEDED( error ))
        return TRUST_E_SUBJECT_NOT_TRUSTED;
    if (error == CERT_E_REVOKED || error == CRYPT_E_REVOKED)
        return CERT_E_REVOKED;
    if (error == CRYPT_E_REVOCATION_OFFLINE ||
        error == CRYPT_E_NO_REVOCATION_CHECK ||
        error == CRYPT_E_NO_REVOCATION_DLL ||
        error == CRYPT_E_NOT_IN_REVOCATION_DATABASE ||
        error == CERT_E_REVOCATION_FAILURE)
        return CRYPT_E_REVOCATION_OFFLINE;
    return error;
}

HRESULT WINAPI appx_signature_get_chain_policy(
    UINT32 flags, BOOL online, DWORD time_flags, DWORD *chain_flags,
    DWORD *url_timeout )
{
    if (!chain_flags || !url_timeout ||
        flags & ~(APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN |
                  APPX_SIGNATURE_VERIFY_BUNDLE) ||
        time_flags & ~CERT_CHAIN_TIMESTAMP_TIME)
        return E_INVALIDARG;
    *chain_flags = 0;
    *url_timeout = 0;

    /* The developer/test policy is deliberately incapable of network I/O. */
    if (online && (flags & APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN))
        return S_FALSE;
    *chain_flags = CERT_CHAIN_CACHE_END_CERT |
                   CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE | time_flags;
    if (!online)
    {
        *chain_flags |= CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL;
        return S_OK;
    }
    *chain_flags |= CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                    CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT;
    *url_timeout = APPX_CHAIN_URL_TIMEOUT_MS;
    return S_OK;
}

HRESULT WINAPI appx_signature_evaluate_chain_status(
    UINT32 flags, BOOL online, HRESULT policy_error, DWORD trust_errors )
{
    BOOL relax_untrusted;

    if (flags & ~(APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN |
                  APPX_SIGNATURE_VERIFY_BUNDLE) ||
        (online && (flags & APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN)))
        return E_INVALIDARG;
    relax_untrusted =
        !online && (flags & APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN);
    policy_error = normalize_chain_policy_error( policy_error );
    if (policy_error &&
        (!relax_untrusted || !is_relaxable_chain_error( policy_error )))
        return policy_error;
    if (trust_errors &&
        (!relax_untrusted || !trust_status_is_relaxable( trust_errors )))
        return trust_status_to_hresult( trust_errors );
    return S_OK;
}

static HRESULT build_and_validate_certificate_chain(
    PCCERT_CONTEXT certificate, HCERTSTORE additional_store, UINT32 flags,
    const char *usage, const FILETIME *verification_time, DWORD time_flags,
    BOOL online )
{
    LPSTR requested_usage = (LPSTR)usage;
    CERT_CHAIN_POLICY_STATUS status = {sizeof(status), 0};
    CERT_CHAIN_POLICY_PARA policy = {sizeof(policy), 0};
    CERT_CHAIN_PARA parameters = {sizeof(parameters)};
    PCCERT_CHAIN_CONTEXT chain = NULL;
    DWORD chain_flags, trust_errors, url_timeout;
    HRESULT hr;

    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &requested_usage;
    if (FAILED( hr = appx_signature_get_chain_policy(
            flags, online, time_flags, &chain_flags, &url_timeout ) ) ||
        hr == S_FALSE)
        return hr;
    parameters.dwUrlRetrievalTimeout = url_timeout;

    if (!CertGetCertificateChain( NULL, certificate,
                                  (FILETIME *)verification_time,
                                  additional_store,
                                  &parameters, chain_flags, NULL, &chain ))
        return crypto_error( TRUST_E_SUBJECT_NOT_TRUSTED );
    trust_errors = chain->TrustStatus.dwErrorStatus;
    if (!CertVerifyCertificateChainPolicy( CERT_CHAIN_POLICY_AUTHENTICODE,
                                           chain, &policy, &status ))
        hr = crypto_error( TRUST_E_SUBJECT_NOT_TRUSTED );
    else
        hr = appx_signature_evaluate_chain_status(
            flags, online, status.dwError, trust_errors );
    CertFreeCertificateChain( chain );
    return hr;
}

static HRESULT verify_certificate_chain( PCCERT_CONTEXT certificate,
                                         HCERTSTORE additional_store,
                                         UINT32 flags, const char *usage,
                                         const FILETIME *verification_time,
                                         DWORD time_flags )
{
    HRESULT hr;

    /*
     * Do not dereference certificate-controlled AIA or revocation URLs until
     * a complete local chain has passed trust, signature, time, requested
     * usage, and critical-extension policy.  The second build is therefore
     * reachable only for a signer already anchored in the local trust store.
     */
    if (FAILED( hr = build_and_validate_certificate_chain(
            certificate, additional_store, flags, usage, verification_time,
            time_flags, FALSE ) ))
        return hr;
    if (flags & APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN)
        return S_OK;

    /*
     * Check every non-root certificate.  The accumulated ten-second budget is
     * passed through CERT_CHAIN_PARA so all revocation retrievals share it.
     * Unknown, unavailable, and offline status are hard failures.
     */
    return build_and_validate_certificate_chain(
        certificate, additional_store, flags, usage, verification_time,
        time_flags, TRUE );
}

static HRESULT get_rfc3161_timestamp( const CMSG_SIGNER_INFO *signer,
                                      const BYTE **data, DWORD *size )
{
    UINT32 timestamp_count = 0, countersign_count = 0;
    DWORD i;

    *data = NULL;
    *size = 0;
    for (i = 0; i < signer->UnauthAttrs.cAttr; i++)
    {
        const CRYPT_ATTRIBUTE *attribute = signer->UnauthAttrs.rgAttr + i;

        if (!strcmp( attribute->pszObjId, oid_rfc3161_counter_signature ))
        {
            if (++timestamp_count != 1 || attribute->cValue != 1 ||
                !attribute->rgValue[0].cbData)
                return malformed_signature();
            *data = attribute->rgValue[0].pbData;
            *size = attribute->rgValue[0].cbData;
        }
        else if (!strcmp( attribute->pszObjId, szOID_RSA_counterSign ))
            countersign_count++;
    }
    if (timestamp_count && countersign_count) return malformed_signature();
    return timestamp_count ? S_OK : S_FALSE;
}

static HRESULT verify_rfc3161_timestamp( const CMSG_SIGNER_INFO *outer_signer,
                                         UINT32 flags, FILETIME *timestamp )
{
    CMSG_CTRL_VERIFY_SIGNATURE_EX_PARA verify_parameters;
    PCCERT_CONTEXT signer_certificate = NULL;
    HCERTSTORE certificate_store = NULL;
    CMSG_SIGNER_INFO *signer = NULL;
    HCRYPTMSG message = NULL;
    char *content_oid = NULL;
    BYTE *content = NULL;
    const BYTE *timestamp_data, *embedded_content;
    struct der_budget content_budget = {0};
    struct der_cursor content_cursor;
    struct der_element content_element;
    SIZE_T embedded_content_size;
    DWORD timestamp_size, content_size, parameter_size;
    DWORD signer_count, message_type;
    const char *stage = "attribute";
    HRESULT hr;

    hr = get_rfc3161_timestamp( outer_signer, &timestamp_data, &timestamp_size );
    if (hr != S_OK) return hr;
    stage = "CMS shape";
    if (timestamp_size > MAX_CMS_CONTENT_SIZE ||
        FAILED( hr = validate_timestamp_cms_shape(
            timestamp_data, timestamp_size, &embedded_content,
            &embedded_content_size ) ))
    {
        hr = FAILED( hr ) ? hr : malformed_signature();
        TRACE( "RFC3161 %s failed, hr %#lx.\n", stage, hr );
        return hr;
    }

    stage = "message open";
    if (!(message = CryptMsgOpenToDecode(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, 0, 0, NULL, NULL )))
        return crypto_error( malformed_signature() );
    stage = "message decode";
    if (!CryptMsgUpdate( message, timestamp_data, timestamp_size, TRUE ))
    {
        hr = crypto_error( malformed_signature() );
        goto done;
    }
    stage = "message type";
    parameter_size = sizeof(message_type);
    if (!CryptMsgGetParam( message, CMSG_TYPE_PARAM, 0, &message_type,
                           &parameter_size ) ||
        parameter_size != sizeof(message_type) || message_type != CMSG_SIGNED)
    {
        hr = malformed_signature();
        goto done;
    }
    stage = "signer count";
    parameter_size = sizeof(signer_count);
    if (!CryptMsgGetParam( message, CMSG_SIGNER_COUNT_PARAM, 0, &signer_count,
                           &parameter_size ) ||
        parameter_size != sizeof(signer_count) || signer_count != 1)
    {
        hr = malformed_signature();
        goto done;
    }
    stage = "content type";
    if (FAILED( hr = get_message_param(
            message, CMSG_INNER_CONTENT_TYPE_PARAM, 0,
            MAX_ATTRIBUTE_OID_CHARS, (void **)&content_oid,
            &parameter_size ) ))
        goto done;
    stage = "content type value";
    if (parameter_size != sizeof(oid_tst_info_string) ||
        memcmp( content_oid, oid_tst_info_string,
                sizeof(oid_tst_info_string) ))
    {
        hr = malformed_signature();
        goto done;
    }
    stage = "content";
    if (FAILED( hr = get_message_param(
            message, CMSG_CONTENT_PARAM, 0, MAX_CMS_CONTENT_SIZE,
            (void **)&content, &parameter_size ) ))
        goto done;
    content_size = parameter_size;
    content_cursor.data = content;
    content_cursor.size = content_size;
    content_cursor.budget = &content_budget;
    if (!((content_size == embedded_content_size &&
           !memcmp( content, embedded_content, content_size )) ||
          (der_read_expected( &content_cursor, 0x04, &content_element ) &&
           !content_cursor.size &&
           content_element.value_size == embedded_content_size &&
           !memcmp( content_element.value, embedded_content,
                    embedded_content_size ))))
    {
        hr = malformed_signature();
        goto done;
    }
    stage = "signer info";
    if (FAILED( hr = get_message_param(
            message, CMSG_SIGNER_INFO_PARAM, 0, MAX_SIGNER_INFO_SIZE,
            (void **)&signer, &parameter_size ) ))
        goto done;
    stage = "signer algorithms";
    if (parameter_size < sizeof(*signer) ||
        signer->dwVersion != CMSG_SIGNER_INFO_V1 ||
        !crypt_algorithm_is_sha256( &signer->HashAlgorithm, signer,
                                    parameter_size ) ||
        (!crypt_algorithm_is_rsa( &signer->HashEncryptionAlgorithm, signer,
                                  parameter_size ) &&
         !crypt_algorithm_is_sha256_rsa(
             &signer->HashEncryptionAlgorithm, signer, parameter_size )))
    {
        hr = malformed_signature();
        goto done;
    }
    stage = "certificate selection";
    if (FAILED( hr = load_certificate_store(
            message, &certificate_store ) ) ||
        FAILED( hr = get_signer_certificate(
            message, certificate_store, &signer_certificate ) ))
    {
        TRACE( "RFC3161 certificate selection failed, hr %#lx.\n", hr );
        goto done;
    }
    stage = "authenticated attributes";
    if (FAILED( hr = validate_authenticated_attributes(
            signer, parameter_size, oid_tst_info, sizeof(oid_tst_info),
            embedded_content, (DWORD)embedded_content_size, TRUE,
            signer_certificate ) ))
    {
        TRACE( "RFC3161 authenticated attributes failed, hr %#lx.\n", hr );
        goto done;
    }
    stage = "timestamp info";
    if (FAILED( hr = parse_timestamp_info(
            embedded_content, embedded_content_size,
            outer_signer, timestamp ) ))
    {
        TRACE( "RFC3161 timestamp info failed, hr %#lx.\n", hr );
        goto done;
    }
    stage = "leaf certificate extensions";
    if (!signer_certificate->pCertInfo ||
        FAILED( hr = appx_signature_validate_leaf_extensions(
            signer_certificate->pCertInfo->rgExtension,
            signer_certificate->pCertInfo->cExtension, TRUE ) ))
    {
        if (!signer_certificate->pCertInfo) hr = malformed_signature();
        TRACE( "RFC3161 leaf certificate extensions failed, hr %#lx.\n", hr );
        goto done;
    }

    stage = "CMS signer verification";
    memset( &verify_parameters, 0, sizeof(verify_parameters) );
    verify_parameters.cbSize = sizeof(verify_parameters);
    verify_parameters.dwSignerIndex = 0;
    verify_parameters.dwSignerType = CMSG_VERIFY_SIGNER_CERT;
    verify_parameters.pvSigner = (void *)signer_certificate;
    if (!CryptMsgControl( message, 0, CMSG_CTRL_VERIFY_SIGNATURE_EX,
                          &verify_parameters ))
    {
        hr = crypto_error( TRUST_E_CERT_SIGNATURE );
        TRACE( "RFC3161 CMS signer verification failed, hr %#lx.\n", hr );
        goto done;
    }
    stage = "signer chain";
    hr = verify_certificate_chain(
        signer_certificate, certificate_store, flags,
        szOID_PKIX_KP_TIMESTAMP_SIGNING, timestamp, 0 );
    if (FAILED( hr ))
        TRACE( "RFC3161 signer chain failed, hr %#lx.\n", hr );

done:
    if (FAILED( hr ))
        TRACE( "RFC3161 %s failed, hr %#lx.\n", stage, hr );
    if (signer_certificate) CertFreeCertificateContext( signer_certificate );
    if (certificate_store) CertCloseStore( certificate_store, 0 );
    HeapFree( GetProcessHeap(), 0, signer );
    HeapFree( GetProcessHeap(), 0, content );
    HeapFree( GetProcessHeap(), 0, content_oid );
    if (message) CryptMsgClose( message );
    return hr;
}

static HRESULT copy_signer_subject( PCCERT_CONTEXT certificate,
                                    struct appx_signature *signature )
{
    CERT_NAME_BLOB *name = &certificate->pCertInfo->Subject;
    BYTE *encoded;
    DWORD length;
    WCHAR *subject;

    if (!name->pbData || !name->cbData ||
        name->cbData > MAX_SIGNER_SUBJECT_DER_SIZE)
        return malformed_signature();
    length = CertNameToStrW( certificate->dwCertEncodingType,
                             name,
                             CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                             NULL, 0 );
    if (length <= 1 || length > MAX_SIGNER_SUBJECT_CHARS)
        return malformed_signature();
    if (!(subject = HeapAlloc( GetProcessHeap(), 0, length * sizeof(*subject) )))
        return E_OUTOFMEMORY;
    if (!(encoded = HeapAlloc( GetProcessHeap(), 0, name->cbData )))
    {
        HeapFree( GetProcessHeap(), 0, subject );
        return E_OUTOFMEMORY;
    }
    if (CertNameToStrW( certificate->dwCertEncodingType,
                        name,
                        CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                        subject, length ) != length)
    {
        HeapFree( GetProcessHeap(), 0, subject );
        HeapFree( GetProcessHeap(), 0, encoded );
        return crypto_error( malformed_signature() );
    }
    memcpy( encoded, name->pbData, name->cbData );
    signature->signer_subject = subject;
    signature->signer_subject_name.cbData = name->cbData;
    signature->signer_subject_name.pbData = encoded;
    return S_OK;
}

static HRESULT copy_signer_certificate_id( PCCERT_CONTEXT certificate,
                                           struct appx_signature *signature )
{
    DWORD size = sizeof(signature->signer_certificate_id);

    if (!certificate->pbCertEncoded || !certificate->cbCertEncoded ||
        !CryptHashCertificate2( sha256_algorithm, 0, NULL,
                                certificate->pbCertEncoded,
                                certificate->cbCertEncoded,
                                signature->signer_certificate_id, &size ) ||
        size != sizeof(signature->signer_certificate_id))
        return crypto_error( TRUST_E_SUBJECT_NOT_TRUSTED );
    return S_OK;
}

HRESULT WINAPI appx_signature_decode_digest_set(
    const BYTE *data, SIZE_T size, struct appx_signature_digest_set *set )
{
    static const DWORD tags[] =
    {
        0x43505841, /* AXPC */
        0x44435841, /* AXCD */
        0x54435841, /* AXCT */
        0x4d425841, /* AXBM */
        0x49435841  /* AXCI */
    };
    BYTE *destinations[ARRAY_SIZE( tags )];
    UINT32 count, i;

    if (!data || !set) return E_INVALIDARG;
    memset( set, 0, sizeof(*set) );
    if (size != APPX_DIGEST_BASE_SIZE && size != APPX_DIGEST_CODE_INTEGRITY_SIZE)
        return APPX_E_INVALID_SIP_CLIENT_DATA;
    if (read_u32( data ) != APPX_DIGEST_MAGIC)
        return APPX_E_INVALID_SIP_CLIENT_DATA;

    destinations[0] = set->package_contents;
    destinations[1] = set->central_directory;
    destinations[2] = set->content_types;
    destinations[3] = set->block_map;
    destinations[4] = set->code_integrity;
    count = size == APPX_DIGEST_CODE_INTEGRITY_SIZE ? 5 : 4;

    for (i = 0, data += 4; i < count; i++, data += APPX_DIGEST_RECORD_SIZE)
    {
        if (read_u32( data ) != tags[i])
        {
            memset( set, 0, sizeof(*set) );
            return APPX_E_INVALID_SIP_CLIENT_DATA;
        }
        memcpy( destinations[i], data + 4, APPX_SIGNATURE_SHA256_SIZE );
        set->flags |= 1u << i;
    }
    return S_OK;
}

HRESULT WINAPI appx_signature_parse_and_verify( const BYTE *data, SIZE_T size,
                                                UINT32 flags,
                                                APPX_SIGNATURE **result )
{
    struct appx_signature *signature = NULL;
    PCCERT_CONTEXT signer_certificate = NULL;
    CMSG_SIGNER_INFO *signer = NULL;
    HCERTSTORE certificate_store = NULL;
    HCRYPTMSG message = NULL;
    char *content_oid = NULL;
    BYTE *content = NULL;
    const BYTE *embedded_content, *authenticated_content;
    SIZE_T embedded_content_size, authenticated_content_size;
    CMSG_CTRL_VERIFY_SIGNATURE_EX_PARA verify_parameters;
    FILETIME timestamp;
    DWORD content_size, parameter_size, signer_count, message_type;
    BOOL timestamped = FALSE;
    HRESULT hr;

    if (!result) return E_POINTER;
    *result = NULL;
    if (!data || size < 4 || size > APPX_SIGNATURE_MAX_SIZE ||
        flags & ~(APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN |
                  APPX_SIGNATURE_VERIFY_BUNDLE))
        return E_INVALIDARG;
    if (read_u32( data ) != APPX_SIGNATURE_MAGIC)
        return malformed_signature();
    data += 4;
    size -= 4;
    if (!size || size > MAXDWORD) return malformed_signature();
    if (FAILED( hr = validate_cms_shape( data, size, &embedded_content,
                                       &embedded_content_size,
                                       &authenticated_content,
                                       &authenticated_content_size ) ))
        return hr;

    if (!(message = CryptMsgOpenToDecode(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, 0, 0, NULL, NULL )))
        return crypto_error( malformed_signature() );
    if (!CryptMsgUpdate( message, data, (DWORD)size, TRUE ))
    {
        hr = crypto_error( malformed_signature() );
        goto done;
    }

    parameter_size = sizeof(message_type);
    if (!CryptMsgGetParam( message, CMSG_TYPE_PARAM, 0, &message_type,
                           &parameter_size ) ||
        parameter_size != sizeof(message_type) || message_type != CMSG_SIGNED)
    {
        hr = malformed_signature();
        goto done;
    }
    parameter_size = sizeof(signer_count);
    if (!CryptMsgGetParam( message, CMSG_SIGNER_COUNT_PARAM, 0, &signer_count,
                           &parameter_size ) ||
        parameter_size != sizeof(signer_count) || signer_count != 1)
    {
        hr = malformed_signature();
        goto done;
    }

    if (FAILED( hr = get_message_param( message, CMSG_INNER_CONTENT_TYPE_PARAM, 0,
                                      MAX_ATTRIBUTE_OID_CHARS,
                                      (void **)&content_oid, &parameter_size ) ))
        goto done;
    if (parameter_size != sizeof(SPC_INDIRECT_DATA_OBJID) ||
        memcmp( content_oid, SPC_INDIRECT_DATA_OBJID,
                sizeof(SPC_INDIRECT_DATA_OBJID) ))
    {
        hr = malformed_signature();
        goto done;
    }
    if (FAILED( hr = get_message_param( message, CMSG_CONTENT_PARAM, 0,
                                      MAX_CMS_CONTENT_SIZE, (void **)&content,
                                      &parameter_size ) ))
        goto done;
    content_size = parameter_size;
    if (content_size != embedded_content_size ||
        memcmp( content, embedded_content, content_size ) ||
        !der_validate_document( content, content_size ))
    {
        hr = APPX_E_INVALID_SIP_CLIENT_DATA;
        goto done;
    }
    if (!(signature = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                 sizeof(*signature) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (FAILED( hr = appx_signature_decode_indirect_data_ex(
            content, content_size, flags & APPX_SIGNATURE_VERIFY_BUNDLE,
            &signature->digests ) ))
        goto done;

    if (FAILED( hr = get_message_param( message, CMSG_SIGNER_INFO_PARAM, 0,
                                      MAX_SIGNER_INFO_SIZE, (void **)&signer,
                                      &parameter_size ) ))
        goto done;
    if (parameter_size < sizeof(*signer) ||
        signer->dwVersion != CMSG_SIGNER_INFO_V1 ||
        !crypt_algorithm_is_sha256( &signer->HashAlgorithm, signer,
                                    parameter_size ) ||
        !crypt_algorithm_is_rsa( &signer->HashEncryptionAlgorithm, signer,
                                 parameter_size ))
    {
        hr = malformed_signature();
        goto done;
    }
    if (FAILED( hr = validate_authenticated_attributes(
            signer, parameter_size, oid_spc_indirect_data,
            sizeof(oid_spc_indirect_data), authenticated_content,
            authenticated_content_size, FALSE, NULL ) ))
    {
        TRACE( "package CMS authenticated attributes failed, hr %#lx.\n", hr );
        goto done;
    }

    if (FAILED( hr = load_certificate_store( message, &certificate_store ) ) ||
        FAILED( hr = get_signer_certificate( message, certificate_store,
                                             &signer_certificate ) ))
        goto done;
    if (!signer_certificate->pCertInfo ||
        FAILED( hr = appx_signature_validate_leaf_extensions(
            signer_certificate->pCertInfo->rgExtension,
            signer_certificate->pCertInfo->cExtension, FALSE ) ))
    {
        if (!signer_certificate->pCertInfo) hr = malformed_signature();
        TRACE( "package leaf certificate extensions failed, hr %#lx.\n", hr );
        goto done;
    }
    memset( &verify_parameters, 0, sizeof(verify_parameters) );
    verify_parameters.cbSize = sizeof(verify_parameters);
    verify_parameters.dwSignerIndex = 0;
    verify_parameters.dwSignerType = CMSG_VERIFY_SIGNER_CERT;
    verify_parameters.pvSigner = (void *)signer_certificate;
    if (!CryptMsgControl( message, 0, CMSG_CTRL_VERIFY_SIGNATURE_EX,
                          &verify_parameters ))
    {
        hr = crypto_error( TRUST_E_CERT_SIGNATURE );
        TRACE( "package CMS signer verification failed, hr %#lx.\n", hr );
        goto done;
    }
    hr = verify_rfc3161_timestamp( signer, flags, &timestamp );
    if (hr == S_OK) timestamped = TRUE;
    else if (hr != S_FALSE)
    {
        TRACE( "RFC3161 verification failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED( hr = verify_certificate_chain(
            signer_certificate, certificate_store, flags,
            szOID_PKIX_KP_CODE_SIGNING,
            timestamped ? &timestamp : NULL,
            timestamped ? CERT_CHAIN_TIMESTAMP_TIME : 0 ) ) ||
        FAILED( hr = copy_signer_certificate_id( signer_certificate,
                                                 signature ) ) ||
        FAILED( hr = copy_signer_subject( signer_certificate, signature ) ))
        goto done;

    *result = signature;
    signature = NULL;
    hr = S_OK;

done:
    appx_signature_free( signature );
    if (signer_certificate) CertFreeCertificateContext( signer_certificate );
    if (certificate_store) CertCloseStore( certificate_store, 0 );
    HeapFree( GetProcessHeap(), 0, signer );
    HeapFree( GetProcessHeap(), 0, content );
    HeapFree( GetProcessHeap(), 0, content_oid );
    if (message) CryptMsgClose( message );
    return hr;
}

void WINAPI appx_signature_free( APPX_SIGNATURE *signature )
{
    if (!signature) return;
    HeapFree( GetProcessHeap(), 0, signature->signer_subject );
    HeapFree( GetProcessHeap(), 0, signature->signer_subject_name.pbData );
    memset( signature, 0, sizeof(*signature) );
    HeapFree( GetProcessHeap(), 0, signature );
}

const struct appx_signature_digest_set * WINAPI appx_signature_get_digest_set(
    const APPX_SIGNATURE *signature )
{
    return signature ? &signature->digests : NULL;
}

const WCHAR * WINAPI appx_signature_get_signer_subject(
    const APPX_SIGNATURE *signature )
{
    return signature ? signature->signer_subject : NULL;
}

HRESULT WINAPI appx_signature_get_signer_certificate_id(
    const APPX_SIGNATURE *signature, BYTE *certificate_id, UINT32 size )
{
    if (!signature || !certificate_id ||
        size != APPX_SIGNATURE_CERTIFICATE_ID_SIZE)
        return E_INVALIDARG;
    memcpy( certificate_id, signature->signer_certificate_id, size );
    return S_OK;
}

static HRESULT cert_str_to_name_supports_reverse( BOOL *supported )
{
    static const WCHAR probe[] = L"CN=A,O=B";
    BYTE forward[64], reverse[64];
    DWORD forward_size = sizeof(forward), reverse_size = sizeof(reverse);

    *supported = FALSE;
    if (!CertStrToNameW( X509_ASN_ENCODING, probe, CERT_X500_NAME_STR,
                         NULL, forward, &forward_size, NULL ) ||
        !CertStrToNameW( X509_ASN_ENCODING, probe,
                         CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                         NULL, reverse, &reverse_size, NULL ))
        return TRUST_E_SUBJECT_FORM_UNKNOWN;
    *supported = forward_size != reverse_size ||
                 memcmp( forward, reverse, forward_size );
    return S_OK;
}

static BOOL publisher_has_multivalued_rdn( const WCHAR *publisher )
{
    BOOL escaped = FALSE, quoted = FALSE;

    while (*publisher)
    {
        if (escaped)
            escaped = FALSE;
        else if (*publisher == '\\')
            escaped = TRUE;
        else if (*publisher == '"')
            quoted = !quoted;
        else if (*publisher == '+' && !quoted)
            return TRUE;
        publisher++;
    }
    return FALSE;
}

static HRESULT reverse_name_blob( CERT_NAME_BLOB *name )
{
    CERT_NAME_INFO reversed, *decoded = NULL;
    CERT_RDN *rdns = NULL;
    BYTE *encoded = NULL;
    DWORD decoded_size = 0, encoded_size = 0, i;
    HRESULT hr = TRUST_E_SUBJECT_FORM_UNKNOWN;

    if (!CryptDecodeObjectEx( X509_ASN_ENCODING, X509_NAME,
                              name->pbData, name->cbData,
                              CRYPT_DECODE_ALLOC_FLAG, NULL,
                              &decoded, &decoded_size ) ||
        !decoded ||
        !decoded->cRDN ||
        !decoded->rgRDN ||
        decoded->cRDN > MAX_SIGNER_SUBJECT_DER_SIZE / sizeof(*rdns))
        goto done;
    if (!(rdns = HeapAlloc( GetProcessHeap(), 0,
                            decoded->cRDN * sizeof(*rdns) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < decoded->cRDN; i++)
        rdns[i] = decoded->rgRDN[decoded->cRDN - i - 1];
    reversed.cRDN = decoded->cRDN;
    reversed.rgRDN = rdns;
    if (!CryptEncodeObjectEx( X509_ASN_ENCODING, X509_NAME, &reversed,
                              0, NULL, NULL, &encoded_size ) ||
        !encoded_size || encoded_size > MAX_SIGNER_SUBJECT_DER_SIZE)
        goto done;
    if (!(encoded = HeapAlloc( GetProcessHeap(), 0, encoded_size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!CryptEncodeObjectEx( X509_ASN_ENCODING, X509_NAME, &reversed,
                              0, NULL, encoded, &encoded_size ))
        goto done;
    HeapFree( GetProcessHeap(), 0, name->pbData );
    name->pbData = encoded;
    name->cbData = encoded_size;
    encoded = NULL;
    hr = S_OK;

done:
    HeapFree( GetProcessHeap(), 0, encoded );
    HeapFree( GetProcessHeap(), 0, rdns );
    LocalFree( decoded );
    return hr;
}

HRESULT WINAPI appx_signature_check_publisher(
    const APPX_SIGNATURE *signature, const WCHAR *publisher )
{
    CERT_NAME_BLOB name = {0};
    CERT_NAME_BLOB signer_name;
    BOOL reverse_supported;
    DWORD size = 0, allocated_size;
    HRESULT hr;

    if (!signature || !publisher) return E_INVALIDARG;
    if (!signature->signer_subject_name.pbData ||
        !signature->signer_subject_name.cbData ||
        signature->signer_subject_name.cbData > MAX_SIGNER_SUBJECT_DER_SIZE)
        return TRUST_E_SUBJECT_NOT_TRUSTED;
    signer_name = signature->signer_subject_name;
    /*
     * Wine's CertStrToNameW currently splits a multi-valued RDN into separate
     * RDNs.  Reject that syntax rather than comparing it with a different
     * certificate name.  Escaped or quoted plus signs remain ordinary value
     * characters.
     */
    if (!*publisher || publisher_has_multivalued_rdn( publisher ) ||
        !CertStrToNameW( X509_ASN_ENCODING, publisher,
                         CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                         NULL, NULL, &size, NULL ) ||
        !size || size > MAX_SIGNER_SUBJECT_DER_SIZE)
        return TRUST_E_SUBJECT_FORM_UNKNOWN;
    allocated_size = size;
    if (!(name.pbData = HeapAlloc( GetProcessHeap(), 0, allocated_size )))
        return E_OUTOFMEMORY;
    name.cbData = allocated_size;
    if (!CertStrToNameW( X509_ASN_ENCODING, publisher,
                         CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                         NULL, name.pbData, &size, NULL ) ||
        !size || size != allocated_size)
        hr = TRUST_E_SUBJECT_FORM_UNKNOWN;
    else
    {
        name.cbData = size;
        /*
         * Wine's current CertStrToNameW implementation accepts the reverse
         * flag but does not apply it.  Detect that behavior so this code
         * retains the native API contract when crypt32 gains the missing
         * support.
         */
        if (FAILED( hr = cert_str_to_name_supports_reverse( &reverse_supported ) ))
            goto done;
        if (!reverse_supported &&
            FAILED( hr = reverse_name_blob( &name ) ))
            goto done;
        hr = CertCompareCertificateName( X509_ASN_ENCODING,
                                         &signer_name,
                                         &name ) ?
             S_OK : TRUST_E_SUBJECT_NOT_TRUSTED;
    }

done:
    HeapFree( GetProcessHeap(), 0, name.pbData );
    return hr;
}

HRESULT WINAPI appx_signature_verify_digest_set(
    const APPX_SIGNATURE *signature,
    const struct appx_signature_digest_set *recalculated )
{
    if (!signature) return E_INVALIDARG;
    return appx_signature_compare_digest_sets( &signature->digests, recalculated );
}

HRESULT WINAPI appx_signature_compare_digest_sets(
    const struct appx_signature_digest_set *signed_set,
    const struct appx_signature_digest_set *recalculated )
{
    const BYTE *expected, *actual;
    volatile UINT32 difference;
    SIZE_T i;

    if (!signed_set || !recalculated) return E_INVALIDARG;
    expected = (const BYTE *)signed_set;
    actual = (const BYTE *)recalculated;
    difference = signed_set->flags ^ recalculated->flags;
    difference |= recalculated->flags & ~APPX_SIGNATURE_DIGEST_ALL;
    difference |= signed_set->flags & ~APPX_SIGNATURE_DIGEST_ALL;
    for (i = sizeof(signed_set->flags); i < sizeof(*signed_set); i++)
        difference |= expected[i] ^ actual[i];
    return difference ? APPX_E_DIGEST_MISMATCH : S_OK;
}
