/*
 * WoW64 registry functions
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

static NTSTATUS publish_handle_result( ULONG *handle_ptr, HANDLE handle, NTSTATUS status )
{
    NTSTATUS publish_status = try_put_handle( handle_ptr, handle );

    if (!publish_status) return status;
    if (handle) NtClose( handle );
    return publish_status;
}

static inline BOOL registry_query_status( NTSTATUS status )
{
    return status == STATUS_SUCCESS || status == STATUS_BUFFER_TOO_SMALL ||
           status == STATUS_BUFFER_OVERFLOW;
}

static void *alloc_registry_output( ULONG len )
{
    void *ret = Wow64AllocateTemp( len ? len : 1 );

    if (!ret) RtlRaiseStatus( STATUS_NO_MEMORY );
    return ret;
}

static ULONG key_info_fixed_size( KEY_INFORMATION_CLASS class )
{
    switch (class)
    {
    case KeyBasicInformation:  return offsetof(KEY_BASIC_INFORMATION, Name);
    case KeyFullInformation:   return offsetof(KEY_FULL_INFORMATION, Class);
    case KeyNodeInformation:   return offsetof(KEY_NODE_INFORMATION, Name);
    case KeyNameInformation:   return offsetof(KEY_NAME_INFORMATION, Name);
    case KeyCachedInformation: return sizeof(KEY_CACHED_INFORMATION);
    default:                    return 0;
    }
}

static ULONG key_value_info_fixed_size( KEY_VALUE_INFORMATION_CLASS class )
{
    switch (class)
    {
    case KeyValueBasicInformation:   return offsetof(KEY_VALUE_BASIC_INFORMATION, Name);
    case KeyValueFullInformation:    return offsetof(KEY_VALUE_FULL_INFORMATION, Name);
    case KeyValuePartialInformation: return offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data);
    default:                         return 0;
    }
}

/* Registry query helpers receive the variable server reply before filling the
 * fixed header.  Publish in the same order so a later guest fault leaves the
 * same partial result as the native path. */
static void put_registry_info( void *out, const void *local, ULONG len,
                               ULONG result_len, ULONG fixed_size )
{
    ULONG written = min( len, result_len );

    if (written > fixed_size)
        wow64_write_user( (char *)out + fixed_size, (const char *)local + fixed_size,
                          written - fixed_size );
    if (written)
        wow64_write_user( out, local, min( written, fixed_size ) );
}

static void put_registry_value_info( void *out, const void *local, ULONG len,
                                     ULONG result_len, ULONG min_size, ULONG fixed_size )
{
    ULONG written = min( len, result_len );

    /* NtQueryValueKey copies the name before issuing the server request. */
    if (written > min_size && fixed_size > min_size)
    {
        ULONG end = min( written, fixed_size );
        wow64_write_user( (char *)out + min_size, (const char *)local + min_size,
                          end - min_size );
    }
    if (written > fixed_size)
        wow64_write_user( (char *)out + fixed_size, (const char *)local + fixed_size,
                          written - fixed_size );
    if (written)
        wow64_write_user( out, local, min( written, min_size ) );
}


/**********************************************************************
 *           wow64_NtCreateKey
 */
