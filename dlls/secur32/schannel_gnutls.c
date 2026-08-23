/*
 * GnuTLS-based implementation of the schannel (SSL/TLS) provider.
 *
 * Copyright 2005 Juan Lang
 * Copyright 2008 Henri Verbeet
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
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <dlfcn.h>
#ifdef SONAME_LIBGNUTLS
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <gnutls/abstract.h>
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "bcrypt.h"
#include "sspi.h"
#include "secur32_priv.h"

#include "wine/unixlib.h"
#include "wine/debug.h"

#if defined(SONAME_LIBGNUTLS)

WINE_DEFAULT_DEBUG_CHANNEL(secur32);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static const char *system_priority_file;

/* Not present in gnutls version < 2.9.10. */
static int (*pgnutls_cipher_get_block_size)(gnutls_cipher_algorithm_t);

/* Not present in gnutls version < 3.0. */
static void (*pgnutls_transport_set_pull_timeout_function)(gnutls_session_t,
                                                           int (*)(gnutls_transport_ptr_t, unsigned int));
static void (*pgnutls_dtls_set_mtu)(gnutls_session_t, unsigned int);
static void (*pgnutls_dtls_set_timeouts)(gnutls_session_t, unsigned int, unsigned int);

/* Not present in gnutls version < 3.2.0. */
static int (*pgnutls_alpn_get_selected_protocol)(gnutls_session_t, gnutls_datum_t *);
static int (*pgnutls_alpn_set_protocols)(gnutls_session_t, const gnutls_datum_t *,
                                         unsigned, unsigned int);

/* Not present in gnutls version < 3.3.0. */
static int (*pgnutls_privkey_import_rsa_raw)(gnutls_privkey_t, const gnutls_datum_t *,
                                        const gnutls_datum_t *, const gnutls_datum_t *,
                                        const gnutls_datum_t *, const gnutls_datum_t *,
                                        const gnutls_datum_t *, const gnutls_datum_t *,
                                        const gnutls_datum_t *);

/* Not present in gnutls version < 3.4.0. */
static int (*pgnutls_privkey_export_x509)(gnutls_privkey_t, gnutls_x509_privkey_t *);

static void *libgnutls_handle;
#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gnutls_alert_get);
MAKE_FUNCPTR(gnutls_alert_get_name);
MAKE_FUNCPTR(gnutls_alert_send);
MAKE_FUNCPTR(gnutls_certificate_allocate_credentials);
MAKE_FUNCPTR(gnutls_certificate_free_credentials);
MAKE_FUNCPTR(gnutls_certificate_get_peers);
MAKE_FUNCPTR(gnutls_certificate_set_x509_key);
MAKE_FUNCPTR(gnutls_cipher_get);
MAKE_FUNCPTR(gnutls_cipher_get_key_size);
MAKE_FUNCPTR(gnutls_credentials_set);
MAKE_FUNCPTR(gnutls_deinit);
MAKE_FUNCPTR(gnutls_global_deinit);
MAKE_FUNCPTR(gnutls_global_init);
MAKE_FUNCPTR(gnutls_global_set_log_function);
MAKE_FUNCPTR(gnutls_global_set_log_level);
MAKE_FUNCPTR(gnutls_handshake);
MAKE_FUNCPTR(gnutls_init);
MAKE_FUNCPTR(gnutls_kx_get);
MAKE_FUNCPTR(gnutls_mac_get);
MAKE_FUNCPTR(gnutls_mac_get_key_size);
MAKE_FUNCPTR(gnutls_perror);
MAKE_FUNCPTR(gnutls_protocol_get_version);
MAKE_FUNCPTR(gnutls_priority_set_direct);
MAKE_FUNCPTR(gnutls_privkey_deinit);
MAKE_FUNCPTR(gnutls_privkey_init);
MAKE_FUNCPTR(gnutls_record_get_max_size);
MAKE_FUNCPTR(gnutls_record_recv);
MAKE_FUNCPTR(gnutls_record_send);
MAKE_FUNCPTR(gnutls_server_name_set);
MAKE_FUNCPTR(gnutls_session_channel_binding);
MAKE_FUNCPTR(gnutls_set_default_priority);
MAKE_FUNCPTR(gnutls_transport_get_ptr);
MAKE_FUNCPTR(gnutls_transport_set_errno);
MAKE_FUNCPTR(gnutls_transport_set_ptr);
MAKE_FUNCPTR(gnutls_transport_set_pull_function);
MAKE_FUNCPTR(gnutls_transport_set_push_function);
MAKE_FUNCPTR(gnutls_x509_crt_deinit);
MAKE_FUNCPTR(gnutls_x509_crt_import);
MAKE_FUNCPTR(gnutls_x509_crt_init);
MAKE_FUNCPTR(gnutls_x509_privkey_deinit);
#undef MAKE_FUNCPTR

#if GNUTLS_VERSION_MAJOR < 3
#define GNUTLS_CIPHER_AES_192_CBC 92
#define GNUTLS_CIPHER_AES_128_GCM 93
#define GNUTLS_CIPHER_AES_256_GCM 94

#define GNUTLS_MAC_AEAD 200

#define GNUTLS_KX_ANON_ECDH     11
#define GNUTLS_KX_ECDHE_RSA     12
#define GNUTLS_KX_ECDHE_ECDSA   13
#define GNUTLS_KX_ECDHE_PSK     14
#endif

#if GNUTLS_VERSION_MAJOR < 3 || (GNUTLS_VERSION_MAJOR == 3 && GNUTLS_VERSION_MINOR < 4)
#define GNUTLS_CIPHER_AES_128_CCM 19
#define GNUTLS_CIPHER_AES_256_CCM 20
#endif

#if GNUTLS_VERSION_MAJOR < 3 || (GNUTLS_VERSION_MAJOR == 3 && GNUTLS_VERSION_MINOR < 5)
#define GNUTLS_ALPN_SERVER_PRECEDENCE (1<<1)
#endif

struct schan_buffers
{
    SIZE_T offset;
    SIZE_T limit;
    const SecBufferDesc *desc;
    int current_buffer_idx;
};

struct schan_transport
{
    gnutls_session_t session;
    struct schan_buffers in;
    struct schan_buffers out;
};

#define SCHAN_CREDENTIAL_TOKEN_TAG UINT64_C(0x5343000000000000)
#define SCHAN_SESSION_TOKEN_TAG    UINT64_C(0x5353000000000000)
#define SCHAN_TOKEN_MASK           UINT64_C(0xffff000000000000)
#define SCHAN_TOKEN_MAX            UINT64_C(0x0000ffffffffffff)

struct schan_credential_object
{
    struct list entry;
    UINT64 token;
    gnutls_certificate_credentials_t credentials;
    unsigned int active, session_refs;
    BOOL closing, close_complete;
};

struct schan_session_object
{
    struct list entry;
    UINT64 token;
    gnutls_session_t session;
    struct schan_transport transport;
    struct schan_credential_object *credential;
    pthread_mutex_t mutex;
    unsigned int active;
    BOOL closing;
};

static pthread_mutex_t schan_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schan_registry_cond = PTHREAD_COND_INITIALIZER;
static struct list schan_credential_list = LIST_INIT(schan_credential_list);
static struct list schan_session_list = LIST_INIT(schan_session_list);
static UINT64 next_credential_token, next_session_token;
static unsigned int schan_calls;
static BOOL schan_attached, schan_draining;

static NTSTATUS schan_provider_enter(void)
{
    NTSTATUS status = STATUS_SUCCESS;

    pthread_mutex_lock( &schan_registry_mutex );
    if (!schan_attached || schan_draining) status = STATUS_DLL_NOT_FOUND;
    else schan_calls++;
    pthread_mutex_unlock( &schan_registry_mutex );
    return status;
}

static void schan_provider_leave(void)
{
    pthread_mutex_lock( &schan_registry_mutex );
    assert( schan_calls );
    if (!--schan_calls) pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
}

static struct schan_credential_object *find_credential( UINT64 token )
{
    struct schan_credential_object *object;

    LIST_FOR_EACH_ENTRY( object, &schan_credential_list, struct schan_credential_object, entry )
        if (object->token == token) return object;
    return NULL;
}

static struct schan_session_object *find_session( UINT64 token )
{
    struct schan_session_object *object;

    LIST_FOR_EACH_ENTRY( object, &schan_session_list, struct schan_session_object, entry )
        if (object->token == token) return object;
    return NULL;
}

