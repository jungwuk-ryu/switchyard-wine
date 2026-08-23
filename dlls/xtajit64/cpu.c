/*
 * x86-64 emulation on ARM64
 *
 * Copyright 2024 Alexandre Julliard
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

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/debug.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(xtajit);

#ifdef HAVE_UNICORN

#define XTAJIT64_CALL(func,params) WINE_UNIX_CALL( unix_ ## func, params )

static ULONG_PTR rtl_exit_user_thread;
static ULONG host_page_size;

#define XTAJIT64_CONTROL_STACK_SIZE 0x40000
#define XTAJIT64_MAX_TRANSITION_DEPTH 64
#define XTAJIT64_MAX_RESYNC_ATTEMPTS 8
#define XTAJIT64_THREAD_STATE_MAGIC 0x363454494a415458ull /* "XTAJIT64" */

enum xtajit64_native_transition
{
    XTAJIT64_NATIVE_RETURN,
    XTAJIT64_NATIVE_EXIT,
    XTAJIT64_NATIVE_JUMP,
};

enum xtajit64_transition_frame_kind
{
    XTAJIT64_FRAME_ENTRY = 1,
    XTAJIT64_FRAME_EXIT,
};

struct xtajit64_transition_frame
{
    UINT64 guest_rsp;
    UINT64 native_sp;
    UINT64 native_pc;
    UINT32 kind;
    UINT32 reserved;
};

struct xtajit64_thread_state
{
    UINT64 magic;
    UINT64 allocation_size;
    UINT64 control_stack_top;
    UINT64 capture_sp;
    UINT64 capture_lr;
    UINT64 capture_target;
    UINT64 capture_x10;
    UINT32 capture_kind;
    UINT32 depth;
    struct xtajit64_transition_frame frames[XTAJIT64_MAX_TRANSITION_DEPTH];
};

typedef NTSTATUS (WINAPI *arm64x_get_information)( ULONG, void *, void * );
typedef NTSTATUS (WINAPI *arm64x_set_information)( ULONG, ULONG_PTR, void * );

extern void *__os_arm64x_get_x64_information;
extern void *__os_arm64x_set_x64_information;
extern NTSTATUS WINAPI __wine_arm64ec_get_x64_syscall_dispatcher( ULONG_PTR *, ULONG * );
extern NTSTATUS WINAPI __wine_arm64ec_prepare_x64_execution(void);

C_ASSERT( offsetof(TEB, ChpeV2CpuAreaInfo) == 0x1788 );
C_ASSERT( offsetof(CHPE_V2_CPU_AREA_INFO, ContextAmd64) == 0x18 );
C_ASSERT( offsetof(CHPE_V2_CPU_AREA_INFO, EmulatorData[0]) == 0x30 );
C_ASSERT( offsetof(struct xtajit64_thread_state, control_stack_top) == 0x10 );
C_ASSERT( offsetof(struct xtajit64_thread_state, capture_sp) == 0x18 );
C_ASSERT( offsetof(struct xtajit64_thread_state, capture_lr) == 0x20 );
C_ASSERT( offsetof(struct xtajit64_thread_state, capture_target) == 0x28 );
C_ASSERT( offsetof(struct xtajit64_thread_state, capture_x10) == 0x30 );
C_ASSERT( offsetof(struct xtajit64_thread_state, capture_kind) == 0x38 );
C_ASSERT( offsetof(struct xtajit64_thread_state, depth) == 0x3c );
C_ASSERT( offsetof(struct xtajit64_thread_state, frames) == 0x40 );
C_ASSERT( sizeof(struct xtajit64_transition_frame) == 0x20 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X8) == 0x78 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, Sp) == 0x98 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, Pc) == 0xf8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, Lr) == 0x120 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X6) == 0x130 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X9) == 0x150 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X15) == 0x190 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, V) == 0x1a0 );

static NTSTATUS init_unixlib(void)
{
    if (__wine_unixlib_handle) return STATUS_SUCCESS;
    return __wine_init_unix_call();
}

static NTSTATUS synchronize_transition_state_mapping( struct xtajit64_thread_state *state,
                                                       BOOL *provider_touched );
static NTSTATUS unregister_transition_state_mapping( struct xtajit64_thread_state *state );

