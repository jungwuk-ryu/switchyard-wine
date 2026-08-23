/*
 * Unix library functions
 *
 * Copyright (C) 1993,1994,1996,1997 John Brezak, Erik Bos, Alex Korobka.
 * Copyright (C) 2001 Stefan Leichter
 * Copyright (C) 2004 Hans Leidekker
 * Copyright (C) 2005 Marcus Meissner
 * Copyright (C) 2006-2008 Kai Blin
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
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef HAVE_NETDB_H
# include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
# include <sys/sockio.h>
#endif
#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#ifdef HAVE_NETINET_TCP_H
# include <netinet/tcp.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
# define if_indextoname unix_if_indextoname
# define if_nametoindex unix_if_nametoindex
# include <net/if.h>
# undef if_indextoname
# undef if_nametoindex
#endif
#ifdef HAVE_IFADDRS_H
# include <ifaddrs.h>
#endif
#include <poll.h>

#ifdef HAVE_NETIPX_IPX_H
# include <netipx/ipx.h>
# define HAS_IPX
#elif defined(HAVE_LINUX_IPX_H)
# ifdef HAVE_ASM_TYPES_H
#  include <asm/types.h>
# endif
# ifdef HAVE_LINUX_TYPES_H
#  include <linux/types.h>
# endif
# include <linux/ipx.h>
# ifdef SOL_IPX
#  define HAS_IPX
# endif
#endif

#ifdef HAVE_LINUX_IRDA_H
# ifdef HAVE_LINUX_TYPES_H
#  include <linux/types.h>
# endif
# include <linux/irda.h>
# define HAS_IRDA
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winerror.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ws2tcpip.h"
#include "wsipx.h"
#include "af_irda.h"
#include "wine/debug.h"

#include "ws2_32_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsock);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

#ifndef HAVE_LINUX_GETHOSTBYNAME_R_6
static pthread_mutex_t host_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#define MAP(x) {WS_ ## x, x}

static const int addrinfo_flag_map[][2] =
{
    MAP( AI_PASSIVE ),
    MAP( AI_CANONNAME ),
    MAP( AI_NUMERICHOST ),
#ifdef AI_NUMERICSERV
    MAP( AI_NUMERICSERV ),
#endif
#ifdef AI_V4MAPPED
    MAP( AI_V4MAPPED ),
#endif
#ifdef AI_ALL
    MAP( AI_ALL ),
#endif
    MAP( AI_ADDRCONFIG ),
};

static const int nameinfo_flag_map[][2] =
{
    MAP( NI_DGRAM ),
    MAP( NI_NAMEREQD ),
    MAP( NI_NOFQDN ),
    MAP( NI_NUMERICHOST ),
    MAP( NI_NUMERICSERV ),
};

static const int family_map[][2] =
{
    MAP( AF_UNSPEC ),
    MAP( AF_INET ),
    MAP( AF_INET6 ),
#ifdef AF_IPX
    MAP( AF_IPX ),
#endif
#ifdef AF_IRDA
    MAP( AF_IRDA ),
#endif
};

static const int socktype_map[][2] =
{
    MAP( SOCK_STREAM ),
    MAP( SOCK_DGRAM ),
    MAP( SOCK_RAW ),
};

static const int ip_protocol_map[][2] =
{
    MAP( IPPROTO_IP ),
    MAP( IPPROTO_TCP ),
    MAP( IPPROTO_UDP ),
    MAP( IPPROTO_IPV6 ),
    MAP( IPPROTO_ICMP ),
    MAP( IPPROTO_IGMP ),
    MAP( IPPROTO_RAW ),
    {WS_IPPROTO_IPV4, IPPROTO_IPIP},
};

#undef MAP

static pthread_once_t hash_init_once = PTHREAD_ONCE_INIT;
static BYTE byte_hash[256];

static void init_hash(void)
{
    unsigned i, index;
    NTSTATUS status;
    BYTE *buf, tmp;
    ULONG buf_len;

    for (i = 0; i < sizeof(byte_hash); ++i)
        byte_hash[i] = i;

    buf_len = sizeof(SYSTEM_INTERRUPT_INFORMATION) * NtCurrentTeb()->Peb->NumberOfProcessors;
    if (!(buf = malloc( buf_len )))
    {
        ERR( "No memory.\n" );
        return;
    }

    for (i = 0; i < sizeof(byte_hash) - 1; ++i)
    {
        if (!(i % buf_len) && (status = NtQuerySystemInformation( SystemInterruptInformation, buf,
                                                                  buf_len, &buf_len )))
        {
            ERR( "Failed to get random bytes.\n" );
            free( buf );
            return;
        }
        index = i + buf[i % buf_len] % (sizeof(byte_hash) - i);
        tmp = byte_hash[index];
        byte_hash[index] = byte_hash[i];
        byte_hash[i] = tmp;
    }
    free( buf );
}

static void hash_random( BYTE *d, const BYTE *s, unsigned int len )
{
    unsigned int i;

    for (i = 0; i < len; ++i)
        d[i] = byte_hash[s[i]];
}

static int addrinfo_flags_from_unix( int flags )
{
    int ws_flags = 0;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(addrinfo_flag_map); ++i)
    {
        if (flags & addrinfo_flag_map[i][1])
        {
            ws_flags |= addrinfo_flag_map[i][0];
            flags &= ~addrinfo_flag_map[i][1];
        }
    }

    if (flags)
        FIXME( "unhandled flags %#x\n", flags );
    return ws_flags;
}

static int addrinfo_flags_to_unix( int flags )
{
    int unix_flags = 0;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(addrinfo_flag_map); ++i)
    {
        if (flags & addrinfo_flag_map[i][0])
        {
            unix_flags |= addrinfo_flag_map[i][1];
            flags &= ~addrinfo_flag_map[i][0];
        }
    }

    if (flags)
        FIXME( "unhandled flags %#x\n", flags );
    return unix_flags;
}

static int nameinfo_flags_to_unix( int flags )
{
    int unix_flags = 0;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(nameinfo_flag_map); ++i)
    {
        if (flags & nameinfo_flag_map[i][0])
        {
            unix_flags |= nameinfo_flag_map[i][1];
            flags &= ~nameinfo_flag_map[i][0];
        }
    }

    if (flags)
        FIXME( "unhandled flags %#x\n", flags );
    return unix_flags;
}

static int family_from_unix( int family )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(family_map); ++i)
    {
        if (family == family_map[i][1])
            return family_map[i][0];
    }

    FIXME( "unhandled family %u\n", family );
    return -1;
}

static int family_to_unix( int family )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(family_map); ++i)
    {
        if (family == family_map[i][0])
            return family_map[i][1];
    }

    FIXME( "unhandled family %u\n", family );
    return -1;
}

static int socktype_from_unix( int type )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(socktype_map); ++i)
    {
        if (type == socktype_map[i][1])
            return socktype_map[i][0];
    }

    FIXME( "unhandled type %u\n", type );
    return -1;
}

static int socktype_to_unix( int type )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(socktype_map); ++i)
    {
        if (type == socktype_map[i][0])
            return socktype_map[i][1];
    }

    FIXME( "unhandled type %u\n", type );
    return -1;
}

static int protocol_from_unix( int protocol )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(ip_protocol_map); ++i)
    {
        if (protocol == ip_protocol_map[i][1])
            return ip_protocol_map[i][0];
    }

    if (protocol >= WS_NSPROTO_IPX && protocol <= WS_NSPROTO_IPX + 255)
        return protocol;

    FIXME( "unhandled protocol %u\n", protocol );
    return -1;
}

static int protocol_to_unix( int protocol )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(ip_protocol_map); ++i)
    {
        if (protocol == ip_protocol_map[i][0])
            return ip_protocol_map[i][1];
    }

    if (protocol >= WS_NSPROTO_IPX && protocol <= WS_NSPROTO_IPX + 255)
        return protocol;

    FIXME( "unhandled protocol %u\n", protocol );
    return -1;
}

static unsigned int errno_from_unix( int err )
{
    switch (err)
    {
        case EINTR:             return WSAEINTR;
        case EBADF:             return WSAEBADF;
        case EPERM:
        case EACCES:            return WSAEACCES;
        case EFAULT:            return WSAEFAULT;
        case EINVAL:            return WSAEINVAL;
        case EMFILE:            return WSAEMFILE;
        case EINPROGRESS:
        case EWOULDBLOCK:       return WSAEWOULDBLOCK;
        case EALREADY:          return WSAEALREADY;
        case ENOTSOCK:          return WSAENOTSOCK;
        case EDESTADDRREQ:      return WSAEDESTADDRREQ;
        case EMSGSIZE:          return WSAEMSGSIZE;
        case EPROTOTYPE:        return WSAEPROTOTYPE;
        case ENOPROTOOPT:       return WSAENOPROTOOPT;
        case EPROTONOSUPPORT:   return WSAEPROTONOSUPPORT;
        case ESOCKTNOSUPPORT:   return WSAESOCKTNOSUPPORT;
        case EOPNOTSUPP:        return WSAEOPNOTSUPP;
        case EPFNOSUPPORT:      return WSAEPFNOSUPPORT;
        case EAFNOSUPPORT:      return WSAEAFNOSUPPORT;
        case EADDRINUSE:        return WSAEADDRINUSE;
        case EADDRNOTAVAIL:     return WSAEADDRNOTAVAIL;
        case ENETDOWN:          return WSAENETDOWN;
        case ENETUNREACH:       return WSAENETUNREACH;
        case ENETRESET:         return WSAENETRESET;
        case ECONNABORTED:      return WSAECONNABORTED;
        case EPIPE:
        case ECONNRESET:        return WSAECONNRESET;
        case ENOBUFS:           return WSAENOBUFS;
        case EISCONN:           return WSAEISCONN;
        case ENOTCONN:          return WSAENOTCONN;
        case ESHUTDOWN:         return WSAESHUTDOWN;
        case ETOOMANYREFS:      return WSAETOOMANYREFS;
        case ETIMEDOUT:         return WSAETIMEDOUT;
        case ECONNREFUSED:      return WSAECONNREFUSED;
        case ELOOP:             return WSAELOOP;
        case ENAMETOOLONG:      return WSAENAMETOOLONG;
        case EHOSTDOWN:         return WSAEHOSTDOWN;
        case EHOSTUNREACH:      return WSAEHOSTUNREACH;
        case ENOTEMPTY:         return WSAENOTEMPTY;
#ifdef EPROCLIM
        case EPROCLIM:          return WSAEPROCLIM;
#endif
#ifdef EUSERS
        case EUSERS:            return WSAEUSERS;
#endif
#ifdef EDQUOT
        case EDQUOT:            return WSAEDQUOT;
#endif
#ifdef ESTALE
        case ESTALE:            return WSAESTALE;
#endif
#ifdef EREMOTE
        case EREMOTE:           return WSAEREMOTE;
#endif
        default:
            FIXME( "unknown error: %s\n", strerror( err ) );
            return WSAEFAULT;
    }
}

static UINT host_errno_from_unix( int err )
{
    WARN( "%d\n", err );

    switch (err)
    {
        case HOST_NOT_FOUND:    return WSAHOST_NOT_FOUND;
        case TRY_AGAIN:         return WSATRY_AGAIN;
        case NO_RECOVERY:       return WSANO_RECOVERY;
        case NO_DATA:           return WSANO_DATA;
        case ENOBUFS:           return WSAENOBUFS;
        case 0:                 return 0;
        default:
            WARN( "Unknown h_errno %d!\n", err );
            return WSAEOPNOTSUPP;
    }
}

static int addrinfo_err_from_unix( int err )
{
    switch (err)
    {
        case 0:             return 0;
        case EAI_AGAIN:     return WS_EAI_AGAIN;
        case EAI_BADFLAGS:  return WS_EAI_BADFLAGS;
        case EAI_FAIL:      return WS_EAI_FAIL;
        case EAI_FAMILY:    return WS_EAI_FAMILY;
        case EAI_MEMORY:    return WS_EAI_MEMORY;
        /* EAI_NODATA is deprecated, but still used by Windows and Linux. We map
         * the newer EAI_NONAME to EAI_NODATA for now until Windows changes too. */
