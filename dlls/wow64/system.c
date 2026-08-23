/*
 * WoW64 system functions
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


static void *system_alloc_temp( SIZE_T size )
{
    void *ret;

    if (!size) return NULL;
    if (!(ret = Wow64AllocateTemp( size ))) RtlRaiseStatus( STATUS_NO_MEMORY );
    return ret;
}


static BOOL system_multiply_size( SIZE_T left, SIZE_T right, SIZE_T *result )
{
    if (right && left > ~(SIZE_T)0 / right) return FALSE;
    *result = left * right;
    return TRUE;
}


static NTSTATUS snapshot_system_unicode_string( UNICODE_STRING *str,
                                                const UNICODE_STRING32 *str32 )
{
    UNICODE_STRING32 local;
    void *buffer = NULL;

    if (!str32) return STATUS_ACCESS_VIOLATION;
    wow64_read_user( &local, str32, sizeof(local) );
    if (local.Length > local.MaximumLength || (local.Length & (sizeof(WCHAR) - 1)))
        return STATUS_INVALID_PARAMETER;
    if (local.Length)
    {
        if (!local.Buffer) return STATUS_ACCESS_VIOLATION;
        buffer = system_alloc_temp( local.Length );
        wow64_read_user( buffer, wow64_guest_memory_ptr( local.Buffer ), local.Length );
    }
    str->Length = local.Length;
    str->MaximumLength = local.MaximumLength;
    str->Buffer = buffer;
    return STATUS_SUCCESS;
}


static NTSTATUS query_system_buffer( SYSTEM_INFORMATION_CLASS class, void *ptr,
                                     ULONG len, ULONG *retlen )
{
    const ULONG untouched = 0xdeadbeef;
    ULONG result = untouched, copy_size = 0, capacity, native_len;
    void *buffer;
    NTSTATUS status;

    if (class == SystemProcessorBrandString && ((ULONG_PTR)ptr & 3))
    {
        ULONG aligned[2];

        status = NtQuerySystemInformation( class, (char *)aligned + 1, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    if (!ptr)
    {
        status = NtQuerySystemInformation( class, NULL, len, &result );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    switch (class)
    {
    case SystemCpuInformation:
    case SystemEmulationProcessorInformation:
        capacity = sizeof(SYSTEM_CPU_INFORMATION);
        break;
    case SystemNativeBasicInformation:
        capacity = len == sizeof(SYSTEM_BASIC_INFORMATION) ? len : 0;
        break;
    case SystemPerformanceInformation:
        capacity = sizeof(SYSTEM_PERFORMANCE_INFORMATION);
        break;
    case SystemTimeOfDayInformation:
        capacity = sizeof(SYSTEM_TIMEOFDAY_INFORMATION);
        if (len > capacity) capacity = 0;  /* native rejects oversized buffers */
        break;
    case SystemProcessorPerformanceInformation:
        capacity = NtCurrentTeb()->Peb->NumberOfProcessors *
                   sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
        break;
    case SystemInterruptInformation:
        capacity = NtCurrentTeb()->Peb->NumberOfProcessors *
                   sizeof(SYSTEM_INTERRUPT_INFORMATION);
        break;
    case SystemTimeAdjustmentInformation:
        capacity = len == sizeof(SYSTEM_TIME_ADJUSTMENT_QUERY) ? len : 0;
        break;
    case SystemKernelDebuggerInformation:
        capacity = sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION);
        break;
    case SystemCurrentTimeZoneInformation:
        capacity = sizeof(RTL_TIME_ZONE_INFORMATION);
        break;
    case SystemRecommendedSharedDataAlignment:
        capacity = sizeof(ULONG);
        break;
    case SystemProcessorIdleCycleTimeInformation:
        capacity = NtCurrentTeb()->Peb->NumberOfProcessors * sizeof(ULONG64);
        break;
    case SystemDynamicTimeZoneInformation:
        capacity = sizeof(RTL_DYNAMIC_TIME_ZONE_INFORMATION);
        break;
    case SystemKernelDebuggerInformationEx:
        capacity = sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION_EX);
        break;
    case SystemCpuSetInformation:
        capacity = 0;  /* native validates the missing query before its output */
        break;
    case SystemSecureBootInformation:
        capacity = 2 * sizeof(BOOLEAN);
        break;
    case SystemLeapSecondInformation:
        capacity = sizeof(SYSTEM_LEAP_SECOND_INFORMATION);
        break;
    case SystemProcessorBrandString:
        capacity = 49;  /* sizeof cpu_name in the native implementation */
        break;
    case SystemProcessorFeaturesInformation:
        capacity = sizeof(SYSTEM_PROCESSOR_FEATURES_INFORMATION);
        break;
    case SystemWineVersionInformation:
        status = NtQuerySystemInformation( class, NULL, 0, &result );
        if (status != STATUS_INFO_LENGTH_MISMATCH)
        {
            if (retlen && result != untouched) put_size( retlen, result );
            return status;
        }
        capacity = result;
        break;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }

    capacity = min( len, capacity );
    native_len = capacity;
    if (!capacity && len && (class == SystemTimeOfDayInformation ||
                             class == SystemTimeAdjustmentInformation ||
                             class == SystemCpuSetInformation ||
                             class == SystemNativeBasicInformation))
        native_len = len;
    buffer = system_alloc_temp( capacity );
    if (buffer) memset( buffer, 0, capacity );
    result = untouched;
    status = NtQuerySystemInformation( class, buffer, native_len, &result );
    if (!status) copy_size = result == untouched ? capacity : min( capacity, result );
    else if (status == STATUS_INFO_LENGTH_MISMATCH && class == SystemWineVersionInformation)
        copy_size = capacity;
    if (copy_size)
    {
        if (ptr) wow64_write_user( ptr, buffer, copy_size );
        else status = STATUS_ACCESS_VIOLATION;
    }
    if (retlen && result != untouched) put_size( retlen, result );
    return status;
}


static void put_system_basic_information( SYSTEM_BASIC_INFORMATION32 *info32,
                                          const SYSTEM_BASIC_INFORMATION *info )
{
    memset( info32, 0, sizeof(*info32) );
    info32->unknown                      = info->unknown;
    info32->KeMaximumIncrement           = info->KeMaximumIncrement;
    info32->PageSize                     = info->PageSize;
    info32->MmNumberOfPhysicalPages      = info->MmNumberOfPhysicalPages;
    info32->MmLowestPhysicalPage         = info->MmLowestPhysicalPage;
    info32->MmHighestPhysicalPage        = info->MmHighestPhysicalPage;
    info32->AllocationGranularity        = info->AllocationGranularity;
    info32->LowestUserAddress            = wow64_guest_memory_addr( info->LowestUserAddress );
    info32->HighestUserAddress           = wow64_guest_memory_addr( info->HighestUserAddress );
    info32->ActiveProcessorsAffinityMask = info->ActiveProcessorsAffinityMask;
    info32->NumberOfProcessors           = info->NumberOfProcessors;
}


static void put_group_affinity( GROUP_AFFINITY32 *info32, const GROUP_AFFINITY *info )
{
    info32->Mask = info->Mask;
    info32->Group = info->Group;
}


static NTSTATUS get_logical_proc_info_ex32_size(
    const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info, ULONG available, ULONG *size )
{
    const ULONG header = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
    SIZE_T variable_size;

    *size = 0;
    if (available < header || info->Size < header || info->Size > available)
        return STATUS_INFO_LENGTH_MISMATCH;

    switch (info->Relationship)
    {
    case RelationProcessorCore:
    case RelationProcessorPackage:
        if (info->Size < offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                  Processor.GroupMask) ||
            !system_multiply_size( info->Processor.GroupCount, sizeof(GROUP_AFFINITY),
                                   &variable_size ) ||
            variable_size > info->Size - offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                                   Processor.GroupMask) ||
            !system_multiply_size( info->Processor.GroupCount, sizeof(GROUP_AFFINITY32),
                                   &variable_size ) ||
            variable_size > MAXDWORD - offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32,
                                                 Processor.GroupMask))
            return STATUS_INFO_LENGTH_MISMATCH;
        *size = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32,
                         Processor.GroupMask) + variable_size;
        break;
    case RelationNumaNode:
        if (info->Size < offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode) +
                         sizeof(info->NumaNode)) return STATUS_INFO_LENGTH_MISMATCH;
        *size = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32, NumaNode) +
                sizeof(((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32 *)0)->NumaNode);
        break;
    case RelationCache:
        if (info->Size < offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) +
                         sizeof(info->Cache)) return STATUS_INFO_LENGTH_MISMATCH;
        *size = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32, Cache) +
                sizeof(((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32 *)0)->Cache);
        break;
    case RelationGroup:
        if (info->Size < offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                  Group.GroupInfo) ||
            !system_multiply_size( info->Group.MaximumGroupCount,
                                   sizeof(PROCESSOR_GROUP_INFO), &variable_size ) ||
            variable_size > info->Size - offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                                   Group.GroupInfo) ||
            !system_multiply_size( info->Group.MaximumGroupCount,
                                   sizeof(PROCESSOR_GROUP_INFO32), &variable_size ) ||
            variable_size > MAXDWORD - offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32,
                                                 Group.GroupInfo))
            return STATUS_INFO_LENGTH_MISMATCH;
        *size = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32,
                         Group.GroupInfo) + variable_size;
        break;
    default:
        break;
    }
    return STATUS_SUCCESS;
}