static NTSTATUS allocate_transition_state( struct xtajit64_thread_state **ret )
{
    struct xtajit64_thread_state *state;
    SIZE_T size = XTAJIT64_CONTROL_STACK_SIZE;
    void *allocation = NULL;
    NTSTATUS status;

    status = NtAllocateVirtualMemory( GetCurrentProcess(), &allocation, 0, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if (status) return status;
    if (size < sizeof(*state) + 0x10000 ||
        (ULONG_PTR)allocation > ~(ULONG_PTR)0 - size)
    {
        void *free_base = allocation;
        SIZE_T free_size = 0;

        NtFreeVirtualMemory( GetCurrentProcess(), &free_base, &free_size, MEM_RELEASE );
        return STATUS_NO_MEMORY;
    }

    state = allocation;
    memset( state, 0, sizeof(*state) );
    state->magic = XTAJIT64_THREAD_STATE_MAGIC;
    state->allocation_size = size;
    state->control_stack_top = ((ULONG_PTR)allocation + size) & ~(ULONG_PTR)15;
    *ret = state;
    return STATUS_SUCCESS;
}

static NTSTATUS free_transition_state( struct xtajit64_thread_state *state )
{
    void *free_base = state;
    SIZE_T free_size = 0;

    return NtFreeVirtualMemory( GetCurrentProcess(), &free_base, &free_size, MEM_RELEASE );
}

static void *resolve_arm64ec_export( HMODULE module, const char *name )
{
    const IMAGE_ARM64EC_REDIRECTION_ENTRY *map;
    const IMAGE_ARM64EC_METADATA *metadata;
    const IMAGE_LOAD_CONFIG_DIRECTORY *cfg;
    const IMAGE_NT_HEADERS *nt;
    ULONG_PTR base = (ULONG_PTR)module;
    ULONG_PTR metadata_ptr, image_end;
    void *raw;
    ULONG size, rva;
    int min, max;

    if (!(raw = RtlFindExportedRoutineByName( module, name ))) return NULL;
    if (!(nt = RtlImageNtHeader( module )) ||
        nt->OptionalHeader.SizeOfImage < sizeof(*metadata) ||
        nt->OptionalHeader.SizeOfImage > ~(ULONG_PTR)0 - base)
        return NULL;
    image_end = base + nt->OptionalHeader.SizeOfImage;
    if (!(cfg = RtlImageDirectoryEntryToData( module, TRUE,
                                              IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, &size )))
        return NULL;
    if (size < sizeof(cfg->Size)) return NULL;
    size = min( size, cfg->Size );
    if (size < offsetof( IMAGE_LOAD_CONFIG_DIRECTORY, CHPEMetadataPointer ) +
               sizeof(cfg->CHPEMetadataPointer))
        return NULL;
    metadata_ptr = cfg->CHPEMetadataPointer;
    if (metadata_ptr < base || metadata_ptr > image_end - sizeof(*metadata)) return NULL;
    metadata = (const IMAGE_ARM64EC_METADATA *)metadata_ptr;
    if (!metadata->RedirectionMetadata || !metadata->RedirectionMetadataCount ||
        metadata->RedirectionMetadata > nt->OptionalHeader.SizeOfImage ||
        metadata->RedirectionMetadataCount > 0x7fffffff ||
        metadata->RedirectionMetadataCount >
            (nt->OptionalHeader.SizeOfImage - metadata->RedirectionMetadata) / sizeof(*map))
        return NULL;
    if ((ULONG_PTR)raw < base || (ULONG_PTR)raw - base > ~(ULONG)0) return NULL;
    rva = (ULONG)((ULONG_PTR)raw - base);
    map = (const IMAGE_ARM64EC_REDIRECTION_ENTRY *)(base + metadata->RedirectionMetadata);

    min = 0;
    max = metadata->RedirectionMetadataCount - 1;
    while (min <= max)
    {
        int pos = min + (max - min) / 2;

        if (map[pos].Source == rva)
        {
            if (map[pos].Destination >= nt->OptionalHeader.SizeOfImage) return NULL;
            return (void *)(base + map[pos].Destination);
        }
        if (map[pos].Source < rva) min = pos + 1;
        else max = pos - 1;
    }
    return NULL;
}

static void context_to_unix( struct xtajit64_x64_context *dst, const AMD64_CONTEXT *src )
{
    dst->rax = src->Rax;
    dst->rbx = src->Rbx;
    dst->rcx = src->Rcx;
    dst->rdx = src->Rdx;
    dst->rsi = src->Rsi;
    dst->rdi = src->Rdi;
    dst->rbp = src->Rbp;
    dst->rsp = src->Rsp;
    dst->r8 = src->R8;
    dst->r9 = src->R9;
    dst->r10 = src->R10;
    dst->r11 = src->R11;
    dst->r12 = src->R12;
    dst->r13 = src->R13;
    dst->r14 = src->R14;
    dst->r15 = src->R15;
    dst->rip = src->Rip;
    dst->eflags = src->EFlags;
    dst->mxcsr = src->MxCsr;
    memcpy( dst->xmm, &src->Xmm0, sizeof(dst->xmm) );
}

static void context_from_unix( AMD64_CONTEXT *dst, const struct xtajit64_x64_context *src )
{
    dst->Rax = src->rax;
    dst->Rbx = src->rbx;
    dst->Rcx = src->rcx;
    dst->Rdx = src->rdx;
    dst->Rsi = src->rsi;
    dst->Rdi = src->rdi;
    dst->Rbp = src->rbp;
    dst->Rsp = src->rsp;
    dst->R8 = src->r8;
    dst->R9 = src->r9;
    dst->R10 = src->r10;
    dst->R11 = src->r11;
    dst->R12 = src->r12;
    dst->R13 = src->r13;
    dst->R14 = src->r14;
    dst->R15 = src->r15;
    dst->Rip = src->rip;
    dst->EFlags = src->eflags;
    dst->MxCsr = src->mxcsr;
    memcpy( &dst->Xmm0, src->xmm, sizeof(src->xmm) );
}

static void poison_provider( const char *operation, NTSTATUS status )
{
    struct xtajit64_poison_params params = { .status = status };

    ERR( "%s failed, poisoning x64 provider with status %#lx\n", operation, status );
    if (__wine_unixlib_handle) XTAJIT64_CALL( poison, &params );
}

static NTSTATUS get_allocation_base( const void *addr, ULONG_PTR *base )
{
    MEMORY_BASIC_INFORMATION info;
    NTSTATUS status;

    status = NtQueryVirtualMemory( GetCurrentProcess(), addr, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    if (!status && info.AllocationBase) *base = (ULONG_PTR)info.AllocationBase;
    else if (!status) status = STATUS_INVALID_ADDRESS;
    return status;
}

static NTSTATUS describe_host_mapping( ULONG_PTR host, SIZE_T size,
                                       ULONG_PTR allocation_base, ULONG protect,
                                       struct xtajit64_memory_params *params )
{
    WINE_TRANSLATED_VIEW_INFORMATION translated = {0};
    ULONG_PTR guest_base, host_base, offset;
    NTSTATUS status;

    C_ASSERT( sizeof(translated) == 48 );
    if (!params || !host || !size || host > ~(ULONG_PTR)0 - size ||
        !allocation_base)
        return STATUS_INVALID_ADDRESS;
    memset( params, 0, sizeof(*params) );
    status = NtQueryVirtualMemory( GetCurrentProcess(), (void *)host,
                                   MemoryWineTranslatedViewInformation,
                                   &translated, sizeof(translated), NULL );
    if (status) return status;
    if (translated.Version != WINE_TRANSLATED_VIEW_INFORMATION_VERSION ||
        translated.Reserved ||
        (translated.Flags & ~WINE_TRANSLATED_VIEW_AMD64_LOW))
        return STATUS_REVISION_MISMATCH;
    if (translated.Flags & WINE_TRANSLATED_VIEW_AMD64_LOW)
        return STATUS_ACCESS_DENIED;  /* owned by the native LOW observer */

    guest_base = (ULONG_PTR)translated.GuestBase;
    host_base = (ULONG_PTR)translated.HostBase;
    if (!guest_base || guest_base != host_base || !translated.RegionSize ||
        host < host_base || (offset = host - host_base) >= translated.RegionSize ||
        size > translated.RegionSize - offset ||
        (ULONG_PTR)translated.AllocationBase != allocation_base)
        return STATUS_INVALID_ADDRESS;

    params->guest = guest_base + offset;
    params->host = host;
    params->size = size;
    params->allocation_base = allocation_base;
    params->protect = protect;
    return STATUS_SUCCESS;
}

static NTSTATUS synchronize_transition_state_mapping( struct xtajit64_thread_state *state,
                                                       BOOL *provider_touched )
{
    struct xtajit64_memory_params params;
    ULONG_PTR allocation_base;
    NTSTATUS status;

    if (!provider_touched) return STATUS_INVALID_PARAMETER;
    *provider_touched = FALSE;
    if (!state || !state->allocation_size ||
        (ULONG_PTR)state > ~(ULONG_PTR)0 - state->allocation_size)
        return STATUS_INVALID_ADDRESS;
    if ((status = get_allocation_base( state, &allocation_base ))) return status;
    if ((status = describe_host_mapping( (ULONG_PTR)state, state->allocation_size,
                                          allocation_base, PAGE_READWRITE, &params )))
        return status;
    *provider_touched = TRUE;
    return XTAJIT64_CALL( memory_map, &params );
}

static NTSTATUS unregister_transition_state_mapping( struct xtajit64_thread_state *state )
{
    struct xtajit64_memory_params params;

    if (!state || !state->allocation_size ||
        (ULONG_PTR)state > ~(ULONG_PTR)0 - state->allocation_size)
        return STATUS_INVALID_ADDRESS;
    memset( &params, 0, sizeof(params) );
    params.guest = (ULONG_PTR)state;
    params.host = (ULONG_PTR)state;
    params.size = state->allocation_size;
    return XTAJIT64_CALL( memory_unmap, &params );
}

struct mapping_snapshot
{
    struct xtajit64_memory_params *ranges;
    ULONG count;
    ULONG capacity;
};

static RTL_SRWLOCK resync_snapshot_lock = RTL_SRWLOCK_INIT;
static struct mapping_snapshot resync_snapshot;

static void free_mapping_snapshot( struct mapping_snapshot *snapshot )
{
    if (snapshot->ranges)
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, snapshot->ranges );
    memset( snapshot, 0, sizeof(*snapshot) );
}

static NTSTATUS append_mapping_snapshot( struct mapping_snapshot *snapshot,
                                         const struct xtajit64_memory_params *params )
{
    struct xtajit64_memory_params *ranges;
    ULONG capacity;
    SIZE_T size;

    if (snapshot->count)
    {
        struct xtajit64_memory_params *last = &snapshot->ranges[snapshot->count - 1];

        if (last->guest + last->size == params->guest &&
            last->host + last->size == params->host &&
            last->allocation_base == params->allocation_base &&
            last->protect == params->protect && last->size <= ~(UINT64)0 - params->size)
        {
            last->size += params->size;
            return STATUS_SUCCESS;
        }
    }
    if (snapshot->count < snapshot->capacity)
    {
        snapshot->ranges[snapshot->count++] = *params;
        return STATUS_SUCCESS;
    }
    if (snapshot->capacity >= (1u << 20)) return STATUS_INSUFFICIENT_RESOURCES;
    capacity = snapshot->capacity ? snapshot->capacity * 2 : 256;
    if (capacity > (1u << 20)) capacity = 1u << 20;
    size = (SIZE_T)capacity * sizeof(*ranges);
    if (snapshot->ranges)
        ranges = RtlReAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                    snapshot->ranges, size );
    else
        ranges = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, size );
    if (!ranges) return STATUS_NO_MEMORY;
    snapshot->ranges = ranges;
    snapshot->capacity = capacity;
    snapshot->ranges[snapshot->count++] = *params;
    return STATUS_SUCCESS;
}

static NTSTATUS collect_existing_mappings( ULONG_PTR lowest, ULONG_PTR highest,
                                           ULONG page_size,
                                           struct mapping_snapshot *snapshot )
{
    ULONG_PTR cursor = max( lowest, (ULONG_PTR)XTAJIT64_GUEST_PAGE_SIZE );
    ULONG_PTR window_end;

    if (!snapshot || highest < cursor || highest == ~(ULONG_PTR)0)
        return STATUS_INVALID_ADDRESS;
    window_end = highest + 1;