static NTSTATUS publish_credential( struct schan_credential_object *object, UINT64 *token )
{
    UINT64 generation;

    pthread_mutex_lock( &schan_registry_mutex );
    if (!schan_attached || schan_draining)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_DLL_NOT_FOUND;
    }
    if (!(generation = ++next_credential_token) || generation > SCHAN_TOKEN_MAX)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    object->token = SCHAN_CREDENTIAL_TOKEN_TAG | generation;
    list_add_tail( &schan_credential_list, &object->entry );
    *token = object->token;
    pthread_mutex_unlock( &schan_registry_mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS publish_session( struct schan_session_object *object, UINT64 *token )
{
    UINT64 generation;

    pthread_mutex_lock( &schan_registry_mutex );
    if (!schan_attached || schan_draining)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_DLL_NOT_FOUND;
    }
    if (!(generation = ++next_session_token) || generation > SCHAN_TOKEN_MAX)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    object->token = SCHAN_SESSION_TOKEN_TAG | generation;
    list_add_tail( &schan_session_list, &object->entry );
    *token = object->token;
    pthread_mutex_unlock( &schan_registry_mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS acquire_credential( UINT64 token, struct schan_credential_object **ret )
{
    struct schan_credential_object *object;
    NTSTATUS status = STATUS_INVALID_HANDLE;

    *ret = NULL;
    if ((token & SCHAN_TOKEN_MASK) != SCHAN_CREDENTIAL_TOKEN_TAG) return status;
    pthread_mutex_lock( &schan_registry_mutex );
    if (schan_attached && !schan_draining && (object = find_credential( token )) && !object->closing)
    {
        object->active++;
        *ret = object;
        status = STATUS_SUCCESS;
    }
    pthread_mutex_unlock( &schan_registry_mutex );
    return status;
}

static void release_credential( struct schan_credential_object *object )
{
    pthread_mutex_lock( &schan_registry_mutex );
    assert( object->active );
    if (!--object->active) pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
}

static void destroy_credential( struct schan_credential_object *object )
{
    pgnutls_certificate_free_credentials( object->credentials );
    free( object );
}

static void release_session_credential( struct schan_credential_object *object )
{
    BOOL destroy = FALSE;

    pthread_mutex_lock( &schan_registry_mutex );
    assert( object->session_refs );
    if (!--object->session_refs && object->closing && object->close_complete && !object->active)
        destroy = TRUE;
    pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
    if (destroy) destroy_credential( object );
}

static void destroy_session( struct schan_session_object *object )
{
    pgnutls_transport_set_ptr( object->session, NULL );
    pgnutls_deinit( object->session );
    pthread_mutex_destroy( &object->mutex );
    release_session_credential( object->credential );
    free( object );
}

static NTSTATUS begin_session( UINT64 token, struct schan_session_object **ret )
{
    struct schan_session_object *object;
    NTSTATUS status;

    *ret = NULL;
    if ((status = schan_provider_enter())) return status;
    if ((token & SCHAN_TOKEN_MASK) != SCHAN_SESSION_TOKEN_TAG)
    {
        schan_provider_leave();
        return STATUS_INVALID_HANDLE;
    }
    pthread_mutex_lock( &schan_registry_mutex );
    if (!schan_attached || schan_draining || !(object = find_session( token )) || object->closing)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        schan_provider_leave();
        return STATUS_INVALID_HANDLE;
    }
    object->active++;
    pthread_mutex_unlock( &schan_registry_mutex );
    pthread_mutex_lock( &object->mutex );
    *ret = object;
    return STATUS_SUCCESS;
}

static void end_session( struct schan_session_object *object )
{
    pthread_mutex_unlock( &object->mutex );
    pthread_mutex_lock( &schan_registry_mutex );
    assert( object->active );
    if (!--object->active) pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
    schan_provider_leave();
}

static int compat_cipher_get_block_size(gnutls_cipher_algorithm_t cipher)
{
    switch(cipher) {
    case GNUTLS_CIPHER_3DES_CBC:
        return 8;
    case GNUTLS_CIPHER_AES_128_CBC:
    case GNUTLS_CIPHER_AES_256_CBC:
        return 16;
    case GNUTLS_CIPHER_ARCFOUR_128:
    case GNUTLS_CIPHER_ARCFOUR_40:
        return 1;
    case GNUTLS_CIPHER_DES_CBC:
        return 8;
    case GNUTLS_CIPHER_NULL:
        return 1;
    case GNUTLS_CIPHER_RC2_40_CBC:
        return 8;
    default:
        FIXME("Unknown cipher %#x, returning 1\n", cipher);
        return 1;
    }
}

static void compat_gnutls_transport_set_pull_timeout_function(gnutls_session_t session,
                                                              int (*func)(gnutls_transport_ptr_t, unsigned int))
{
    FIXME("\n");
}

static int compat_gnutls_privkey_export_x509(gnutls_privkey_t privkey, gnutls_x509_privkey_t *key)
{
    FIXME("\n");
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_import_rsa_raw(gnutls_privkey_t key, const gnutls_datum_t *p1,
                                        const gnutls_datum_t *p2, const gnutls_datum_t *p3,
                                        const gnutls_datum_t *p4, const gnutls_datum_t *p5,
                                        const gnutls_datum_t *p6, const gnutls_datum_t *p7,
                                        const gnutls_datum_t *p8)
{
    FIXME("\n");
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_alpn_get_selected_protocol(gnutls_session_t session, gnutls_datum_t *protocol)
{
    FIXME("\n");
    return GNUTLS_E_INVALID_REQUEST;
}

static int compat_gnutls_alpn_set_protocols(gnutls_session_t session, const gnutls_datum_t *protocols,
                                            unsigned size, unsigned int flags)
{
    FIXME("\n");
    return GNUTLS_E_INVALID_REQUEST;
}

static void compat_gnutls_dtls_set_mtu(gnutls_session_t session, unsigned int mtu)
{
    FIXME("\n");
}

static void compat_gnutls_dtls_set_timeouts(gnutls_session_t session, unsigned int retrans_timeout,
        unsigned int total_timeout)
{
    FIXME("\n");
}

static void init_schan_buffers(struct schan_buffers *s, const PSecBufferDesc desc)
{
    s->offset = 0;
    s->limit = ~0UL;
    s->desc = desc;
    s->current_buffer_idx = -1;
}

static int get_next_buffer(struct schan_buffers *s)
{
    if (s->current_buffer_idx == -1)
        return s->desc->cBuffers ? 0 : -1;
    if (s->current_buffer_idx == s->desc->cBuffers - 1)
        return -1;
    return s->current_buffer_idx + 1;
}

static char *get_buffer(struct schan_buffers *s, SIZE_T *count)
{
    SIZE_T max_count;
    PSecBuffer buffer;

    if (!s->desc)
    {
        TRACE("No desc\n");
        return NULL;
    }

    if (s->current_buffer_idx == -1)
    {
        /* Initial buffer */
        int buffer_idx = get_next_buffer(s);
        if (buffer_idx == -1)
        {
            TRACE("No next buffer\n");
            return NULL;
        }
        s->current_buffer_idx = buffer_idx;
    }

    buffer = &s->desc->pBuffers[s->current_buffer_idx];
    TRACE("Using buffer %d: cbBuffer %d, BufferType %#x, pvBuffer %p\n",
          s->current_buffer_idx, (unsigned)buffer->cbBuffer, (unsigned)buffer->BufferType, buffer->pvBuffer);

    max_count = buffer->cbBuffer - s->offset;
    if (s->limit != ~0UL && s->limit < max_count)
        max_count = s->limit;

    while (!max_count)
    {
        int buffer_idx;

        buffer_idx = get_next_buffer(s);
        if (buffer_idx == -1)
        {
            TRACE("No next buffer\n");
            return NULL;
        }
        s->current_buffer_idx = buffer_idx;
        s->offset = 0;
        buffer = &s->desc->pBuffers[buffer_idx];
        max_count = buffer->cbBuffer;
        if (s->limit != ~0UL && s->limit < max_count)
            max_count = s->limit;
    }

    if (*count > max_count)
        *count = max_count;
    if (s->limit != ~0UL)
        s->limit -= *count;

    return (char *)buffer->pvBuffer + s->offset;
}

static ssize_t pull_adapter(gnutls_transport_ptr_t transport, void *buff, size_t buff_len)
{
    struct schan_transport *t = (struct schan_transport*)transport;
    SIZE_T len = buff_len;
    char *b;

    TRACE("Pull %lu bytes\n", len);

    b = get_buffer(&t->in, &len);
    if (!b)
    {
        pgnutls_transport_set_errno(t->session, EAGAIN);
        return -1;
    }
    memcpy(buff, b, len);
    t->in.offset += len;
    TRACE("Read %lu bytes\n", len);
    return len;
}

static ssize_t push_adapter(gnutls_transport_ptr_t transport, const void *buff, size_t buff_len)
{
    struct schan_transport *t = (struct schan_transport*)transport;
    SIZE_T len = buff_len;
    char *b;

    TRACE("Push %lu bytes\n", len);

    b = get_buffer(&t->out, &len);
    if (!b)
    {
        pgnutls_transport_set_errno(t->session, EAGAIN);
        return -1;
    }
    memcpy(b, buff, len);
    t->out.offset += len;
    TRACE("Wrote %lu bytes\n", len);
    return len;
}

struct protocol_priority_flag {
    DWORD enable_flag;
    const char *gnutls_flag;
};

static const struct protocol_priority_flag client_protocol_priority_flags[] = {
    {SP_PROT_DTLS1_2_CLIENT, "VERS-DTLS1.2"},
    {SP_PROT_DTLS1_0_CLIENT, "VERS-DTLS1.0"},
    {SP_PROT_TLS1_3_CLIENT, "VERS-TLS1.3"},
    {SP_PROT_TLS1_2_CLIENT, "VERS-TLS1.2"},
    {SP_PROT_TLS1_1_CLIENT, "VERS-TLS1.1"},
    {SP_PROT_TLS1_0_CLIENT, "VERS-TLS1.0"},
    {SP_PROT_SSL3_CLIENT,   "VERS-SSL3.0"}
    /* {SP_PROT_SSL2_CLIENT} is not supported by GnuTLS */
};

static const struct protocol_priority_flag server_protocol_priority_flags[] = {
    {SP_PROT_DTLS1_2_SERVER, "VERS-DTLS1.2"},
    {SP_PROT_DTLS1_0_SERVER, "VERS-DTLS1.0"},
    {SP_PROT_TLS1_3_SERVER, "VERS-TLS1.3"},
    {SP_PROT_TLS1_2_SERVER, "VERS-TLS1.2"},
    {SP_PROT_TLS1_1_SERVER, "VERS-TLS1.1"},
    {SP_PROT_TLS1_0_SERVER, "VERS-TLS1.0"},
    {SP_PROT_SSL3_SERVER,   "VERS-SSL3.0"}
    /* {SP_PROT_SSL2_SERVER} is not supported by GnuTLS */
};

static DWORD supported_protocols;

static void check_supported_protocols(
 const struct protocol_priority_flag *flags, int num_flags, BOOLEAN server)
{
    const char *type_desc = server ? "server" : "client";
    gnutls_session_t session;
    char priority[64];
    unsigned i;
    int err;

    err = pgnutls_init(&session, server ? GNUTLS_SERVER : GNUTLS_CLIENT);
    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        return;
    }

    for(i = 0; i < num_flags; i++)
    {
        snprintf(priority, sizeof(priority), "NORMAL:-%s", flags[i].gnutls_flag);
        err = pgnutls_priority_set_direct(session, priority, NULL);
        if (err == GNUTLS_E_SUCCESS)
        {
            TRACE("%s %s is supported\n", type_desc, flags[i].gnutls_flag);
            supported_protocols |= flags[i].enable_flag;
        }
        else
            TRACE("%s %s is not supported\n", type_desc, flags[i].gnutls_flag);
    }

    pgnutls_deinit(session);
}

static NTSTATUS schan_get_enabled_protocols( void *args )
{
    NTSTATUS status, ret;

    if ((status = schan_provider_enter())) return status;
    ret = supported_protocols;
    schan_provider_leave();
    return ret;
}

static int pull_timeout(gnutls_transport_ptr_t transport, unsigned int timeout)
{
    struct schan_transport *t = (struct schan_transport*)transport;
    SIZE_T count = 0;

    TRACE("\n");

    if (get_buffer(&t->in, &count)) return 1;

    return 0;
}

static NTSTATUS set_priority(const schan_credentials *cred, gnutls_session_t session)
{
    char priority[128] = "NORMAL:%LATEST_RECORD_VERSION", *p;
    BOOL server = !!(cred->credential_use & SECPKG_CRED_INBOUND);
    const struct protocol_priority_flag *protocols =
        server ? server_protocol_priority_flags : client_protocol_priority_flags;
    int num_protocols = server ? ARRAYSIZE(server_protocol_priority_flags)
                               : ARRAYSIZE(client_protocol_priority_flags);
    BOOL using_vers_all = FALSE, disabled;
    int i, err;

    if (system_priority_file && strcmp(system_priority_file, "/dev/null"))
    {
        TRACE("Using defaults with system priority file override\n");
        err = pgnutls_set_default_priority(session);
        if (err != GNUTLS_E_SUCCESS)
        {
            pgnutls_perror(err);
            return STATUS_INTERNAL_ERROR;
        }
        return STATUS_SUCCESS;
    }

    p = priority + strlen(priority);

    /* VERS-ALL is nice to use for forward compatibility. It was introduced before support for TLS1.3,
     * so if TLS1.3 is supported, we may safely use it. Otherwise explicitly disable all known
     * disabled protocols. */
    if (supported_protocols & SP_PROT_TLS1_3_CLIENT)
    {
        strcpy(p, ":-VERS-ALL");
        p += strlen(p);
        using_vers_all = TRUE;
    }

    for (i = 0; i < num_protocols; i++)
    {
        if (!(supported_protocols & protocols[i].enable_flag)) continue;

        disabled = !(cred->enabled_protocols & protocols[i].enable_flag);
        if (using_vers_all && disabled) continue;

        *p++ = ':';
        *p++ = disabled ? '-' : '+';
        strcpy(p, protocols[i].gnutls_flag);
        p += strlen(p);
    }

    TRACE("Using %s priority\n", debugstr_a(priority));
    err = pgnutls_priority_set_direct(session, priority, NULL);
    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        return STATUS_INTERNAL_ERROR;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS create_session_data( const schan_credentials *cred, schan_session *session )
{
    unsigned int flags = (cred->credential_use == SECPKG_CRED_INBOUND) ? GNUTLS_SERVER : GNUTLS_CLIENT;
    struct schan_credential_object *credential = NULL;
    struct schan_session_object *object = NULL;
    gnutls_session_t s;
    NTSTATUS status;
    int err;

    if (cred->enabled_protocols & SP_PROT_DTLS1_X)
    {
        flags |= GNUTLS_DATAGRAM | GNUTLS_NONBLOCK;
    }

    err = pgnutls_init(&s, flags);
    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        return STATUS_INTERNAL_ERROR;
    }

    if (!(object = calloc( 1, sizeof(*object) )))
    {
        pgnutls_deinit(s);
        return STATUS_INTERNAL_ERROR;
    }
    if (pthread_mutex_init( &object->mutex, NULL ))
    {
        pgnutls_deinit( s );
        free( object );
        return STATUS_INTERNAL_ERROR;
    }
    object->session = s;
    object->transport.session = s;

    if ((status = set_priority(cred, s)))
    {
        pgnutls_deinit(s);
        pthread_mutex_destroy( &object->mutex );
        free(object);
        return status;
    }

    if ((status = acquire_credential( cred->credentials, &credential )))
    {
        pgnutls_deinit( s );
        pthread_mutex_destroy( &object->mutex );
        free( object );
        return status;
    }
    err = pgnutls_credentials_set(s, GNUTLS_CRD_CERTIFICATE, credential->credentials);
    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        release_credential( credential );
        pgnutls_deinit(s);
        pthread_mutex_destroy( &object->mutex );
        free(object);
        return STATUS_INTERNAL_ERROR;
    }
    pthread_mutex_lock( &schan_registry_mutex );
    credential->session_refs++;
    pthread_mutex_unlock( &schan_registry_mutex );
    release_credential( credential );
    object->credential = credential;

    pgnutls_transport_set_pull_function(s, pull_adapter);
    if (flags & GNUTLS_DATAGRAM) pgnutls_transport_set_pull_timeout_function(s, pull_timeout);
    pgnutls_transport_set_push_function(s, push_adapter);
    pgnutls_transport_set_ptr(s, (gnutls_transport_ptr_t)&object->transport);
    if ((status = publish_session( object, session )))
    {
        destroy_session( object );
        return status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS schan_create_session( void *args )
{
    const struct create_session_params *params = args;
    NTSTATUS status;

    if ((status = schan_provider_enter())) return status;
    *params->session = 0;
    status = create_session_data( params->cred, params->session );
    schan_provider_leave();
    return status;
}

static NTSTATUS schan_dispose_session( void *args )
{
    const struct session_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = schan_provider_enter())) return status;
    if ((params->session & SCHAN_TOKEN_MASK) != SCHAN_SESSION_TOKEN_TAG)
    {
        schan_provider_leave();
        return STATUS_INVALID_HANDLE;
    }
    pthread_mutex_lock( &schan_registry_mutex );
    if (!(object = find_session( params->session )) || object->closing)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        schan_provider_leave();
        return STATUS_INVALID_HANDLE;
    }
    object->closing = TRUE;
    list_remove( &object->entry );
    while (object->active) pthread_cond_wait( &schan_registry_cond, &schan_registry_mutex );
    pthread_mutex_unlock( &schan_registry_mutex );
    destroy_session( object );
    schan_provider_leave();
    return STATUS_SUCCESS;
}

static NTSTATUS schan_set_session_target( void *args )
{
    const struct set_session_target_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    pgnutls_server_name_set( object->session, GNUTLS_NAME_DNS,
                             params->target, strlen(params->target) );
    end_session( object );
    return STATUS_SUCCESS;
}

static gnutls_alert_level_t map_alert_type(unsigned int type)
{
    switch (type)
    {
    case TLS1_ALERT_WARNING: return GNUTLS_AL_WARNING;
    case TLS1_ALERT_FATAL:   return GNUTLS_AL_FATAL;
    default:
        FIXME( "unknown type %u\n", type );
        return -1;
    }
}

