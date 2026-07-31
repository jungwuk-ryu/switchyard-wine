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

#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "winerror.h"
#include "wintrust.h"

#include "signature.h"

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

static const BYTE oid_pkcs7_signed_data[] =
    { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
static const BYTE oid_spc_indirect_data[] =
    { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04 };
static const BYTE oid_spc_siginfo[] =
    { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x1e };
static const BYTE oid_sha256[] =
    { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const BYTE appx_sip_guid[] =
    { 0x4b, 0xdf, 0xc5, 0x0a, 0x07, 0xce, 0xe2, 0x4d,
      0xb7, 0x6e, 0x23, 0xc8, 0x39, 0xa0, 0x9f, 0xd1 };
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

static HRESULT parse_sip_info( const struct der_element *wrapper )
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
        memcmp( value.value, appx_sip_guid, sizeof(appx_sip_guid) ))
        return APPX_E_INVALID_SIP_CLIENT_DATA;
    for (i = 0; i < 5; i++)
    {
        if (!der_read_expected( &cursor, 0x02, &value ) ||
            !der_integer_is_u32( &value, 0 ))
            return APPX_E_INVALID_SIP_CLIENT_DATA;
    }
    return cursor.size ? APPX_E_INVALID_SIP_CLIENT_DATA : S_OK;
}

HRESULT WINAPI appx_signature_decode_indirect_data(
    const BYTE *data, SIZE_T size, struct appx_signature_digest_set *digests )
{
    struct der_budget budget = {0};
    struct der_cursor document = {data, size, &budget}, outer, attribute, digest_info;
    struct der_element sequence, data_part, digest_part, oid, value, algorithm, digest;
    HRESULT hr;

    if (!data || !digests) return E_INVALIDARG;
    memset( digests, 0, sizeof(*digests) );
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
    if (FAILED( hr = parse_sip_info( &value ) )) return hr;

    digest_info.data = digest_part.value;
    digest_info.size = digest_part.value_size;
    digest_info.budget = &budget;
    if (!der_read_expected( &digest_info, 0x30, &algorithm ) ||
        !algorithm_identifier_is_sha256( &algorithm, TRUE ) ||
        !der_read_expected( &digest_info, 0x04, &digest ) || digest_info.size)
        return APPX_E_INVALID_SIP_CLIENT_DATA;

    return appx_signature_decode_digest_set( digest.value, digest.value_size, digests );
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

static HRESULT get_message_param( HCRYPTMSG message, DWORD parameter, DWORD index,
                                  SIZE_T maximum, void **result, DWORD *result_size )
{
    DWORD size = 0;
    void *buffer;

    *result = NULL;
    *result_size = 0;
    if (!CryptMsgGetParam( message, parameter, index, NULL, &size ))
        return crypto_error( malformed_signature() );
    if (!size || size > maximum) return malformed_signature();
    if (!(buffer = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size )))
        return E_OUTOFMEMORY;
    if (!CryptMsgGetParam( message, parameter, index, buffer, &size ))
    {
        HRESULT hr = crypto_error( malformed_signature() );
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

static HRESULT validate_authenticated_attributes( const CMSG_SIGNER_INFO *signer,
                                                  DWORD signer_size,
                                                  const BYTE *content,
                                                  DWORD content_size )
{
    BYTE signed_digest[APPX_SIGNATURE_SHA256_SIZE], computed_digest[APPX_SIGNATURE_SHA256_SIZE];
    DWORD computed_size = sizeof(computed_digest);
    UINT32 content_type_count = 0, message_digest_count = 0;
    DWORD i;

    if (!signer->AuthAttrs.cAttr ||
        signer->AuthAttrs.cAttr > MAX_SIGNER_ATTRIBUTES ||
        signer->UnauthAttrs.cAttr > MAX_SIGNER_ATTRIBUTES ||
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
                                             oid_spc_indirect_data,
                                             sizeof(oid_spc_indirect_data) ))
                return malformed_signature();
        }
        else if (!strcmp( attribute->pszObjId, szOID_RSA_messageDigest ))
        {
            if (++message_digest_count != 1 || attribute->cValue != 1 ||
                !decode_digest_attribute_value( attribute->rgValue, signed_digest ))
                return malformed_signature();
        }
    }
    if (content_type_count != 1 || message_digest_count != 1)
        return malformed_signature();

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
        if (!strcmp( attribute->pszObjId, szOID_NESTED_SIGNATURE ))
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
    if (errors & CERT_TRUST_IS_NOT_SIGNATURE_VALID)
        return TRUST_E_CERT_SIGNATURE;
    if (errors & CERT_TRUST_IS_NOT_TIME_VALID)
        return CERT_E_EXPIRED;
    if (errors & CERT_TRUST_IS_NOT_TIME_NESTED)
        return CERT_E_VALIDITYPERIODNESTING;
    if (errors & CERT_TRUST_IS_NOT_VALID_FOR_USAGE)
        return CERT_E_WRONG_USAGE;
    if (errors & CERT_TRUST_IS_PARTIAL_CHAIN)
        return CERT_E_CHAINING;
    if (errors & CERT_TRUST_IS_UNTRUSTED_ROOT)
        return CERT_E_UNTRUSTEDROOT;
    return TRUST_E_SUBJECT_NOT_TRUSTED;
}