    while (cursor < window_end)
    {
        struct xtajit64_memory_params params = {0};
        MEMORY_BASIC_INFORMATION info;
        ULONG_PTR region_base, region_end, start, end;
        NTSTATUS status;

        status = NtQueryVirtualMemory( GetCurrentProcess(), (void *)cursor,
                                       MemoryBasicInformation, &info, sizeof(info), NULL );
        if (status) return status;
        region_base = (ULONG_PTR)info.BaseAddress;
        if (!info.RegionSize || region_base > ~(ULONG_PTR)0 - info.RegionSize)
            return STATUS_INVALID_ADDRESS;
        region_end = region_base + info.RegionSize;
        if (region_end <= cursor) return STATUS_INVALID_ADDRESS;
        start = max( cursor, region_base );
        end = min( region_end, window_end );

        if (info.State == MEM_COMMIT && info.AllocationBase && end > start)
        {
            if ((start | end) & (XTAJIT64_GUEST_PAGE_SIZE - 1))
                return STATUS_INVALID_ADDRESS;
            status = describe_host_mapping( start, end - start,
                                            (ULONG_PTR)info.AllocationBase,
                                            info.Protect, &params );
            /* The capability-negotiated low-memory observer is the sole
             * structural authority for fixed-low AMD64 mappings.  Legacy
             * snapshots retain identity views only and must not replay that
             * translated lane. */
            if (status == STATUS_ACCESS_DENIED)
            {
                cursor = region_end;
                continue;
            }
            if (status) return status;
            if (params.guest < XTAJIT64_GUEST_KUSER + page_size &&
                params.guest + params.size > XTAJIT64_GUEST_KUSER)
            {
                if (params.guest != XTAJIT64_GUEST_KUSER ||
                    params.size != page_size)
                    return STATUS_INVALID_ADDRESS;
                cursor = region_end;
                continue;  /* ProcessInit installed the explicit low mapping. */
            }
            if ((status = append_mapping_snapshot( snapshot, &params ))) return status;
        }
        cursor = region_end;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS resync_existing_mappings(void)
{
    struct xtajit64_memory_resync_params params = {0};
    struct xtajit64_memory_resync_begin_params begin;
    struct mapping_snapshot *snapshot = &resync_snapshot;
    SYSTEM_BASIC_INFORMATION info;
    NTSTATUS status;
    ULONG attempt;

    status = NtQuerySystemInformation( SystemBasicInformation, &info, sizeof(info), NULL );
    if (status) return status;
    if (info.PageSize < XTAJIT64_GUEST_PAGE_SIZE ||
        info.PageSize > XTAJIT64_MAX_HOST_PAGE_SIZE ||
        (info.PageSize & (info.PageSize - 1)))
        return STATUS_INVALID_PARAMETER;
    /* Keep the bounded snapshot allocation for reuse.  Allocating and freeing
     * it inside every authoritative pass changes the very address space being
     * scanned and can prevent the outer generation retry from converging. */
    RtlAcquireSRWLockExclusive( &resync_snapshot_lock );
    snapshot->count = 0;
    for (attempt = 0; attempt < XTAJIT64_MAX_RESYNC_ATTEMPTS; ++attempt)
    {
        /* A concurrent map/protect/unmap after this token makes the commit
         * return STATUS_RETRY instead of republishing a stale snapshot. */
        if ((status = XTAJIT64_CALL( memory_resync_begin, &begin ))) break;
        status = collect_existing_mappings( (ULONG_PTR)info.LowestUserAddress,
                                            (ULONG_PTR)info.HighestUserAddress,
                                            info.PageSize, snapshot );
        if (status) break;
        params.ranges = (ULONG_PTR)snapshot->ranges;
        params.generation = begin.generation;
        params.count = snapshot->count;
        status = XTAJIT64_CALL( memory_resync, &params );
        if (!status)
        {
            TRACE( "resynchronized %lu committed x64/native mapping runs through %p\n",
                   snapshot->count, info.HighestUserAddress );
            break;
        }
        snapshot->count = 0;
        if (status != STATUS_RETRY) break;
    }
    snapshot->count = 0;
    RtlReleaseSRWLockExclusive( &resync_snapshot_lock );
    return status;
}

static NTSTATUS synchronize_mapping_window( ULONG_PTR lowest, ULONG_PTR highest )
{
    struct mapping_snapshot snapshot = {0};
    NTSTATUS status;
    ULONG i;

    if (!host_page_size || !lowest || highest <= lowest)
        return STATUS_INVALID_ADDRESS;
    status = collect_existing_mappings( lowest, highest - 1, host_page_size, &snapshot );
    for (i = 0; !status && i < snapshot.count; ++i)
        status = XTAJIT64_CALL( memory_map, &snapshot.ranges[i] );
    /* A window containing only a fixed-low view is intentionally absent from
     * the legacy identity snapshot. */
    free_mapping_snapshot( &snapshot );
    return status;
}

static NTSTATUS get_current_thread_teb_window( ULONG_PTR *lowest, ULONG_PTR *highest,
                                               UINT64 *allocation_base )
{
    MEMORY_BASIC_INFORMATION info;
    ULONG_PTR teb = (ULONG_PTR)NtCurrentTeb();
    ULONG_PTR region_start, region_end, teb_end, start, end;
    NTSTATUS status;

    if (!lowest || !highest || !allocation_base || !host_page_size ||
        teb > ~(ULONG_PTR)0 - sizeof(TEB))
        return STATUS_INVALID_ADDRESS;
    status = NtQueryVirtualMemory( GetCurrentProcess(), (void *)teb,
                                   MemoryBasicInformation, &info, sizeof(info), NULL );
    if (status) return status;
    region_start = (ULONG_PTR)info.BaseAddress;
    if (info.State != MEM_COMMIT || !info.AllocationBase || !info.RegionSize ||
        region_start > ~(ULONG_PTR)0 - info.RegionSize)
        return STATUS_INVALID_ADDRESS;
    region_end = region_start + info.RegionSize;
    teb_end = teb + sizeof(TEB);
    if (teb_end > ~(ULONG_PTR)0 - (host_page_size - 1)) return STATUS_INVALID_ADDRESS;
    start = teb & ~(ULONG_PTR)(host_page_size - 1);
    end = (teb_end + host_page_size - 1) & ~(ULONG_PTR)(host_page_size - 1);
    if (start < region_start || end > region_end || start >= end)
        return STATUS_INVALID_ADDRESS;
    *allocation_base = (ULONG_PTR)info.AllocationBase;
    *lowest = start;
    *highest = end;
    return STATUS_SUCCESS;
}

static NTSTATUS synchronize_current_thread_mappings(void)
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    ULONG_PTR native_limit = (ULONG_PTR)NtCurrentTeb()->Tib.StackLimit;
    ULONG_PTR native_base = (ULONG_PTR)NtCurrentTeb()->Tib.StackBase;
    ULONG_PTR teb_limit, teb_base;
    UINT64 teb_allocation;
    NTSTATUS status;

    if ((status = get_current_thread_teb_window( &teb_limit, &teb_base,
                                                  &teb_allocation )))
        return status;
    if ((status = synchronize_mapping_window( teb_limit, teb_base ))) return status;
    if ((status = synchronize_mapping_window( native_limit, native_base ))) return status;
    if (cpu && cpu->EmulatorStackLimit && cpu->EmulatorStackBase > cpu->EmulatorStackLimit &&
        (cpu->EmulatorStackLimit < native_limit || cpu->EmulatorStackBase > native_base))
        status = synchronize_mapping_window( cpu->EmulatorStackLimit,
                                              cpu->EmulatorStackBase );
    return status;
}

static void unregister_thread_teb_window( ULONG_PTR lowest, ULONG_PTR highest )
{
    struct xtajit64_memory_params params =
    {
        .guest = lowest,
        .size = highest - lowest,
    };
    NTSTATUS status;

    if (!lowest || highest <= lowest || !__wine_unixlib_handle) return;
    if ((status = XTAJIT64_CALL( memory_unmap, &params )))
        poison_provider( "thread-TEB unregister", status );
}

static void unregister_thread_stack_allocation( ULONG_PTR allocation_base )
{
    struct xtajit64_memory_params params = { .guest = allocation_base };
    NTSTATUS status;

    if (!allocation_base || !host_page_size || !__wine_unixlib_handle) return;
    if ((status = XTAJIT64_CALL( memory_unmap, &params )))
        poison_provider( "thread-stack unregister", status );
}

static void flush_unicorn_cache( const void *addr, SIZE_T size )
{
    struct xtajit64_memory_params params =
    {
        .guest = (ULONG_PTR)addr,
        .size = size,
    };
    NTSTATUS status;

    if (addr && size && (ULONG_PTR)addr > ~(ULONG_PTR)0 - size)
    {
        poison_provider( "instruction-cache range", STATUS_INVALID_ADDRESS );
        return;
    }
    if (__wine_unixlib_handle &&
        (status = XTAJIT64_CALL( flush_instruction_cache, &params )))
        poison_provider( "instruction-cache synchronization", status );
}

static struct xtajit64_thread_state *get_thread_state(void)
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct xtajit64_thread_state *state;

    if (!cpu || !(state = cpu->EmulatorData[0]) ||
        state->magic != XTAJIT64_THREAD_STATE_MAGIC)
        return NULL;
    return state;
}