static gnutls_alert_description_t map_alert_number(unsigned int number)
{
    switch (number)
    {
    case TLS1_ALERT_CLOSE_NOTIFY:           return GNUTLS_A_CLOSE_NOTIFY;
    case TLS1_ALERT_UNEXPECTED_MESSAGE:     return GNUTLS_A_UNEXPECTED_MESSAGE;
    case TLS1_ALERT_BAD_RECORD_MAC:         return GNUTLS_A_BAD_RECORD_MAC;
    case TLS1_ALERT_DECRYPTION_FAILED:      return GNUTLS_A_DECRYPTION_FAILED;
    case TLS1_ALERT_RECORD_OVERFLOW:        return GNUTLS_A_RECORD_OVERFLOW;
    case TLS1_ALERT_DECOMPRESSION_FAIL:     return GNUTLS_A_DECOMPRESSION_FAILURE;
    case TLS1_ALERT_HANDSHAKE_FAILURE:      return GNUTLS_A_HANDSHAKE_FAILURE;
    case TLS1_ALERT_BAD_CERTIFICATE:        return GNUTLS_A_BAD_CERTIFICATE;
    case TLS1_ALERT_UNSUPPORTED_CERT:       return GNUTLS_A_UNSUPPORTED_CERTIFICATE;
    case TLS1_ALERT_CERTIFICATE_REVOKED:    return GNUTLS_A_CERTIFICATE_REVOKED;
    case TLS1_ALERT_CERTIFICATE_EXPIRED:    return GNUTLS_A_CERTIFICATE_EXPIRED;
    case TLS1_ALERT_CERTIFICATE_UNKNOWN:    return GNUTLS_A_CERTIFICATE_UNKNOWN;
    case TLS1_ALERT_ILLEGAL_PARAMETER:      return GNUTLS_A_ILLEGAL_PARAMETER;
    case TLS1_ALERT_UNKNOWN_CA:             return GNUTLS_A_UNKNOWN_CA;
    case TLS1_ALERT_ACCESS_DENIED:          return GNUTLS_A_ACCESS_DENIED;
    case TLS1_ALERT_DECODE_ERROR:           return GNUTLS_A_DECODE_ERROR;
    case TLS1_ALERT_DECRYPT_ERROR:          return GNUTLS_A_DECRYPT_ERROR;
    case TLS1_ALERT_EXPORT_RESTRICTION:     return GNUTLS_A_EXPORT_RESTRICTION;
    case TLS1_ALERT_PROTOCOL_VERSION:       return GNUTLS_A_PROTOCOL_VERSION;
    case TLS1_ALERT_INSUFFIENT_SECURITY:    return GNUTLS_A_INSUFFICIENT_SECURITY;
    case TLS1_ALERT_INTERNAL_ERROR:         return GNUTLS_A_INTERNAL_ERROR;
    case TLS1_ALERT_USER_CANCELED:          return GNUTLS_A_USER_CANCELED;
    case TLS1_ALERT_NO_RENEGOTIATION:       return GNUTLS_A_NO_RENEGOTIATION;
    case TLS1_ALERT_UNSUPPORTED_EXT:        return GNUTLS_A_UNSUPPORTED_EXTENSION;
    case TLS1_ALERT_UNKNOWN_PSK_IDENTITY:   return GNUTLS_A_UNKNOWN_PSK_IDENTITY;
    case TLS1_ALERT_NO_APP_PROTOCOL:        return GNUTLS_A_NO_APPLICATION_PROTOCOL;
    default:
        FIXME("unhandled alert %u\n", number);
        return -1;
    }
}

static NTSTATUS send_alert(gnutls_session_t session, unsigned int type, unsigned int number)
{
    gnutls_alert_level_t level = map_alert_type(type);
    gnutls_alert_description_t desc = map_alert_number(number);
    int ret;

    do
    {
        ret = pgnutls_alert_send(session, level, desc);
    }
    while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

    if (ret < 0)
    {
        pgnutls_perror(ret);
        return SEC_E_INTERNAL_ERROR;
    }
    return SEC_E_OK;
}

static NTSTATUS handshake_session( struct schan_session_object *object,
                                   const struct handshake_params *params )
{
    gnutls_session_t s = object->session;
    struct schan_transport *t = &object->transport;
    NTSTATUS status;
    int err;

    init_schan_buffers(&t->in, params->input);
    t->in.limit = params->input_size;
    init_schan_buffers(&t->out, params->output);

    if (params->control_token)
    {
        status = send_alert(s, params->alert_type, params->alert_number);
        goto done;
    }

    while (1)
    {
        err = pgnutls_handshake(s);
        if (err == GNUTLS_E_SUCCESS)
        {
            TRACE("Handshake completed\n");
            status = SEC_E_OK;
        }
        else if (err == GNUTLS_E_AGAIN)
        {
            TRACE("Continue...\n");
            status = SEC_I_CONTINUE_NEEDED;
        }
        else if (err == GNUTLS_E_WARNING_ALERT_RECEIVED)
        {
            gnutls_alert_description_t alert = pgnutls_alert_get(s);

            WARN("WARNING ALERT: %d %s\n", alert, pgnutls_alert_get_name(alert));

            if (alert == GNUTLS_A_UNRECOGNIZED_NAME)
            {
                TRACE("Ignoring\n");
                continue;
            }
            else
                status = SEC_E_INTERNAL_ERROR;
        }
        else if (err == GNUTLS_E_FATAL_ALERT_RECEIVED)
        {
            gnutls_alert_description_t alert = pgnutls_alert_get(s);
            WARN("FATAL ALERT: %d %s\n", alert, pgnutls_alert_get_name(alert));
            status = SEC_E_INTERNAL_ERROR;
        }
        else
        {
            pgnutls_perror(err);
            status = SEC_E_INTERNAL_ERROR;
        }
        break;
    }

done:
    *params->input_offset = t->in.offset;
    *params->output_buffer_idx = t->out.current_buffer_idx;
    *params->output_offset = t->out.offset;

    return status;
}

static NTSTATUS schan_handshake( void *args )
{
    const struct handshake_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    status = handshake_session( object, params );
    end_session( object );
    return status;
}

static DWORD get_protocol(gnutls_protocol_t proto)
{
    /* FIXME: currently schannel only implements client connections, but
     * there's no reason it couldn't be used for servers as well.  The
     * context doesn't tell us which it is, so assume client for now.
     */
    switch (proto)
    {
    case GNUTLS_SSL3: return SP_PROT_SSL3_CLIENT;
    case GNUTLS_TLS1_0: return SP_PROT_TLS1_0_CLIENT;
    case GNUTLS_TLS1_1: return SP_PROT_TLS1_1_CLIENT;
    case GNUTLS_TLS1_2: return SP_PROT_TLS1_2_CLIENT;
    case GNUTLS_DTLS1_0: return SP_PROT_DTLS1_0_CLIENT;
    case GNUTLS_DTLS1_2: return SP_PROT_DTLS1_2_CLIENT;
    default:
        FIXME("unknown protocol %d\n", proto);
        return 0;
    }
}

static ALG_ID get_cipher_algid(gnutls_cipher_algorithm_t cipher)
{
    switch (cipher)
    {
    case GNUTLS_CIPHER_UNKNOWN:
    case GNUTLS_CIPHER_NULL: return 0;
    case GNUTLS_CIPHER_ARCFOUR_40:
    case GNUTLS_CIPHER_ARCFOUR_128: return CALG_RC4;
    case GNUTLS_CIPHER_DES_CBC: return CALG_DES;
    case GNUTLS_CIPHER_3DES_CBC: return CALG_3DES;
    case GNUTLS_CIPHER_AES_128_CBC:
    case GNUTLS_CIPHER_AES_128_GCM: return CALG_AES_128;
    case GNUTLS_CIPHER_AES_192_CBC: return CALG_AES_192;
    case GNUTLS_CIPHER_AES_256_GCM:
    case GNUTLS_CIPHER_AES_256_CBC: return CALG_AES_256;
    case GNUTLS_CIPHER_RC2_40_CBC: return CALG_RC2;
    default:
        FIXME("unknown algorithm %d\n", cipher);
        return 0;
    }
}

static ALG_ID get_mac_algid(gnutls_mac_algorithm_t mac, gnutls_cipher_algorithm_t cipher)
{
    switch (mac)
    {
    case GNUTLS_MAC_UNKNOWN:
    case GNUTLS_MAC_NULL: return 0;
    case GNUTLS_MAC_MD2: return CALG_MD2;
    case GNUTLS_MAC_MD5: return CALG_MD5;
    case GNUTLS_MAC_SHA1: return CALG_SHA1;
    case GNUTLS_MAC_SHA256: return CALG_SHA_256;
    case GNUTLS_MAC_SHA384: return CALG_SHA_384;
    case GNUTLS_MAC_SHA512: return CALG_SHA_512;
    case GNUTLS_MAC_AEAD:
        /* When using AEAD (such as GCM), we return PRF algorithm instead
           which is defined in RFC 5289. */
        switch (cipher)
        {
        case GNUTLS_CIPHER_AES_128_GCM: return CALG_SHA_256;
        case GNUTLS_CIPHER_AES_256_GCM: return CALG_SHA_384;
        default:
            break;
        }
        /* fall through */
    default:
        FIXME("unknown algorithm %d, cipher %d\n", mac, cipher);
        return 0;
    }
}

static ALG_ID get_kx_algid(int kx)
{
    switch (kx)
    {
    case GNUTLS_KX_UNKNOWN: return 0;
    case GNUTLS_KX_RSA:
    case GNUTLS_KX_RSA_EXPORT: return CALG_RSA_KEYX;
    case GNUTLS_KX_DHE_PSK:
    case GNUTLS_KX_DHE_DSS:
    case GNUTLS_KX_DHE_RSA: return CALG_DH_EPHEM;
    case GNUTLS_KX_ANON_ECDH: return CALG_ECDH;
    case GNUTLS_KX_ECDHE_RSA:
    case GNUTLS_KX_ECDHE_PSK:
    case GNUTLS_KX_ECDHE_ECDSA: return CALG_ECDH_EPHEM;
    default:
        FIXME("unknown algorithm %d\n", kx);
        return 0;
    }
}

static NTSTATUS schan_get_session_cipher_block_size( void *args )
{
    const struct session_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status, ret;

    if ((status = begin_session( params->session, &object ))) return status;
    ret = pgnutls_cipher_get_block_size( pgnutls_cipher_get( object->session ) );
    end_session( object );
    return ret;
}

static NTSTATUS schan_get_max_message_size( void *args )
{
    const struct session_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status, ret;

    if ((status = begin_session( params->session, &object ))) return status;
    ret = pgnutls_record_get_max_size( object->session );
    end_session( object );
    return ret;
}

static NTSTATUS schan_get_connection_info( void *args )
{
    const struct get_connection_info_params *params = args;
    struct schan_session_object *object;
    SecPkgContext_ConnectionInfo *info = params->info;
    gnutls_protocol_t proto;
    gnutls_cipher_algorithm_t alg;
    gnutls_mac_algorithm_t mac;
    gnutls_kx_algorithm_t kx;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    proto = pgnutls_protocol_get_version( object->session );
    alg = pgnutls_cipher_get( object->session );
    mac = pgnutls_mac_get( object->session );
    kx = pgnutls_kx_get( object->session );

    info->dwProtocol = get_protocol(proto);
    info->aiCipher = get_cipher_algid(alg);
    info->dwCipherStrength = pgnutls_cipher_get_key_size(alg) * 8;
    info->aiHash = get_mac_algid(mac, alg);
    info->dwHashStrength = pgnutls_mac_get_key_size(mac) * 8;
    info->aiExch = get_kx_algid(kx);
    /* FIXME: info->dwExchStrength? */
    info->dwExchStrength = 0;
    end_session( object );
    return SEC_E_OK;
}

static DWORD get_protocol_version( gnutls_session_t session )
{
    gnutls_protocol_t proto = pgnutls_protocol_get_version( session );

    switch (proto)
    {
    case GNUTLS_SSL3:    return 0x300;
    case GNUTLS_TLS1_0:  return 0x301;
    case GNUTLS_TLS1_1:  return 0x302;
    case GNUTLS_TLS1_2:  return 0x303;
    case GNUTLS_DTLS1_0: return 0x201;
    case GNUTLS_DTLS1_2: return 0x202;
    default:
        FIXME( "unknown protocol %u\n", proto );
        return 0;
    }
}

static const WCHAR *get_cipher_str( gnutls_session_t session )
{
    static const WCHAR aesW[] = {'A','E','S',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};
    gnutls_cipher_algorithm_t cipher = pgnutls_cipher_get( session );

    switch (cipher)
    {
    case GNUTLS_CIPHER_AES_128_CBC:
    case GNUTLS_CIPHER_AES_192_CBC:
    case GNUTLS_CIPHER_AES_256_CBC:
    case GNUTLS_CIPHER_AES_128_GCM:
    case GNUTLS_CIPHER_AES_256_GCM:
    case GNUTLS_CIPHER_AES_128_CCM:
    case GNUTLS_CIPHER_AES_256_CCM:
        return aesW;
    default:
        FIXME( "unknown cipher %u\n", cipher );
        return unknownW;
    }
}

static DWORD get_cipher_len( gnutls_session_t session )
{
    gnutls_cipher_algorithm_t cipher = pgnutls_cipher_get( session );

    switch (cipher)
    {
    case GNUTLS_CIPHER_AES_128_CBC:
    case GNUTLS_CIPHER_AES_128_GCM:
    case GNUTLS_CIPHER_AES_128_CCM:
        return 128;
    case GNUTLS_CIPHER_AES_192_CBC:
        return 192;
    case GNUTLS_CIPHER_AES_256_CBC:
    case GNUTLS_CIPHER_AES_256_GCM:
    case GNUTLS_CIPHER_AES_256_CCM:
        return 256;
    default:
        FIXME( "unknown cipher %u\n", cipher );
        return 0;
    }
}

static DWORD get_cipher_block_len( gnutls_session_t session )
{
    gnutls_cipher_algorithm_t cipher = pgnutls_cipher_get( session );
    return pgnutls_cipher_get_block_size( cipher );
}

static const WCHAR *get_hash_str( gnutls_session_t session, BOOL full )
{
    static const WCHAR shaW[] = {'S','H','A',0};
    static const WCHAR sha1W[] = {'S','H','A','1',0};
    static const WCHAR sha224W[] = {'S','H','A','2','2','4',0};
    static const WCHAR sha256W[] = {'S','H','A','2','5','6',0};
    static const WCHAR sha384W[] = {'S','H','A','3','8','4',0};
    static const WCHAR sha512W[] = {'S','H','A','5','1','2',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};
    static const WCHAR emptyW[] = {0};
    gnutls_mac_algorithm_t mac = pgnutls_mac_get( session );

    switch (mac)
    {
    case GNUTLS_MAC_SHA1:   return full ? sha1W : shaW;
    case GNUTLS_MAC_SHA224: return sha224W;
    case GNUTLS_MAC_SHA256: return sha256W;
    case GNUTLS_MAC_SHA384: return sha384W;
    case GNUTLS_MAC_SHA512: return sha512W;
    case GNUTLS_MAC_AEAD:   return emptyW;
    default:
        FIXME( "unknown mac %u\n", mac );
        return unknownW;
    }
}

