/*
 * WoW64 private definitions
 *
 * Copyright 2021 Alexandre Julliard
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

#ifndef __WOW64WIN_PRIVATE_H
#define __WOW64WIN_PRIVATE_H

#include "../win32u/win32syscalls.h"
#include "ntuser.h"
#include "wine/wow64_user.h"

#define SYSCALL_ENTRY(id,name,_args) extern NTSTATUS WINAPI wow64_ ## name( UINT *args );
ALL_SYSCALLS32
#undef SYSCALL_ENTRY

extern ntuser_callback user_callbacks[];

struct object_attr64
{
    OBJECT_ATTRIBUTES   attr;
    UNICODE_STRING      str;
    SECURITY_DESCRIPTOR sd;
    SECURITY_QUALITY_OF_SERVICE qos;
};

typedef struct
{
    ULONG Length;
    ULONG RootDirectory;
    ULONG ObjectName;
    ULONG Attributes;
    ULONG SecurityDescriptor;
    ULONG SecurityQualityOfService;
} OBJECT_ATTRIBUTES32;

static inline ULONG get_ulong( UINT **args ) { return *(*args)++; }
static inline HANDLE get_handle( UINT **args ) { return LongToHandle( *(*args)++ ); }
static inline void *get_ptr( UINT **args )
{
    return wine_wow64_guest_memory_ptr( *(*args)++ );
}

/* NtUser pointer arguments denote guest memory unless a wrapper explicitly
 * identifies the value as a client callback or module address. */
static inline void *wow64win_guest_memory_ptr( ULONG address )
{
    return wine_wow64_guest_memory_ptr( address );
}

static inline ULONG wow64win_guest_memory_addr( const void *address )
{
    return wine_wow64_guest_memory_addr( address );
}

static inline void *get_memory_ptr( UINT **args )
{
    return wow64win_guest_memory_ptr( get_ulong( args ) );
}

static inline void *get_client_ptr( UINT **args )
{
    return ULongToPtr( get_ulong( args ) );
}

static inline void *wow64win_guest_memory_or_atom_ptr( ULONG address )
{
    if (!HIWORD( address )) return ULongToPtr( address );
    return wow64win_guest_memory_ptr( address );
}

static inline void *get_memory_or_atom_ptr( UINT **args )
{
    return wow64win_guest_memory_or_atom_ptr( get_ulong( args ) );
}

static inline void wow64win_read_user( void *dst, const void *src, SIZE_T size )
{
    NTSTATUS status = wine_wow64_copy_from_user( dst, src, size );
    if (status) RtlRaiseStatus( status );
}

static inline void wow64win_write_user( void *dst, const void *src, SIZE_T size )
{
    NTSTATUS status = wine_wow64_copy_to_user( dst, src, size );
    if (status) RtlRaiseStatus( status );
}

static inline NTSTATUS wow64win_try_write_user( void *dst, const void *src, SIZE_T size )
{
    return wine_wow64_try_copy_to_user( dst, src, size );
}

static inline void wow64win_probe_user_read( const void *src, SIZE_T size )
{
    NTSTATUS status = wine_wow64_probe_user_read( src, size );
    if (status) RtlRaiseStatus( status );
}

static inline void wow64win_probe_user_write( void *dst, SIZE_T size )
{
    NTSTATUS status = wine_wow64_probe_user_write( dst, size );
    if (status) RtlRaiseStatus( status );
}

static inline void **addr_32to64( void **addr, ULONG *addr32 )
{
    if (!addr32) return NULL;
    *addr = wow64win_guest_memory_ptr( *addr32 );
    return addr;
}

static inline SIZE_T *size_32to64( SIZE_T *size, ULONG *size32 )
{
    if (!size32) return NULL;
    *size = *size32;
    return size;
}

static inline void put_addr( ULONG *addr32, void *addr )
{
    if (addr32) *addr32 = wow64win_guest_memory_addr( addr );
}

static inline void put_size( ULONG *size32, SIZE_T size )
{
    if (size32) *size32 = min( size, MAXDWORD );
}