static BOOL guest_range_to_host( UINT64 guest, SIZE_T size, ULONG required_access,
                                 ULONG_PTR *host )
{
    struct xtajit64_memory_translate_params params =
    {
        .address = guest,
        .size = size,
        .flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST | required_access,
    };

    if (!host || (required_access & ~(XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ |
                                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE |
                                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_EXECUTE)) ||
        !size || guest > XTAJIT64_X64_USER_ADDRESS_MAX ||
        size - 1 > XTAJIT64_X64_USER_ADDRESS_MAX - guest ||
        !__wine_unixlib_handle || XTAJIT64_CALL( memory_translate, &params ) ||
        params.guest != guest)
        return FALSE;
    *host = (ULONG_PTR)params.host;
    return TRUE;
}

static NTSTATUS read_guest_u64( UINT64 guest, UINT64 *value )
{
    ULONG_PTR host;
    SIZE_T read = 0;
    NTSTATUS status;

    if (!value || !guest_range_to_host( guest, sizeof(*value),
                                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ,
                                        &host ))
        return STATUS_ACCESS_VIOLATION;
    status = NtReadVirtualMemory( GetCurrentProcess(), (void *)host, value,
                                  sizeof(*value), &read );
    if (!status && read != sizeof(*value)) status = STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS write_guest_u64( UINT64 guest, UINT64 value )
{
    ULONG_PTR host;
    SIZE_T written = 0;
    NTSTATUS status;

    if (!guest_range_to_host( guest, sizeof(value),
                              XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE, &host ))
        return STATUS_ACCESS_VIOLATION;
    status = NtWriteVirtualMemory( GetCurrentProcess(), (void *)host, &value,
                                   sizeof(value), &written );
    if (!status && written != sizeof(value)) status = STATUS_PARTIAL_COPY;
    return status;
}

static BOOL get_x64_stack_bounds( UINT64 rsp, UINT64 *limit, UINT64 *base )
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    ULONG_PTR host_rsp;

    if (!cpu || !guest_range_to_host( rsp, sizeof(UINT64),
                                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ,
                                      &host_rsp )) return FALSE;
    if (host_rsp >= cpu->EmulatorStackLimit && host_rsp < cpu->EmulatorStackBase)
    {
        *limit = cpu->EmulatorStackLimit;
        *base = cpu->EmulatorStackBase;
        return TRUE;
    }
    if (host_rsp >= (ULONG_PTR)NtCurrentTeb()->Tib.StackLimit &&
        host_rsp < (ULONG_PTR)NtCurrentTeb()->Tib.StackBase)
    {
        *limit = (ULONG_PTR)NtCurrentTeb()->Tib.StackLimit;
        *base = (ULONG_PTR)NtCurrentTeb()->Tib.StackBase;
        return TRUE;
    }
    return FALSE;
}

static NTSTATUS resolve_ec_entry_thunk( UINT64 guest_target, ULONG_PTR *native_target,
                                        ULONG_PTR *entry )
{
    MEMORY_BASIC_INFORMATION target_info, entry_info;
    ULONG_PTR target, candidate;
    SIZE_T read = 0;
    UINT32 encoded;
    INT32 delta;
    NTSTATUS status;

    if (!native_target || !entry ||
        !guest_range_to_host( guest_target, sizeof(encoded),
                              XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ, &target ) ||
        target < sizeof(encoded) || !RtlIsEcCode( target ))
        return STATUS_INVALID_ADDRESS;
    status = NtQueryVirtualMemory( GetCurrentProcess(), (void *)target,
                                   MemoryBasicInformation, &target_info,
                                   sizeof(target_info), NULL );
    if (status) return status;
    if (target_info.State != MEM_COMMIT || !target_info.AllocationBase ||
        target - sizeof(encoded) < (ULONG_PTR)target_info.AllocationBase)
        return STATUS_INVALID_IMAGE_FORMAT;
    status = NtReadVirtualMemory( GetCurrentProcess(), (void *)(target - sizeof(encoded)),
                                  &encoded, sizeof(encoded), &read );
    if (status) return status;
    if (read != sizeof(encoded)) return STATUS_PARTIAL_COPY;

    /* Public ARM64EC ABI: the word immediately before an EC function is a
     * signed target-relative entry-thunk offset with two low flag bits. */
    delta = (INT32)(encoded & ~3u);
    if (!delta || (delta > 0 && target > ~(ULONG_PTR)0 - (UINT32)delta) ||
        (delta < 0 && target < (ULONG_PTR)-(INT64)delta))
        return STATUS_INVALID_IMAGE_FORMAT;
    candidate = target + delta;
    if ((candidate & 3) || !RtlIsEcCode( candidate )) return STATUS_INVALID_IMAGE_FORMAT;
    status = NtQueryVirtualMemory( GetCurrentProcess(), (void *)candidate,
                                   MemoryBasicInformation, &entry_info,
                                   sizeof(entry_info), NULL );
    if (status) return status;
    if (entry_info.State != MEM_COMMIT ||
        entry_info.AllocationBase != target_info.AllocationBase ||
        !((entry_info.Protect & 0xff) == PAGE_EXECUTE ||
          (entry_info.Protect & 0xff) == PAGE_EXECUTE_READ ||
          (entry_info.Protect & 0xff) == PAGE_EXECUTE_READWRITE ||
          (entry_info.Protect & 0xff) == PAGE_EXECUTE_WRITECOPY))
        return STATUS_INVALID_IMAGE_FORMAT;

    *native_target = target;
    *entry = candidate;
    return STATUS_SUCCESS;
}

static struct xtajit64_transition_frame *push_transition_frame(
    struct xtajit64_thread_state *state, enum xtajit64_transition_frame_kind kind )
{
    struct xtajit64_transition_frame *frame;

    if (!state || state->depth >= XTAJIT64_MAX_TRANSITION_DEPTH) return NULL;
    frame = &state->frames[state->depth++];
    memset( frame, 0, sizeof(*frame) );
    frame->kind = kind;
    return frame;
}

static NTSTATUS capture_fp_state( AMD64_CONTEXT *context )
{
    arm64x_get_information get_info = (arm64x_get_information)__os_arm64x_get_x64_information;
    UINT mxcsr;
    NTSTATUS status;

    if (!get_info) return STATUS_ENTRYPOINT_NOT_FOUND;
    if ((status = get_info( 0, &mxcsr, NULL ))) return status;
    context->MxCsr = mxcsr;
    context->FltSave.MxCsr = mxcsr;
    return STATUS_SUCCESS;
}

static NTSTATUS restore_fp_state( const AMD64_CONTEXT *context )
{
    arm64x_set_information set_info = (arm64x_set_information)__os_arm64x_set_x64_information;

    if (!set_info) return STATUS_ENTRYPOINT_NOT_FOUND;
    return set_info( 0, context->MxCsr, NULL );
}

static DECLSPEC_NORETURN void xtajit64_restore_native( ARM64EC_NT_CONTEXT *context );

static DECLSPEC_NORETURN void terminate_transition( NTSTATUS status )
{
    NtTerminateProcess( GetCurrentProcess(), status ? status : STATUS_NOT_SUPPORTED );
    RtlRaiseStatus( status ? status : STATUS_NOT_SUPPORTED );
}

static DECLSPEC_NORETURN void abort_transition( struct xtajit64_thread_state *state,
                                               NTSTATUS status, const char *reason )
{
    ERR( "%s, status %#lx transition %u depth %u\n", reason, status,
         state ? state->capture_kind : ~0u, state ? state->depth : 0 );
    terminate_transition( status );
}

static DECLSPEC_NORETURN void abort_simulation( struct xtajit64_thread_state *state,
                                               NTSTATUS status, UINT stop_reason,
                                               UINT unicorn_error )
{
    ERR( "unsupported x64 simulation boundary, status %#lx reason %u unicorn %u "
         "transition %u depth %u\n", status, stop_reason, unicorn_error,
         state ? state->capture_kind : ~0u, state ? state->depth : 0 );
    terminate_transition( status );
}