static DWORD get_hash_len( gnutls_session_t session )
{
    gnutls_mac_algorithm_t mac = pgnutls_mac_get( session );
    return pgnutls_mac_get_key_size( mac ) * 8;
}

static const WCHAR *get_exchange_str( gnutls_session_t session, BOOL full )
{
    static const WCHAR ecdhW[] = {'E','C','D','H',0};
    static const WCHAR ecdheW[] = {'E','C','D','H','E',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};
    gnutls_kx_algorithm_t kx = pgnutls_kx_get( session );

    switch (kx)
    {
    case GNUTLS_KX_ECDHE_RSA:
    case GNUTLS_KX_ECDHE_ECDSA:
        return full ? ecdheW : ecdhW;
    default:
        FIXME( "unknown kx %u\n", kx );
        return unknownW;
    }
}

static const WCHAR *get_certificate_str( gnutls_session_t session )
{
    static const WCHAR rsaW[] = {'R','S','A',0};
    static const WCHAR ecdsaW[] = {'E','C','D','S','A',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};
    gnutls_kx_algorithm_t kx = pgnutls_kx_get( session );

    switch (kx)
    {
    case GNUTLS_KX_RSA:
    case GNUTLS_KX_RSA_EXPORT:
    case GNUTLS_KX_DHE_RSA:
    case GNUTLS_KX_ECDHE_RSA:   return rsaW;
    case GNUTLS_KX_ECDHE_ECDSA: return ecdsaW;
    default:
        FIXME( "unknown kx %u\n", kx );
        return unknownW;
    }
}

static const WCHAR *get_chaining_mode_str( gnutls_session_t session )
{
    static const WCHAR cbcW[] = {'C','B','C',0};
    static const WCHAR ccmW[] = {'C','C','M',0};
    static const WCHAR gcmW[] = {'G','C','M',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};
    gnutls_cipher_algorithm_t cipher = pgnutls_cipher_get( session );

    switch (cipher)
    {
    case GNUTLS_CIPHER_AES_128_CBC:
    case GNUTLS_CIPHER_AES_192_CBC:
    case GNUTLS_CIPHER_AES_256_CBC:
        return cbcW;
    case GNUTLS_CIPHER_AES_128_GCM:
    case GNUTLS_CIPHER_AES_256_GCM:
        return gcmW;
    case GNUTLS_CIPHER_AES_128_CCM:
    case GNUTLS_CIPHER_AES_256_CCM:
        return ccmW;
    default:
        FIXME( "unknown cipher %u\n", cipher );
        return unknownW;
    }
}

static NTSTATUS schan_get_cipher_info( void *args )
{
    static const WCHAR tlsW[] = {'T','L','S','_',0};
    static const WCHAR underscoreW[] = {'_',0};
    static const WCHAR widthW[] = {'_','W','I','T','H','_',0};
    static const WCHAR sha384W[] = {'S','H','A','3','8','4',0};
    const struct get_cipher_info_params *params = args;
    struct schan_session_object *object;
    gnutls_session_t session;
    SecPkgContext_CipherInfo *info = params->info;
    char buf[11];
    WCHAR *ptr;
    const WCHAR *hash;
    NTSTATUS status;
    int len;

    if ((status = begin_session( params->session, &object ))) return status;
    session = object->session;

    info->dwProtocol = get_protocol_version( session );
    info->dwCipherSuite = 0; /* FIXME */
    info->dwBaseCipherSuite = 0; /* FIXME */
    wcscpy( info->szCipher, get_cipher_str( session ) );
    info->dwCipherLen = get_cipher_len( session );
    info->dwCipherBlockLen = get_cipher_block_len( session );
    wcscpy( info->szHash, get_hash_str( session, TRUE ) );
    info->dwHashLen = get_hash_len( session );
    wcscpy( info->szExchange, get_exchange_str( session, FALSE ) );
    info->dwMinExchangeLen = 0;
    info->dwMaxExchangeLen = 65536;
    wcscpy( info->szCertificate, get_certificate_str( session ) );
    info->dwKeyType = 0; /* FIXME */

    wcscpy( info->szCipherSuite, tlsW );
    wcscat( info->szCipherSuite, get_exchange_str( session, TRUE ) );
    wcscat( info->szCipherSuite, underscoreW );
    wcscat( info->szCipherSuite, info->szCertificate );
    wcscat( info->szCipherSuite, widthW );
    wcscat( info->szCipherSuite, info->szCipher );
    wcscat( info->szCipherSuite, underscoreW );
    len = snprintf( buf, sizeof(buf), "%u", (unsigned int)info->dwCipherLen ) + 1;
    ptr = info->szCipherSuite + wcslen( info->szCipherSuite );
    ntdll_umbstowcs( buf, len, ptr, len );
    wcscat( info->szCipherSuite, underscoreW );
    wcscat( info->szCipherSuite, get_chaining_mode_str( session ) );
    wcscat( info->szCipherSuite, underscoreW );
    hash = get_hash_str( session, FALSE );
    if (hash[0]) wcscat( info->szCipherSuite, hash );
    else wcscat( info->szCipherSuite, sha384W ); /* FIXME */
    end_session( object );
    return SEC_E_OK;
}

static NTSTATUS schan_get_unique_channel_binding( void *args )
{
    const struct get_unique_channel_binding_params *params = args;
    struct schan_session_object *object;
    gnutls_datum_t datum;
    int rc;
    SECURITY_STATUS ret;

    if ((ret = begin_session( params->session, &object ))) return ret;
    rc = pgnutls_session_channel_binding(object->session, GNUTLS_CB_TLS_UNIQUE, &datum);
    if (rc)
    {
        pgnutls_perror(rc);
        end_session( object );
        return SEC_E_INTERNAL_ERROR;
    }
    if (params->buffer && *params->bufsize >= datum.size)
    {
        memcpy( params->buffer, datum.data, datum.size );
        ret = SEC_E_OK;
    }
    else ret = SEC_E_BUFFER_TOO_SMALL;

    *params->bufsize = datum.size;
    free(datum.data);
    end_session( object );
    return ret;
}

static NTSTATUS schan_get_key_signature_algorithm( void *args )
{
    const struct session_params *params = args;
    struct schan_session_object *object;
    gnutls_kx_algorithm_t kx;
    NTSTATUS status, ret;

    if ((status = begin_session( params->session, &object ))) return status;
    kx = pgnutls_kx_get( object->session );
    TRACE("(%p)\n", object->session);

    switch (kx)
    {
    case GNUTLS_KX_UNKNOWN: ret = 0; break;
    case GNUTLS_KX_RSA:
    case GNUTLS_KX_RSA_EXPORT:
    case GNUTLS_KX_DHE_RSA:
    case GNUTLS_KX_ECDHE_RSA: ret = CALG_RSA_SIGN; break;
    case GNUTLS_KX_ECDHE_ECDSA: ret = CALG_ECDSA; break;
    default:
        FIXME("unknown algorithm %d\n", kx);
        ret = 0;
    }
    end_session( object );
    return ret;
}

static NTSTATUS schan_get_session_peer_certificate( void *args )
{
    const struct get_session_peer_certificate_params *params = args;
    struct schan_session_object *object;
    const gnutls_datum_t *datum;
    unsigned int i, size, count;
    BYTE *ptr;
    ULONG *sizes;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    if (!(datum = pgnutls_certificate_get_peers(object->session, &count)))
    {
        status = SEC_E_INTERNAL_ERROR;
        goto done;
    }

    if (count > ~(unsigned int)0 / sizeof(*sizes))
    {
        status = SEC_E_INTERNAL_ERROR;
        goto done;
    }
    size = count * sizeof(*sizes);
    for (i = 0; i < count; i++)
    {
        if (datum[i].size > ~(unsigned int)0 - size)
        {
            status = SEC_E_INTERNAL_ERROR;
            goto done;
        }
        size += datum[i].size;
    }

    if (!params->buffer || *params->bufsize < size)
    {
        *params->bufsize = size;
        status = SEC_E_BUFFER_TOO_SMALL;
        goto done;
    }
    sizes = (ULONG *)params->buffer;
    ptr = params->buffer + count * sizeof(*sizes);
    for (i = 0; i < count; i++)
    {
        sizes[i] = datum[i].size;
        memcpy(ptr, datum[i].data, datum[i].size);
        ptr += datum[i].size;
    }

    *params->bufsize = size;
    *params->retcount = count;
    status = SEC_E_OK;
done:
    end_session( object );
    return status;
}

static NTSTATUS send_session( struct schan_session_object *object,
                              const struct send_params *params )
{
    gnutls_session_t s = object->session;
    struct schan_transport *t = &object->transport;
    SSIZE_T ret, total = 0;

    init_schan_buffers(&t->out, params->output);

    for (;;)
    {
        ret = pgnutls_record_send(s, (const char *)params->buffer + total, params->length - total);
        if (ret >= 0)
        {
            total += ret;
            TRACE( "sent %ld now %ld/%u\n", ret, total, (unsigned)params->length );
            if (total == params->length) break;
        }
        else if (ret == GNUTLS_E_AGAIN)
        {
            SIZE_T count = 0;

            if (get_buffer(&t->out, &count)) continue;
            return SEC_I_CONTINUE_NEEDED;
        }
        else
        {
            pgnutls_perror(ret);
            return SEC_E_INTERNAL_ERROR;
        }
    }

    *params->output_buffer_idx = t->out.current_buffer_idx;
    *params->output_offset = t->out.offset;
    return SEC_E_OK;
}

static NTSTATUS schan_send( void *args )
{
    const struct send_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    status = send_session( object, params );
    end_session( object );
    return status;
}

static NTSTATUS recv_session( struct schan_session_object *object,
                              const struct recv_params *params, SIZE_T *published,
                              BOOL *length_set )
{
    gnutls_session_t s = object->session;
    struct schan_transport *t = &object->transport;
    size_t data_size = *params->length;
    size_t received = 0;
    ssize_t ret;
    SECURITY_STATUS status = SEC_E_OK;

    if (published) *published = 0;
    if (length_set) *length_set = FALSE;

    init_schan_buffers(&t->in, params->input);
    t->in.limit = params->input_size;

    while (received < data_size)
    {
        ret = pgnutls_record_recv(s, (char *)params->buffer + received, data_size - received);

        if (ret > 0) received += ret;
        else if (!ret) break;
        else if (ret == GNUTLS_E_AGAIN)
        {
            SIZE_T count = 0;

            if (!get_buffer(&t->in, &count)) break;
        }
        else if (ret == GNUTLS_E_REHANDSHAKE)
        {
            TRACE("Rehandshake requested\n");
            status = SEC_I_RENEGOTIATE;
            break;
        }
        else
        {
            pgnutls_perror(ret);
            if (published) *published = received;
            return SEC_E_INTERNAL_ERROR;
        }
    }

    *params->length = received;
    if (published) *published = received;
    if (length_set) *length_set = TRUE;
    return status;
}

static NTSTATUS schan_recv( void *args )
{
    const struct recv_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    status = recv_session( object, params, NULL, NULL );
    end_session( object );
    return status;
}

static unsigned int parse_alpn_protocol_list(unsigned char *buffer, unsigned int buflen, gnutls_datum_t *list)
{
    unsigned int len, offset = 0, count = 0;

    while (buflen)
    {
        len = buffer[offset++];
        buflen--;
        if (!len || len > buflen) return 0;
        if (list)
        {
            list[count].data = &buffer[offset];
            list[count].size = len;
        }
        buflen -= len;
        offset += len;
        count++;
    }

    return count;
}

static NTSTATUS set_application_protocols_session( struct schan_session_object *object,
                                                    const struct set_application_protocols_params *params )
{
    gnutls_session_t s = object->session;
    unsigned int extension_len, extension, count = 0, offset = 0;
    unsigned short list_len;
    gnutls_datum_t *protocols;
    int ret;

    if (sizeof(extension_len) > params->buflen) return STATUS_INVALID_PARAMETER;
    extension_len = *(unsigned int *)&params->buffer[offset];
    offset += sizeof(extension_len);

    if (offset + sizeof(extension) > params->buflen) return STATUS_INVALID_PARAMETER;
    extension = *(unsigned int *)&params->buffer[offset];
    if (extension != SecApplicationProtocolNegotiationExt_ALPN)
    {
        FIXME("extension %u not supported\n", extension);
        return STATUS_NOT_SUPPORTED;
    }
    offset += sizeof(extension);

    if (offset + sizeof(list_len) > params->buflen) return STATUS_INVALID_PARAMETER;
    list_len = *(unsigned short *)&params->buffer[offset];
    offset += sizeof(list_len);

    if (offset + list_len > params->buflen) return STATUS_INVALID_PARAMETER;
    count = parse_alpn_protocol_list(&params->buffer[offset], list_len, NULL);
    if (!count || !(protocols = malloc(count * sizeof(*protocols)))) return STATUS_NO_MEMORY;

    parse_alpn_protocol_list(&params->buffer[offset], list_len, protocols);
    if ((ret = pgnutls_alpn_set_protocols(s, protocols, count,
                                           GNUTLS_ALPN_SERVER_PRECEDENCE)) < 0)
    {
        pgnutls_perror(ret);
        free(protocols);
        return STATUS_INTERNAL_ERROR;
    }

    free(protocols);
    return STATUS_SUCCESS;
}

static NTSTATUS schan_set_application_protocols( void *args )
{
    const struct set_application_protocols_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    status = set_application_protocols_session( object, params );
    end_session( object );
    return status;
}

static NTSTATUS schan_get_application_protocol( void *args )
{
    const struct get_application_protocol_params *params = args;
    struct schan_session_object *object;
    SecPkgContext_ApplicationProtocol *protocol = params->protocol;
    gnutls_datum_t selected;

    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    memset(protocol, 0, sizeof(*protocol));
    if (pgnutls_alpn_get_selected_protocol(object->session, &selected) < 0)
    {
        end_session( object );
        return SEC_E_OK;
    }

    if (selected.size <= sizeof(protocol->ProtocolId))
    {
        protocol->ProtoNegoStatus = SecApplicationProtocolNegotiationStatus_Success;
        protocol->ProtoNegoExt    = SecApplicationProtocolNegotiationExt_ALPN;
        protocol->ProtocolIdSize  = selected.size;
        memcpy(protocol->ProtocolId, selected.data, selected.size);
        TRACE("returning %s\n", wine_dbgstr_an((const char *)selected.data, selected.size));
    }
    end_session( object );
    return SEC_E_OK;
}