#ifdef EAI_NODATA
        case EAI_NODATA:    return WS_EAI_NODATA;
#endif
#ifdef EAI_NONAME
        case EAI_NONAME:    return WS_EAI_NODATA;
#endif
        case EAI_SERVICE:   return WS_EAI_SERVICE;
        case EAI_SOCKTYPE:  return WS_EAI_SOCKTYPE;
        case EAI_SYSTEM:
            if (errno == EBUSY) ERR_(winediag)("getaddrinfo() returned EBUSY. You may be missing a libnss plugin\n");
            /* some broken versions of glibc return EAI_SYSTEM and set errno to
             * 0 instead of returning EAI_NONAME */
            return errno ? errno_from_unix( errno ) : WS_EAI_NONAME;

        default:
            FIXME( "unhandled error %d\n", err );
            return err;
    }
}

union unix_sockaddr
{
    struct sockaddr addr;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
#ifdef HAS_IPX
    struct sockaddr_ipx ipx;
#endif
#ifdef HAS_IRDA
    struct sockaddr_irda irda;
#endif
};

/* different from the version in ntdll and server; it does not return failure if
 * given a short buffer */
static int sockaddr_from_unix( const union unix_sockaddr *uaddr, struct WS_sockaddr *wsaddr, socklen_t wsaddrlen )
{
    if (wsaddrlen) memset( wsaddr, 0, wsaddrlen );

    switch (uaddr->addr.sa_family)
    {
    case AF_INET:
    {
        struct WS_sockaddr_in win = {0};

        if (wsaddrlen >= sizeof(win))
        {
            win.sin_family = WS_AF_INET;
            win.sin_port = uaddr->in.sin_port;
            memcpy( &win.sin_addr, &uaddr->in.sin_addr, sizeof(win.sin_addr) );
            memcpy( wsaddr, &win, sizeof(win) );
        }
        return sizeof(win);
    }

    case AF_INET6:
    {
        struct WS_sockaddr_in6 win = {0};

        if (wsaddrlen >= sizeof(win))
        {
            win.sin6_family = WS_AF_INET6;
            win.sin6_port = uaddr->in6.sin6_port;
            win.sin6_flowinfo = uaddr->in6.sin6_flowinfo;
            memcpy( &win.sin6_addr, &uaddr->in6.sin6_addr, sizeof(win.sin6_addr) );
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_SCOPE_ID
            win.sin6_scope_id = uaddr->in6.sin6_scope_id;
#endif
            memcpy( wsaddr, &win, sizeof(win) );
        }
        return sizeof(win);
    }

#ifdef HAS_IPX
    case AF_IPX:
    {
        struct WS_sockaddr_ipx win = {0};

        if (wsaddrlen >= sizeof(win))
        {
            win.sa_family = WS_AF_IPX;
            memcpy( win.sa_netnum, &uaddr->ipx.sipx_network, sizeof(win.sa_netnum) );
            memcpy( win.sa_nodenum, &uaddr->ipx.sipx_node, sizeof(win.sa_nodenum) );
            win.sa_socket = uaddr->ipx.sipx_port;
            memcpy( wsaddr, &win, sizeof(win) );
        }
        return sizeof(win);
    }
#endif

#ifdef HAS_IRDA
    case AF_IRDA:
    {
        SOCKADDR_IRDA win;

        if (wsaddrlen >= sizeof(win))
        {
            win.irdaAddressFamily = WS_AF_IRDA;
            memcpy( win.irdaDeviceID, &uaddr->irda.sir_addr, sizeof(win.irdaDeviceID) );
            if (uaddr->irda.sir_lsap_sel != LSAP_ANY)
                snprintf( win.irdaServiceName, sizeof(win.irdaServiceName), "LSAP-SEL%u", uaddr->irda.sir_lsap_sel );
            else
                memcpy( win.irdaServiceName, uaddr->irda.sir_name, sizeof(win.irdaServiceName) );
            memcpy( wsaddr, &win, sizeof(win) );
        }
        return sizeof(win);
    }
#endif

    case AF_UNSPEC:
        return 0;

    default:
        FIXME( "unknown address family %d\n", uaddr->addr.sa_family );
        return 0;
    }
}

