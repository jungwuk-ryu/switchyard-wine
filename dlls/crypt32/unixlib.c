/*
 * Copyright 2019 Hans Leidekker for CodeWeavers
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <Security/Security.h>
#endif
#ifdef SONAME_LIBGNUTLS
#include <gnutls/pkcs12.h>
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wincrypt.h"
#include "bcrypt.h"
#include "crypt32_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

#ifdef SONAME_LIBGNUTLS

WINE_DECLARE_DEBUG_CHANNEL(winediag);

int gnutls_x509_privkey_get_pk_algorithm2(gnutls_x509_privkey_t, unsigned int*);

static void *libgnutls_handle;
struct cert_store_data;

#define CERT_STORE_TOKEN_TAG  UINT64_C(0xc320000000000000)
#define CERT_STORE_TOKEN_MASK UINT64_C(0xffff000000000000)
#define CERT_STORE_TOKEN_MAX  UINT64_C(0x0000ffffffffffff)

struct cert_store_entry
{
    struct list entry;
    cert_store_data_t token;
    struct cert_store_data *data;
    unsigned int active;
    BOOL closing;
};

static pthread_mutex_t store_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t store_cond = PTHREAD_COND_INITIALIZER;
static struct list store_entries = LIST_INIT(store_entries);
static UINT64 next_store_token;
static unsigned int store_calls;
static BOOL store_attached, store_draining;

static void destroy_cert_store( struct cert_store_data *data );

static NTSTATUS store_provider_enter(void)
{
    NTSTATUS status = STATUS_SUCCESS;

    pthread_mutex_lock( &store_mutex );
    if (!store_attached || store_draining) status = STATUS_DLL_NOT_FOUND;
    else store_calls++;
    pthread_mutex_unlock( &store_mutex );
    return status;
}

static void store_provider_leave(void)
{
    pthread_mutex_lock( &store_mutex );
    assert( store_calls );
    if (!--store_calls) pthread_cond_broadcast( &store_cond );
    pthread_mutex_unlock( &store_mutex );
}

static struct cert_store_entry *find_store_entry( cert_store_data_t token )
{
    struct cert_store_entry *entry;

    LIST_FOR_EACH_ENTRY( entry, &store_entries, struct cert_store_entry, entry )
        if (entry->token == token) return entry;
    return NULL;
}

static NTSTATUS publish_store( struct cert_store_data *data, cert_store_data_t *token )
{
    struct cert_store_entry *entry;
    UINT64 generation;

    if (!(entry = calloc( 1, sizeof(*entry) ))) return STATUS_NO_MEMORY;
    pthread_mutex_lock( &store_mutex );
    if (!store_attached || store_draining)
    {
        pthread_mutex_unlock( &store_mutex );
        free( entry );
        return STATUS_DLL_NOT_FOUND;
    }
    if (!(generation = ++next_store_token) || generation > CERT_STORE_TOKEN_MAX)
    {
        pthread_mutex_unlock( &store_mutex );
        free( entry );
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    entry->token = CERT_STORE_TOKEN_TAG | generation;
    entry->data = data;
    list_add_tail( &store_entries, &entry->entry );
    *token = entry->token;
    pthread_mutex_unlock( &store_mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS acquire_store( cert_store_data_t token, struct cert_store_entry **ret )
{
    struct cert_store_entry *entry;
    NTSTATUS status = STATUS_INVALID_HANDLE;

    *ret = NULL;
    if ((token & CERT_STORE_TOKEN_MASK) != CERT_STORE_TOKEN_TAG) return status;
    pthread_mutex_lock( &store_mutex );
    if (store_attached && !store_draining && (entry = find_store_entry( token )) && !entry->closing)
    {
        entry->active++;
        *ret = entry;
        status = STATUS_SUCCESS;
    }
    pthread_mutex_unlock( &store_mutex );
    return status;
}

static void release_store( struct cert_store_entry *entry )
{
    pthread_mutex_lock( &store_mutex );
    assert( entry->active );
    if (!--entry->active) pthread_cond_broadcast( &store_cond );
    pthread_mutex_unlock( &store_mutex );
}

static NTSTATUS close_store( cert_store_data_t token )
{
    struct cert_store_entry *entry;

    if (!token) return STATUS_SUCCESS;
    if ((token & CERT_STORE_TOKEN_MASK) != CERT_STORE_TOKEN_TAG) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &store_mutex );
    if (!(entry = find_store_entry( token )) || entry->closing)
    {
        pthread_mutex_unlock( &store_mutex );
        return STATUS_INVALID_HANDLE;
    }
    entry->closing = TRUE;
    list_remove( &entry->entry );
    while (entry->active) pthread_cond_wait( &store_cond, &store_mutex );
    pthread_mutex_unlock( &store_mutex );
    destroy_cert_store( entry->data );
    free( entry );
    return STATUS_SUCCESS;
}
#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gnutls_global_deinit);
MAKE_FUNCPTR(gnutls_global_init);
MAKE_FUNCPTR(gnutls_global_set_log_function);
MAKE_FUNCPTR(gnutls_global_set_log_level);
MAKE_FUNCPTR(gnutls_perror);
MAKE_FUNCPTR(gnutls_pkcs12_bag_decrypt);
MAKE_FUNCPTR(gnutls_pkcs12_bag_deinit);
MAKE_FUNCPTR(gnutls_pkcs12_bag_get_count);
MAKE_FUNCPTR(gnutls_pkcs12_bag_get_data);
MAKE_FUNCPTR(gnutls_pkcs12_bag_get_type);
MAKE_FUNCPTR(gnutls_pkcs12_bag_init);
MAKE_FUNCPTR(gnutls_pkcs12_bag_set_crt);
MAKE_FUNCPTR(gnutls_pkcs12_bag_set_privkey);
MAKE_FUNCPTR(gnutls_pkcs12_deinit);
MAKE_FUNCPTR(gnutls_pkcs12_export2);
MAKE_FUNCPTR(gnutls_pkcs12_generate_mac2);
MAKE_FUNCPTR(gnutls_pkcs12_get_bag);
MAKE_FUNCPTR(gnutls_pkcs12_import);
MAKE_FUNCPTR(gnutls_pkcs12_init);
MAKE_FUNCPTR(gnutls_pkcs12_set_bag);
MAKE_FUNCPTR(gnutls_pkcs12_verify_mac);
MAKE_FUNCPTR(gnutls_x509_crt_deinit);
MAKE_FUNCPTR(gnutls_x509_crt_export);
MAKE_FUNCPTR(gnutls_x509_crt_import);
MAKE_FUNCPTR(gnutls_x509_crt_init);
MAKE_FUNCPTR(gnutls_x509_privkey_deinit);
MAKE_FUNCPTR(gnutls_x509_privkey_export_rsa_raw2);
MAKE_FUNCPTR(gnutls_x509_privkey_get_pk_algorithm2);
MAKE_FUNCPTR(gnutls_x509_privkey_import_pkcs8);
MAKE_FUNCPTR(gnutls_x509_privkey_import_rsa_raw);
MAKE_FUNCPTR(gnutls_x509_privkey_init);
#undef MAKE_FUNCPTR

static void gnutls_log( int level, const char *msg )
{
    TRACE( "<%d> %s", level, msg );
}

static NTSTATUS process_attach( void *args )
{
    const char *env_str;
    int ret;

    pthread_mutex_lock( &store_mutex );
    if (store_attached)
    {
        pthread_mutex_unlock( &store_mutex );
        return TRUE;
    }
    if (store_draining)
    {
        pthread_mutex_unlock( &store_mutex );
        return STATUS_DEVICE_BUSY;
    }
    store_draining = TRUE;
    pthread_mutex_unlock( &store_mutex );

    if ((env_str = getenv("GNUTLS_SYSTEM_PRIORITY_FILE")))
    {
        WARN("GNUTLS_SYSTEM_PRIORITY_FILE is %s.\n", debugstr_a(env_str));
    }
    else
    {
        WARN("Setting GNUTLS_SYSTEM_PRIORITY_FILE to \"/dev/null\".\n");
        setenv("GNUTLS_SYSTEM_PRIORITY_FILE", "/dev/null", 0);
    }

    if (!(libgnutls_handle = dlopen( SONAME_LIBGNUTLS, RTLD_NOW )))
    {
        ERR_(winediag)( "failed to load libgnutls, no support for pfx import/export\n" );
        pthread_mutex_lock( &store_mutex );
        store_draining = FALSE;
        pthread_cond_broadcast( &store_cond );
        pthread_mutex_unlock( &store_mutex );
        return STATUS_DLL_NOT_FOUND;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = dlsym( libgnutls_handle, #f ))) \
    { \
        ERR( "failed to load %s\n", #f ); \
        goto fail; \
    }

    LOAD_FUNCPTR(gnutls_global_deinit)
    LOAD_FUNCPTR(gnutls_global_init)
    LOAD_FUNCPTR(gnutls_global_set_log_function)
    LOAD_FUNCPTR(gnutls_global_set_log_level)
    LOAD_FUNCPTR(gnutls_perror)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_decrypt)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_deinit)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_get_count)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_get_data)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_get_type)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_init)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_set_crt)
    LOAD_FUNCPTR(gnutls_pkcs12_bag_set_privkey)
    LOAD_FUNCPTR(gnutls_pkcs12_deinit)
    LOAD_FUNCPTR(gnutls_pkcs12_export2)
    LOAD_FUNCPTR(gnutls_pkcs12_generate_mac2)
    LOAD_FUNCPTR(gnutls_pkcs12_get_bag)
    LOAD_FUNCPTR(gnutls_pkcs12_import)
    LOAD_FUNCPTR(gnutls_pkcs12_init)
    LOAD_FUNCPTR(gnutls_pkcs12_set_bag)
    LOAD_FUNCPTR(gnutls_pkcs12_verify_mac)
    LOAD_FUNCPTR(gnutls_x509_crt_deinit)
    LOAD_FUNCPTR(gnutls_x509_crt_export)
    LOAD_FUNCPTR(gnutls_x509_crt_import)
    LOAD_FUNCPTR(gnutls_x509_crt_init)
    LOAD_FUNCPTR(gnutls_x509_privkey_deinit)
    LOAD_FUNCPTR(gnutls_x509_privkey_export_rsa_raw2)
    LOAD_FUNCPTR(gnutls_x509_privkey_get_pk_algorithm2)
    LOAD_FUNCPTR(gnutls_x509_privkey_import_pkcs8)
    LOAD_FUNCPTR(gnutls_x509_privkey_import_rsa_raw)
    LOAD_FUNCPTR(gnutls_x509_privkey_init)
#undef LOAD_FUNCPTR

    if ((ret = pgnutls_global_init()) != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror( ret );
        goto fail;
    }

    if (TRACE_ON( crypt ))
    {
        char *env = getenv("GNUTLS_DEBUG_LEVEL");
        int level = env ? atoi(env) : 4;
        pgnutls_global_set_log_level(level);
        pgnutls_global_set_log_function( gnutls_log );
    }

    pthread_mutex_lock( &store_mutex );
    store_attached = TRUE;
    store_draining = FALSE;
    pthread_cond_broadcast( &store_cond );
    pthread_mutex_unlock( &store_mutex );
    return TRUE;

fail:
    dlclose( libgnutls_handle );
    libgnutls_handle = NULL;
    pthread_mutex_lock( &store_mutex );
    store_draining = FALSE;
    pthread_cond_broadcast( &store_cond );
    pthread_mutex_unlock( &store_mutex );
    return STATUS_DLL_INIT_FAILED;
}

static NTSTATUS process_detach( void *args )
{
    struct list detached = LIST_INIT(detached);
    struct cert_store_entry *entry;

    pthread_mutex_lock( &store_mutex );
    if (!store_attached)
    {
        pthread_mutex_unlock( &store_mutex );
        return STATUS_SUCCESS;
    }
    store_draining = TRUE;
    while (store_calls) pthread_cond_wait( &store_cond, &store_mutex );
    while (!list_empty( &store_entries ))
    {
        struct list *head = list_head( &store_entries );
        list_remove( head );
        list_add_tail( &detached, head );
    }
    pthread_mutex_unlock( &store_mutex );

    while (!list_empty( &detached ))
    {
        entry = LIST_ENTRY( list_head( &detached ), struct cert_store_entry, entry );
        list_remove( &entry->entry );
        destroy_cert_store( entry->data );
        free( entry );
    }
    if (libgnutls_handle)
    {
        if (TRACE_ON( crypt ))
            pgnutls_global_set_log_function( NULL );
        pgnutls_global_deinit();
        dlclose( libgnutls_handle );
        libgnutls_handle = NULL;
    }
    pthread_mutex_lock( &store_mutex );
    store_attached = FALSE;
    store_draining = FALSE;
    pthread_cond_broadcast( &store_cond );
    pthread_mutex_unlock( &store_mutex );
    return STATUS_SUCCESS;
}
#define RSA_MAGIC_KEY  ('R' | ('S' << 8) | ('A' << 16) | ('2' << 24))
#define RSA_PUBEXP     65537

struct cert_store_data
{
    gnutls_pkcs12_t p12;
    gnutls_x509_privkey_t key;
    gnutls_x509_crt_t *chain;
    unsigned int key_bitlen;
    unsigned int chain_len;
};

static void destroy_cert_store( struct cert_store_data *data )
{
    unsigned int i;

    if (!data) return;
    for (i = 0; i < data->chain_len; i++) pgnutls_x509_crt_deinit( data->chain[i] );
    free( data->chain );
    if (data->key) pgnutls_x509_privkey_deinit( data->key );
    if (data->p12) pgnutls_pkcs12_deinit( data->p12 );
    free( data );
}

static NTSTATUS import_store_key_data( struct cert_store_data *data, void *buf, DWORD *buf_size )
{
    int i, ret;
    unsigned int bitlen;
    gnutls_datum_t m, e, d, p, q, u, e1, e2;
    BLOBHEADER *hdr;
    RSAPUBKEY *rsakey;
    BYTE *src, *dst;
    DWORD size;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    if (!data->key) return STATUS_NOT_FOUND;
    bitlen = data->key_bitlen;

    if (!bitlen || bitlen % 16 ||
        bitlen / 16 > (~(DWORD)0 - sizeof(*hdr) - sizeof(*rsakey)) / 9)
        return STATUS_INVALID_PARAMETER;

    size = sizeof(*hdr) + sizeof(*rsakey) + (bitlen / 16) * 9;
    if (!buf || *buf_size < size)
    {
        *buf_size = size;
        return STATUS_BUFFER_TOO_SMALL;
    }

    if ((ret = pgnutls_x509_privkey_export_rsa_raw2( data->key, &m, &e, &d, &p, &q, &u, &e1, &e2 )) < 0)
    {
        pgnutls_perror( ret );
        return STATUS_INVALID_PARAMETER;
    }

    hdr = buf;
    hdr->bType    = PRIVATEKEYBLOB;
    hdr->bVersion = CUR_BLOB_VERSION;
    hdr->reserved = 0;
    hdr->aiKeyAlg = CALG_RSA_KEYX;

    rsakey = (RSAPUBKEY *)(hdr + 1);
    rsakey->magic  = RSA_MAGIC_KEY;
    rsakey->bitlen = bitlen;
    rsakey->pubexp = RSA_PUBEXP;

    dst = (BYTE *)(rsakey + 1);
    if (m.size == bitlen / 8 + 1 && !m.data[0]) src = m.data + 1;
    else if (m.size != bitlen / 8) goto done;
    else src = m.data;
    for (i = bitlen / 8 - 1; i >= 0; i--) *dst++ = src[i];

    if (p.size == bitlen / 16 + 1 && !p.data[0]) src = p.data + 1;
    else if (p.size != bitlen / 16) goto done;
    else src = p.data;
    for (i = bitlen / 16 - 1; i >= 0; i--) *dst++ = src[i];

    if (q.size == bitlen / 16 + 1 && !q.data[0]) src = q.data + 1;
    else if (q.size != bitlen / 16) goto done;
    else src = q.data;
    for (i = bitlen / 16 - 1; i >= 0; i--) *dst++ = src[i];

    if (e1.size == bitlen / 16 + 1 && !e1.data[0]) src = e1.data + 1;
    else if (e1.size != bitlen / 16) goto done;
    else src = e1.data;
    for (i = bitlen / 16 - 1; i >= 0; i--) *dst++ = src[i];

    if (e2.size == bitlen / 16 + 1 && !e2.data[0]) src = e2.data + 1;
    else if (e2.size != bitlen / 16) goto done;
    else src = e2.data;
    for (i = bitlen / 16 - 1; i >= 0; i--) *dst++ = src[i];

    if (u.size == bitlen / 16 + 1 && !u.data[0]) src = u.data + 1;
    else if (u.size != bitlen / 16) goto done;
    else src = u.data;
    for (i = bitlen / 16 - 1; i >= 0; i--) *dst++ = src[i];

    if (d.size == bitlen / 8 + 1 && !d.data[0]) src = d.data + 1;
    else if (d.size != bitlen / 8) goto done;
    else src = d.data;
    for (i = bitlen / 8 - 1; i >= 0; i--) *dst++ = src[i];
    status = STATUS_SUCCESS;

done:
    free( m.data );
    free( e.data );
    free( d.data );
    free( p.data );
    free( q.data );
    free( u.data );
    free( e1.data );
    free( e2.data );
    return status;
}

static NTSTATUS import_store_key( void *args )
{
    const struct import_store_key_params *params = args;
    struct cert_store_entry *entry;
    NTSTATUS status;

    if ((status = store_provider_enter())) return status;
    if (!(status = acquire_store( params->data, &entry )))
    {
        status = import_store_key_data( entry->data, params->buf, params->buf_size );
        release_store( entry );
    }
    store_provider_leave();
    return status;
}

static char *password_to_ascii( const WCHAR *str )
{
    char *ret;
    SIZE_T len;
    unsigned int i = 0;

    len = lstrlenW( str );
    if (len > ~(SIZE_T)0 / sizeof(*ret) - 1 ||
        !(ret = malloc( (len + 1) * sizeof(*ret) ))) return NULL;
    while (*str)
    {
        if (*str > 0x7f) WARN( "password contains non-ascii characters\n" );
        ret[i++] = *str++;
    }
    ret[i] = 0;
    return ret;
}

static int parse_bag_datum( gnutls_pkcs12_bag_t bag, unsigned int index, const char *pwd,
        gnutls_x509_crt_t *certs, unsigned int *cert_count,
        gnutls_x509_privkey_t *key )
{
    gnutls_datum_t data;
    int type = pgnutls_pkcs12_bag_get_type( bag, index );

    if (pgnutls_pkcs12_bag_get_data( bag, index, &data ) < 0) return -1;

    switch (type)
    {
    case GNUTLS_BAG_CERTIFICATE:
    {
        gnutls_x509_crt_t crt;

        if (pgnutls_x509_crt_init( &crt ) < 0) return -1;
        if (pgnutls_x509_crt_import( crt, &data, GNUTLS_X509_FMT_DER ) < 0)
        {
            pgnutls_x509_crt_deinit( crt );
            return -1;
        }
        certs[(*cert_count)++] = crt;
        break;
    }
    case GNUTLS_BAG_PKCS8_ENCRYPTED_KEY:
    case GNUTLS_BAG_PKCS8_KEY:
        if (*key) break; /* already found a key */
        if (pgnutls_x509_privkey_init( key ) < 0) return -1;
        if (pgnutls_x509_privkey_import_pkcs8( *key, &data, GNUTLS_X509_FMT_DER, pwd,
                type == GNUTLS_BAG_PKCS8_KEY ? GNUTLS_PKCS_PLAIN : 0 ) < 0)
        {
            pgnutls_x509_privkey_deinit( *key );
            *key = NULL;
            return -1;
        }
        break;
    }
    return 0;
}