static NTSTATUS schan_set_dtls_mtu( void *args )
{
    const struct set_dtls_mtu_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    pgnutls_dtls_set_mtu(object->session, params->mtu);
    TRACE("MTU set to %u\n", params->mtu);
    end_session( object );
    return SEC_E_OK;
}

static NTSTATUS schan_set_dtls_timeouts( void *args )
{
    const struct set_dtls_timeouts_params *params = args;
    struct schan_session_object *object;
    NTSTATUS status;

    if ((status = begin_session( params->session, &object ))) return status;
    pgnutls_dtls_set_timeouts(object->session, params->retrans_timeout, params->total_timeout);
    end_session( object );
    return SEC_E_OK;
}

static BOOL copy_rsa_component( gnutls_datum_t *component, BYTE **dst, SIZE_T *capacity,
                                const BYTE *src, ULONG len )
{
    SIZE_T prefix = len && (src[0] & 0x80);

    if ((SIZE_T)len + prefix > *capacity) return FALSE;
    component->data = *dst;
    component->size = len + prefix;
    if (prefix) *(*dst)++ = 0;
    if (len)
    {
        memcpy( *dst, src, len );
        *dst += len;
    }
    *capacity -= len + prefix;
    return TRUE;
}

struct x509_key_snapshot
{
    BYTE *data;
    gnutls_datum_t m, e, d, p, q, u, e1, e2;
};

static BOOL validate_x509_key_header( ULONG key_size, const BCRYPT_RSAKEY_BLOB *hdr,
                                      SIZE_T *component_size )
{
    SIZE_T remaining, needed;

    if (key_size < sizeof(*hdr)) return FALSE;
    if (hdr->Magic != BCRYPT_RSAFULLPRIVATE_MAGIC)
    {
        TRACE("unexpected magic %#x\n", (unsigned)hdr->Magic);
        return FALSE;
    }

    TRACE("BCRYPT RSA key bitlen %u cbExp %u cbMod %u cbP1 %u cbP2 %u\n",
          (unsigned)hdr->BitLength, (unsigned)hdr->cbPublicExp, (unsigned)hdr->cbModulus,
          (unsigned)hdr->cbPrime1, (unsigned)hdr->cbPrime2);

    remaining = key_size - sizeof(*hdr);
    needed = hdr->cbPublicExp;
    if (needed > remaining || hdr->cbModulus > (remaining - needed) / 2 ||
        hdr->cbPrime1 > (remaining - needed - 2 * (SIZE_T)hdr->cbModulus) / 3 ||
        hdr->cbPrime2 > (remaining - needed - 2 * (SIZE_T)hdr->cbModulus -
                          3 * (SIZE_T)hdr->cbPrime1) / 2)
        return FALSE;
    needed += 2 * (SIZE_T)hdr->cbModulus + 3 * (SIZE_T)hdr->cbPrime1 +
              2 * (SIZE_T)hdr->cbPrime2;
    if (needed > remaining) return FALSE;
    if (component_size) *component_size = needed;
    return TRUE;
}

static BOOL capture_x509_key( SIZE_T component_size, const BYTE *key_blob,
                              struct x509_key_snapshot *snapshot )
{
    const BCRYPT_RSAKEY_BLOB *hdr = (const BCRYPT_RSAKEY_BLOB *)key_blob;
    const BYTE *src = (const BYTE *)(hdr + 1);
    BYTE *dst;
    SIZE_T capacity;

    memset( snapshot, 0, sizeof(*snapshot) );
    if (!(snapshot->data = malloc( component_size + 8 ))) return FALSE;
    dst = snapshot->data;
    capacity = component_size + 8;

    /* BCRYPT blob: PublicExp, Modulus, Prime1, Prime2, Exponent1, Exponent2, Coefficient, PrivateExponent */
    if (!copy_rsa_component( &snapshot->e, &dst, &capacity, src, hdr->cbPublicExp )) goto error;
    src += hdr->cbPublicExp;
    if (!copy_rsa_component( &snapshot->m, &dst, &capacity, src, hdr->cbModulus )) goto error;
    src += hdr->cbModulus;
    if (!copy_rsa_component( &snapshot->p, &dst, &capacity, src, hdr->cbPrime1 )) goto error;
    src += hdr->cbPrime1;
    if (!copy_rsa_component( &snapshot->q, &dst, &capacity, src, hdr->cbPrime2 )) goto error;
    src += hdr->cbPrime2;
    if (!copy_rsa_component( &snapshot->e1, &dst, &capacity, src, hdr->cbPrime1 )) goto error;
    src += hdr->cbPrime1;
    if (!copy_rsa_component( &snapshot->e2, &dst, &capacity, src, hdr->cbPrime2 )) goto error;
    src += hdr->cbPrime2;
    if (!copy_rsa_component( &snapshot->u, &dst, &capacity, src, hdr->cbPrime1 )) goto error;
    src += hdr->cbPrime1;
    if (!copy_rsa_component( &snapshot->d, &dst, &capacity, src, hdr->cbModulus )) goto error;
    return TRUE;

error:
    free( snapshot->data );
    snapshot->data = NULL;
    return FALSE;
}

static gnutls_x509_privkey_t import_x509_key( const struct x509_key_snapshot *snapshot )
{
    gnutls_privkey_t key = NULL;
    gnutls_x509_privkey_t x509key = NULL;
    int ret;

    if ((ret = pgnutls_privkey_init(&key)) < 0)
    {
        pgnutls_perror(ret);
        return NULL;
    }
    if (((ret = pgnutls_privkey_import_rsa_raw(key, &snapshot->m, &snapshot->e,
                                               &snapshot->d, &snapshot->p, &snapshot->q,
                                               &snapshot->u, &snapshot->e1,
                                               &snapshot->e2)) < 0) ||
        (ret = pgnutls_privkey_export_x509(key, &x509key)) < 0)
    {
        pgnutls_perror(ret);
        pgnutls_privkey_deinit(key);
        return NULL;
    }
    pgnutls_privkey_deinit(key);
    return x509key;
}

/* BCRYPT_RSAKEY_BLOB layout: already big-endian, matching GnuTLS expectations. */
static gnutls_x509_privkey_t get_x509_key(ULONG key_size, const BYTE *key_blob)
{
    const BCRYPT_RSAKEY_BLOB *hdr = (const BCRYPT_RSAKEY_BLOB *)key_blob;
    struct x509_key_snapshot snapshot;
    gnutls_x509_privkey_t x509key;
    SIZE_T component_size;

    if (!validate_x509_key_header( key_size, hdr, &component_size ) ||
        !capture_x509_key( component_size, key_blob, &snapshot )) return NULL;
    x509key = import_x509_key( &snapshot );
    free( snapshot.data );
    return x509key;
}

static gnutls_x509_crt_t get_x509_crt(const struct allocate_certificate_credentials_params *params)
{
    gnutls_datum_t data;
    gnutls_x509_crt_t crt;
    int ret;

    if (params->cert_encoding != X509_ASN_ENCODING)
    {
        FIXME("encoding type %u not supported\n", (unsigned)params->cert_encoding);
        return NULL;
    }

    if ((ret = pgnutls_x509_crt_init(&crt)) < 0)
    {
        pgnutls_perror(ret);
        return NULL;
    }

    data.data = params->cert_blob;
    data.size = params->cert_size;
    if ((ret = pgnutls_x509_crt_import(crt, &data, GNUTLS_X509_FMT_DER)) < 0)
    {
        pgnutls_perror(ret);
        pgnutls_x509_crt_deinit(crt);
        return NULL;
    }

    return crt;
}

static NTSTATUS publish_certificate_credentials( gnutls_certificate_credentials_t creds,
                                                 UINT64 *token )
{
    struct schan_credential_object *object;
    NTSTATUS status;

    if (!(object = calloc( 1, sizeof(*object) )))
    {
        pgnutls_certificate_free_credentials(creds);
        return STATUS_NO_MEMORY;
    }
    object->credentials = creds;
    if ((status = publish_credential( object, token ))) destroy_credential( object );
    return status;
}

static NTSTATUS finish_allocate_certificate_credentials(
    const struct allocate_certificate_credentials_params *params,
    gnutls_certificate_credentials_t creds, UINT64 *token )
{
    gnutls_x509_crt_t crt;
    gnutls_x509_privkey_t key;
    int ret;

    if (params->cert_blob)
    {
        if (!(crt = get_x509_crt(params)))
        {
            pgnutls_certificate_free_credentials(creds);
            return STATUS_INTERNAL_ERROR;
        }
        if (!(key = get_x509_key(params->key_size, params->key_blob)))
        {
            pgnutls_x509_crt_deinit(crt);
            pgnutls_certificate_free_credentials(creds);
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_certificate_set_x509_key(creds, &crt, 1, key);
        pgnutls_x509_privkey_deinit(key);
        pgnutls_x509_crt_deinit(crt);
        if (ret != GNUTLS_E_SUCCESS)
        {
            pgnutls_perror(ret);
            pgnutls_certificate_free_credentials(creds);
            return STATUS_INTERNAL_ERROR;
        }
    }
    return publish_certificate_credentials( creds, token );
}

static NTSTATUS schan_allocate_certificate_credentials( void *args )
{
    const struct allocate_certificate_credentials_params *params = args;
    gnutls_certificate_credentials_t creds;
    UINT64 token;
    NTSTATUS status;
    int ret;

    if ((status = schan_provider_enter())) return status;
    if ((ret = pgnutls_certificate_allocate_credentials(&creds)) != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(ret);
        status = STATUS_INTERNAL_ERROR;
    }
    else if (!(status = finish_allocate_certificate_credentials( params, creds, &token )))
        params->c->credentials = token;
    schan_provider_leave();
    return status;
}

static NTSTATUS schan_free_certificate_credentials( void *args )
{
    const struct free_certificate_credentials_params *params = args;
    struct schan_credential_object *object;
    UINT64 token = params->c->credentials;
    BOOL destroy;
    NTSTATUS status;

    if ((status = schan_provider_enter())) return status;
    if ((token & SCHAN_TOKEN_MASK) != SCHAN_CREDENTIAL_TOKEN_TAG)
    {
        schan_provider_leave();
        return STATUS_INVALID_HANDLE;
    }
    pthread_mutex_lock( &schan_registry_mutex );
    if (!(object = find_credential( token )) || object->closing)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        schan_provider_leave();
        return STATUS_INVALID_HANDLE;
    }
    object->closing = TRUE;
    list_remove( &object->entry );
    while (object->active) pthread_cond_wait( &schan_registry_cond, &schan_registry_mutex );
    object->close_complete = TRUE;
    destroy = !object->session_refs;
    pthread_mutex_unlock( &schan_registry_mutex );
    if (destroy) destroy_credential( object );
    schan_provider_leave();
    return STATUS_SUCCESS;
}

static void gnutls_log(int level, const char *msg)
{
    TRACE("<%d> %s", level, msg);
}

