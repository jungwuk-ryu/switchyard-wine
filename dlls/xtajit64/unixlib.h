/*
 * x86-64 emulation on ARM64 Unix interface
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_XTAJIT64_UNIXLIB_H
#define __WINE_XTAJIT64_UNIXLIB_H

#include "windef.h"
#include "winnt.h"
#include "wine/low_va.h"
#include "wine/unixlib.h"
#include "flight_recorder.h"

#define XTAJIT64_GUEST_PAGE_SIZE      0x1000
#define XTAJIT64_MAX_HOST_PAGE_SIZE   0x10000
#define XTAJIT64_CONTROL_STACK_SIZE   0x40000
#define XTAJIT64_GUEST_KUSER          WINE_USER_SHARED_DATA_ADDRESS
#define XTAJIT64_X64_USER_ADDRESS_MAX 0x00007fffffffffffull
#define XTAJIT64_X64_SYSCALL_NT_READ_VIRTUAL_MEMORY 0x003fu
#define XTAJIT64_PROCESS_ABI_VERSION          19u
#define XTAJIT64_ARM64_HOST_CONTEXT_VERSION    1u
#define XTAJIT64_PROCESS_INIT_PARAMS_SIZE     104u
#define XTAJIT64_BEGIN_PARAMS_SIZE            472u
#define XTAJIT64_DISPATCH_PARAMS_SIZE         512u
#define XTAJIT64_RECONSTRUCT_PARAMS_SIZE       1248u
#define XTAJIT64_PROVIDER_ABI_IDENTITY \
    "switchyard-xtajit64-fex-provider-abi-v19-flight-bind-process-init-104-begin-472-doorbell-reconstruct-1248-jit-signal-stack-query-exec-range-dispatch-512-direct-64-native-stack-protect"

#define XTAJIT64_DIRECT_CAPSULE_VERSION 1u

struct xtajit64_direct_capsule
{
    UINT32 size;
    UINT32 version;
    UINT64 bridge;
    UINT64 prepare;
    UINT64 complete;
    UINT64 authenticated_teb;
    UINT64 binding_id;
    UINT64 process_instance;
    UINT64 reserved;
};

C_ASSERT( sizeof(struct xtajit64_direct_capsule) == 64 );
C_ASSERT( offsetof(struct xtajit64_direct_capsule, bridge) == 8 );
C_ASSERT( offsetof(struct xtajit64_direct_capsule, authenticated_teb) == 32 );

#define XTAJIT64_PROCESS_CUSTOM_DISPATCH 0x00000001u

#define XTAJIT64_CAP_GS_NATIVE_DOMAIN 0x00000001u
#define XTAJIT64_CAP_ADDRESS_CODEC    0x00000002u
#define XTAJIT64_CAP_MUTATION_CODEC   0x00000004u
#define XTAJIT64_CAP_SUSPEND_DOORBELL 0x00000008u
#define XTAJIT64_CAP_UNALIGNED_TSO_REPAIR 0x00000010u
#define XTAJIT64_CAP_CALLRET_REPAIR    0x00000020u
#define XTAJIT64_CAP_EXCEPTION_STACK  0x00000040u
#define XTAJIT64_CAP_EXECUTABLE_RANGE 0x00000080u
#define XTAJIT64_CAPABILITIES         (XTAJIT64_CAP_GS_NATIVE_DOMAIN | \
                                       XTAJIT64_CAP_ADDRESS_CODEC | \
                                       XTAJIT64_CAP_MUTATION_CODEC | \
                                       XTAJIT64_CAP_SUSPEND_DOORBELL | \
                                       XTAJIT64_CAP_UNALIGNED_TSO_REPAIR | \
                                       XTAJIT64_CAP_CALLRET_REPAIR | \
                                       XTAJIT64_CAP_EXCEPTION_STACK | \
                                       XTAJIT64_CAP_EXECUTABLE_RANGE)

#define XTAJIT64_MEMORY_VALID_FLAGS   0u

#define XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST 0x00000001u
#define XTAJIT64_MEMORY_TRANSLATE_HOST_TO_GUEST 0x00000002u
#define XTAJIT64_MEMORY_TRANSLATE_DIRECTION_MASK \
    (XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST | XTAJIT64_MEMORY_TRANSLATE_HOST_TO_GUEST)
#define XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ    0x00000004u
#define XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE   0x00000008u
#define XTAJIT64_MEMORY_TRANSLATE_REQUIRE_EXECUTE 0x00000010u
#define XTAJIT64_MEMORY_TRANSLATE_VALID_FLAGS \
    (XTAJIT64_MEMORY_TRANSLATE_DIRECTION_MASK | \
     XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ | \
     XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE | \
     XTAJIT64_MEMORY_TRANSLATE_REQUIRE_EXECUTE)

/* Returned translation domains are part of the PE/Unix validation contract.
 * PE fast paths may dereference an address only when the provider explicitly
 * authenticates the identity lane. */