static DECLSPEC_NORETURN void run_x64_simulation( struct xtajit64_thread_state *state )
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    ARM64EC_NT_CONTEXT *ec_context;
    struct xtajit64_transition_frame *frame;
    struct xtajit64_begin_params params = {0};
    UINT64 guest_return;
    ULONG_PTR native_target, entry, host_rsp;
    NTSTATUS status;

    if (!state || state->magic != XTAJIT64_THREAD_STATE_MAGIC || !cpu ||
        !(ec_context = cpu->ContextAmd64))
        abort_transition( state, STATUS_INVALID_PARAMETER, "missing x64 transition state" );

    /* BeginSimulation and every native return/exit/jump converge here.  Do not
     * expose x64 execution until ntdll has committed any VM/cache resync that a
     * nested provider callback had to defer. */
    if ((status = __wine_arm64ec_prepare_x64_execution()))
        abort_transition( state, status, "cannot prepare deferred x64 mapping state" );

    context_to_unix( &params.context, &ec_context->AMD64_Context );
    params.gs_base = (ULONG_PTR)NtCurrentTeb();
    if (params.context.rip > XTAJIT64_X64_USER_ADDRESS_MAX ||
        params.context.rsp > XTAJIT64_X64_USER_ADDRESS_MAX ||
        params.gs_base > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !get_x64_stack_bounds( params.context.rsp, &params.stack_limit,
                               &params.stack_base ))
        abort_transition( state, STATUS_INVALID_ADDRESS, "invalid semantic x64 stack" );

    cpu->InSimulation = 1;
    status = XTAJIT64_CALL( begin_simulation, &params );
    context_from_unix( &ec_context->AMD64_Context, &params.context );
    if (status || params.stop_reason != XTAJIT64_STOP_EC_TRANSITION)
    {
        TRACE( "x64 simulation stopped fault %p target %p\n",
               (void *)(ULONG_PTR)params.fault_address,
               (void *)(ULONG_PTR)params.transition_target );
        abort_simulation( state, status ? status : STATUS_NOT_SUPPORTED,
                          params.stop_reason, params.unicorn_error );
    }

    /* A returning x64 callee reaches the exact ARM64EC instruction following
     * the exit thunk's helper call.  This is a continuation, not a callable
     * function entry and therefore must never be parsed as entry-thunk metadata. */
    if (state->depth &&
        (frame = &state->frames[state->depth - 1])->kind == XTAJIT64_FRAME_EXIT &&
        params.transition_target == frame->native_pc)
    {
        if (params.context.rsp != frame->guest_rsp)
            abort_transition( state, STATUS_BAD_STACK,
                              "x64 exit-thunk continuation stack mismatch" );
        --state->depth;
        ec_context->Sp = frame->native_sp;
        ec_context->Pc = frame->native_pc;
        ec_context->Lr = frame->native_pc;
        if ((status = restore_fp_state( &ec_context->AMD64_Context )))
            abort_transition( state, status, "cannot restore native FP state" );
        cpu->InSimulation = 0;
        TRACE( "return x64 target to EC continuation %p native sp %p depth %u\n",
               (void *)(ULONG_PTR)frame->native_pc,
               (void *)(ULONG_PTR)frame->native_sp, state->depth );
        xtajit64_restore_native( ec_context );
    }

    if (params.context.rsp > ~(UINT64)0 - sizeof(guest_return) ||
        (status = read_guest_u64( params.context.rsp, &guest_return )))
        abort_transition( state, status ? status : STATUS_BAD_STACK,
                          "cannot pop x64 return address for EC entry" );
    if (!guest_return ||
        (status = resolve_ec_entry_thunk( params.transition_target,
                                          &native_target, &entry )))
        abort_transition( state, status ? status : STATUS_INVALID_IMAGE_FORMAT,
                          "invalid ARM64EC entry-thunk metadata" );
    if (!guest_range_to_host( params.context.rsp + sizeof(guest_return),
                              sizeof(guest_return),
                              XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ, &host_rsp ))
        abort_transition( state, STATUS_BAD_STACK,
                          "cannot materialize EC entry stack" );
    if (!(frame = push_transition_frame( state, XTAJIT64_FRAME_ENTRY )))
        abort_transition( state, STATUS_STACK_OVERFLOW,
                          "ARM64EC transition nesting limit exceeded" );

    frame->guest_rsp = params.context.rsp + sizeof(guest_return);
    ec_context->X4 = host_rsp;  /* public entry-thunk ABI: original post-pop x64 RSP */
    ec_context->X9 = native_target;
    ec_context->Lr = guest_return;
    ec_context->Sp = host_rsp & ~(ULONG_PTR)15;
    ec_context->Pc = entry;
    if ((status = restore_fp_state( &ec_context->AMD64_Context )))
        abort_transition( state, status, "cannot restore EC entry FP state" );
    cpu->InSimulation = 0;
    TRACE( "enter EC target %p through compiler thunk %p x64 return %p "
           "guest rsp %p native sp %p depth %u\n",
           (void *)native_target, (void *)entry, (void *)(ULONG_PTR)guest_return,
           (void *)(ULONG_PTR)frame->guest_rsp, (void *)(ULONG_PTR)ec_context->Sp,
           state->depth );
    xtajit64_restore_native( ec_context );
}

static void __attribute__((used, noreturn)) xtajit64_transition_from_native(
    struct xtajit64_thread_state *state )
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    ARM64EC_NT_CONTEXT *ec_context;
    struct xtajit64_transition_frame *frame;
    UINT64 guest_sp, guest_target;
    NTSTATUS status;

    if (!state || state != get_thread_state() || !cpu || !(ec_context = cpu->ContextAmd64))
        abort_transition( state, STATUS_INVALID_PARAMETER, "invalid native capture state" );
    if ((status = capture_fp_state( &ec_context->AMD64_Context )))
        abort_transition( state, status, "cannot capture native FP state" );

    switch (state->capture_kind)
    {
    case XTAJIT64_NATIVE_RETURN:
        if (!state->depth ||
            (frame = &state->frames[state->depth - 1])->kind != XTAJIT64_FRAME_ENTRY ||
            !state->capture_lr || state->capture_lr > XTAJIT64_X64_USER_ADDRESS_MAX)
            abort_transition( state, STATUS_INVALID_UNWIND_TARGET,
                              "unmatched ARM64EC entry return" );
        ec_context->AMD64_Context.Rsp = frame->guest_rsp;
        ec_context->AMD64_Context.Rip = state->capture_lr;
        --state->depth;
        cpu->InSimulation = 1;
        TRACE( "return EC entry to x64 rip %p rsp %p depth %u\n",
               (void *)(ULONG_PTR)state->capture_lr,
               (void *)(ULONG_PTR)frame->guest_rsp, state->depth );
        run_x64_simulation( state );

    case XTAJIT64_NATIVE_EXIT:
        if (state->capture_sp < sizeof(UINT64) || !state->capture_lr ||
            !state->capture_target ||
            state->capture_sp > XTAJIT64_X64_USER_ADDRESS_MAX ||
            state->capture_lr > XTAJIT64_X64_USER_ADDRESS_MAX ||
            state->capture_target > XTAJIT64_X64_USER_ADDRESS_MAX)
            abort_transition( state, STATUS_INVALID_ADDRESS,
                              "invalid ARM64EC exit-thunk state" );
        guest_sp = state->capture_sp;
        guest_target = state->capture_target;
        if (!(frame = push_transition_frame( state, XTAJIT64_FRAME_EXIT )))
            abort_transition( state, STATUS_STACK_OVERFLOW,
                              "ARM64EC transition nesting limit exceeded" );
        frame->guest_rsp = guest_sp;
        frame->native_sp = state->capture_sp;
        frame->native_pc = state->capture_lr;
        if ((status = write_guest_u64( guest_sp - sizeof(UINT64), state->capture_lr )))
            abort_transition( state, status, "cannot push EC exit continuation" );
        ec_context->AMD64_Context.Rsp = guest_sp - sizeof(UINT64);
        ec_context->AMD64_Context.Rip = guest_target;
        cpu->InSimulation = 1;
        TRACE( "exit EC through compiler thunk to x64 target %p continuation %p "
               "guest rsp %p depth %u\n", (void *)(ULONG_PTR)guest_target,
               (void *)(ULONG_PTR)state->capture_lr,
               (void *)(ULONG_PTR)ec_context->AMD64_Context.Rsp, state->depth );
        run_x64_simulation( state );

    case XTAJIT64_NATIVE_JUMP:
        if (!state->depth ||
            (frame = &state->frames[state->depth - 1])->kind != XTAJIT64_FRAME_ENTRY ||
            !state->capture_lr || !state->capture_target ||
            state->capture_lr > XTAJIT64_X64_USER_ADDRESS_MAX ||
            state->capture_target > XTAJIT64_X64_USER_ADDRESS_MAX ||
            frame->guest_rsp < sizeof(UINT64))
            abort_transition( state, STATUS_INVALID_UNWIND_TARGET,
                              "invalid signature-less x64 jump" );
        guest_target = state->capture_target;
        if ((status = write_guest_u64( frame->guest_rsp - sizeof(UINT64),
                                       state->capture_lr )))
            abort_transition( state, status, "cannot restore tail-jump return address" );
        ec_context->AMD64_Context.Rsp = frame->guest_rsp - sizeof(UINT64);
        ec_context->AMD64_Context.Rip = guest_target;
        --state->depth;  /* the custom entry thunk is tail-forwarding */
        cpu->InSimulation = 1;
        TRACE( "tail-forward EC adjustor to x64 target %p return %p depth %u\n",
               (void *)(ULONG_PTR)guest_target, (void *)(ULONG_PTR)state->capture_lr,
               state->depth );
        run_x64_simulation( state );

    default:
        abort_transition( state, STATUS_INVALID_PARAMETER,
                          "unknown ARM64EC native transition" );
    }
}