static void put_logical_proc_info_ex( SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32 *info32,
                                      const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info )
{
    ULONG i;

    info32->Relationship = info->Relationship;
    info32->Size = offsetof( SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32, Processor );
    switch (info->Relationship)
    {
    case RelationProcessorCore:
    case RelationProcessorPackage:
        info32->Processor.Flags           = info->Processor.Flags;
        info32->Processor.EfficiencyClass = info->Processor.EfficiencyClass;
        info32->Processor.GroupCount      = info->Processor.GroupCount;
        for (i = 0; i < info->Processor.GroupCount; i++)
            put_group_affinity( &info32->Processor.GroupMask[i], &info->Processor.GroupMask[i] );
        info32->Size += offsetof( PROCESSOR_RELATIONSHIP32, GroupMask[i] );
        break;
    case RelationNumaNode:
        info32->NumaNode.NodeNumber = info->NumaNode.NodeNumber;
        put_group_affinity( &info32->NumaNode.GroupMask, &info->NumaNode.GroupMask );
        info32->Size += sizeof(info32->NumaNode);
        break;
    case RelationCache:
        info32->Cache.Level         = info->Cache.Level;
        info32->Cache.Associativity = info->Cache.Associativity;
        info32->Cache.LineSize      = info->Cache.LineSize;
        info32->Cache.CacheSize     = info->Cache.CacheSize;
        info32->Cache.Type          = info->Cache.Type;
        put_group_affinity( &info32->Cache.GroupMask, &info->Cache.GroupMask );
        info32->Size += sizeof(info32->Cache);
        break;
    case RelationGroup:
        info32->Group.MaximumGroupCount = info->Group.MaximumGroupCount;
        info32->Group.ActiveGroupCount = info->Group.ActiveGroupCount;
        for (i = 0; i < info->Group.MaximumGroupCount; i++)
        {
            info32->Group.GroupInfo[i].MaximumProcessorCount = info->Group.GroupInfo[i].MaximumProcessorCount;
            info32->Group.GroupInfo[i].ActiveProcessorCount = info->Group.GroupInfo[i].ActiveProcessorCount;
            info32->Group.GroupInfo[i].ActiveProcessorMask = info->Group.GroupInfo[i].ActiveProcessorMask;
        }
        info32->Size += offsetof( GROUP_RELATIONSHIP32, GroupInfo[i] );
        break;
    default:
        break;
    }
}


static NTSTATUS put_system_proc_info( SYSTEM_PROCESS_INFORMATION32 *info32, void *guest_buffer,
                                      const SYSTEM_PROCESS_INFORMATION *info, ULONG info_len,
                                      BOOL ext_info, ULONG len, ULONG *retlen, ULONG *written )
{
    SIZE_T inpos = 0, outpos = 0;
    ULONG i;
    SYSTEM_PROCESS_INFORMATION32 *prev = NULL;
    const SIZE_T ti_size = ext_info ? sizeof(SYSTEM_EXTENDED_THREAD_INFORMATION) :
                                         sizeof(SYSTEM_THREAD_INFORMATION);
    const SIZE_T ti_size32 = ext_info ? sizeof(SYSTEM_EXTENDED_THREAD_INFORMATION32) :
                                           sizeof(SYSTEM_THREAD_INFORMATION32);

    *written = 0;

    for (;;)
    {
        SYSTEM_EXTENDED_THREAD_INFORMATION *ti;
        SYSTEM_EXTENDED_THREAD_INFORMATION32 *ti32;
        SYSTEM_PROCESS_INFORMATION *proc;
        SYSTEM_PROCESS_INFORMATION32 *proc32;
        SIZE_T native_threads_size, threads_size, proc_len, entry_len, next_out;
        ULONG_PTR name, base;

        if (inpos > info_len || info_len - inpos < offsetof(SYSTEM_PROCESS_INFORMATION, ti))
            return STATUS_INFO_LENGTH_MISMATCH;
        proc = (SYSTEM_PROCESS_INFORMATION *)((char *)info + inpos);
        entry_len = proc->NextEntryOffset ? proc->NextEntryOffset : info_len - inpos;
        if (entry_len > info_len - inpos || entry_len < offsetof(SYSTEM_PROCESS_INFORMATION, ti) ||
            !system_multiply_size( proc->dwThreadCount, ti_size, &native_threads_size ) ||
            native_threads_size > entry_len - offsetof(SYSTEM_PROCESS_INFORMATION, ti) ||
            !system_multiply_size( proc->dwThreadCount, ti_size32, &threads_size ) ||
            threads_size > ~(SIZE_T)0 - offsetof(SYSTEM_PROCESS_INFORMATION32, ti))
            return STATUS_INFO_LENGTH_MISMATCH;
        proc_len = offsetof(SYSTEM_PROCESS_INFORMATION32, ti) + threads_size;
        if (proc->ProcessName.MaximumLength > ~(SIZE_T)0 - proc_len ||
            outpos > ~(SIZE_T)0 - proc_len - proc->ProcessName.MaximumLength)
            return STATUS_INFO_LENGTH_MISMATCH;
        next_out = outpos + proc_len + proc->ProcessName.MaximumLength;
        if (next_out > ~(SIZE_T)7) return STATUS_INFO_LENGTH_MISMATCH;
        next_out = (next_out + 7) & ~(SIZE_T)7;
        if (proc->ProcessName.Length > proc->ProcessName.MaximumLength)
            return STATUS_INFO_LENGTH_MISMATCH;
        if (proc->ProcessName.MaximumLength)
        {
            name = (ULONG_PTR)proc->ProcessName.Buffer;
            base = (ULONG_PTR)info;

            if (!name || name < base || name - base > info_len ||
                proc->ProcessName.MaximumLength > info_len - (name - base))
                return STATUS_INFO_LENGTH_MISMATCH;
        }

        if (outpos <= len && proc_len + proc->ProcessName.MaximumLength <= len - outpos)
        {
            proc32 = (SYSTEM_PROCESS_INFORMATION32 *)((char *)info32 + outpos);
            memset( proc32, 0, proc_len );

            proc32->dwThreadCount                = proc->dwThreadCount;
            proc32->WorkingSetPrivateSize        = proc->WorkingSetPrivateSize;
            proc32->HardFaultCount               = proc->HardFaultCount;
            proc32->NumberOfThreadsHighWatermark = proc->NumberOfThreadsHighWatermark;
            proc32->CycleTime                    = proc->CycleTime;
            proc32->CreationTime                 = proc->CreationTime;
            proc32->UserTime                     = proc->UserTime;
            proc32->KernelTime                   = proc->KernelTime;
            proc32->ProcessName.Length           = proc->ProcessName.Length;
            proc32->ProcessName.MaximumLength    = proc->ProcessName.MaximumLength;
            proc32->ProcessName.Buffer           = wow64_guest_memory_addr( (char *)guest_buffer +
                                                                            outpos + proc_len );
            proc32->dwBasePriority               = proc->dwBasePriority;
            proc32->UniqueProcessId              = HandleToULong( proc->UniqueProcessId );
            proc32->ParentProcessId              = HandleToULong( proc->ParentProcessId );
            proc32->HandleCount                  = proc->HandleCount;
            proc32->SessionId                    = proc->SessionId;
            proc32->UniqueProcessKey             = proc->UniqueProcessKey;
            proc32->ioCounters                   = proc->ioCounters;
            put_vm_counters( &proc32->vmCounters, &proc->vmCounters, sizeof(proc32->vmCounters) );
            for (i = 0; i < proc->dwThreadCount; i++)
            {
                ti = (SYSTEM_EXTENDED_THREAD_INFORMATION *)((char *)proc->ti + i * ti_size);
                ti32 = (SYSTEM_EXTENDED_THREAD_INFORMATION32 *)((char *)proc32->ti + i * ti_size32);
                ti32->ThreadInfo.KernelTime        = ti->ThreadInfo.KernelTime;
                ti32->ThreadInfo.UserTime          = ti->ThreadInfo.UserTime;
                ti32->ThreadInfo.CreateTime        = ti->ThreadInfo.CreateTime;
                ti32->ThreadInfo.dwTickCount       = ti->ThreadInfo.dwTickCount;
                ti32->ThreadInfo.StartAddress      = wow64_guest_memory_addr( ti->ThreadInfo.StartAddress );
                ti32->ThreadInfo.dwCurrentPriority = ti->ThreadInfo.dwCurrentPriority;
                ti32->ThreadInfo.dwBasePriority    = ti->ThreadInfo.dwBasePriority;
                ti32->ThreadInfo.dwContextSwitches = ti->ThreadInfo.dwContextSwitches;
                ti32->ThreadInfo.dwThreadState     = ti->ThreadInfo.dwThreadState;
                ti32->ThreadInfo.dwWaitReason      = ti->ThreadInfo.dwWaitReason;
                ti32->ThreadInfo.ClientId.UniqueProcess = HandleToLong( ti->ThreadInfo.ClientId.UniqueProcess );
                ti32->ThreadInfo.ClientId.UniqueThread = HandleToLong( ti->ThreadInfo.ClientId.UniqueThread );
                if (ext_info)
                {
                    ti32->StackBase         = wow64_guest_memory_addr( ti->StackBase );
                    ti32->StackLimit        = wow64_guest_memory_addr( ti->StackLimit );
                    ti32->Win32StartAddress = wow64_guest_memory_addr( ti->Win32StartAddress );
                    ti32->TebBase           = wow64_guest_memory_addr( ti->TebBase );
                    ti32->Reserved2         = ti->Reserved2;
                    ti32->Reserved3         = ti->Reserved3;
                    ti32->Reserved4         = ti->Reserved4;
                }
            }
            if (proc->ProcessName.MaximumLength)
                memcpy( (char *)proc32 + proc_len, proc->ProcessName.Buffer,
                        proc->ProcessName.MaximumLength );

            if (prev) prev->NextEntryOffset = (char *)proc32 - (char *)prev;
            prev = proc32;
            *written = min( next_out, len );
        }
        outpos = next_out;
        if (!proc->NextEntryOffset) break;
        inpos += proc->NextEntryOffset;
    }
    if (retlen) *retlen = min( outpos, MAXDWORD );
    if (outpos <= len) return STATUS_SUCCESS;
    else return STATUS_INFO_LENGTH_MISMATCH;
}


