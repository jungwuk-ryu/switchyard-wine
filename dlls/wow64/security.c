/*
 * WoW64 security functions
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

#include <stdarg.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "wow64_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);


static SIZE_T checked_array_size( SIZE_T offset, ULONG count, SIZE_T elem_size )
{
    SIZE_T size;

    if (count > (~(SIZE_T)0 - offset) / elem_size) RtlRaiseStatus( STATUS_INTEGER_OVERFLOW );
    size = offset + count * elem_size;
    if (size > MAXDWORD) RtlRaiseStatus( STATUS_INTEGER_OVERFLOW );
    return size;
}

static ULONG guest_addr_add( const void *base, SIZE_T offset )
{
    ULONG addr = wow64_guest_memory_addr( base );

    if (offset > MAXDWORD - addr) RtlRaiseStatus( STATUS_INTEGER_OVERFLOW );
    return addr + offset;
}

static void *alloc_temp( SIZE_T size )
{
    void *ret;

    if (!(ret = Wow64AllocateTemp( size ))) RtlRaiseStatus( STATUS_NO_MEMORY );
    return ret;
}

static SID *sid_from_user( const SID *sid32 )
{
    SID header, *sid;
    SIZE_T size;

    if (!sid32) return NULL;
    wow64_read_user( &header, sid32, offsetof( SID, SubAuthority ) );
    size = offsetof( SID, SubAuthority[header.SubAuthorityCount] );
    sid = alloc_temp( size );
    wow64_read_user( sid, sid32, size );
    if (sid->SubAuthorityCount != header.SubAuthorityCount) RtlRaiseStatus( STATUS_INVALID_SID );
    return sid;
}

static ACL *acl_from_user( const ACL *acl32 )
{
    ACL header, *acl;

    if (!acl32) return NULL;
    wow64_read_user( &header, acl32, sizeof(header) );
    if (header.AclSize < sizeof(header)) RtlRaiseStatus( STATUS_INVALID_ACL );
    acl = alloc_temp( header.AclSize );
    wow64_read_user( acl, acl32, header.AclSize );
    if (acl->AclSize != header.AclSize) RtlRaiseStatus( STATUS_INVALID_ACL );
    return acl;
}

static SECURITY_DESCRIPTOR *security_descriptor_from_user( SECURITY_DESCRIPTOR *out,
                                                            const SECURITY_DESCRIPTOR *in )
{
    SECURITY_DESCRIPTOR_RELATIVE sd;
    SECURITY_DESCRIPTOR *ret = out;

    if (!in) return NULL;
    wow64_read_user( &sd, in, sizeof(sd) );
    ret->Revision = sd.Revision;
    ret->Sbz1 = sd.Sbz1;
    ret->Control = sd.Control & ~SE_SELF_RELATIVE;
    if (sd.Control & SE_SELF_RELATIVE)
    {
        ret->Owner = sd.Owner ? wow64_guest_memory_ptr( guest_addr_add( in, sd.Owner )) : NULL;
        ret->Group = sd.Group ? wow64_guest_memory_ptr( guest_addr_add( in, sd.Group )) : NULL;
        ret->Sacl = ((sd.Control & SE_SACL_PRESENT) && sd.Sacl) ?
            wow64_guest_memory_ptr( guest_addr_add( in, sd.Sacl )) : NULL;
        ret->Dacl = ((sd.Control & SE_DACL_PRESENT) && sd.Dacl) ?
            wow64_guest_memory_ptr( guest_addr_add( in, sd.Dacl )) : NULL;
    }
    else
    {
        ret->Owner = wow64_guest_memory_ptr( sd.Owner );
        ret->Group = wow64_guest_memory_ptr( sd.Group );
        ret->Sacl = (sd.Control & SE_SACL_PRESENT) ? wow64_guest_memory_ptr( sd.Sacl ) : NULL;
        ret->Dacl = (sd.Control & SE_DACL_PRESENT) ? wow64_guest_memory_ptr( sd.Dacl ) : NULL;
    }
    ret->Owner = sid_from_user( ret->Owner );
    ret->Group = sid_from_user( ret->Group );
    if (ret->Control & SE_SACL_PRESENT) ret->Sacl = acl_from_user( ret->Sacl );
    if (ret->Control & SE_DACL_PRESENT) ret->Dacl = acl_from_user( ret->Dacl );
    return ret;
}

static UNICODE_STRING *unicode_string_from_user( UNICODE_STRING *out,
                                                  const UNICODE_STRING32 *in )
{
    UNICODE_STRING32 value;
    WCHAR *buffer = NULL;

    if (!in) return NULL;
    wow64_read_user( &value, in, sizeof(value) );
    if (value.Length > value.MaximumLength || (value.Length & 1))
        RtlRaiseStatus( STATUS_INVALID_PARAMETER );
    if (value.Buffer)
    {
        buffer = alloc_temp( max( value.Length, (USHORT)sizeof(WCHAR) ) );
        if (value.Length)
            wow64_read_user( buffer, wow64_guest_memory_ptr( value.Buffer ), value.Length );
    }
    out->Length = value.Length;
    out->MaximumLength = value.MaximumLength;
    out->Buffer = buffer;
    return out;
}

static TOKEN_PRIVILEGES *token_privileges_from_user( const TOKEN_PRIVILEGES *privs32 )
{
    TOKEN_PRIVILEGES header, *privs;
    SIZE_T size;

    if (!privs32) return NULL;
    wow64_read_user( &header.PrivilegeCount, privs32, sizeof(header.PrivilegeCount) );
    size = checked_array_size( offsetof( TOKEN_PRIVILEGES, Privileges ),
                               header.PrivilegeCount, sizeof(header.Privileges[0]) );
    privs = alloc_temp( size );
    wow64_read_user( privs, privs32, size );
    if (privs->PrivilegeCount != header.PrivilegeCount)
        RtlRaiseStatus( STATUS_INVALID_PARAMETER );
    return privs;
}

static PRIVILEGE_SET *privilege_set_from_user( const PRIVILEGE_SET *privs32, SIZE_T *ret_size )
{
    PRIVILEGE_SET header, *privs;
    SIZE_T size;

    if (!privs32) return NULL;
    wow64_read_user( &header, privs32, offsetof( PRIVILEGE_SET, Privilege ) );
    size = checked_array_size( offsetof( PRIVILEGE_SET, Privilege ),
                               header.PrivilegeCount, sizeof(header.Privilege[0]) );
    privs = alloc_temp( size );
    wow64_read_user( privs, privs32, size );
    if (privs->PrivilegeCount != header.PrivilegeCount)
        RtlRaiseStatus( STATUS_INVALID_PARAMETER );
    if (ret_size) *ret_size = size;
    return privs;
}

static TOKEN_GROUPS *token_groups_32to64( const TOKEN_GROUPS32 *groups32 )
{
    TOKEN_GROUPS32 *snapshot;
    TOKEN_GROUPS *groups;
    SIZE_T size32, size;
    ULONG i;

    if (!groups32) return NULL;
    wow64_read_user( &i, groups32, sizeof(i) );
    size32 = checked_array_size( offsetof( TOKEN_GROUPS32, Groups ), i,
                                 sizeof(groups32->Groups[0]) );
    size = checked_array_size( offsetof( TOKEN_GROUPS, Groups ), i,
                               sizeof(groups->Groups[0]) );
    snapshot = alloc_temp( size32 );
    wow64_read_user( snapshot, groups32, size32 );
    if (snapshot->GroupCount != i) RtlRaiseStatus( STATUS_INVALID_PARAMETER );
    groups = alloc_temp( size );
    groups->GroupCount = i;
    for (i = 0; i < groups->GroupCount; i++)
    {
        groups->Groups[i].Sid = sid_from_user( wow64_guest_memory_ptr( snapshot->Groups[i].Sid ));
        groups->Groups[i].Attributes = snapshot->Groups[i].Attributes;
    }
    return groups;
}

static OBJECT_TYPE_LIST *objtypelist_32to64( const OBJECT_TYPE_LIST32 *list32, ULONG count )
{
    OBJECT_TYPE_LIST32 *snapshot;
    OBJECT_TYPE_LIST *list;
    SIZE_T size32, size;
    ULONG i;

    if (!list32) return NULL;
    size32 = checked_array_size( 0, count, sizeof(*list32) );
    size = checked_array_size( 0, count, sizeof(*list) );
    snapshot = alloc_temp( size32 );
    list = alloc_temp( size );
    wow64_read_user( snapshot, list32, size32 );
    for (i = 0; i < count; i++)
    {
        list[i].Level = snapshot[i].Level;
        list[i].Sbz = snapshot[i].Sbz;
        if (snapshot[i].ObjectType)
        {
            GUID *guid = alloc_temp( sizeof(*guid) );

            wow64_read_user( guid, wow64_guest_memory_ptr( snapshot[i].ObjectType ), sizeof(*guid) );
            list[i].ObjectType = guid;
        }
        else list[i].ObjectType = NULL;
    }
    return list;
}

static void prepare_handle_output( ULONG *handle_ptr )
{
    ULONG handle = 0;

    wow64_write_user( handle_ptr, &handle, sizeof(handle) );
}

static NTSTATUS publish_local_handle( ULONG *handle_ptr, HANDLE handle, NTSTATUS status )
{
    NTSTATUS publish_status;

    if (!handle) return status;
    if (!(publish_status = try_put_handle( handle_ptr, handle ))) return status;
    NtClose( handle );
    return publish_status;
}


/**********************************************************************
 *           wow64_NtAccessCheck
 */