enum xtajit64_memory_domain
{
    XTAJIT64_MEMORY_ADDRESS_INVALID,
    XTAJIT64_MEMORY_ADDRESS_IDENTITY,
    XTAJIT64_MEMORY_ADDRESS_AMD64_LOW,
};

static inline BOOL xtajit64_process_term_notification_may_cleanup(
    UINT_PTR handle, BOOL is_post, NTSTATUS status )
{
    (void)handle;
    (void)is_post;
    (void)status;

    /* Wine sends both callbacks around a soft NtTerminateProcess(NULL) that
     * must return for guest LdrShutdownProcess.  Neither notification is a
     * quiescent teardown boundary; the OS reclaims this process-lifetime
     * provider after the terminal process exit. */
    return FALSE;
}

enum xtajit64_unix_funcs
{
    unix_process_init,
    unix_process_term,
    unix_thread_init,
    unix_thread_term,
    unix_memory_map,
    unix_memory_unmap,
    unix_memory_protect,
    unix_memory_resync,
    unix_flush_instruction_cache,
    unix_poison,
    unix_begin_simulation,
    unix_memory_resync_begin,
    unix_memory_translate,
    unix_flight_bind,
    unix_reconstruct_jit_fault,
    unix_prepare_dispatch,
    unix_complete_dispatch,
    unix_bind_direct_dispatch,
    unix_control_stack_protect,
    unix_funcs_count
};

enum xtajit64_stop_reason
{
    XTAJIT64_STOP_NONE,
    XTAJIT64_STOP_EC_TRANSITION,
    XTAJIT64_STOP_SYSCALL,
    XTAJIT64_STOP_MEMORY_FAULT,
    XTAJIT64_STOP_MAPPING_MISS,
    XTAJIT64_STOP_INVALID_INSTRUCTION,
    XTAJIT64_STOP_UNSUPPORTED_TRANSITION,
    XTAJIT64_STOP_INTERNAL_ERROR,
    XTAJIT64_STOP_SUSPEND,
    XTAJIT64_STOP_SINGLE_STEP,
    XTAJIT64_STOP_GUEST_EXCEPTION
};

struct xtajit64_x64_context
{
    UINT64 rax, rbx, rcx, rdx;
    UINT64 rsi, rdi, rbp, rsp;
    UINT64 r8, r9, r10, r11;
    UINT64 r12, r13, r14, r15;
    UINT64 rip, eflags;
    UINT32 mxcsr;
    UINT32 reserved;
    UINT64 xmm[16][2];
};

struct xtajit64_process_init_params
{
    UINT64 ec_bitmap;
    UINT64 highest_user_address;
    UINT64 guest_kuser;
    UINT64 host_kuser;
    UINT64 kuser_size;
    UINT64 rtl_exit_user_thread;
    UINT32 abi_version;
    UINT32 abi_size;
    UINT32 required_capabilities;
    UINT32 enabled_capabilities;
    UINT64 x64_syscall_dispatcher;
    UINT32 x64_syscall_count;
    /* ABI16: opt-in XTAJIT64_PROCESS_* flags, retaining the fixed layout. */
    UINT32 reserved;
    /* This native target is resolved by the PE-side ARM64EC metadata parser.
     * It lets the Unix provider distinguish Wine's RtlQueryPerformanceCounter
     * from an arbitrary EC function with a superficially similar body. */
    UINT64 rtl_query_performance_counter;
    /* The raw x64 export authenticates the final branch of an ARM64X
     * hybrid-patch thunk.  It is intentionally not redirected through the
     * ARM64EC metadata table. */
    UINT64 nt_query_performance_counter;
    /* Output only, zero on input. Windows PageSize describes logical pages,
     * not the Unix protection granularity needed for native stack barriers. */
    UINT64 native_page_size;
};