/**********************************************************************
 *           wow64_NtDisplayString
 */
NTSTATUS WINAPI wow64_NtDisplayString( UINT *args )
{
    const UNICODE_STRING32 *str32 = get_ptr( &args );

    UNICODE_STRING str;
    NTSTATUS status;

    if ((status = snapshot_system_unicode_string( &str, str32 ))) return status;
    return NtDisplayString( &str );
}


/**********************************************************************
 *           wow64_NtInitiatePowerAction
 */
NTSTATUS WINAPI wow64_NtInitiatePowerAction( UINT *args )
{
    POWER_ACTION action = get_ulong( &args );
    SYSTEM_POWER_STATE state = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    BOOLEAN async = get_ulong( &args );

    return NtInitiatePowerAction( action, state, flags, async );
}


/**********************************************************************
 *           wow64_NtLoadDriver
 */
NTSTATUS WINAPI wow64_NtLoadDriver( UINT *args )
{
    UNICODE_STRING32 *str32 = get_ptr( &args );

    UNICODE_STRING str;
    NTSTATUS status;

    if ((status = snapshot_system_unicode_string( &str, str32 ))) return status;
    return NtLoadDriver( &str );
}


/**********************************************************************
 *           wow64_NtPowerInformation
 */
NTSTATUS WINAPI wow64_NtPowerInformation( UINT *args )
{
    POWER_INFORMATION_LEVEL level = get_ulong( &args );
    void *in_buf = get_ptr( &args );
    ULONG in_len = get_ulong( &args );
    void *out_buf = get_ptr( &args );
    ULONG out_len = get_ulong( &args );

    ULONG in_size = 0, out_size = 0;
    SIZE_T size;
    void *in = NULL, *out = NULL;
    BOOL preflight_out = FALSE;
    NTSTATUS status;

    switch (level)
    {
    case SystemPowerPolicyAc:   /* SYSTEM_POWER_POLICY */
    case SystemPowerPolicyDc:   /* SYSTEM_POWER_POLICY */
        if ((!in_buf && in_len) || (in_buf && in_len && in_len < sizeof(SYSTEM_POWER_POLICY)) ||
            (!out_buf && !in_len) || (out_buf && !out_len) ||
            (out_buf && out_len < sizeof(SYSTEM_POWER_POLICY)) || (!out_buf && out_len))
        {
            SYSTEM_POWER_POLICY dummy;

            return NtPowerInformation( level, in_buf ? &dummy : NULL, in_len,
                                       out_buf ? &dummy : NULL, out_len );
        }
        if (in_len) in_size = sizeof(SYSTEM_POWER_POLICY);
        if (out_buf) out_size = sizeof(SYSTEM_POWER_POLICY);
        preflight_out = !!(in_size && out_size);
        break;

    case VerifySystemPolicyAc:   /* SYSTEM_POWER_POLICY */
    case VerifySystemPolicyDc:   /* SYSTEM_POWER_POLICY */
        if (!in_buf || !in_len || !out_buf || !out_len ||
            in_len < sizeof(SYSTEM_POWER_POLICY) || out_len < sizeof(SYSTEM_POWER_POLICY))
        {
            SYSTEM_POWER_POLICY dummy;

            return NtPowerInformation( level, in_buf ? &dummy : NULL, in_len,
                                       out_buf ? &dummy : NULL, out_len );
        }
        in_size = out_size = sizeof(SYSTEM_POWER_POLICY);
        break;

    case SystemPowerCapabilities:   /* SYSTEM_POWER_CAPABILITIES */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(SYSTEM_POWER_CAPABILITIES)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(SYSTEM_POWER_CAPABILITIES);
        break;

    case SystemBatteryState:   /* SYSTEM_BATTERY_STATE */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(SYSTEM_BATTERY_STATE)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(SYSTEM_BATTERY_STATE);
        break;

    case SystemPowerStateHandler:
    case ProcessorStateHandler:
    case SystemPowerStateNotifyHandler:
        return NtPowerInformation( level, NULL, in_len, NULL, out_len );

    case SystemPowerPolicyCurrent:   /* SYSTEM_POWER_POLICY */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(SYSTEM_POWER_POLICY)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(SYSTEM_POWER_POLICY);
        break;

    case AdministratorPowerPolicy:   /* ADMINISTRATOR_POWER_POLICY */
        if (in_len) return in_buf ? STATUS_ACCESS_DENIED : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(ADMINISTRATOR_POWER_POLICY)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(ADMINISTRATOR_POWER_POLICY);
        break;

    case SystemReserveHiberFile:   /* BOOLEAN */
        return NtPowerInformation( level, NULL, in_len, NULL, out_len );

    case ProcessorInformation:   /* PROCESSOR_POWER_INFORMATION */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_buf || !out_len) return STATUS_INVALID_PARAMETER;
        if (!system_multiply_size( NtCurrentTeb()->Peb->NumberOfProcessors,
                                   sizeof(PROCESSOR_POWER_INFORMATION), &size ) || size > MAXDWORD)
            return STATUS_BUFFER_TOO_SMALL;
        out_size = size;
        if (out_len < out_size) return STATUS_BUFFER_TOO_SMALL;
        break;

    case SystemPowerInformation:   /* SYSTEM_POWER_INFORMATION */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(SYSTEM_POWER_INFORMATION)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(SYSTEM_POWER_INFORMATION);
        break;

    case LastWakeTime:   /* ULONGLONG */
    case LastSleepTime:   /* ULONGLONG */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(ULONGLONG)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(ULONGLONG);
        break;

    case SystemExecutionState:   /* EXECUTION_STATE */
        if (in_len) return in_buf ? STATUS_PRIVILEGE_NOT_HELD : STATUS_INVALID_PARAMETER;
        if (!out_len) return STATUS_INVALID_PARAMETER;
        if (out_len < sizeof(EXECUTION_STATE)) return STATUS_BUFFER_TOO_SMALL;
        if (!out_buf) return STATUS_INVALID_PARAMETER;
        out_size = sizeof(EXECUTION_STATE);
        break;

    case ProcessorStateHandler2:
    case ProcessorPowerPolicyAc:   /* PROCESSOR_POWER_POLICY */
    case ProcessorPowerPolicyDc:   /* PROCESSOR_POWER_POLICY */
    case VerifyProcessorPowerPolicyAc:   /* PROCESSOR_POWER_POLICY */
    case VerifyProcessorPowerPolicyDc:   /* PROCESSOR_POWER_POLICY */
    case ProcessorPowerPolicyCurrent:   /* PROCESSOR_POWER_POLICY */
        return NtPowerInformation( level, NULL, in_len, NULL, out_len );

    default:
        FIXME( "unsupported level %u\n", level );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (in_size)
    {
        in = system_alloc_temp( in_size );
        wow64_read_user( in, in_buf, in_size );
    }
    if (out_size)
    {
        if (preflight_out) wow64_probe_user_write( out_buf, out_size );
        out = system_alloc_temp( out_size );
        memset( out, 0, out_size );
    }
    status = NtPowerInformation( level, in, in_len, out, out_len );
    if (!status && out_size) wow64_write_user( out_buf, out, out_size );
    return status;
}