static socklen_t sockaddr_to_unix( const struct WS_sockaddr *wsaddr, int wsaddrlen, union unix_sockaddr *uaddr )
{
    memset( uaddr, 0, sizeof(*uaddr) );

    switch (wsaddr->sa_family)
    {
    case WS_AF_INET:
    {
        struct WS_sockaddr_in win = {0};

        if (wsaddrlen < sizeof(win)) return 0;
        memcpy( &win, wsaddr, sizeof(win) );
        uaddr->in.sin_family = AF_INET;
        uaddr->in.sin_port = win.sin_port;
        memcpy( &uaddr->in.sin_addr, &win.sin_addr, sizeof(win.sin_addr) );
        return sizeof(uaddr->in);
    }

    case WS_AF_INET6:
    {
        struct WS_sockaddr_in6 win = {0};

        if (wsaddrlen < sizeof(win)) return 0;
        memcpy( &win, wsaddr, sizeof(win) );
        uaddr->in6.sin6_family = AF_INET6;
        uaddr->in6.sin6_port = win.sin6_port;
        uaddr->in6.sin6_flowinfo = win.sin6_flowinfo;
        memcpy( &uaddr->in6.sin6_addr, &win.sin6_addr, sizeof(win.sin6_addr) );
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_SCOPE_ID
        uaddr->in6.sin6_scope_id = win.sin6_scope_id;
#endif
        return sizeof(uaddr->in6);
    }

#ifdef HAS_IPX
    case WS_AF_IPX:
    {
        struct WS_sockaddr_ipx win = {0};

        if (wsaddrlen < sizeof(win)) return 0;
        memcpy( &win, wsaddr, sizeof(win) );
        uaddr->ipx.sipx_family = AF_IPX;
        memcpy( &uaddr->ipx.sipx_network, win.sa_netnum, sizeof(win.sa_netnum) );
        memcpy( &uaddr->ipx.sipx_node, win.sa_nodenum, sizeof(win.sa_nodenum) );
        uaddr->ipx.sipx_port = win.sa_socket;
        return sizeof(uaddr->ipx);
    }
#endif

#ifdef HAS_IRDA
    case WS_AF_IRDA:
    {
        SOCKADDR_IRDA win = {0};
        unsigned int lsap_sel;

        if (wsaddrlen < sizeof(win)) return 0;
        memcpy( &win, wsaddr, sizeof(win) );
        uaddr->irda.sir_family = AF_IRDA;
        if (sscanf( win.irdaServiceName, "LSAP-SEL%u", &lsap_sel ) == 1)
            uaddr->irda.sir_lsap_sel = lsap_sel;
        else
        {
            uaddr->irda.sir_lsap_sel = LSAP_ANY;
            memcpy( uaddr->irda.sir_name, win.irdaServiceName, sizeof(win.irdaServiceName) );
        }
        memcpy( &uaddr->irda.sir_addr, win.irdaDeviceID, sizeof(win.irdaDeviceID) );
        return sizeof(uaddr->irda);
    }
#endif

    case WS_AF_UNSPEC:
        switch (wsaddrlen)
        {
        default: /* likely an ipv4 address */
        case sizeof(struct WS_sockaddr_in):
            return sizeof(uaddr->in);

#ifdef HAS_IPX
        case sizeof(struct WS_sockaddr_ipx):
            return sizeof(uaddr->ipx);
#endif

#ifdef HAS_IRDA
        case sizeof(SOCKADDR_IRDA):
            return sizeof(uaddr->irda);
#endif

        case sizeof(struct WS_sockaddr_in6):
            return sizeof(uaddr->in6);
        }

    default:
        FIXME( "unknown address family %u\n", wsaddr->sa_family );
        return 0;
    }
}

static BOOL addrinfo_in_list( const struct WS_addrinfo *list, const struct WS_addrinfo *ai )
{
    const struct WS_addrinfo *cursor = list;
    while (cursor)
    {
        if (ai->ai_flags == cursor->ai_flags &&
            ai->ai_family == cursor->ai_family &&
            ai->ai_socktype == cursor->ai_socktype &&
            ai->ai_protocol == cursor->ai_protocol &&
            ai->ai_addrlen == cursor->ai_addrlen &&
            !memcmp( ai->ai_addr, cursor->ai_addr, ai->ai_addrlen ) &&
            ((ai->ai_canonname && cursor->ai_canonname && !strcmp( ai->ai_canonname, cursor->ai_canonname ))
            || (!ai->ai_canonname && !cursor->ai_canonname)))
        {
            return TRUE;
        }
        cursor = cursor->ai_next;
    }
    return FALSE;
}

static BOOL add_size_checked( SIZE_T *size, SIZE_T add )
{
    if (add > ~(SIZE_T)0 - *size) return FALSE;
    *size += add;
    return TRUE;
}

static BOOL add_array_size_checked( SIZE_T *size, SIZE_T count, SIZE_T element_size )
{
    if (count && element_size > ~(SIZE_T)0 / count) return FALSE;
    return add_size_checked( size, count * element_size );
}

static BOOL get_cstring_size( const char *string, SIZE_T *size )
{
    SIZE_T len = strnlen( string, UINT_MAX );

    if (len == UINT_MAX) return FALSE;
    *size = len + 1;
    return TRUE;
}

static BOOL add_cstring_size_checked( SIZE_T *size, const char *string )
{
    SIZE_T len;

    return get_cstring_size( string, &len ) && add_size_checked( size, len );
}

#ifdef HAVE_GETADDRINFO
static NTSTATUS prepare_addrinfo_hints( const struct WS_addrinfo *hints, struct addrinfo *unix_hints )
{
    if (!hints) return 0;

    unix_hints->ai_flags = addrinfo_flags_to_unix( hints->ai_flags );

    if (hints->ai_family)
        unix_hints->ai_family = family_to_unix( hints->ai_family );

    if (hints->ai_socktype)
    {
        if ((unix_hints->ai_socktype = socktype_to_unix( hints->ai_socktype )) < 0)
            return WSAESOCKTNOSUPPORT;
    }

    if (hints->ai_protocol)
        unix_hints->ai_protocol = max( protocol_to_unix( hints->ai_protocol ), 0 );

    /* Windows allows some invalid combinations */
    if (unix_hints->ai_protocol == IPPROTO_TCP
            && unix_hints->ai_socktype != SOCK_STREAM
            && unix_hints->ai_socktype != SOCK_SEQPACKET)
    {
        WARN( "ignoring invalid type %u for TCP\n", unix_hints->ai_socktype );
        unix_hints->ai_socktype = 0;
    }
    else if (unix_hints->ai_protocol == IPPROTO_UDP && unix_hints->ai_socktype != SOCK_DGRAM)
    {
        WARN( "ignoring invalid type %u for UDP\n", unix_hints->ai_socktype );
        unix_hints->ai_socktype = 0;
    }
    else if (unix_hints->ai_protocol >= WS_NSPROTO_IPX && unix_hints->ai_protocol <= WS_NSPROTO_IPX + 255
            && unix_hints->ai_socktype != SOCK_DGRAM)
    {
        WARN( "ignoring invalid type %u for IPX\n", unix_hints->ai_socktype );
        unix_hints->ai_socktype = 0;
    }
    else if (unix_hints->ai_protocol == IPPROTO_IPV6)
    {
        WARN( "ignoring protocol IPv6\n" );
        unix_hints->ai_protocol = 0;
    }

    return 0;
}