static int parse_pkcs12_bag( gnutls_pkcs12_t p12, unsigned int index, const char *pwd,
        gnutls_x509_crt_t *certs, unsigned int *cert_count,
        gnutls_x509_privkey_t *key )
{
    gnutls_pkcs12_bag_t bag;
    unsigned int j, bag_count;
    int ret = -1;

    if (pgnutls_pkcs12_bag_init( &bag ) < 0) return -1;
    if (pgnutls_pkcs12_get_bag( p12, index, bag ) < 0) goto done;

    if (pgnutls_pkcs12_bag_get_type( bag, 0 ) == GNUTLS_BAG_ENCRYPTED
        && pgnutls_pkcs12_bag_decrypt( bag, pwd ) < 0)
        goto done;

    bag_count = pgnutls_pkcs12_bag_get_count( bag );
    for (j = 0; j < bag_count; j++)
        if (parse_bag_datum( bag, j, pwd, certs, cert_count, key ) < 0) goto done;

    ret = 0;
done:
    pgnutls_pkcs12_bag_deinit( bag );
    return ret;
}

static NTSTATUS parse_pkcs12_bags( gnutls_pkcs12_t p12, const char *pwd,
        gnutls_x509_crt_t **certs_ret, unsigned int *cert_count_ret,
        gnutls_x509_privkey_t *key_ret )
{
    gnutls_x509_privkey_t key = NULL;
    gnutls_x509_crt_t *certs = NULL;
    unsigned int i, bag_count, total_items = 0, cert_count = 0;
    SIZE_T alloc_count;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    for (i = 0; ; i++)
    {
        gnutls_pkcs12_bag_t bag;
        int count;

        if (pgnutls_pkcs12_bag_init( &bag ) < 0) goto error;
        if (pgnutls_pkcs12_get_bag( p12, i, bag ) < 0)
        {
            pgnutls_pkcs12_bag_deinit( bag );
            break;
        }
        if (pgnutls_pkcs12_bag_get_type( bag, 0 ) == GNUTLS_BAG_ENCRYPTED
            && pgnutls_pkcs12_bag_decrypt( bag, pwd ) < 0)
        {
            pgnutls_pkcs12_bag_deinit( bag );
            goto error;
        }

        count = pgnutls_pkcs12_bag_get_count( bag );
        pgnutls_pkcs12_bag_deinit( bag );
        if (count < 0) goto error;
        if ((unsigned int)count > ~0u - total_items) goto error;
        total_items += count;
    }
    bag_count = i;

    alloc_count = total_items;
    if (!alloc_count || alloc_count > ~(SIZE_T)0 / sizeof(*certs)) goto error;
    if (!(certs = malloc( alloc_count * sizeof(*certs) )))
    {
        status = STATUS_NO_MEMORY;
        goto error;
    }

    for (i = 0; i < bag_count; i++)
        if (parse_pkcs12_bag( p12, i, pwd, certs, &cert_count, &key ) < 0) goto error;

    if (!cert_count) goto error;

    *certs_ret = certs;
    *cert_count_ret = cert_count;
    *key_ret = key;
    return STATUS_SUCCESS;

error:
    for (i = 0; i < cert_count; i++) pgnutls_x509_crt_deinit( certs[i] );
    free( certs );
    if (key) pgnutls_x509_privkey_deinit( key );
    return status;
}