/**********************************************************************
 *           wow64_NtQueryLicenseValue
 */
NTSTATUS WINAPI wow64_NtQueryLicenseValue( UINT *args )
{
    UNICODE_STRING32 *str32 = get_ptr( &args );
    ULONG *type = get_ptr( &args );
    void *buffer = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    UNICODE_STRING str;
    const ULONG untouched = 0xdeadbeef;
    ULONG local_type = 0, required = untouched, result = untouched;
    void *out = NULL;
    NTSTATUS status;

    if (!str32)
        return NtQueryLicenseValue( NULL, type ? &local_type : NULL, NULL, len,
                                    retlen ? &result : NULL );
    if ((status = snapshot_system_unicode_string( &str, str32 ))) return status;
    if (!retlen)
        return NtQueryLicenseValue( &str, type ? &local_type : NULL, NULL, len, NULL );

    status = NtQueryLicenseValue( &str, type ? &local_type : NULL, NULL, 0, &required );
    if (status != STATUS_BUFFER_TOO_SMALL && status)
        return status;
    if (status == STATUS_BUFFER_TOO_SMALL && len < required)
    {
        if (type) wow64_write_user( type, &local_type, sizeof(local_type) );
        put_size( retlen, required );
        return status;
    }
    if (required)
    {
        out = system_alloc_temp( required );
        memset( out, 0, required );
    }
    result = untouched;
    status = NtQueryLicenseValue( &str, type ? &local_type : NULL, out, required,
                                  retlen ? &result : NULL );
    if (!status || status == STATUS_BUFFER_TOO_SMALL)
    {
        if (type) wow64_write_user( type, &local_type, sizeof(local_type) );
    }
    if (retlen && result != untouched) put_size( retlen, result );
    if (!status && result)
    {
        if (buffer) wow64_write_user( buffer, out, min( required, result ) );
        else status = STATUS_ACCESS_VIOLATION;
    }
    return status;
}


/**********************************************************************
 *           wow64_NtQuerySystemEnvironmentValue
 */
NTSTATUS WINAPI wow64_NtQuerySystemEnvironmentValue( UINT *args )
{
    UNICODE_STRING32 *str32 = get_ptr( &args );
    void *buffer = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    UNICODE_STRING str;
    NTSTATUS status;

    (void)buffer;
    (void)retlen;
    if ((status = snapshot_system_unicode_string( &str, str32 ))) return status;
    /* The native stub does not inspect or modify its output arguments. */
    return NtQuerySystemEnvironmentValue( &str, NULL, len, NULL );
}


/**********************************************************************
 *           wow64_NtQuerySystemEnvironmentValueEx
 */
NTSTATUS WINAPI wow64_NtQuerySystemEnvironmentValueEx( UINT *args )
{
    UNICODE_STRING32 *str32 = get_ptr( &args );
    GUID *vendor = get_ptr( &args );
    void *buffer = get_ptr( &args );
    ULONG *retlen = get_ptr( &args );
    ULONG *attributes = get_ptr( &args );

    UNICODE_STRING str;
    GUID local_vendor;
    NTSTATUS status;

    (void)buffer;
    (void)retlen;
    (void)attributes;
    if ((status = snapshot_system_unicode_string( &str, str32 ))) return status;
    if (vendor) wow64_read_user( &local_vendor, vendor, sizeof(local_vendor) );
    /* The native stub does not inspect or modify its output arguments. */
    return NtQuerySystemEnvironmentValueEx( &str, vendor ? &local_vendor : NULL,
                                            NULL, NULL, NULL );
}


/**********************************************************************
 *           wow64_NtQuerySystemInformation
 */
