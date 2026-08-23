/*
 * WoW64 process (and thread) functions
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
#include "ddk/ntddk.h"
#include "wow64_private.h"
#include "wine/asm.h"
#include "wine/exception.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);

#define WOW64_MAX_SERVER_HANDLE_ENTRIES 0x00ffffffu


static BOOL is_process_wow64( HANDLE handle )
{
    ULONG_PTR info;

    if (handle == GetCurrentProcess()) return TRUE;
    if (NtQueryInformationProcess( handle, ProcessWow64Information, &info, sizeof(info), NULL ))
        return FALSE;
    return !!info;
}


static BOOL is_process_id_wow64( const CLIENT_ID *id )
{
    HANDLE handle;
    BOOL ret = FALSE;

    if (id->UniqueProcess == ULongToHandle(GetCurrentProcessId())) return TRUE;
    if (!NtOpenProcess( &handle, PROCESS_QUERY_LIMITED_INFORMATION, NULL, id ))
    {
        ret = is_process_wow64( handle );
        NtClose( handle );
    }
    return ret;
}


static void *alloc_temp( SIZE_T size )
{
    void *ret;

    if (!size) return NULL;
    if (!(ret = Wow64AllocateTemp( size ))) RtlRaiseStatus( STATUS_NO_MEMORY );
    return ret;
}


static BOOL multiply_size( SIZE_T left, SIZE_T right, SIZE_T *result )
{
    if (right && left > ~(SIZE_T)0 / right) return FALSE;
    *result = left * right;
    return TRUE;
}


static NTSTATUS snapshot_environment( WCHAR **result, ULONG address, ULONG size )
{
    WCHAR chunk[2048];
    SIZE_T offset = 0, actual_size = 0, i, count;
    BOOL previous_zero = FALSE;
    WCHAR *environment;

    *result = NULL;
    if (!address) return STATUS_SUCCESS;
    if (!size || (size & (sizeof(WCHAR) - 1)) ||
        (ULONGLONG)address + size > 0x100000000ull)
        return STATUS_INVALID_PARAMETER;

    while (offset < size && !actual_size)
    {
        SIZE_T bytes = min( (SIZE_T)size - offset, sizeof(chunk) );

        wow64_read_user( chunk, wow64_guest_memory_ptr( address + (ULONG)offset ), bytes );
        count = bytes / sizeof(*chunk);
        for (i = 0; i < count; i++)
        {
            if (!chunk[i] && ((!offset && !i) || previous_zero))
            {
                actual_size = offset + (i + 1) * sizeof(*chunk);
                break;
            }
            previous_zero = !chunk[i];
        }
        offset += bytes;
    }
    if (!actual_size) return STATUS_INVALID_PARAMETER;

    environment = alloc_temp( actual_size );
    wow64_read_user( environment, wow64_guest_memory_ptr( address ), actual_size );
    *result = environment;
    return STATUS_SUCCESS;
}


static NTSTATUS snapshot_unicode_string( UNICODE_STRING *str, const UNICODE_STRING32 *str32 )
{
    void *buffer = NULL;

    if (str32->Length > str32->MaximumLength || (str32->Length & (sizeof(WCHAR) - 1)))
        return STATUS_INVALID_PARAMETER;
    if (str32->Length)
    {
        if (!str32->Buffer) return STATUS_ACCESS_VIOLATION;
        buffer = alloc_temp( str32->Length );
        wow64_read_user( buffer, wow64_guest_memory_ptr( str32->Buffer ), str32->Length );
    }
    str->Length = str32->Length;
    str->MaximumLength = str32->MaximumLength;
    str->Buffer = buffer;
    return STATUS_SUCCESS;
}


static NTSTATUS process_params_32to64( RTL_USER_PROCESS_PARAMETERS **params,
                                       const RTL_USER_PROCESS_PARAMETERS32 *params32 )
{
    RTL_USER_PROCESS_PARAMETERS32 local;
    UNICODE_STRING image, dllpath, curdir, cmdline, title, desktop, shell, runtime;
    WCHAR *environment = NULL;
    RTL_USER_PROCESS_PARAMETERS *ret;
    NTSTATUS status;

    *params = NULL;
    wow64_read_user( &local, params32, sizeof(local) );
    if ((status = snapshot_unicode_string( &image, &local.ImagePathName )) ||
        (status = snapshot_unicode_string( &dllpath, &local.DllPath )) ||
        (status = snapshot_unicode_string( &curdir, &local.CurrentDirectory.DosPath )) ||
        (status = snapshot_unicode_string( &cmdline, &local.CommandLine )) ||
        (status = snapshot_unicode_string( &title, &local.WindowTitle )) ||
        (status = snapshot_unicode_string( &desktop, &local.Desktop )) ||
        (status = snapshot_unicode_string( &shell, &local.ShellInfo )) ||
        (status = snapshot_unicode_string( &runtime, &local.RuntimeInfo )))
        return status;

    if (local.Environment)
    {
        if ((status = snapshot_environment( &environment, local.Environment,
                                             local.EnvironmentSize )))
            return status;
    }

    if ((status = RtlCreateProcessParametersEx( &ret, &image, &dllpath, &curdir, &cmdline,
                                                environment, &title, &desktop, &shell, &runtime,
                                                PROCESS_PARAMS_FLAG_NORMALIZED )))
        return status;

    ret->DebugFlags            = local.DebugFlags;
    ret->ConsoleHandle         = LongToHandle( local.ConsoleHandle );
    ret->ConsoleFlags          = local.ConsoleFlags;
    ret->hStdInput             = LongToHandle( local.hStdInput );
    ret->hStdOutput            = LongToHandle( local.hStdOutput );
    ret->hStdError             = LongToHandle( local.hStdError );
    ret->dwX                   = local.dwX;
    ret->dwY                   = local.dwY;
    ret->dwXSize               = local.dwXSize;
    ret->dwYSize               = local.dwYSize;
    ret->dwXCountChars         = local.dwXCountChars;
    ret->dwYCountChars         = local.dwYCountChars;
    ret->dwFillAttribute       = local.dwFillAttribute;
    ret->dwFlags               = local.dwFlags;
    ret->wShowWindow           = local.wShowWindow;
    ret->EnvironmentVersion    = local.EnvironmentVersion;
    ret->PackageDependencyData = wow64_guest_memory_ptr( local.PackageDependencyData );
    ret->ProcessGroupId        = local.ProcessGroupId;
    ret->LoaderThreads         = local.LoaderThreads;
    *params = ret;
    return STATUS_SUCCESS;
}


static NTSTATUS ps_create_info_32to64( PS_CREATE_INFO *info, PS_CREATE_INFO32 *snapshot,
                                       const PS_CREATE_INFO32 *info32 )
{
    wow64_read_user( snapshot, info32, sizeof(*snapshot) );
    if (snapshot->Size != sizeof(*snapshot) || snapshot->State >= PsCreateMaximumStates)
        return STATUS_INVALID_PARAMETER;

    memset( info, 0, sizeof(*info) );
    info->Size = sizeof(*info);
    info->State = snapshot->State;
    if (snapshot->State == PsCreateInitialState)
    {
        info->InitState.InitFlags = snapshot->InitState.InitFlags;
        info->InitState.AdditionalFileAccess = snapshot->InitState.AdditionalFileAccess;
    }
    return STATUS_SUCCESS;
}


static void ps_create_info_64to32( PS_CREATE_INFO32 *local,
                                   const PS_CREATE_INFO32 *snapshot,
                                   const PS_CREATE_INFO *info )
{
    *local = *snapshot;

    local->State = info->State;
    switch (info->State)
    {
    case PsCreateInitialState:
        local->InitState.InitFlags            = info->InitState.InitFlags;
        local->InitState.AdditionalFileAccess = info->InitState.AdditionalFileAccess;
        break;
    case PsCreateFailOnSectionCreate:
        local->FailSection.FileHandle = HandleToLong( info->FailSection.FileHandle );
        break;
    case PsCreateFailExeFormat:
        local->ExeFormat.DllCharacteristics = info->ExeFormat.DllCharacteristics;
        break;
    case PsCreateFailExeName:
        local->ExeName.IFEOKey = HandleToLong( info->ExeName.IFEOKey );
        break;
    case PsCreateSuccess:
        local->SuccessState.OutputFlags                 = info->SuccessState.OutputFlags;
        local->SuccessState.FileHandle                  = HandleToLong( info->SuccessState.FileHandle );
        local->SuccessState.SectionHandle               = HandleToLong( info->SuccessState.SectionHandle );
        local->SuccessState.UserProcessParametersNative = info->SuccessState.UserProcessParametersNative;
        local->SuccessState.UserProcessParametersWow64  = info->SuccessState.UserProcessParametersWow64;
        local->SuccessState.CurrentParameterFlags       = info->SuccessState.CurrentParameterFlags;
        local->SuccessState.PebAddressNative            = info->SuccessState.PebAddressNative;
        local->SuccessState.PebAddressWow64             = info->SuccessState.PebAddressWow64;
        local->SuccessState.ManifestAddress             = info->SuccessState.ManifestAddress;
        local->SuccessState.ManifestSize                = info->SuccessState.ManifestSize;
        break;
    default:
        break;
    }
}


static NTSTATUS put_ps_create_info( PS_CREATE_INFO32 *info32, const PS_CREATE_INFO32 *snapshot,
                                    const PS_CREATE_INFO *info )
{
    PS_CREATE_INFO32 local;

    ps_create_info_64to32( &local, snapshot, info );
    return wow64_try_write_user( info32, &local, sizeof(local) );
}


static NTSTATUS ps_attributes_32to64( PS_ATTRIBUTE_LIST **attr, PS_ATTRIBUTE_LIST32 **snapshot,
                                      ULONG *attr_count, const PS_ATTRIBUTE_LIST32 *attr32 )
{
    const SIZE_T header32 = offsetof( PS_ATTRIBUTE_LIST32, Attributes );
    const SIZE_T header = offsetof( PS_ATTRIBUTE_LIST, Attributes );
    PS_ATTRIBUTE_LIST32 *local;
    PS_ATTRIBUTE_LIST *ret;
    SIZE_T native_size;
    ULONG total, i, count;

    *attr = NULL;
    *snapshot = NULL;
    *attr_count = 0;
    if (!attr32) return STATUS_SUCCESS;

    wow64_read_user( &total, attr32, sizeof(total) );
    if (total < header32 || (total - header32) % sizeof(PS_ATTRIBUTE32))
        return STATUS_INVALID_PARAMETER;
    count = (total - header32) / sizeof(PS_ATTRIBUTE32);
    if (count > PsAttributeMax) return STATUS_INVALID_PARAMETER;
    if (!multiply_size( count, sizeof(PS_ATTRIBUTE), &native_size ) ||
        native_size > ~(SIZE_T)0 - header)
        return STATUS_INVALID_PARAMETER;
    native_size += header;
    local = alloc_temp( total );
    wow64_read_user( local, attr32, total );
    if (local->TotalLength != total) return STATUS_INVALID_PARAMETER;
    ret = alloc_temp( native_size );
    memset( ret, 0, native_size );
    ret->TotalLength = native_size;
    for (i = 0; i < count; i++)
    {
        const PS_ATTRIBUTE32 *in = &local->Attributes[i];

        ret->Attributes[i].Attribute    = in->Attribute;
        ret->Attributes[i].Size         = in->Size;
        ret->Attributes[i].Value        = in->Value;
        ret->Attributes[i].ReturnLength = NULL;
        switch (ret->Attributes[i].Attribute)
        {
        case PS_ATTRIBUTE_IMAGE_NAME:
            {
                OBJECT_ATTRIBUTES objattr;
                UNICODE_STRING path;
                void *buffer = NULL;

                if (in->Size > (USHORT)~0 || (in->Size & (sizeof(WCHAR) - 1)))
                    return STATUS_INVALID_PARAMETER;
                if (in->Size)
                {
                    if (!in->Value) return STATUS_ACCESS_VIOLATION;
                    buffer = alloc_temp( in->Size );
                    wow64_read_user( buffer, wow64_guest_memory_ptr( in->Value ), in->Size );
                }
                path.Length = path.MaximumLength = in->Size;
                path.Buffer = buffer;
                ret->Attributes[i].ValuePtr = path.Buffer;
                InitializeObjectAttributes( &objattr, &path, OBJ_CASE_INSENSITIVE, 0, 0 );
                if (get_file_redirect( &objattr ))
                {
                    ret->Attributes[i].Size = objattr.ObjectName->Length;
                    ret->Attributes[i].ValuePtr = objattr.ObjectName->Buffer;
                }
            }
            break;
        case PS_ATTRIBUTE_HANDLE_LIST:
        case PS_ATTRIBUTE_JOB_LIST:
            {
                LONG *handles32;
                ULONG j, handles_count;

                if (in->Size % sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
                handles_count = in->Size / sizeof(ULONG);
                /* The server handle table cannot contain more entries than this.  Reject an
                 * attacker-controlled widened allocation that could never be consumed. */
                if (handles_count > WOW64_MAX_SERVER_HANDLE_ENTRIES)
                    return STATUS_INVALID_PARAMETER;
                if (in->Size && !in->Value) return STATUS_ACCESS_VIOLATION;
                if (!multiply_size( handles_count, sizeof(HANDLE), &ret->Attributes[i].Size ))
                    return STATUS_INVALID_PARAMETER;
                ret->Attributes[i].ValuePtr = alloc_temp( ret->Attributes[i].Size );
                handles32 = alloc_temp( in->Size );
                if (in->Size) wow64_read_user( handles32, wow64_guest_memory_ptr( in->Value ), in->Size );
                for (j = 0; j < handles_count; j++)
                    ((HANDLE *)ret->Attributes[i].ValuePtr)[j] = LongToHandle( handles32[j] );
            }
            break;
        case PS_ATTRIBUTE_PARENT_PROCESS:
        case PS_ATTRIBUTE_DEBUG_PORT:
        case PS_ATTRIBUTE_TOKEN:
            ret->Attributes[i].Size     = sizeof(HANDLE);
            ret->Attributes[i].ValuePtr = LongToHandle( in->Value );
            break;
        case PS_ATTRIBUTE_CLIENT_ID:
            ret->Attributes[i].Size     = sizeof(CLIENT_ID);
            ret->Attributes[i].ValuePtr = alloc_temp( ret->Attributes[i].Size );
            memset( ret->Attributes[i].ValuePtr, 0, ret->Attributes[i].Size );
            wow64_probe_user_write( wow64_guest_memory_ptr( in->Value ),
                                    min( in->Size, sizeof(CLIENT_ID32) ));
            if (in->ReturnLength)
                wow64_probe_user_write( wow64_guest_memory_ptr( in->ReturnLength ), sizeof(ULONG) );
            break;
        case PS_ATTRIBUTE_IMAGE_INFO:
            ret->Attributes[i].Size     = sizeof(SECTION_IMAGE_INFORMATION);
            ret->Attributes[i].ValuePtr = alloc_temp( ret->Attributes[i].Size );
            memset( ret->Attributes[i].ValuePtr, 0, ret->Attributes[i].Size );
            wow64_probe_user_write( wow64_guest_memory_ptr( in->Value ),
                                    min( in->Size, sizeof(SECTION_IMAGE_INFORMATION32) ));
            if (in->ReturnLength)
                wow64_probe_user_write( wow64_guest_memory_ptr( in->ReturnLength ), sizeof(ULONG) );
            break;
        case PS_ATTRIBUTE_TEB_ADDRESS:
            ret->Attributes[i].Size     = sizeof(TEB *);
            ret->Attributes[i].ValuePtr = alloc_temp( ret->Attributes[i].Size );
            memset( ret->Attributes[i].ValuePtr, 0, ret->Attributes[i].Size );
            wow64_probe_user_write( wow64_guest_memory_ptr( in->Value ),
                                    min( in->Size, sizeof(ULONG) ));
            if (in->ReturnLength)
                wow64_probe_user_write( wow64_guest_memory_ptr( in->ReturnLength ), sizeof(ULONG) );
            break;
        case PS_ATTRIBUTE_GROUP_AFFINITY:
            {
                GROUP_AFFINITY32 aff32;
                GROUP_AFFINITY *aff64;

                if (in->Size < sizeof(aff32)) return STATUS_INVALID_PARAMETER;
                wow64_read_user( &aff32, wow64_guest_memory_ptr( in->Value ), sizeof(aff32) );
                ret->Attributes[i].Size     = sizeof(GROUP_AFFINITY);
                ret->Attributes[i].ValuePtr = alloc_temp( ret->Attributes[i].Size );
                aff64 = ret->Attributes[i].ValuePtr;
                aff64->Mask = aff32.Mask;
                aff64->Group = aff32.Group;
                aff64->Reserved[0] = aff32.Reserved[0];
                aff64->Reserved[1] = aff32.Reserved[1];
                aff64->Reserved[2] = aff32.Reserved[2];
            }
            break;
        }
    }
    *attr = ret;
    *snapshot = local;
    *attr_count = count;
    return STATUS_SUCCESS;
}