NTSTATUS WINAPI wow64_NtCreateKey( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG index = get_ulong( &args );
    UNICODE_STRING32 *class32 = get_ptr( &args );
    ULONG options = get_ulong( &args );
    ULONG *dispos = get_ptr( &args );

    struct object_attr64 attr;
    UNICODE_STRING class;
    HANDLE handle = 0;
    ULONG disposition;
    NTSTATUS status, publish_status;

    put_handle( handle_ptr, 0 );
    if (dispos) wow64_probe_user_write( dispos, sizeof(*dispos) );
    status = NtCreateKey( &handle, access, objattr_32to64( &attr, attr32 ), index,
                          unicode_str_32to64_temp( &class, class32 ), options,
                          dispos ? &disposition : NULL );
    if (!status && dispos &&
        (publish_status = wow64_try_write_user( dispos, &disposition, sizeof(disposition) )))
    {
        if (handle) NtClose( handle );
        return publish_status;
    }
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateKeyTransacted
 */
NTSTATUS WINAPI wow64_NtCreateKeyTransacted( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG index = get_ulong( &args );
    UNICODE_STRING32 *class32 = get_ptr( &args );
    ULONG options = get_ulong( &args );
    HANDLE transacted = get_handle( &args );
    ULONG *dispos = get_ptr( &args );

    struct object_attr64 attr;
    UNICODE_STRING class;
    HANDLE handle = 0;
    ULONG disposition;
    NTSTATUS status, publish_status;

    put_handle( handle_ptr, 0 );
    if (dispos) wow64_probe_user_write( dispos, sizeof(*dispos) );
    status = NtCreateKeyTransacted( &handle, access, objattr_32to64( &attr, attr32 ), index,
                                    unicode_str_32to64_temp( &class, class32 ), options, transacted,
                                    dispos ? &disposition : NULL );
    if (!status && dispos &&
        (publish_status = wow64_try_write_user( dispos, &disposition, sizeof(disposition) )))
    {
        if (handle) NtClose( handle );
        return publish_status;
    }
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtDeleteKey
 */
NTSTATUS WINAPI wow64_NtDeleteKey( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtDeleteKey( handle );
}


/**********************************************************************
 *           wow64_NtDeleteValueKey
 */
NTSTATUS WINAPI wow64_NtDeleteValueKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    UNICODE_STRING32 *str32 = get_ptr( &args );

    UNICODE_STRING str;
    UNICODE_STRING *name = unicode_str_32to64( &str, str32 );

    if (name && name->Length <= 16383 * sizeof(WCHAR)) unicode_str_32to64_materialize( name );
    return NtDeleteValueKey( handle, name );
}


/**********************************************************************
 *           wow64_NtEnumerateKey
 */
NTSTATUS WINAPI wow64_NtEnumerateKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG index = get_ulong( &args );
    KEY_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    ULONG fixed_size, result_len = 0;
    void *local;
    NTSTATUS status;

    if (index == ~(ULONG)0) return NtEnumerateKey( handle, index, class, ptr, len, retlen );
    if (!(fixed_size = key_info_fixed_size( class )))
        return NtEnumerateKey( handle, index, class, ptr, len, retlen );
    local = alloc_registry_output( len );
    status = NtEnumerateKey( handle, index, class, local, len, &result_len );
    if (registry_query_status( status ))
    {
        put_registry_info( ptr, local, len, result_len, fixed_size );
        wow64_write_user( retlen, &result_len, sizeof(result_len) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtEnumerateValueKey
 */
NTSTATUS WINAPI wow64_NtEnumerateValueKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG index = get_ulong( &args );
    KEY_VALUE_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    ULONG fixed_size, result_len = 0;
    void *local;
    NTSTATUS status;

    if (!(fixed_size = key_value_info_fixed_size( class )))
        return NtEnumerateValueKey( handle, index, class, ptr, len, retlen );
    local = alloc_registry_output( len );
    status = NtEnumerateValueKey( handle, index, class, local, len, &result_len );
    if (registry_query_status( status ))
    {
        put_registry_info( ptr, local, len, result_len, fixed_size );
        wow64_write_user( retlen, &result_len, sizeof(result_len) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtFlushKey
 */
NTSTATUS WINAPI wow64_NtFlushKey( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtFlushKey( handle );
}


/**********************************************************************
 *           wow64_NtLoadKey
 */
NTSTATUS WINAPI wow64_NtLoadKey( UINT *args )
{
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    OBJECT_ATTRIBUTES32 *file32 = get_ptr( &args );

    struct object_attr64 attr, file;

    return NtLoadKey( objattr_32to64( &attr, attr32 ), objattr_32to64( &file, file32 ));
}


/**********************************************************************
 *           wow64_NtLoadKey2
 */
NTSTATUS WINAPI wow64_NtLoadKey2( UINT *args )
{
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    OBJECT_ATTRIBUTES32 *file32 = get_ptr( &args );
    ULONG flags = get_ulong( &args );

    struct object_attr64 attr, file;

    return NtLoadKey2( objattr_32to64( &attr, attr32 ), objattr_32to64( &file, file32 ), flags );
}

/**********************************************************************
 *           wow64_NtLoadKeyEx
 */
NTSTATUS WINAPI wow64_NtLoadKeyEx( UINT *args )
{
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    OBJECT_ATTRIBUTES32 *file32 = get_ptr( &args );
    ULONG flags = get_ulong( &args );
    HANDLE trustkey = get_handle( &args );
    HANDLE event = get_handle( &args );
    ACCESS_MASK desired_access = get_ulong( &args );
    HANDLE *rootkey = get_ptr( &args );
    IO_STATUS_BLOCK32 *io32 = get_ptr( &args );

    struct object_attr64 attr, file;
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtLoadKeyEx( objattr_32to64( &attr, attr32 ), objattr_32to64( &file, file32 ), flags,
                        trustkey, event, desired_access, rootkey, iosb_32to64( &io, io32 ) );
    put_iosb( io32, &io );
    return status;
}


/**********************************************************************
 *           wow64_NtNotifyChangeKey
 */
NTSTATUS WINAPI wow64_NtNotifyChangeKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    HANDLE event = get_handle( &args );
    ULONG apc = get_ulong( &args );
    ULONG apc_param = get_ulong( &args );
    IO_STATUS_BLOCK32 *io32 = get_ptr( &args );
    ULONG filter = get_ulong( &args );
    BOOLEAN subtree = get_ulong( &args );
    void *buffer = get_ptr( &args );
    ULONG len = get_ulong( &args );
    BOOLEAN async = get_ulong( &args );

    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtNotifyChangeKey( handle, event, apc_32to64( apc ), apc_param_32to64( apc, apc_param ),
                                iosb_32to64( &io, io32 ), filter, subtree, buffer, len, async );
    put_iosb( io32, &io );
    return status;
}


/**********************************************************************
 *           wow64_NtNotifyChangeMultipleKeys
 */
NTSTATUS WINAPI wow64_NtNotifyChangeMultipleKeys( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG count = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    HANDLE event = get_handle( &args );
    ULONG apc = get_ulong( &args );
    ULONG apc_param = get_ulong( &args );
    IO_STATUS_BLOCK32 *io32 = get_ptr( &args );
    ULONG filter = get_ulong( &args );
    BOOLEAN subtree = get_ulong( &args );
    void *buffer = get_ptr( &args );
    ULONG len = get_ulong( &args );
    BOOLEAN async = get_ulong( &args );

    struct object_attr64 attr;
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtNotifyChangeMultipleKeys( handle, count, objattr_32to64( &attr, attr32 ), event,
                                         apc_32to64( apc ), apc_param_32to64( apc, apc_param ),
                                         iosb_32to64( &io, io32 ), filter, subtree, buffer, len, async );
    put_iosb( io32, &io );
    return status;
}


/**********************************************************************
 *           wow64_NtOpenKey
 */
NTSTATUS WINAPI wow64_NtOpenKey( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtOpenKey( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenKeyEx
 */
NTSTATUS WINAPI wow64_NtOpenKeyEx( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG options = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtOpenKeyEx( &handle, access, objattr_32to64( &attr, attr32 ), options );
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenKeyTransacted
 */
NTSTATUS WINAPI wow64_NtOpenKeyTransacted( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    HANDLE transaction = get_handle( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtOpenKeyTransacted( &handle, access, objattr_32to64( &attr, attr32 ), transaction );
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenKeyTransactedEx
 */
NTSTATUS WINAPI wow64_NtOpenKeyTransactedEx( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG options = get_ulong( &args );
    HANDLE transaction = get_handle( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtOpenKeyTransactedEx( &handle, access, objattr_32to64( &attr, attr32 ), options, transaction );
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtQueryKey
 */
NTSTATUS WINAPI wow64_NtQueryKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    KEY_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    ULONG fixed_size, result_len = 0;
    void *local;
    NTSTATUS status;

    if (!(fixed_size = key_info_fixed_size( class )))
        return NtQueryKey( handle, class, info, len, retlen );
    local = alloc_registry_output( len );
    status = NtQueryKey( handle, class, local, len, &result_len );
    if (registry_query_status( status ))
    {
        put_registry_info( info, local, len, result_len, fixed_size );
        wow64_write_user( retlen, &result_len, sizeof(result_len) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtQueryMultipleValueKey
 */
NTSTATUS WINAPI wow64_NtQueryMultipleValueKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    KEY_MULTIPLE_VALUE_INFORMATION *info = get_ptr( &args );
    ULONG count = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    FIXME( "%p %p %lu %p %lu %p: stub\n", handle, info, count, ptr, len, retlen );
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           wow64_NtQueryValueKey
 */
NTSTATUS WINAPI wow64_NtQueryValueKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    UNICODE_STRING32 *str32 = get_ptr( &args );
    KEY_VALUE_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    UNICODE_STRING str;
    ULONG min_size = 0, fixed_size = 0, result_len = 0;
    UNICODE_STRING *name;
    void *local;
    NTSTATUS status;

    name = unicode_str_32to64( &str, str32 );
    if (!name) return NtQueryValueKey( handle, NULL, class, ptr, len, retlen );
    switch (class)
    {
    case KeyValueBasicInformation:
        min_size = offsetof(KEY_VALUE_BASIC_INFORMATION, Name);
        fixed_size = min_size + name->Length;
        break;
    case KeyValueFullInformation:
        min_size = offsetof(KEY_VALUE_FULL_INFORMATION, Name);
        fixed_size = min_size + name->Length;
        break;
    case KeyValuePartialInformation:
        min_size = fixed_size = offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data);
        break;
    case KeyValuePartialInformationAlign64:
        min_size = fixed_size = offsetof(KEY_VALUE_PARTIAL_INFORMATION_ALIGN64, Data);
        break;
    default:
        return NtQueryValueKey( handle, name, class, ptr, len, retlen );
    }
    if (name->Length > 16383 * sizeof(WCHAR))
        return NtQueryValueKey( handle, name, class, ptr, len, retlen );
    unicode_str_32to64_materialize( name );

    local = alloc_registry_output( len );
    status = NtQueryValueKey( handle, name, class, local, len, &result_len );
    if (registry_query_status( status ))
    {
        put_registry_value_info( ptr, local, len, result_len, min_size, fixed_size );
        wow64_write_user( retlen, &result_len, sizeof(result_len) );
    }
    else if (name->Length <= 16383 * sizeof(WCHAR) && fixed_size > min_size && len > min_size)
    {
        ULONG name_len = min( len - min_size, (ULONG)name->Length );
        wow64_write_user( (char *)ptr + min_size, (char *)local + min_size, name_len );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtRenameKey
 */
NTSTATUS WINAPI wow64_NtRenameKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    UNICODE_STRING32 *str32 = get_ptr( &args );

    UNICODE_STRING str;
    UNICODE_STRING *name = unicode_str_32to64( &str, str32 );

    if (name && name->Buffer && name->Length) unicode_str_32to64_materialize( name );
    return NtRenameKey( handle, name );
}


/**********************************************************************
 *           wow64_NtReplaceKey
 */
NTSTATUS WINAPI wow64_NtReplaceKey( UINT *args )
{
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    HANDLE handle = get_handle( &args );
    OBJECT_ATTRIBUTES32 *replace32 = get_ptr( &args );

    struct object_attr64 attr, replace;

    return NtReplaceKey( objattr_32to64( &attr, attr32 ), handle, objattr_32to64( &replace, replace32 ));
}


/**********************************************************************
 *           wow64_NtRestoreKey
 */
NTSTATUS WINAPI wow64_NtRestoreKey( UINT *args )
{
    HANDLE key = get_handle( &args );
    HANDLE file = get_handle( &args );
    ULONG flags = get_ulong( &args );

    return NtRestoreKey( key, file, flags );
}


/**********************************************************************
 *           wow64_NtSaveKey
 */
NTSTATUS WINAPI wow64_NtSaveKey( UINT *args )
{
    HANDLE key = get_handle( &args );
    HANDLE file = get_handle( &args );

    return NtSaveKey( key, file );
}


/**********************************************************************
 *           wow64_NtSetInformationKey
 */
NTSTATUS WINAPI wow64_NtSetInformationKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    int class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );

    return NtSetInformationKey( handle, class, info, len );
}


/**********************************************************************
 *           wow64_NtSetValueKey
 */
NTSTATUS WINAPI wow64_NtSetValueKey( UINT *args )
{
    HANDLE handle = get_handle( &args );
    const UNICODE_STRING32 *str32 = get_ptr( &args );
    ULONG index = get_ulong( &args );
    ULONG type = get_ulong( &args );
    const void *data = get_ptr( &args );
    ULONG count = get_ulong( &args );

    UNICODE_STRING str;
    UNICODE_STRING *name = unicode_str_32to64( &str, str32 );

    if (name && name->Length <= 16383 * sizeof(WCHAR)) unicode_str_32to64_materialize( name );
    return NtSetValueKey( handle, name, index, type, data, count );
}


/**********************************************************************
 *           wow64_NtUnloadKey
 */
NTSTATUS WINAPI wow64_NtUnloadKey( UINT *args )
{
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;

    return NtUnloadKey( objattr_32to64( &attr, attr32 ));
}