NTSTATUS WINAPI wow64_NtQuerySystemInformation( UINT *args )
{
    SYSTEM_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;

    switch (class)
    {
    case SystemPerformanceInformation:  /* SYSTEM_PERFORMANCE_INFORMATION */
    case SystemTimeOfDayInformation:  /* SYSTEM_TIMEOFDAY_INFORMATION */
    case SystemProcessorPerformanceInformation:  /* SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION */
    case SystemInterruptInformation:  /* SYSTEM_INTERRUPT_INFORMATION */
    case SystemTimeAdjustmentInformation:  /* SYSTEM_TIME_ADJUSTMENT_QUERY */
    case SystemKernelDebuggerInformation:  /* SYSTEM_KERNEL_DEBUGGER_INFORMATION */
    case SystemCurrentTimeZoneInformation:   /* RTL_TIME_ZONE_INFORMATION */
    case SystemRecommendedSharedDataAlignment:  /* ULONG */
    case SystemProcessorIdleCycleTimeInformation:  /* ULONG64[] */
    case SystemDynamicTimeZoneInformation:  /* RTL_DYNAMIC_TIME_ZONE_INFORMATION */
    case SystemKernelDebuggerInformationEx:  /* SYSTEM_KERNEL_DEBUGGER_INFORMATION_EX */
    case SystemCpuSetInformation:  /* SYSTEM_CPU_SET_INFORMATION */
    case SystemSecureBootInformation:  /* SYSTEM_SECUREBOOT_INFORMATION */
    case SystemLeapSecondInformation: /* SYSTEM_LEAP_SECOND_INFORMATION */
    case SystemProcessorBrandString:  /* char[] */
    case SystemProcessorFeaturesInformation:  /* SYSTEM_PROCESSOR_FEATURES_INFORMATION */
    case SystemWineVersionInformation:  /* char[] */
        return query_system_buffer( class, ptr, len, retlen );

    case SystemFirmwareTableInformation:  /* SYSTEM_FIRMWARE_TABLE_INFORMATION */
    {
        const ULONG min_size = offsetof(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);
        SYSTEM_FIRMWARE_TABLE_INFORMATION header;
        ULONG capacity = min_size, result = 0, copy_size;
        SYSTEM_FIRMWARE_TABLE_INFORMATION *info;

        if (len < min_size)
        {
            status = NtQuerySystemInformation( class, NULL, len, &result );
            put_size( retlen, result );
            return status;
        }

        wow64_read_user( &header, ptr, min_size );
        info = system_alloc_temp( min_size );
        memcpy( info, &header, min_size );
        status = NtQuerySystemInformation( class, info, min_size, &result );
        if (status == STATUS_BUFFER_TOO_SMALL && result >= min_size && result <= len)
        {
            capacity = result;
            info = system_alloc_temp( result );
            memcpy( info, &header, min_size );
            status = NtQuerySystemInformation( class, info, result, &result );
        }
        if (status == STATUS_BUFFER_TOO_SMALL)
            wow64_write_user( (char *)ptr + offsetof(SYSTEM_FIRMWARE_TABLE_INFORMATION,
                                                     TableBufferLength),
                              &info->TableBufferLength, sizeof(info->TableBufferLength) );
        else if (!status)
        {
            if (result < min_size || result > capacity ||
                info->TableBufferLength > result - min_size)
                return STATUS_INFO_LENGTH_MISMATCH;
            copy_size = sizeof(info->TableBufferLength) + info->TableBufferLength;
            wow64_write_user( (char *)ptr + offsetof(SYSTEM_FIRMWARE_TABLE_INFORMATION,
                                                     TableBufferLength),
                              &info->TableBufferLength, copy_size );
        }
        put_size( retlen, result );
        return status;
    }

    case SystemCodeIntegrityInformation:  /* SYSTEM_CODEINTEGRITY_INFORMATION */
    {
        SYSTEM_CODEINTEGRITY_INFORMATION info = {0};
        ULONG result = 0;

        status = NtQuerySystemInformation( class, len >= sizeof(info) ? &info : NULL,
                                           min( len, (ULONG)sizeof(info) ), &result );
        if (!status)
            wow64_write_user( (char *)ptr + offsetof(SYSTEM_CODEINTEGRITY_INFORMATION,
                                                     CodeIntegrityOptions),
                              &info.CodeIntegrityOptions, sizeof(info.CodeIntegrityOptions) );
        put_size( retlen, result );
        return status;
    }

    case SystemCpuInformation:  /* SYSTEM_CPU_INFORMATION */
    case SystemEmulationProcessorInformation:  /* SYSTEM_CPU_INFORMATION */
    {
        ULONG capacity = min( len, (ULONG)sizeof(SYSTEM_CPU_INFORMATION) ), retsize = 0;
        SYSTEM_CPU_INFORMATION *info;

        if (!ptr)
        {
            status = NtQuerySystemInformation( SystemEmulationProcessorInformation, NULL,
                                               len, &retsize );
            put_size( retlen, retsize );
            return status;
        }
        info = system_alloc_temp( capacity );
        if (capacity) memset( info, 0, capacity );
        status = NtQuerySystemInformation( SystemEmulationProcessorInformation, info,
                                           capacity, &retsize );
        if (!status)
        {
            if (pBTCpuUpdateProcessorInformation) pBTCpuUpdateProcessorInformation( info );
            if (retsize) wow64_write_user( ptr, info, min( capacity, retsize ) );
        }
        put_size( retlen, retsize );
        return status;
    }

    case SystemBasicInformation:  /* SYSTEM_BASIC_INFORMATION */
    case SystemEmulationBasicInformation:  /* SYSTEM_BASIC_INFORMATION */
        if (len == sizeof(SYSTEM_BASIC_INFORMATION32))
        {
            SYSTEM_BASIC_INFORMATION info;
            SYSTEM_BASIC_INFORMATION32 info32;

            if (!(status = NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL )))
            {
                put_system_basic_information( &info32, &info );
                wow64_write_user( ptr, &info32, sizeof(info32) );
            }
        }
        else status = STATUS_INFO_LENGTH_MISMATCH;
        put_size( retlen, sizeof(SYSTEM_BASIC_INFORMATION32) );
        return status;

    case SystemProcessInformation:  /* SYSTEM_PROCESS_INFORMATION */
    case SystemExtendedProcessInformation:  /* SYSTEM_PROCESS_INFORMATION */
    {
        ULONG size = 0, capacity, out_capacity, retsize = 0, outsize = 0, written = 0;
        SYSTEM_PROCESS_INFORMATION32 *info32;
        SYSTEM_PROCESS_INFORMATION *info;

        status = NtQuerySystemInformation( class, NULL, 0, &size );
        if (status != STATUS_INFO_LENGTH_MISMATCH && status)
        {
            put_size( retlen, size );
            return status;
        }
        info = system_alloc_temp( size );
        capacity = size;
        status = NtQuerySystemInformation( class, info, size, &retsize );
        if (status == STATUS_INFO_LENGTH_MISMATCH && retsize > capacity)
        {
            size = retsize;
            info = system_alloc_temp( size );
            capacity = size;
            status = NtQuerySystemInformation( class, info, size, &retsize );
        }
        if (!status)
        {
            if (retsize > capacity) return STATUS_INFO_LENGTH_MISMATCH;
            status = put_system_proc_info( NULL, ptr, info, retsize,
                                           class == SystemExtendedProcessInformation,
                                           0, &outsize, &written );
            if (status != STATUS_INFO_LENGTH_MISMATCH && status) return status;
            out_capacity = min( len, outsize );
            info32 = system_alloc_temp( out_capacity );
            if (out_capacity) memset( info32, 0, out_capacity );
            status = put_system_proc_info( info32, ptr, info, retsize,
                                           class == SystemExtendedProcessInformation,
                                           len, &outsize, &written );
            if (written) wow64_write_user( ptr, info32, written );
            put_size( retlen, outsize );
            return status;
        }
        put_size( retlen, retsize );
        return status;
    }

    case SystemModuleInformation:  /* RTL_PROCESS_MODULES */
    {
        const ULONG native_header = offsetof(RTL_PROCESS_MODULES, Modules);
        const ULONG guest_header = offsetof(RTL_PROCESS_MODULES32, Modules);
        ULONG count = 0, retsize = 0, guest_retsize = 0;
        SIZE_T converted;

        status = NtQuerySystemInformation( class, NULL, 0, &retsize );
        if (retsize >= native_header)
        {
            ULONG bytes = retsize - native_header;

            count = bytes / sizeof(RTL_PROCESS_MODULE_INFORMATION) +
                    !!(bytes % sizeof(RTL_PROCESS_MODULE_INFORMATION));
            if (system_multiply_size( count, sizeof(RTL_PROCESS_MODULE_INFORMATION32),
                                      &converted ) && converted <= MAXDWORD - guest_header)
                guest_retsize = guest_header + converted;
            else guest_retsize = MAXDWORD;
        }
        else guest_retsize = retsize;

        if (status == STATUS_INFO_LENGTH_MISMATCH && guest_retsize != MAXDWORD &&
            len >= guest_retsize && retsize >= native_header)
        {
            RTL_PROCESS_MODULES *info = system_alloc_temp( retsize );
            RTL_PROCESS_MODULES32 *info32 = system_alloc_temp( guest_retsize );
            ULONG capacity = retsize, i;

            memset( info32, 0, guest_retsize );
            if (!(status = NtQuerySystemInformation( class, info, capacity, &retsize )))
            {
                if (retsize < native_header || retsize > capacity ||
                    info->ModulesCount > (retsize - native_header) /
                                         sizeof(info->Modules[0]) ||
                    info->ModulesCount > count)
                    return STATUS_INFO_LENGTH_MISMATCH;
                info32->ModulesCount = info->ModulesCount;
                for (i = 0; i < info->ModulesCount; i++)
                {
                    info32->Modules[i].Section           = HandleToULong( info->Modules[i].Section );
                    info32->Modules[i].MappedBaseAddress = 0;
                    info32->Modules[i].ImageBaseAddress  = 0;
                    info32->Modules[i].ImageSize         = info->Modules[i].ImageSize;
                    info32->Modules[i].Flags             = info->Modules[i].Flags;
                    info32->Modules[i].LoadOrderIndex    = info->Modules[i].LoadOrderIndex;
                    info32->Modules[i].InitOrderIndex    = info->Modules[i].InitOrderIndex;
                    info32->Modules[i].LoadCount         = info->Modules[i].LoadCount;
                    info32->Modules[i].NameOffset        = info->Modules[i].NameOffset;
                    memcpy( info32->Modules[i].Name, info->Modules[i].Name,
                            sizeof(info32->Modules[i].Name) );
                }
                wow64_write_user( ptr, info32,
                                  guest_header + info->ModulesCount * sizeof(info32->Modules[0]) );
            }
        }

        if (retsize >= native_header)
        {
            ULONG bytes = retsize - native_header;

            count = bytes / sizeof(RTL_PROCESS_MODULE_INFORMATION) +
                    !!(bytes % sizeof(RTL_PROCESS_MODULE_INFORMATION));

            if (system_multiply_size( count, sizeof(RTL_PROCESS_MODULE_INFORMATION32), &converted ) &&
                converted <= MAXDWORD - guest_header)
                guest_retsize = guest_header + converted;
            else
                guest_retsize = MAXDWORD;
        }
        else guest_retsize = retsize;
        put_size( retlen, guest_retsize );
        return status;
    }

    case SystemProcessIdInformation:  /* SYSTEM_PROCESS_ID_INFORMATION */
    {
        SYSTEM_PROCESS_ID_INFORMATION32 info32;
        SYSTEM_PROCESS_ID_INFORMATION info;
        WCHAR *buffer = NULL;

        put_size( retlen, sizeof(info32) );
        if (len < sizeof(info32)) return STATUS_INFO_LENGTH_MISMATCH;
        wow64_read_user( &info32, ptr, sizeof(info32) );
        if (info32.ImageName.Length) return STATUS_INVALID_PARAMETER;
        if (info32.ImageName.MaximumLength && !info32.ImageName.Buffer)
            return STATUS_ACCESS_VIOLATION;

        info.ProcessId = info32.ProcessId;
        info.ImageName.Length = info32.ImageName.Length;
        info.ImageName.MaximumLength = 0;
        info.ImageName.Buffer = NULL;
        status = NtQuerySystemInformation( class, &info, sizeof(info), NULL );
        if (status == STATUS_INFO_LENGTH_MISMATCH &&
            info.ImageName.MaximumLength <= info32.ImageName.MaximumLength)
        {
            if (info.ImageName.MaximumLength)
            {
                buffer = system_alloc_temp( info.ImageName.MaximumLength );
                memset( buffer, 0, info.ImageName.MaximumLength );
            }
            info.ImageName.Buffer = buffer;
            status = NtQuerySystemInformation( class, &info, sizeof(info), NULL );
        }
        if (!status || status == STATUS_INFO_LENGTH_MISMATCH)
        {
            wow64_write_user( (char *)ptr + offsetof(SYSTEM_PROCESS_ID_INFORMATION32,
                                                     ImageName.MaximumLength),
                              &info.ImageName.MaximumLength,
                              sizeof(info.ImageName.MaximumLength) );
            if (!status)
            {
                if (info.ImageName.Length > info.ImageName.MaximumLength ||
                    info.ImageName.MaximumLength > info32.ImageName.MaximumLength ||
                    info.ImageName.MaximumLength < sizeof(WCHAR) ||
                    info.ImageName.Length > info.ImageName.MaximumLength - sizeof(WCHAR))
                    return STATUS_INFO_LENGTH_MISMATCH;
                wow64_write_user( (char *)ptr + offsetof(SYSTEM_PROCESS_ID_INFORMATION32,
                                                         ImageName.Length),
                                  &info.ImageName.Length, sizeof(info.ImageName.Length) );
                if (info.ImageName.Length)
                    wow64_write_user( wow64_guest_memory_ptr( info32.ImageName.Buffer ), buffer,
                                      info.ImageName.Length + sizeof(WCHAR) );
            }
        }
        return status;
    }

    case SystemHandleInformation:  /* SYSTEM_HANDLE_INFORMATION */
    {
        const ULONG native_header = offsetof(SYSTEM_HANDLE_INFORMATION, Handle);
        const ULONG guest_header = offsetof(SYSTEM_HANDLE_INFORMATION32, Handle);
        ULONG count = 0, retsize = 0, guest_retsize = 0;
        SIZE_T converted;

        status = NtQuerySystemInformation( class, NULL, 0, &retsize );
        if (retsize >= native_header)
        {
            ULONG bytes = retsize - native_header;

            count = bytes / sizeof(SYSTEM_HANDLE_ENTRY) +
                    !!(bytes % sizeof(SYSTEM_HANDLE_ENTRY));
            if (system_multiply_size( count, sizeof(SYSTEM_HANDLE_ENTRY32), &converted ) &&
                converted <= MAXDWORD - guest_header)
                guest_retsize = guest_header + converted;
            else guest_retsize = MAXDWORD;
        }
        else guest_retsize = retsize;

        if (status == STATUS_INFO_LENGTH_MISMATCH && guest_retsize != MAXDWORD &&
            len >= guest_retsize && retsize >= native_header)
        {
            SYSTEM_HANDLE_INFORMATION *info = system_alloc_temp( retsize );
            SYSTEM_HANDLE_INFORMATION32 *info32 = system_alloc_temp( guest_retsize );
            ULONG capacity = retsize, i;

            memset( info32, 0, guest_retsize );
            if (!(status = NtQuerySystemInformation( class, info, capacity, &retsize )))
            {
                if (retsize < native_header || retsize > capacity ||
                    info->Count > (retsize - native_header) / sizeof(info->Handle[0]) ||
                    info->Count > count)
                    return STATUS_INFO_LENGTH_MISMATCH;
                info32->Count = info->Count;
                for (i = 0; i < info->Count; i++)
                {
                    info32->Handle[i].OwnerPid      = info->Handle[i].OwnerPid;
                    info32->Handle[i].ObjectType    = info->Handle[i].ObjectType;
                    info32->Handle[i].HandleFlags   = info->Handle[i].HandleFlags;
                    info32->Handle[i].HandleValue   = info->Handle[i].HandleValue;
                    info32->Handle[i].ObjectPointer = PtrToUlong( info->Handle[i].ObjectPointer );
                    info32->Handle[i].AccessMask    = info->Handle[i].AccessMask;
                }
                wow64_write_user( ptr, info32,
                                  guest_header + info->Count * sizeof(info32->Handle[0]) );
            }
        }
        if (retsize >= native_header)
        {
            ULONG bytes = retsize - native_header;

            count = bytes / sizeof(SYSTEM_HANDLE_ENTRY) +
                    !!(bytes % sizeof(SYSTEM_HANDLE_ENTRY));

            if (system_multiply_size( count, sizeof(SYSTEM_HANDLE_ENTRY32), &converted ) &&
                converted <= MAXDWORD - guest_header)
                guest_retsize = guest_header + converted;
            else guest_retsize = MAXDWORD;
        }
        else guest_retsize = retsize;
        put_size( retlen, guest_retsize );
        return status;
    }

    case SystemFileCacheInformation:   /* SYSTEM_CACHE_INFORMATION */
        if (len >= sizeof(SYSTEM_CACHE_INFORMATION32))
        {
            SYSTEM_CACHE_INFORMATION info;
            SYSTEM_CACHE_INFORMATION32 info32;

            if (!(status = NtQuerySystemInformation( class, &info, sizeof(info), NULL )))
            {
                info32.CurrentSize                           = info.CurrentSize;
                info32.PeakSize                              = info.PeakSize;
                info32.PageFaultCount                        = info.PageFaultCount;
                info32.MinimumWorkingSet                     = info.MinimumWorkingSet;
                info32.MaximumWorkingSet                     = info.MaximumWorkingSet;
                info32.CurrentSizeIncludingTransitionInPages = info.CurrentSizeIncludingTransitionInPages;
                info32.PeakSizeIncludingTransitionInPages    = info.PeakSizeIncludingTransitionInPages;
                info32.TransitionRePurposeCount              = info.TransitionRePurposeCount;
                info32.Flags                                 = info.Flags;
                wow64_write_user( ptr, &info32, sizeof(info32) );
            }
        }
        else status = STATUS_INFO_LENGTH_MISMATCH;
        put_size( retlen, sizeof(SYSTEM_CACHE_INFORMATION32) );
        return status;

    case SystemRegistryQuotaInformation:  /* SYSTEM_REGISTRY_QUOTA_INFORMATION */
        if (len >= sizeof(SYSTEM_REGISTRY_QUOTA_INFORMATION32))
        {
            SYSTEM_REGISTRY_QUOTA_INFORMATION info;
            SYSTEM_REGISTRY_QUOTA_INFORMATION32 info32;

            if (!(status = NtQuerySystemInformation( class, &info, sizeof(info), NULL )))
            {
                info32.RegistryQuotaAllowed = info.RegistryQuotaAllowed;
                info32.RegistryQuotaUsed = info.RegistryQuotaUsed;
                info32.Reserved1 = PtrToUlong( info.Reserved1 );
                wow64_write_user( ptr, &info32, sizeof(info32) );
            }
        }
        else status = STATUS_INFO_LENGTH_MISMATCH;
        put_size( retlen, sizeof(SYSTEM_REGISTRY_QUOTA_INFORMATION32) );
        return status;

    case SystemExtendedHandleInformation:  /* SYSTEM_HANDLE_INFORMATION_EX */
    {
        const ULONG native_header = offsetof(SYSTEM_HANDLE_INFORMATION_EX, Handles);
        const ULONG guest_header = offsetof(SYSTEM_HANDLE_INFORMATION_EX32, Handles);
        ULONG count = 0, retsize = 0, guest_retsize = 0;
        SIZE_T converted;

        status = NtQuerySystemInformation( class, NULL, 0, &retsize );
        if (retsize >= native_header)
        {
            ULONG bytes = retsize - native_header;

            count = bytes / sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX) +
                    !!(bytes % sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX));
            if (system_multiply_size( count, sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX32),
                                      &converted ) && converted <= MAXDWORD - guest_header)
                guest_retsize = guest_header + converted;
            else guest_retsize = MAXDWORD;
        }
        else guest_retsize = retsize;

        if (status == STATUS_INFO_LENGTH_MISMATCH && guest_retsize != MAXDWORD &&
            len >= guest_retsize && retsize >= native_header)
        {
            SYSTEM_HANDLE_INFORMATION_EX *info = system_alloc_temp( retsize );
            SYSTEM_HANDLE_INFORMATION_EX32 *info32 = system_alloc_temp( guest_retsize );
            ULONG capacity = retsize, i;

            memset( info32, 0, guest_retsize );
            if (!(status = NtQuerySystemInformation( class, info, capacity, &retsize )))
            {
                if (retsize < native_header || retsize > capacity ||
                    info->NumberOfHandles > (retsize - native_header) /
                                            sizeof(info->Handles[0]) ||
                    info->NumberOfHandles > count)
                    return STATUS_INFO_LENGTH_MISMATCH;
                info32->NumberOfHandles = info->NumberOfHandles;
                info32->Reserved        = info->Reserved;
                for (i = 0; i < info->NumberOfHandles; i++)
                {
                    info32->Handles[i].Object                = PtrToUlong( info->Handles[i].Object );
                    info32->Handles[i].UniqueProcessId       = info->Handles[i].UniqueProcessId;
                    info32->Handles[i].HandleValue           = info->Handles[i].HandleValue;
                    info32->Handles[i].GrantedAccess         = info->Handles[i].GrantedAccess;
                    info32->Handles[i].CreatorBackTraceIndex = info->Handles[i].CreatorBackTraceIndex;
                    info32->Handles[i].ObjectTypeIndex       = info->Handles[i].ObjectTypeIndex;
                    info32->Handles[i].HandleAttributes      = info->Handles[i].HandleAttributes;
                    info32->Handles[i].Reserved              = info->Handles[i].Reserved;
                }
                wow64_write_user( ptr, info32,
                                  guest_header + info32->NumberOfHandles * sizeof(info32->Handles[0]) );
            }
        }
        if (retsize >= native_header)
        {
            ULONG bytes = retsize - native_header;

            count = bytes / sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX) +
                    !!(bytes % sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX));

            if (system_multiply_size( count, sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX32), &converted ) &&
                converted <= MAXDWORD - guest_header)
                guest_retsize = guest_header + converted;
            else guest_retsize = MAXDWORD;
        }
        else guest_retsize = retsize;
        put_size( retlen, guest_retsize );
        return status;
    }

    case SystemLogicalProcessorInformation:  /* SYSTEM_LOGICAL_PROCESSOR_INFORMATION */
        {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info;
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION32 *info32;
            ULONG i, size = 0, capacity, count, out_size;
            SIZE_T converted;

            status = NtQuerySystemInformation( class, NULL, 0, &size );
            if (status != STATUS_INFO_LENGTH_MISMATCH && status) return status;
            info = system_alloc_temp( size );
            capacity = size;
            status = NtQuerySystemInformation( class, info, size, &size );
            if (status) return status;
            if (size > capacity || size % sizeof(*info)) return STATUS_INFO_LENGTH_MISMATCH;
            count = size / sizeof(*info);
            if (!system_multiply_size( count, sizeof(*info32), &converted ) || converted > MAXDWORD)
                return STATUS_INFO_LENGTH_MISMATCH;
            out_size = converted;
            if (len >= out_size)
            {
                info32 = system_alloc_temp( out_size );
                for (i = 0; i < count; i++)
                {
                    info32[i].ProcessorMask = info[i].ProcessorMask;
                    info32[i].Relationship  = info[i].Relationship;
                    info32[i].Reserved[0]   = info[i].Reserved[0];
                    info32[i].Reserved[1]   = info[i].Reserved[1];
                }
                if (out_size) wow64_write_user( ptr, info32, out_size );
            }
            else status = STATUS_INFO_LENGTH_MISMATCH;
            put_size( retlen, out_size );
            return status;
        }

    case SystemModuleInformationEx:   /* RTL_PROCESS_MODULE_INFORMATION_EX */
    {
        RTL_PROCESS_MODULE_INFORMATION_EX32 *info32;
        RTL_PROCESS_MODULE_INFORMATION_EX *info, *mod;
        ULONG native_size = 0, native_capacity, guest_size, count = 0, pos = 0, i;
        SIZE_T converted;
        BOOL terminated = FALSE;

        status = NtQuerySystemInformation( class, NULL, 0, &native_size );
        if (status != STATUS_INFO_LENGTH_MISMATCH && status) return status;
        info = system_alloc_temp( native_size );
        native_capacity = native_size;
        status = NtQuerySystemInformation( class, info, native_size, &native_size );
        if (status) return status;
        if (native_size > native_capacity) return STATUS_INFO_LENGTH_MISMATCH;
        while (pos < native_size)
        {
            mod = (RTL_PROCESS_MODULE_INFORMATION_EX *)((char *)info + pos);
            if (native_size - pos < sizeof(mod->NextOffset)) return STATUS_INFO_LENGTH_MISMATCH;
            if (!mod->NextOffset)
            {
                terminated = TRUE;
                break;
            }
            if (mod->NextOffset < sizeof(*mod) || mod->NextOffset > native_size - pos)
                return STATUS_INFO_LENGTH_MISMATCH;
            count++;
            pos += mod->NextOffset;
        }
        if (!terminated) return STATUS_INFO_LENGTH_MISMATCH;
        if (!system_multiply_size( count, sizeof(*info32), &converted ) ||
            converted > MAXDWORD - sizeof(USHORT))
            return STATUS_INFO_LENGTH_MISMATCH;
        guest_size = converted + sizeof(USHORT);
        put_size( retlen, guest_size );
        if (len < guest_size) return STATUS_INFO_LENGTH_MISMATCH;
        info32 = system_alloc_temp( guest_size );
        memset( info32, 0, guest_size );
        mod = info;
        for (i = 0; i < count; i++)
        {
            info32[i].NextOffset                 = sizeof(*info32);
            info32[i].BaseInfo.Section           = HandleToULong( mod->BaseInfo.Section );
            info32[i].BaseInfo.MappedBaseAddress = 0;
            info32[i].BaseInfo.ImageBaseAddress  = 0;
            info32[i].BaseInfo.ImageSize         = mod->BaseInfo.ImageSize;
            info32[i].BaseInfo.Flags             = mod->BaseInfo.Flags;
            info32[i].BaseInfo.LoadOrderIndex    = mod->BaseInfo.LoadOrderIndex;
            info32[i].BaseInfo.InitOrderIndex    = mod->BaseInfo.InitOrderIndex;
            info32[i].BaseInfo.LoadCount         = mod->BaseInfo.LoadCount;
            info32[i].BaseInfo.NameOffset        = mod->BaseInfo.NameOffset;
            info32[i].ImageCheckSum              = mod->ImageCheckSum;
            info32[i].TimeDateStamp              = mod->TimeDateStamp;
            info32[i].DefaultBase                = 0;
            memcpy( info32[i].BaseInfo.Name, mod->BaseInfo.Name, sizeof(info32[i].BaseInfo.Name) );
            mod = (RTL_PROCESS_MODULE_INFORMATION_EX *)((char *)mod + mod->NextOffset);
        }
        *(USHORT *)((char *)info32 + count * sizeof(*info32)) = 0;
        wow64_write_user( ptr, info32, guest_size );
        return STATUS_SUCCESS;
    }

    case SystemNativeBasicInformation:
        return STATUS_INVALID_INFO_CLASS;

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtQuerySystemInformationEx
 */