static HRESULT verify_certificate_chain( PCCERT_CONTEXT certificate,
                                         HCERTSTORE additional_store,
                                         UINT32 flags )
{
    LPSTR code_signing_usage = (LPSTR)szOID_PKIX_KP_CODE_SIGNING;
    CERT_CHAIN_POLICY_STATUS status = {sizeof(status), 0};
    CERT_CHAIN_POLICY_PARA policy = {sizeof(policy), 0};
    CERT_CHAIN_PARA parameters = {sizeof(parameters)};
    PCCERT_CHAIN_CONTEXT chain = NULL;
    DWORD trust_errors;
    HRESULT hr = S_OK;

    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &code_signing_usage;

    if (!CertGetCertificateChain( NULL, certificate, NULL, additional_store,
                                  &parameters, CERT_CHAIN_CACHE_END_CERT |
                                  CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL,
                                  NULL, &chain ))
        return crypto_error( TRUST_E_SUBJECT_NOT_TRUSTED );
    trust_errors = chain->TrustStatus.dwErrorStatus;
    if (!CertVerifyCertificateChainPolicy( CERT_CHAIN_POLICY_AUTHENTICODE,
                                           chain, &policy, &status ))
        hr = crypto_error( TRUST_E_SUBJECT_NOT_TRUSTED );
    else if (status.dwError &&
             (!(flags & APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN) ||
              !is_relaxable_chain_error( status.dwError )))
        hr = status.dwError;
    /*
     * Wine and older chain engines may return a successful Authenticode
     * policy status while the chain context still reports a partial or
     * untrusted path.  Never let that policy differential turn into trust.
     */
    if (SUCCEEDED( hr ) && trust_errors &&
        (!(flags & APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN) ||
         !trust_status_is_relaxable( trust_errors )))
        hr = trust_status_to_hresult( trust_errors );
    CertFreeCertificateChain( chain );
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
    DWORD content_size, parameter_size, signer_count, message_type;
    HRESULT hr;

    if (!result) return E_POINTER;
    *result = NULL;
    if (!data || size < 4 || size > APPX_SIGNATURE_MAX_SIZE ||
        flags & ~APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN)
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
    if (FAILED( hr = appx_signature_decode_indirect_data( content, content_size,
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
    if (FAILED( hr = validate_authenticated_attributes( signer, parameter_size,
                                                      authenticated_content,
                                                      authenticated_content_size ) ))
        goto done;

    if (FAILED( hr = load_certificate_store( message, &certificate_store ) ) ||
        FAILED( hr = get_signer_certificate( message, certificate_store,
                                             &signer_certificate ) ))
        goto done;
    memset( &verify_parameters, 0, sizeof(verify_parameters) );
    verify_parameters.cbSize = sizeof(verify_parameters);
    verify_parameters.dwSignerIndex = 0;
    verify_parameters.dwSignerType = CMSG_VERIFY_SIGNER_CERT;
    verify_parameters.pvSigner = (void *)signer_certificate;
    if (!CryptMsgControl( message, 0, CMSG_CTRL_VERIFY_SIGNATURE_EX,
                          &verify_parameters ))
    {
        hr = crypto_error( TRUST_E_CERT_SIGNATURE );
        goto done;
    }
    if (FAILED( hr = verify_certificate_chain( signer_certificate,
                                             certificate_store, flags ) ) ||
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