static NTSTATUS process_attach( void *args )
{
    int ret;

    pthread_mutex_lock( &schan_registry_mutex );
    if (schan_attached)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_SUCCESS;
    }
    if (schan_draining)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_DEVICE_BUSY;
    }
    schan_draining = TRUE;
    pthread_mutex_unlock( &schan_registry_mutex );

    if ((system_priority_file = getenv("GNUTLS_SYSTEM_PRIORITY_FILE")))
    {
        TRACE("GNUTLS_SYSTEM_PRIORITY_FILE is %s.\n", debugstr_a(system_priority_file));
    }
    else
    {
        WARN("Setting GNUTLS_SYSTEM_PRIORITY_FILE to \"/dev/null\".\n");
        setenv("GNUTLS_SYSTEM_PRIORITY_FILE", "/dev/null", 0);
    }

    libgnutls_handle = dlopen(SONAME_LIBGNUTLS, RTLD_NOW);
    if (!libgnutls_handle)
    {
        ERR_(winediag)("Failed to load libgnutls, secure connections will not be available: %s\n", dlerror());
        pthread_mutex_lock( &schan_registry_mutex );
        schan_draining = FALSE;
        pthread_cond_broadcast( &schan_registry_cond );
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_DLL_NOT_FOUND;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = dlsym(libgnutls_handle, #f))) \
    { \
        ERR("Failed to load %s\n", #f); \
        goto fail; \
    }

    LOAD_FUNCPTR(gnutls_alert_get)
    LOAD_FUNCPTR(gnutls_alert_get_name)
    LOAD_FUNCPTR(gnutls_alert_send)
    LOAD_FUNCPTR(gnutls_certificate_allocate_credentials)
    LOAD_FUNCPTR(gnutls_certificate_free_credentials)
    LOAD_FUNCPTR(gnutls_certificate_get_peers)
    LOAD_FUNCPTR(gnutls_certificate_set_x509_key)
    LOAD_FUNCPTR(gnutls_cipher_get)
    LOAD_FUNCPTR(gnutls_cipher_get_key_size)
    LOAD_FUNCPTR(gnutls_credentials_set)
    LOAD_FUNCPTR(gnutls_deinit)
    LOAD_FUNCPTR(gnutls_global_deinit)
    LOAD_FUNCPTR(gnutls_global_init)
    LOAD_FUNCPTR(gnutls_global_set_log_function)
    LOAD_FUNCPTR(gnutls_global_set_log_level)
    LOAD_FUNCPTR(gnutls_handshake)
    LOAD_FUNCPTR(gnutls_init)
    LOAD_FUNCPTR(gnutls_kx_get)
    LOAD_FUNCPTR(gnutls_mac_get)
    LOAD_FUNCPTR(gnutls_mac_get_key_size)
    LOAD_FUNCPTR(gnutls_perror)
    LOAD_FUNCPTR(gnutls_protocol_get_version)
    LOAD_FUNCPTR(gnutls_priority_set_direct)
    LOAD_FUNCPTR(gnutls_privkey_deinit)
    LOAD_FUNCPTR(gnutls_privkey_init)
    LOAD_FUNCPTR(gnutls_record_get_max_size);
    LOAD_FUNCPTR(gnutls_record_recv);
    LOAD_FUNCPTR(gnutls_record_send);
    LOAD_FUNCPTR(gnutls_server_name_set)
    LOAD_FUNCPTR(gnutls_session_channel_binding)
    LOAD_FUNCPTR(gnutls_set_default_priority)
    LOAD_FUNCPTR(gnutls_transport_get_ptr)
    LOAD_FUNCPTR(gnutls_transport_set_errno)
    LOAD_FUNCPTR(gnutls_transport_set_ptr)
    LOAD_FUNCPTR(gnutls_transport_set_pull_function)
    LOAD_FUNCPTR(gnutls_transport_set_push_function)
    LOAD_FUNCPTR(gnutls_x509_crt_deinit)
    LOAD_FUNCPTR(gnutls_x509_crt_import)
    LOAD_FUNCPTR(gnutls_x509_crt_init)
    LOAD_FUNCPTR(gnutls_x509_privkey_deinit)
#undef LOAD_FUNCPTR

    if (!(pgnutls_cipher_get_block_size = dlsym(libgnutls_handle, "gnutls_cipher_get_block_size")))
    {
        WARN("gnutls_cipher_get_block_size not found\n");
        pgnutls_cipher_get_block_size = compat_cipher_get_block_size;
    }
    if (!(pgnutls_transport_set_pull_timeout_function = dlsym(libgnutls_handle, "gnutls_transport_set_pull_timeout_function")))
    {
        WARN("gnutls_transport_set_pull_timeout_function not found\n");
        pgnutls_transport_set_pull_timeout_function = compat_gnutls_transport_set_pull_timeout_function;
    }
    if (!(pgnutls_alpn_set_protocols = dlsym(libgnutls_handle, "gnutls_alpn_set_protocols")))
    {
        WARN("gnutls_alpn_set_protocols not found\n");
        pgnutls_alpn_set_protocols = compat_gnutls_alpn_set_protocols;
    }
    if (!(pgnutls_alpn_get_selected_protocol = dlsym(libgnutls_handle, "gnutls_alpn_get_selected_protocol")))
    {
        WARN("gnutls_alpn_get_selected_protocol not found\n");
        pgnutls_alpn_get_selected_protocol = compat_gnutls_alpn_get_selected_protocol;
    }
    if (!(pgnutls_dtls_set_mtu = dlsym(libgnutls_handle, "gnutls_dtls_set_mtu")))
    {
        WARN("gnutls_dtls_set_mtu not found\n");
        pgnutls_dtls_set_mtu = compat_gnutls_dtls_set_mtu;
    }
    if (!(pgnutls_dtls_set_timeouts = dlsym(libgnutls_handle, "gnutls_dtls_set_timeouts")))
    {
        WARN("gnutls_dtls_set_timeouts not found\n");
        pgnutls_dtls_set_timeouts = compat_gnutls_dtls_set_timeouts;
    }
    if (!(pgnutls_privkey_export_x509 = dlsym(libgnutls_handle, "gnutls_privkey_export_x509")))
    {
        WARN("gnutls_privkey_export_x509 not found\n");
        pgnutls_privkey_export_x509 = compat_gnutls_privkey_export_x509;
    }
    if (!(pgnutls_privkey_import_rsa_raw = dlsym(libgnutls_handle, "gnutls_privkey_import_rsa_raw")))
    {
        WARN("gnutls_privkey_import_rsa_raw not found\n");
        pgnutls_privkey_import_rsa_raw = compat_gnutls_privkey_import_rsa_raw;
    }

    ret = pgnutls_global_init();
    if (ret != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(ret);
        goto fail;
    }

    if (TRACE_ON(secur32))
    {
        char *env = getenv("GNUTLS_DEBUG_LEVEL");
        int level = env ? atoi(env) : 4;
        pgnutls_global_set_log_level(level);
        pgnutls_global_set_log_function(gnutls_log);
    }

    check_supported_protocols(client_protocol_priority_flags, ARRAYSIZE(client_protocol_priority_flags), FALSE);
    check_supported_protocols(server_protocol_priority_flags, ARRAYSIZE(server_protocol_priority_flags), TRUE);
    pthread_mutex_lock( &schan_registry_mutex );
    schan_attached = TRUE;
    schan_draining = FALSE;
    pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
    return STATUS_SUCCESS;

fail:
    dlclose(libgnutls_handle);
    libgnutls_handle = NULL;
    pthread_mutex_lock( &schan_registry_mutex );
    schan_draining = FALSE;
    pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
    return STATUS_DLL_NOT_FOUND;
}

static NTSTATUS process_detach( void *args )
{
    struct list sessions = LIST_INIT(sessions), credentials = LIST_INIT(credentials);
    struct schan_session_object *session;
    struct schan_credential_object *credential;

    pthread_mutex_lock( &schan_registry_mutex );
    if (!schan_attached)
    {
        pthread_mutex_unlock( &schan_registry_mutex );
        return STATUS_SUCCESS;
    }
    schan_draining = TRUE;
    while (schan_calls) pthread_cond_wait( &schan_registry_cond, &schan_registry_mutex );
    while (!list_empty( &schan_session_list ))
    {
        struct list *entry = list_head( &schan_session_list );
        list_remove( entry );
        list_add_tail( &sessions, entry );
    }
    while (!list_empty( &schan_credential_list ))
    {
        struct list *entry = list_head( &schan_credential_list );
        list_remove( entry );
        list_add_tail( &credentials, entry );
    }
    pthread_mutex_unlock( &schan_registry_mutex );

    while (!list_empty( &sessions ))
    {
        session = LIST_ENTRY( list_head( &sessions ), struct schan_session_object, entry );
        list_remove( &session->entry );
        destroy_session( session );
    }
    while (!list_empty( &credentials ))
    {
        credential = LIST_ENTRY( list_head( &credentials ), struct schan_credential_object, entry );
        list_remove( &credential->entry );
        assert( !credential->active && !credential->session_refs );
        destroy_credential( credential );
    }
    if (libgnutls_handle)
    {
        if (TRACE_ON(secur32))
            pgnutls_global_set_log_function(NULL);
        pgnutls_global_deinit();
        dlclose(libgnutls_handle);
        libgnutls_handle = NULL;
    }
    pthread_mutex_lock( &schan_registry_mutex );
    schan_attached = FALSE;
    schan_draining = FALSE;
    pthread_cond_broadcast( &schan_registry_cond );
    pthread_mutex_unlock( &schan_registry_mutex );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_attach,
    process_detach,
    schan_allocate_certificate_credentials,
    schan_create_session,
    schan_dispose_session,
    schan_free_certificate_credentials,
    schan_get_application_protocol,
    schan_get_cipher_info,
    schan_get_connection_info,
    schan_get_enabled_protocols,
    schan_get_key_signature_algorithm,
    schan_get_max_message_size,
    schan_get_session_cipher_block_size,
    schan_get_session_peer_certificate,
    schan_get_unique_channel_binding,
    schan_handshake,
    schan_recv,
    schan_send,
    schan_set_application_protocols,
    schan_set_dtls_mtu,
    schan_set_session_target,
    schan_set_dtls_timeouts,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);

#ifdef _WIN64

typedef ULONG PTR32;

typedef struct SecBufferDesc32
{
    ULONG ulVersion;
    ULONG cBuffers;
    PTR32 pBuffers;
} SecBufferDesc32;

typedef struct SecBuffer32
{
    ULONG cbBuffer;
    ULONG BufferType;
    PTR32 pvBuffer;
} SecBuffer32;

struct allocate_certificate_credentials_params32
{
    PTR32 c;
    ULONG cert_encoding;
    ULONG cert_size;
    PTR32 cert_blob;
    ULONG key_size;
    PTR32 key_blob;
};

struct create_session_params32 { PTR32 cred, session; };
struct free_certificate_credentials_params32 { PTR32 c; };
struct session_output_params32 { schan_session session; PTR32 output; };
struct peer_certificate_params32
{
    schan_session session;
    PTR32 buffer, bufsize, retcount;
};
struct unique_binding_params32 { schan_session session; PTR32 buffer, bufsize; };
struct handshake_params32
{
    schan_session session;
    PTR32 input;
    ULONG input_size;
    PTR32 output, input_offset, output_buffer_idx, output_offset;
    enum control_token control_token;
    unsigned int alert_type, alert_number;
};
struct recv_params32
{
    schan_session session;
    PTR32 input;
    ULONG input_size;
    PTR32 buffer, length;
};
struct send_params32
{
    schan_session session;
    PTR32 output, buffer;
    ULONG length;
    PTR32 output_buffer_idx, output_offset;
};
struct set_application_protocols_params32
{
    schan_session session;
    PTR32 buffer;
    unsigned int buflen;
};
struct set_session_target_params32 { schan_session session; PTR32 target; };

C_ASSERT( sizeof(struct allocate_certificate_credentials_params32) == 24 );
C_ASSERT( sizeof(struct create_session_params32) == 8 );
C_ASSERT( sizeof(struct session_params) == 8 );
C_ASSERT( sizeof(struct free_certificate_credentials_params32) == 4 );
C_ASSERT( sizeof(struct session_output_params32) == 16 );
C_ASSERT( sizeof(struct peer_certificate_params32) == 24 );
C_ASSERT( sizeof(struct unique_binding_params32) == 16 );
C_ASSERT( sizeof(struct handshake_params32) == 48 );
C_ASSERT( sizeof(struct recv_params32) == 24 );
C_ASSERT( sizeof(struct send_params32) == 32 );
C_ASSERT( sizeof(struct set_application_protocols_params32) == 16 );
C_ASSERT( sizeof(struct set_dtls_mtu_params) == 16 );
C_ASSERT( sizeof(struct set_session_target_params32) == 16 );
C_ASSERT( sizeof(struct set_dtls_timeouts_params) == 16 );

static NTSTATUS wow64_schan_check_call( UINT32 size, UINT32 flags )
{
    struct ntdll_wow64_unixlib_call_context context;
    NTSTATUS status;

    if ((status = ntdll_wow64_get_unixlib_call_context( &context ))) return status;
    if (context.args_size != size || context.flags != flags || (!context.guest_args && size))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_schan_guest_range( PTR32 address, SIZE_T size, void **host )
{
    NTSTATUS status;

    if (size && (!address || size - 1 > ~(ULONG)0 - address)) return STATUS_ACCESS_VIOLATION;
    if ((status = ntdll_wow64_guest32_to_host( address, host ))) return status;
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_schan_copy_from_guest( PTR32 address, void *buffer, SIZE_T size )
{
    void *host;
    NTSTATUS status;

    if ((status = wow64_schan_guest_range( address, size, &host ))) return status;
    return ntdll_wow64_copy_from_user( buffer, host, size );
}

static NTSTATUS wow64_schan_validate_guest_range( PTR32 address, SIZE_T size, BOOL output )
{
    void *host;
    NTSTATUS status;

    if ((status = wow64_schan_guest_range( address, size, &host ))) return status;
    if (output) return ntdll_wow64_probe_user_write( host, size );
    return ntdll_wow64_probe_user_read( host, size );
}

static NTSTATUS wow64_schan_capture_rsa_component( PTR32 address, SIZE_T *offset,
                                                   gnutls_datum_t *component,
                                                   BYTE **dst, SIZE_T *capacity,
                                                   ULONG len )
{
    SIZE_T prefix = 0;
    NTSTATUS status;

    if (len > *capacity) return STATUS_INVALID_PARAMETER;
    component->data = *dst;
    component->size = len;
    if (len)
    {
        if (*offset > ~(ULONG)0 || address > ~(ULONG)0 - (ULONG)*offset)
            return STATUS_ACCESS_VIOLATION;
        if ((status = wow64_schan_copy_from_guest( address + (ULONG)*offset,
                                                   *dst, len ))) return status;
        if ((*dst)[0] & 0x80)
        {
            if (len == *capacity) return STATUS_INVALID_PARAMETER;
            memmove( *dst + 1, *dst, len );
            **dst = 0;
            prefix = 1;
            component->size++;
        }
    }
    *dst += len + prefix;
    *capacity -= len + prefix;
    *offset += len;
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_schan_capture_x509_key( PTR32 address, SIZE_T component_size,
                                              const BCRYPT_RSAKEY_BLOB *hdr,
                                              struct x509_key_snapshot *snapshot )
{
    BYTE *dst;
    SIZE_T capacity, offset = sizeof(*hdr);
    NTSTATUS status;

    memset( snapshot, 0, sizeof(*snapshot) );
    if (!(snapshot->data = malloc( component_size + 8 ))) return STATUS_NO_MEMORY;
    dst = snapshot->data;
    capacity = component_size + 8;

#define CAPTURE_COMPONENT(component, size) \
    if ((status = wow64_schan_capture_rsa_component( address, &offset, \
            &snapshot->component, &dst, &capacity, size ))) goto error
    CAPTURE_COMPONENT( e, hdr->cbPublicExp );
    CAPTURE_COMPONENT( m, hdr->cbModulus );
    CAPTURE_COMPONENT( p, hdr->cbPrime1 );
    CAPTURE_COMPONENT( q, hdr->cbPrime2 );
    CAPTURE_COMPONENT( e1, hdr->cbPrime1 );
    CAPTURE_COMPONENT( e2, hdr->cbPrime2 );
    CAPTURE_COMPONENT( u, hdr->cbPrime1 );
    CAPTURE_COMPONENT( d, hdr->cbModulus );
#undef CAPTURE_COMPONENT
    return STATUS_SUCCESS;

error:
    free( snapshot->data );
    snapshot->data = NULL;
    return status;
}

static NTSTATUS wow64_schan_output_range( PTR32 address, const void *data, SIZE_T size,
                                          struct ntdll_wow64_user_write_range *range )
{
    NTSTATUS status;

    if ((status = wow64_schan_guest_range( address, size, &range->dst ))) return status;
    range->src = data;
    range->size = size;
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_schan_copy_cstr( PTR32 address, char **ret )
{
    char *buffer;
    unsigned int i;
    NTSTATUS status;

    *ret = NULL;
    if (!address) return STATUS_SUCCESS;
    if (!(buffer = malloc( 0x10000 ))) return STATUS_NO_MEMORY;
    for (i = 0; i < 0x10000; i++)
    {
        if (address > ~(ULONG)0 - i)
        {
            free( buffer );
            return STATUS_ACCESS_VIOLATION;
        }
        if ((status = wow64_schan_copy_from_guest( address + i, &buffer[i], 1 )))
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

struct wow64_schan_desc
{
    SecBufferDesc desc;
    SecBuffer buffers[4];
    SecBuffer32 buffers32[4];
    BYTE *storage[4];
};

/* The PE side stages authentication output in a 64 KiB token buffer.  TLS and
 * DTLS record plaintext is smaller, and the largest wire header is 13 bytes. */
#define SCHAN_WOW64_MAX_TOKEN_SIZE 0x10000
#define SCHAN_WOW64_MAX_RECORD_BUFFER_SIZE (0xffff + 13)

static void wow64_schan_free_desc( struct wow64_schan_desc *desc )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(desc->storage); i++)
    {
        free( desc->storage[i] );
        desc->storage[i] = NULL;
    }
}

static NTSTATUS wow64_schan_capture_desc( PTR32 address,
                                          struct wow64_schan_desc *snapshot )
{
    SecBufferDesc32 desc32;
    unsigned int i;
    NTSTATUS status;

    memset( snapshot, 0, sizeof(*snapshot) );
    snapshot->desc.pBuffers = snapshot->buffers;
    if ((status = wow64_schan_copy_from_guest( address, &desc32, sizeof(desc32) ))) return status;
    if (desc32.cBuffers > ARRAY_SIZE(snapshot->buffers)) return STATUS_INVALID_PARAMETER;
    snapshot->desc.ulVersion = desc32.ulVersion;
    snapshot->desc.cBuffers = desc32.cBuffers;
    if (desc32.cBuffers &&
        (status = wow64_schan_copy_from_guest( desc32.pBuffers, snapshot->buffers32,
                                               desc32.cBuffers * sizeof(SecBuffer32) )))
        return status;
    for (i = 0; i < desc32.cBuffers; i++)
    {
        snapshot->buffers[i].cbBuffer = snapshot->buffers32[i].cbBuffer;
        snapshot->buffers[i].BufferType = snapshot->buffers32[i].BufferType;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_schan_capture_desc_data( struct wow64_schan_desc *snapshot,
                                               BOOL input, SIZE_T input_limit,
                                               SIZE_T staging_limit )
{
    unsigned int i;
    SIZE_T size;
    NTSTATUS status;

    /* The transport consumes the descriptor sequentially, so cap total native
     * staging across all buffers rather than granting the limit to each one. */
    for (i = 0; i < snapshot->desc.cBuffers; i++)
    {
        size = snapshot->buffers[i].cbBuffer;
        if (input && size > input_limit) size = input_limit;
        if (input) input_limit -= size;
        if (input && size > staging_limit)
        {
            status = SEC_E_INVALID_TOKEN;
            goto error;
        }
        if (!input && size > staging_limit) size = staging_limit;
        snapshot->buffers[i].cbBuffer = size;
        if (!size) continue;
        if ((status = wow64_schan_validate_guest_range( snapshot->buffers32[i].pvBuffer,
                                                        size, !input ))) goto error;
        if (!(snapshot->storage[i] = malloc( size )))
        {
            status = STATUS_NO_MEMORY;
            goto error;
        }
        snapshot->buffers[i].pvBuffer = snapshot->storage[i];
        if (input && (status = wow64_schan_copy_from_guest( snapshot->buffers32[i].pvBuffer,
                                                            snapshot->storage[i],
                                                            size )))
            goto error;
        staging_limit -= size;
    }
    return STATUS_SUCCESS;
error:
    wow64_schan_free_desc( snapshot );
    return status;
}

static NTSTATUS wow64_schan_add_output_buffers( const struct wow64_schan_desc *desc,
                                                int index, SIZE_T offset,
                                                struct ntdll_wow64_user_write_range *ranges,
                                                ULONG *count )
{
    unsigned int i;
    SIZE_T size;
    NTSTATUS status;

    if (index < -1 || index >= (int)desc->desc.cBuffers) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < desc->desc.cBuffers && (int)i <= index; i++)
    {
        size = (int)i < index ? desc->buffers[i].cbBuffer : offset;
        if (size > desc->buffers[i].cbBuffer) return STATUS_INVALID_PARAMETER;
        if (!size) continue;
        if ((status = wow64_schan_output_range( desc->buffers32[i].pvBuffer,
                                                desc->storage[i], size, &ranges[*count] )))
            return status;
        (*count)++;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_schan_allocate_certificate_credentials( void *args )
{
    const struct allocate_certificate_credentials_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range range;
    struct allocate_certificate_credentials_params params = {0};
    gnutls_certificate_credentials_t creds;
    gnutls_x509_crt_t crt = NULL;
    gnutls_x509_privkey_t key = NULL;
    struct x509_key_snapshot key_snapshot = {0};
    BCRYPT_RSAKEY_BLOB key_header;
    gnutls_datum_t cert_data;
    BYTE *cert_blob = NULL;
    SIZE_T key_component_size;
    UINT64 token;
    NTSTATUS status;
    int ret;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = schan_provider_enter())) return status;
    if ((ret = pgnutls_certificate_allocate_credentials( &creds )) != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror( ret );
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }
    params.cert_encoding = params32->cert_encoding;
    params.cert_size = params32->cert_size;
    params.key_size = params32->key_size;
    if (params32->cert_blob)
    {
        if (params.cert_encoding != X509_ASN_ENCODING)
        {
            FIXME("encoding type %u not supported\n", (unsigned)params.cert_encoding);
            status = STATUS_INTERNAL_ERROR;
            goto free_creds;
        }
        if ((ret = pgnutls_x509_crt_init( &crt )) < 0)
        {
            pgnutls_perror( ret );
            status = STATUS_INTERNAL_ERROR;
            goto free_creds;
        }
        if (params.cert_size && !(cert_blob = malloc( params.cert_size )))
        {
            status = STATUS_NO_MEMORY;
            goto free_crt;
        }
        if ((status = wow64_schan_copy_from_guest( params32->cert_blob, cert_blob,
                                                   params.cert_size ))) goto free_crt;
        cert_data.data = cert_blob;
        cert_data.size = params.cert_size;
        if ((ret = pgnutls_x509_crt_import( crt, &cert_data,
                                            GNUTLS_X509_FMT_DER )) < 0)
        {
            pgnutls_perror( ret );
            status = STATUS_INTERNAL_ERROR;
            goto free_crt;
        }
        if (params.key_size < sizeof(key_header))
        {
            status = STATUS_INTERNAL_ERROR;
            goto free_crt;
        }
        if ((status = wow64_schan_copy_from_guest( params32->key_blob, &key_header,
                                                   sizeof(key_header) ))) goto free_crt;
        if (!validate_x509_key_header( params.key_size, &key_header,
                                       &key_component_size ))
        {
            status = STATUS_INTERNAL_ERROR;
            goto free_crt;
        }
        if ((status = wow64_schan_capture_x509_key( params32->key_blob,
                                                    key_component_size, &key_header,
                                                    &key_snapshot ))) goto free_crt;
        key = import_x509_key( &key_snapshot );
        free( key_snapshot.data );
        key_snapshot.data = NULL;
        if (!key)
        {
            status = STATUS_INTERNAL_ERROR;
            goto free_crt;
        }
        ret = pgnutls_certificate_set_x509_key( creds, &crt, 1, key );
        pgnutls_x509_privkey_deinit( key );
        key = NULL;
        pgnutls_x509_crt_deinit( crt );
        crt = NULL;
        if (ret != GNUTLS_E_SUCCESS)
        {
            pgnutls_perror( ret );
            status = STATUS_INTERNAL_ERROR;
            goto free_creds;
        }
    }
    if ((status = publish_certificate_credentials( creds, &token ))) goto done;
    if (params32->c > ~(ULONG)0 - offsetof(schan_credentials, credentials))
        status = STATUS_ACCESS_VIOLATION;
    else if (!(status = wow64_schan_output_range( params32->c + offsetof(schan_credentials, credentials),
                                             &token, sizeof(token), &range )))
        status = ntdll_wow64_atomic_writev( &range, 1 );
    if (status)
    {
        schan_credentials local = {0};
        local.credentials = token;
        schan_free_certificate_credentials( &(struct free_certificate_credentials_params){ &local } );
    }
    goto done;
free_crt:
    if (key) pgnutls_x509_privkey_deinit( key );
    if (crt) pgnutls_x509_crt_deinit( crt );
free_creds:
    pgnutls_certificate_free_credentials( creds );
done:
    free( key_snapshot.data );
    free( cert_blob );
    schan_provider_leave();
    return status;
}

static NTSTATUS wow64_schan_create_session( void *args )
{
    const struct create_session_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range range;
    schan_credentials credential;
    schan_session session = 0;
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = schan_provider_enter())) return status;
    if ((status = wow64_schan_copy_from_guest( params32->cred, &credential,
                                               sizeof(credential) ))) goto done;
    if ((status = wow64_schan_output_range( params32->session, &session,
                                            sizeof(session), &range )) ||
        (status = ntdll_wow64_atomic_writev( &range, 1 ))) goto done;
    if ((status = create_session_data( &credential, &session ))) goto done;
    if (!(status = wow64_schan_output_range( params32->session, &session,
                                             sizeof(session), &range )))
        status = ntdll_wow64_atomic_writev( &range, 1 );
    if (status) schan_dispose_session( &(struct session_params){ session } );
done:
    schan_provider_leave();
    return status;
}

static NTSTATUS wow64_schan_free_certificate_credentials( void *args )
{
    const struct free_certificate_credentials_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED;
    schan_credentials credential = {0};
    struct free_certificate_credentials_params params = { &credential };
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if (params32->c > ~(ULONG)0 - offsetof(schan_credentials, credentials))
        return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_schan_copy_from_guest(
            params32->c + offsetof(schan_credentials, credentials), &credential.credentials,
            sizeof(credential.credentials) ))) return status;
    return schan_free_certificate_credentials( &params );
}

static NTSTATUS wow64_schan_get_application_protocol( void *args )
{
    const struct session_output_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range range;
    SecPkgContext_ApplicationProtocol output = {0};
    struct get_application_protocol_params params = { params32->session, &output };
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = schan_get_application_protocol( &params ))) return status;
    if ((status = wow64_schan_output_range( params32->output, &output,
                                            sizeof(output), &range ))) return status;
    return ntdll_wow64_atomic_writev( &range, 1 );
}

static NTSTATUS wow64_schan_get_connection_info( void *args )
{
    const struct session_output_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range range;
    SecPkgContext_ConnectionInfo output = {0};
    struct get_connection_info_params params = { params32->session, &output };
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = schan_get_connection_info( &params ))) return status;
    if ((status = wow64_schan_output_range( params32->output, &output,
                                            sizeof(output), &range ))) return status;
    return ntdll_wow64_atomic_writev( &range, 1 );
}