NTSTATUS WINAPI wow64_NtQuerySystemInformationEx( UINT *args )
{
    SYSTEM_INFORMATION_CLASS class = get_ulong( &args );
    void *query = get_ptr( &args );
    ULONG query_len = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    HANDLE handle;
    NTSTATUS status;

    switch (class)
    {
    case SystemProcessorIdleCycleTimeInformation:
    {
        USHORT group;
        ULONG capacity, result = 0;
        void *out;

        if (!query || query_len < sizeof(group)) return STATUS_INVALID_PARAMETER;
        wow64_read_user( &group, query, sizeof(group) );
        capacity = min( len, NtCurrentTeb()->Peb->NumberOfProcessors * sizeof(ULONG64) );
        out = ptr ? system_alloc_temp( capacity ) : NULL;
        if (out) memset( out, 0, capacity );
        status = NtQuerySystemInformationEx( class, &group, sizeof(group), out, capacity, &result );
        if (!status && result) wow64_write_user( ptr, out, min( capacity, result ) );
        put_size( retlen, result );
        return status;
    }

    case SystemLogicalProcessorInformationEx:  /* SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX */
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32 *ex32, *info32;
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *ex, *info;
        ULONG relation, size = 0, capacity, out_capacity, size32;
        ULONG pos = 0, pos32 = 0, written = 0;

        if (!query || query_len < sizeof(LONG)) return STATUS_INVALID_PARAMETER;
        wow64_read_user( &relation, query, sizeof(relation) );
        status = NtQuerySystemInformationEx( class, &relation, sizeof(relation), NULL, 0, &size );
        if (status != STATUS_INFO_LENGTH_MISMATCH && status) return status;
        info = system_alloc_temp( size );
        capacity = size;
        status = NtQuerySystemInformationEx( class, &relation, sizeof(relation), info, size, &size );
        if (!status)
        {
            if (size > capacity) return STATUS_INFO_LENGTH_MISMATCH;
            for (pos = 0; pos < size; pos += ex->Size)
            {
                ex = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((char *)info + pos);
                if ((status = get_logical_proc_info_ex32_size( ex, size - pos, &size32 )))
                    return status;
                if (size32)
                {
                    if (pos32 > MAXDWORD - size32) return STATUS_INFO_LENGTH_MISMATCH;
                    pos32 += size32;
                }
            }

            out_capacity = min( len, pos32 );
            info32 = system_alloc_temp( out_capacity );
            if (out_capacity) memset( info32, 0, out_capacity );
            for (pos = pos32 = 0; pos < size; pos += ex->Size)
            {
                ex = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((char *)info + pos);
                if ((status = get_logical_proc_info_ex32_size( ex, size - pos, &size32 )))
                    return status;
                if (!size32) continue;
                if (pos32 <= out_capacity && size32 <= out_capacity - pos32)
                {
                    ex32 = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX32 *)((char *)info32 + pos32);
                    put_logical_proc_info_ex( ex32, ex );
                    written = pos32 + size32;
                }
                pos32 += size32;
            }
            if (pos32 > len) status = STATUS_INFO_LENGTH_MISMATCH;
            if (written) wow64_write_user( ptr, info32, written );
        }
        put_size( retlen, pos32 );
        return status;
    }

    case SystemCpuSetInformation:  /* SYSTEM_CPU_SET_INFORMATION */
    case SystemSupportedProcessorArchitectures:  /* SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION */
    case SystemSupportedProcessorArchitectures2: /* SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION */
        if (!query || query_len < sizeof(LONG)) return STATUS_INVALID_PARAMETER;
    {
        const ULONG untouched = 0xdeadbeef;
        ULONG capacity, result = untouched;
        LONG handle32;
        void *out;

        wow64_read_user( &handle32, query, sizeof(handle32) );
        handle = LongToHandle( handle32 );
        if (class == SystemCpuSetInformation)
            capacity = NtCurrentTeb()->Peb->NumberOfProcessors *
                       sizeof(SYSTEM_CPU_SET_INFORMATION);
        else
            capacity = 9 * sizeof(SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION);
        capacity = min( len, capacity );
        out = ptr ? system_alloc_temp( capacity ) : NULL;
        if (out) memset( out, 0, capacity );
        status = NtQuerySystemInformationEx( class, &handle, sizeof(handle), out, capacity, &result );
        if (!status && capacity)
            wow64_write_user( ptr, out, result == untouched ? capacity : min( capacity, result ) );
        if (retlen && result != untouched) put_size( retlen, result );
        return status;
    }

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
}