static NTSTATUS finish_open_cert_store( gnutls_pkcs12_t p12, const CRYPT_DATA_BLOB *pfx,
                                        const char *pwd, cert_store_data_t *token_ret,
                                        unsigned int *key_count_ret )
{
    gnutls_datum_t pfx_data;
    gnutls_x509_privkey_t key = NULL;
    gnutls_x509_crt_t *certs = NULL;
    unsigned int i, cert_count = 0, bitlen;
    NTSTATUS status;
    int ret;
    struct cert_store_data *store_data;
    cert_store_data_t token;

    pfx_data.data = pfx->pbData;
    pfx_data.size = pfx->cbData;
    if ((ret = pgnutls_pkcs12_import( p12, &pfx_data, GNUTLS_X509_FMT_DER, 0 )) < 0) goto error;
    if ((ret = pgnutls_pkcs12_verify_mac( p12, pwd )) < 0) goto error;

    status = parse_pkcs12_bags( p12, pwd, &certs, &cert_count, &key );
    if (status)
        goto done;

    if (key)
    {
        if ((ret = pgnutls_x509_privkey_get_pk_algorithm2( key, &bitlen )) < 0) goto error;

        if (ret != GNUTLS_PK_RSA)
        {
            FIXME( "key algorithm %u not supported\n", ret );
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
    }

    if (!(store_data = malloc( sizeof(*store_data) )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    store_data->p12 = p12;
    store_data->key = key;
    store_data->chain = certs;
    store_data->key_bitlen = key ? bitlen : 0;
    store_data->chain_len = cert_count;

    if ((status = publish_store( store_data, &token )))
    {
        destroy_cert_store( store_data );
        return status;
    }
    *token_ret = token;
    *key_count_ret = store_data->key ? 1 : 0;
    return STATUS_SUCCESS;

error:
    pgnutls_perror( ret );
    status = STATUS_INVALID_PARAMETER;
done:
    for (i = 0; i < cert_count; i++) pgnutls_x509_crt_deinit( certs[i] );
    free( certs );
    if (key) pgnutls_x509_privkey_deinit( key );
    if (p12) pgnutls_pkcs12_deinit( p12 );
    return status;
}

static NTSTATUS open_cert_store( void *args )
{
    const struct open_cert_store_params *params = args;
    gnutls_pkcs12_t p12 = NULL;
    cert_store_data_t token;
    unsigned int key_count;
    char *pwd = NULL;
    NTSTATUS status;
    int ret;

    if ((status = store_provider_enter())) return status;
    if (params->password && !(pwd = password_to_ascii( params->password )))
        status = STATUS_NO_MEMORY;
    else if ((ret = pgnutls_pkcs12_init( &p12 )) < 0)
    {
        pgnutls_perror( ret );
        status = STATUS_INVALID_PARAMETER;
    }
    else if (!(status = finish_open_cert_store( p12, params->pfx, pwd ? pwd : "",
                                                 &token, &key_count )))
    {
        *params->data_ret = token;
        *params->key_count_ret = key_count;
    }
    free( pwd );
    store_provider_leave();
    return status;
}

static NTSTATUS import_store_cert_data( struct cert_store_data *data, unsigned int index,
                                        void *buf, DWORD *buf_size )
{
    size_t size = 0;
    int ret;

    if (index >= data->chain_len) return STATUS_NO_MORE_ENTRIES;

    if ((ret = pgnutls_x509_crt_export( data->chain[index], GNUTLS_X509_FMT_DER, NULL, &size )) != GNUTLS_E_SHORT_MEMORY_BUFFER ||
        size > ~(DWORD)0)
        return STATUS_INVALID_PARAMETER;

    if (!buf || *buf_size < size)
    {
        *buf_size = size;
        return STATUS_BUFFER_TOO_SMALL;
    }
    if ((ret = pgnutls_x509_crt_export( data->chain[index], GNUTLS_X509_FMT_DER, buf, &size )) < 0)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

static NTSTATUS import_store_cert( void *args )
{
    const struct import_store_cert_params *params = args;
    struct cert_store_entry *entry;
    NTSTATUS status;

    if ((status = store_provider_enter())) return status;
    if (!(status = acquire_store( params->data, &entry )))
    {
        status = import_store_cert_data( entry->data, params->index,
                                         params->buf, params->buf_size );
        release_store( entry );
    }
    store_provider_leave();
    return status;
}

static NTSTATUS close_cert_store( void *args )
{
    const struct close_cert_store_params *params = args;
    NTSTATUS status;

    if ((status = store_provider_enter())) return status;
    status = close_store( params->data );
    store_provider_leave();
    return status;
}

struct cert_store_export_context
{
    gnutls_pkcs12_t p12;
    gnutls_pkcs12_bag_t cert_bag, key_bag;
    gnutls_x509_crt_t crt;
    gnutls_x509_privkey_t key;
    gnutls_datum_t out;
    char *pwd;
};

static void cleanup_cert_store_export( struct cert_store_export_context *context )
{
    free( context->out.data );
    if (context->p12) pgnutls_pkcs12_deinit( context->p12 );
    if (context->key_bag) pgnutls_pkcs12_bag_deinit( context->key_bag );
    if (context->cert_bag) pgnutls_pkcs12_bag_deinit( context->cert_bag );
    if (context->key) pgnutls_x509_privkey_deinit( context->key );
    if (context->crt) pgnutls_x509_crt_deinit( context->crt );
    free( context->pwd );
}

static NTSTATUS init_cert_store_export( struct cert_store_export_context *context,
                                        const WCHAR *password )
{
    int ret;

    if (password && !(context->pwd = password_to_ascii( password )))
        return STATUS_NO_MEMORY;
    /* Create cert bag (always initialized, even for empty stores). */
    if ((ret = pgnutls_pkcs12_bag_init( &context->cert_bag )) < 0)
    {
        pgnutls_perror( ret );
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS init_cert_to_export( struct cert_store_export_context *context )
{
    int ret;

    if ((ret = pgnutls_x509_crt_init( &context->crt )) < 0)
    {
        pgnutls_perror( ret );
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS import_cert_to_export( struct cert_store_export_context *context,
                                       const BYTE *cert_data, DWORD cert_size )
{
    gnutls_datum_t cert_datum;
    int ret;

    cert_datum.data = (unsigned char *)cert_data;
    cert_datum.size = cert_size;
    if ((ret = pgnutls_x509_crt_import( context->crt, &cert_datum,
                                        GNUTLS_X509_FMT_DER )) < 0) goto error;
    if ((ret = pgnutls_pkcs12_bag_set_crt( context->cert_bag,
                                           context->crt )) < 0) goto error;
    return STATUS_SUCCESS;

error:
    pgnutls_perror( ret );
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS add_cert_to_export( struct cert_store_export_context *context,
                                    const BYTE *cert_data, DWORD cert_size )
{
    NTSTATUS status;

    /* Import the certificate if provided. */
    if (!cert_data || !cert_size) return STATUS_SUCCESS;
    if ((status = init_cert_to_export( context ))) return status;
    return import_cert_to_export( context, cert_data, cert_size );
}

static NTSTATUS validate_key_to_export( const BCRYPT_RSAKEY_BLOB *hdr,
                                        DWORD key_blob_size, SIZE_T *capture_size )
{
    SIZE_T remaining, needed;

    if (key_blob_size < sizeof(*hdr)) return STATUS_INVALID_PARAMETER;
    if (hdr->Magic != BCRYPT_RSAFULLPRIVATE_MAGIC)
    {
        WARN( "unexpected key blob magic %08lx\n", (unsigned long)hdr->Magic );
        return STATUS_INVALID_PARAMETER;
    }

    remaining = key_blob_size - sizeof(*hdr);
    needed = hdr->cbPublicExp;
    if (needed > remaining || hdr->cbModulus > (remaining - needed) / 2 ||
        hdr->cbPrime1 > (remaining - needed - 2 * (SIZE_T)hdr->cbModulus) / 3 ||
        hdr->cbPrime2 > (remaining - needed - 2 * (SIZE_T)hdr->cbModulus -
                          3 * (SIZE_T)hdr->cbPrime1) / 2)
        return STATUS_INVALID_PARAMETER;
    needed += 2 * (SIZE_T)hdr->cbModulus + 3 * (SIZE_T)hdr->cbPrime1 +
              2 * (SIZE_T)hdr->cbPrime2;
    if (needed > remaining) return STATUS_INVALID_PARAMETER;
    if (capture_size) *capture_size = sizeof(*hdr) + needed;
    return STATUS_SUCCESS;
}

static NTSTATUS init_key_to_export( struct cert_store_export_context *context )
{
    int ret;

    if ((ret = pgnutls_x509_privkey_init( &context->key )) < 0)
    {
        pgnutls_perror( ret );
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS import_key_to_export( struct cert_store_export_context *context,
                                      const BYTE *key_blob )
{
    const BCRYPT_RSAKEY_BLOB *hdr = (const BCRYPT_RSAKEY_BLOB *)key_blob;
    const BYTE *ptr = key_blob + sizeof(*hdr);
    gnutls_datum_t m, e, d, p, q, u;
    int ret;

    /* Layout after header: PublicExp, Modulus, Prime1, Prime2, Exponent1, Exponent2, Coefficient, PrivateExponent */
    e.data = (unsigned char *)ptr;                    e.size = hdr->cbPublicExp;
    ptr += hdr->cbPublicExp;
    m.data = (unsigned char *)ptr;                    m.size = hdr->cbModulus;
    ptr += hdr->cbModulus;
    p.data = (unsigned char *)ptr;                    p.size = hdr->cbPrime1;
    ptr += hdr->cbPrime1;
    q.data = (unsigned char *)ptr;                    q.size = hdr->cbPrime2;
    ptr += hdr->cbPrime2;
    /* Skip Exponent1 and Exponent2 - GnuTLS computes them. */
    ptr += hdr->cbPrime1;
    ptr += hdr->cbPrime2;
    u.data = (unsigned char *)ptr;                    u.size = hdr->cbPrime1;
    ptr += hdr->cbPrime1;
    d.data = (unsigned char *)ptr;                    d.size = hdr->cbModulus;

    if ((ret = pgnutls_x509_privkey_import_rsa_raw( context->key, &m, &e, &d,
                                                    &p, &q, &u )) < 0) goto error;
    if ((ret = pgnutls_pkcs12_bag_init( &context->key_bag )) < 0) goto error;
    if ((ret = pgnutls_pkcs12_bag_set_privkey( context->key_bag, context->key,
                                               context->pwd ? context->pwd : "", 0 )) < 0)
        goto error;
    return STATUS_SUCCESS;

error:
    pgnutls_perror( ret );
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS add_key_to_export( struct cert_store_export_context *context,
                                   const BYTE *key_blob, DWORD key_blob_size )
{
    NTSTATUS status;

    if (!key_blob || !key_blob_size) return STATUS_SUCCESS;
    if (key_blob_size < sizeof(BCRYPT_RSAKEY_BLOB)) return STATUS_INVALID_PARAMETER;
    if ((status = validate_key_to_export( (const BCRYPT_RSAKEY_BLOB *)key_blob,
                                          key_blob_size, NULL ))) return status;
    if ((status = init_key_to_export( context ))) return status;
    return import_key_to_export( context, key_blob );
}

static NTSTATUS finish_cert_store_export( struct cert_store_export_context *context )
{
    int ret;

    if ((ret = pgnutls_pkcs12_init( &context->p12 )) < 0) goto error;
    if ((ret = pgnutls_pkcs12_set_bag( context->p12, context->cert_bag )) < 0) goto error;
    if (context->key_bag &&
        (ret = pgnutls_pkcs12_set_bag( context->p12, context->key_bag )) < 0) goto error;
    if ((ret = pgnutls_pkcs12_generate_mac2( context->p12, GNUTLS_MAC_SHA256,
                                             context->pwd ? context->pwd : "" )) < 0)
        goto error;
    if ((ret = pgnutls_pkcs12_export2( context->p12, GNUTLS_X509_FMT_DER,
                                       &context->out )) < 0) goto error;
    if (context->out.size > ~(DWORD)0 - 8) return STATUS_INTEGER_OVERFLOW;
    return STATUS_SUCCESS;

error:
    pgnutls_perror( ret );
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS export_cert_store_data( const struct export_cert_store_params *params )
{
    struct cert_store_export_context context = {0};
    DWORD required;
    NTSTATUS status;

    if ((status = init_cert_store_export( &context, params->password ))) goto done;
    if ((status = add_cert_to_export( &context, params->cert_data,
                                      params->cert_size ))) goto done;
    if ((status = add_key_to_export( &context, params->key_blob,
                                     params->key_blob_size ))) goto done;
    if ((status = finish_cert_store_export( &context ))) goto done;

    required = context.out.size + 8;
    if (!params->pfx_data)
    {
        *params->pfx_size = required;
        status = STATUS_SUCCESS;
    }
    else if (*params->pfx_size < required)
    {
        *params->pfx_size = required;
        status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        memcpy( params->pfx_data, context.out.data, context.out.size );
        *params->pfx_size = context.out.size;
        status = STATUS_SUCCESS;
    }
done:
    cleanup_cert_store_export( &context );
    return status;
}

static NTSTATUS export_cert_store( void *args )
{
    NTSTATUS status;

    if ((status = store_provider_enter())) return status;
    status = export_cert_store_data( args );
    store_provider_leave();
    return status;
}

#else /* SONAME_LIBGNUTLS */

static NTSTATUS process_attach( void *args ) { return STATUS_SUCCESS; }
static NTSTATUS process_detach( void *args ) { return STATUS_SUCCESS; }
static NTSTATUS open_cert_store( void *args ) { return STATUS_DLL_NOT_FOUND; }
static NTSTATUS import_store_key( void *args ) { return STATUS_DLL_NOT_FOUND; }
static NTSTATUS import_store_cert( void *args ) { return STATUS_DLL_NOT_FOUND; }
static NTSTATUS close_cert_store( void *args ) { return STATUS_DLL_NOT_FOUND; }
static NTSTATUS export_cert_store( void *args ) { return STATUS_DLL_NOT_FOUND; }

#endif /* SONAME_LIBGNUTLS */

struct root_cert
{
    struct list entry;
    SIZE_T      size;
    BYTE        data[1];
};

static struct list root_cert_list = LIST_INIT(root_cert_list);
static pthread_mutex_t root_cert_mutex = PTHREAD_MUTEX_INITIALIZER;
static BOOL root_certs_loaded;

static BYTE *add_cert( SIZE_T size )
{
    struct root_cert *cert;

    if (size > ~(SIZE_T)0 - offsetof( struct root_cert, data )) return NULL;
    cert = malloc( offsetof( struct root_cert, data ) + size );

    if (!cert) return NULL;
    cert->size = size;
    list_add_tail( &root_cert_list, &cert->entry );
    return cert->data;
}

struct DynamicBuffer
{
    DWORD allocated;
    DWORD used;
    char *data;
};

static inline void reset_buffer(struct DynamicBuffer *buffer)
{
    buffer->used = 0;
    if (buffer->data) buffer->data[0] = 0;
}

static BOOL add_line_to_buffer(struct DynamicBuffer *buffer, LPCSTR line)
{
    SIZE_T line_len = strlen( line );

    if (buffer->used == ~(DWORD)0 || line_len > ~(DWORD)0 - buffer->used - 1)
        return FALSE;
    if (buffer->used + line_len + 1 > buffer->allocated)
    {
        DWORD doubled = buffer->allocated > ~(DWORD)0 / 2 ? ~(DWORD)0 : buffer->allocated * 2;
        DWORD new_size = max( max( doubled, 1024 ), buffer->used + line_len + 1 );
        void *ptr = realloc( buffer->data, new_size );
        if (!ptr) return FALSE;
        buffer->data = ptr;
        buffer->allocated = new_size;
        if (!buffer->used) buffer->data[0] = 0;
    }
    strcpy( buffer->data + buffer->used, line );
    buffer->used += line_len;
    return TRUE;
}

#define BASE64_DECODE_PADDING    0x100
#define BASE64_DECODE_WHITESPACE 0x200
#define BASE64_DECODE_INVALID    0x300

static inline int decodeBase64Byte(char c)
{
    int ret = BASE64_DECODE_INVALID;

    if (c >= 'A' && c <= 'Z')
        ret = c - 'A';
    else if (c >= 'a' && c <= 'z')
        ret = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
        ret = c - '0' + 52;
    else if (c == '+')
        ret = 62;
    else if (c == '/')
        ret = 63;
    else if (c == '=')
        ret = BASE64_DECODE_PADDING;
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        ret = BASE64_DECODE_WHITESPACE;
    return ret;
}

static BOOL base64_to_cert( const char *str )
{
    DWORD i, valid, out, hasPadding;
    BYTE block[4], *data;

    for (i = valid = out = hasPadding = 0; str[i]; i++)
    {
        int d = decodeBase64Byte( str[i] );
        if (d == BASE64_DECODE_INVALID) return FALSE;
        if (d == BASE64_DECODE_WHITESPACE) continue;

        /* When padding starts, data is not acceptable */
        if (hasPadding && d != BASE64_DECODE_PADDING) return FALSE;

        /* Padding after a full block (like "VVVV=") is ok and stops decoding */
        if (d == BASE64_DECODE_PADDING && (valid & 3) == 0) break;

        valid++;
        if (d == BASE64_DECODE_PADDING)
        {
            hasPadding = 1;
            /* When padding reaches a full block, stop decoding */
            if ((valid & 3) == 0) break;
            continue;
        }

        /* out is incremented in the 4-char block as follows: "1-23" */
        if ((valid & 3) != 2) out++;
    }
    /* Fail if the block has bad padding; omitting padding is fine */
    if ((valid & 3) != 0 && hasPadding) return FALSE;

    if (!(data = add_cert( out ))) return FALSE;
    for (i = valid = out = 0; str[i]; i++)
    {
        int d = decodeBase64Byte( str[i] );
        if (d == BASE64_DECODE_WHITESPACE) continue;
        if (d == BASE64_DECODE_PADDING) break;
        block[valid & 3] = d;
        valid += 1;
        switch (valid & 3)
        {
        case 1:
            data[out++] = (block[0] << 2);
            break;
        case 2:
            data[out-1] = (block[0] << 2) | (block[1] >> 4);
            break;
        case 3:
            data[out++] = (block[1] << 4) | (block[2] >> 2);
            break;
        case 0:
            data[out++] = (block[2] << 6) | (block[3] >> 0);
            break;
        }
    }
    return TRUE;
}

/* Reads the file fd, and imports any certificates in it into store. */
static BOOL import_certs_from_file( int fd )
{
    FILE *fp = fdopen(fd, "r");
    char line[1024];
    BOOL in_cert = FALSE;
    struct DynamicBuffer saved_cert = { 0, 0, NULL };
    int num_certs = 0;

    if (!fp) return FALSE;
    TRACE("\n");
    while (fgets(line, sizeof(line), fp))
    {
        static const char header[] = "-----BEGIN CERTIFICATE-----";
        static const char trailer[] = "-----END CERTIFICATE-----";

        if (!strncmp(line, header, strlen(header)))
        {
            TRACE("begin new certificate\n");
            in_cert = TRUE;
            reset_buffer(&saved_cert);
        }
        else if (!strncmp(line, trailer, strlen(trailer)))
        {
            TRACE("end of certificate, adding cert\n");
            if (in_cert && saved_cert.data && base64_to_cert( saved_cert.data )) num_certs++;
            in_cert = FALSE;
        }
        else if (in_cert && !add_line_to_buffer(&saved_cert, line)) in_cert = FALSE;
    }
    free( saved_cert.data );
    TRACE("Read %d certs\n", num_certs);
    fclose(fp);
    return TRUE;
}

static void import_certs_from_path(LPCSTR path, BOOL allow_dir);

static BOOL check_buffer_resize(char **ptr_buf, size_t *buf_size, size_t check_size)
{
    if (check_size > *buf_size)
    {
        void *ptr = realloc(*ptr_buf, check_size);

        if (!ptr) return FALSE;
        *buf_size = check_size;
        *ptr_buf = ptr;
    }
    return TRUE;
}

/* Opens path, which must be a directory, and imports certificates from every
 * file in the directory into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static void import_certs_from_dir( LPCSTR path )
{
    DIR *dir;

    dir = opendir(path);
    if (dir)
    {
        size_t path_len = strlen(path), bufsize = 0;
        char *filebuf = NULL;

        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            {
                size_t name_len = strlen(entry->d_name);

                if (path_len > ~(size_t)0 - 2 || name_len > ~(size_t)0 - path_len - 2) break;
                if (!check_buffer_resize(&filebuf, &bufsize, path_len + 1 + name_len + 1)) break;
                snprintf(filebuf, bufsize, "%s/%s", path, entry->d_name);
                import_certs_from_path(filebuf, FALSE);
            }
        }
        free(filebuf);
        closedir(dir);
    }
}

/* Opens path, which may be a file or a directory, and imports any certificates
 * it finds into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static void import_certs_from_path(LPCSTR path, BOOL allow_dir)
{
    int fd;

    TRACE("(%s, %d)\n", debugstr_a(path), allow_dir);

    fd = open(path, O_RDONLY);
    if (fd != -1)
    {
        struct stat st;

        if (fstat(fd, &st) == 0)
        {
            if (S_ISREG(st.st_mode))
            {
                if (import_certs_from_file(fd)) fd = -1;
            }
            else if (S_ISDIR(st.st_mode))
            {
                if (allow_dir)
                    import_certs_from_dir(path);
                else
                    WARN("%s is a directory and directories are disallowed\n",
                     debugstr_a(path));
            }
            else
                ERR("%s: invalid file type\n", path);
        }
        if (fd != -1) close(fd);
    }
}

static const char * const CRYPT_knownLocations[] = {
 "/etc/ssl/certs/ca-certificates.crt",
 "/etc/ssl/certs",
 "/etc/pki/tls/certs/ca-bundle.crt",
 "/usr/share/ca-certificates/ca-bundle.crt",
 "/usr/local/share/certs/",
 "/etc/sfw/openssl/certs",
 "/etc/security/cacerts",  /* Android */
};

static void load_root_certs(void)
{
    unsigned int i;

#ifdef __APPLE__
    const SecTrustSettingsDomain domains[] = {
        kSecTrustSettingsDomainSystem,
        kSecTrustSettingsDomainAdmin,
        kSecTrustSettingsDomainUser
    };
    OSStatus status;
    CFArrayRef certs;
    DWORD domain;

    for (domain = 0; domain < ARRAY_SIZE(domains); domain++)
    {
        status = SecTrustSettingsCopyCertificates(domains[domain], &certs);
        if (status == noErr)
        {
            for (i = 0; i < CFArrayGetCount(certs); i++)
            {
                SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(certs, i);
                CFDataRef certData;
                if ((status = SecItemExport(cert, kSecFormatX509Cert, 0, NULL, &certData)) == noErr)
                {
                    BYTE *data = add_cert( CFDataGetLength(certData) );
                    if (data) memcpy( data, CFDataGetBytePtr(certData), CFDataGetLength(certData) );
                    CFRelease(certData);
                }
                else
                    WARN("could not export certificate %u to X509 format: 0x%08x\n", i, (unsigned int)status);
            }
            CFRelease(certs);
        }
    }
#endif

    for (i = 0; i < ARRAY_SIZE(CRYPT_knownLocations) && list_empty(&root_cert_list); i++)
        import_certs_from_path( CRYPT_knownLocations[i], TRUE );
}

static NTSTATUS enum_root_certs( void *args )
{
    struct enum_root_certs_params *params = args;
    struct list *ptr;
    struct root_cert *cert;
    NTSTATUS status = STATUS_SUCCESS;

    pthread_mutex_lock( &root_cert_mutex );
    if (!root_certs_loaded) load_root_certs();
    root_certs_loaded = TRUE;

    if (!(ptr = list_head( &root_cert_list )))
    {
        pthread_mutex_unlock( &root_cert_mutex );
        return STATUS_NO_MORE_ENTRIES;
    }
    cert = LIST_ENTRY( ptr, struct root_cert, entry );
    if (cert->size > ~(DWORD)0)
    {
        pthread_mutex_unlock( &root_cert_mutex );
        return STATUS_INTEGER_OVERFLOW;
    }
    __TRY
    {
        *params->needed = cert->size;
        if (cert->size <= params->size)
        {
            if (cert->size) memcpy( params->buffer, cert->data, cert->size );
            list_remove( &cert->entry );
            free( cert );
        }
    }
    __EXCEPT
    {
        status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY
    pthread_mutex_unlock( &root_cert_mutex );
    return status;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_attach,
    process_detach,
    open_cert_store,
    import_store_key,
    import_store_cert,
    close_cert_store,
    enum_root_certs,
    export_cert_store,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

typedef ULONG PTR32;

typedef struct
{
    DWORD cbData;
    PTR32 pbData;
} CRYPT_DATA_BLOB32;

struct open_cert_store_params32
{
    PTR32 pfx;
    PTR32 password;
    PTR32 data_ret;
    PTR32 key_count_ret;
};

struct import_store_key_params32
{
    cert_store_data_t data;
    PTR32 buf;
    PTR32 buf_size;
};

struct import_store_cert_params32
{
    cert_store_data_t data;
    unsigned int index;
    PTR32 buf;
    PTR32 buf_size;
};

struct enum_root_certs_params32
{
    PTR32 buffer;
    DWORD size;
    PTR32 needed;
};

struct export_cert_store_params32
{
    PTR32 cert_data;
    DWORD cert_size;
    PTR32 key_blob;
    DWORD key_blob_size;
    PTR32 password;
    PTR32 pfx_data;
    PTR32 pfx_size;
};

C_ASSERT( sizeof(struct open_cert_store_params32) == 16 );
C_ASSERT( sizeof(struct import_store_key_params32) == 16 );
C_ASSERT( sizeof(struct import_store_cert_params32) == 24 );
C_ASSERT( sizeof(struct close_cert_store_params) == 8 );
C_ASSERT( sizeof(struct enum_root_certs_params32) == 12 );
C_ASSERT( sizeof(struct export_cert_store_params32) == 28 );

static NTSTATUS wow64_check_call( UINT32 size, UINT32 flags )
{
    struct ntdll_wow64_unixlib_call_context context;
    NTSTATUS status;

    if ((status = ntdll_wow64_get_unixlib_call_context( &context ))) return status;
    if (context.args_size != size || context.flags != flags || (!context.guest_args && size))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_guest_range( PTR32 address, SIZE_T size, void **host )
{
    NTSTATUS status;

    if (size && (!address || size - 1 > ~(ULONG)0 - address)) return STATUS_ACCESS_VIOLATION;
    if ((status = ntdll_wow64_guest32_to_host( address, host ))) return status;
    return STATUS_SUCCESS;
}

#ifdef SONAME_LIBGNUTLS
static NTSTATUS wow64_copy_from_guest( PTR32 address, void *buffer, SIZE_T size )
{
    void *host;
    NTSTATUS status;

    if ((status = wow64_guest_range( address, size, &host ))) return status;
    return ntdll_wow64_copy_from_user( buffer, host, size );
}
#endif

static NTSTATUS wow64_output_range( PTR32 address, const void *data, SIZE_T size,
                                    struct ntdll_wow64_user_write_range *range )
{
    NTSTATUS status;

    if ((status = wow64_guest_range( address, size, &range->dst ))) return status;
    range->src = data;
    range->size = size;
    return STATUS_SUCCESS;
}

#ifdef SONAME_LIBGNUTLS
static NTSTATUS wow64_copy_wstr( PTR32 address, WCHAR **ret )
{
    WCHAR *buffer;
    unsigned int i;
    NTSTATUS status;

    *ret = NULL;
    if (!address) return STATUS_SUCCESS;
    if (!(buffer = malloc( 0x8000 * sizeof(*buffer) ))) return STATUS_NO_MEMORY;
    for (i = 0; i < 0x8000; i++)
    {
        if (address > ~(ULONG)0 - i * sizeof(*buffer))
        {
            free( buffer );
            return STATUS_ACCESS_VIOLATION;
        }
        if ((status = wow64_copy_from_guest( address + i * sizeof(*buffer), &buffer[i],
                                              sizeof(*buffer) )))
        {
            free( buffer );
            return status;
        }
        if (!buffer[i])
        {
            *ret = buffer;
            return STATUS_SUCCESS;
        }
    }
    free( buffer );
    return STATUS_NAME_TOO_LONG;
}
#endif

static NTSTATUS wow64_open_cert_store( void *args )
{
    const struct open_cert_store_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
#ifdef SONAME_LIBGNUTLS
    struct ntdll_wow64_user_write_range ranges[2];
    CRYPT_DATA_BLOB32 pfx32;
    CRYPT_DATA_BLOB pfx = {0};
    gnutls_pkcs12_t p12 = NULL;
    cert_store_data_t token;
    unsigned int key_count;
    WCHAR *password = NULL;
    char *pwd = NULL;
    NTSTATUS status;
    int ret;

    if ((status = wow64_check_call( sizeof(*params32), flags ))) return status;
    if ((status = store_provider_enter())) return status;
    if ((status = wow64_copy_wstr( params32->password, &password ))) goto done;
    if (password && !(pwd = password_to_ascii( password )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    if ((ret = pgnutls_pkcs12_init( &p12 )) < 0)
    {
        pgnutls_perror( ret );
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if ((status = wow64_copy_from_guest( params32->pfx, &pfx32, sizeof(pfx32) ))) goto done;
    pfx.cbData = pfx32.cbData;
    if (pfx.cbData)
    {
        if (!(pfx.pbData = malloc( pfx.cbData )))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        if ((status = wow64_copy_from_guest( pfx32.pbData, pfx.pbData, pfx.cbData ))) goto done;
    }
    if ((status = finish_open_cert_store( p12, &pfx, pwd ? pwd : "", &token, &key_count )))
    {
        p12 = NULL; /* finish_open_cert_store() consumed it on every path. */
        goto done;
    }
    p12 = NULL;
    if ((status = wow64_output_range( params32->data_ret, &token, sizeof(token), &ranges[0] )) ||
        (status = wow64_output_range( params32->key_count_ret, &key_count,
                                      sizeof(key_count), &ranges[1] )) ||
        (status = ntdll_wow64_atomic_writev( ranges, ARRAY_SIZE(ranges) )))
        close_store( token );
done:
    if (p12) pgnutls_pkcs12_deinit( p12 );
    free( pfx.pbData );
    free( pwd );
    free( password );
    store_provider_leave();
    return status;
#else
    NTSTATUS status = wow64_check_call( sizeof(*params32), flags );
    return status ? status : open_cert_store( args );
#endif
}

static NTSTATUS wow64_import_store_key( void *args )
{
    const struct import_store_key_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
#ifdef SONAME_LIBGNUTLS
    struct ntdll_wow64_user_write_range range;
    struct cert_store_entry *entry = NULL;
    DWORD capacity, required = 0, local_size;
    BYTE *buffer = NULL;
    NTSTATUS status;

    if ((status = wow64_check_call( sizeof(*params32), flags ))) return status;
    if ((status = store_provider_enter())) return status;
    if ((status = acquire_store( params32->data, &entry ))) goto done;
    if ((status = wow64_copy_from_guest( params32->buf_size, &capacity, sizeof(capacity) ))) goto done;
    status = import_store_key_data( entry->data, NULL, &required );
    if (status != STATUS_BUFFER_TOO_SMALL) goto done;
    if (!params32->buf || capacity < required)
    {
        if (!(status = wow64_output_range( params32->buf_size, &required,
                                           sizeof(required), &range )))
            status = ntdll_wow64_atomic_writev( &range, 1 );
        if (!status) status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    if (!(buffer = malloc( required )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    local_size = capacity;
    if ((status = import_store_key_data( entry->data, buffer, &local_size ))) goto done;
    if (!(status = wow64_output_range( params32->buf, buffer, required, &range )))
        status = ntdll_wow64_atomic_writev( &range, 1 );
done:
    free( buffer );
    if (entry) release_store( entry );
    store_provider_leave();
    return status;
#else
    NTSTATUS status = wow64_check_call( sizeof(*params32), flags );
    return status ? status : import_store_key( args );
#endif
}

static NTSTATUS wow64_import_store_cert( void *args )
{
    const struct import_store_cert_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
#ifdef SONAME_LIBGNUTLS
    struct ntdll_wow64_user_write_range range;
    struct cert_store_entry *entry = NULL;
    DWORD capacity, required = 0, local_size;
    BYTE *buffer = NULL;
    NTSTATUS status;

    if ((status = wow64_check_call( sizeof(*params32), flags ))) return status;
    if ((status = store_provider_enter())) return status;
    if ((status = acquire_store( params32->data, &entry ))) goto done;
    if ((status = wow64_copy_from_guest( params32->buf_size, &capacity, sizeof(capacity) ))) goto done;
    status = import_store_cert_data( entry->data, params32->index, NULL, &required );
    if (status != STATUS_BUFFER_TOO_SMALL) goto done;
    if (!params32->buf || capacity < required)
    {
        if (!(status = wow64_output_range( params32->buf_size, &required,
                                           sizeof(required), &range )))
            status = ntdll_wow64_atomic_writev( &range, 1 );
        if (!status) status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    if (!(buffer = malloc( required )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    local_size = capacity;
    if ((status = import_store_cert_data( entry->data, params32->index,
                                          buffer, &local_size ))) goto done;
    if (!(status = wow64_output_range( params32->buf, buffer, required, &range )))
        status = ntdll_wow64_atomic_writev( &range, 1 );
done:
    free( buffer );
    if (entry) release_store( entry );
    store_provider_leave();
    return status;
#else
    NTSTATUS status = wow64_check_call( sizeof(*params32), flags );
    return status ? status : import_store_cert( args );
#endif
}

static NTSTATUS wow64_enum_root_certs( void *args )
{
    const struct enum_root_certs_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range ranges[2];
    struct root_cert *cert;
    struct list *ptr;
    DWORD needed;
    ULONG count = 1;
    NTSTATUS status;

    if ((status = wow64_check_call( sizeof(*params32), flags ))) return status;
    pthread_mutex_lock( &root_cert_mutex );
    if (!root_certs_loaded) load_root_certs();
    root_certs_loaded = TRUE;
    if (!(ptr = list_head( &root_cert_list )))
    {
        status = STATUS_NO_MORE_ENTRIES;
        goto done;
    }
    cert = LIST_ENTRY( ptr, struct root_cert, entry );
    if (cert->size > ~(DWORD)0)
    {
        status = STATUS_INTEGER_OVERFLOW;
        goto done;
    }
    needed = cert->size;
    if ((status = wow64_output_range( params32->needed, &needed, sizeof(needed), &ranges[0] )))
        goto done;
    if (needed <= params32->size)
    {
        if ((status = wow64_output_range( params32->buffer, cert->data,
                                           cert->size, &ranges[1] ))) goto done;
        count = 2;
    }
    if ((status = ntdll_wow64_atomic_writev( ranges, count ))) goto done;
    if (count == 2)
    {
        list_remove( &cert->entry );
        free( cert );
    }
done:
    pthread_mutex_unlock( &root_cert_mutex );
    return status;
}

static NTSTATUS wow64_export_cert_store( void *args )
{
    const struct export_cert_store_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
#ifdef SONAME_LIBGNUTLS
    struct ntdll_wow64_user_write_range ranges[2];
    struct cert_store_export_context context = {0};
    BCRYPT_RSAKEY_BLOB key_header;
    DWORD capacity, required, actual;
    SIZE_T key_capture_size;
    BYTE *cert = NULL, *key = NULL;
    WCHAR *password = NULL;
    NTSTATUS status;

    if ((status = wow64_check_call( sizeof(*params32), flags ))) return status;
    if ((status = store_provider_enter())) return status;
    if ((status = wow64_copy_wstr( params32->password, &password ))) goto done;
    if ((status = init_cert_store_export( &context, password ))) goto done;
    if (params32->cert_data && params32->cert_size)
    {
        if ((status = init_cert_to_export( &context ))) goto done;
        if (!(cert = malloc( params32->cert_size )))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        if ((status = wow64_copy_from_guest( params32->cert_data, cert,
                                              params32->cert_size ))) goto done;
        if ((status = import_cert_to_export( &context, cert,
                                             params32->cert_size ))) goto done;
    }
    if (params32->key_blob && params32->key_blob_size)
    {
        if (params32->key_blob_size < sizeof(key_header))
        {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        if ((status = wow64_copy_from_guest( params32->key_blob, &key_header,
                                              sizeof(key_header) ))) goto done;
        if ((status = validate_key_to_export( &key_header, params32->key_blob_size,
                                              &key_capture_size ))) goto done;
        if ((status = init_key_to_export( &context ))) goto done;
        if (!(key = malloc( key_capture_size )))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        memcpy( key, &key_header, sizeof(key_header) );
        if (key_capture_size > sizeof(key_header) &&
            params32->key_blob > ~(ULONG)0 - sizeof(key_header))
        {
            status = STATUS_ACCESS_VIOLATION;
            goto done;
        }
        if (key_capture_size > sizeof(key_header) &&
            (status = wow64_copy_from_guest( params32->key_blob + sizeof(key_header),
                                              key + sizeof(key_header),
                                              key_capture_size - sizeof(key_header) )))
            goto done;
        if ((status = import_key_to_export( &context, key ))) goto done;
    }
    if ((status = finish_cert_store_export( &context ))) goto done;
    required = context.out.size + 8;
    if (!params32->pfx_data)
    {
        if (!(status = wow64_output_range( params32->pfx_size, &required,
                                           sizeof(required), &ranges[0] )))
            status = ntdll_wow64_atomic_writev( ranges, 1 );
        goto done;
    }
    if ((status = wow64_copy_from_guest( params32->pfx_size, &capacity,
                                          sizeof(capacity) ))) goto done;
    if (capacity < required)
    {
        if (!(status = wow64_output_range( params32->pfx_size, &required,
                                           sizeof(required), &ranges[0] )))
            status = ntdll_wow64_atomic_writev( ranges, 1 );
        if (!status) status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    actual = context.out.size;
    if ((status = wow64_output_range( params32->pfx_data, context.out.data,
                                      actual, &ranges[0] )) ||
        (status = wow64_output_range( params32->pfx_size, &actual, sizeof(actual), &ranges[1] )))
        goto done;
    status = ntdll_wow64_atomic_writev( ranges, ARRAY_SIZE(ranges) );
done:
    cleanup_cert_store_export( &context );
    free( key );
    free( cert );
    free( password );
    store_provider_leave();
    return status;
#else
    NTSTATUS status = wow64_check_call( sizeof(*params32), flags );
    return status ? status : export_cert_store( args );
#endif
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    process_attach,
    process_detach,
    wow64_open_cert_store,
    wow64_import_store_key,
    wow64_import_store_cert,
    close_cert_store,
    wow64_enum_root_certs,
    wow64_export_cert_store,
};

static const struct wine_unixlib_dispatch_entry_v2 wow64_dispatch_metadata[] =
{
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct open_cert_store_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct import_store_key_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct import_store_cert_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct close_cert_store_params, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct enum_root_certs_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct export_cert_store_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
};

WINE_UNIXLIB_DISPATCH_SOURCE_V2( __wine_unix_call_wow64_funcs, wow64_dispatch_metadata );

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );
C_ASSERT( ARRAYSIZE(wow64_dispatch_metadata) == unix_funcs_count );

#endif  /* _WIN64 */