static NTSTATUS wow64_schan_get_cipher_info( void *args )
{
    const struct session_output_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range range;
    SecPkgContext_CipherInfo output = {0}, original;
    struct get_cipher_info_params params = { params32->session, &output };
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = schan_get_cipher_info( &params ))) return status;
    if ((status = wow64_schan_copy_from_guest( params32->output, &original,
                                               sizeof(original) ))) return status;
    original.dwProtocol = output.dwProtocol;
    original.dwCipherSuite = output.dwCipherSuite;
    original.dwBaseCipherSuite = output.dwBaseCipherSuite;
    wcscpy( original.szCipherSuite, output.szCipherSuite );
    wcscpy( original.szCipher, output.szCipher );
    original.dwCipherLen = output.dwCipherLen;
    original.dwCipherBlockLen = output.dwCipherBlockLen;
    wcscpy( original.szHash, output.szHash );
    original.dwHashLen = output.dwHashLen;
    wcscpy( original.szExchange, output.szExchange );
    original.dwMinExchangeLen = output.dwMinExchangeLen;
    original.dwMaxExchangeLen = output.dwMaxExchangeLen;
    wcscpy( original.szCertificate, output.szCertificate );
    original.dwKeyType = output.dwKeyType;
    if ((status = wow64_schan_output_range( params32->output, &original,
                                            sizeof(original), &range ))) return status;
    return ntdll_wow64_atomic_writev( &range, 1 );
}

static NTSTATUS wow64_schan_get_session_peer_certificate( void *args )
{
    const struct peer_certificate_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range ranges[3];
    struct schan_session_object *object;
    const gnutls_datum_t *datum;
    unsigned int i, count, size;
    ULONG capacity, *sizes;
    BYTE *buffer = NULL, *ptr;
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    if (!(datum = pgnutls_certificate_get_peers( object->session, &count )))
    {
        status = SEC_E_INTERNAL_ERROR;
        goto unlock;
    }
    if (count > ~(unsigned int)0 / sizeof(*sizes))
    {
        status = SEC_E_INTERNAL_ERROR;
        goto unlock;
    }
    size = count * sizeof(*sizes);
    for (i = 0; i < count; i++)
    {
        if (datum[i].size > ~(unsigned int)0 - size)
        {
            status = SEC_E_INTERNAL_ERROR;
            goto unlock;
        }
        size += datum[i].size;
    }
    if ((status = wow64_schan_copy_from_guest( params32->bufsize, &capacity,
                                               sizeof(capacity) ))) goto unlock;
    if (!params32->buffer || capacity < size)
    {
        end_session( object );
        if ((status = wow64_schan_output_range( params32->bufsize, &size,
                                                sizeof(size), &ranges[0] ))) return status;
        status = ntdll_wow64_atomic_writev( ranges, 1 );
        return status ? status : SEC_E_BUFFER_TOO_SMALL;
    }
    if (size && !(buffer = malloc( size )))
    {
        status = STATUS_NO_MEMORY;
        goto unlock;
    }
    if (count)
    {
        sizes = (ULONG *)buffer;
        ptr = buffer + count * sizeof(*sizes);
        for (i = 0; i < count; i++)
        {
            sizes[i] = datum[i].size;
            memcpy( ptr, datum[i].data, datum[i].size );
            ptr += datum[i].size;
        }
    }
    end_session( object );
    if ((status = wow64_schan_output_range( params32->buffer, buffer, size, &ranges[0] )) ||
        (status = wow64_schan_output_range( params32->bufsize, &size, sizeof(size), &ranges[1] )) ||
        (status = wow64_schan_output_range( params32->retcount, &count, sizeof(count), &ranges[2] )))
        goto done;
    status = ntdll_wow64_atomic_writev( ranges, ARRAY_SIZE(ranges) );
    goto done;
unlock:
    end_session( object );
done:
    free( buffer );
    return status;
}