/* Native ARM64EC entry and exit thunks use the register mapping encoded by
 * ARM64EC_NT_CONTEXT.  Capture every mapped integer/SIMD register before using
 * x16/x17 as scratch.  x13/x14/x23/x24/x28 are reserved by the public ABI and
 * are deliberately never touched here. */
static void __attribute__((used, naked)) xtajit64_capture_native(void)
{
    __asm__(
        "ldr x17, [x18, #0x1788]\n\t"       /* TEB.ChpeV2CpuAreaInfo */
        "cbz x17, 1f\n\t"
        "ldr x9, [x17, #0x30]\n\t"         /* cpu.EmulatorData[0] */
        "cmp x9, x16\n\t"
        "b.ne 1f\n\t"
        "ldr x17, [x17, #0x18]\n\t"        /* cpu.ContextAmd64 */
        "cbz x17, 1f\n\t"
        "stp x8, x0,   [x17, #0x78]\n\t"   /* Rax,Rcx */
        "stp x1, x27,  [x17, #0x88]\n\t"   /* Rdx,Rbx */
        "ldr x9, [x16, #0x18]\n\t"        /* captured native SP */
        "stp x9, x29,  [x17, #0x98]\n\t"   /* Rsp,Rbp */
        "stp x25, x26, [x17, #0xa8]\n\t"   /* Rsi,Rdi */
        "stp x2, x3,   [x17, #0xb8]\n\t"   /* R8,R9 */
        "stp x4, x5,   [x17, #0xc8]\n\t"   /* R10,R11 */
        "stp x19, x20, [x17, #0xd8]\n\t"   /* R12,R13 */
        "stp x21, x22, [x17, #0xe8]\n\t"   /* R14,R15 */
        "ldr x9, [x16, #0x20]\n\t"        /* captured LR */
        "str x9, [x17, #0xf8]\n\t"        /* Rip */
        "str x9, [x17, #0x120]\n\t"       /* hidden LR */
        "str x6, [x17, #0x130]\n\t"
        "str x7, [x17, #0x140]\n\t"
        "ldr x9, [x16, #0x28]\n\t"        /* captured target x9 */
        "str x9, [x17, #0x150]\n\t"
        "ldr x9, [x16, #0x30]\n\t"        /* captured x10 */
        "str x9, [x17, #0x160]\n\t"
        "str x11, [x17, #0x170]\n\t"
        "str x12, [x17, #0x180]\n\t"
        "str x15, [x17, #0x190]\n\t"
        "stp q0, q1,   [x17, #0x1a0]\n\t"
        "stp q2, q3,   [x17, #0x1c0]\n\t"
        "stp q4, q5,   [x17, #0x1e0]\n\t"
        "stp q6, q7,   [x17, #0x200]\n\t"
        "stp q8, q9,   [x17, #0x220]\n\t"
        "stp q10, q11, [x17, #0x240]\n\t"
        "stp q12, q13, [x17, #0x260]\n\t"
        "stp q14, q15, [x17, #0x280]\n\t"
        "ldr x17, [x16, #0x10]\n\t"       /* private control-stack top */
        "mov sp, x17\n\t"
        "mov x0, x16\n\t"
        "adr x30, 2f\n\t"
        "b \"#xtajit64_transition_from_native\"\n\t"
        "1: brk #0xf64\n\t"
        "2: brk #0xf65\n\t" );
}

static DECLSPEC_NORETURN void __attribute__((naked)) xtajit64_restore_native(
    ARM64EC_NT_CONTEXT *context )
{
    __asm__(
        "mov x16, x0\n\t"
        "ldr x17, [x16, #0xf8]\n\t"       /* native PC */
        "ldr x15, [x16, #0x98]\n\t"
        "mov sp, x15\n\t"
        "ldp q0, q1,   [x16, #0x1a0]\n\t"
        "ldp q2, q3,   [x16, #0x1c0]\n\t"
        "ldp q4, q5,   [x16, #0x1e0]\n\t"
        "ldp q6, q7,   [x16, #0x200]\n\t"
        "ldp q8, q9,   [x16, #0x220]\n\t"
        "ldp q10, q11, [x16, #0x240]\n\t"
        "ldp q12, q13, [x16, #0x260]\n\t"
        "ldp q14, q15, [x16, #0x280]\n\t"
        "ldr x30, [x16, #0x120]\n\t"
        "ldr x6,  [x16, #0x130]\n\t"
        "ldr x7,  [x16, #0x140]\n\t"
        "ldr x9,  [x16, #0x150]\n\t"
        "ldr x10, [x16, #0x160]\n\t"
        "ldr x11, [x16, #0x170]\n\t"
        "ldr x12, [x16, #0x180]\n\t"
        "ldr x15, [x16, #0x190]\n\t"
        "ldr x8,  [x16, #0x78]\n\t"
        "ldr x0,  [x16, #0x80]\n\t"
        "ldp x1, x27,  [x16, #0x88]\n\t"
        "ldr x29, [x16, #0xa0]\n\t"
        "ldp x25, x26, [x16, #0xa8]\n\t"
        "ldp x2, x3,   [x16, #0xb8]\n\t"
        "ldp x4, x5,   [x16, #0xc8]\n\t"
        "ldp x19, x20, [x16, #0xd8]\n\t"
        "ldp x21, x22, [x16, #0xe8]\n\t"
        "br x17\n\t" );
}

#endif /* HAVE_UNICORN */


/**********************************************************************
 *           DispatchJump  (xtajit64.@)
 *
 * Implementation of __os_arm64x_x64_jump.
 */
#ifdef HAVE_UNICORN

static void __attribute__((used, naked)) capture_transition(void)
{
    __asm__(
        "sub sp, sp, #16\n\t"
        "stp x16, x17, [sp]\n\t"          /* preserve transition kind */
        "add x16, sp, #16\n\t"            /* original native SP */
        "ldr x17, [x18, #0x1788]\n\t"      /* TEB.ChpeV2CpuAreaInfo */
        "cbz x17, 1f\n\t"
        "ldr x17, [x17, #0x30]\n\t"       /* cpu.EmulatorData[0] */
        "cbz x17, 1f\n\t"
        "str x17, [sp]\n\t"
        "ldr x17, [x17]\n\t"
        "movz x16, #0x5458\n\t"
        "movk x16, #0x4a41, lsl #16\n\t"
        "movk x16, #0x5449, lsl #32\n\t"
        "movk x16, #0x3634, lsl #48\n\t"
        "cmp x17, x16\n\t"
        "b.ne 1f\n\t"
        "ldr x17, [sp]\n\t"
        "add x16, sp, #16\n\t"            /* original native SP */
        "str x16, [x17, #0x18]\n\t"
        "str x30, [x17, #0x20]\n\t"
        "str x9,  [x17, #0x28]\n\t"
        "str x10, [x17, #0x30]\n\t"
        "ldr w16, [sp, #8]\n\t"
        "str w16, [x17, #0x38]\n\t"
        "add sp, sp, #16\n\t"
        "mov x16, x17\n\t"               /* state for capture common */
        "b \"#xtajit64_capture_native\"\n\t"
        "1: brk #0xf66\n\t" );
}

void WINAPI __attribute__((naked)) DispatchJump(void)
{
    __asm__( "mov w17, #2\n\tb \"#capture_transition\"\n\t" );
}


/**********************************************************************
 *           RetToEntryThunk  (xtajit64.@)
 *
 * Implementation of __os_arm64x_dispatch_ret.
 */
void WINAPI __attribute__((naked)) RetToEntryThunk(void)
{
    __asm__( "mov w17, #0\n\tb \"#capture_transition\"\n\t" );
}


/**********************************************************************
 *           ExitToX64  (xtajit64.@)
 *
 * Implementation of __os_arm64x_dispatch_call_no_redirect.
 */
void WINAPI __attribute__((naked)) ExitToX64(void)
{
    __asm__( "mov w17, #1\n\tb \"#capture_transition\"\n\t" );
}

#else

void WINAPI DispatchJump(void)
{
    ERR( "x64 emulation not implemented\n" );
    NtTerminateProcess( GetCurrentProcess(), 1 );
}

void WINAPI RetToEntryThunk(void)
{
    ERR( "x64 emulation not implemented\n" );
    NtTerminateProcess( GetCurrentProcess(), 1 );
}

void WINAPI ExitToX64(void)
{
    ERR( "x64 emulation not implemented\n" );
    NtTerminateProcess( GetCurrentProcess(), 1 );
}

#endif


/**********************************************************************
 *           BeginSimulation  (xtajit64.@)
 */