/**********************************************************************
 *           wow64_NtQuerySystemTime
 */
NTSTATUS WINAPI wow64_NtQuerySystemTime( UINT *args )
{
    LARGE_INTEGER *time = get_ptr( &args );
    LARGE_INTEGER local;
    NTSTATUS status;

    status = NtQuerySystemTime( &local );
    if (!status) wow64_write_user( time, &local, sizeof(local) );
    return status;
}


/**********************************************************************
 *           wow64_NtRaiseHardError
 */
NTSTATUS WINAPI wow64_NtRaiseHardError( UINT *args )
{
    NTSTATUS status = get_ulong( &args );
    ULONG count = get_ulong( &args );
    ULONG params_mask = get_ulong( &args );
    ULONG *params = get_ptr( &args );
    HARDERROR_RESPONSE_OPTION option = get_ulong( &args );
    HARDERROR_RESPONSE *response = get_ptr( &args );

    FIXME( "%08lx %lu %lx %p %u %p: stub\n", status, count, params_mask, params, option, response );
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *           wow64_NtSetIntervalProfile
 */
NTSTATUS WINAPI wow64_NtSetIntervalProfile( UINT *args )
{
    ULONG interval = get_ulong( &args );
    KPROFILE_SOURCE source = get_ulong( &args );

    return NtSetIntervalProfile( interval, source );
}


/**********************************************************************
 *           wow64_NtSetSystemInformation
 */
NTSTATUS WINAPI wow64_NtSetSystemInformation( UINT *args )
{
    SYSTEM_INFORMATION_CLASS class;
    ULONG len;

    class = get_ulong( &args );
    get_ptr( &args );
    len = get_ulong( &args );

    /* The native implementation is a stub and does not inspect the buffer. */
    return NtSetSystemInformation( class, NULL, len );
}


/**********************************************************************
 *           wow64_NtSetSystemTime
 */
NTSTATUS WINAPI wow64_NtSetSystemTime( UINT *args )
{
    const LARGE_INTEGER *new = get_ptr( &args );
    LARGE_INTEGER *old = get_ptr( &args );
    LARGE_INTEGER local_new, local_old;
    NTSTATUS status;

    if (old)
    {
        status = NtQuerySystemTime( &local_old );
        if (status) return status;
        wow64_write_user( old, &local_old, sizeof(local_old) );
    }
    wow64_read_user( &local_new, new, sizeof(local_new) );
    return NtSetSystemTime( &local_new, NULL );
}


/**********************************************************************
 *           wow64_NtShutdownSystem
 */
NTSTATUS WINAPI wow64_NtShutdownSystem( UINT *args )
{
    SHUTDOWN_ACTION action = get_ulong( &args );

    return NtShutdownSystem( action );
}


/**********************************************************************
 *           wow64_NtSystemDebugControl
 */
NTSTATUS WINAPI wow64_NtSystemDebugControl( UINT *args )
{
    SYSDBG_COMMAND command;
    ULONG in_len, out_len;

    command = get_ulong( &args );
    get_ptr( &args );
    in_len = get_ulong( &args );
    get_ptr( &args );
    out_len = get_ulong( &args );
    get_ptr( &args );

    switch (command)
    {
    case SysDbgBreakPoint:
    case SysDbgEnableKernelDebugger:
    case SysDbgDisableKernelDebugger:
    case SysDbgGetAutoKdEnable:
    case SysDbgSetAutoKdEnable:
    case SysDbgGetPrintBufferSize:
    case SysDbgSetPrintBufferSize:
    case SysDbgGetKdUmExceptionEnable:
    case SysDbgSetKdUmExceptionEnable:
    case SysDbgGetTriageDump:
    case SysDbgGetKdBlockEnable:
    case SysDbgSetKdBlockEnable:
    case SysDbgRegisterForUmBreakInfo:
    case SysDbgGetUmBreakPid:
    case SysDbgClearUmBreakPid:
    case SysDbgGetUmAttachPid:
    case SysDbgClearUmAttachPid:
        /* The native implementation only reports an inactive debugger. */
        return NtSystemDebugControl( command, NULL, in_len, NULL, out_len, NULL );

    default:
        return STATUS_NOT_IMPLEMENTED;  /* not implemented on Windows either */
    }
}


/**********************************************************************
 *           wow64_NtUnloadDriver
 */
NTSTATUS WINAPI wow64_NtUnloadDriver( UINT *args )
{
    UNICODE_STRING32 *str32 = get_ptr( &args );

    UNICODE_STRING str;
    NTSTATUS status;

    if ((status = snapshot_system_unicode_string( &str, str32 ))) return status;
    return NtUnloadDriver( &str );
}


/**********************************************************************
 *           wow64_NtWow64GetNativeSystemInformation
 */
NTSTATUS WINAPI wow64_NtWow64GetNativeSystemInformation( UINT *args )
{
    ULONG class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    NTSTATUS status;

    switch (class)
    {
    case SystemBasicInformation:
    case SystemEmulationBasicInformation:
        if (len == sizeof(SYSTEM_BASIC_INFORMATION32))
        {
            SYSTEM_BASIC_INFORMATION info;
            SYSTEM_BASIC_INFORMATION32 info32;

            if (!(status = NtQuerySystemInformation( class, &info, sizeof(info), NULL )))
            {
                put_system_basic_information( &info32, &info );
                wow64_write_user( ptr, &info32, sizeof(info32) );
            }
        }
        else status = STATUS_INFO_LENGTH_MISMATCH;
        put_size( retlen, sizeof(SYSTEM_BASIC_INFORMATION32) );
        return status;

    case SystemCpuInformation:
    case SystemEmulationProcessorInformation:
    case SystemNativeBasicInformation:
        return query_system_buffer( class, ptr, len, retlen );

    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}
