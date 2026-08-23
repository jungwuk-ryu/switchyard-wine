/*
 * WoW64 synchronization objects and functions
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


static void *alloc_temp( SIZE_T size )
{
    void *ret;

    if (!(ret = Wow64AllocateTemp( size ))) RtlRaiseStatus( STATUS_NO_MEMORY );
    return ret;
}

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

static LARGE_INTEGER *large_integer_from_user( LARGE_INTEGER *out, const LARGE_INTEGER *in )
{
    if (!in) return NULL;
    wow64_read_user( out, in, sizeof(*out) );
    return out;
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

static NTSTATUS copy_scalar_output( void *dst, const void *src, SIZE_T size, NTSTATUS status )
{
    if (NT_SUCCESS(status) && dst) wow64_write_user( dst, src, size );
    return status;
}

static void put_object_type_info( OBJECT_TYPE_INFORMATION32 *info32,
                                  const OBJECT_TYPE_INFORMATION *info, ULONG guest_addr )
{
    if (info->TypeName.Length)
    {
        if (sizeof(*info32) > MAXDWORD - guest_addr) RtlRaiseStatus( STATUS_INTEGER_OVERFLOW );
        memcpy( info32 + 1, info->TypeName.Buffer, info->TypeName.Length + sizeof(WCHAR) );
        info32->TypeName.Length        = info->TypeName.Length;
        info32->TypeName.MaximumLength = info->TypeName.Length + sizeof(WCHAR);
        info32->TypeName.Buffer        = guest_addr + sizeof(*info32);
    }
    else memset( &info32->TypeName, 0, sizeof(info32->TypeName) );
    info32->TotalNumberOfObjects       = info->TotalNumberOfObjects;
    info32->TotalNumberOfHandles       = info->TotalNumberOfHandles;
    info32->TotalPagedPoolUsage        = info->TotalPagedPoolUsage;
    info32->TotalNonPagedPoolUsage     = info->TotalNonPagedPoolUsage;
    info32->TotalNamePoolUsage         = info->TotalNamePoolUsage;
    info32->TotalHandleTableUsage      = info->TotalHandleTableUsage;
    info32->HighWaterNumberOfObjects   = info->HighWaterNumberOfObjects;
    info32->HighWaterNumberOfHandles   = info->HighWaterNumberOfHandles;
    info32->HighWaterPagedPoolUsage    = info->HighWaterPagedPoolUsage;
    info32->HighWaterNonPagedPoolUsage = info->HighWaterNonPagedPoolUsage;
    info32->HighWaterNamePoolUsage     = info->HighWaterNamePoolUsage;
    info32->HighWaterHandleTableUsage  = info->HighWaterHandleTableUsage;
    info32->InvalidAttributes          = info->InvalidAttributes;
    info32->GenericMapping             = info->GenericMapping;
    info32->ValidAccessMask            = info->ValidAccessMask;
    info32->SecurityRequired           = info->SecurityRequired;
    info32->MaintainHandleCount        = info->MaintainHandleCount;
    info32->TypeIndex                  = info->TypeIndex;
    info32->ReservedByte               = info->ReservedByte;
    info32->PoolType                   = info->PoolType;
    info32->DefaultPagedPoolCharge     = info->DefaultPagedPoolCharge;
    info32->DefaultNonPagedPoolCharge  = info->DefaultNonPagedPoolCharge;
}


static JOBOBJECT_BASIC_LIMIT_INFORMATION *job_basic_limit_info_32to64( JOBOBJECT_BASIC_LIMIT_INFORMATION *out,
                                                                       const JOBOBJECT_BASIC_LIMIT_INFORMATION32 *in )
{
    out->PerProcessUserTimeLimit = in->PerProcessUserTimeLimit;
    out->PerJobUserTimeLimit     = in->PerJobUserTimeLimit;
    out->LimitFlags              = in->LimitFlags;
    out->MinimumWorkingSetSize   = in->MinimumWorkingSetSize;
    out->MaximumWorkingSetSize   = in->MaximumWorkingSetSize;
    out->ActiveProcessLimit      = in->ActiveProcessLimit;
    out->Affinity                = in->Affinity;
    out->PriorityClass           = in->PriorityClass;
    out->SchedulingClass         = in->SchedulingClass;
    return out;
}


static void put_job_basic_limit_info( JOBOBJECT_BASIC_LIMIT_INFORMATION32 *info32,
                                      const JOBOBJECT_BASIC_LIMIT_INFORMATION *info )
{
    info32->PerProcessUserTimeLimit = info->PerProcessUserTimeLimit;
    info32->PerJobUserTimeLimit     = info->PerJobUserTimeLimit;
    info32->LimitFlags              = info->LimitFlags;
    info32->MinimumWorkingSetSize   = info->MinimumWorkingSetSize;
    info32->MaximumWorkingSetSize   = info->MaximumWorkingSetSize;
    info32->ActiveProcessLimit      = info->ActiveProcessLimit;
    info32->Affinity                = info->Affinity;
    info32->PriorityClass           = info->PriorityClass;
    info32->SchedulingClass         = info->SchedulingClass;
}


void put_section_image_info( SECTION_IMAGE_INFORMATION32 *info32, const SECTION_IMAGE_INFORMATION *info )
{
    if (info->Machine == IMAGE_FILE_MACHINE_AMD64 || info->Machine == IMAGE_FILE_MACHINE_ARM64)
    {
        info32->TransferAddress    = 0x81231234;  /* sic */
        info32->MaximumStackSize   = 0x100000;
        info32->CommittedStackSize = 0x10000;
    }
    else
    {
        info32->TransferAddress    = wow64_guest_memory_addr( info->TransferAddress );
        info32->MaximumStackSize   = info->MaximumStackSize;
        info32->CommittedStackSize = info->CommittedStackSize;
    }
    info32->ZeroBits                    = info->ZeroBits;
    info32->SubSystemType               = info->SubSystemType;
    info32->MinorSubsystemVersion       = info->MinorSubsystemVersion;
    info32->MajorSubsystemVersion       = info->MajorSubsystemVersion;
    info32->MajorOperatingSystemVersion = info->MajorOperatingSystemVersion;
    info32->MinorOperatingSystemVersion = info->MinorOperatingSystemVersion;
    info32->ImageCharacteristics        = info->ImageCharacteristics;
    info32->DllCharacteristics          = info->DllCharacteristics;
    info32->Machine                     = info->Machine;
    info32->ImageContainsCode           = info->ImageContainsCode;
    info32->ImageFlags                  = info->ImageFlags;
    info32->LoaderFlags                 = info->LoaderFlags;
    info32->ImageFileSize               = info->ImageFileSize;
    info32->CheckSum                    = info->CheckSum;
}


/**********************************************************************
 *           wow64_NtAcceptConnectPort
 */
NTSTATUS WINAPI wow64_NtAcceptConnectPort( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ULONG id = get_ulong( &args );
    LPC_MESSAGE *msg = get_ptr( &args );
    BOOLEAN accept = get_ulong( &args );
    LPC_SECTION_WRITE *write = get_ptr( &args );
    LPC_SECTION_READ *read = get_ptr( &args );

    FIXME( "%p %lu %p %u %p %p: stub\n", handle_ptr, id, msg, accept, write, read );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtCancelTimer
 */
NTSTATUS WINAPI wow64_NtCancelTimer( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN *state = get_ptr( &args );

    BOOLEAN value = FALSE;
    NTSTATUS status;

    if (state) wow64_probe_user_write( state, sizeof(value) );
    status = NtCancelTimer( handle, state ? &value : NULL );
    return copy_scalar_output( state, &value, sizeof(value), status );
}


/**********************************************************************
 *           wow64_NtClearEvent
 */
NTSTATUS WINAPI wow64_NtClearEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtClearEvent( handle );
}


/**********************************************************************
 *           wow64_NtCompareObjects
 */
NTSTATUS WINAPI wow64_NtCompareObjects( UINT *args )
{
    HANDLE first = get_handle( &args );
    HANDLE second = get_handle( &args );

    return NtCompareObjects( first, second );
}


/**********************************************************************
 *           wow64_NtCompleteConnectPort
 */
NTSTATUS WINAPI wow64_NtCompleteConnectPort( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtCompleteConnectPort( handle );
}


/**********************************************************************
 *           wow64_NtConnectPort
 */