struct xtajit64_memory_params
{
    UINT64 guest;
    UINT64 host;
    UINT64 size;
    UINT64 allocation_base;
    UINT32 protect;
    UINT32 flags;
};

/* The caller owns a fresh, uniformly RW private allocation. Unix validates
 * its identity and protects exactly its second native page. The PE caller
 * must publish all three final runs before exposing the thread state; errors
 * are released through the ordinary PE VM path so deferred cleanup is tracked. */
struct xtajit64_control_stack_params
{
    UINT64 allocation;
    UINT64 size;
};

C_ASSERT( sizeof(struct xtajit64_control_stack_params) == 16 );

struct xtajit64_memory_resync_params
{
    UINT64 ranges;
    UINT64 generation;
    UINT32 count;
    UINT32 reserved;
};

struct xtajit64_memory_resync_begin_params
{
    UINT64 generation;
};

struct xtajit64_memory_translate_params
{
    UINT64 address;
    UINT64 size;
    UINT64 guest;
    UINT64 host;
    UINT64 allocation_base;
    UINT32 flags;
    UINT32 domain;
};

struct xtajit64_begin_params
{
    struct xtajit64_x64_context context;
    UINT64 gs_base;
    UINT64 stack_limit;
    UINT64 stack_base;
    UINT64 transition_target;
    UINT64 fault_address;
    UINT32 fault_access;
    UINT32 stop_reason;
    UINT32 provider_error;
    UINT32 reserved;
    UINT64 suspend_doorbell;
};

/* Each prepared execution owns the provider mapping lease until completion or
 * fault reconstruction. Return through the immutable entry once, then complete
 * before any native guest call. The three-part token rejects stale bindings. */
struct xtajit64_dispatch_params
{
    struct xtajit64_begin_params simulation;
    UINT64 entry;
    UINT64 frame;
    UINT64 generation;
    UINT64 binding_id;
    UINT64 process_instance;
};

struct xtajit64_direct_params
{
    struct xtajit64_dispatch_params dispatch;
    UINT64 binding_id;
    UINT64 process_instance;
};

C_ASSERT( sizeof(struct xtajit64_direct_params) == 528 );
C_ASSERT( offsetof(struct xtajit64_direct_params, binding_id) == 512 );

/* Stable copy of the native ARM64 register state captured by Wine's signal
 * path.  It deliberately excludes Darwin ucontext pointers and the larger
 * Windows debug-register tail.  GPR order is x0--x30 and vector order is
 * v0--v31. */
struct xtajit64_arm64_host_context
{
    UINT32 size;
    UINT32 version;
    UINT32 flags;
    UINT32 reserved;
    UINT64 gpr[31];
    UINT64 vector[32][2];
    UINT64 pc;
    UINT64 pstate;
    UINT32 fpcr;
    UINT32 fpsr;
};

/* ResetToConsistentState uses this only after Wine has abandoned an active
 * native FEX JIT stack.  On success the provider consumes that execution and
 * returns the complete guest state.  Access violations additionally return
 * the canonical guest fault address. */
struct xtajit64_reconstruct_params
{
    struct xtajit64_arm64_host_context host_context;
    struct xtajit64_x64_context context;
    UINT64 host_fault_address;
    UINT64 guest_fault_address;
    UINT32 exception_code;
    UINT32 fault_access;
    UINT32 stop_reason;
    UINT32 provider_error;
    UINT32 reserved;
};

/* Optional, diagnostic-only association.  Kept separate from begin_params so
 * operational provider ABI additions (for example suspend signaling) do not
 * need to share a hot-path diagnostic layout. */
struct xtajit64_flight_bind_params
{
    UINT64 recorder;
    UINT64 causal_boundary_id;
    UINT64 context_generation;
    UINT64 transition_generation;
    /* Stable PE-side x18 claim captured before this Unix dispatcher entry.
     * unix_flight_bind authenticates it against WINE_UNIX_LIB NtCurrentTeb(). */
    UINT64 claimed_teb;
    UINT64 guest_rip;
    UINT64 guest_rsp;
    UINT64 guest_stack_limit;
    UINT64 guest_stack_base;
    UINT64 control_stack_limit;
    UINT64 control_stack_top;
};

struct xtajit64_poison_params
{
    UINT32 status;
    UINT32 reserved;
};