NTSTATUS WINAPI wow64_NtAccessCheck( UINT *args )
{
    SECURITY_DESCRIPTOR *sd32 = get_ptr( &args );
    HANDLE handle = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    GENERIC_MAPPING *mapping = get_ptr( &args );
    PRIVILEGE_SET *privs = get_ptr( &args );
    ULONG *retlen = get_ptr( &args );
    ACCESS_MASK *access_granted = get_ptr( &args );
    NTSTATUS *access_status = get_ptr( &args );

    SECURITY_DESCRIPTOR sd;
    GENERIC_MAPPING mapping_buf;
    PRIVILEGE_SET *privs_buf;
    SECURITY_DESCRIPTOR *sd64;
    ACCESS_MASK granted_buf = 0;
    NTSTATUS access_status_buf = STATUS_SUCCESS, status;
    ULONG guest_len, call_len;

    if (!privs || !retlen) return STATUS_ACCESS_VIOLATION;
    wow64_read_user( &guest_len, retlen, sizeof(guest_len) );
    sd64 = security_descriptor_from_user( &sd, sd32 );
    wow64_read_user( &mapping_buf, mapping, sizeof(mapping_buf) );
    call_len = max( guest_len, (ULONG)offsetof( PRIVILEGE_SET, Privilege ) );
    privs_buf = alloc_temp( call_len );
    memset( privs_buf, 0, call_len );
    status = NtAccessCheck( sd64, handle, access, &mapping_buf,
                            privs_buf, &call_len, &granted_buf, &access_status_buf );
    if (!status && guest_len < call_len) status = STATUS_BUFFER_TOO_SMALL;
    if (!status)
    {
        wow64_write_user( privs, privs_buf, call_len );
        wow64_write_user( access_granted, &granted_buf, sizeof(granted_buf) );
        wow64_write_user( access_status, &access_status_buf, sizeof(access_status_buf) );
    }
    if (!status || status == STATUS_BUFFER_TOO_SMALL)
        wow64_write_user( retlen, &call_len, sizeof(call_len) );
    return status;
}