void WINAPI BeginSimulation(void)
{
#ifdef HAVE_UNICORN
    struct xtajit64_thread_state *state = get_thread_state();

    if (!state) RtlRaiseStatus( STATUS_INVALID_PARAMETER );
    run_x64_simulation( state );
#else
    ERR( "x64 emulation not implemented\n" );
    NtTerminateProcess( GetCurrentProcess(), 1 );
#endif
}


/**********************************************************************
 *           BTCpu64FlushInstructionCache  (xtajit64.@)
 */
void WINAPI BTCpu64FlushInstructionCache( void *addr, SIZE_T size )
{
    TRACE( "%p %Ix\n", addr, size );
#ifdef HAVE_UNICORN
    flush_unicorn_cache( addr, size );
#endif
}


/**********************************************************************
 *           BTCpu64IsProcessorFeaturePresent  (xtajit64.@)
 */
BOOLEAN WINAPI BTCpu64IsProcessorFeaturePresent( UINT feature )
{
    static const ULONGLONG x86_features =
        (1ull << PF_COMPARE_EXCHANGE_DOUBLE) |
        (1ull << PF_MMX_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_XMMI_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_RDTSC_INSTRUCTION_AVAILABLE) |
        (1ull << PF_XMMI64_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_NX_ENABLED) |
        (1ull << PF_SSE3_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_COMPARE_EXCHANGE128) |
        (1ull << PF_FASTFAIL_AVAILABLE) |
        (1ull << PF_RDTSCP_INSTRUCTION_AVAILABLE) |
        (1ull << PF_SSSE3_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_SSE4_1_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_SSE4_2_INSTRUCTIONS_AVAILABLE);

    return feature < 64 && (x86_features & (1ull << feature));
}


/**********************************************************************
 *           BTCpu64NotifyMemoryDirty  (xtajit64.@)
 */
void WINAPI BTCpu64NotifyMemoryDirty( void *addr, SIZE_T size )
{
    TRACE( "%p %Ix\n", addr, size );
#ifdef HAVE_UNICORN
    flush_unicorn_cache( addr, size );
#endif
}


/**********************************************************************
 *           BTCpu64NotifyReadFile  (xtajit64.@)
 */
void WINAPI BTCpu64NotifyReadFile( HANDLE handle, void *addr, SIZE_T size, BOOL is_post, NTSTATUS status )
{
    (void)is_post;
    (void)status;
    TRACE( "%p %p %Ix\n", handle, addr, size );
#ifdef HAVE_UNICORN
    if (is_post && !status) flush_unicorn_cache( addr, size );
#endif
}


/**********************************************************************
 *           FlushInstructionCacheHeavy  (xtajit64.@)
 */
void WINAPI FlushInstructionCacheHeavy( void *addr, SIZE_T size )
{
    TRACE( "%p %Ix\n", addr, size );
#ifdef HAVE_UNICORN
    flush_unicorn_cache( addr, size );
#endif
}


/**********************************************************************
 *           ResyncIdentityMemoryMappingsStatus  (xtajit64.@)
 *
 * Status-returning initialization barrier for the target-owned cross-process
 * work list.  Keep this separate from the legacy void callback below so a
 * mismatched older provider cannot be mistaken for a successful resync.
 */
NTSTATUS WINAPI ResyncIdentityMemoryMappingsStatus(void)
{
#ifdef HAVE_UNICORN
    return resync_existing_mappings();
#else
    return STATUS_SUCCESS;
#endif
}


/**********************************************************************
 *           ResyncIdentityMemoryMappings  (xtajit64.@)
 *
 * Rebuild the legacy identity lane from the target process after an external
 * address-space mutation.  The Unix provider preserves observer-owned LOW
 * ranges while atomically replacing identity state.
 */
void WINAPI ResyncIdentityMemoryMappings(void)
{
#ifdef HAVE_UNICORN
    NTSTATUS status;

    if ((status = ResyncIdentityMemoryMappingsStatus()))
        poison_provider( "authoritative identity mapping resynchronization", status );
#endif
}


/**********************************************************************
 *           NotifyMapViewOfSection  (xtajit64.@)
 */
NTSTATUS WINAPI NotifyMapViewOfSection( void *unk1, void *addr, void *unk2, SIZE_T size,
                                        ULONG alloc_type, ULONG protect )
{
#ifdef HAVE_UNICORN
    ULONG_PTR lowest = (ULONG_PTR)addr;
    NTSTATUS status;

    if (!lowest || !size || lowest > ~(ULONG_PTR)0 - size)
    {
        status = STATUS_INVALID_ADDRESS;
        poison_provider( "mapped-view range validation", status );
    }
    else if ((status = synchronize_mapping_window( lowest, lowest + size )))
    {
        /* The LOW lane is owned by the native observer.  A stale legacy
         * callback that intersects it is a validation-only no-op. */
        if (status == STATUS_ACCESS_DENIED) status = STATUS_SUCCESS;
        else poison_provider( "mapped-view synchronization", status );
    }
#endif

    (void)unk1;
    (void)unk2;
    TRACE( "%p %Ix %lx %lx\n", addr, size, alloc_type, protect );
#ifdef HAVE_UNICORN
    return status;
#else
    return STATUS_SUCCESS;
#endif
}


/**********************************************************************
 *           NotifyMemoryAlloc  (xtajit64.@)
 */
void WINAPI NotifyMemoryAlloc( void *addr, SIZE_T size, ULONG type, ULONG prot, BOOL is_post, NTSTATUS status )
{
    (void)type;
    (void)prot;
    (void)is_post;
    (void)status;
    TRACE( "%p %Ix\n", addr, size );
#ifdef HAVE_UNICORN
    if (is_post && !status && (type & (MEM_RESERVE | MEM_COMMIT)))
    {
        struct xtajit64_memory_params params;
        ULONG_PTR allocation_base;

        if ((status = get_allocation_base( addr, &allocation_base )))
            poison_provider( "allocation-base query", status );
        else if ((status = describe_host_mapping( (ULONG_PTR)addr, size,
                                                   allocation_base,
                                                   (type & MEM_COMMIT) ? prot : PAGE_NOACCESS,
                                                   &params )))
        {
            if (status != STATUS_ACCESS_DENIED)
                poison_provider( "allocation address translation", status );
        }
        else if ((status = XTAJIT64_CALL( memory_map, &params )) &&
                 status != STATUS_ACCESS_DENIED)
            poison_provider( "allocation synchronization", status );
    }
#endif
}


/**********************************************************************
 *           NotifyMemoryFree  (xtajit64.@)
 */
void WINAPI NotifyMemoryFree( void *addr, SIZE_T size, ULONG type, BOOL is_post, NTSTATUS status )
{
    (void)is_post;
    (void)status;
    TRACE( "%p %Ix %lx\n", addr, size, type );
#ifdef HAVE_UNICORN
    if (is_post && !status)
    {
        NTSTATUS sync_status;
        struct xtajit64_memory_params params =
        {
            .guest = (ULONG_PTR)addr,
            .host = (ULONG_PTR)addr,
            .size = size,
            .protect = type,
        };

        sync_status = XTAJIT64_CALL( memory_unmap, &params );
        if (sync_status && sync_status != STATUS_ACCESS_DENIED)
            poison_provider( "allocation-free synchronization", sync_status );
    }
#endif
}


/**********************************************************************
 *           NotifyMemoryProtect  (xtajit64.@)
 */
void WINAPI NotifyMemoryProtect( void *addr, SIZE_T size, ULONG prot, BOOL is_post, NTSTATUS status )
{
    (void)is_post;
    (void)status;
    TRACE( "%p %Ix %lx\n", addr, size, prot );
#ifdef HAVE_UNICORN
    if (is_post && !status)
    {
        NTSTATUS sync_status;
        struct xtajit64_memory_params params =
        {
            .guest = (ULONG_PTR)addr,
            .host = (ULONG_PTR)addr,
            .size = size,
            .protect = prot,
        };

        sync_status = XTAJIT64_CALL( memory_protect, &params );
        if (sync_status && sync_status != STATUS_ACCESS_DENIED)
            poison_provider( "memory-protection synchronization", sync_status );
    }
#endif
}


/**********************************************************************
 *           NotifyUnmapViewOfSection  (xtajit64.@)
 */
void WINAPI NotifyUnmapViewOfSection( void *addr, BOOL is_post, NTSTATUS status )
{
    (void)is_post;
    (void)status;
    TRACE( "%p\n", addr );
#ifdef HAVE_UNICORN
    if (is_post && !status)
    {
        NTSTATUS sync_status;
        struct xtajit64_memory_params params =
        {
            .guest = (ULONG_PTR)addr,
            .host = (ULONG_PTR)addr,
        };

        sync_status = XTAJIT64_CALL( memory_unmap, &params );
        if (sync_status && sync_status != STATUS_ACCESS_DENIED)
            poison_provider( "mapped-view unmap synchronization", sync_status );
    }
#endif
}