C_ASSERT( sizeof(struct xtajit64_x64_context) == 408 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, abi_version) == 48 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, abi_size) == 52 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, required_capabilities) == 56 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, enabled_capabilities) == 60 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, x64_syscall_dispatcher) == 64 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, x64_syscall_count) == 72 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, reserved) == 76 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, rtl_query_performance_counter) == 80 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, nt_query_performance_counter) == 88 );
C_ASSERT( offsetof(struct xtajit64_process_init_params, native_page_size) == 96 );
C_ASSERT( sizeof(struct xtajit64_process_init_params) ==
          XTAJIT64_PROCESS_INIT_PARAMS_SIZE );
/* The new handshake intentionally cannot satisfy the legacy tail validation. */
C_ASSERT( (((UINT64)sizeof(struct xtajit64_process_init_params) << 32) |
           XTAJIT64_PROCESS_ABI_VERSION) != WINE_LOW_VA_SHADOW_BASE );
C_ASSERT( (UINT64)XTAJIT64_CAP_GS_NATIVE_DOMAIN != WINE_LOW_VA_SHADOW_SIZE );
C_ASSERT( sizeof(struct xtajit64_memory_params) == 40 );
C_ASSERT( sizeof(struct xtajit64_memory_resync_params) == 24 );
C_ASSERT( sizeof(struct xtajit64_memory_resync_begin_params) == 8 );
C_ASSERT( offsetof(struct xtajit64_memory_translate_params, flags) == 40 );
C_ASSERT( sizeof(struct xtajit64_memory_translate_params) == 48 );
C_ASSERT( sizeof(((struct xtajit64_begin_params *)0)->gs_base) == 8 );
C_ASSERT( offsetof(struct xtajit64_begin_params, gs_base) == 408 );
C_ASSERT( offsetof(struct xtajit64_begin_params, stack_limit) == 416 );
C_ASSERT( offsetof(struct xtajit64_begin_params, stack_base) == 424 );
C_ASSERT( offsetof(struct xtajit64_begin_params, transition_target) == 432 );
C_ASSERT( offsetof(struct xtajit64_begin_params, fault_address) == 440 );
C_ASSERT( offsetof(struct xtajit64_begin_params, fault_access) == 448 );
C_ASSERT( offsetof(struct xtajit64_begin_params, stop_reason) == 452 );
C_ASSERT( offsetof(struct xtajit64_begin_params, provider_error) == 456 );
C_ASSERT( offsetof(struct xtajit64_begin_params, reserved) == 460 );
C_ASSERT( offsetof(struct xtajit64_begin_params, suspend_doorbell) == 464 );
C_ASSERT( sizeof(struct xtajit64_begin_params) == XTAJIT64_BEGIN_PARAMS_SIZE );
C_ASSERT( sizeof(struct xtajit64_dispatch_params) == XTAJIT64_DISPATCH_PARAMS_SIZE );
C_ASSERT( offsetof(struct xtajit64_dispatch_params, entry) == XTAJIT64_BEGIN_PARAMS_SIZE );
C_ASSERT( offsetof(struct xtajit64_arm64_host_context, gpr) == 16 );
C_ASSERT( offsetof(struct xtajit64_arm64_host_context, vector) == 264 );
C_ASSERT( offsetof(struct xtajit64_arm64_host_context, pc) == 776 );
C_ASSERT( offsetof(struct xtajit64_arm64_host_context, fpcr) == 792 );
C_ASSERT( sizeof(struct xtajit64_arm64_host_context) == 800 );
C_ASSERT( offsetof(struct xtajit64_reconstruct_params, context) == 800 );
C_ASSERT( offsetof(struct xtajit64_reconstruct_params, host_fault_address) == 1208 );
C_ASSERT( offsetof(struct xtajit64_reconstruct_params, guest_fault_address) == 1216 );
C_ASSERT( offsetof(struct xtajit64_reconstruct_params, exception_code) == 1224 );
C_ASSERT( offsetof(struct xtajit64_reconstruct_params, provider_error) == 1236 );
C_ASSERT( sizeof(struct xtajit64_reconstruct_params) == XTAJIT64_RECONSTRUCT_PARAMS_SIZE );
C_ASSERT( sizeof(struct xtajit64_flight_bind_params) == 88 );
C_ASSERT( sizeof(struct xtajit64_poison_params) == 8 );
C_ASSERT( !(XTAJIT64_GUEST_KUSER & (XTAJIT64_MAX_HOST_PAGE_SIZE - 1)) );

#endif /* __WINE_XTAJIT64_UNIXLIB_H */