static NTSTATUS put_ps_attributes( const PS_ATTRIBUTE_LIST32 *attr32, ULONG count,
                                  const PS_ATTRIBUTE_LIST *attr )
{
    NTSTATUS status;
    ULONG i;

    if (!attr32) return STATUS_SUCCESS;
    for (i = 0; i < count; i++)
    {
        switch (attr->Attributes[i].Attribute)
        {
        case PS_ATTRIBUTE_CLIENT_ID:
        {
            CLIENT_ID32 id32;
            ULONG size = min( attr32->Attributes[i].Size, sizeof(id32) );
            const CLIENT_ID *id = attr->Attributes[i].ValuePtr;

            id32.UniqueProcess = HandleToLong( id->UniqueProcess );
            id32.UniqueThread = HandleToLong( id->UniqueThread );
            status = wow64_try_write_user( wow64_guest_memory_ptr( attr32->Attributes[i].Value ),
                                           &id32, size );
            if (status) return status;
            if (attr32->Attributes[i].ReturnLength)
            {
                ULONG length = size;

                status = wow64_try_write_user( wow64_guest_memory_ptr(
                                               attr32->Attributes[i].ReturnLength ),
                                               &length, sizeof(length) );
                if (status) return status;
            }
            break;
        }
        case PS_ATTRIBUTE_IMAGE_INFO:
        {
            SECTION_IMAGE_INFORMATION32 info32;
            ULONG size = min( attr32->Attributes[i].Size, sizeof(info32) );
            put_section_image_info( &info32, attr->Attributes[i].ValuePtr );
            status = wow64_try_write_user( wow64_guest_memory_ptr( attr32->Attributes[i].Value ),
                                           &info32, size );
            if (status) return status;
            if (attr32->Attributes[i].ReturnLength)
            {
                ULONG length = size;

                status = wow64_try_write_user( wow64_guest_memory_ptr(
                                               attr32->Attributes[i].ReturnLength ),
                                               &length, sizeof(length) );
                if (status) return status;
            }
            break;
        }
        case PS_ATTRIBUTE_TEB_ADDRESS:
        {
            TEB **teb = attr->Attributes[i].ValuePtr;
            ULONG teb32 = wow64_guest_memory_addr( *teb ) + 0x2000;
            ULONG size = min( attr32->Attributes[i].Size, sizeof(teb32) );
            status = wow64_try_write_user( wow64_guest_memory_ptr( attr32->Attributes[i].Value ),
                                           &teb32, size );
            if (status) return status;
            if (attr32->Attributes[i].ReturnLength)
            {
                ULONG length = size;

                status = wow64_try_write_user( wow64_guest_memory_ptr(
                                               attr32->Attributes[i].ReturnLength ),
                                               &length, sizeof(length) );
                if (status) return status;
            }
            break;
        }
        }
    }
    return STATUS_SUCCESS;
}