static NTSTATUS wow64_schan_get_unique_channel_binding( void *args )
{
    const struct unique_binding_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range ranges[2];
    struct schan_session_object *object;
    gnutls_datum_t datum = {0};
    ULONG capacity, size;
    NTSTATUS status;
    int ret;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    if ((ret = pgnutls_session_channel_binding( object->session, GNUTLS_CB_TLS_UNIQUE, &datum )))
    {
        pgnutls_perror( ret );
        status = SEC_E_INTERNAL_ERROR;
        goto unlock;
    }
    size = datum.size;
    if ((status = wow64_schan_copy_from_guest( params32->bufsize, &capacity,
                                               sizeof(capacity) ))) goto unlock;
    end_session( object );
    if (!params32->buffer || capacity < size)
    {
        if (!(status = wow64_schan_output_range( params32->bufsize, &size,
                                                 sizeof(size), &ranges[0] )))
            status = ntdll_wow64_atomic_writev( ranges, 1 );
        if (!status) status = SEC_E_BUFFER_TOO_SMALL;
        goto done;
    }
    if ((status = wow64_schan_output_range( params32->buffer, datum.data,
                                            size, &ranges[0] )) ||
        (status = wow64_schan_output_range( params32->bufsize, &size,
                                            sizeof(size), &ranges[1] ))) goto done;
    status = ntdll_wow64_atomic_writev( ranges, ARRAY_SIZE(ranges) );
    goto done;
unlock:
    end_session( object );
done:
    free( datum.data );
    return status;
}

static NTSTATUS wow64_schan_handshake( void *args )
{
    const struct handshake_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range ranges[7];
    struct wow64_schan_desc input = {0}, output = {0};
    struct schan_session_object *object;
    ULONG input_offset = 0, output_offset = 0;
    int output_buffer_idx = -1;
    struct handshake_params params =
    {
        params32->session, NULL, params32->input_size, NULL,
        &input_offset, &output_buffer_idx, &output_offset,
        params32->control_token, params32->alert_type, params32->alert_number
    };
    NTSTATUS status, call_status;
    ULONG count = 0;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    if (params32->input)
    {
        if ((status = wow64_schan_capture_desc( params32->input, &input ))) goto unlock;
        params.input = &input.desc;
    }
    if (params32->output)
    {
        if ((status = wow64_schan_capture_desc( params32->output, &output ))) goto unlock;
        params.output = &output.desc;
    }
    if (params32->input && !params32->control_token &&
        (status = wow64_schan_capture_desc_data( &input, TRUE,
                                                 params32->input_size,
                                                 SCHAN_WOW64_MAX_TOKEN_SIZE ))) goto unlock;
    if (params32->output &&
        (status = wow64_schan_capture_desc_data( &output, FALSE, 0,
                                                 SCHAN_WOW64_MAX_TOKEN_SIZE ))) goto unlock;
    call_status = handshake_session( object, &params );
    if (params32->output &&
        (status = wow64_schan_add_output_buffers( &output, output_buffer_idx,
                                                  output_offset, ranges, &count ))) goto unlock;
    if ((status = wow64_schan_output_range( params32->input_offset, &input_offset,
                                            sizeof(input_offset), &ranges[count++] )) ||
        (status = wow64_schan_output_range( params32->output_buffer_idx, &output_buffer_idx,
                                            sizeof(output_buffer_idx), &ranges[count++] )) ||
        (status = wow64_schan_output_range( params32->output_offset, &output_offset,
                                            sizeof(output_offset), &ranges[count++] ))) goto unlock;
    end_session( object );
    status = ntdll_wow64_atomic_writev( ranges, count );
    if (!status) status = call_status;
    goto done;
unlock:
    end_session( object );
done:
    wow64_schan_free_desc( &output );
    wow64_schan_free_desc( &input );
    return status;
}

static NTSTATUS wow64_schan_recv( void *args )
{
    const struct recv_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range ranges[2];
    struct wow64_schan_desc input = {0};
    struct schan_session_object *object;
    ULONG length;
    BYTE *buffer = NULL;
    SIZE_T published = 0;
    BOOL length_set = FALSE;
    struct recv_params params = { params32->session, NULL, params32->input_size,
                                  NULL, &length };
    SIZE_T max_message_size;
    NTSTATUS status, call_status;
    ULONG count = 0;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    if (params32->input)
    {
        if ((status = wow64_schan_capture_desc( params32->input, &input ))) goto unlock;
        params.input = &input.desc;
    }
    if ((status = wow64_schan_copy_from_guest( params32->length, &length,
                                               sizeof(length) ))) goto unlock;
    max_message_size = pgnutls_record_get_max_size( object->session );
    if (length > max_message_size) length = max_message_size;
    if (length && (status = wow64_schan_validate_guest_range( params32->buffer,
                                                              length, TRUE ))) goto unlock;
    if (length && !(buffer = malloc( length )))
    {
        status = STATUS_NO_MEMORY;
        goto unlock;
    }
    params.buffer = buffer;
    if (params32->input &&
        (status = wow64_schan_capture_desc_data( &input, TRUE,
                                                 params32->input_size,
                                                 SCHAN_WOW64_MAX_RECORD_BUFFER_SIZE ))) goto unlock;
    call_status = recv_session( object, &params, &published, &length_set );
    if (published &&
        (status = wow64_schan_output_range( params32->buffer, buffer, published,
                                            &ranges[count++] ))) goto unlock;
    if (length_set &&
        (status = wow64_schan_output_range( params32->length, &length, sizeof(length),
                                            &ranges[count++] ))) goto unlock;
    end_session( object );
    if (count && (status = ntdll_wow64_atomic_writev( ranges, count ))) goto done;
    status = call_status;
    goto done;
unlock:
    end_session( object );
done:
    free( buffer );
    wow64_schan_free_desc( &input );
    return status;
}

static NTSTATUS wow64_schan_send( void *args )
{
    const struct send_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT;
    struct ntdll_wow64_user_write_range ranges[6];
    struct wow64_schan_desc output = {0};
    struct schan_session_object *object;
    BYTE *buffer = NULL;
    int output_buffer_idx = -1;
    ULONG output_offset = 0, count = 0;
    struct send_params params = { params32->session, NULL, NULL, params32->length,
                                  &output_buffer_idx, &output_offset };
    SIZE_T max_message_size;
    NTSTATUS status, call_status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    max_message_size = pgnutls_record_get_max_size( object->session );
    if (params.length > max_message_size)
    {
        status = SEC_E_BUFFER_TOO_SMALL;
        goto unlock;
    }
    if (params32->output)
    {
        if ((status = wow64_schan_capture_desc( params32->output, &output ))) goto unlock;
        if ((status = wow64_schan_capture_desc_data( &output, FALSE, 0,
                                                     SCHAN_WOW64_MAX_RECORD_BUFFER_SIZE ))) goto unlock;
        params.output = &output.desc;
    }
    if (params.length)
    {
        if ((status = wow64_schan_validate_guest_range( params32->buffer,
                                                        params.length, FALSE ))) goto unlock;
        if (!(buffer = malloc( params.length )))
        {
            status = STATUS_NO_MEMORY;
            goto unlock;
        }
        if ((status = wow64_schan_copy_from_guest( params32->buffer, buffer,
                                                   params.length ))) goto unlock;
    }
    params.buffer = buffer;
    call_status = send_session( object, &params );
    output_buffer_idx = object->transport.out.current_buffer_idx;
    output_offset = object->transport.out.offset;
    if (params32->output &&
        (status = wow64_schan_add_output_buffers( &output, output_buffer_idx,
                                                  output_offset, ranges, &count ))) goto unlock;
    if (call_status == SEC_E_OK)
    {
        if ((status = wow64_schan_output_range( params32->output_buffer_idx,
                                                &output_buffer_idx,
                                                sizeof(output_buffer_idx), &ranges[count++] )) ||
            (status = wow64_schan_output_range( params32->output_offset, &output_offset,
                                                sizeof(output_offset), &ranges[count++] ))) goto unlock;
    }
    end_session( object );
    if (count && (status = ntdll_wow64_atomic_writev( ranges, count ))) goto done;
    status = call_status;
    goto done;
unlock:
    end_session( object );
done:
    free( buffer );
    wow64_schan_free_desc( &output );
    return status;
}

static NTSTATUS wow64_schan_set_application_protocols( void *args )
{
    const struct set_application_protocols_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED;
    struct schan_session_object *object;
    struct set_application_protocols_params params = { params32->session, NULL, 0 };
    BYTE header[sizeof(unsigned int) * 2 + sizeof(unsigned short)];
    unsigned int offset = 0, extension;
    unsigned short list_len;
    BYTE *buffer = NULL;
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    if (sizeof(unsigned int) > params32->buflen)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if ((status = wow64_schan_copy_from_guest( params32->buffer, header,
                                               sizeof(unsigned int) ))) goto done;
    offset += sizeof(unsigned int);
    if (offset + sizeof(extension) > params32->buflen)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (params32->buffer > ~(ULONG)0 - offset)
    {
        status = STATUS_ACCESS_VIOLATION;
        goto done;
    }
    if ((status = wow64_schan_copy_from_guest( params32->buffer + offset, &extension,
                                               sizeof(extension) ))) goto done;
    memcpy( header + offset, &extension, sizeof(extension) );
    if (extension != SecApplicationProtocolNegotiationExt_ALPN)
    {
        FIXME("extension %u not supported\n", extension);
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    offset += sizeof(extension);
    if (offset + sizeof(list_len) > params32->buflen)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (params32->buffer > ~(ULONG)0 - offset)
    {
        status = STATUS_ACCESS_VIOLATION;
        goto done;
    }
    if ((status = wow64_schan_copy_from_guest( params32->buffer + offset, &list_len,
                                               sizeof(list_len) ))) goto done;
    memcpy( header + offset, &list_len, sizeof(list_len) );
    offset += sizeof(list_len);
    if (list_len > params32->buflen - offset)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (!(buffer = malloc( offset + list_len )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    memcpy( buffer, header, offset );
    if (params32->buffer > ~(ULONG)0 - offset)
    {
        status = STATUS_ACCESS_VIOLATION;
        goto done;
    }
    if ((status = wow64_schan_copy_from_guest( params32->buffer + offset, buffer + offset,
                                               list_len ))) goto done;
    params.buffer = buffer;
    params.buflen = offset + list_len;
    status = set_application_protocols_session( object, &params );
done:
    free( buffer );
    end_session( object );
    return status;
}

static NTSTATUS wow64_schan_set_session_target( void *args )
{
    const struct set_session_target_params32 *params32 = args;
    const UINT32 flags = WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                         WINE_UNIXLIB_DISPATCH_ENTRY_NESTED;
    struct schan_session_object *object;
    char *target = NULL;
    NTSTATUS status;

    if ((status = wow64_schan_check_call( sizeof(*params32), flags ))) return status;
    if ((status = begin_session( params32->session, &object ))) return status;
    if (!params32->target)
    {
        status = STATUS_ACCESS_VIOLATION;
        goto done;
    }
    if ((status = wow64_schan_copy_cstr( params32->target, &target ))) goto done;
    pgnutls_server_name_set( object->session, GNUTLS_NAME_DNS, target, strlen(target) );
    status = STATUS_SUCCESS;
done:
    free( target );
    end_session( object );
    return status;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    process_attach,
    process_detach,
    wow64_schan_allocate_certificate_credentials,
    wow64_schan_create_session,
    schan_dispose_session,
    wow64_schan_free_certificate_credentials,
    wow64_schan_get_application_protocol,
    wow64_schan_get_cipher_info,
    wow64_schan_get_connection_info,
    schan_get_enabled_protocols,
    schan_get_key_signature_algorithm,
    schan_get_max_message_size,
    schan_get_session_cipher_block_size,
    wow64_schan_get_session_peer_certificate,
    wow64_schan_get_unique_channel_binding,
    wow64_schan_handshake,
    wow64_schan_recv,
    wow64_schan_send,
    wow64_schan_set_application_protocols,
    schan_set_dtls_mtu,
    wow64_schan_set_session_target,
    schan_set_dtls_timeouts,
};

static const struct wine_unixlib_dispatch_entry_v2 wow64_dispatch_metadata[] =
{
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct allocate_certificate_credentials_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct create_session_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_params, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct free_certificate_credentials_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_output_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_output_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_output_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_params, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_params, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct session_params, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct peer_certificate_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct unique_binding_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct handshake_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct recv_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct send_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct set_application_protocols_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct set_dtls_mtu_params, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct set_session_target_params32,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct set_dtls_timeouts_params, 0 ),
};

WINE_UNIXLIB_DISPATCH_SOURCE_V2( __wine_unix_call_wow64_funcs, wow64_dispatch_metadata );

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );
C_ASSERT( ARRAYSIZE(wow64_dispatch_metadata) == unix_funcs_count );

#endif /* _WIN64 */

#endif /* SONAME_LIBGNUTLS */