/**********************************************************************
 *           ProcessInit  (xtajit64.@)
 */
NTSTATUS WINAPI ProcessInit(void)
{
#ifdef HAVE_UNICORN
    SYSTEM_BASIC_INFORMATION info;
    struct xtajit64_process_init_params params = {0};
    UNICODE_STRING ntdll_name = RTL_CONSTANT_STRING( L"ntdll.dll" );
    HMODULE ntdll;
    ULONG syscall_count;
    ULONG_PTR syscall_dispatcher;
    ULONG_PTR shared_data;
    NTSTATUS status;

    if ((status = init_unixlib())) return status;
    status = NtQuerySystemInformation( SystemBasicInformation, &info, sizeof(info), NULL );
    if (status) return status;
    if (info.PageSize < XTAJIT64_GUEST_PAGE_SIZE ||
        info.PageSize > XTAJIT64_MAX_HOST_PAGE_SIZE ||
        (info.PageSize & (info.PageSize - 1)))
        return STATUS_INVALID_PARAMETER;
    shared_data = (ULONG_PTR)NtCurrentTeb()->Peb->SharedData;
    if (!shared_data || (shared_data & (info.PageSize - 1)) ||
        (XTAJIT64_GUEST_KUSER & (info.PageSize - 1)))
        return STATUS_INVALID_ADDRESS;
    if ((status = LdrGetDllHandle( NULL, 0, &ntdll_name, &ntdll ))) return status;
    rtl_exit_user_thread = (ULONG_PTR)resolve_arm64ec_export( ntdll, "RtlExitUserThread" );
    if (!rtl_exit_user_thread) return STATUS_ENTRYPOINT_NOT_FOUND;
    if ((status = __wine_arm64ec_get_x64_syscall_dispatcher( &syscall_dispatcher,
                                                             &syscall_count )))
        return status;

    params.ec_bitmap = (ULONG_PTR)NtCurrentTeb()->Peb->EcCodeBitMap;
    params.highest_user_address = (ULONG_PTR)info.HighestUserAddress;
    params.guest_kuser = XTAJIT64_GUEST_KUSER;
    params.host_kuser = shared_data;
    params.kuser_size = info.PageSize;
    params.rtl_exit_user_thread = rtl_exit_user_thread;
    params.abi_version = XTAJIT64_PROCESS_ABI_VERSION;
    params.abi_size = sizeof(params);
    params.required_capabilities = XTAJIT64_CAPABILITIES;
    params.x64_syscall_dispatcher = syscall_dispatcher;
    params.x64_syscall_count = syscall_count;
    if ((status = XTAJIT64_CALL( process_init, &params ))) return status;
    if ((params.enabled_capabilities & params.required_capabilities) !=
            params.required_capabilities ||
        (params.enabled_capabilities & ~XTAJIT64_CAPABILITIES))
    {
        XTAJIT64_CALL( process_term, NULL );
        rtl_exit_user_thread = 0;
        return STATUS_REVISION_MISMATCH;
    }
    host_page_size = info.PageSize;
    status = resync_existing_mappings();
    if (status)
    {
        XTAJIT64_CALL( process_term, NULL );
        rtl_exit_user_thread = 0;
        host_page_size = 0;
    }
    return status;
#else
    return STATUS_SUCCESS;
#endif
}


/**********************************************************************
 *           ProcessTerm  (xtajit64.@)
 */
void WINAPI ProcessTerm( HANDLE handle, BOOL is_post, NTSTATUS status )
{
    (void)xtajit64_process_term_notification_may_cleanup( (UINT_PTR)handle,
                                                          is_post, status );
    TRACE( "soft process termination notification handle %p post %u status %#lx\n",
           handle, is_post, status );
}


/**********************************************************************
 *           ResetToConsistentState  (xtajit64.@)
 */
void WINAPI ResetToConsistentState( EXCEPTION_RECORD *rec, CONTEXT *context, ARM64_NT_CONTEXT *arm_ctx )
{
    TRACE( "%p %p %p\n", rec, context, arm_ctx );
}


/**********************************************************************
 *           ThreadInit  (xtajit64.@)
 */
NTSTATUS WINAPI ThreadInit(void)
{
#ifdef HAVE_UNICORN
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct xtajit64_thread_state *state;
    NTSTATUS cleanup_status, status;
    BOOL provider_touched;

    if (!cpu || !cpu->ContextAmd64) return STATUS_INVALID_PARAMETER;
    if ((state = cpu->EmulatorData[0]))
        return state->magic == XTAJIT64_THREAD_STATE_MAGIC ? STATUS_SUCCESS :
                                                            STATUS_ALREADY_INITIALIZED;
    if ((status = synchronize_current_thread_mappings()))
    {
        poison_provider( "thread mapping synchronization", status );
        return status;
    }
    if ((status = allocate_transition_state( &state ))) return status;
    /* Publish this known uniform allocation directly while ntdll defers its
     * nested VM notification.  Ntdll acknowledges the exact single mutation;
     * any additional or concurrent mutation retains the full-resync fallback. */
    if ((status = synchronize_transition_state_mapping( state, &provider_touched )))
    {
        if (!provider_touched) free_transition_state( state );
        else poison_provider( "transition-state synchronization", status );
        return status;
    }
    if ((status = XTAJIT64_CALL( thread_init, NULL )))
    {
        cleanup_status = unregister_transition_state_mapping( state );
        if (!cleanup_status) cleanup_status = free_transition_state( state );
        if (cleanup_status) poison_provider( "transition-state cleanup", cleanup_status );
        return status;
    }

    cpu->EmulatorData[0] = state;
    return STATUS_SUCCESS;
#else
    return STATUS_SUCCESS;
#endif
}


/**********************************************************************
 *           ThreadTerm  (xtajit64.@)
 */
void WINAPI ThreadTerm( HANDLE handle, LONG exit_code )
{
#ifdef HAVE_UNICORN
    struct xtajit64_thread_state *state = NULL;
    CHPE_V2_CPU_AREA_INFO *cpu;
    ULONG_PTR native_stack_allocation = 0, emulator_stack_allocation = 0;
    ULONG_PTR teb_limit = 0, teb_base = 0;
    UINT64 teb_allocation = 0;
    NTSTATUS status;
#endif

    TRACE( "%p %lx\n", handle, exit_code );
#ifdef HAVE_UNICORN

    if (!RtlIsCurrentThread( handle )) return;
    get_current_thread_teb_window( &teb_limit, &teb_base, &teb_allocation );
    get_allocation_base( &state, &native_stack_allocation );
    if ((cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo) && cpu->EmulatorStackBase > cpu->EmulatorStackLimit)
        get_allocation_base( (void *)(cpu->EmulatorStackBase - 1),
                             &emulator_stack_allocation );
    if (__wine_unixlib_handle) XTAJIT64_CALL( thread_term, NULL );
    if (teb_allocation != native_stack_allocation &&
        teb_allocation != emulator_stack_allocation)
        unregister_thread_teb_window( teb_limit, teb_base );
    unregister_thread_stack_allocation( native_stack_allocation );
    if (emulator_stack_allocation != native_stack_allocation)
        unregister_thread_stack_allocation( emulator_stack_allocation );
    if (!cpu || !(state = get_thread_state())) return;
    if ((status = unregister_transition_state_mapping( state )))
    {
        poison_provider( "transition-state unregister", status );
        return;
    }
    if (state->allocation_size <= ~(ULONG_PTR)0 - (ULONG_PTR)state &&
        (ULONG_PTR)&state >= (ULONG_PTR)state &&
        (ULONG_PTR)&state < (ULONG_PTR)state + state->allocation_size)
    {
        cpu->EmulatorData[0] = NULL;
        state->magic = 0;
        ERR( "cannot release x64 transition state while running on its control stack\n" );
    }
    else
    {
        cpu->EmulatorData[0] = NULL;
        state->magic = 0;
        if ((status = free_transition_state( state )))
        {
            state->magic = XTAJIT64_THREAD_STATE_MAGIC;
            cpu->EmulatorData[0] = state;
            poison_provider( "transition-state release", status );
        }
    }
#endif
}


/**********************************************************************
 *           UpdateProcessorInformation  (xtajit64.@)
 */
void WINAPI UpdateProcessorInformation( SYSTEM_CPU_INFORMATION *info )
{
    info->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    info->ProcessorLevel = 21;
    info->ProcessorRevision = 1;
}


/**********************************************************************
 *           DllMain
 */
BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        LdrDisableThreadCalloutsForDll( inst );
#ifdef HAVE_UNICORN
        if (init_unixlib()) return FALSE;
#endif
    }
    return TRUE;
}