static inline UNICODE_STRING *unicode_str_32to64( UNICODE_STRING *str, const UNICODE_STRING32 *str32 )
{
    UNICODE_STRING32 local;

    if (!str32) return NULL;
    wow64win_read_user( &local, str32, sizeof(local) );
    str->Length = local.Length;
    str->MaximumLength = local.MaximumLength;
    str->Buffer = wow64win_guest_memory_ptr( local.Buffer );
    return str;
}

static inline SECURITY_DESCRIPTOR *secdesc_32to64( SECURITY_DESCRIPTOR *out, const SECURITY_DESCRIPTOR *in )
{
    /* relative descr has the same layout for 32 and 64 */
    SECURITY_DESCRIPTOR_RELATIVE sd;

    if (!in) return NULL;
    wow64win_read_user( &sd, in, sizeof(sd) );
    out->Revision = sd.Revision;
    out->Sbz1     = sd.Sbz1;
    out->Control  = sd.Control & ~SE_SELF_RELATIVE;
    if (sd.Control & SE_SELF_RELATIVE)
    {
        out->Owner = sd.Owner ? (PSID)((BYTE *)in + sd.Owner) : NULL;
        out->Group = sd.Group ? (PSID)((BYTE *)in + sd.Group) : NULL;
        out->Sacl = ((sd.Control & SE_SACL_PRESENT) && sd.Sacl) ? (PSID)((BYTE *)in + sd.Sacl) : NULL;
        out->Dacl = ((sd.Control & SE_DACL_PRESENT) && sd.Dacl) ? (PSID)((BYTE *)in + sd.Dacl) : NULL;
    }
    else
    {
        out->Owner = wow64win_guest_memory_ptr( sd.Owner );
        out->Group = wow64win_guest_memory_ptr( sd.Group );
        out->Sacl = (sd.Control & SE_SACL_PRESENT) ? wow64win_guest_memory_ptr( sd.Sacl ) : NULL;
        out->Dacl = (sd.Control & SE_DACL_PRESENT) ? wow64win_guest_memory_ptr( sd.Dacl ) : NULL;
    }
    return out;
}

static inline OBJECT_ATTRIBUTES *objattr_32to64( struct object_attr64 *out, const OBJECT_ATTRIBUTES32 *in )
{
    OBJECT_ATTRIBUTES32 local;

    memset( out, 0, sizeof(*out) );
    if (!in) return NULL;
    wow64win_read_user( &local, in, sizeof(local) );
    if (local.Length != sizeof(local)) return &out->attr;

    out->attr.Length = sizeof(out->attr);
    out->attr.RootDirectory = LongToHandle( local.RootDirectory );
    out->attr.Attributes = local.Attributes;
    out->attr.ObjectName = unicode_str_32to64( &out->str,
                                               wow64win_guest_memory_ptr( local.ObjectName ));
    if (local.SecurityQualityOfService)
    {
        SECURITY_QUALITY_OF_SERVICE qos32;

        wow64win_read_user( &qos32, wow64win_guest_memory_ptr( local.SecurityQualityOfService ),
                            sizeof(qos32) );
        out->qos.Length = qos32.Length;
        out->qos.ImpersonationLevel = qos32.ImpersonationLevel;
        out->qos.ContextTrackingMode = qos32.ContextTrackingMode;
        out->qos.EffectiveOnly = qos32.EffectiveOnly;
        out->attr.SecurityQualityOfService = &out->qos;
    }
    out->attr.SecurityDescriptor = secdesc_32to64( &out->sd,
                                                   wow64win_guest_memory_ptr( local.SecurityDescriptor ));
    return &out->attr;
}

static inline void set_last_error32( DWORD err )
{
    TEB *teb = NtCurrentTeb();
    TEB32 *teb32 = (TEB32 *)((char *)teb + teb->WowTebOffset);
    teb32->LastErrorValue = err;
}

#endif /* __WOW64WIN_PRIVATE_H */