void put_vm_counters( VM_COUNTERS_EX32 *info32, const VM_COUNTERS_EX *info, ULONG size )
{
    info32->PeakVirtualSize            = info->PeakVirtualSize;
    info32->VirtualSize                = info->VirtualSize;
    info32->PageFaultCount             = info->PageFaultCount;
    info32->PeakWorkingSetSize         = info->PeakWorkingSetSize;
    info32->WorkingSetSize             = info->WorkingSetSize;
    info32->QuotaPeakPagedPoolUsage    = info->QuotaPeakPagedPoolUsage;
    info32->QuotaPagedPoolUsage        = info->QuotaPagedPoolUsage;
    info32->QuotaPeakNonPagedPoolUsage = info->QuotaPeakNonPagedPoolUsage;
    info32->QuotaNonPagedPoolUsage     = info->QuotaNonPagedPoolUsage;
    info32->PagefileUsage              = info->PagefileUsage;
    info32->PeakPagefileUsage          = info->PeakPagefileUsage;
    if (size == sizeof(VM_COUNTERS_EX32)) info32->PrivateUsage = info->PrivateUsage;
}


static NTSTATUS query_process_buffer( HANDLE handle, PROCESSINFOCLASS class, void *ptr,
                                      ULONG len, ULONG *retlen )
{
    const ULONG untouched = 0xdeadbeef;
    ULONG result = untouched;
    SIZE_T capacity;
    void *buffer = NULL;
    ULONG copy_size = 0;
    BOOL oversized_output = FALSE;
    NTSTATUS status;

    switch (class)
    {
    case ProcessIoCounters:              capacity = sizeof(IO_COUNTERS); break;
    case ProcessTimes:                   capacity = sizeof(KERNEL_USER_TIMES); break;
    case ProcessDefaultHardErrorMode:    capacity = sizeof(ULONG); break;
    case ProcessPriorityClass:           capacity = sizeof(PROCESS_PRIORITY_CLASS); break;
    case ProcessPriorityBoost:           capacity = sizeof(ULONG); break;
    case ProcessHandleCount:             capacity = sizeof(ULONG); break;
    case ProcessSessionInformation:      capacity = sizeof(ULONG); break;
    case ProcessDebugFlags:              capacity = sizeof(ULONG); break;
    case ProcessExecuteFlags:            capacity = sizeof(ULONG); break;
    case ProcessCookie:                  capacity = sizeof(ULONG); break;
    case ProcessCycleTime:               capacity = sizeof(PROCESS_CYCLE_TIME_INFORMATION); break;
    case ProcessPowerThrottlingState:    capacity = sizeof(PROCESS_POWER_THROTTLING_STATE); break;
    default:                             return STATUS_INVALID_INFO_CLASS;
    }

    if (!ptr)
    {
        status = NtQueryInformationProcess( handle, class, NULL, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    oversized_output = class == ProcessIoCounters || class == ProcessTimes ||
                       class == ProcessHandleCount;
    if (len < capacity || (!oversized_output && len != capacity))
    {
        status = NtQueryInformationProcess( handle, class, NULL, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    buffer = alloc_temp( capacity );
    if (buffer) memset( buffer, 0, capacity );
    status = NtQueryInformationProcess( handle, class, buffer, capacity, &result );
    if (oversized_output && len > capacity) status = STATUS_INFO_LENGTH_MISMATCH;
    if (!status) copy_size = result == untouched ? capacity : min( (ULONG)capacity, result );
    else if (status == STATUS_INFO_LENGTH_MISMATCH && result != untouched)
    {
        if (oversized_output && result <= capacity) copy_size = result;
    }
    else if (result != untouched && (class == ProcessPriorityBoost || class == ProcessDebugFlags))
        copy_size = min( (ULONG)capacity, result );
    if (copy_size)
    {
        if (ptr) wow64_write_user( ptr, buffer, copy_size );
        else status = STATUS_ACCESS_VIOLATION;
    }
    if (retlen && result != untouched) put_size( retlen, result );
    return status;
}


static NTSTATUS query_process_handle_table( HANDLE handle, void *ptr, ULONG len,
                                            ULONG *retlen )
{
    const ULONG untouched = 0xdeadbeef;
    ULONG result = untouched, required;
    void *buffer;
    NTSTATUS status;

    if (!ptr && len)
    {
        status = NtQueryInformationProcess( handle, ProcessHandleTable, NULL, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    status = NtQueryInformationProcess( handle, ProcessHandleTable, NULL, 0, &result );
    if (result == untouched)
    {
        return status;
    }
    if (status && status != STATUS_INFO_LENGTH_MISMATCH)
    {
        if (retlen) put_size( retlen, result );
        return status;
    }
    if (result % sizeof(ULONG) || result / sizeof(ULONG) > WOW64_MAX_SERVER_HANDLE_ENTRIES)
    {
        if (retlen) put_size( retlen, result );
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (len < result)
    {
        if (retlen) put_size( retlen, result );
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!result)
    {
        if (retlen) put_size( retlen, 0 );
        return STATUS_SUCCESS;
    }

    buffer = alloc_temp( result );
    memset( buffer, 0, result );
    required = untouched;
    status = NtQueryInformationProcess( handle, ProcessHandleTable, buffer, result, &required );
    if (!status)
    {
        if (required > result || required % sizeof(ULONG))
            return STATUS_INFO_LENGTH_MISMATCH;
        if (required) wow64_write_user( ptr, buffer, required );
    }
    if (retlen && required != untouched) put_size( retlen, required );
    return status;
}


static NTSTATUS query_thread_buffer( HANDLE handle, THREADINFOCLASS class, void *ptr,
                                     ULONG len, ULONG *retlen )
{
    const ULONG untouched = 0xdeadbeef;
    ULONG result = untouched;
    SIZE_T capacity;
    void *buffer;
    ULONG copy_size = 0;
    NTSTATUS status;

    switch (class)
    {
    case ThreadTimes:                       capacity = sizeof(KERNEL_USER_TIMES); break;
    case ThreadEnableAlignmentFaultFixup:   capacity = sizeof(BOOLEAN); break;
    case ThreadAmILastThread:               capacity = sizeof(ULONG); break;
    case ThreadIsIoPending:                 capacity = sizeof(ULONG); break;
    case ThreadIsTerminated:                capacity = sizeof(ULONG); break;
    case ThreadHideFromDebugger:            capacity = sizeof(BOOLEAN); break;
    case ThreadSuspendCount:                capacity = sizeof(ULONG); break;
    case ThreadPriorityBoost:               capacity = sizeof(ULONG); break;
    case ThreadIdealProcessorEx:            capacity = sizeof(PROCESSOR_NUMBER); break;
    case ThreadCycleTime:                   capacity = sizeof(PROCESS_CYCLE_TIME_INFORMATION); break;
    case ThreadPagePriority:                capacity = sizeof(MEMORY_PRIORITY_INFORMATION); break;
    case ThreadPowerThrottlingState:        capacity = sizeof(THREAD_POWER_THROTTLING_STATE); break;
    default:                                return STATUS_INVALID_INFO_CLASS;
    }
    if (class == ThreadHideFromDebugger && retlen)
        wow64_probe_user_write( retlen, sizeof(*retlen) );
    if (!ptr)
    {
        status = NtQueryInformationThread( handle, class, NULL, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }
    if (class == ThreadTimes) capacity = min( capacity, len );
    else if (len != capacity)
    {
        status = NtQueryInformationThread( handle, class, NULL, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }
    buffer = alloc_temp( capacity );
    if (buffer) memset( buffer, 0, capacity );
    status = NtQueryInformationThread( handle, class, buffer, capacity, &result );
    if (!status) copy_size = result == untouched ? capacity : min( (ULONG)capacity, result );
    if (copy_size)
    {
        if (ptr) wow64_write_user( ptr, buffer, copy_size );
        else status = STATUS_ACCESS_VIOLATION;
    }
    if (retlen && result != untouched) put_size( retlen, result );
    return status;
}


static NTSTATUS publish_handle_result( ULONG *handle_ptr, HANDLE handle, NTSTATUS status )
{
    NTSTATUS publish_status = try_put_handle( handle_ptr, handle );

    if (!publish_status) return status;
    if (handle) NtClose( handle );
    return publish_status;
}


struct create_user_thread_cleanup
{
    struct process_address_codec codec;
    HANDLE handle;
    HANDLE transaction;
    NTSTATUS status;
};


static void cleanup_create_user_thread( BOOL normal, void *arg )
{
    struct create_user_thread_cleanup *cleanup = arg;

    if (cleanup->transaction)
        __wine_wow64_complete_user_thread( cleanup->transaction, FALSE, cleanup->status );
    if (cleanup->handle) NtClose( cleanup->handle );
    close_process_address_codec( normal, &cleanup->codec );
}


struct create_user_process_cleanup
{
    HANDLE process;
    HANDLE thread;
    HANDLE transaction;
    RTL_USER_PROCESS_PARAMETERS *params;
    NTSTATUS status;
};


static void cleanup_create_user_process( BOOL normal, void *arg )
{
    struct create_user_process_cleanup *cleanup = arg;

    (void)normal;
    if (cleanup->transaction)
        __wine_wow64_complete_user_process( cleanup->transaction, FALSE, cleanup->status );
    if (cleanup->thread) NtClose( cleanup->thread );
    if (cleanup->process) NtClose( cleanup->process );
    if (cleanup->params) RtlDestroyProcessParameters( cleanup->params );
}


/**********************************************************************
 *           wow64_NtAlertMultipleThreadByThreadId
 */
NTSTATUS WINAPI wow64_NtAlertMultipleThreadByThreadId( UINT *args )
{
    LONG *handles_ptr = get_ptr( &args );
    ULONG count = get_ulong( &args );
    void *unk1 = get_raw_ptr( &args );
    void *unk2 = get_raw_ptr( &args );
    LONG handles32_buf[256], *handles32;
    HANDLE handles_buf[256], *handles;
    SIZE_T guest_size, native_size;
    unsigned int i;

    if (!multiply_size( count, sizeof(*handles32), &guest_size ) ||
        !multiply_size( count, sizeof(*handles), &native_size ))
        return STATUS_INVALID_PARAMETER;
    if (guest_size) wow64_probe_user_read( handles_ptr, guest_size );
    if (count <= ARRAY_SIZE(handles_buf))
    {
        handles32 = handles32_buf;
        handles = handles_buf;
    }
    else
    {
        handles32 = alloc_temp( guest_size );
        handles = alloc_temp( native_size );
    }
    if (guest_size) wow64_read_user( handles32, handles_ptr, guest_size );
    for (i = 0; i < count; ++i) handles[i] = LongToHandle( handles32[i] );

    return NtAlertMultipleThreadByThreadId( handles, count, unk1, unk2 );
}


/**********************************************************************
 *           wow64_NtAlertResumeThread
 */
NTSTATUS WINAPI wow64_NtAlertResumeThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG *count = get_ptr( &args );

    ULONG local;
    NTSTATUS status;

    if (count) wow64_probe_user_write( count, sizeof(*count) );
    status = NtAlertResumeThread( handle, count ? &local : NULL );
    if (!status && count) wow64_write_user( count, &local, sizeof(local) );
    return status;
}


/**********************************************************************
 *           wow64_NtAlertThread
 */
NTSTATUS WINAPI wow64_NtAlertThread( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtAlertThread( handle );
}


/**********************************************************************
 *           wow64_NtAlertThreadByThreadId
 */
NTSTATUS WINAPI wow64_NtAlertThreadByThreadId( UINT *args )
{
    HANDLE tid = get_handle( &args );

    return NtAlertThreadByThreadId( tid );
}


/**********************************************************************
 *           wow64_NtAssignProcessToJobObject
 */
NTSTATUS WINAPI wow64_NtAssignProcessToJobObject( UINT *args )
{
    HANDLE job = get_handle( &args );
    HANDLE process = get_handle( &args );

    return NtAssignProcessToJobObject( job, process );
}


/**********************************************************************
 *           wow64_NtCreateThread
 */
NTSTATUS WINAPI wow64_NtCreateThread( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    HANDLE process = get_handle( &args );
    CLIENT_ID32 *id32 = get_ptr( &args );
    I386_CONTEXT *context = get_ptr( &args );
    void *initial_teb = get_ptr( &args );
    BOOLEAN suspended = get_ulong( &args );

    FIXME( "%p %lx %p %p %p %p %p %u: stub\n", handle_ptr, access, attr32, process,
           id32, context, initial_teb, suspended );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtCreateThreadEx
 */
NTSTATUS WINAPI wow64_NtCreateThreadEx( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    HANDLE process = get_handle( &args );
    ULONG start = get_ulong( &args );
    ULONG param = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    ULONG_PTR zero_bits = get_ulong( &args );
    SIZE_T stack_commit = get_ulong( &args );
    SIZE_T stack_reserve = get_ulong( &args );
    PS_ATTRIBUTE_LIST32 *attr_list32 = get_ptr( &args );

    struct object_attr64 attr;
    PS_ATTRIBUTE_LIST32 *attr_snapshot;
    PS_ATTRIBUTE_LIST *attr_list;
    ULONG attr_count;
    struct wine_wow64_create_user_thread_params create =
    {
        .version = WINE_WOW64_CREATE_USER_THREAD_VERSION,
        .size = sizeof(create),
    };
    struct create_user_thread_cleanup cleanup = {0};
    NTSTATUS status, publish_status = STATUS_SUCCESS, complete_status;

    put_handle( handle_ptr, 0 );
    if ((status = init_process_address_codec( process, &cleanup.codec ))) return status;

    __TRY
    {
        do
        {
            if ((status = ps_attributes_32to64( &attr_list, &attr_snapshot, &attr_count,
                                                attr_list32 ))) break;

            create.process = (ULONG_PTR)cleanup.codec.process;
            create.object_attributes = (ULONG_PTR)objattr_32to64( &attr, attr32 );
            create.attribute_list = (ULONG_PTR)attr_list;
            create.start = (ULONG_PTR)decode_process_address( &cleanup.codec, start );
            create.param = (ULONG_PTR)decode_process_address( &cleanup.codec, param );
            create.zero_bits = get_zero_bits( zero_bits );
            create.stack_commit = stack_commit;
            create.stack_reserve = stack_reserve;
            create.access = access;
            create.flags = flags;

            status = __wine_wow64_create_user_thread( &create );
            cleanup.handle = (HANDLE)(ULONG_PTR)create.handle;
            cleanup.transaction = (HANDLE)(ULONG_PTR)create.transaction;
            cleanup.status = status;
            if ((!status && (!cleanup.handle || !cleanup.transaction)) ||
                (status && (cleanup.handle || cleanup.transaction)))
                RtlExitUserProcess( STATUS_INVALID_PARAMETER );

            publish_status = put_ps_attributes( attr_snapshot, attr_count, attr_list );
            if (!publish_status)
                publish_status = try_put_handle( handle_ptr, cleanup.handle );
            if (publish_status)
            {
                cleanup.status = publish_status;
                status = publish_status;
                break;
            }

            if (!cleanup.transaction)
            {
                cleanup.handle = 0;
                break;
            }

            /* The final handle publication transfers ownership to the guest.
             * Commit only releases the private transaction suspension; failure
             * after publication cannot be returned with a stale visible handle. */
            cleanup.handle = 0;
            complete_status = __wine_wow64_complete_user_thread(
                cleanup.transaction, TRUE, STATUS_SUCCESS );
            cleanup.transaction = 0;
            if (complete_status) RtlExitUserProcess( complete_status );
        } while (0);
    }
    __FINALLY_CTX( cleanup_create_user_thread, &cleanup )

    return status;
}


/**********************************************************************
 *           wow64_NtCreateUserProcess
 */
NTSTATUS WINAPI wow64_NtCreateUserProcess( UINT *args )
{
    ULONG *process_handle_ptr = get_ptr( &args );
    ULONG *thread_handle_ptr = get_ptr( &args );
    ACCESS_MASK process_access = get_ulong( &args );
    ACCESS_MASK thread_access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *process_attr32 = get_ptr( &args );
    OBJECT_ATTRIBUTES32 *thread_attr32 = get_ptr( &args );
    ULONG process_flags = get_ulong( &args );
    ULONG thread_flags = get_ulong( &args );
    RTL_USER_PROCESS_PARAMETERS32 *params32 = get_ptr( &args );
    PS_CREATE_INFO32 *info32 = get_ptr( &args );
    PS_ATTRIBUTE_LIST32 *attr32 = get_ptr( &args );

    struct object_attr64 process_attr, thread_attr;
    OBJECT_ATTRIBUTES *process_attr_ptr, *thread_attr_ptr;
    RTL_USER_PROCESS_PARAMETERS *params;
    PS_CREATE_INFO info, non_mz_native_info = {0};
    PS_CREATE_INFO32 info_snapshot, non_mz_info;
    PS_ATTRIBUTE_LIST32 *attr_snapshot;
    PS_ATTRIBUTE_LIST *attr;
    ULONG attr_count;
    struct wine_wow64_create_user_process_params create =
    {
        .version = WINE_WOW64_CREATE_USER_PROCESS_VERSION,
        .size = sizeof(create),
    };
    struct create_user_process_cleanup cleanup = {0};
    NTSTATUS status, publish_status = STATUS_SUCCESS, complete_status;

    if ((status = try_put_handle_pair( process_handle_ptr, 0, thread_handle_ptr, 0 )))
        return status;
    if ((status = process_params_32to64( &params, params32 ))) return status;
    cleanup.params = params;

    __TRY
    {
        do
        {
            if ((status = ps_create_info_32to64( &info, &info_snapshot, info32 ))) break;
            wow64_probe_user_write( info32, sizeof(*info32) );
            if ((status = ps_attributes_32to64( &attr, &attr_snapshot, &attr_count,
                                                attr32 ))) break;

            process_attr_ptr = objattr_32to64( &process_attr, process_attr32 );
            thread_attr_ptr = objattr_32to64( &thread_attr, thread_attr32 );
            ps_create_info_64to32( &non_mz_info, &info_snapshot, &non_mz_native_info );
            create.process_attributes = (ULONG_PTR)process_attr_ptr;
            create.thread_attributes = (ULONG_PTR)thread_attr_ptr;
            create.process_parameters = (ULONG_PTR)params;
            create.create_info = (ULONG_PTR)&info;
            create.attribute_list = (ULONG_PTR)attr;
            create.guest_process_handle = (ULONG_PTR)process_handle_ptr;
            create.guest_thread_handle = (ULONG_PTR)thread_handle_ptr;
            create.guest_create_info = (ULONG_PTR)info32;
            create.non_mz_create_info = (ULONG_PTR)&non_mz_info;
            create.process_access = process_access;
            create.thread_access = thread_access;
            create.process_flags = process_flags;
            create.thread_flags = thread_flags;
            create.non_mz_create_info_size = sizeof(non_mz_info);

            status = __wine_wow64_create_user_process( &create );
            cleanup.process = (HANDLE)(ULONG_PTR)create.process_handle;
            cleanup.thread = (HANDLE)(ULONG_PTR)create.thread_handle;
            cleanup.transaction = (HANDLE)(ULONG_PTR)create.transaction;
            cleanup.status = status;

            if (create.result == WINE_WOW64_CREATE_USER_PROCESS_NON_MZ_COMMITTED)
            {
                if (status || cleanup.process || cleanup.thread || cleanup.transaction)
                    RtlExitUserProcess( STATUS_INVALID_PARAMETER );
            }
            else
            {
                if (create.result == WINE_WOW64_CREATE_USER_PROCESS_PE_TRANSACTION)
                {
                    if (status || !cleanup.transaction || !cleanup.process || !cleanup.thread)
                        publish_status = STATUS_INVALID_PARAMETER;
                }
                else if (create.result != WINE_WOW64_CREATE_USER_PROCESS_NONE || !status ||
                         cleanup.transaction || cleanup.process || cleanup.thread)
                    publish_status = STATUS_INVALID_PARAMETER;

                if (!publish_status)
                    publish_status = put_ps_create_info( info32, &info_snapshot, &info );
                if (!publish_status)
                    publish_status = put_ps_attributes( attr_snapshot, attr_count, attr );
                if (!publish_status)
                    publish_status = try_put_handle_pair( process_handle_ptr, cleanup.process,
                                                          thread_handle_ptr, cleanup.thread );
                if (!publish_status && cleanup.transaction)
                {
                    /* The atomic pair publication transfers handle ownership to the guest.
                     * A commit failure after that point cannot be returned without exposing
                     * handles to a process which the token close would cancel. */
                    cleanup.process = cleanup.thread = 0;
                    complete_status = __wine_wow64_complete_user_process(
                        cleanup.transaction, TRUE, STATUS_SUCCESS );
                    cleanup.transaction = 0;
                    if (complete_status) RtlExitUserProcess( complete_status );
                }
                if (publish_status)
                {
                    cleanup.status = publish_status;
                    status = publish_status;
                }
                else if (!cleanup.transaction)
                {
                    /* No transaction means either a failure result with zero handles or a
                     * successfully committed pair whose ownership has already transferred. */
                    cleanup.process = cleanup.thread = 0;
                }
            }
        } while (0);
    }
    __FINALLY_CTX( cleanup_create_user_process, &cleanup )

    return status;
}


/**********************************************************************
 *           wow64_NtDebugActiveProcess
 */
NTSTATUS WINAPI wow64_NtDebugActiveProcess( UINT *args )
{
    HANDLE process = get_handle( &args );
    HANDLE debug = get_handle( &args );

    return NtDebugActiveProcess( process, debug );
}


/**********************************************************************
 *           wow64_NtFlushProcessWriteBuffers
 */
NTSTATUS WINAPI wow64_NtFlushProcessWriteBuffers( UINT *args )
{
    return NtFlushProcessWriteBuffers();
}


/**********************************************************************
 *           wow64_NtGetNextProcess
 */
NTSTATUS WINAPI wow64_NtGetNextProcess( UINT *args )
{
    HANDLE process = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    ULONG attributes = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    HANDLE handle = 0;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtGetNextProcess( process, access, attributes, flags, &handle );
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtGetNextThread
 */
NTSTATUS WINAPI wow64_NtGetNextThread( UINT *args )
{
    HANDLE process = get_handle( &args );
    HANDLE thread = get_handle( &args );
    ACCESS_MASK access = get_ulong( &args );
    ULONG attributes = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    ULONG *handle_ptr = get_ptr( &args );

    HANDLE handle = 0;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtGetNextThread( process, thread, access, attributes, flags, &handle );
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtIsProcessInJob
 */
NTSTATUS WINAPI wow64_NtIsProcessInJob( UINT *args )
{
    HANDLE process = get_handle( &args );
    HANDLE job = get_handle( &args );

    return NtIsProcessInJob( process, job );
}


/**********************************************************************
 *           wow64_NtOpenProcess
 */
NTSTATUS WINAPI wow64_NtOpenProcess( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    CLIENT_ID32 *id32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    CLIENT_ID id;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtOpenProcess( &handle, access, objattr_32to64( &attr, attr32 ), client_id_32to64( &id, id32 ));
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtOpenThread
 */
NTSTATUS WINAPI wow64_NtOpenThread( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    CLIENT_ID32 *id32 = get_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = 0;
    CLIENT_ID id;
    NTSTATUS status;

    put_handle( handle_ptr, 0 );
    status = NtOpenThread( &handle, access, objattr_32to64( &attr, attr32 ), client_id_32to64( &id, id32 ));
    return publish_handle_result( handle_ptr, handle, status );
}


/**********************************************************************
 *           wow64_NtQueryInformationProcess
 */
NTSTATUS WINAPI wow64_NtQueryInformationProcess( UINT *args )
{
    HANDLE handle = get_handle( &args );
    PROCESSINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;

    switch (class)
    {
    case ProcessBasicInformation:  /* PROCESS_BASIC_INFORMATION */
        if (len == sizeof(PROCESS_BASIC_INFORMATION32))
        {
            PROCESS_BASIC_INFORMATION info;
            PROCESS_BASIC_INFORMATION32 info32;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                if (is_process_wow64( handle ))
                    info32.PebBaseAddress = wow64_guest_memory_addr( info.PebBaseAddress ) + 0x1000;
                else
                    info32.PebBaseAddress = 0;
                info32.ExitStatus = info.ExitStatus;
                info32.AffinityMask = info.AffinityMask;
                info32.BasePriority = info.BasePriority;
                info32.UniqueProcessId = info.UniqueProcessId;
                info32.InheritedFromUniqueProcessId = info.InheritedFromUniqueProcessId;
                wow64_write_user( ptr, &info32, sizeof(info32) );
                put_size( retlen, sizeof(info32) );
            }
            return status;
        }
        put_size( retlen, sizeof(PROCESS_BASIC_INFORMATION32) );
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessIoCounters:  /* IO_COUNTERS */
    case ProcessTimes:  /* KERNEL_USER_TIMES */
    case ProcessDefaultHardErrorMode:  /* ULONG */
    case ProcessPriorityClass:  /* PROCESS_PRIORITY_CLASS */
    case ProcessPriorityBoost:  /* ULONG */
    case ProcessHandleCount:  /* ULONG */
    case ProcessSessionInformation:  /* ULONG */
    case ProcessDebugFlags:  /* ULONG */
    case ProcessExecuteFlags:  /* ULONG */
    case ProcessCookie:  /* ULONG */
    case ProcessCycleTime:  /* PROCESS_CYCLE_TIME_INFORMATION */
    case ProcessPowerThrottlingState:  /* PROCESS_POWER_THROTTLING_STATE */
        return query_process_buffer( handle, class, ptr, len, retlen );

    case ProcessHandleTable:  /* ULONG[] */
        return query_process_handle_table( handle, ptr, len, retlen );

    case ProcessQuotaLimits:  /* QUOTA_LIMITS */
        if (len == sizeof(QUOTA_LIMITS32))
        {
            QUOTA_LIMITS info;
            QUOTA_LIMITS32 info32 = {0};

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                info32.PagedPoolLimit        = info.PagedPoolLimit;
                info32.NonPagedPoolLimit     = info.NonPagedPoolLimit;
                info32.MinimumWorkingSetSize = info.MinimumWorkingSetSize;
                info32.MaximumWorkingSetSize = info.MaximumWorkingSetSize;
                info32.PagefileLimit         = info.PagefileLimit;
                info32.TimeLimit             = info.TimeLimit;
                wow64_write_user( ptr, &info32, sizeof(info32) );
                put_size( retlen, len );
            }
            return status;
        }
        put_size( retlen, sizeof(QUOTA_LIMITS32) );
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessVmCounters:  /* VM_COUNTERS_EX */
        if (len == sizeof(VM_COUNTERS32) || len == sizeof(VM_COUNTERS_EX32))
        {
            VM_COUNTERS_EX info;
            VM_COUNTERS_EX32 info32;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                put_vm_counters( &info32, &info, len );
                wow64_write_user( ptr, &info32, len );
                put_size( retlen, len );
            }
            return status;
        }
        put_size( retlen, sizeof(VM_COUNTERS_EX32) );
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessDebugPort:  /* ULONG_PTR */
    case ProcessAffinityMask:  /* ULONG_PTR */
    case ProcessDebugObjectHandle:  /* HANDLE */
        if (class == ProcessDebugObjectHandle && retlen)
            wow64_probe_user_write( retlen, sizeof(*retlen) );
        if (len == sizeof(ULONG))
        {
            ULONG_PTR data;

            if (!(status = NtQueryInformationProcess( handle, class, &data, sizeof(data), NULL )))
            {
                ULONG data32 = data;
                wow64_write_user( ptr, &data32, sizeof(data32) );
                put_size( retlen, sizeof(data32) );
            }
            else if (status == STATUS_PORT_NOT_SET)
            {
                if (!ptr) return STATUS_ACCESS_VIOLATION;
                put_size( retlen, sizeof(ULONG) );
            }
            return status;
        }
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessWow64Information:  /* ULONG_PTR containing a guest PEB address */
        if (len == sizeof(ULONG))
        {
            ULONG_PTR data;

            if (!(status = NtQueryInformationProcess( handle, class, &data, sizeof(data), NULL )))
            {
                ULONG data32 = wow64_guest_memory_addr( (void *)data );
                wow64_write_user( ptr, &data32, sizeof(data32) );
                put_size( retlen, sizeof(data32) );
            }
            return status;
        }
        return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessImageFileName:
    case ProcessImageFileNameWin32:  /* UNICODE_STRING + string */
        {
            const ULONG delta = sizeof(UNICODE_STRING) - sizeof(UNICODE_STRING32);
            const ULONG untouched = 0xdeadbeef;
            ULONG required = untouched, retsize = untouched, size, out_size;
            UNICODE_STRING32 str32;
            UNICODE_STRING *str;

            if (len > ~(ULONG)0 - delta) return STATUS_INFO_LENGTH_MISMATCH;
            size = len + delta;

            status = NtQueryInformationProcess( handle, class, NULL, 0, &required );
            if (status != STATUS_INFO_LENGTH_MISMATCH || required == untouched || required > size)
            {
                if (retlen && required != untouched)
                {
                    out_size = required >= delta ? required - delta : required;
                    put_size( retlen, out_size );
                }
                return status;
            }
            if (required < sizeof(*str) + sizeof(WCHAR)) return STATUS_INFO_LENGTH_MISMATCH;
            str = alloc_temp( required );
            memset( str, 0, required );

            status = NtQueryInformationProcess( handle, class, str, required, &retsize );
            if (!status)
            {
                ULONG string_size;

                if (len < sizeof(str32) || retsize > required ||
                    str->Length > str->MaximumLength || str->Buffer != (WCHAR *)(str + 1) ||
                    str->MaximumLength > required - sizeof(*str))
                    return STATUS_INFO_LENGTH_MISMATCH;
                str32.Length = str->Length;
                str32.MaximumLength = str->MaximumLength;
                str32.Buffer = wow64_guest_memory_addr( (char *)ptr + sizeof(str32) );
                string_size = min( len - sizeof(str32), str->MaximumLength );
                wow64_write_user( ptr, &str32, sizeof(str32) );
                if (string_size)
                    wow64_write_user( (char *)ptr + sizeof(str32), str->Buffer, string_size );
            }
            if (retlen && retsize != untouched)
            {
                out_size = retsize >= delta ? retsize - delta : retsize;
                put_size( retlen, out_size );
            }
            return status;
        }

    case ProcessImageInformation:  /* SECTION_IMAGE_INFORMATION */
        if (len == sizeof(SECTION_IMAGE_INFORMATION32))
        {
            SECTION_IMAGE_INFORMATION info;
            SECTION_IMAGE_INFORMATION32 info32;

            if (!(status = NtQueryInformationProcess( handle, class, &info, sizeof(info), NULL )))
            {
                put_section_image_info( &info32, &info );
                wow64_write_user( ptr, &info32, sizeof(info32) );
                put_size( retlen, sizeof(info32) );
            }
            return status;
        }
        put_size( retlen, sizeof(SECTION_IMAGE_INFORMATION32) );
        return STATUS_INFO_LENGTH_MISMATCH;

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtQueryInformationThread
 */
NTSTATUS WINAPI wow64_NtQueryInformationThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    THREADINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;

    switch (class)
    {
    case ThreadBasicInformation:  /* THREAD_BASIC_INFORMATION */
    {
        THREAD_BASIC_INFORMATION32 info32;
        THREAD_BASIC_INFORMATION info;

        status = NtQueryInformationThread( handle, class, &info, sizeof(info), NULL );
        if (!status)
        {
            ULONG size = min( len, sizeof(info32) );

            info32.ExitStatus = info.ExitStatus;
            info32.TebBaseAddress = is_process_id_wow64( &info.ClientId ) && info.TebBaseAddress ?
                                    wow64_guest_memory_addr(info.TebBaseAddress) + 0x2000 : 0;
            info32.ClientId.UniqueProcess = HandleToULong( info.ClientId.UniqueProcess );
            info32.ClientId.UniqueThread = HandleToULong( info.ClientId.UniqueThread );
            info32.AffinityMask = info.AffinityMask;
            info32.Priority = info.Priority;
            info32.BasePriority = info.BasePriority;
            wow64_write_user( ptr, &info32, size );
            put_size( retlen, size );
        }
        return status;
    }

    case ThreadTimes:  /* KERNEL_USER_TIMES */
    case ThreadEnableAlignmentFaultFixup:  /* set only */
    case ThreadAmILastThread:  /* ULONG */
    case ThreadIsIoPending:  /* ULONG */
    case ThreadIsTerminated: /* ULONG */
    case ThreadHideFromDebugger:  /* BOOLEAN */
    case ThreadSuspendCount:  /* ULONG */
    case ThreadPriorityBoost:   /* ULONG */
    case ThreadIdealProcessorEx: /* PROCESSOR_NUMBER */
    case ThreadCycleTime:  /* PROCESS_CYCLE_TIME_INFORMATION */
    case ThreadPagePriority:  /* MEMORY_PRIORITY_INFORMATION */
    case ThreadPowerThrottlingState:  /* THREAD_POWER_THROTTLING_STATE */
        return query_thread_buffer( handle, class, ptr, len, retlen );

    case ThreadAffinityMask:  /* ULONG_PTR */
    case ThreadQuerySetWin32StartAddress:  /* PRTL_THREAD_START_ROUTINE */
    {
        ULONG_PTR data;

        status = NtQueryInformationThread( handle, class, &data, sizeof(data), NULL );
        if (!status)
        {
            ULONG data32 = data;
            ULONG size = min( len, sizeof(data32) );

            wow64_write_user( ptr, &data32, size );
            put_size( retlen, size );
        }
        return status;
    }

    case ThreadDescriptorTableEntry:  /* THREAD_DESCRIPTOR_INFORMATION */
    {
        const ULONG untouched = 0xdeadbeef;
        THREAD_DESCRIPTOR_INFORMATION buffer;
        ULONG result = untouched;

        if (len == sizeof(buffer)) wow64_read_user( &buffer, ptr, sizeof(buffer) );
        status = RtlWow64GetThreadSelectorEntry( handle, len == sizeof(buffer) ? &buffer : NULL,
                                                 len, retlen ? &result : NULL );
        if (!status) wow64_write_user( ptr, &buffer, sizeof(buffer) );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    case ThreadWow64Context:  /* WOW64_CONTEXT* */
        return STATUS_INVALID_INFO_CLASS;

    case ThreadGroupInformation:  /* GROUP_AFFINITY */
    {
        GROUP_AFFINITY info;

        status = NtQueryInformationThread( handle, class, &info, sizeof(info), NULL );
        if (!status)
        {
            GROUP_AFFINITY32 info32 = { info.Mask, info.Group };
            ULONG size = min( len, sizeof(info32) );

            wow64_write_user( ptr, &info32, size );
            put_size( retlen, size );
        }
        return status;
    }

    case ThreadNameInformation:  /* THREAD_NAME_INFORMATION */
    {
        THREAD_NAME_INFORMATION *info;
        THREAD_NAME_INFORMATION32 info32;
        const ULONG delta = sizeof(*info) - sizeof(info32);
        ULONG capacity, size, ret_size = 0, out_size;

        if (len >= sizeof(info32))
        {
            if (len > MAXDWORD - delta) return STATUS_INFO_LENGTH_MISMATCH;
            size = len + delta;
            status = NtQueryInformationThread( handle, class, NULL, 0, &ret_size );
            capacity = min( size, max( ret_size, (ULONG)sizeof(*info) ) );
            info = alloc_temp( capacity );
            status = NtQueryInformationThread( handle, class, info, capacity, &ret_size );
            if (status == STATUS_BUFFER_TOO_SMALL && ret_size > capacity && ret_size <= size)
            {
                capacity = ret_size;
                info = alloc_temp( capacity );
                status = NtQueryInformationThread( handle, class, info, capacity, &ret_size );
            }
            if (!status)
            {
                ULONG string_size;

                if (ret_size > capacity || info->ThreadName.Length > info->ThreadName.MaximumLength ||
                    info->ThreadName.Buffer != (WCHAR *)(info + 1) ||
                    info->ThreadName.MaximumLength > capacity - sizeof(*info))
                    return STATUS_INFO_LENGTH_MISMATCH;
                info32.ThreadName.Length = info->ThreadName.Length;
                info32.ThreadName.MaximumLength = info->ThreadName.MaximumLength;
                info32.ThreadName.Buffer = wow64_guest_memory_addr( (char *)ptr + sizeof(info32) );
                string_size = min( len - sizeof(info32), info->ThreadName.MaximumLength );
                wow64_write_user( ptr, &info32, sizeof(info32) );
                if (string_size)
                    wow64_write_user( (char *)ptr + sizeof(info32), info + 1, string_size );
            }
        }
        else status = NtQueryInformationThread( handle, class, NULL, 0, &ret_size );

        if (retlen && (status == STATUS_SUCCESS || status == STATUS_BUFFER_TOO_SMALL))
        {
            out_size = ret_size >= sizeof(*info) ?
                       sizeof(THREAD_NAME_INFORMATION32) + ret_size - sizeof(*info) : ret_size;
            put_size( retlen, out_size );
        }
        return status;
    }

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtQueueApcThread
 */
NTSTATUS WINAPI wow64_NtQueueApcThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG func = get_ulong( &args );
    ULONG arg1 = get_ulong( &args );
    ULONG arg2 = get_ulong( &args );
    ULONG arg3 = get_ulong( &args );

    return NtQueueApcThread( handle, apc_32to64( func ),
                             (ULONG_PTR)apc_param_32to64( func, arg1 ), arg2, arg3 );
}


/**********************************************************************
 *           wow64_NtQueueApcThreadEx
 */
NTSTATUS WINAPI wow64_NtQueueApcThreadEx( UINT *args )
{
    HANDLE handle = get_handle( &args );
    HANDLE reserve_handle = get_handle( &args );
    ULONG func = get_ulong( &args );
    ULONG arg1 = get_ulong( &args );
    ULONG arg2 = get_ulong( &args );
    ULONG arg3 = get_ulong( &args );

    return NtQueueApcThreadEx2( handle, reserve_handle, 0, apc_32to64( func ),
                                (ULONG_PTR)apc_param_32to64( func, arg1 ), arg2, arg3 );
}


/**********************************************************************
 *           wow64_NtQueueApcThreadEx
 */
NTSTATUS WINAPI wow64_NtQueueApcThreadEx2( UINT *args )
{
    HANDLE handle = get_handle( &args );
    HANDLE reserve_handle = get_handle( &args );
    ULONG flags = get_ulong( &args );
    ULONG func = get_ulong( &args );
    ULONG arg1 = get_ulong( &args );
    ULONG arg2 = get_ulong( &args );
    ULONG arg3 = get_ulong( &args );

    return NtQueueApcThreadEx2( handle, reserve_handle, flags, apc_32to64( func ),
                                (ULONG_PTR)apc_param_32to64( func, arg1 ), arg2, arg3 );
}


/**********************************************************************
 *           wow64_NtRemoveProcessDebug
 */
NTSTATUS WINAPI wow64_NtRemoveProcessDebug( UINT *args )
{
    HANDLE process = get_handle( &args );
    HANDLE debug = get_handle( &args );

    return NtRemoveProcessDebug( process, debug );
}


/**********************************************************************
 *           wow64_NtResumeProcess
 */
NTSTATUS WINAPI wow64_NtResumeProcess( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtResumeProcess( handle );
}


/**********************************************************************
 *           wow64_NtResumeThread
 */
NTSTATUS WINAPI wow64_NtResumeThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG *count = get_ptr( &args );
    ULONG local;
    NTSTATUS status;

    if (count) wow64_probe_user_write( count, sizeof(*count) );
    status = NtResumeThread( handle, count ? &local : NULL );
    if (!status && count) wow64_write_user( count, &local, sizeof(local) );
    return status;
}


/**********************************************************************
 *           wow64_NtSetInformationProcess
 */
NTSTATUS WINAPI wow64_NtSetInformationProcess( UINT *args )
{
    HANDLE handle = get_handle( &args );
    PROCESSINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );

    NTSTATUS status;

    switch (class)
    {
    case ProcessDefaultHardErrorMode:   /* ULONG */
    case ProcessBasePriority:   /* ULONG */
    case ProcessPriorityBoost:  /* ULONG */
    {
        ULONG value;

        if (len != sizeof(value)) return NtSetInformationProcess( handle, class, NULL, len );
        wow64_read_user( &value, ptr, sizeof(value) );
        return NtSetInformationProcess( handle, class, &value, sizeof(value) );
    }

    case ProcessPriorityClass:   /* PROCESS_PRIORITY_CLASS */
    {
        PROCESS_PRIORITY_CLASS priority;

        if (len != sizeof(priority)) return NtSetInformationProcess( handle, class, NULL, len );
        wow64_read_user( &priority, ptr, sizeof(priority) );
        return NtSetInformationProcess( handle, class, &priority, sizeof(priority) );
    }

    case ProcessPowerThrottlingState:   /* PROCESS_POWER_THROTTLING_STATE */
    {
        PROCESS_POWER_THROTTLING_STATE state;

        if (len != sizeof(state)) return NtSetInformationProcess( handle, class, NULL, len );
        wow64_read_user( &state, ptr, sizeof(state) );
        return NtSetInformationProcess( handle, class, &state, sizeof(state) );
    }

    case ProcessPagePriority:   /* MEMORY_PRIORITY_INFORMATION */
    case ProcessLeapSecondInformation:   /* PROCESS_LEAP_SECOND_INFO */
        /* These classes are currently rejected without inspecting their buffer. */
        return NtSetInformationProcess( handle, class, NULL, len );

    case ProcessWineGrantAdminToken:   /* NULL */
        return NtSetInformationProcess( handle, class, NULL, len );

    case ProcessExecuteFlags:   /* ULONG */
    {
        ULONG flags;

        if (len != sizeof(flags)) return NtSetInformationProcess( handle, class, NULL, len );
        wow64_read_user( &flags, ptr, sizeof(flags) );
        status = NtSetInformationProcess( handle, class, &flags, sizeof(flags) );
        if (!status && pBTCpuNotifyProcessExecuteFlagsChange)
            pBTCpuNotifyProcessExecuteFlagsChange( flags );
        return status;
    }

    case ProcessAccessToken: /* PROCESS_ACCESS_TOKEN */
        if (len == sizeof(PROCESS_ACCESS_TOKEN32))
        {
            PROCESS_ACCESS_TOKEN32 stack;
            PROCESS_ACCESS_TOKEN info;

            wow64_read_user( &stack, ptr, sizeof(stack) );
            info.Thread = ULongToHandle( stack.Thread );
            info.Token = ULongToHandle( stack.Token );
            return NtSetInformationProcess( handle, class, &info, sizeof(info) );
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessAffinityMask:   /* ULONG_PTR */
        if (len == sizeof(ULONG))
        {
            ULONG mask32;
            ULONG_PTR mask;

            wow64_read_user( &mask32, ptr, sizeof(mask32) );
            mask = mask32;
            return NtSetInformationProcess( handle, class, &mask, sizeof(mask) );
        }
        else return STATUS_INVALID_PARAMETER;

    case ProcessInstrumentationCallback:   /* PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION */
        if (len >= sizeof(ULONG))
        {
            FIXME( "ProcessInstrumentationCallback stub\n" );
            return STATUS_SUCCESS;
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessThreadStackAllocation:   /* PROCESS_STACK_ALLOCATION_INFORMATION(_EX) */
        if (len == sizeof(PROCESS_STACK_ALLOCATION_INFORMATION_EX32))
        {
            PROCESS_STACK_ALLOCATION_INFORMATION_EX32 stack;
            PROCESS_STACK_ALLOCATION_INFORMATION_EX info;

            wow64_read_user( &stack, ptr, sizeof(stack) );
            wow64_probe_user_write( (char *)ptr + offsetof(PROCESS_STACK_ALLOCATION_INFORMATION_EX32,
                                                            AllocInfo.StackBase), sizeof(ULONG) );
            info.PreferredNode = stack.PreferredNode;
            info.Reserved0 = stack.Reserved0;
            info.Reserved1 = stack.Reserved1;
            info.Reserved2 = stack.Reserved2;
            info.AllocInfo.ReserveSize = stack.AllocInfo.ReserveSize;
            info.AllocInfo.ZeroBits = get_zero_bits( stack.AllocInfo.ZeroBits );
            if (!(status = NtSetInformationProcess( handle, class, &info, sizeof(info) )))
            {
                ULONG base = wow64_guest_memory_addr( info.AllocInfo.StackBase );
                NTSTATUS publish_status = wow64_try_write_user(
                    (char *)ptr + offsetof(PROCESS_STACK_ALLOCATION_INFORMATION_EX32,
                                           AllocInfo.StackBase), &base, sizeof(base) );

                if (publish_status)
                {
                    SIZE_T free_size = 0;
                    void *free_base = info.AllocInfo.StackBase;

                    NtFreeVirtualMemory( NtCurrentProcess(), &free_base, &free_size, MEM_RELEASE );
                    return publish_status;
                }
            }
            return status;
        }
        else if (len == sizeof(PROCESS_STACK_ALLOCATION_INFORMATION32))
        {
            PROCESS_STACK_ALLOCATION_INFORMATION32 stack;
            PROCESS_STACK_ALLOCATION_INFORMATION info;

            wow64_read_user( &stack, ptr, sizeof(stack) );
            wow64_probe_user_write( (char *)ptr + offsetof(PROCESS_STACK_ALLOCATION_INFORMATION32,
                                                            StackBase), sizeof(ULONG) );
            info.ReserveSize = stack.ReserveSize;
            info.ZeroBits = get_zero_bits( stack.ZeroBits );
            if (!(status = NtSetInformationProcess( handle, class, &info, sizeof(info) )))
            {
                ULONG base = wow64_guest_memory_addr( info.StackBase );
                NTSTATUS publish_status = wow64_try_write_user(
                    (char *)ptr + offsetof(PROCESS_STACK_ALLOCATION_INFORMATION32, StackBase),
                    &base, sizeof(base) );

                if (publish_status)
                {
                    SIZE_T free_size = 0;
                    void *free_base = info.StackBase;

                    NtFreeVirtualMemory( NtCurrentProcess(), &free_base, &free_size, MEM_RELEASE );
                    return publish_status;
                }
            }
            return status;
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    case ProcessManageWritesToExecutableMemory: /* MANAGE_WRITES_TO_EXECUTABLE_MEMORY */
        return STATUS_INVALID_INFO_CLASS;

    case ProcessWineMakeProcessSystem:   /* HANDLE* */
        if (len == sizeof(ULONG))
        {
            HANDLE event = 0;

            wow64_probe_user_write( ptr, sizeof(ULONG) );
            status = NtSetInformationProcess( handle, class, &event, sizeof(HANDLE *) );
            return publish_handle_result( ptr, event, status );
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtSetInformationThread
 */
NTSTATUS WINAPI wow64_NtSetInformationThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    THREADINFOCLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );

    switch (class)
    {
    case ThreadPriority:   /* ULONG */
    case ThreadBasePriority:   /* ULONG */
    case ThreadIdealProcessor:   /* ULONG */
    case ThreadPriorityBoost:   /* ULONG */
    {
        ULONG value;

        if (len != sizeof(value)) return NtSetInformationThread( handle, class, NULL, len );
        wow64_read_user( &value, ptr, sizeof(value) );
        return NtSetInformationThread( handle, class, &value, sizeof(value) );
    }

    case ThreadZeroTlsCell:   /* ULONG */
    {
        ULONG value;

        if (handle != GetCurrentThread()) return NtSetInformationThread( handle, class, NULL, len );
        if (len != sizeof(value)) return NtSetInformationThread( handle, class, NULL, len );
        wow64_read_user( &value, ptr, sizeof(value) );
        return NtSetInformationThread( handle, class, &value, sizeof(value) );
    }

    case ThreadHideFromDebugger:   /* void */
        return NtSetInformationThread( handle, class, NULL, len );

    case ThreadEnableAlignmentFaultFixup:   /* BOOLEAN */
    {
        BOOLEAN enable;

        if (len != sizeof(enable)) return NtSetInformationThread( handle, class, NULL, len );
        wow64_read_user( &enable, ptr, sizeof(enable) );
        return NtSetInformationThread( handle, class, &enable, sizeof(enable) );
    }

    case ThreadPagePriority:  /* MEMORY_PRIORITY_INFORMATION */
    {
        MEMORY_PRIORITY_INFORMATION info;

        if (len != sizeof(info)) return NtSetInformationThread( handle, class, NULL, len );
        wow64_read_user( &info, ptr, sizeof(info) );
        return NtSetInformationThread( handle, class, &info, sizeof(info) );
    }

    case ThreadPowerThrottlingState:  /* THREAD_POWER_THROTTLING_STATE */
    {
        THREAD_POWER_THROTTLING_STATE info;

        if (len != sizeof(info)) return NtSetInformationThread( handle, class, NULL, len );
        wow64_read_user( &info, ptr, sizeof(info) );
        return NtSetInformationThread( handle, class, &info, sizeof(info) );
    }

    case ThreadImpersonationToken:   /* HANDLE */
        if (len == sizeof(ULONG))
        {
            ULONG token32;
            HANDLE token;

            wow64_read_user( &token32, ptr, sizeof(token32) );
            token = LongToHandle( token32 );
            return NtSetInformationThread( handle, class, &token, sizeof(token) );
        }
        else return STATUS_INVALID_PARAMETER;

    case ThreadAffinityMask:  /* ULONG_PTR */
    case ThreadQuerySetWin32StartAddress:   /* PRTL_THREAD_START_ROUTINE */
        if (len == sizeof(ULONG))
        {
            ULONG value32;
            ULONG_PTR mask;

            wow64_read_user( &value32, ptr, sizeof(value32) );
            mask = value32;
            return NtSetInformationThread( handle, class, &mask, sizeof(mask) );
        }
        else return STATUS_INVALID_PARAMETER;

    case ThreadWow64Context:  /* WOW64_CONTEXT* */
    case ThreadManageWritesToExecutableMemory: /* MANAGE_WRITES_TO_EXECUTABLE_MEMORY */
        return STATUS_INVALID_INFO_CLASS;

    case ThreadGroupInformation:   /* GROUP_AFFINITY */
        if (len == sizeof(GROUP_AFFINITY32))
        {
            GROUP_AFFINITY32 info32;
            GROUP_AFFINITY info;

            wow64_read_user( &info32, ptr, sizeof(info32) );
            memset( &info, 0, sizeof(info) );
            info.Mask = info32.Mask;
            info.Group = info32.Group;

            return NtSetInformationThread( handle, class, &info, sizeof(info) );
        }
        else return STATUS_INVALID_PARAMETER;

    case ThreadNameInformation:   /* THREAD_NAME_INFORMATION */
    case ThreadWineNativeThreadName:
        if (len == sizeof(THREAD_NAME_INFORMATION32))
        {
            THREAD_NAME_INFORMATION32 info32;
            THREAD_NAME_INFORMATION info;
            NTSTATUS status;

            wow64_read_user( &info32, ptr, sizeof(info32) );
            if ((status = snapshot_unicode_string( &info.ThreadName, &info32.ThreadName )))
                return status;
            return NtSetInformationThread( handle, class, &info, sizeof(info) );
        }
        else return STATUS_INFO_LENGTH_MISMATCH;

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtSetThreadExecutionState
 */
NTSTATUS WINAPI wow64_NtSetThreadExecutionState( UINT *args )
{
    EXECUTION_STATE new_state = get_ulong( &args );
    EXECUTION_STATE *old_state = get_ptr( &args );

    EXECUTION_STATE local;
    NTSTATUS status;

    if (old_state) wow64_probe_user_write( old_state, sizeof(*old_state) );
    status = NtSetThreadExecutionState( new_state, old_state ? &local : NULL );
    if (!status && old_state) wow64_write_user( old_state, &local, sizeof(local) );
    return status;
}


/**********************************************************************
 *           wow64_NtSuspendProcess
 */
NTSTATUS WINAPI wow64_NtSuspendProcess( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtSuspendProcess( handle );
}


/**********************************************************************
 *           wow64_NtSuspendThread
 */
NTSTATUS WINAPI wow64_NtSuspendThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG *count = get_ptr( &args );

    ULONG local;
    NTSTATUS status;

    if (count) wow64_probe_user_write( count, sizeof(*count) );
    status = NtSuspendThread( handle, count ? &local : NULL );
    if (!status && count) wow64_write_user( count, &local, sizeof(local) );
    return status;
}


/**********************************************************************
 *           wow64_NtTerminateProcess
 */
NTSTATUS WINAPI wow64_NtTerminateProcess( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LONG exit_code = get_ulong( &args );

    NTSTATUS status;

    if (!handle && pBTCpuProcessTerm) pBTCpuProcessTerm( handle, FALSE, 0 );
    status = NtTerminateProcess( handle, exit_code );
    if (!handle && pBTCpuProcessTerm) pBTCpuProcessTerm( handle, TRUE, status );
    return status;
}


/**********************************************************************
 *           wow64_NtTerminateThread
 */
NTSTATUS WINAPI wow64_NtTerminateThread( UINT *args )
{
    HANDLE handle = get_handle( &args );
    LONG exit_code = get_ulong( &args );

    if (pBTCpuThreadTerm) pBTCpuThreadTerm( handle, exit_code );

    return NtTerminateThread( handle, exit_code );
}


/**********************************************************************
 *           wow64_NtTerminateThread
 */
NTSTATUS WINAPI wow64_NtWorkerFactoryWorkerReady( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtWorkerFactoryWorkerReady( handle );
}


/**********************************************************************
 *           wow64_NtWow64QueryInformationProcess64
 */
NTSTATUS WINAPI wow64_NtWow64QueryInformationProcess64( UINT *args )
{
    HANDLE handle = get_handle( &args );
    PROCESSINFOCLASS class = get_ulong( &args );
    void *info = get_ptr( &args );
    ULONG size = get_ulong( &args );
    ULONG *ret_len = get_ptr( &args );
    PROCESS_BASIC_INFORMATION native_info;
    ULONG result = 0;
    NTSTATUS status;

    switch (class)
    {
    case ProcessBasicInformation:
        if (size < sizeof(native_info))
        {
            status = NtQueryInformationProcess( handle, class, NULL, size, &result );
        }
        else
        {
            status = NtQueryInformationProcess( handle, class, &native_info,
                                                sizeof(native_info), &result );
            if (!status) wow64_write_user( info, &native_info, sizeof(native_info) );
            if (size > sizeof(native_info)) status = STATUS_INFO_LENGTH_MISMATCH;
        }
        if (ret_len) put_size( ret_len, result );
        return status;
    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}