static NTSTATUS addrinfo_from_unix( struct getaddrinfo_params *params, const struct addrinfo *unix_info )
{
    const struct WS_addrinfo *hints = params->hints;
    const struct addrinfo *src;
    struct WS_addrinfo *dst, *prev = NULL;
    SIZE_T needed_size = 0;

    for (src = unix_info; src != NULL; src = src->ai_next)
    {
        SIZE_T addr_size = sockaddr_from_unix( (const union unix_sockaddr *)src->ai_addr, NULL, 0 );

        if (!add_size_checked( &needed_size, sizeof(struct WS_addrinfo) ) ||
            (src->ai_canonname && !add_cstring_size_checked( &needed_size, src->ai_canonname )) ||
            !add_size_checked( &needed_size, addr_size ) || needed_size > UINT_MAX)
            return WSAENOBUFS;
    }

    if (!needed_size) return 0;

    if (*params->size < needed_size)
    {
        *params->size = (unsigned int)needed_size;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    dst = params->info;

    memset( params->info, 0, needed_size );

    for (src = unix_info; src != NULL; src = src->ai_next)
    {
        void *next = dst + 1;

        dst->ai_flags = addrinfo_flags_from_unix( src->ai_flags );
        dst->ai_family = family_from_unix( src->ai_family );
        if (hints)
        {
            dst->ai_socktype = hints->ai_socktype;
            dst->ai_protocol = hints->ai_protocol;
        }
        else
        {
            dst->ai_socktype = socktype_from_unix( src->ai_socktype );
            dst->ai_protocol = protocol_from_unix( src->ai_protocol );
        }
        if (src->ai_canonname)
        {
            SIZE_T len;

            if (!get_cstring_size( src->ai_canonname, &len )) return WSAENOBUFS;
            dst->ai_canonname = next;
            memcpy( dst->ai_canonname, src->ai_canonname, len );
            next = dst->ai_canonname + len;
        }

        dst->ai_addrlen = sockaddr_from_unix( (const union unix_sockaddr *)src->ai_addr, NULL, 0 );
        dst->ai_addr = next;
        sockaddr_from_unix( (const union unix_sockaddr *)src->ai_addr, dst->ai_addr, dst->ai_addrlen );
        dst->ai_next = NULL;
        next = (char *)dst->ai_addr + dst->ai_addrlen;

        if (dst == params->info || !addrinfo_in_list( params->info, dst ))
        {
            if (prev)
                prev->ai_next = dst;
            prev = dst;
            dst = next;
        }
    }

    return 0;
}

static NTSTATUS unix_getaddrinfo_result( struct getaddrinfo_params *params, const char *node,
                                         const char *service, const struct addrinfo *unix_hints )
{
    struct addrinfo *unix_info;
    NTSTATUS status;
    int ret;

    if ((ret = getaddrinfo( node, service, unix_hints, &unix_info )))
        return addrinfo_err_from_unix( ret );
    status = addrinfo_from_unix( params, unix_info );
    freeaddrinfo( unix_info );
    return status;
}
#else
static NTSTATUS unix_getaddrinfo_result( struct getaddrinfo_params *params, const char *node,
                                         const char *service, const void *unix_hints )
{
    FIXME( "getaddrinfo() not found during build time\n" );
    return WS_EAI_FAIL;
}
#endif

static NTSTATUS unix_getaddrinfo( void *args )
{
    struct getaddrinfo_params *params = args;
    const char *service = params->service;
#ifdef HAVE_GETADDRINFO
    struct addrinfo unix_hints = {0};
    NTSTATUS status;

    /* servname tweak required by OSX and BSD kernels */
    if (service && !service[0]) service = "0";
    if ((status = prepare_addrinfo_hints( params->hints, &unix_hints ))) return status;
    return unix_getaddrinfo_result( params, params->node, service,
                                    params->hints ? &unix_hints : NULL );
#else
    return unix_getaddrinfo_result( params, params->node, service, NULL );
#endif
}


static int hostent_from_unix( const struct hostent *unix_host, struct WS_hostent *host, unsigned int *const size )
{
    SIZE_T needed_size = sizeof(struct WS_hostent), alias_count = 0, addr_count = 0, i;
    SIZE_T aliases_offset, addr_list_offset, offset;
    char *base = (char *)host;

    if (!unix_host->h_name || !unix_host->h_aliases || !unix_host->h_addr_list ||
        unix_host->h_length < 0 || unix_host->h_length > 0x7fff ||
        !add_cstring_size_checked( &needed_size, unix_host->h_name ))
        return WSAENOBUFS;

    for (alias_count = 0; unix_host->h_aliases[alias_count] != NULL; ++alias_count)
        if (!add_size_checked( &needed_size, sizeof(char *) ) ||
            !add_cstring_size_checked( &needed_size, unix_host->h_aliases[alias_count] ))
            return WSAENOBUFS;
    if (!add_size_checked( &needed_size, sizeof(char *) )) return WSAENOBUFS; /* null terminator */

    for (addr_count = 0; unix_host->h_addr_list[addr_count] != NULL; ++addr_count)
        if (!add_size_checked( &needed_size, sizeof(char *) ) ||
            !add_size_checked( &needed_size, unix_host->h_length ))
            return WSAENOBUFS;
    if (!add_size_checked( &needed_size, sizeof(char *) ) || needed_size > UINT_MAX)
        return WSAENOBUFS; /* null terminator */

    if (*size < needed_size)
    {
        *size = (unsigned int)needed_size;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    memset( host, 0, needed_size );

    /* arrange the memory in the same order as windows >= XP */

    host->h_addrtype = family_from_unix( unix_host->h_addrtype );
    host->h_length = unix_host->h_length;

    aliases_offset = sizeof(*host);
    addr_list_offset = aliases_offset;
    if (!add_array_size_checked( &addr_list_offset, alias_count + 1, sizeof(char *) ))
        return WSAENOBUFS;
    offset = addr_list_offset;
    if (!add_array_size_checked( &offset, addr_count + 1, sizeof(char *) ) || offset > needed_size)
        return WSAENOBUFS;
    host->h_aliases = (char **)(base + aliases_offset);
    host->h_addr_list = (char **)(base + addr_list_offset);

    for (i = 0; i < addr_count; ++i)
    {
        SIZE_T next = offset;

        if (!add_size_checked( &next, unix_host->h_length ) || next > needed_size)
            return WSAENOBUFS;
        host->h_addr_list[i] = base + offset;
        memcpy( host->h_addr_list[i], unix_host->h_addr_list[i], unix_host->h_length );
        offset = next;
    }

    for (i = 0; i < alias_count; ++i)
    {
        SIZE_T len, next;

        if (!get_cstring_size( unix_host->h_aliases[i], &len )) return WSAENOBUFS;
        next = offset;
        if (!add_size_checked( &next, len ) || next > needed_size) return WSAENOBUFS;
        host->h_aliases[i] = base + offset;
        memcpy( host->h_aliases[i], unix_host->h_aliases[i], len );
        offset = next;
    }

    host->h_name = base + offset;
    {
        SIZE_T len, next;

        if (!get_cstring_size( unix_host->h_name, &len )) return WSAENOBUFS;
        next = offset;
        if (!add_size_checked( &next, len ) || next != needed_size) return WSAENOBUFS;
        memcpy( host->h_name, unix_host->h_name, len );
    }

    return 0;
}


typedef NTSTATUS (*hostent_result_func)( const struct hostent *host, void *context );

struct native_hostent_result
{
    struct WS_hostent *host;
    unsigned int *size;
};

static NTSTATUS native_hostent_result( const struct hostent *host, void *context )
{
    struct native_hostent_result *result = context;

    return hostent_from_unix( host, result->host, result->size );
}

static NTSTATUS unix_gethostbyaddr_result( const void *address, int len, int family,
                                           hostent_result_func result_func, void *context )
{
    const void *addr = address;
    const struct in_addr loopback = { htonl( INADDR_LOOPBACK ) };
    int unix_family = family_to_unix( family );
    struct hostent *unix_host;
    int ret;

    if (family == WS_AF_INET && len == 4 && !memcmp( addr, magic_loopback_addr, 4 ))
        addr = &loopback;

#ifdef HAVE_LINUX_GETHOSTBYNAME_R_6
    {
        char *unix_buffer, *new_buffer;
        struct hostent stack_host;
        SIZE_T unix_size = 1024;
        int locerr;

        if (!(unix_buffer = malloc( unix_size )))
            return WSAENOBUFS;

        while (gethostbyaddr_r( addr, len, unix_family, &stack_host, unix_buffer,
                                unix_size, &unix_host, &locerr ) == ERANGE)
        {
            if (unix_size > ~(SIZE_T)0 / 2)
            {
                free( unix_buffer );
                return WSAENOBUFS;
            }
            unix_size *= 2;
            if (!(new_buffer = realloc( unix_buffer, unix_size )))
            {
                free( unix_buffer );
                return WSAENOBUFS;
            }
            unix_buffer = new_buffer;
        }

        if (!unix_host)
            ret = (locerr < 0 ? errno_from_unix( errno ) : host_errno_from_unix( locerr ));
        else
            ret = result_func( unix_host, context );

        free( unix_buffer );
        return ret;
    }
#else
    pthread_mutex_lock( &host_mutex );

    if (!(unix_host = gethostbyaddr( addr, len, unix_family )))
    {
        ret = (h_errno < 0 ? errno_from_unix( errno ) : host_errno_from_unix( h_errno ));
        pthread_mutex_unlock( &host_mutex );
        return ret;
    }

    ret = result_func( unix_host, context );

    pthread_mutex_unlock( &host_mutex );
    return ret;
#endif
}

static NTSTATUS unix_gethostbyaddr( void *args )
{
    struct gethostbyaddr_params *params = args;
    struct native_hostent_result result = {params->host, params->size};

    return unix_gethostbyaddr_result( params->addr, params->len, params->family,
                                      native_hostent_result, &result );
}

static int compare_addrs_hashed( const void *a1, const void *a2, unsigned int addr_len )
{
    char a1_hashed[16], a2_hashed[16];

    hash_random( (BYTE *)a1_hashed, a1, addr_len );
    hash_random( (BYTE *)a2_hashed, a2, addr_len );
    return memcmp( a1_hashed, a2_hashed, addr_len );
}

static NTSTATUS sort_addrs_hashed( struct hostent *host )
{
    /* On Unix gethostbyname() may return IP addresses in random order on each call. On Windows the order of
     * IP addresses is not determined as well but it is the same on consequent calls (changes after network
     * resets and probably DNS timeout expiration).
     * Life is Strange Remastered depends on gethostbyname() returning IP addresses in the same order to reuse
     * the established TLS connection and avoid timeouts that happen in game when establishing multiple extra TLS
     * connections.
     * Just sorting the addresses would break server load balancing provided by gethostbyname(), so randomize the
     * sort once per process. */
    unsigned int i, j;
    char *tmp;

    if (!host->h_addr_list || host->h_length < 0 || host->h_length > 16) return WSAENOBUFS;

    pthread_once( &hash_init_once, init_hash );

    for (i = 0; host->h_addr_list[i]; ++i)
    {
        for (j = i + 1; host->h_addr_list[j]; ++j)
        {
            if (compare_addrs_hashed( host->h_addr_list[j], host->h_addr_list[i], host->h_length ) < 0)
            {
                tmp = host->h_addr_list[j];
                host->h_addr_list[j] = host->h_addr_list[i];
                host->h_addr_list[i] = tmp;
            }
        }
    }
    return 0;
}

#ifdef HAVE_LINUX_GETHOSTBYNAME_R_6
static NTSTATUS unix_gethostbyname_result( const char *name, hostent_result_func result_func,
                                           void *context )
{
    struct hostent stack_host, *unix_host;
    char *unix_buffer, *new_buffer;
    SIZE_T unix_size = 1024;
    int locerr;
    int ret;

    if (!(unix_buffer = malloc( unix_size )))
        return WSAENOBUFS;

    while (gethostbyname_r( name, &stack_host, unix_buffer, unix_size, &unix_host, &locerr ) == ERANGE)
    {
        if (unix_size > ~(SIZE_T)0 / 2)
        {
            free( unix_buffer );
            return WSAENOBUFS;
        }
        unix_size *= 2;
        if (!(new_buffer = realloc( unix_buffer, unix_size )))
        {
            free( unix_buffer );
            return WSAENOBUFS;
        }
        unix_buffer = new_buffer;
    }

    if (!unix_host)
    {
        ret = (locerr < 0 ? errno_from_unix( errno ) : host_errno_from_unix( locerr ));
    }
    else
    {
        if (!(ret = sort_addrs_hashed( unix_host ))) ret = result_func( unix_host, context );
    }

    free( unix_buffer );
    return ret;
}
#else
static NTSTATUS unix_gethostbyname_result( const char *name, hostent_result_func result_func,
                                           void *context )
{
    struct hostent *unix_host;
    int ret;

    pthread_mutex_lock( &host_mutex );

    if (!(unix_host = gethostbyname( name )))
    {
        ret = (h_errno < 0 ? errno_from_unix( errno ) : host_errno_from_unix( h_errno ));
        pthread_mutex_unlock( &host_mutex );
        return ret;
    }

    if (!(ret = sort_addrs_hashed( unix_host ))) ret = result_func( unix_host, context );

    pthread_mutex_unlock( &host_mutex );
    return ret;
}
#endif

static NTSTATUS unix_gethostbyname( void *args )
{
    struct gethostbyname_params *params = args;
    struct native_hostent_result result = {params->host, params->size};

    return unix_gethostbyname_result( params->name, native_hostent_result, &result );
}


static NTSTATUS unix_gethostname( void *args )
{
    struct gethostname_params *params = args;

    if (!gethostname( params->name, params->size ))
        return 0;
    return errno_from_unix( errno );
}


static NTSTATUS unix_getnameinfo( void *args )
{
    struct getnameinfo_params *params = args;
    union unix_sockaddr unix_addr;
    socklen_t unix_addr_len;

    unix_addr_len = sockaddr_to_unix( params->addr, params->addr_len, &unix_addr );

    return addrinfo_err_from_unix( getnameinfo( &unix_addr.addr, unix_addr_len, params->host, params->host_len,
                                                params->serv, params->serv_len,
                                                nameinfo_flags_to_unix( params->flags ) ) );
}


const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_getaddrinfo,
    unix_gethostbyaddr,
    unix_gethostbyname,
    unix_gethostname,
    unix_getnameinfo,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == ws_unix_funcs_count );

#ifdef _WIN64

typedef ULONG PTR32;

#define WOW64_RESOLVER_STRING_MAX 0x10000u
#define WOW64_GUEST_PAGE_SIZE     0x1000u

struct wow64_getaddrinfo_params
{
    PTR32 node;
    PTR32 service;
    PTR32 hints;
    PTR32 info;
    PTR32 size;
};

struct wow64_gethostbyaddr_params
{
    PTR32 addr;
    int len;
    int family;
    PTR32 host;
    PTR32 size;
};

struct wow64_gethostbyname_params
{
    PTR32 name;
    PTR32 host;
    PTR32 size;
};

struct wow64_gethostname_params
{
    PTR32 name;
    unsigned int size;
};

struct wow64_getnameinfo_params
{
    PTR32 addr;
    int addr_len;
    PTR32 host;
    DWORD host_len;
    PTR32 serv;
    DWORD serv_len;
    unsigned int flags;
};

C_ASSERT( sizeof(struct wow64_getaddrinfo_params) == 20 );
C_ASSERT( sizeof(struct wow64_gethostbyaddr_params) == 20 );
C_ASSERT( sizeof(struct wow64_gethostbyname_params) == 12 );
C_ASSERT( sizeof(struct wow64_gethostname_params) == 8 );
C_ASSERT( sizeof(struct wow64_getnameinfo_params) == 28 );

struct WS_addrinfo32
{
    int   ai_flags;
    int   ai_family;
    int   ai_socktype;
    int   ai_protocol;
    PTR32 ai_addrlen;
    PTR32 ai_canonname;
    PTR32 ai_addr;
    PTR32 ai_next;
};

struct WS_hostent32
{
    PTR32 h_name;
    PTR32 h_aliases;
    short h_addrtype;
    short h_length;
    PTR32 h_addr_list;
};

struct wow64_publish_range
{
    PTR32 address;
    const void *data;
    SIZE_T size;
};

union wow64_ws_sockaddr
{
    struct WS_sockaddr addr;
    struct WS_sockaddr_in in;
    struct WS_sockaddr_in6 in6;
#ifdef HAS_IPX
    struct WS_sockaddr_ipx ipx;
#endif
#ifdef HAS_IRDA
    SOCKADDR_IRDA irda;
#endif
};

static NTSTATUS wow64_guest_pointer( PTR32 address, void **ptr )
{
    return ntdll_wow64_guest32_to_host( address, ptr ) ? WSAEFAULT : 0;
}

static BOOL wow64_guest_range_valid( PTR32 address, SIZE_T size )
{
    return !size || size - 1 <= MAXDWORD - address;
}

static NTSTATUS wow64_read_guest( void *dst, PTR32 address, SIZE_T size )
{
    void *src;

    if (!size) return 0;
    if (!wow64_guest_range_valid( address, size ) || wow64_guest_pointer( address, &src ))
        return WSAEFAULT;
    return ntdll_wow64_copy_from_user( dst, src, size ) ? WSAEFAULT : 0;
}

static NTSTATUS wow64_publish( const struct wow64_publish_range *publish, unsigned int count )
{
    struct ntdll_wow64_user_write_range ranges[2];
    unsigned int i, range_count = 0;

    if (count > ARRAY_SIZE(ranges)) return WSAEFAULT;
    for (i = 0; i < count; ++i)
    {
        void *dst;

        if (!publish[i].size) continue;
        if (!wow64_guest_range_valid( publish[i].address, publish[i].size ) ||
            wow64_guest_pointer( publish[i].address, &dst ))
            return WSAEFAULT;
        ranges[range_count].dst = dst;
        ranges[range_count].src = publish[i].data;
        ranges[range_count].size = publish[i].size;
        ++range_count;
    }
    if (!range_count) return 0;
    return ntdll_wow64_atomic_writev( ranges, range_count ) ? WSAEFAULT : 0;
}

static NTSTATUS wow64_publish_one( PTR32 address, const void *data, SIZE_T size )
{
    const struct wow64_publish_range publish = {address, data, size};

    return wow64_publish( &publish, 1 );
}

static BOOL wow64_guest_offset( PTR32 base, SIZE_T offset, PTR32 *address )
{
    if (offset > MAXDWORD - base) return FALSE;
    *address = base + offset;
    return TRUE;
}

static NTSTATUS wow64_capture_string_tail( PTR32 address, char first, char **result )
{
    SIZE_T capacity = 64, len = 1;
    char *buffer, *new_buffer, *nul;
    NTSTATUS status;

    if (!(buffer = malloc( capacity ))) return WSAENOBUFS;
    buffer[0] = first;
    if (!first)
    {
        *result = buffer;
        return 0;
    }

    while (len < WOW64_RESOLVER_STRING_MAX)
    {
        SIZE_T chunk, required;
        PTR32 current;

        if (len > MAXDWORD - address)
        {
            free( buffer );
            return WSAEFAULT;
        }
        current = address + len;
        chunk = min( (SIZE_T)WOW64_RESOLVER_STRING_MAX - len,
                     (SIZE_T)WOW64_GUEST_PAGE_SIZE - (current & (WOW64_GUEST_PAGE_SIZE - 1)) );
        required = len + chunk;
        if (required > capacity)
        {
            SIZE_T new_capacity = min( max( capacity * 2, required ),
                                       (SIZE_T)WOW64_RESOLVER_STRING_MAX );

            if (!(new_buffer = realloc( buffer, new_capacity )))
            {
                free( buffer );
                return WSAENOBUFS;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }
        if ((status = wow64_read_guest( buffer + len, current, chunk )))
        {
            free( buffer );
            return status;
        }
        if ((nul = memchr( buffer + len, 0, chunk )))
        {
            *result = buffer;
            return 0;
        }
        len = required;
    }

    free( buffer );
    return WSAEFAULT;
}

static NTSTATUS wow64_capture_string( PTR32 address, char **result )
{
    char first;
    NTSTATUS status;

    *result = NULL;
    if (!address) return 0;
    if ((status = wow64_read_guest( &first, address, 1 ))) return status;
    return wow64_capture_string_tail( address, first, result );
}

static NTSTATUS put_addrinfo32( const struct WS_addrinfo *info, void *buffer, PTR32 guest_base,
                                unsigned int capacity, unsigned int *needed_size )
{
    const struct WS_addrinfo *src;
    SIZE_T needed = 0, offset, prev_offset = ~(SIZE_T)0;
    BYTE *dst = buffer;

    for (src = info; src != NULL; src = src->ai_next)
    {
        if (!add_size_checked( &needed, sizeof(struct WS_addrinfo32) ) ||
            (src->ai_canonname && !add_cstring_size_checked( &needed, src->ai_canonname )) ||
            !add_size_checked( &needed, src->ai_addrlen ) || needed > MAXDWORD)
            return WSAENOBUFS;
    }

    *needed_size = (unsigned int)needed;
    if (capacity < needed)
    {
        return ERROR_INSUFFICIENT_BUFFER;
    }

    memset( buffer, 0, needed );
    offset = 0;

    for (src = info; src != NULL; src = src->ai_next)
    {
        struct WS_addrinfo32 entry = {0};
        SIZE_T entry_offset = offset;
        PTR32 address;

        if (!add_size_checked( &offset, sizeof(entry) )) return WSAENOBUFS;
        entry.ai_flags = src->ai_flags;
        entry.ai_family = src->ai_family;
        entry.ai_socktype = src->ai_socktype;
        entry.ai_protocol = src->ai_protocol;
        if (src->ai_canonname)
        {
            SIZE_T len, next;

            if (!get_cstring_size( src->ai_canonname, &len )) return WSAENOBUFS;
            if (!wow64_guest_offset( guest_base, offset, &entry.ai_canonname )) return WSAEFAULT;
            next = offset;
            if (!add_size_checked( &next, len ) || next > needed) return WSAENOBUFS;
            memcpy( dst + offset, src->ai_canonname, len );
            offset = next;
        }
        if (src->ai_addrlen > MAXDWORD) return WSAENOBUFS;
        entry.ai_addrlen = (PTR32)src->ai_addrlen;
        if (!wow64_guest_offset( guest_base, offset, &entry.ai_addr )) return WSAEFAULT;
        {
            SIZE_T next = offset;

            if (!add_size_checked( &next, src->ai_addrlen ) || next > needed) return WSAENOBUFS;
            memcpy( dst + offset, src->ai_addr, src->ai_addrlen );
            offset = next;
        }

        if (prev_offset != ~(SIZE_T)0)
        {
            if (!wow64_guest_offset( guest_base, entry_offset, &address )) return WSAEFAULT;
            memcpy( dst + prev_offset + offsetof(struct WS_addrinfo32, ai_next),
                    &address, sizeof(address) );
        }
        memcpy( dst + entry_offset, &entry, sizeof(entry) );
        prev_offset = entry_offset;
    }
    return 0;
}

#ifdef HAVE_GETADDRINFO
static NTSTATUS wow64_addrinfo_from_unix( const struct addrinfo *unix_info,
                                          const struct WS_addrinfo *hints, PTR32 guest_base,
                                          unsigned int capacity, void **wire,
                                          unsigned int *needed_size )
{
    struct getaddrinfo_params params = {0};
    unsigned int native_size = 0;
    void *native_info = NULL;
    NTSTATUS status;

    *wire = NULL;
    *needed_size = 0;
    params.hints = hints;
    params.size = &native_size;
    status = addrinfo_from_unix( &params, unix_info );
    if (status != ERROR_INSUFFICIENT_BUFFER) return status;
    if (!(native_info = malloc( max(native_size, 1u) ))) return WSAENOBUFS;

    params.info = native_info;
    status = addrinfo_from_unix( &params, unix_info );
    if (status) goto done;

    status = put_addrinfo32( native_info, NULL, guest_base, 0, needed_size );
    if (status != ERROR_INSUFFICIENT_BUFFER) goto done;
    if (capacity < *needed_size) goto done;
    if (!guest_base || !wow64_guest_range_valid( guest_base, *needed_size ))
    {
        status = WSAEFAULT;
        goto done;
    }
    if (!(*wire = malloc( max(*needed_size, 1u) )))
    {
        status = WSAENOBUFS;
        goto done;
    }
    status = put_addrinfo32( native_info, *wire, guest_base, capacity, needed_size );

done:
    free( native_info );
    return status;
}
#endif

static NTSTATUS put_hostent32( const struct WS_hostent *host, void *buffer, PTR32 guest_base,
                               unsigned int capacity, unsigned int *needed_size )
{
    SIZE_T alias_count = 0, addr_count = 0, i;
    SIZE_T needed = sizeof(struct WS_hostent32), aliases_offset, addr_list_offset, offset;
    struct WS_hostent32 host32 = {0};
    BYTE *dst = buffer;
    PTR32 address;

    if (host->h_length < 0 || !add_cstring_size_checked( &needed, host->h_name ))
        return WSAENOBUFS;

    for (alias_count = 0; host->h_aliases[alias_count] != NULL; ++alias_count)
        if (!add_size_checked( &needed, sizeof(ULONG) ) ||
            !add_cstring_size_checked( &needed, host->h_aliases[alias_count] ))
            return WSAENOBUFS;
    if (!add_size_checked( &needed, sizeof(ULONG) )) return WSAENOBUFS; /* null terminator */

    for (addr_count = 0; host->h_addr_list[addr_count] != NULL; ++addr_count)
        if (!add_size_checked( &needed, sizeof(ULONG) ) ||
            !add_size_checked( &needed, host->h_length )) return WSAENOBUFS;
    if (!add_size_checked( &needed, sizeof(ULONG) ) || needed > MAXDWORD)
        return WSAENOBUFS; /* null terminator */

    *needed_size = (unsigned int)needed;
    if (capacity < needed)
    {
        return ERROR_INSUFFICIENT_BUFFER;
    }

    memset( buffer, 0, needed );

    /* arrange the memory in the same order as windows >= XP */
    host32.h_addrtype = host->h_addrtype;
    host32.h_length = host->h_length;
    aliases_offset = sizeof(host32);
    addr_list_offset = aliases_offset;
    if (!add_array_size_checked( &addr_list_offset, alias_count + 1, sizeof(ULONG) ))
        return WSAENOBUFS;
    offset = addr_list_offset;
    if (!add_array_size_checked( &offset, addr_count + 1, sizeof(ULONG) ) || offset > needed)
        return WSAENOBUFS;
    if (!wow64_guest_offset( guest_base, aliases_offset, &host32.h_aliases ) ||
        !wow64_guest_offset( guest_base, addr_list_offset, &host32.h_addr_list ))
        return WSAEFAULT;

    for (i = 0; i < addr_count; ++i)
    {
        SIZE_T next = offset;

        if (!wow64_guest_offset( guest_base, offset, &address )) return WSAEFAULT;
        if (!add_size_checked( &next, host->h_length ) || next > needed) return WSAENOBUFS;
        memcpy( dst + addr_list_offset + i * sizeof(address), &address, sizeof(address) );
        memcpy( dst + offset, host->h_addr_list[i], host->h_length );
        offset = next;
    }

    for (i = 0; i < alias_count; ++i)
    {
        SIZE_T len, next;

        if (!get_cstring_size( host->h_aliases[i], &len )) return WSAENOBUFS;
        if (!wow64_guest_offset( guest_base, offset, &address )) return WSAEFAULT;
        next = offset;
        if (!add_size_checked( &next, len ) || next > needed) return WSAENOBUFS;
        memcpy( dst + aliases_offset + i * sizeof(address), &address, sizeof(address) );
        memcpy( dst + offset, host->h_aliases[i], len );
        offset = next;
    }

    if (!wow64_guest_offset( guest_base, offset, &host32.h_name )) return WSAEFAULT;
    {
        SIZE_T len, next;

        if (!get_cstring_size( host->h_name, &len )) return WSAENOBUFS;
        next = offset;
        if (!add_size_checked( &next, len ) || next != needed) return WSAENOBUFS;
        memcpy( dst + offset, host->h_name, len );
    }
    memcpy( dst, &host32, sizeof(host32) );
    return 0;
}

struct wow64_hostent_result
{
    PTR32 guest_base;
    unsigned int capacity;
    unsigned int needed_size;
    void *wire;
};

static NTSTATUS wow64_hostent_result( const struct hostent *unix_host, void *context )
{
    struct wow64_hostent_result *result = context;
    struct WS_hostent *native_host = NULL;
    unsigned int native_size = 0;
    NTSTATUS status;

    status = hostent_from_unix( unix_host, NULL, &native_size );
    if (status != ERROR_INSUFFICIENT_BUFFER) return status;
    if (!(native_host = malloc( max(native_size, 1u) ))) return WSAENOBUFS;
    status = hostent_from_unix( unix_host, native_host, &native_size );
    if (status) goto done;

    status = put_hostent32( native_host, NULL, result->guest_base, 0, &result->needed_size );
    if (status != ERROR_INSUFFICIENT_BUFFER) goto done;
    if (result->capacity < result->needed_size) goto done;
    if (!result->guest_base || !wow64_guest_range_valid( result->guest_base, result->needed_size ))
    {
        status = WSAEFAULT;
        goto done;
    }
    if (!(result->wire = malloc( max(result->needed_size, 1u) )))
    {
        status = WSAENOBUFS;
        goto done;
    }
    status = put_hostent32( native_host, result->wire, result->guest_base,
                            result->capacity, &result->needed_size );

done:
    free( native_host );
    return status;
}

static NTSTATUS wow64_publish_size( PTR32 address, unsigned int size )
{
    return wow64_publish_one( address, &size, sizeof(size) );
}

static NTSTATUS wow64_unix_getaddrinfo( void *args )
{
    const struct wow64_getaddrinfo_params *params32 = args;
    struct WS_addrinfo hints = {0};
    unsigned int capacity, needed = 0;
    char service_first, *node = NULL, *service_buffer = NULL;
    const char *service = NULL;
    void *wire = NULL;
    NTSTATUS status;
#ifdef HAVE_GETADDRINFO
    struct addrinfo unix_hints = {0}, *unix_info = NULL;
    int ret;
#else
    struct getaddrinfo_params params = {0};
#endif

    if (params32->hints)
    {
        if ((status = wow64_read_guest( &hints, params32->hints,
                                        offsetof(struct WS_addrinfo, ai_addrlen) )))
            return status;
#ifndef HAVE_GETADDRINFO
        params.hints = &hints;
#endif
    }
    if ((status = wow64_read_guest( &capacity, params32->size, sizeof(capacity) ))) return status;

    if (params32->service)
    {
        if ((status = wow64_read_guest( &service_first, params32->service, 1 ))) goto done;
        if (!service_first) service = "0";
    }
#ifdef HAVE_GETADDRINFO
    if ((status = prepare_addrinfo_hints( params32->hints ? &hints : NULL, &unix_hints ))) goto done;
#endif
    if (params32->service && service_first)
    {
        if ((status = wow64_capture_string_tail( params32->service, service_first,
                                                  &service_buffer ))) goto done;
        service = service_buffer;
    }
    if ((status = wow64_capture_string( params32->node, &node ))) goto done;
#ifdef HAVE_GETADDRINFO
    if ((ret = getaddrinfo( node, service, params32->hints ? &unix_hints : NULL, &unix_info )))
        status = addrinfo_err_from_unix( ret );
    else
        status = wow64_addrinfo_from_unix( unix_info, params32->hints ? &hints : NULL,
                                           params32->info, capacity, &wire, &needed );
    if (unix_info) freeaddrinfo( unix_info );
#else
    status = unix_getaddrinfo_result( &params, node, service, NULL );
#endif
    if (status == ERROR_INSUFFICIENT_BUFFER)
    {
        status = wow64_publish_size( params32->size, needed );
        if (!status) status = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }
    if (status) goto done;
    status = wow64_publish_one( params32->info, wire, needed );

done:
    free( service_buffer );
    free( node );
    free( wire );
    return status;
}


static NTSTATUS wow64_unix_gethostbyaddr( void *args )
{
    const struct wow64_gethostbyaddr_params *params32 = args;
    struct wow64_hostent_result result = {params32->host};
    unsigned int capacity;
    void *addr = NULL;
    NTSTATUS status;

    if ((status = wow64_read_guest( &capacity, params32->size, sizeof(capacity) ))) return status;
    result.capacity = capacity;
    family_to_unix( params32->family );
    if (params32->len < 0 || params32->len > 256)
    {
        status = WSAEFAULT;
        goto done;
    }
    if (!(addr = malloc( max(params32->len, 1) )))
    {
        status = WSAENOBUFS;
        goto done;
    }
    if ((status = wow64_read_guest( addr, params32->addr, params32->len ))) goto done;
    status = unix_gethostbyaddr_result( addr, params32->len, params32->family,
                                        wow64_hostent_result, &result );
    if (status == ERROR_INSUFFICIENT_BUFFER)
    {
        status = wow64_publish_size( params32->size, result.needed_size );
        if (!status) status = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }
    if (status) goto done;
    status = wow64_publish_one( params32->host, result.wire, result.needed_size );

done:
    free( addr );
    free( result.wire );
    return status;
}


static NTSTATUS wow64_unix_gethostbyname( void *args )
{
    const struct wow64_gethostbyname_params *params32 = args;
    struct wow64_hostent_result result = {params32->host};
    unsigned int capacity;
    char *name = NULL;
    NTSTATUS status;

    if ((status = wow64_read_guest( &capacity, params32->size, sizeof(capacity) ))) return status;
    result.capacity = capacity;
    if (!params32->name)
    {
        status = WSAEFAULT;
        goto done;
    }
    if ((status = wow64_capture_string( params32->name, &name ))) goto done;
    status = unix_gethostbyname_result( name, wow64_hostent_result, &result );
    if (status == ERROR_INSUFFICIENT_BUFFER)
    {
        status = wow64_publish_size( params32->size, result.needed_size );
        if (!status) status = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }
    if (status) goto done;
    status = wow64_publish_one( params32->host, result.wire, result.needed_size );

done:
    free( name );
    free( result.wire );
    return status;
}


static NTSTATUS wow64_unix_gethostname( void *args )
{
    const struct wow64_gethostname_params *params32 = args;
    struct gethostname_params params;
    unsigned int capacity;
    SIZE_T len;
    NTSTATUS status;

    capacity = min( params32->size, WOW64_RESOLVER_STRING_MAX );
    if (!(params.name = malloc( max( capacity, 1u ) ))) return WSAENOBUFS;
    params.size = capacity;
    status = unix_gethostname( &params );
    if (!status)
    {
        len = strnlen( params.name, capacity );
        if (len == capacity) status = WSAEFAULT;
        else status = wow64_publish_one( params32->name, params.name, len + 1 );
    }
    free( params.name );
    return status;
}


static NTSTATUS wow64_capture_sockaddr( PTR32 address, int length, union wow64_ws_sockaddr *addr )
{
    USHORT family;
    SIZE_T size = 0;
    NTSTATUS status;

    memset( addr, 0, sizeof(*addr) );
    if ((status = wow64_read_guest( &family, address, sizeof(family) ))) return status;
    addr->addr.sa_family = family;

    switch (family)
    {
    case WS_AF_INET:
        if (length >= sizeof(addr->in)) size = sizeof(addr->in);
        break;
    case WS_AF_INET6:
        if (length >= sizeof(addr->in6)) size = sizeof(addr->in6);
        break;
#ifdef HAS_IPX
    case WS_AF_IPX:
        if (length >= sizeof(addr->ipx)) size = sizeof(addr->ipx);
        break;
#endif
#ifdef HAS_IRDA
    case WS_AF_IRDA:
        if (length >= sizeof(addr->irda)) size = sizeof(addr->irda);
        break;
#endif
    }
    if (size) return wow64_read_guest( addr, address, size );
    return 0;
}

static NTSTATUS wow64_unix_getnameinfo( void *args )
{
    const struct wow64_getnameinfo_params *params32 = args;
    union wow64_ws_sockaddr addr;
    struct getnameinfo_params params = {0};
    struct wow64_publish_range publish[2];
    unsigned int count = 0;
    DWORD host_capacity = min( params32->host_len, WOW64_RESOLVER_STRING_MAX );
    DWORD serv_capacity = min( params32->serv_len, WOW64_RESOLVER_STRING_MAX );
    NTSTATUS status;
    SIZE_T len;

    if ((status = wow64_capture_sockaddr( params32->addr, params32->addr_len, &addr ))) return status;
    params.addr = &addr.addr;
    params.addr_len = params32->addr_len;
    params.host_len = params32->host ? host_capacity : params32->host_len;
    params.serv_len = params32->serv ? serv_capacity : params32->serv_len;
    params.flags = params32->flags;
    if (params32->host && !(params.host = malloc( max( host_capacity, 1u ) ))) return WSAENOBUFS;
    if (params32->serv && !(params.serv = malloc( max( serv_capacity, 1u ) )))
    {
        free( params.host );
        return WSAENOBUFS;
    }

    status = unix_getnameinfo( &params );
    if (!status && params.host && host_capacity)
    {
        len = strnlen( params.host, host_capacity );
        if (len == host_capacity) status = WSAEFAULT;
        else
        {
            publish[count].address = params32->host;
            publish[count].data = params.host;
            publish[count++].size = len + 1;
        }
    }
    if (!status && params.serv && serv_capacity)
    {
        len = strnlen( params.serv, serv_capacity );
        if (len == serv_capacity) status = WSAEFAULT;
        else
        {
            publish[count].address = params32->serv;
            publish[count].data = params.serv;
            publish[count++].size = len + 1;
        }
    }
    if (!status) status = wow64_publish( publish, count );
    free( params.serv );
    free( params.host );
    return status;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_unix_getaddrinfo,
    wow64_unix_gethostbyaddr,
    wow64_unix_gethostbyname,
    wow64_unix_gethostname,
    wow64_unix_getnameinfo,
};

static const struct wine_unixlib_dispatch_entry_v2 wow64_dispatch_metadata[] =
{
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_getaddrinfo_params,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_gethostbyaddr_params,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_gethostbyname_params,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_gethostname_params,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_getnameinfo_params,
                                   WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
};

WINE_UNIXLIB_DISPATCH_SOURCE_V2(__wine_unix_call_wow64_funcs, wow64_dispatch_metadata);

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == ws_unix_funcs_count );
C_ASSERT( ARRAYSIZE(wow64_dispatch_metadata) == ws_unix_funcs_count );

#endif  /* _WIN64 */