NTSTATUS WINAPI wow64_NtConnectPort( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    UNICODE_STRING32 *name32 = get_ptr( &args );
    SECURITY_QUALITY_OF_SERVICE *qos = get_ptr( &args );
    LPC_SECTION_WRITE *write = get_ptr( &args );
    LPC_SECTION_READ *read = get_ptr( &args );
    ULONG *max_len = get_ptr( &args );
    void *info = get_ptr( &args );
    ULONG *info_len = get_ptr( &args );

    FIXME( "%p %p %p %p %p %p %p %p: stub\n",
           handle_ptr, name32, qos, write, read, max_len, info, info_len );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtCreateDebugObject
 */
NTSTATUS WINAPI wow64_NtCreateDebugObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG flags = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateDebugObject( &handle, access, objattr_32to64( &attr, attr32 ), flags );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateDirectoryObject
 */
NTSTATUS WINAPI wow64_NtCreateDirectoryObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateDirectoryObject( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateEvent
 */
NTSTATUS WINAPI wow64_NtCreateEvent( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    EVENT_TYPE type = get_ulong( &args );
    BOOLEAN state = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateEvent( &handle, access, objattr_32to64( &attr, attr32 ), type, state );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateIoCompletion
 */
NTSTATUS WINAPI wow64_NtCreateIoCompletion( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG threads = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateIoCompletion( &handle, access, objattr_32to64( &attr, attr32 ), threads );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateJobObject
 */
NTSTATUS WINAPI wow64_NtCreateJobObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateJobObject( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateKeyedEvent
 */
NTSTATUS WINAPI wow64_NtCreateKeyedEvent( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG flags = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateKeyedEvent( &handle, access, objattr_32to64( &attr, attr32 ), flags );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateMutant
 */
NTSTATUS WINAPI wow64_NtCreateMutant( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    BOOLEAN owned = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateMutant( &handle, access, objattr_32to64( &attr, attr32 ), owned );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreatePort
 */
NTSTATUS WINAPI wow64_NtCreatePort( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    ULONG info_len = get_ulong( &args );
    ULONG data_len = get_ulong( &args );
    ULONG *reserved = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreatePort( &handle, objattr_32to64( &attr, attr32 ), info_len, data_len, reserved );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateSection
 */
NTSTATUS WINAPI wow64_NtCreateSection( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    const LARGE_INTEGER *size = get_ptr( &args );
    ULONG protect = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    HANDLE file = get_handle( &args );

    struct object_attr64 attr;
    LARGE_INTEGER size_buf;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateSection( &handle, access, objattr_32to64( &attr, attr32 ),
                              large_integer_from_user( &size_buf, size ), protect, flags, file );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateSemaphore
 */
NTSTATUS WINAPI wow64_NtCreateSemaphore( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    LONG initial = get_ulong( &args );
    LONG max = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateSemaphore( &handle, access, objattr_32to64( &attr, attr32 ), initial, max );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateSymbolicLinkObject
 */
NTSTATUS WINAPI wow64_NtCreateSymbolicLinkObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    UNICODE_STRING32 *target32 = get_ptr( &args );

    struct object_attr64 attr;
    UNICODE_STRING target;
    OBJECT_ATTRIBUTES *attr64;
    UNICODE_STRING *target64;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    target64 = unicode_string_from_user( &target, target32 );
    attr64 = objattr_32to64( &attr, attr32 );
    status = NtCreateSymbolicLinkObject( &handle, access, attr64, target64 );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCreateTimer
 */
NTSTATUS WINAPI wow64_NtCreateTimer( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    TIMER_TYPE type = get_ulong( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtCreateTimer( &handle, access, objattr_32to64( &attr, attr32 ), type );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtDebugContinue
 */
NTSTATUS WINAPI wow64_NtDebugContinue( UINT *args )
{
    HANDLE handle = get_handle( &args );
    CLIENT_ID32 *id32 = get_ptr( &args );
    NTSTATUS status = get_ulong( &args );

    CLIENT_ID id;

    return NtDebugContinue( handle, client_id_32to64( &id, id32 ), status );
}


/**********************************************************************
 *           wow64_NtDelayExecution
 */
NTSTATUS WINAPI wow64_NtDelayExecution( UINT *args )
{
    BOOLEAN alertable = get_ulong( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    LARGE_INTEGER timeout_buf;

    return NtDelayExecution( alertable, large_integer_from_user( &timeout_buf, timeout ) );
}


/**********************************************************************
 *           wow64_NtDuplicateObject
 */
NTSTATUS WINAPI wow64_NtDuplicateObject( UINT *args )
{
    HANDLE source_process = get_handle( &args );
    HANDLE source_handle = get_handle( &args );
    HANDLE dest_process = get_handle( &args );
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    ULONG attributes = get_ulong( &args );
    ULONG options = get_ulong( &args );

    HANDLE handle = 0, cleanup = 0;
    NTSTATUS status, publish_status;

    if (handle_ptr) prepare_handle_output( handle_ptr );
    status = NtDuplicateObject( source_process, source_handle, dest_process,
                                handle_ptr ? &handle : NULL,
                                access, attributes, options );
    if (!handle_ptr || !handle) return status;
    if (!(publish_status = try_put_handle( handle_ptr, handle ))) return status;

    if (dest_process == NtCurrentProcess()) NtClose( handle );
    else
    {
        NtDuplicateObject( dest_process, handle, NtCurrentProcess(), &cleanup, 0, 0,
                           DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS );
        if (cleanup) NtClose( cleanup );
    }
    return publish_status;
}


/**********************************************************************
 *           wow64_NtImpersonateClientOfPort
 */
NTSTATUS WINAPI wow64_NtImpersonateClientOfPort( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LPC_MESSAGE *msg = get_ptr( &args );

    FIXME( "%p %p: stub\n", handle, msg );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtListenPort
 */
NTSTATUS WINAPI wow64_NtListenPort( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LPC_MESSAGE *msg = get_ptr( &args );

    FIXME( "%p %p: stub\n", handle, msg );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtMakePermanentObject
 */
NTSTATUS WINAPI wow64_NtMakePermanentObject( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtMakePermanentObject( handle );
}


/**********************************************************************
 *           wow64_NtMakeTemporaryObject
 */
NTSTATUS WINAPI wow64_NtMakeTemporaryObject( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtMakeTemporaryObject( handle );
}


/**********************************************************************
 *           wow64_NtOpenDirectoryObject
 */
NTSTATUS WINAPI wow64_NtOpenDirectoryObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenDirectoryObject( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenEvent
 */
NTSTATUS WINAPI wow64_NtOpenEvent( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenEvent( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenIoCompletion
 */
NTSTATUS WINAPI wow64_NtOpenIoCompletion( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenIoCompletion( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenJobObject
 */
NTSTATUS WINAPI wow64_NtOpenJobObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenJobObject( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenKeyedEvent
 */
NTSTATUS WINAPI wow64_NtOpenKeyedEvent( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenKeyedEvent( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenMutant
 */
NTSTATUS WINAPI wow64_NtOpenMutant( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenMutant( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenSection
 */
NTSTATUS WINAPI wow64_NtOpenSection( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenSection( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenSemaphore
 */
NTSTATUS WINAPI wow64_NtOpenSemaphore( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenSemaphore( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenSymbolicLinkObject
 */
NTSTATUS WINAPI wow64_NtOpenSymbolicLinkObject( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenSymbolicLinkObject( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenTimer
 */
NTSTATUS WINAPI wow64_NtOpenTimer( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    status = NtOpenTimer( &handle, access, objattr_32to64( &attr, attr32 ));
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtPulseEvent
 */
NTSTATUS WINAPI wow64_NtPulseEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LONG *prev_state = get_ptr( &args );

    LONG value = 0;
    NTSTATUS status;

    if (prev_state) wow64_probe_user_write( prev_state, sizeof(value) );
    status = NtPulseEvent( handle, prev_state ? &value : NULL );
    return copy_scalar_output( prev_state, &value, sizeof(value), status );
}


/**********************************************************************
 *           wow64_NtQueryDirectoryObject
 */
NTSTATUS WINAPI wow64_NtQueryDirectoryObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    DIRECTORY_BASIC_INFORMATION32 *info32 = get_ptr( &args );
    ULONG size32 = get_ulong( &args );
    BOOLEAN single_entry = get_ulong( &args );
    BOOLEAN restart = get_ulong( &args );
    ULONG *context = get_ptr( &args );
    ULONG *retlen = get_ptr( &args );
    ULONG context_buf, retsize = 0;
    SIZE_T native_size;
    NTSTATUS status;
    DIRECTORY_BASIC_INFORMATION *info;

    context_buf = 0;
    if (!restart) wow64_read_user( &context_buf, context, sizeof(context_buf) );
    native_size = (SIZE_T)size32 + 2 * sizeof(*info) - 2 * sizeof(*info32);
    if (native_size > MAXDWORD) return STATUS_INTEGER_OVERFLOW;
    info = alloc_temp( native_size );
    status = NtQueryDirectoryObject( handle, info, native_size, single_entry, restart,
                                     &context_buf, &retsize );
    if (NT_SUCCESS(status))
    {
        DIRECTORY_BASIC_INFORMATION32 *out = size32 ? alloc_temp( size32 ) : NULL;
        SIZE_T i, count, used_size, used_count, strpool_head;
        SIZE_T validsize = min( native_size, retsize );

        used_count = 0;
        used_size = sizeof(*info32);  /* "null terminator" entry */
        for (count = 0;
             count + 1 <= validsize / sizeof(*info) &&
             info[count].ObjectName.MaximumLength;
             count++)
        {
            SIZE_T entry_size = sizeof(*info32);

            if (info[count].ObjectName.Length > info[count].ObjectName.MaximumLength ||
                info[count].ObjectTypeName.Length > info[count].ObjectTypeName.MaximumLength ||
                info[count].ObjectName.MaximumLength > validsize ||
                info[count].ObjectTypeName.MaximumLength > validsize - info[count].ObjectName.MaximumLength)
                return STATUS_INVALID_BUFFER_SIZE;
            entry_size += info[count].ObjectName.MaximumLength +
                          info[count].ObjectTypeName.MaximumLength;

            if (entry_size <= size32 - min( used_size, (SIZE_T)size32 ))
            {
                used_count++;
                used_size += entry_size;
            }
        }

        if (used_count != count)
        {
            ERR( "64bit dir list (%Iu+%Iu bytes, %Iu entries) truncated for 32bit buffer (%Iu+%Iu bytes, %Iu entries)\n",
                 validsize, native_size - validsize, count, used_size,
                 size32 - min( (SIZE_T)size32, used_size ), used_count );

            if (!status) status = STATUS_MORE_ENTRIES;
            context_buf -= count - used_count;
        }

        /*
         * Avoid making strpool_head a pointer, since it can point beyond end
         * of the buffer.  Out-of-bounds pointers trigger undefined behavior
         * just by existing, even when they are never dereferenced.
         */
        strpool_head = sizeof(*info32) * (used_count + 1);  /* after the "null terminator" entry */
        for (i = 0; i < used_count; i++)
        {
            if ((char *)info[i].ObjectName.Buffer < (char *)info ||
                (char *)info[i].ObjectName.Buffer > (char *)info + validsize -
                                                       info[i].ObjectName.MaximumLength ||
                (char *)info[i].ObjectTypeName.Buffer < (char *)info ||
                (char *)info[i].ObjectTypeName.Buffer > (char *)info + validsize -
                                                           info[i].ObjectTypeName.MaximumLength)
                return STATUS_INVALID_BUFFER_SIZE;
            out[i].ObjectName.Buffer = guest_addr_add( info32, strpool_head );
            out[i].ObjectName.Length = info[i].ObjectName.Length;
            out[i].ObjectName.MaximumLength = info[i].ObjectName.MaximumLength;
            memcpy( (char *)out + strpool_head, info[i].ObjectName.Buffer,
                    info[i].ObjectName.MaximumLength );
            strpool_head += info[i].ObjectName.MaximumLength;

            out[i].ObjectTypeName.Buffer = guest_addr_add( info32, strpool_head );
            out[i].ObjectTypeName.Length = info[i].ObjectTypeName.Length;
            out[i].ObjectTypeName.MaximumLength = info[i].ObjectTypeName.MaximumLength;
            memcpy( (char *)out + strpool_head, info[i].ObjectTypeName.Buffer,
                    info[i].ObjectTypeName.MaximumLength );
            strpool_head += info[i].ObjectTypeName.MaximumLength;
        }

        if (size32 >= sizeof(*info32))
            memset( &out[used_count], 0, sizeof(out[used_count]) );

        if (strpool_head <= size32) wow64_write_user( info32, out, strpool_head );
        wow64_write_user( context, &context_buf, sizeof(context_buf) );
        if (retlen)
        {
            ULONG value = strpool_head;

            wow64_write_user( retlen, &value, sizeof(value) );
        }
    }
    else if (retlen && status == STATUS_BUFFER_TOO_SMALL)
    {
        ULONG value;

        if (retsize < 2 * sizeof(*info) - 2 * sizeof(*info32)) return STATUS_INVALID_BUFFER_SIZE;
        value = retsize - 2 * sizeof(*info) + 2 * sizeof(*info32);
        wow64_write_user( retlen, &value, sizeof(value) );
    }
    else if (retlen && status == STATUS_NO_MORE_ENTRIES)
    {
        ULONG value = 0;

        wow64_write_user( retlen, &value, sizeof(value) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtQueryEvent
 */
NTSTATUS WINAPI wow64_NtQueryEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    EVENT_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    void *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (buffer) memset( buffer, 0, len );
    status = NtQueryEvent( handle, class, buffer, len, &ret_size );
    if (!status && ret_size) wow64_write_user( info, buffer, min( len, ret_size ) );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtQueryInformationJobObject
 */
NTSTATUS WINAPI wow64_NtQueryInformationJobObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    JOBOBJECTINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;

    switch (class)
    {
    case JobObjectBasicAccountingInformation:   /* JOBOBJECT_BASIC_ACCOUNTING_INFORMATION */
    {
        void *buffer = len ? alloc_temp( len ) : NULL;
        ULONG ret_size = 0;

        if (buffer) memset( buffer, 0, len );
        status = NtQueryInformationJobObject( handle, class, buffer, len, &ret_size );
        if (!status && ret_size) wow64_write_user( ptr, buffer, min( len, ret_size ) );
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case JobObjectBasicLimitInformation:   /* JOBOBJECT_BASIC_LIMIT_INFORMATION */
        if (len >= sizeof(JOBOBJECT_BASIC_LIMIT_INFORMATION32))
        {
            JOBOBJECT_BASIC_LIMIT_INFORMATION32 info32;
            JOBOBJECT_BASIC_LIMIT_INFORMATION info;
            ULONG ret_size = sizeof(info32);

            status = NtQueryInformationJobObject( handle, class, &info, sizeof(info), NULL );
            if (!status)
            {
                put_job_basic_limit_info( &info32, &info );
                wow64_write_user( ptr, &info32, sizeof(info32) );
            }
            if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
            return status;
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    case JobObjectBasicProcessIdList:   /* JOBOBJECT_BASIC_PROCESS_ID_LIST */
        if (len >= sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST32))
        {
            JOBOBJECT_BASIC_PROCESS_ID_LIST32 *info32;
            JOBOBJECT_BASIC_PROCESS_ID_LIST *info;
            ULONG i, count, size, size32;

            count = (len - offsetof( JOBOBJECT_BASIC_PROCESS_ID_LIST32, ProcessIdList )) /
                    sizeof(info32->ProcessIdList[0]);
            size = checked_array_size( offsetof( JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList ),
                                       count, sizeof(info->ProcessIdList[0]) );
            size32 = checked_array_size( offsetof( JOBOBJECT_BASIC_PROCESS_ID_LIST32, ProcessIdList ),
                                         count, sizeof(info32->ProcessIdList[0]) );
            info = alloc_temp( size );
            info32 = alloc_temp( size32 );
            memset( info32, 0, size32 );
            status = NtQueryInformationJobObject( handle, class, info, size, NULL );
            if (!status)
            {
                if (info->NumberOfProcessIdsInList > count) return STATUS_INVALID_BUFFER_SIZE;
                info32->NumberOfAssignedProcesses = info->NumberOfAssignedProcesses;
                info32->NumberOfProcessIdsInList  = info->NumberOfProcessIdsInList;
                for (i = 0; i < info->NumberOfProcessIdsInList; i++)
                    info32->ProcessIdList[i] = info->ProcessIdList[i];
                size32 = offsetof( JOBOBJECT_BASIC_PROCESS_ID_LIST32, ProcessIdList[i] );
                wow64_write_user( ptr, info32, size32 );
                if (retlen) wow64_write_user( retlen, &size32, sizeof(size32) );
            }
            return status;
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    case JobObjectExtendedLimitInformation:   /* JOBOBJECT_EXTENDED_LIMIT_INFORMATION */
        if (len >= sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION32))
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION32 info32;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
            ULONG ret_size = sizeof(info32);

            status = NtQueryInformationJobObject( handle, class, &info, sizeof(info), NULL );
            if (!status)
            {
                put_job_basic_limit_info( &info32.BasicLimitInformation, &info.BasicLimitInformation );
                info32.IoInfo                = info.IoInfo;
                info32.ProcessMemoryLimit    = info.ProcessMemoryLimit;
                info32.JobMemoryLimit        = info.JobMemoryLimit;
                info32.PeakProcessMemoryUsed = info.PeakProcessMemoryUsed;
                info32.PeakJobMemoryUsed     = info.PeakJobMemoryUsed;
                wow64_write_user( ptr, &info32, sizeof(info32) );
            }
            if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
            return status;
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    default:
        if (class >= MaxJobObjectInfoClass) return STATUS_INVALID_PARAMETER;
        FIXME( "unsupported class %u\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}


/**********************************************************************
 *           wow64_NtQueryIoCompletion
 */
NTSTATUS WINAPI wow64_NtQueryIoCompletion( UINT *args )
{
    HANDLE handle = get_handle( &args );
    IO_COMPLETION_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    void *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (buffer) memset( buffer, 0, len );
    status = NtQueryIoCompletion( handle, class, buffer, len, &ret_size );
    if (!status && ret_size) wow64_write_user( info, buffer, min( len, ret_size ) );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtQueryMutant
 */
NTSTATUS WINAPI wow64_NtQueryMutant( UINT *args )
{
    HANDLE handle = get_handle( &args );
    MUTANT_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    void *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (buffer) memset( buffer, 0, len );
    status = NtQueryMutant( handle, class, buffer, len, &ret_size );
    if (!status && ret_size) wow64_write_user( info, buffer, min( len, ret_size ) );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtQueryObject
 */
NTSTATUS WINAPI wow64_NtQueryObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    OBJECT_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;
    ULONG ret_size;

    switch (class)
    {
    case ObjectBasicInformation:   /* OBJECT_BASIC_INFORMATION */
    case ObjectHandleFlagInformation:   /* OBJECT_HANDLE_FLAG_INFORMATION */
    {
        void *buffer = len ? alloc_temp( len ) : NULL;

        ret_size = 0;
        if (buffer) memset( buffer, 0, len );
        status = NtQueryObject( handle, class, buffer, len, &ret_size );
        if (!status && ret_size) wow64_write_user( ptr, buffer, min( len, ret_size ) );
        if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        return status;
    }

    case ObjectNameInformation:   /* OBJECT_NAME_INFORMATION */
    {
        SIZE_T size = (SIZE_T)len + sizeof(OBJECT_NAME_INFORMATION) - sizeof(OBJECT_NAME_INFORMATION32);
        OBJECT_NAME_INFORMATION *info;

        if (size > MAXDWORD) return STATUS_INTEGER_OVERFLOW;
        info = alloc_temp( size );
        if (!(status = NtQueryObject( handle, class, info, size, &ret_size )))
        {
            ULONG out_size;
            OBJECT_NAME_INFORMATION32 *out;

            if (info->Name.Length > info->Name.MaximumLength)
                return STATUS_INVALID_BUFFER_SIZE;
            out_size = sizeof(*out) + info->Name.MaximumLength;
            if (len >= out_size)
            {
                out = alloc_temp( out_size );
                memset( out, 0, out_size );
                if (info->Name.Length)
                {
                    if (info->Name.Length > MAXWORD - sizeof(WCHAR) ||
                        info->Name.MaximumLength < info->Name.Length + sizeof(WCHAR) ||
                        (char *)info->Name.Buffer < (char *)info ||
                        (char *)info->Name.Buffer > (char *)info + size - info->Name.MaximumLength)
                        return STATUS_INVALID_BUFFER_SIZE;
                    memcpy( out + 1, info->Name.Buffer, info->Name.Length + sizeof(WCHAR) );
                    out->Name.Length = info->Name.Length;
                    out->Name.MaximumLength = info->Name.Length + sizeof(WCHAR);
                    out->Name.Buffer = guest_addr_add( ptr, sizeof(*out) );
                }
                wow64_write_user( ptr, out, out_size );
            }
            else status = STATUS_INFO_LENGTH_MISMATCH;
            ret_size = out_size;
            if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        }
        else if (status == STATUS_INFO_LENGTH_MISMATCH || status == STATUS_BUFFER_OVERFLOW)
        {
            if (ret_size < sizeof(*info) - sizeof(OBJECT_NAME_INFORMATION32))
                return STATUS_INVALID_BUFFER_SIZE;
            ret_size = ret_size - sizeof(*info) + sizeof(OBJECT_NAME_INFORMATION32);
            if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        }
        return status;
    }

    case ObjectTypeInformation:   /* OBJECT_TYPE_INFORMATION */
    {
        ULONG_PTR buffer[(sizeof(OBJECT_TYPE_INFORMATION) + 64) / sizeof(ULONG_PTR)];
        OBJECT_TYPE_INFORMATION *info = (OBJECT_TYPE_INFORMATION *)buffer;

        if (!(status = NtQueryObject( handle, class, info, sizeof(buffer), NULL )))
        {
            ULONG out_size;
            OBJECT_TYPE_INFORMATION32 *out;

            if (info->TypeName.Length > info->TypeName.MaximumLength ||
                (info->TypeName.Length &&
                 (info->TypeName.MaximumLength < info->TypeName.Length + sizeof(WCHAR) ||
                  (char *)info->TypeName.Buffer < (char *)buffer ||
                  (char *)info->TypeName.Buffer > (char *)buffer + sizeof(buffer) -
                                                        info->TypeName.MaximumLength)))
                return STATUS_INVALID_BUFFER_SIZE;
            out_size = sizeof(*out) + info->TypeName.MaximumLength;
            if (len >= out_size)
            {
                out = alloc_temp( out_size );
                memset( out, 0, out_size );
                put_object_type_info( out, info, wow64_guest_memory_addr( ptr ) );
                wow64_write_user( ptr, out, out_size );
            }
            else
                status = STATUS_INFO_LENGTH_MISMATCH;
            ret_size = sizeof(*out) + info->TypeName.Length + sizeof(WCHAR);
            if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
        }
        return status;
    }

    case ObjectTypesInformation:   /* OBJECT_TYPES_INFORMATION */
    {
        OBJECT_TYPES_INFORMATION *info, *out;
        /* assume at most 32 types, with an average 16-char name */
        ULONG ret_size, size = 32 * (sizeof(OBJECT_TYPE_INFORMATION) + 16 * sizeof(WCHAR));

        info = alloc_temp( size );
        if (!(status = NtQueryObject( handle, class, info, size, &ret_size )))
        {
            OBJECT_TYPE_INFORMATION *type;
            OBJECT_TYPE_INFORMATION32 *type32;
            ULONG align = TYPE_ALIGNMENT( OBJECT_TYPE_INFORMATION ) - 1;
            ULONG align32 = TYPE_ALIGNMENT( OBJECT_TYPE_INFORMATION32 ) - 1;
            ULONG i, pos = (sizeof(*info) + align) & ~align;
            ULONG pos32 = (sizeof(*out) + align32) & ~align32;

            out = len ? alloc_temp( len ) : NULL;
            if (out) memset( out, 0, len );
            if (pos32 <= len) out->NumberOfTypes = info->NumberOfTypes;
            for (i = 0; i < info->NumberOfTypes; i++)
            {
                SIZE_T native_next, guest_next;

                if (pos > size - sizeof(*type)) return STATUS_INVALID_BUFFER_SIZE;
                type = (OBJECT_TYPE_INFORMATION *)((char *)info + pos);
                if (type->TypeName.Length > type->TypeName.MaximumLength ||
                    type->TypeName.MaximumLength > size - pos - sizeof(*type) ||
                    (type->TypeName.Length &&
                     (type->TypeName.MaximumLength < type->TypeName.Length + sizeof(WCHAR) ||
                      (char *)type->TypeName.Buffer < (char *)info ||
                      (char *)type->TypeName.Buffer > (char *)info + size -
                                                            type->TypeName.MaximumLength)))
                    return STATUS_INVALID_BUFFER_SIZE;
                native_next = (SIZE_T)pos + sizeof(*type) +
                              ((type->TypeName.MaximumLength + align) & ~align);
                guest_next = (SIZE_T)pos32 + sizeof(*type32) +
                             ((type->TypeName.MaximumLength + align32) & ~align32);
                if (native_next > size || guest_next > MAXDWORD) return STATUS_INVALID_BUFFER_SIZE;
                if (guest_next <= len)
                {
                    type32 = (OBJECT_TYPE_INFORMATION32 *)((char *)out + pos32);
                    put_object_type_info( type32, type,
                                          guest_addr_add( ptr, pos32 ) );
                }
                pos = native_next;
                pos32 = guest_next;
            }
            if (pos32 > len) status = STATUS_INFO_LENGTH_MISMATCH;
            else wow64_write_user( ptr, out, pos32 );
            if (retlen) wow64_write_user( retlen, &pos32, sizeof(pos32) );
        }
        return status;
    }

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}


/**********************************************************************
 *           wow64_NtQueryPerformanceCounter
 */
NTSTATUS WINAPI wow64_NtQueryPerformanceCounter( UINT *args )
{
    LARGE_INTEGER *counter = get_ptr( &args );
    LARGE_INTEGER *frequency = get_ptr( &args );

    LARGE_INTEGER counter_buf, frequency_buf;
    NTSTATUS status;

    if (counter) wow64_probe_user_write( counter, sizeof(counter_buf) );
    if (frequency) wow64_probe_user_write( frequency, sizeof(frequency_buf) );
    status = NtQueryPerformanceCounter( counter ? &counter_buf : NULL,
                                        frequency ? &frequency_buf : NULL );
    if (NT_SUCCESS(status))
    {
        if (counter) wow64_write_user( counter, &counter_buf, sizeof(counter_buf) );
        if (frequency) wow64_write_user( frequency, &frequency_buf, sizeof(frequency_buf) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtQuerySection
 */
NTSTATUS WINAPI wow64_NtQuerySection( UINT *args )
{
    HANDLE handle = get_handle( &args );
    SECTION_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    SIZE_T size = get_ulong( &args );
    ULONG *ret_ptr = get_ptr( &args );

    NTSTATUS status;
    SIZE_T ret_size = 0;

    switch (class)
    {
    case SectionBasicInformation:
    {
        SECTION_BASIC_INFORMATION info;
        SECTION_BASIC_INFORMATION32 info32;

        if (size < sizeof(info32)) return STATUS_INFO_LENGTH_MISMATCH;
        if (!(status = NtQuerySection( handle, class, &info, sizeof(info), &ret_size )))
        {
            info32.BaseAddress = wow64_guest_memory_addr( info.BaseAddress );
            info32.Attributes  = info.Attributes;
            info32.Size        = info.Size;
            wow64_write_user( ptr, &info32, sizeof(info32) );
            ret_size = sizeof(info32);
        }
        break;
    }
    case SectionImageInformation:
    {
        SECTION_IMAGE_INFORMATION info;
        SECTION_IMAGE_INFORMATION32 info32;

        if (size < sizeof(info32)) return STATUS_INFO_LENGTH_MISMATCH;
        if (!(status = NtQuerySection( handle, class, &info, sizeof(info), &ret_size )))
        {
            put_section_image_info( &info32, &info );
            wow64_write_user( ptr, &info32, sizeof(info32) );
            ret_size = sizeof(info32);
        }
        break;
    }
    default:
	FIXME( "class %u not implemented\n", class );
	return STATUS_NOT_IMPLEMENTED;
    }
    put_size( ret_ptr, ret_size );
    return status;
}


/**********************************************************************
 *           wow64_NtQuerySemaphore
 */
NTSTATUS WINAPI wow64_NtQuerySemaphore( UINT *args )
{
    HANDLE handle = get_handle( &args );
    SEMAPHORE_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    void *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (buffer) memset( buffer, 0, len );
    status = NtQuerySemaphore( handle, class, buffer, len, &ret_size );
    if (!status && ret_size) wow64_write_user( info, buffer, min( len, ret_size ) );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtQuerySymbolicLinkObject
 */
NTSTATUS WINAPI wow64_NtQuerySymbolicLinkObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    UNICODE_STRING32 *target32 = get_ptr( &args );
    ULONG *retlen = get_ptr( &args );

    UNICODE_STRING32 target_value;
    UNICODE_STRING target;
    ULONG ret_size = 0;
    NTSTATUS status;

    wow64_read_user( &target_value, target32, sizeof(target_value) );
    target.Length = target_value.Length;
    target.MaximumLength = target_value.MaximumLength;
    target.Buffer = target.MaximumLength ? alloc_temp( target.MaximumLength ) : NULL;
    status = NtQuerySymbolicLinkObject( handle, &target, &ret_size );
    if (!status)
    {
        if (target.MaximumLength < sizeof(WCHAR) ||
            target.Length > target.MaximumLength - sizeof(WCHAR))
            return STATUS_INVALID_BUFFER_SIZE;
        wow64_write_user( wow64_guest_memory_ptr( target_value.Buffer ), target.Buffer,
                          target.Length + sizeof(WCHAR) );
        target_value.Length = target.Length;
        wow64_write_user( target32, &target_value, sizeof(target_value) );
    }
    if (retlen && (NT_SUCCESS(status) || status == STATUS_BUFFER_TOO_SMALL))
        wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtQueryTimer
 */
NTSTATUS WINAPI wow64_NtQueryTimer( UINT *args )
{
    HANDLE handle = get_handle( &args );
    TIMER_INFORMATION_CLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    void *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (buffer) memset( buffer, 0, len );
    status = NtQueryTimer( handle, class, buffer, len, &ret_size );
    if (!status && ret_size) wow64_write_user( info, buffer, min( len, ret_size ) );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtQueryTimerResolution
 */
NTSTATUS WINAPI wow64_NtQueryTimerResolution( UINT *args )
{
    ULONG *min_res = get_ptr( &args );
    ULONG *max_res = get_ptr( &args );
    ULONG *current_res = get_ptr( &args );

    ULONG min_buf, max_buf, current_buf;
    NTSTATUS status;

    wow64_probe_user_write( min_res, sizeof(min_buf) );
    wow64_probe_user_write( max_res, sizeof(max_buf) );
    wow64_probe_user_write( current_res, sizeof(current_buf) );
    status = NtQueryTimerResolution( &min_buf, &max_buf, &current_buf );
    if (NT_SUCCESS(status))
    {
        wow64_write_user( min_res, &min_buf, sizeof(min_buf) );
        wow64_write_user( max_res, &max_buf, sizeof(max_buf) );
        wow64_write_user( current_res, &current_buf, sizeof(current_buf) );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtRegisterThreadTerminatePort
 */
NTSTATUS WINAPI wow64_NtRegisterThreadTerminatePort( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtRegisterThreadTerminatePort( handle );
}


/**********************************************************************
 *           wow64_NtReleaseKeyedEvent
 */
NTSTATUS WINAPI wow64_NtReleaseKeyedEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    void *key = get_raw_ptr( &args );
    BOOLEAN alertable = get_ulong( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    LARGE_INTEGER timeout_buf;

    return NtReleaseKeyedEvent( handle, key, alertable,
                                large_integer_from_user( &timeout_buf, timeout ) );
}


/**********************************************************************
 *           wow64_NtReleaseMutant
 */
NTSTATUS WINAPI wow64_NtReleaseMutant( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LONG *prev_count = get_ptr( &args );

    LONG value = 0;
    NTSTATUS status;

    if (prev_count) wow64_probe_user_write( prev_count, sizeof(value) );
    status = NtReleaseMutant( handle, prev_count ? &value : NULL );
    return copy_scalar_output( prev_count, &value, sizeof(value), status );
}


/**********************************************************************
 *           wow64_NtReleaseSemaphore
 */
NTSTATUS WINAPI wow64_NtReleaseSemaphore( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG count = get_ulong( &args );
    ULONG *previous = get_ptr( &args );

    ULONG value = 0;
    NTSTATUS status;

    if (previous) wow64_probe_user_write( previous, sizeof(value) );
    status = NtReleaseSemaphore( handle, count, previous ? &value : NULL );
    return copy_scalar_output( previous, &value, sizeof(value), status );
}


/**********************************************************************
 *           wow64_NtReadRequestData
 */
NTSTATUS WINAPI wow64_NtReadRequestData( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LPC_MESSAGE *request = get_ptr( &args );
    ULONG id = get_ulong( &args );
    void *buffer = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    FIXME( "%p %p %lu %p %lu %p: stub\n", handle, request, id, buffer, len, retlen );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtReplyPort
 */
NTSTATUS WINAPI wow64_NtReplyPort( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LPC_MESSAGE *reply = get_ptr( &args );

    FIXME( "%p %p: stub\n", handle, reply );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtReplyWaitReceivePort
 */
NTSTATUS WINAPI wow64_NtReplyWaitReceivePort( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG *id = get_ptr( &args );
    LPC_MESSAGE *reply = get_ptr( &args );
    LPC_MESSAGE *msg = get_ptr( &args );

    FIXME( "%p %p %p %p: stub\n", handle, id, reply, msg );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtReplyWaitReceivePortEx
 */
NTSTATUS WINAPI wow64_NtReplyWaitReceivePortEx( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG *id = get_ptr( &args );
    LPC_MESSAGE *reply = get_ptr( &args );
    LPC_MESSAGE *msg = get_ptr( &args );
    LARGE_INTEGER *timeout = get_ptr( &args );

    FIXME( "%p %p %p %p %p: stub\n", handle, id, reply, msg, timeout );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtRequestWaitReplyPort
 */
NTSTATUS WINAPI wow64_NtRequestWaitReplyPort( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LPC_MESSAGE *msg_in = get_ptr( &args );
    LPC_MESSAGE *msg_out = get_ptr( &args );

    FIXME( "%p %p %p: stub\n", handle, msg_in, msg_out );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtResetEvent
 */
NTSTATUS WINAPI wow64_NtResetEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LONG *prev_state = get_ptr( &args );

    LONG value = 0;
    NTSTATUS status;

    if (prev_state) wow64_probe_user_write( prev_state, sizeof(value) );
    status = NtResetEvent( handle, prev_state ? &value : NULL );
    return copy_scalar_output( prev_state, &value, sizeof(value), status );
}


/**********************************************************************
 *           wow64_NtSecureConnectPort
 */
NTSTATUS WINAPI wow64_NtSecureConnectPort( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    UNICODE_STRING32 *name32 = get_ptr( &args );
    SECURITY_QUALITY_OF_SERVICE *qos = get_ptr( &args );
    LPC_SECTION_WRITE *write = get_ptr( &args );
    SID *sid = get_ptr( &args );
    LPC_SECTION_READ *read = get_ptr( &args );
    ULONG *max_len = get_ptr( &args );
    void *info = get_ptr( &args );
    ULONG *info_len = get_ptr( &args );

    FIXME( "%p %p %p %p %p %p %p %p %p: stub\n",
           handle_ptr, name32, qos, write, sid, read, max_len, info, info_len );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtSetEvent
 */
NTSTATUS WINAPI wow64_NtSetEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LONG *prev_state = get_ptr( &args );

    LONG value = 0;
    NTSTATUS status;

    if (prev_state) wow64_probe_user_write( prev_state, sizeof(value) );
    status = NtSetEvent( handle, prev_state ? &value : NULL );
    return copy_scalar_output( prev_state, &value, sizeof(value), status );
}


/**********************************************************************
 *           wow64_NtSetEventBoostPriority
 */
NTSTATUS WINAPI wow64_NtSetEventBoostPriority( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtSetEventBoostPriority( handle );
}


/**********************************************************************
 *           wow64_NtSetInformationDebugObject
 */
NTSTATUS WINAPI wow64_NtSetInformationDebugObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    DEBUGOBJECTINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    void *buffer = len ? alloc_temp( len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (len) wow64_read_user( buffer, ptr, len );
    if (retlen) wow64_probe_user_write( retlen, sizeof(ret_size) );
    status = NtSetInformationDebugObject( handle, class, buffer, len,
                                          retlen ? &ret_size : NULL );
    if (retlen) wow64_write_user( retlen, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtSetInformationJobObject
 */
NTSTATUS WINAPI wow64_NtSetInformationJobObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    JOBOBJECTINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );

    switch (class)
    {
    case JobObjectBasicLimitInformation:   /* JOBOBJECT_BASIC_LIMIT_INFORMATION */
        if (len == sizeof(JOBOBJECT_BASIC_LIMIT_INFORMATION32))
        {
            JOBOBJECT_BASIC_LIMIT_INFORMATION32 info32;
            JOBOBJECT_BASIC_LIMIT_INFORMATION info;

            wow64_read_user( &info32, ptr, sizeof(info32) );
            return NtSetInformationJobObject( handle, class,
                                              job_basic_limit_info_32to64( &info, &info32 ),
                                              sizeof(info) );
        }
        else return STATUS_INVALID_PARAMETER;

    case JobObjectBasicUIRestrictions:
        FIXME( "unsupported class JobObjectBasicUIRestrictions\n" );
        return STATUS_SUCCESS;

    case JobObjectAssociateCompletionPortInformation:   /* JOBOBJECT_ASSOCIATE_COMPLETION_PORT */
        if (len == sizeof(JOBOBJECT_ASSOCIATE_COMPLETION_PORT32))
        {
            JOBOBJECT_ASSOCIATE_COMPLETION_PORT32 info32;
            JOBOBJECT_ASSOCIATE_COMPLETION_PORT info;

            wow64_read_user( &info32, ptr, sizeof(info32) );
            info.CompletionKey  = wow64_raw_ptr32( info32.CompletionKey );
            info.CompletionPort = LongToHandle( info32.CompletionPort );
            return NtSetInformationJobObject( handle, class, &info, sizeof(info) );
        }
        else return STATUS_INVALID_PARAMETER;

    case JobObjectExtendedLimitInformation:   /* JOBOBJECT_EXTENDED_LIMIT_INFORMATION */
        if (len == sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION32))
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION32 info32;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

            wow64_read_user( &info32, ptr, sizeof(info32) );
            info.IoInfo                = info32.IoInfo;
            info.ProcessMemoryLimit    = info32.ProcessMemoryLimit;
            info.JobMemoryLimit        = info32.JobMemoryLimit;
            info.PeakProcessMemoryUsed = info32.PeakProcessMemoryUsed;
            info.PeakJobMemoryUsed     = info32.PeakJobMemoryUsed;
            return NtSetInformationJobObject( handle, class,
                                              job_basic_limit_info_32to64( &info.BasicLimitInformation,
                                                                           &info32.BasicLimitInformation ),
                                              sizeof(info) );
        }
        else return STATUS_INVALID_PARAMETER;

    default:
        if (class >= MaxJobObjectInfoClass) return STATUS_INVALID_PARAMETER;
        FIXME( "unsupported class %u\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}


/**********************************************************************
 *           wow64_NtSetInformationObject
 */
NTSTATUS WINAPI wow64_NtSetInformationObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    OBJECT_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );

    switch (class)
    {
    case ObjectHandleFlagInformation:   /* OBJECT_HANDLE_FLAG_INFORMATION */
    {
        OBJECT_HANDLE_FLAG_INFORMATION info;

        if (len < sizeof(info)) return STATUS_INFO_LENGTH_MISMATCH;
        wow64_read_user( &info, ptr, sizeof(info) );
        return NtSetInformationObject( handle, class, &info, sizeof(info) );
    }

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}


/**********************************************************************
 *           wow64_NtSetIoCompletion
 */
NTSTATUS WINAPI wow64_NtSetIoCompletion( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG_PTR key = get_ulong( &args );
    ULONG_PTR value = get_ulong( &args );
    NTSTATUS status = get_ulong( &args );
    SIZE_T count = get_ulong( &args );

    return NtSetIoCompletion( handle, key, value, status, count );
}


/**********************************************************************
 *           wow64_NtSetIoCompletionEx
 */
NTSTATUS WINAPI wow64_NtSetIoCompletionEx( UINT *args )
{
    HANDLE completion_handle = get_handle( &args );
    HANDLE completion_reserve_handle = get_handle( &args );
    ULONG_PTR key = get_ulong( &args );
    ULONG_PTR value = get_ulong( &args );
    NTSTATUS status = get_ulong( &args );
    SIZE_T count = get_ulong( &args );

    return NtSetIoCompletionEx( completion_handle, completion_reserve_handle, key, value, status, count );
}


/**********************************************************************
 *           wow64_NtSetTimer
 */
NTSTATUS WINAPI wow64_NtSetTimer( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LARGE_INTEGER *when = get_ptr( &args );
    ULONG apc = get_ulong( &args );
    ULONG apc_param = get_ulong( &args );
    BOOLEAN resume = get_ulong( &args );
    ULONG period = get_ulong( &args );
    BOOLEAN *state = get_ptr( &args );

    LARGE_INTEGER when_buf;
    LARGE_INTEGER *when64;
    BOOLEAN state_buf = FALSE;
    NTSTATUS status;

    when64 = large_integer_from_user( &when_buf, when );
    if (state) wow64_probe_user_write( state, sizeof(state_buf) );
    status = NtSetTimer( handle, when64, apc_32to64( apc ),
                         apc_param_32to64( apc, apc_param ), resume, period,
                         state ? &state_buf : NULL );
    return copy_scalar_output( state, &state_buf, sizeof(state_buf), status );
}


/**********************************************************************
 *           wow64_NtSetTimerResolution
 */
NTSTATUS WINAPI wow64_NtSetTimerResolution( UINT *args )
{
    ULONG res = get_ulong( &args );
    BOOLEAN set = get_ulong( &args );
    ULONG *current_res = get_ptr( &args );

    ULONG current_buf = 0;
    NTSTATUS status;

    wow64_probe_user_write( current_res, sizeof(current_buf) );
    status = NtSetTimerResolution( res, set, &current_buf );
    return copy_scalar_output( current_res, &current_buf, sizeof(current_buf), status );
}


/**********************************************************************
 *           wow64_NtSignalAndWaitForSingleObject
 */
NTSTATUS WINAPI wow64_NtSignalAndWaitForSingleObject( UINT *args )
{
    HANDLE signal = get_handle( &args );
    HANDLE wait = get_handle( &args );
    BOOLEAN alertable = get_ulong( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    LARGE_INTEGER timeout_buf;

    return NtSignalAndWaitForSingleObject( signal, wait, alertable,
                                           large_integer_from_user( &timeout_buf, timeout ) );
}


/**********************************************************************
 *           wow64_NtTerminateJobObject
 */
NTSTATUS WINAPI wow64_NtTerminateJobObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    NTSTATUS status = get_ulong( &args );

    return NtTerminateJobObject( handle, status );
}


/**********************************************************************
 *           wow64_NtTestAlert
 */
NTSTATUS WINAPI wow64_NtTestAlert( UINT *args )
{
    return NtTestAlert();
}


/**********************************************************************
 *           wow64_NtTraceControl
 */
NTSTATUS WINAPI wow64_NtTraceControl( UINT *args )
{
    ULONG code = get_ulong( &args );
    void *inbuf = get_ptr( &args );
    ULONG inbuf_len = get_ulong( &args );
    void *outbuf = get_ptr( &args );
    ULONG outbuf_len = get_ulong( &args );
    ULONG *size = get_ptr( &args );

    void *input = inbuf_len ? alloc_temp( inbuf_len ) : NULL;
    void *output = outbuf_len ? alloc_temp( outbuf_len ) : NULL;
    ULONG ret_size = 0;
    NTSTATUS status;

    if (inbuf_len) wow64_read_user( input, inbuf, inbuf_len );
    if (output) memset( output, 0, outbuf_len );
    if (outbuf_len) wow64_probe_user_write( outbuf, outbuf_len );
    if (size) wow64_probe_user_write( size, sizeof(ret_size) );
    status = NtTraceControl( code, input, inbuf_len, output, outbuf_len,
                             size ? &ret_size : NULL );
    if (NT_SUCCESS(status) && outbuf_len)
        wow64_write_user( outbuf, output, min( outbuf_len, ret_size ) );
    if (size) wow64_write_user( size, &ret_size, sizeof(ret_size) );
    return status;
}


/**********************************************************************
 *           wow64_NtWaitForAlertByThreadId
 */
NTSTATUS WINAPI wow64_NtWaitForAlertByThreadId( UINT *args )
{
    const void *address = get_raw_ptr( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    LARGE_INTEGER timeout_buf;

    return NtWaitForAlertByThreadId( address, large_integer_from_user( &timeout_buf, timeout ) );
}


/* helper to wow64_NtWaitForDebugEvent; retrieve machine from PE image */
static NTSTATUS get_image_machine( HANDLE handle, USHORT *machine )
{
    IMAGE_DOS_HEADER dos_hdr;
    IMAGE_NT_HEADERS nt_hdr;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER offset;
    FILE_POSITION_INFORMATION pos_info;
    NTSTATUS status;

    offset.QuadPart = 0;
    status = NtReadFile( handle, NULL, NULL, NULL,
                         &iosb, &dos_hdr, sizeof(dos_hdr), &offset, NULL );
    if (!status)
    {
        offset.QuadPart = dos_hdr.e_lfanew;
        status = NtReadFile( handle, NULL, NULL, NULL, &iosb,
                             &nt_hdr, FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader), &offset, NULL );
        if (!status)
            *machine = nt_hdr.FileHeader.Machine;
        /* Reset file pos at beginning of file */
        pos_info.CurrentByteOffset.QuadPart = 0;
        NtSetInformationFile( handle, &iosb, &pos_info, sizeof(pos_info), FilePositionInformation );
    }
    return status;
}

/* Drop ownership acquired by NtWaitForDebugEvent when the translated event
 * cannot be delivered.  The server event has already moved to EVENT_SENT, so
 * it must also be continued or the debuggee remains reply-pending forever. */
static void discard_debug_state_change( HANDLE handle, DBGUI_WAIT_STATE_CHANGE *state )
{
    NTSTATUS continue_status = DBG_CONTINUE;
    NTSTATUS status;

    switch (state->NewState)
    {
    case DbgCreateThreadStateChange:
        if (state->StateInfo.CreateThread.HandleToThread)
            NtClose( state->StateInfo.CreateThread.HandleToThread );
        break;
    case DbgCreateProcessStateChange:
        if (state->StateInfo.CreateProcessInfo.HandleToProcess)
            NtClose( state->StateInfo.CreateProcessInfo.HandleToProcess );
        if (state->StateInfo.CreateProcessInfo.HandleToThread)
            NtClose( state->StateInfo.CreateProcessInfo.HandleToThread );
        if (state->StateInfo.CreateProcessInfo.NewProcess.FileHandle)
            NtClose( state->StateInfo.CreateProcessInfo.NewProcess.FileHandle );
        break;
    case DbgExceptionStateChange:
    case DbgBreakpointStateChange:
    case DbgSingleStepStateChange:
        continue_status = DBG_EXCEPTION_NOT_HANDLED;
        break;
    case DbgLoadDllStateChange:
        if (state->StateInfo.LoadDll.FileHandle)
            NtClose( state->StateInfo.LoadDll.FileHandle );
        break;
    default:
        break;
    }

    status = NtDebugContinue( handle, &state->AppClientId, continue_status );
    if (status) WARN( "failed to continue discarded debug event %#lx\n", status );
}

/* helper to wow64_NtWaitForDebugEvent; only pass debug events for current machine */
static BOOL filter_out_state_change( HANDLE handle, DBGUI_WAIT_STATE_CHANGE *state )
{
    BOOL filter_out;

    switch (state->NewState)
    {
    case DbgLoadDllStateChange:
        filter_out = ((ULONG64)state->StateInfo.LoadDll.BaseOfDll >> 32) != 0 &&
                     ((ULONG_PTR)state->StateInfo.LoadDll.BaseOfDll < WINE_LOW_VA_SHADOW_BASE ||
                      (ULONG_PTR)state->StateInfo.LoadDll.BaseOfDll >=
                          WINE_LOW_VA_SHADOW_BASE + WINE_LOW_VA_SHADOW_SIZE);
        if (!filter_out)
        {
            USHORT machine;
            filter_out = !get_image_machine( state->StateInfo.LoadDll.FileHandle, &machine) && machine != current_machine;
        }
        break;
    case DbgUnloadDllStateChange:
        filter_out = ((ULONG_PTR)state->StateInfo.UnloadDll.BaseAddress >> 32) != 0 &&
                     ((ULONG_PTR)state->StateInfo.UnloadDll.BaseAddress < WINE_LOW_VA_SHADOW_BASE ||
                      (ULONG_PTR)state->StateInfo.UnloadDll.BaseAddress >=
                          WINE_LOW_VA_SHADOW_BASE + WINE_LOW_VA_SHADOW_SIZE);
        break;
    default:
        filter_out = FALSE;
        break;
    }
    if (filter_out)
        discard_debug_state_change( handle, state );
    return filter_out;
}


/**********************************************************************
 *           wow64_NtWaitForDebugEvent
 */
NTSTATUS WINAPI wow64_NtWaitForDebugEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN alertable = get_ulong( &args );
    LARGE_INTEGER *timeout = get_ptr( &args );
    DBGUI_WAIT_STATE_CHANGE32 *state32 = get_ptr( &args );

    ULONG i;
    LARGE_INTEGER timeout_buf;
    DBGUI_WAIT_STATE_CHANGE state;
    DBGUI_WAIT_STATE_CHANGE32 out;
    NTSTATUS status;

    timeout = (LARGE_INTEGER *)large_integer_from_user( &timeout_buf, timeout );
    wow64_probe_user_write( state32, sizeof(out) );
    do
    {
        status = NtWaitForDebugEvent( handle, alertable, timeout, &state );
    } while (!status && filter_out_state_change( handle, &state ));

    if (!status)
    {
        memset( &out, 0, sizeof(out) );
        out.NewState = state.NewState;
        out.AppClientId.UniqueProcess = HandleToULong( state.AppClientId.UniqueProcess );
        out.AppClientId.UniqueThread = HandleToULong( state.AppClientId.UniqueThread );
        switch (state.NewState)
        {
#define COPY_ULONG(field)  out.StateInfo.field = state.StateInfo.field
#define COPY_HANDLE(field) out.StateInfo.field = PtrToUlong( state.StateInfo.field )
#define COPY_ADDR(field)   out.StateInfo.field = wow64_guest_memory_addr( state.StateInfo.field )
        case DbgCreateThreadStateChange:
            COPY_HANDLE( CreateThread.HandleToThread );
            COPY_ADDR( CreateThread.NewThread.StartAddress );
            COPY_ULONG( CreateThread.NewThread.SubSystemKey );
            break;
        case DbgCreateProcessStateChange:
            COPY_HANDLE( CreateProcessInfo.HandleToProcess );
            COPY_HANDLE( CreateProcessInfo.HandleToThread );
            COPY_HANDLE( CreateProcessInfo.NewProcess.FileHandle );
            COPY_ADDR( CreateProcessInfo.NewProcess.BaseOfImage );
            COPY_ADDR( CreateProcessInfo.NewProcess.InitialThread.StartAddress );
            COPY_ULONG( CreateProcessInfo.NewProcess.InitialThread.SubSystemKey );
            COPY_ULONG( CreateProcessInfo.NewProcess.DebugInfoFileOffset );
            COPY_ULONG( CreateProcessInfo.NewProcess.DebugInfoSize );
            break;
        case DbgExitThreadStateChange:
        case DbgExitProcessStateChange:
            COPY_ULONG( ExitThread.ExitStatus );
            break;
        case DbgExceptionStateChange:
        case DbgBreakpointStateChange:
        case DbgSingleStepStateChange:
            COPY_ULONG( Exception.FirstChance );
            COPY_ULONG( Exception.ExceptionRecord.ExceptionCode );
            COPY_ULONG( Exception.ExceptionRecord.ExceptionFlags );
            COPY_ULONG( Exception.ExceptionRecord.NumberParameters );
            COPY_ADDR( Exception.ExceptionRecord.ExceptionRecord );
            COPY_ADDR( Exception.ExceptionRecord.ExceptionAddress );
            if (state.StateInfo.Exception.ExceptionRecord.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS)
            {
                status = STATUS_INVALID_PARAMETER;
                goto failed;
            }
            for (i = 0; i < state.StateInfo.Exception.ExceptionRecord.NumberParameters; i++)
                COPY_ULONG( Exception.ExceptionRecord.ExceptionInformation[i] );
            break;
        case DbgLoadDllStateChange:
            COPY_HANDLE( LoadDll.FileHandle );
            COPY_ADDR( LoadDll.BaseOfDll );
            COPY_ULONG( LoadDll.DebugInfoFileOffset );
            COPY_ULONG( LoadDll.DebugInfoSize );
            COPY_ADDR( LoadDll.NamePointer );
            break;
        case DbgUnloadDllStateChange:
            COPY_ADDR( UnloadDll.BaseAddress );
            break;
        default:
            break;
        }
#undef COPY_ULONG
#undef COPY_HANDLE
#undef COPY_ADDR
        if ((status = wow64_try_write_user( state32, &out, sizeof(out) ))) goto failed;
    }
    return status;

failed:
    discard_debug_state_change( handle, &state );
    return status;
}


/**********************************************************************
 *           wow64_NtWaitForKeyedEvent
 */
NTSTATUS WINAPI wow64_NtWaitForKeyedEvent( UINT *args )
{
    HANDLE handle = get_handle( &args );
    const void *key = get_raw_ptr( &args );
    BOOLEAN alertable = get_ulong( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    LARGE_INTEGER timeout_buf;

    return NtWaitForKeyedEvent( handle, key, alertable,
                                large_integer_from_user( &timeout_buf, timeout ) );
}


/**********************************************************************
 *           wow64_NtWaitForMultipleObjects
 */
NTSTATUS WINAPI wow64_NtWaitForMultipleObjects( UINT *args )
{
    DWORD count = get_ulong( &args );
    LONG *handles_ptr = get_ptr( &args );
    WAIT_TYPE type = get_ulong( &args );
    BOOLEAN alertable = get_ulong( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    LONG handles32[MAXIMUM_WAIT_OBJECTS];
    LARGE_INTEGER timeout_buf;
    DWORD i;

    if (!count || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER_1;
    wow64_read_user( handles32, handles_ptr, count * sizeof(handles32[0]) );
    for (i = 0; i < count; i++) handles[i] = LongToHandle( handles32[i] );
    return NtWaitForMultipleObjects( count, handles, type, alertable,
                                     large_integer_from_user( &timeout_buf, timeout ) );
}


/**********************************************************************
 *           wow64_NtWaitForSingleObject
 */
NTSTATUS WINAPI wow64_NtWaitForSingleObject( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN alertable = get_ulong( &args );
    const LARGE_INTEGER *timeout = get_ptr( &args );

    LARGE_INTEGER timeout_buf;

    return NtWaitForSingleObject( handle, alertable,
                                  large_integer_from_user( &timeout_buf, timeout ) );
}


/**********************************************************************
 *           wow64_NtWriteRequestData
 */
NTSTATUS WINAPI wow64_NtWriteRequestData( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LPC_MESSAGE *request = get_ptr( &args );
    ULONG id = get_ulong( &args );
    void *buffer = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    FIXME( "%p %p %lu %p %lu %p: stub\n", handle, request, id, buffer, len, retlen );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtYieldExecution
 */
NTSTATUS WINAPI wow64_NtYieldExecution( UINT *args )
{
    return NtYieldExecution();
}


/**********************************************************************
 *           wow64_NtCreateTransaction
 */
NTSTATUS WINAPI wow64_NtCreateTransaction( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    GUID *guid = get_ptr( &args );
    HANDLE tm = get_handle( &args );
    ULONG options = get_ulong( &args );
    ULONG isol_level = get_ulong( &args );
    ULONG isol_flags = get_ulong( &args );
    LARGE_INTEGER *timeout = get_ptr( &args );
    UNICODE_STRING32 *desc32 = get_ptr( &args );

    struct object_attr64 attr;
    UNICODE_STRING desc;
    OBJECT_ATTRIBUTES *attr64;
    UNICODE_STRING *desc64;
    GUID guid_buf, *guid64 = NULL;
    LARGE_INTEGER timeout_buf, *timeout64;
    HANDLE handle = 0;
    NTSTATUS status;

    prepare_handle_output( handle_ptr );
    attr64 = objattr_32to64( &attr, attr32 );
    if (guid)
    {
        wow64_read_user( &guid_buf, guid, sizeof(guid_buf) );
        guid64 = &guid_buf;
    }
    timeout64 = large_integer_from_user( &timeout_buf, timeout );
    desc64 = unicode_string_from_user( &desc, desc32 );
    status = NtCreateTransaction( &handle, access, attr64, guid64, tm, options,
                                  isol_level, isol_flags, timeout64, desc64 );
    return publish_local_handle( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtCommitTransaction
 */
NTSTATUS WINAPI wow64_NtCommitTransaction( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN wait = get_ulong( &args );

    return NtCommitTransaction( handle, wait );
}


/**********************************************************************
 *           wow64_NtRollbackTransaction
 */
NTSTATUS WINAPI wow64_NtRollbackTransaction( UINT *args )
{
    HANDLE handle = get_handle( &args );
    BOOLEAN wait = get_ulong( &args );

    return NtRollbackTransaction( handle, wait );
}


/**********************************************************************
 *           wow64_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter
 */
NTSTATUS WINAPI wow64_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter( UINT *args )
{
    ULONG flags = get_ulong( &args );
    ULONGLONG *from = get_ptr( &args );
    ULONGLONG *to = get_ptr( &args );
    ULONGLONG *error = get_ptr( &args );

    ULONGLONG from_buf, to_buf = 0, error_buf = 0;
    NTSTATUS status;

    wow64_read_user( &from_buf, from, sizeof(from_buf) );
    wow64_probe_user_write( to, sizeof(to_buf) );
    wow64_probe_user_write( error, sizeof(error_buf) );
    status = NtConvertBetweenAuxiliaryCounterAndPerformanceCounter( flags, &from_buf,
                                                                    &to_buf, &error_buf );
    if (NT_SUCCESS(status))
    {
        wow64_write_user( to, &to_buf, sizeof(to_buf) );
        wow64_write_user( error, &error_buf, sizeof(error_buf) );
    }
    return status;
}