/**********************************************************************
 *           wow64_NtAccessCheckAndAuditAlarm
 */
NTSTATUS WINAPI wow64_NtAccessCheckAndAuditAlarm( UINT *args )
{
    UNICODE_STRING32 *subsystem32 = get_ptr( &args );
    HANDLE handle = get_handle( &args );
    UNICODE_STRING32 *typename32 = get_ptr( &args );
    UNICODE_STRING32 *objname32 = get_ptr( &args );
    SECURITY_DESCRIPTOR *sd32 = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    GENERIC_MAPPING *mapping = get_ptr( &args );
    BOOLEAN creation = get_ulong( &args );
    ACCESS_MASK *access_granted = get_ptr( &args );
    NTSTATUS *access_status = get_ptr( &args );
    BOOLEAN *onclose = get_ptr( &args );

    UNICODE_STRING subsystem, typename, objname;
    SECURITY_DESCRIPTOR sd;
    GENERIC_MAPPING mapping_buf;
    UNICODE_STRING *subsystem64, *typename64, *objname64;
    SECURITY_DESCRIPTOR *sd64;
    ACCESS_MASK granted_buf = 0;
    NTSTATUS access_status_buf = STATUS_SUCCESS, status;
    BOOLEAN onclose_buf = FALSE;

    wow64_read_user( &mapping_buf, mapping, sizeof(mapping_buf) );
    subsystem64 = unicode_string_from_user( &subsystem, subsystem32 );
    typename64 = unicode_string_from_user( &typename, typename32 );
    objname64 = unicode_string_from_user( &objname, objname32 );
    sd64 = security_descriptor_from_user( &sd, sd32 );
    wow64_probe_user_write( access_granted, sizeof(granted_buf) );
    wow64_probe_user_write( access_status, sizeof(access_status_buf) );
    wow64_probe_user_write( onclose, sizeof(onclose_buf) );
    status = NtAccessCheckAndAuditAlarm( subsystem64, handle, typename64, objname64,
                                         sd64, access,
                                         &mapping_buf, creation, &granted_buf,
                                         &access_status_buf, &onclose_buf );
    if (NT_SUCCESS(status))
    {
        wow64_write_user( access_granted, &granted_buf, sizeof(granted_buf) );
        wow64_write_user( access_status, &access_status_buf, sizeof(access_status_buf) );
        wow64_write_user( onclose, &onclose_buf, sizeof(onclose_buf) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtAccessCheckByTypeAndAuditAlarm
 */
NTSTATUS WINAPI wow64_NtAccessCheckByTypeAndAuditAlarm( UINT *args )
{
    UNICODE_STRING32 *subsystem32 = get_ptr( &args );
    HANDLE handle = get_handle( &args );
    UNICODE_STRING32 *typename32 = get_ptr( &args );
    UNICODE_STRING32 *objname32 = get_ptr( &args );
    SECURITY_DESCRIPTOR *sd32 = get_ptr( &args );
    SID *sid = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    AUDIT_EVENT_TYPE audit_type = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    OBJECT_TYPE_LIST32 *list32 = get_ptr( &args );
    ULONG list_len = get_ulong( &args );
    GENERIC_MAPPING *mapping = get_ptr( &args );
    BOOLEAN creation = get_ulong( &args );
    ACCESS_MASK *access_granted = get_ptr( &args );
    NTSTATUS *access_status = get_ptr( &args );
    BOOLEAN *onclose = get_ptr( &args );

    UNICODE_STRING subsystem, typename, objname;
    SECURITY_DESCRIPTOR sd;
    GENERIC_MAPPING mapping_buf;
    UNICODE_STRING *subsystem64, *typename64, *objname64;
    SECURITY_DESCRIPTOR *sd64;
    SID *sid64;
    OBJECT_TYPE_LIST *obj_list;
    ACCESS_MASK granted_buf = 0;
    NTSTATUS access_status_buf = STATUS_SUCCESS, status;
    BOOLEAN onclose_buf = FALSE;

    wow64_read_user( &mapping_buf, mapping, sizeof(mapping_buf) );
    subsystem64 = unicode_string_from_user( &subsystem, subsystem32 );
    typename64 = unicode_string_from_user( &typename, typename32 );
    objname64 = unicode_string_from_user( &objname, objname32 );
    sd64 = security_descriptor_from_user( &sd, sd32 );
    sid64 = sid_from_user( sid );
    obj_list = objtypelist_32to64( list32, list_len );
    wow64_probe_user_write( access_granted, sizeof(granted_buf) );
    wow64_probe_user_write( access_status, sizeof(access_status_buf) );
    wow64_probe_user_write( onclose, sizeof(onclose_buf) );
    status = NtAccessCheckByTypeAndAuditAlarm( subsystem64, handle, typename64, objname64,
                                               sd64, sid64, access, audit_type, flags,
                                               obj_list, list_len,
                                               &mapping_buf, creation, &granted_buf,
                                               &access_status_buf, &onclose_buf );
    if (NT_SUCCESS(status))
    {
        wow64_write_user( access_granted, &granted_buf, sizeof(granted_buf) );
        wow64_write_user( access_status, &access_status_buf, sizeof(access_status_buf) );
        wow64_write_user( onclose, &onclose_buf, sizeof(onclose_buf) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtAdjustGroupsToken
 */
NTSTATUS WINAPI wow64_NtAdjustGroupsToken( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN reset = get_ulong( &args );
    TOKEN_GROUPS32 *groups = get_ptr( &args );
    ULONG len = get_ulong( &args );
    TOKEN_GROUPS32 *prev = get_ptr( &args );
    ULONG *retlen = get_ptr( &args );

    FIXME( "%p %d %p %lu %p %p\n", handle, reset, groups, len, prev, retlen );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtAdjustPrivilegesToken
 */
NTSTATUS WINAPI wow64_NtAdjustPrivilegesToken( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN disable = get_ulong( &args );
    TOKEN_PRIVILEGES *privs = get_ptr( &args );
    ULONG len = get_ulong( &args );
    TOKEN_PRIVILEGES *prev = get_ptr( &args );
    ULONG *retlen = get_ptr( &args );

    TOKEN_PRIVILEGES *privs_buf = disable ? NULL : token_privileges_from_user( privs );
    TOKEN_PRIVILEGES *prev_buf = NULL;
    ULONG retlen_buf = 0, copy_size = 0;
    NTSTATUS status;

    if (prev)
    {
        SIZE_T size = max( len, (ULONG)offsetof( TOKEN_PRIVILEGES, Privileges ) );

        prev_buf = alloc_temp( size );
        memset( prev_buf, 0, size );
        wow64_probe_user_write( prev, size );
        if (retlen) wow64_probe_user_write( retlen, sizeof(retlen_buf) );
    }
    status = NtAdjustPrivilegesToken( handle, disable, privs_buf, len, prev_buf,
                                      prev ? &retlen_buf : NULL );
    if (prev)
    {
        copy_size = max( (ULONG)offsetof( TOKEN_PRIVILEGES, Privileges ),
                         min( len, retlen_buf ) );
        wow64_write_user( prev, prev_buf, copy_size );
        if (retlen) wow64_write_user( retlen, &retlen_buf, sizeof(retlen_buf) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtCloseObjectAuditAlarm
 */
NTSTATUS WINAPI wow64_NtCloseObjectAuditAlarm( UINT *args )
{
    UNICODE_STRING32 *subsystem32 = get_ptr( &args );
    HANDLE handle = get_handle( &args );
    BOOLEAN onclose = get_ulong( &args );

    UNICODE_STRING subsystem;

    return NtCloseObjectAuditAlarm( unicode_string_from_user( &subsystem, subsystem32 ),
                                    handle, onclose );
}


/**********************************************************************
 *           wow64_NtCreateLowBoxToken
 */
NTSTATUS WINAPI wow64_NtCreateLowBoxToken( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    HANDLE token = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    SID *sid = get_ptr( &args );
    ULONG count = get_ulong( &args );
    SID_AND_ATTRIBUTES32 *capabilities32 = get_ptr( &args );
    ULONG handle_count = get_ulong( &args );
    ULONG *handles32 = get_ptr( &args );

    FIXME( "%p %p %lx %p %p %lu %p %lu %p: stub\n",
           handle_ptr, token, access, attr32, sid, count, capabilities32, handle_count, handles32 );

    prepare_handle_output( handle_ptr );
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           wow64_NtCreateToken
 */
NTSTATUS WINAPI wow64_NtCreateToken( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    TOKEN_TYPE type = get_ulong( &args );
    LUID *luid = get_ptr( &args );
    LARGE_INTEGER *expire = get_ptr( &args );
    TOKEN_USER32 *user32 = get_ptr( &args );
    TOKEN_GROUPS32 *groups32 = get_ptr( &args );
    TOKEN_PRIVILEGES *privs = get_ptr( &args );
    TOKEN_OWNER32 *owner32 = get_ptr( &args );
    TOKEN_PRIMARY_GROUP32 *group32 = get_ptr( &args );
    TOKEN_DEFAULT_DACL32 *dacl32 = get_ptr( &args );
    TOKEN_SOURCE *source = get_ptr( &args );

    struct object_attr64 attr;
    OBJECT_ATTRIBUTES *attr64;
    LUID luid_buf, *luid64 = NULL;
    LARGE_INTEGER expire_buf, *expire64 = NULL;
    TOKEN_USER user;
    TOKEN_USER *user64;
    TOKEN_GROUPS *groups64;
    TOKEN_PRIVILEGES *privs64;
    TOKEN_OWNER owner;
    TOKEN_OWNER *owner64;
    TOKEN_PRIMARY_GROUP group;
    TOKEN_PRIMARY_GROUP *group64;
    TOKEN_DEFAULT_DACL dacl;
    TOKEN_DEFAULT_DACL *dacl64;
    TOKEN_SOURCE source_buf, *source64 = NULL;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    attr64 = objattr_32to64( &attr, attr32 );
    if (luid)
    {
        wow64_read_user( &luid_buf, luid, sizeof(luid_buf) );
        luid64 = &luid_buf;
    }
    if (expire)
    {
        wow64_read_user( &expire_buf, expire, sizeof(expire_buf) );
        expire64 = &expire_buf;
    }
    if ((user64 = token_user_32to64( &user, user32 )))
        user64->User.Sid = sid_from_user( user64->User.Sid );
    groups64 = token_groups_32to64( groups32 );
    privs64 = token_privileges_from_user( privs );
    if ((owner64 = token_owner_32to64( &owner, owner32 )))
        owner64->Owner = sid_from_user( owner64->Owner );
    if ((group64 = token_primary_group_32to64( &group, group32 )))
        group64->PrimaryGroup = sid_from_user( group64->PrimaryGroup );
    if ((dacl64 = token_default_dacl_32to64( &dacl, dacl32 )))
        dacl64->DefaultDacl = acl_from_user( dacl64->DefaultDacl );
    if (source)
    {
        wow64_read_user( &source_buf, source, sizeof(source_buf) );
        source64 = &source_buf;
    }
    status = NtCreateToken( &handle, access, attr64, type, luid64, expire64,
                            user64, groups64, privs64, owner64, group64, dacl64, source64 );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtDuplicateToken
 */
NTSTATUS WINAPI wow64_NtDuplicateToken( UINT *args )
{
    HANDLE token = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    BOOLEAN effective_only = get_ulong( &args );
    TOKEN_TYPE type = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtDuplicateToken( token, access, objattr_32to64( &attr, attr32 ), effective_only, type, &handle );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtFilterToken
 */
NTSTATUS WINAPI wow64_NtFilterToken( UINT *args )
{
    HANDLE token = get_handle( &args );
    ULONG flags = get_ulong( &args );
    TOKEN_GROUPS32 *disable_sids32 = get_ptr( &args );
    TOKEN_PRIVILEGES *privs = get_ptr( &args );
    TOKEN_GROUPS32 *restrict_sids32 = get_ptr( &args );
    ULONG *handle_ptr = get_ptr( &args );

    TOKEN_GROUPS *disable_sids, *restrict_sids;
    TOKEN_PRIVILEGES *privileges;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    disable_sids = token_groups_32to64( disable_sids32 );
    privileges = token_privileges_from_user( privs );
    restrict_sids = token_groups_32to64( restrict_sids32 );
    status = NtFilterToken( token, flags, disable_sids, privileges, restrict_sids, &handle );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCompareTokens
 */
NTSTATUS WINAPI wow64_NtCompareTokens( UINT *args )
{
    HANDLE first = get_handle( &args );
    HANDLE second = get_handle( &args );
    BOOLEAN *equal = get_ptr( &args );

    BOOLEAN value = FALSE;
    NTSTATUS status;

    status = NtCompareTokens( first, second, &value );
    if (!status) wow64_write_user( equal, &value, sizeof(value) );
    return status;
}

/**********************************************************************
 *           wow64_NtImpersonateAnonymousToken
 */
NTSTATUS WINAPI wow64_NtImpersonateAnonymousToken( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtImpersonateAnonymousToken( handle );
}


/**********************************************************************
 *           wow64_NtOpenProcessToken
 */
NTSTATUS WINAPI wow64_NtOpenProcessToken( UINT *args )
{
    HANDLE process = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenProcessToken( process, access, &handle );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenProcessTokenEx
 */
NTSTATUS WINAPI wow64_NtOpenProcessTokenEx( UINT *args )
{
    HANDLE process = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    ULONG attributes = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenProcessTokenEx( process, access, attributes, &handle );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenThreadToken
 */
NTSTATUS WINAPI wow64_NtOpenThreadToken( UINT *args )
{
    HANDLE thread = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    BOOLEAN self = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenThreadToken( thread, access, self, &handle );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenThreadTokenEx
 */
NTSTATUS WINAPI wow64_NtOpenThreadTokenEx( UINT *args )
{
    HANDLE thread = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    BOOLEAN self = get_ulong( &args );
    ULONG attributes = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenThreadTokenEx( thread, access, self, attributes, &handle );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtPrivilegeCheck
 */
NTSTATUS WINAPI wow64_NtPrivilegeCheck( UINT *args )
{
    HANDLE token = get_handle( &args );
    PRIVILEGE_SET *privs = get_ptr( &args );
    BOOLEAN *res = get_ptr( &args );

    SIZE_T size;
    PRIVILEGE_SET *local = privilege_set_from_user( privs, &size );
    BOOLEAN value = FALSE;
    NTSTATUS status;

    if (!local) RtlRaiseStatus( STATUS_ACCESS_VIOLATION );
    status = NtPrivilegeCheck( token, (PRIVILEGE_SET *)local, &value );
    if (!status)
    {
        wow64_write_user( privs, local, size );
        wow64_write_user( res, &value, sizeof(value) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtQueryInformationToken
 */
NTSTATUS WINAPI wow64_NtQueryInformationToken( UINT *args )
{
    HANDLE handle = get_handle( &args );
    TOKEN_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;
    ULONG ret_size = 0, sid_len;

    switch (class)
    {
    case TokenPrivileges: /* TOKEN_PRIVILEGES */
    case TokenImpersonationLevel:  /* SECURITY_IMPERSONATION_LEVEL */
    case TokenStatistics:  /* TOKEN_STATISTICS */
    case TokenType: /* TOKEN_TYPE */
    case TokenElevationType:  /* TOKEN_ELEVATION_TYPE */
    case TokenElevation: /* TOKEN_ELEVATION */
    case TokenSessionId:  /* ULONG */
    case TokenVirtualizationEnabled:  /* ULONG */
    case TokenUIAccess:  /* ULONG */
    case TokenIsAppContainer:  /* ULONG */
    {
        void *buffer = len ? alloc_temp( len ) : NULL;

        if (buffer) memset( buffer, 0, len );
        status = NtQueryInformationToken( handle, class, buffer, len, &ret_size );
        if (!status && ret_size) wow64_write_user( info, buffer, min( len, ret_size ) );
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case TokenUser:  /* TOKEN_USER + SID */
    case TokenIntegrityLevel:  /* TOKEN_MANDATORY_LABEL + SID */
    {
        ULONG_PTR buffer[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG_PTR)];
        TOKEN_USER *user = (TOKEN_USER *)buffer;
        TOKEN_USER32 *user32 = info;
        SID *sid;

        status = NtQueryInformationToken( handle, class, &buffer, sizeof(buffer), &ret_size );
        if (status) return status;
        sid = user->User.Sid;
        if ((char *)sid < (char *)buffer ||
            (char *)sid > (char *)buffer + sizeof(buffer) - offsetof( SID, SubAuthority ))
            return STATUS_INVALID_SID;
        sid_len = offsetof( SID, SubAuthority[sid->SubAuthorityCount] );
        if (sid_len > (char *)buffer + sizeof(buffer) - (char *)sid) return STATUS_INVALID_SID;
        if (len >= sizeof(*user32) + sid_len)
        {
            TOKEN_USER32 *out = alloc_temp( sizeof(*out) + sid_len );

            out->User.Sid = guest_addr_add( info, sizeof(*out) );
            out->User.Attributes = user->User.Attributes;
            memcpy( out + 1, sid, sid_len );
            wow64_write_user( info, out, sizeof(*out) + sid_len );
        }
        else status = STATUS_BUFFER_TOO_SMALL;
        ret_size = sizeof(*user32) + sid_len;
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case TokenOwner:  /* TOKEN_OWNER + SID  */
    case TokenPrimaryGroup:  /* TOKEN_PRIMARY_GROUP + SID */
    case TokenAppContainerSid:  /* TOKEN_APPCONTAINER_INFORMATION + SID */
    {
        ULONG_PTR buffer[(sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE) / sizeof(ULONG_PTR)];
        TOKEN_OWNER *owner = (TOKEN_OWNER *)buffer;
        TOKEN_OWNER32 *owner32 = info;
        SID *sid;

        status = NtQueryInformationToken( handle, class, &buffer, sizeof(buffer), &ret_size );
        if (status) return status;
        sid = owner->Owner;
        if ((char *)sid < (char *)buffer ||
            (char *)sid > (char *)buffer + sizeof(buffer) - offsetof( SID, SubAuthority ))
            return STATUS_INVALID_SID;
        sid_len = offsetof( SID, SubAuthority[sid->SubAuthorityCount] );
        if (sid_len > (char *)buffer + sizeof(buffer) - (char *)sid) return STATUS_INVALID_SID;
        if (len >= sizeof(*owner32) + sid_len)
        {
            TOKEN_OWNER32 *out = alloc_temp( sizeof(*out) + sid_len );

            out->Owner = guest_addr_add( info, sizeof(*out) );
            memcpy( out + 1, sid, sid_len );
            wow64_write_user( info, out, sizeof(*out) + sid_len );
        }
        else status = STATUS_BUFFER_TOO_SMALL;
        ret_size = sizeof(*owner32) + sid_len;
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case TokenGroups:  /* TOKEN_GROUPS */
    case TokenLogonSid:   /* TOKEN_GROUPS */
    {
        TOKEN_GROUPS32 *groups32 = info;
        TOKEN_GROUPS *groups;
        ULONG i, group_len, group32_len;

        status = NtQueryInformationToken( handle, class, NULL, 0, &ret_size );
        if (status != STATUS_BUFFER_TOO_SMALL) return status;
        groups = alloc_temp( ret_size );
        status = NtQueryInformationToken( handle, class, groups, ret_size, &ret_size );
        if (status) return status;
        group_len = checked_array_size( offsetof( TOKEN_GROUPS, Groups ), groups->GroupCount,
                                        sizeof(groups->Groups[0]) );
        group32_len = checked_array_size( offsetof( TOKEN_GROUPS32, Groups ), groups->GroupCount,
                                          sizeof(groups32->Groups[0]) );
        if (group_len > ret_size) return STATUS_INVALID_BUFFER_SIZE;
        sid_len = ret_size - group_len;
        if (group32_len > MAXDWORD - sid_len) return STATUS_INTEGER_OVERFLOW;
        ret_size = group32_len + sid_len;
        if (len >= ret_size)
        {
            SID *sid = (SID *)((char *)groups + group_len);
            SID *sid32;
            TOKEN_GROUPS32 *out = alloc_temp( ret_size );

            memset( out, 0, ret_size );
            sid32 = (SID *)((char *)out + group32_len);
            memcpy( sid32, sid, sid_len );
            out->GroupCount = groups->GroupCount;
            for (i = 0; i < groups->GroupCount; i++)
            {
                SIZE_T offset;

                if ((char *)groups->Groups[i].Sid < (char *)sid ||
                    (char *)groups->Groups[i].Sid >= (char *)sid + sid_len)
                    return STATUS_INVALID_SID;
                offset = (char *)groups->Groups[i].Sid - (char *)sid;
                out->Groups[i].Sid = guest_addr_add( info, group32_len + offset );
                out->Groups[i].Attributes = groups->Groups[i].Attributes;
            }
            wow64_write_user( info, out, ret_size );
        }
        else status = STATUS_BUFFER_TOO_SMALL;
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case TokenDefaultDacl:  /* TOKEN_DEFAULT_DACL + ACL */
    {
        SIZE_T size = (SIZE_T)len + sizeof(TOKEN_DEFAULT_DACL) - sizeof(TOKEN_DEFAULT_DACL32);
        TOKEN_DEFAULT_DACL32 *dacl32 = info;
        TOKEN_DEFAULT_DACL *dacl;

        if (size > MAXDWORD) return STATUS_INTEGER_OVERFLOW;
        dacl = alloc_temp( max( size, sizeof(*dacl) ) );

        status = NtQueryInformationToken( handle, class, dacl, size, &ret_size );
        if (!status)
        {
            ULONG out_size;
            TOKEN_DEFAULT_DACL32 *out;

            if (ret_size < sizeof(*dacl)) return STATUS_INVALID_BUFFER_SIZE;
            if (ret_size - sizeof(*dacl) > MAXDWORD - sizeof(*out)) return STATUS_INTEGER_OVERFLOW;
            out_size = ret_size - sizeof(*dacl) + sizeof(*out);
            out = alloc_temp( out_size );
            out->DefaultDacl = dacl->DefaultDacl ?
                guest_addr_add( info, sizeof(*out) ) : 0;
            if (dacl->DefaultDacl)
            {
                SIZE_T acl_len = out_size - sizeof(*out);

                if ((char *)dacl->DefaultDacl < (char *)dacl + sizeof(*dacl) ||
                    acl_len > ret_size ||
                    (char *)dacl->DefaultDacl > (char *)dacl + ret_size - acl_len)
                    return STATUS_INVALID_ACL;
                memcpy( out + 1, dacl->DefaultDacl, acl_len );
            }
            wow64_write_user( info, out, out_size );
        }
        if (ret_size >= sizeof(*dacl)) ret_size -= sizeof(*dacl) - sizeof(*dacl32);
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case TokenLinkedToken:  /* TOKEN_LINKED_TOKEN */
    {
        TOKEN_LINKED_TOKEN link;
        NTSTATUS publish_status;

        ret_size = sizeof(ULONG);
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        if (len < ret_size) return STATUS_BUFFER_TOO_SMALL;
        wow64_probe_user_write( info, ret_size );
        status = NtQueryInformationToken( handle, class, &link, sizeof(link), &ret_size );
        if (!status)
        {
            publish_status = try_put_handle( info, link.LinkedToken );
            if (publish_status)
            {
                if (link.LinkedToken) NtClose( link.LinkedToken );
                return publish_status;
            }
        }
        return status;
    }

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtQuerySecurityObject
 */
NTSTATUS WINAPI wow64_NtQuerySecurityObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    SECURITY_INFORMATION info = get_ulong( &args );
    SECURITY_DESCRIPTOR *sd = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    SECURITY_DESCRIPTOR *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (buffer) memset( buffer, 0, len );
    status = NtQuerySecurityObject( handle, info, buffer, len, &ret_size );
    if (!status && ret_size) wow64_write_user( sd, buffer, min( len, ret_size ) );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtSetInformationToken
 */
NTSTATUS WINAPI wow64_NtSetInformationToken( UINT *args )
{
    HANDLE handle = get_handle( &args );
    TOKEN_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );

    switch (class)
    {
    case TokenIntegrityLevel: /* TOKEN_MANDATORY_LABEL */
        if (len >= sizeof(TOKEN_MANDATORY_LABEL32))
        {
            TOKEN_MANDATORY_LABEL32 label32;
            TOKEN_MANDATORY_LABEL label;

            wow64_read_user( &label32, ptr, sizeof(label32) );
            label.Label.Sid = sid_from_user( wow64_guest_memory_ptr( label32.Label.Sid ));
            label.Label.Attributes = label32.Label.Attributes;
            return NtSetInformationToken( handle, class, &label, sizeof(label) );
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    case TokenSessionId:   /* ULONG */
        if (len >= sizeof(ULONG))
        {
            ULONG value;

            wow64_read_user( &value, ptr, sizeof(value) );
            return NtSetInformationToken( handle, class, &value, sizeof(value) );
        }
        return STATUS_INFO_LENGTH_MISMATCH;

    case TokenDefaultDacl:   /* TOKEN_DEFAULT_DACL */
        if (len >= sizeof(TOKEN_DEFAULT_DACL32))
        {
            TOKEN_DEFAULT_DACL32 dacl32;
            TOKEN_DEFAULT_DACL dacl;

            wow64_read_user( &dacl32, ptr, sizeof(dacl32) );
            dacl.DefaultDacl = acl_from_user( wow64_guest_memory_ptr( dacl32.DefaultDacl ));
            return NtSetInformationToken( handle, class, &dacl, sizeof(dacl) );
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}


/**********************************************************************
 *           wow64_NtSetSecurityObject
 */
NTSTATUS WINAPI wow64_NtSetSecurityObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    SECURITY_INFORMATION info = get_ulong( &args );
    SECURITY_DESCRIPTOR *sd32 = get_ptr( &args );

    SECURITY_DESCRIPTOR sd;

    return NtSetSecurityObject( handle, info, security_descriptor_from_user( &sd, sd32 ));
}
