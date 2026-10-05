/*
 * Native Switchyard FEX Unix-provider contract tests
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep standalone
#endif

#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <os/arch/arm64.h>
#include <switchyard_fex.h>

static enum switchyard_fex_result test_adapter_thread_destroy( struct switchyard_fex_thread *thread );
static enum switchyard_fex_result test_adapter_import_state( struct switchyard_fex_thread *thread,
                                                            const struct switchyard_fex_x64_state *state );
static enum switchyard_fex_result test_adapter_export_state( struct switchyard_fex_thread *thread,
                                                            struct switchyard_fex_x64_state *state );
static enum switchyard_fex_result test_prepare_native_window( struct switchyard_fex_thread *thread,
    const struct switchyard_fex_register_window *window,
    const struct switchyard_fex_execution *execution, uint64_t *generation );
static atomic_uint fused_initial_calls;
static enum switchyard_fex_result test_execute_export_window( struct switchyard_fex_thread *thread,
    uint64_t generation, struct switchyard_fex_stop *stop,
    const struct switchyard_fex_register_window *window );
static atomic_uint completion_calls;
static enum switchyard_fex_result test_execute_native_gate( struct switchyard_fex_thread *thread,
    const struct switchyard_fex_register_window *window, struct switchyard_fex_stop *stop );
static atomic_uint native_gate_calls;
#define switchyard_fex_experiment_prepare_native_window test_prepare_native_window
#define switchyard_fex_experiment_execute_export_window test_execute_export_window
#define switchyard_fex_experiment_execute_native_gate test_execute_native_gate
#define switchyard_fex_thread_destroy test_adapter_thread_destroy
#define switchyard_fex_thread_import_state test_adapter_import_state
#define switchyard_fex_thread_export_state test_adapter_export_state

#define __wine_register_arm64ec_low_memory_observer_v1 \
    test_register_arm64ec_low_memory_observer_v1
#define __wine_register_arm64ec_code_observer_v1 \
    test_register_arm64ec_code_observer_v1
#define __wine_register_arm64ec_jit_signal_observer_v3 \
    test_register_arm64ec_jit_signal_observer_v3

/* Include the implementation so this focused regression can validate the
 * transaction and fork state without adding production control opcodes. */
#define NtCurrentTeb test_current_teb
#define __wine_get_arm64ec_native_call_v1 test_get_native_call_bridge
#define NtQueryVirtualMemory test_query_virtual_memory
#define NtProtectVirtualMemory test_protect_virtual_memory
#include "../unixlib_fex.c"
#undef switchyard_fex_experiment_prepare_native_window
#undef switchyard_fex_experiment_execute_export_window
#undef switchyard_fex_experiment_execute_native_gate
#undef switchyard_fex_thread_destroy
#undef switchyard_fex_thread_import_state
#undef switchyard_fex_thread_export_state
#undef NtCurrentTeb
#undef __wine_get_arm64ec_native_call_v1
#undef NtQueryVirtualMemory
#undef NtProtectVirtualMemory
#include "../guest_exception.h"

static enum switchyard_fex_result test_prepare_native_window( struct switchyard_fex_thread *thread,
    const struct switchyard_fex_register_window *window,
    const struct switchyard_fex_execution *execution, uint64_t *generation )
{
    atomic_fetch_add_explicit( &fused_initial_calls, 1, memory_order_relaxed );
    return switchyard_fex_experiment_prepare_native_window( thread, window, execution, generation );
}

static enum switchyard_fex_result test_execute_export_window( struct switchyard_fex_thread *thread,
    uint64_t generation, struct switchyard_fex_stop *stop,
    const struct switchyard_fex_register_window *window )
{
    atomic_fetch_add_explicit( &completion_calls, 1, memory_order_relaxed );
    return switchyard_fex_experiment_execute_export_window( thread, generation, stop, window );
}

static enum switchyard_fex_result test_execute_native_gate( struct switchyard_fex_thread *thread,
    const struct switchyard_fex_register_window *window, struct switchyard_fex_stop *stop )
{
    atomic_fetch_add_explicit( &native_gate_calls, 1, memory_order_relaxed );
    return switchyard_fex_experiment_execute_native_gate( thread, window, stop );
}

static void *test_control_allocation;
static _Thread_local int track_hidden_syscall;
static _Thread_local unsigned int hidden_imports, hidden_exports;
static _Thread_local struct switchyard_fex_x64_state expected_hidden;

/* Test-only seed after the real full export. No production hook or guessed
 * timer is needed: the real continuation must import and preserve this state. */
static enum switchyard_fex_result test_adapter_export_state( struct switchyard_fex_thread *thread,
                                                            struct switchyard_fex_x64_state *state )
{
    enum switchyard_fex_result result = switchyard_fex_thread_export_state( thread, state );
    size_t index;

    if (!track_hidden_syscall || result != SWITCHYARD_FEX_OK) return result;
    ++hidden_exports;
    state->fcw = 0x027f;
    state->abridged_ftw = 0xa5;
    for (index = 0; index < 8; ++index)
    {
        state->x87[index].low = UINT64_C(0x8102736455ab00ff) + index;
        state->x87[index].high = 0x3fff;
    }
    for (index = 0; index < 16; ++index)
    {
        state->ymm_high[index].low = UINT64_C(0x5577226688aabbcc) + index;
        state->ymm_high[index].high = UINT64_C(0xcafefeed33441122) - index;
    }
    state->segment[4] = 0x18;
    state->segment_base[4] = UINT64_C(0x1020304000);
    memcpy( &expected_hidden, state, sizeof(*state) );
    return result;
}

static enum switchyard_fex_result test_adapter_import_state( struct switchyard_fex_thread *thread,
                                                            const struct switchyard_fex_x64_state *state )
{
    if (track_hidden_syscall)
    {
        ++hidden_imports;
        if (getenv( "XTAJIT64_FEX_TEST_WINDOW_SYSCALL_MUTANT" ))
        {
            struct xtajit64_x64_context context;
            struct switchyard_fex_register_window window = {
                .size = sizeof(window), .version = SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
                .data = (uintptr_t)&context, .data_size = sizeof(context),
                .gs_base = state->segment_base[5],
            };
            export_fex_state( &context, state );
            return switchyard_fex_thread_import_register_window( thread, &window );
        }
    }
    return switchyard_fex_thread_import_state( thread, state );
}

NTSTATUS WINAPI test_query_virtual_memory( HANDLE process, const void *address,
    MEMORY_INFORMATION_CLASS kind, void *buffer, SIZE_T size, SIZE_T *ret_size )
{
    (void)process;
    (void)ret_size;
    if (!test_control_allocation || address != test_control_allocation) return STATUS_INVALID_ADDRESS;
    if (kind == MemoryBasicInformation && size == sizeof(MEMORY_BASIC_INFORMATION))
    {
        MEMORY_BASIC_INFORMATION *info = buffer;
        memset( info, 0, size );
        info->BaseAddress = info->AllocationBase = test_control_allocation;
        info->RegionSize = XTAJIT64_CONTROL_STACK_SIZE;
        info->State = MEM_COMMIT;
        info->Type = MEM_PRIVATE;
        info->Protect = PAGE_READWRITE;
    }
    else if (kind == MemoryWineTranslatedViewInformation && size == sizeof(WINE_TRANSLATED_VIEW_INFORMATION))
    {
        WINE_TRANSLATED_VIEW_INFORMATION *info = buffer;
        memset( info, 0, size );
        info->Version = WINE_TRANSLATED_VIEW_INFORMATION_VERSION;
        info->GuestBase = info->HostBase = info->AllocationBase = test_control_allocation;
        info->RegionSize = XTAJIT64_CONTROL_STACK_SIZE;
    }
    else return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI test_protect_virtual_memory( HANDLE process, void **address,
                                             SIZE_T *size, ULONG protect, ULONG *old_protect )
{
    (void)process;
    if (!test_control_allocation || *address != (char *)test_control_allocation + provider.native_page_size ||
        *size != provider.native_page_size || protect != PAGE_NOACCESS)
        return STATUS_INVALID_ADDRESS;
    if (mprotect( *address, *size, PROT_NONE )) return STATUS_UNSUCCESSFUL;
    *old_protect = PAGE_READWRITE;
    return STATUS_SUCCESS;
}

/* This native provider fixture tests capsule publication/ownership in system
 * mode. The real x18/PE bridge has a separate actual-source ABI/signal test. */
NTSTATUS test_native_call_bridge( unixlib_entry_t entry, void *args, UINT64 teb )
{
    (void)entry;
    (void)args;
    (void)teb;
    abort();
}

UINT64 test_get_native_call_bridge(void)
{
    return (uintptr_t)test_native_call_bridge;
}

#undef __wine_register_arm64ec_low_memory_observer_v1
#undef __wine_register_arm64ec_code_observer_v1
#undef __wine_register_arm64ec_jit_signal_observer_v3

#define TEST_PAGE           0x4000u
#define TEST_PAGE_COUNT     8u
#define TEST_KUSER_BASE     (WINE_LOW_VA_SHADOW_BASE + 0x04000000ull)
#define TEST_IDENTITY_BASE  (WINE_LOW_VA_SHADOW_BASE + 0x06000000ull)
#define TEST_FALLBACK_BASE  0x0000001006000000ull
#define TEST_TIMEOUT_MS     15000u
#define TEST_SYSCALL_COUNT  64u

static const struct wine_arm64ec_low_memory_observer_v1 *registered_low_observer;
static const struct wine_arm64ec_code_observer_v1 *registered_code_observer;
static const struct wine_arm64ec_jit_signal_observer_v3 *registered_signal_observer;
static volatile sig_atomic_t signal_repairs;
static atomic_int hold_mutation;
static atomic_int mutation_quiesced;
static atomic_int release_mutation;
static atomic_int mutation_waiting;
static atomic_int count_prepared;
static atomic_int prepared_count;
static atomic_int hold_wake;
static atomic_int wake_observed;
static atomic_int release_wake;
static unsigned int failures;
static unsigned char *test_pages;
static unsigned char *test_kuser;
static uint64_t test_base;
static uint64_t target_address;
static uint64_t dispatcher_address;
static uint64_t teb_address;
static uint64_t stack_address;
static uint64_t loop_address;
static uint64_t *ec_bitmap;
static size_t ec_bitmap_size;
static BOOL test_custom_dispatch;
static BOOL test_direct_dispatch;
static BOOL refuse_next_destroy;
static void (*test_set_custom_x18)(bool);
static bool (*test_custom_x18_enabled)(void);

/* This standalone process has no Wine thread_data. Model only the Unix TEB
 * authority here; full Wine PE tests exercise its real pthread-owned value. */
TEB * WINAPI test_current_teb(void)
{
    return (TEB *)(uintptr_t)teb_address;
}

/* The host test owns the SDK mode pair that Wine's Unixlib dispatcher normally
 * supplies. No host C activation runs while custom mode is enabled. */
static void __attribute__((naked)) invoke_test_dispatch(
    UINT64 entry, UINT64 frame, UINT64 teb, void (*set_mode)(bool) )
{
    __asm__("stp x19, x20, [sp, #-64]!\n\t"
            "stp x29, x30, [sp, #16]\n\t"
            "stp x0, x1, [sp, #32]\n\t"
            "stp x2, x3, [sp, #48]\n\t"
            "mrs x19, fpcr\n\tmrs x20, fpsr\n\t"
            "mov w0, #1\n\tblr x3\n\t"
            "ldr x18, [sp, #48]\n\t"
            "ldp x16, x0, [sp, #32]\n\tblr x16\n\t"
            "ldr x16, [sp, #56]\n\tmov w0, #0\n\tblr x16\n\t"
            "msr fpcr, x19\n\tmsr fpsr, x20\n\t"
            "ldp x29, x30, [sp, #16]\n\t"
            "ldp x19, x20, [sp], #64\n\tret");
}

static NTSTATUS test_begin_simulation( struct xtajit64_begin_params *params )
{
    struct xtajit64_direct_params direct = {0};
    struct xtajit64_dispatch_params dispatch = {0};
    struct xtajit64_direct_capsule capsule = {
        .size = sizeof(capsule), .version = XTAJIT64_DIRECT_CAPSULE_VERSION };
    NTSTATUS status;

    if (!test_custom_dispatch && !test_direct_dispatch) return begin_simulation( params );
    dispatch.simulation = *params;
    if (test_direct_dispatch)
    {
        if (bind_direct_dispatch( &capsule )) abort();
        direct.binding_id = capsule.binding_id;
        direct.process_instance = capsule.process_instance;
        direct.dispatch = dispatch;
        status = prepare_direct_dispatch( &direct );
        dispatch = direct.dispatch;
    }
    else status = prepare_dispatch( &dispatch );
    while (!status && dispatch.entry)
    {
        struct xtajit64_dispatch_params stale = dispatch;

        /* These must not consume either execution lease. */
        --stale.generation;
        if (complete_dispatch( &stale ) != STATUS_INVALID_DEVICE_STATE) abort();
        stale = dispatch;
        ++stale.binding_id;
        if (complete_dispatch( &stale ) != STATUS_INVALID_DEVICE_STATE) abort();
        stale = dispatch;
        ++stale.process_instance;
        if (complete_dispatch( &stale ) != STATUS_INVALID_DEVICE_STATE) abort();
        if (test_direct_dispatch)
        {
            struct xtajit64_direct_params invalid = direct;
            invalid.dispatch = dispatch;
            ++invalid.binding_id;
            if (complete_direct_dispatch( &invalid ) != STATUS_INVALID_DEVICE_STATE) abort();
            invalid.binding_id = direct.binding_id;
            ++invalid.process_instance;
            if (complete_direct_dispatch( &invalid ) != STATUS_INVALID_DEVICE_STATE) abort();
        }
        invoke_test_dispatch( dispatch.entry, dispatch.frame, teb_address, test_set_custom_x18 );
        if (test_direct_dispatch)
        {
            direct.dispatch = dispatch;
            status = complete_direct_dispatch( &direct );
            dispatch = direct.dispatch;
        }
        else status = complete_dispatch( &dispatch );
    }
    *params = dispatch.simulation;
    if (!dispatch.entry && complete_dispatch( &dispatch ) != STATUS_INVALID_DEVICE_STATE) abort();
    return status;
}

/* Reuse the exact syscall/fault/suspension/mutation/fork assertions below. */
#define begin_simulation test_begin_simulation

static void check_failure( const char *message )
{
    ++failures;
    fprintf( stderr, "failure: %s\n", message );
}

#define check(condition, message) \
    do { if (!(condition)) check_failure( message ); } while (0)

static void test_state_input_initialization(void)
{
    struct xtajit64_x64_context input, before;
    struct switchyard_fex_x64_state expected;
    struct
    {
        uint64_t before;
        struct switchyard_fex_x64_state state;
        uint64_t after;
    } outputs[2];
    const uint64_t canary = UINT64_C(0x1739b4c8f206ad5e);
    uint64_t random = UINT64_C(0x9630c2e4a81957bd), gs_base;
    unsigned int iteration, poison;
    size_t i;

    /* Full-byte equivalence includes every reserved byte and the invalid
     * YMM high halves. A missing initialization must fail both poisons. */
    for (iteration = 0; iteration < 132; ++iteration)
    {
        unsigned char *bytes = (unsigned char *)&input;
        for (i = 0; i < sizeof(input); ++i)
        {
            random = random * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
            bytes[i] = iteration < 2 ? (iteration ? 0xff : 0) : (unsigned char)(random >> 56);
        }
        gs_base = iteration == 0 ? 0 : iteration == 1 ? UINT64_MAX : random;
        memcpy( &before, &input, sizeof(input) );
        memset( &expected, 0, sizeof(expected) );
        expected.size = sizeof(expected);
        expected.version = SWITCHYARD_FEX_STATE_VERSION;
        expected.gpr[0] = input.rax;
        expected.gpr[1] = input.rcx;
        expected.gpr[2] = input.rdx;
        expected.gpr[3] = input.rbx;
        expected.gpr[4] = input.rsp;
        expected.gpr[5] = input.rbp;
        expected.gpr[6] = input.rsi;
        expected.gpr[7] = input.rdi;
        expected.gpr[8] = input.r8;
        expected.gpr[9] = input.r9;
        expected.gpr[10] = input.r10;
        expected.gpr[11] = input.r11;
        expected.gpr[12] = input.r12;
        expected.gpr[13] = input.r13;
        expected.gpr[14] = input.r14;
        expected.gpr[15] = input.r15;
        expected.rip = input.rip;
        expected.rflags = input.eflags;
        expected.mxcsr = input.mxcsr;
        expected.fcw = 0x037f;
        expected.segment[1] = 0x30;
        expected.segment_base[5] = gs_base;
        memcpy( expected.xmm, input.xmm, sizeof(expected.xmm) );
        for (poison = 0; poison < 2; ++poison)
        {
            memset( &outputs[poison], poison ? 0x5a : 0xa5, sizeof(outputs[poison]) );
            outputs[poison].before = outputs[poison].after = canary;
            import_fex_state( &outputs[poison].state, &input, gs_base );
            check( !memcmp( &outputs[poison].state, &expected, sizeof(expected) ),
                   "state input is not completely initialized" );
            check( outputs[poison].before == canary && outputs[poison].after == canary,
                   "state input initialization exceeded its destination" );
            check( !memcmp( &input, &before, sizeof(input) ), "state input source was modified" );
        }
    }
}

static void test_state_output_publication(void)
{
    struct thread_binding binding = {0};
    struct switchyard_fex_x64_state state;
    struct xtajit64_begin_params params, before;
    const unsigned char *bytes = (const unsigned char *)&state;
    size_t i;

    memset( &state, 0xa5, sizeof(state) );
    memset( &params, 0x5a, sizeof(params) );
    memcpy( &before, &params, sizeof(params) );
    binding.flight_guest_rip = 0x1122334455667788ull;
    binding.flight_guest_rsp = 0x8877665544332211ull;
    /* The real adapter rejects a NULL thread. No output payload is valid. */
    check( export_thread_state( &binding, &state, &params ) == STATUS_INVALID_PARAMETER,
           "state export failure was not propagated" );
    check( !memcmp( &params, &before, sizeof(params) ) &&
           binding.flight_guest_rip == 0x1122334455667788ull &&
           binding.flight_guest_rsp == 0x8877665544332211ull,
           "failed export published payload or flight state" );
    check( state.size == sizeof(state) && state.version == SWITCHYARD_FEX_STATE_VERSION,
           "state export output header is invalid" );
    for (i = offsetof(struct switchyard_fex_x64_state, flags); i < sizeof(state); ++i)
        check( bytes[i] == 0xa5, "provider cleared an unused output payload" );
    check( export_thread_register_window( &binding, &params ) == STATUS_INVALID_PARAMETER,
           "window export failure was not propagated" );
    check( !memcmp( &params, &before, sizeof(params) ) &&
           binding.flight_guest_rip == 0x1122334455667788ull &&
           binding.flight_guest_rsp == 0x8877665544332211ull,
           "failed window export published payload or flight state" );
}

static void test_direct_binding(void)
{
    struct xtajit64_direct_capsule capsule = {
        .size = sizeof(capsule), .version = XTAJIT64_DIRECT_CAPSULE_VERSION };
    struct xtajit64_direct_params params = {0};
    struct thread_binding *binding = pthread_getspecific( binding_key );

    check( bind_direct_dispatch( NULL ) == STATUS_INVALID_PARAMETER, "NULL direct capsule" );
    capsule.size--;
    check( bind_direct_dispatch( &capsule ) == STATUS_INVALID_PARAMETER, "short direct capsule" );
    capsule.size += 2;
    check( bind_direct_dispatch( &capsule ) == STATUS_INVALID_PARAMETER, "large direct capsule" );
    capsule.size = sizeof(capsule);
    capsule.version++;
    check( bind_direct_dispatch( &capsule ) == STATUS_INVALID_PARAMETER, "capsule version" );
    capsule.version--;
    capsule.reserved = 1;
    check( bind_direct_dispatch( &capsule ) == STATUS_INVALID_PARAMETER, "capsule reserved" );
    capsule.reserved = 0;
    capsule.prepare = 1;
    check( bind_direct_dispatch( &capsule ) == STATUS_INVALID_PARAMETER, "capsule output not empty" );
    capsule.prepare = 0;
    check( !bind_direct_dispatch( &capsule ) && capsule.bridge == (uintptr_t)test_native_call_bridge &&
           capsule.prepare == (uintptr_t)prepare_direct_dispatch &&
           capsule.complete == (uintptr_t)complete_direct_dispatch &&
           capsule.authenticated_teb == teb_address && capsule.binding_id == binding->id &&
           capsule.process_instance == binding->process_instance, "capsule authority" );
    check( prepare_direct_dispatch( NULL ) == STATUS_INVALID_PARAMETER, "NULL direct prepare" );
    check( complete_direct_dispatch( NULL ) == STATUS_INVALID_PARAMETER, "NULL direct complete" );
    check( prepare_direct_dispatch( &params ) == STATUS_INVALID_DEVICE_STATE, "zero direct identity" );
    params.binding_id = capsule.binding_id + 1;
    params.process_instance = capsule.process_instance;
    check( prepare_direct_dispatch( &params ) == STATUS_INVALID_DEVICE_STATE, "stale binding prepare" );
    params.binding_id = capsule.binding_id;
    params.process_instance++;
    check( prepare_direct_dispatch( &params ) == STATUS_INVALID_DEVICE_STATE, "stale process prepare" );
    check( !binding_is_active( binding ), "rejected direct call acquired admission" );
}

static enum switchyard_fex_result test_adapter_thread_destroy( struct switchyard_fex_thread *thread )
{
    int ret = pthread_mutex_trylock( &provider.mutex );

    check( ret == EBUSY, "adapter teardown escaped the provider membership mutex" );
    if (!ret) pthread_mutex_unlock( &provider.mutex );
    if (refuse_next_destroy)
    {
        refuse_next_destroy = FALSE;
        return SWITCHYARD_FEX_ERROR_BUSY;
    }
    return switchyard_fex_thread_destroy( thread );
}

static void test_retirement_failure(void)
{
    struct thread_binding *binding = pthread_getspecific( binding_key );
    NTSTATUS status;
    BOOL retained;

    refuse_next_destroy = TRUE;
    status = thread_term( NULL );
    check( status == STATUS_DEVICE_BUSY && !pthread_getspecific( binding_key ),
           "adapter destroy refusal was not propagated" );
    pthread_mutex_lock( &provider.mutex );
    retained = provider.bindings == binding;
    check( retained && provider.poison_status == status &&
           (switchyard_fex_admission_load( binding->admission ) & SWITCHYARD_FEX_ADMISSION_FLAGS) ==
               SWITCHYARD_FEX_ADMISSION_CLOSED,
           "failed teardown freed ownership or left admission open" );
    pthread_mutex_unlock( &provider.mutex );
    check( process_term( NULL ) == STATUS_DEVICE_BUSY, "process outlived unowned failed teardown" );
    /* Fixture-only recovery: injected BUSY changed no adapter state. Real
     * partial destruction must stay poisoned and is not retried automatically. */
    if (!retained) _Exit( 94 );
    pthread_mutex_lock( &provider.mutex );
    status = destroy_binding_locked( binding );
    if (!status) provider.poison_status = STATUS_SUCCESS;
    pthread_mutex_unlock( &provider.mutex );
    check( !status, "cannot release retained fixture binding" );
    if (!status) free( binding );
    check( !thread_init( NULL ), "cannot reattach after fixture recovery" );
}

static void timeout_handler( int signal )
{
    static const char message[] = "failure: FEX Unix-provider test timed out\n";

    (void)signal;
    write( STDERR_FILENO, message, sizeof(message) - 1 );
    _exit( 124 );
}

static void test_guest_exception_mapping(void)
{
    struct xtajit64_guest_exception_mapping mapping;
    const UINT64 rip = 0x123456789abcull;
    const UINT64 rax = 0x1122334455667788ull;
    const UINT64 rcx = 0x8877665544332211ull;

    xtajit64_map_guest_exception( 5, 3, 0, rip, rax, rcx, &mapping );
    check( mapping.code == STATUS_BREAKPOINT &&
           mapping.context_rip == rip && mapping.exception_address == rip - 1 &&
           mapping.parameter_count == 1 && !mapping.information[0],
           "FEX #BP exception address changed the context RIP" );

    xtajit64_map_guest_exception( 11, 13, (3 << 3) | 2,
                                  rip, rax, rcx, &mapping );
    check( mapping.code == STATUS_BREAKPOINT &&
           mapping.context_rip == rip + 2 &&
           mapping.exception_address == rip + 1 &&
           mapping.parameter_count == 1 && !mapping.information[0],
           "FEX int 3 context RIP or exception address is incorrect" );

    xtajit64_map_guest_exception( 11, 13, (0x2d << 3) | 2,
                                  rip, rax, rcx, &mapping );
    check( mapping.code == STATUS_BREAKPOINT &&
           mapping.context_rip == rip + 3 &&
           mapping.exception_address == rip + 3 &&
           mapping.parameter_count == 1 && mapping.information[0] == rax,
           "FEX int 0x2d context RIP, address, or argument is incorrect" );

    xtajit64_map_guest_exception( 11, 13, (0x29 << 3) | 2,
                                  rip, rax, rcx, &mapping );
    check( mapping.code == STATUS_STACK_BUFFER_OVERRUN &&
           mapping.context_rip == rip && mapping.exception_address == rip &&
           mapping.parameter_count == 1 && mapping.information[0] == rcx,
           "FEX fast-fail exception mapping is incorrect" );

    xtajit64_map_guest_exception( 11, 14, 0, rip, rax, rcx, &mapping );
    check( mapping.code == STATUS_ACCESS_VIOLATION &&
           mapping.parameter_count == 2 &&
           mapping.information[0] == EXCEPTION_EXECUTE_FAULT &&
           mapping.information[1] == rip,
           "FEX generated page-fault mapping is incorrect" );

    xtajit64_map_guest_exception( 5, 1, 0, rip, rax, rcx, &mapping );
    check( mapping.code == STATUS_SINGLE_STEP && mapping.clear_trap_flag &&
           mapping.context_rip == rip && mapping.exception_address == rip,
           "FEX single-step mapping is incorrect" );
}

static BOOL wait_atomic_value( atomic_int *value, int expected,
                               unsigned int timeout_ms )
{
    struct timespec delay = {0, 1000000};
    unsigned int elapsed;

    for (elapsed = 0; elapsed < timeout_ms; ++elapsed)
    {
        if (atomic_load_explicit( value, memory_order_acquire ) == expected)
            return TRUE;
        nanosleep( &delay, NULL );
    }
    return FALSE;
}

void xtajit64_fex_test_mutation_waiting(void)
{
    atomic_store_explicit( &mutation_waiting, 1, memory_order_release );
}

void xtajit64_fex_test_mutation_quiesced(void)
{
    if (!atomic_load_explicit( &hold_mutation, memory_order_acquire )) return;
    atomic_store_explicit( &mutation_quiesced, 1, memory_order_release );
    while (!atomic_load_explicit( &release_mutation, memory_order_acquire ))
        sched_yield();
}

void xtajit64_fex_test_execution_prepared(void)
{
    if (atomic_load_explicit( &count_prepared, memory_order_acquire ))
        atomic_fetch_add_explicit( &prepared_count, 1, memory_order_release );
}

void xtajit64_fex_test_execution_wake(void)
{
    if (!atomic_load_explicit( &hold_wake, memory_order_acquire )) return;
    atomic_store_explicit( &wake_observed, 1, memory_order_release );
    if (!wait_atomic_value( &release_wake, 1, TEST_TIMEOUT_MS )) _Exit( 95 );
}

void xtajit64_fex_test_observer_status( const char *observer, NTSTATUS status,
                                        unsigned int stage )
{
    fprintf( stderr, "%s observer completion status %#x at stage %u\n",
             observer, (unsigned int)status, stage );
}

int32_t test_register_arm64ec_low_memory_observer_v1(
    const struct wine_arm64ec_low_memory_observer_v1 *observer )
{
    struct wine_arm64ec_low_memory_range_v1 range;
    struct wine_arm64ec_low_memory_event_v1 event;
    void *transaction = NULL;
    NTSTATUS status;

    if (registered_low_observer) return STATUS_ALREADY_REGISTERED;
    if (!observer || observer->version != WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION ||
        observer->size != sizeof(*observer) || !observer->begin ||
        !observer->complete || observer->capabilities !=
            WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT)
    {
        if (observer)
            fprintf( stderr, "LOW descriptor %u/%u size %u/%lu callbacks %p/%p "
                     "caps %#llx/%#llx\n", observer->version,
                     WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION, observer->size,
                     (unsigned long)sizeof(*observer), observer->begin,
                     observer->complete, (unsigned long long)observer->capabilities,
                     (unsigned long long)
                         WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT );
        return STATUS_INVALID_PARAMETER;
    }

    status = observer->begin( observer->context, WINE_WOW64_MEMORY_RESYNC,
                              WINE_LOW_VA_SHADOW_BASE,
                              WINE_LOW_VA_SHADOW_SIZE, 0, &transaction );
    if (status) return status;

    memset( &range, 0, sizeof(range) );
    range.host_address = WINE_LOW_VA_SHADOW_BASE;
    range.size = WINE_LOW_VA_SHADOW_SIZE;
    range.state = MEM_FREE;
    range.protect = PAGE_NOACCESS;
    memset( &event, 0, sizeof(event) );
    event.version = WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_RESYNC;
    event.flags = WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT;
    event.host_address = WINE_LOW_VA_SHADOW_BASE;
    event.size_covered = WINE_LOW_VA_SHADOW_SIZE;
    event.ranges = &range;
    event.range_count = 1;
    observer->complete( observer->context, transaction, &event );
    if (provider.poison_status) return provider.poison_status;
    registered_low_observer = observer;
    return STATUS_SUCCESS;
}

int32_t test_register_arm64ec_code_observer_v1(
    const struct wine_arm64ec_code_observer_v1 *observer )
{
    struct wine_arm64ec_code_event_v1 event;
    void *transaction = NULL;
    NTSTATUS status;

    if (registered_code_observer) return STATUS_ALREADY_REGISTERED;
    if (!observer || observer->version != WINE_ARM64EC_CODE_OBSERVER_VERSION ||
        observer->size != sizeof(*observer) || !observer->begin ||
        !observer->complete || observer->capabilities !=
            WINE_ARM64EC_CODE_OBSERVER_CAP_EXACT_INVALIDATION_RANGES)
    {
        if (observer)
            fprintf( stderr, "code descriptor %u/%u size %u/%lu callbacks %p/%p "
                     "caps %#llx/%#llx\n", observer->version,
                     WINE_ARM64EC_CODE_OBSERVER_VERSION, observer->size,
                     (unsigned long)sizeof(*observer), observer->begin,
                     observer->complete, (unsigned long long)observer->capabilities,
                     (unsigned long long)
                         WINE_ARM64EC_CODE_OBSERVER_CAP_EXACT_INVALIDATION_RANGES );
        return STATUS_INVALID_PARAMETER;
    }

    status = observer->begin( observer->context, WINE_ARM64EC_CODE_RESYNC,
                              &transaction );
    if (status) return status;
    memset( &event, 0, sizeof(event) );
    event.version = WINE_ARM64EC_CODE_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_ARM64EC_CODE_RESYNC;
    event.flags = WINE_ARM64EC_CODE_EVENT_FULL_INVALIDATION;
    observer->complete( observer->context, transaction, &event );
    if (provider.poison_status) return provider.poison_status;
    registered_code_observer = observer;
    return STATUS_SUCCESS;
}

int32_t test_register_arm64ec_jit_signal_observer_v3(
    const struct wine_arm64ec_jit_signal_observer_v3 *observer )
{
    if (registered_signal_observer) return STATUS_ALREADY_REGISTERED;
    if (!observer || observer->version != WINE_ARM64EC_JIT_SIGNAL_OBSERVER_VERSION ||
        observer->size != sizeof(*observer) || observer->flags || observer->reserved ||
        !observer->repair_jit_fault || !observer->query_exception_stack || observer->capabilities !=
            WINE_ARM64EC_JIT_SIGNAL_OBSERVER_CAPABILITIES)
        return STATUS_INVALID_PARAMETER;
    registered_signal_observer = observer;
    return STATUS_SUCCESS;
}

static void repair_test_sigbus( int signal, siginfo_t *info, void *opaque )
{
    ucontext_t *context = opaque;
    struct wine_arm64ec_jit_host_context_v1 host;
    unsigned int i;
    BOOL custom = test_custom_dispatch && test_custom_x18_enabled();

    if (custom) test_set_custom_x18( false );

    memset( &host, 0, sizeof(host) );
    host.size = sizeof(host);
    host.version = WINE_ARM64EC_JIT_HOST_CONTEXT_VERSION;
    for (i = 0; i < 29; ++i) host.gpr[i] = context->uc_mcontext->__ss.__x[i];
    host.gpr[29] = context->uc_mcontext->__ss.__fp;
    host.gpr[30] = context->uc_mcontext->__ss.__lr;
    memcpy( host.vector, context->uc_mcontext->__ns.__v, sizeof(host.vector) );
    host.pc = context->uc_mcontext->__ss.__pc;
    host.pstate = context->uc_mcontext->__ss.__cpsr;
    host.fpcr = context->uc_mcontext->__ns.__fpcr;
    host.fpsr = context->uc_mcontext->__ns.__fpsr;
    if (!registered_signal_observer ||
        registered_signal_observer->repair_jit_fault(
            registered_signal_observer->context, &host,
            (context->uc_mcontext->__es.__esr >> 6) & 1,
            (context->uc_mcontext->__es.__esr & 0x3f) == 0x21 ?
                WINE_ARM64EC_JIT_SIGNAL_ALIGNMENT_FAULT : 0,
            (uintptr_t)info->si_addr ))
        _exit( 128 + signal );
    context->uc_mcontext->__ss.__pc = host.pc;
    context->uc_mcontext->__ss.__x[25] = host.gpr[25];
    ++signal_repairs;
    if (custom)
    {
        test_set_custom_x18( true );
        __asm__ volatile( "mov x18, %0" : : "r"(context->uc_mcontext->__ss.__x[18]) );
    }
}

static void *alloc_pages_at( uint64_t address, size_t count )
{
    size_t size;
    void *result;

    if (!count || count > SIZE_MAX / TEST_PAGE) return NULL;
    size = count * TEST_PAGE;
    result = mmap( (void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (result == MAP_FAILED) return NULL;
    if ((uintptr_t)result == address) return result;
    munmap( result, size );
    return NULL;
}

static NTSTATUS map_identity_page( uint64_t address, unsigned int protect )
{
    struct xtajit64_memory_params params;

    memset( &params, 0, sizeof(params) );
    params.guest = address;
    params.host = address;
    params.size = TEST_PAGE;
    params.allocation_base = address;
    params.protect = protect;
    return memory_map( &params );
}

static NTSTATUS flush_code_page( uint64_t address )
{
    struct xtajit64_memory_params params;

    memset( &params, 0, sizeof(params) );
    params.guest = address;
    params.size = TEST_PAGE;
    return flush_instruction_cache( &params );
}

static void set_ec_page( uint64_t address, BOOL enabled )
{
    uint64_t page = address / TEST_PAGE;
    uint64_t mask = 1ull << (page & 63);

    if (enabled) __atomic_fetch_or( &ec_bitmap[page / 64], mask, __ATOMIC_RELEASE );
    else __atomic_fetch_and( &ec_bitmap[page / 64], ~mask, __ATOMIC_RELEASE );
}

static void emit_u64( unsigned char *code, size_t *offset, uint64_t value )
{
    memcpy( code + *offset, &value, sizeof(value) );
    *offset += sizeof(value);
}

static void emit_jump_r11( unsigned char *code, size_t *offset, uint64_t target )
{
    code[(*offset)++] = 0x49;
    code[(*offset)++] = 0xbb;
    emit_u64( code, offset, target );
    code[(*offset)++] = 0x41;
    code[(*offset)++] = 0xff;
    code[(*offset)++] = 0xe3;
}

static NTSTATUS publish_code( uint64_t address, const unsigned char *code,
                              size_t size )
{
    if (!size || size > TEST_PAGE) return STATUS_INVALID_PARAMETER;
    if (mprotect( (void *)(uintptr_t)address, TEST_PAGE,
                  PROT_READ | PROT_WRITE ))
        return STATUS_UNSUCCESSFUL;
    memset( (void *)(uintptr_t)address, 0xcc, TEST_PAGE );
    memcpy( (void *)(uintptr_t)address, code, size );
    if (mprotect( (void *)(uintptr_t)address, TEST_PAGE,
                  PROT_READ | PROT_EXEC ))
        return STATUS_UNSUCCESSFUL;
    return flush_code_page( address );
}

static void init_begin_params( struct xtajit64_begin_params *params,
                               uint64_t rip, volatile uint32_t *doorbell )
{
    memset( params, 0, sizeof(*params) );
    params->context.rip = rip;
    params->context.rsp = stack_address + TEST_PAGE - 32;
    params->context.eflags = 0x202;
    params->context.mxcsr = 0x1f80;
    params->gs_base = teb_address;
    params->stack_limit = stack_address;
    params->stack_base = stack_address + TEST_PAGE;
    params->suspend_doorbell = (uintptr_t)doorbell;
}

static void test_observer_reentrancy(void)
{
    struct wine_arm64ec_low_memory_event_v1 low_event;
    struct wine_arm64ec_code_event_v1 code_event;
    void *low_transaction = NULL, *code_transaction = NULL, *nested = NULL;
    NTSTATUS status;

    status = low_observer_begin( &provider, WINE_WOW64_MEMORY_MAP,
                                 WINE_LOW_VA_SHADOW_BASE + TEST_PAGE,
                                 TEST_PAGE, WINE_LOW_VA_SHADOW_BASE + TEST_PAGE,
                                 &low_transaction );
    check( !status && low_transaction, "LOW observer begin failed" );
    if (status) return;
    status = low_observer_begin( &provider, WINE_WOW64_MEMORY_MAP,
                                 WINE_LOW_VA_SHADOW_BASE + TEST_PAGE, TEST_PAGE,
                                 WINE_LOW_VA_SHADOW_BASE + TEST_PAGE, &nested );
    check( status == STATUS_INVALID_DEVICE_STATE && !nested,
           "recursive LOW observer was accepted" );
    status = code_observer_begin( &provider, WINE_ARM64EC_CODE_MAP, &nested );
    check( status == STATUS_INVALID_DEVICE_STATE && !nested,
           "code observer nested inside LOW mutation was accepted" );
    memset( &low_event, 0, sizeof(low_event) );
    low_event.version = WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION;
    low_event.size = sizeof(low_event);
    low_event.operation = WINE_WOW64_MEMORY_MAP;
    low_event.status = STATUS_UNSUCCESSFUL;
    low_event.host_address = WINE_LOW_VA_SHADOW_BASE + TEST_PAGE;
    low_event.size_covered = TEST_PAGE;
    low_event.host_allocation_base = WINE_LOW_VA_SHADOW_BASE + TEST_PAGE;
    low_observer_complete( &provider, low_transaction, &low_event );

    status = code_observer_begin( &provider, WINE_ARM64EC_CODE_MAP,
                                  &code_transaction );
    check( !status && code_transaction, "code observer begin failed" );
    if (status) return;
    status = code_observer_begin( &provider, WINE_ARM64EC_CODE_MAP, &nested );
    check( status == STATUS_INVALID_DEVICE_STATE && !nested,
           "recursive code observer was accepted" );
    nested = NULL;
    status = low_observer_begin( &provider, WINE_WOW64_MEMORY_MAP,
                                 WINE_LOW_VA_SHADOW_BASE + 2 * TEST_PAGE,
                                 TEST_PAGE, WINE_LOW_VA_SHADOW_BASE + 2 * TEST_PAGE,
                                 &nested );
    check( status == STATUS_INVALID_DEVICE_STATE && !nested,
           "LOW observer nested inside code mutation was accepted" );
    memset( &code_event, 0, sizeof(code_event) );
    code_event.version = WINE_ARM64EC_CODE_OBSERVER_VERSION;
    code_event.size = sizeof(code_event);
    code_event.operation = WINE_ARM64EC_CODE_MAP;
    code_event.status = STATUS_UNSUCCESSFUL;
    code_observer_complete( &provider, code_transaction, &code_event );
    check( !provider.poison_status && !provider.mutating,
           "observer reentrancy test poisoned or stranded provider" );
}

static NTSTATUS begin_competing_observer( BOOL code, void **transaction )
{
    if (code) return code_observer_begin( &provider, WINE_ARM64EC_CODE_MAP, transaction );
    return low_observer_begin( &provider, WINE_WOW64_MEMORY_MAP,
                               WINE_LOW_VA_SHADOW_BASE + TEST_PAGE, TEST_PAGE,
                               WINE_LOW_VA_SHADOW_BASE + TEST_PAGE, transaction );
}

static void cancel_competing_observer( BOOL code, void *transaction )
{
    if (code)
    {
        struct wine_arm64ec_code_event_v1 event = {0};

        event.version = WINE_ARM64EC_CODE_OBSERVER_VERSION;
        event.size = sizeof(event);
        event.operation = WINE_ARM64EC_CODE_MAP;
        event.status = STATUS_UNSUCCESSFUL;
        code_observer_complete( &provider, transaction, &event );
    }
    else
    {
        struct wine_arm64ec_low_memory_event_v1 event = {0};

        event.version = WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION;
        event.size = sizeof(event);
        event.operation = WINE_WOW64_MEMORY_MAP;
        event.status = STATUS_UNSUCCESSFUL;
        event.host_address = WINE_LOW_VA_SHADOW_BASE + TEST_PAGE;
        event.size_covered = TEST_PAGE;
        event.host_allocation_base = event.host_address;
        low_observer_complete( &provider, transaction, &event );
    }
}

struct competing_observer
{
    BOOL code;
    atomic_int finished;
    NTSTATUS status;
    uint64_t generation;
};

static void *run_competing_observer( void *argument )
{
    struct competing_observer *test = argument;
    void *transaction = NULL;

    test->status = begin_competing_observer( test->code, &transaction );
    if (!test->status)
    {
        if (test->code)
            test->generation = ((struct code_observer_transaction *)transaction)->generation;
        else
            test->generation = ((struct low_observer_transaction *)transaction)->generation;
        cancel_competing_observer( test->code, transaction );
    }
    atomic_store_explicit( &test->finished, 1, memory_order_release );
    return NULL;
}

static void test_competing_observers(void)
{
    const struct timespec delay = {0, 1000000};
    unsigned int owner, waiter, elapsed;

    for (owner = 0; owner < 2; ++owner)
    for (waiter = 0; waiter < 2; ++waiter)
    {
        struct competing_observer test = {0};
        void *transaction = NULL;
        uint64_t generation;
        pthread_t thread;
        NTSTATUS status;
        int ret;

        status = begin_competing_observer( owner, &transaction );
        check( !status && transaction, "cannot acquire owner observer transaction" );
        if (status) continue;
        generation = provider.generation;
        test.code = waiter;
        atomic_init( &test.finished, 0 );
        atomic_store_explicit( &mutation_waiting, 0, memory_order_release );
        ret = pthread_create( &thread, NULL, run_competing_observer, &test );
        check( !ret, "cannot create competing observer thread" );
        if (!ret)
        {
            /* Wait for the actual condition-variable boundary, or the old
             * incorrect early return. No timing assumption establishes order. */
            for (elapsed = 0; elapsed < TEST_TIMEOUT_MS; ++elapsed)
            {
                if (atomic_load_explicit( &mutation_waiting, memory_order_acquire ) ||
                    atomic_load_explicit( &test.finished, memory_order_acquire )) break;
                nanosleep( &delay, NULL );
            }
            check( atomic_load_explicit( &mutation_waiting, memory_order_acquire ) &&
                   !atomic_load_explicit( &test.finished, memory_order_acquire ),
                   "competing observer did not wait for the mutation owner" );
            pthread_mutex_lock( &provider.mutex );
            check( current_thread_owns_mutation_locked() && provider.generation == generation &&
                   (owner ? (void *)provider.code_transaction :
                            (void *)provider.low_transaction) == transaction,
                   "competing observer changed the active transaction" );
            pthread_mutex_unlock( &provider.mutex );
        }
        cancel_competing_observer( owner, transaction );
        if (!ret)
        {
            pthread_join( thread, NULL );
            if (test.status)
                fprintf( stderr, "competing observer owner=%u waiter=%u status=%#x\n",
                         owner, waiter, (unsigned int)test.status );
            check( !test.status && test.generation > generation,
                   "competing observer did not acquire a new transaction generation" );
        }
        check( !provider.poison_status && !provider.mutating &&
               !provider.low_transaction && !provider.code_transaction,
               "competing observer left a poisoned or stranded transaction" );
    }
}

static void test_low_observer_mapping(void)
{
    static const uint64_t low_guest = 0x00100000;
    const uint64_t low_host = WINE_LOW_VA_SHADOW_BASE + low_guest;
    struct wine_arm64ec_low_memory_range_v1 range;
    struct wine_arm64ec_low_memory_event_v1 event;
    struct xtajit64_memory_translate_params translation;
    void *transaction = NULL;
    NTSTATUS status;

    status = low_observer_begin( &provider, WINE_WOW64_MEMORY_MAP,
                                 low_host, TEST_PAGE, low_host, &transaction );
    check( !status && transaction, "LOW observer map begin failed" );
    if (status) return;
    memset( &range, 0, sizeof(range) );
    range.host_address = low_host;
    range.size = TEST_PAGE;
    range.host_allocation_base = low_host;
    range.state = MEM_COMMIT;
    range.protect = PAGE_READWRITE;
    memset( &event, 0, sizeof(event) );
    event.version = WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_MAP;
    event.host_address = low_host;
    event.size_covered = TEST_PAGE;
    event.host_allocation_base = low_host;
    event.ranges = &range;
    event.range_count = 1;
    low_observer_complete( &provider, transaction, &event );
    check( !provider.poison_status, "LOW observer map poisoned provider" );

    memset( &translation, 0, sizeof(translation) );
    translation.address = low_guest;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    status = memory_translate( &translation );
    check( !status && translation.guest == low_guest &&
           translation.host == low_host &&
           translation.allocation_base == low_guest &&
           translation.domain == XTAJIT64_MEMORY_ADDRESS_AMD64_LOW,
           "LOW guest-to-shadow translation was not exact" );

    memset( &translation, 0, sizeof(translation) );
    translation.address = low_host;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_HOST_TO_GUEST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ;
    status = memory_translate( &translation );
    check( !status && translation.guest == low_guest &&
           translation.host == low_host &&
           translation.allocation_base == low_guest &&
           translation.domain == XTAJIT64_MEMORY_ADDRESS_AMD64_LOW,
           "LOW shadow-to-guest translation was not exact" );

    transaction = NULL;
    status = low_observer_begin( &provider, WINE_WOW64_MEMORY_PROTECT,
                                 low_host, TEST_PAGE, low_host, &transaction );
    check( !status && transaction, "LOW observer protect begin failed" );
    if (status) return;
    range.protect = PAGE_READONLY;
    event.operation = WINE_WOW64_MEMORY_PROTECT;
    low_observer_complete( &provider, transaction, &event );
    check( !provider.poison_status, "LOW observer protect poisoned provider" );
    memset( &translation, 0, sizeof(translation) );
    translation.address = low_guest;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    check( memory_translate( &translation ) == STATUS_INVALID_ADDRESS,
           "LOW observer protect retained write permission" );

    transaction = NULL;
    status = low_observer_begin( &provider, WINE_WOW64_MEMORY_UNMAP,
                                 low_host, TEST_PAGE, low_host, &transaction );
    check( !status && transaction, "LOW observer unmap begin failed" );
    if (status) return;
    range.host_allocation_base = 0;
    range.state = MEM_FREE;
    range.protect = PAGE_NOACCESS;
    event.operation = WINE_WOW64_MEMORY_UNMAP;
    event.host_allocation_base = 0;
    low_observer_complete( &provider, transaction, &event );
    check( !provider.poison_status, "LOW observer unmap poisoned provider" );
    memset( &translation, 0, sizeof(translation) );
    translation.address = low_guest;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ;
    check( memory_translate( &translation ) == STATUS_INVALID_ADDRESS,
           "LOW observer unmap retained a guest mapping" );
}

static void test_identity_registry_mutations(void)
{
    const uint64_t address = test_base + 6 * TEST_PAGE;
    struct xtajit64_memory_translate_params translation;
    struct xtajit64_memory_params params;
    NTSTATUS status;

    memset( &params, 0, sizeof(params) );
    params.guest = address;
    params.size = TEST_PAGE;
    params.protect = PAGE_READONLY;
    status = memory_protect( &params );
    check( !status, "identity mapping protect failed" );
    memset( &translation, 0, sizeof(translation) );
    translation.address = address;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    check( memory_translate( &translation ) == STATUS_INVALID_ADDRESS,
           "identity mapping protect retained write permission" );
    memset( &translation, 0, sizeof(translation) );
    translation.address = address;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ;
    status = memory_translate( &translation );
    check( !status && translation.host == address &&
           translation.domain == XTAJIT64_MEMORY_ADDRESS_IDENTITY,
           "identity read translation failed after protect" );

    params.protect = PAGE_READWRITE;
    check( !memory_protect( &params ), "identity write permission restore failed" );
    params.guest = test_base + 7 * TEST_PAGE;
    params.size = TEST_PAGE;
    params.protect = 0;
    check( !memory_unmap( &params ), "identity mapping unmap failed" );
    memset( &translation, 0, sizeof(translation) );
    translation.address = params.guest;
    translation.size = TEST_PAGE;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ;
    check( memory_translate( &translation ) == STATUS_INVALID_ADDRESS,
           "identity mapping unmap retained a translation" );
    check( !map_identity_page( params.guest, PAGE_READWRITE ),
           "identity mapping remap failed" );
}

static void test_code_observer_invalidation(void)
{
    struct wine_arm64ec_code_range_v1 range;
    struct wine_arm64ec_code_event_v1 event;
    void *transaction = NULL;
    NTSTATUS status;

    status = code_observer_begin( &provider, WINE_ARM64EC_CODE_MAP,
                                  &transaction );
    check( !status && transaction, "code observer map begin failed" );
    if (status) return;
    range.address = test_base;
    range.size = TEST_PAGE;
    memset( &event, 0, sizeof(event) );
    event.version = WINE_ARM64EC_CODE_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_ARM64EC_CODE_MAP;
    event.ranges = &range;
    event.range_count = 1;
    code_observer_complete( &provider, transaction, &event );
    check( !provider.poison_status, "code observer map poisoned provider" );
}

static void test_unaligned_signal_bridge( volatile uint32_t *doorbell )
{
    struct xtajit64_begin_params params;
    struct wine_arm64ec_jit_host_context_v1 host = {0};
    struct sigaction action = {0}, old_bus;
    unsigned char code[32] = {0x48, 0x89, 0x06}; /* mov [rsi],rax */
    unsigned char *destination = test_pages + 6 * TEST_PAGE + 15;
    const uint64_t value = 0x123456789abcdef0ull;
    uint64_t actual;
    size_t offset = 3;
    unsigned int iteration;
    NTSTATUS status;

    host.size = sizeof(host);
    host.version = WINE_ARM64EC_JIT_HOST_CONTEXT_VERSION;
    host.pc = test_base;
    check( registered_signal_observer &&
           registered_signal_observer->repair_jit_fault(
               registered_signal_observer->context, &host, 0, 0, 0 ) == STATUS_NOT_SUPPORTED,
           "idle signal observer accepted a host context" );
    actual = UINT64_MAX;
    check( registered_signal_observer &&
           registered_signal_observer->query_exception_stack(
               registered_signal_observer->context, &host, 0, 0, &actual ) == STATUS_NOT_SUPPORTED &&
           actual == UINT64_MAX,
           "idle signal observer accepted a stack query or changed its output" );
    emit_jump_r11( code, &offset, target_address );
    check( !publish_code( test_base, code, offset ),
           "cannot publish dynamic unaligned store" );
    sigemptyset( &action.sa_mask );
    action.sa_sigaction = repair_test_sigbus;
    action.sa_flags = SA_SIGINFO;
    if (sigaction( SIGBUS, &action, &old_bus ))
    {
        check_failure( "cannot install unaligned signal test handler" );
        return;
    }
    signal_repairs = 0;
    for (iteration = 0; iteration < 2; ++iteration)
    {
        memset( destination, 0, sizeof(value) );
        init_begin_params( &params, test_base, doorbell );
        params.context.rax = value;
        params.context.rsi = (uintptr_t)destination;
        status = begin_simulation( &params );
        memcpy( &actual, destination, sizeof(actual) );
        check( !status && params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
               params.transition_target == target_address && actual == value,
               "signal repair did not resume to the exact EC boundary" );
        check( signal_repairs == 1, "unaligned site did not repair exactly once" );
        check( !__atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE ),
               "execution left a stale signal binding" );
    }
    check( !sigaction( SIGBUS, &old_bus, NULL ),
           "cannot restore unaligned signal test handler" );
}

static void test_execution_boundaries( volatile uint32_t *doorbell )
{
    struct xtajit64_begin_params params;
    unsigned char code[64];
    uint64_t expected_rip;
    size_t offset;
    NTSTATUS status;

    memset( code, 0, sizeof(code) );
    offset = 0;
    code[offset++] = 0x48;
    code[offset++] = 0xb8;
    emit_u64( code, &offset, 0x1122334455667788ull );
    emit_jump_r11( code, &offset, target_address );
    check( !publish_code( test_base, code, offset ), "cannot publish EC test code" );
    init_begin_params( &params, test_base, doorbell );
    status = begin_simulation( &params );
    check( !status && params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           params.transition_target == target_address &&
           params.context.rax == 0x1122334455667788ull,
           "EC transition did not preserve exact x64 state" );

    memset( code, 0, sizeof(code) );
    offset = 0;
    code[offset++] = 0xb8;
    code[offset++] = 1;
    code[offset++] = 0;
    code[offset++] = 0;
    code[offset++] = 0;
    code[offset++] = 0x0f;
    code[offset++] = 0x05;
    expected_rip = test_base + offset;
    check( !publish_code( test_base, code, offset ), "cannot publish syscall code" );
    init_begin_params( &params, test_base, doorbell );
    params.context.r10 = 0x123456789abcdef0ull;
    track_hidden_syscall = 1;
    hidden_imports = hidden_exports = 0;
    status = begin_simulation( &params );
    track_hidden_syscall = 0;
    check( !status && params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           params.transition_target == dispatcher_address &&
           params.context.rcx == 0x123456789abcdef0ull &&
           params.context.r10 == expected_rip,
           "valid syscall did not enter the exact ARM64EC dispatcher state" );
    {
        struct thread_binding *binding = pthread_getspecific( binding_key );
        struct switchyard_fex_x64_state after = {
            .size = sizeof(after), .version = SWITCHYARD_FEX_STATE_VERSION,
        };
        check( hidden_imports == 1 && hidden_exports == 1,
               "syscall did not use exactly one full-state exchange" );
        check( switchyard_fex_thread_export_state( binding->thread, &after ) == SWITCHYARD_FEX_OK &&
               after.fcw == expected_hidden.fcw && after.abridged_ftw == expected_hidden.abridged_ftw &&
               !memcmp( after.x87, expected_hidden.x87, sizeof(after.x87) ) &&
               !memcmp( after.ymm_high, expected_hidden.ymm_high, sizeof(after.ymm_high) ) &&
               !memcmp( after.segment, expected_hidden.segment, sizeof(after.segment) ) &&
               !memcmp( after.segment_base, expected_hidden.segment_base, sizeof(after.segment_base) ),
               "syscall lost full x87/segment/YMM state" );
    }

    memset( code, 0, sizeof(code) );
    offset = 0;
    code[offset++] = 0xb8;
    code[offset++] = TEST_SYSCALL_COUNT;
    code[offset++] = 0;
    code[offset++] = 0;
    code[offset++] = 0;
    code[offset++] = 0x0f;
    code[offset++] = 0x05;
    emit_jump_r11( code, &offset, target_address );
    check( !publish_code( test_base, code, offset ),
           "cannot publish invalid-syscall code" );
    init_begin_params( &params, test_base, doorbell );
    status = begin_simulation( &params );
    check( !status && params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           params.transition_target == target_address &&
           (NTSTATUS)params.context.rax == STATUS_INVALID_SYSTEM_SERVICE,
           "invalid syscall did not resume with Windows status" );

    code[0] = 0x0f;
    code[1] = 0x0b;
    check( !publish_code( test_base, code, 2 ), "cannot publish UD2 code" );
    init_begin_params( &params, test_base, doorbell );
    status = begin_simulation( &params );
    check( status == STATUS_ILLEGAL_INSTRUCTION &&
           params.stop_reason == XTAJIT64_STOP_INVALID_INSTRUCTION &&
           params.context.rip == test_base,
           "UD2 did not return an exact invalid-instruction boundary" );

    __atomic_store_n( (uint32_t *)doorbell, UINT32_MAX, __ATOMIC_RELEASE );
    init_begin_params( &params, test_base, doorbell );
    status = begin_simulation( &params );
    check( !status && params.stop_reason == XTAJIT64_STOP_SUSPEND,
           "prepublished suspend doorbell was not acknowledged" );
    __atomic_store_n( (uint32_t *)doorbell, 0, __ATOMIC_RELEASE );
}

struct running_test
{
    struct xtajit64_begin_params params;
    struct xtajit64_memory_params flush;
    atomic_int ready;
    NTSTATUS execute_status;
    NTSTATUS flush_status;
};

static void *run_infinite_guest( void *arg )
{
    struct running_test *test = arg;

    test->execute_status = thread_init( NULL );
    atomic_store_explicit( &test->ready, 1, memory_order_release );
    if (!test->execute_status) test->execute_status = begin_simulation( &test->params );
    thread_term( NULL );
    return NULL;
}

static void *run_flush( void *arg )
{
    struct running_test *test = arg;

    test->flush_status = flush_instruction_cache( &test->flush );
    return NULL;
}

struct warm_gate_lock
{
    atomic_int held;
    atomic_int release;
};

static void *hold_warm_gate_mutex( void *arg )
{
    struct warm_gate_lock *lock = arg;

    pthread_mutex_lock( &provider.mutex );
    atomic_store_explicit( &lock->held, 1, memory_order_release );
    /* Deliberate bounded test contention, not a runtime spin protocol. The
     * fixture's existing alarm/supervisor bounds a wrongly locked warm path. */
    while (!atomic_load_explicit( &lock->release, memory_order_acquire )) sched_yield();
    pthread_mutex_unlock( &provider.mutex );
    return NULL;
}

static void *copied_warm_owner( void *arg )
{
    struct thread_binding *binding = arg;
    struct xtajit64_begin_params params;
    uint64_t before = switchyard_fex_admission_load( binding->admission );
    uint64_t cache = binding->native_gate_epoch;

    /* Stronger than copying a PE descriptor: a test-only forged TSD slot still
     * must not authenticate another pthread or mutate the owner's cache/TLS. */
    check( !pthread_setspecific( binding_key, binding ), "cannot install foreign test-only TSD" );
    init_begin_params( &params, test_base, binding->doorbell );
    check( begin_simulation( &params ) == STATUS_INVALID_DEVICE_STATE &&
           switchyard_fex_admission_load( binding->admission ) == before &&
           binding->native_gate_epoch == cache && !active_signal_binding,
           "copied owner descriptor changed native gate authority" );
    check( !pthread_setspecific( binding_key, NULL ), "cannot clear foreign test-only TSD" );
    return NULL;
}

static void test_warm_native_gate( volatile uint32_t *doorbell )
{
    unsigned char code[23] = {0x48,0xb8,0,0,0,0,0,0,0,0,0x49,0xbb,0,0,0,0,0,0,0,0,0x41,0xff,0xe3};
    unsigned char saved_code[TEST_PAGE];
    struct warm_gate_lock lock = {0};
    struct xtajit64_begin_params params;
    struct thread_binding *binding = pthread_getspecific( binding_key );
    const uint64_t value = 0x1726354;
    uint64_t current_generation, word;
    unsigned int entries, completions, gates, index;
    pthread_t holder, foreign;

    if (test_custom_dispatch) return;
    memcpy( saved_code, (const void *)(uintptr_t)test_base, sizeof(saved_code) );
    memcpy( code + 2, &value, sizeof(value) );
    memcpy( code + 12, &target_address, sizeof(target_address) );
    check( !publish_code( test_base, code, sizeof(code) ), "cannot install exact warm native gate code" );
    init_begin_params( &params, test_base, doorbell );
    check( !begin_simulation( &params ) && params.context.rax == value &&
           params.stop_reason == XTAJIT64_STOP_EC_TRANSITION && binding->native_gate_epoch,
           "native gate cold attachment did not execute" );
    check( !pthread_create( &foreign, NULL, copied_warm_owner, binding ) &&
           !pthread_join( foreign, NULL ), "foreign native gate owner test failed" );
    entries = atomic_load( &fused_initial_calls );
    completions = atomic_load( &completion_calls );
    gates = atomic_load( &native_gate_calls );
    check( !pthread_create( &holder, NULL, hold_warm_gate_mutex, &lock ), "cannot hold provider mutex" );
    while (!atomic_load_explicit( &lock.held, memory_order_acquire )) sched_yield();
    for (index = 0; index < 32; ++index)
    {
        init_begin_params( &params, test_base, doorbell );
        check( !begin_simulation( &params ) && params.context.rax == value &&
               params.stop_reason == XTAJIT64_STOP_EC_TRANSITION && !binding_is_active( binding ) &&
               !active_signal_binding, "warm native gate did not release before callback" );
    }
    atomic_store_explicit( &lock.release, 1, memory_order_release );
    check( !pthread_join( holder, NULL ), "provider mutex holder did not finish" );
    check( atomic_load( &fused_initial_calls ) == entries && atomic_load( &completion_calls ) == completions &&
           atomic_load( &native_gate_calls ) == gates + 32,
           "warm native gate used cold entry/completion or skipped actual SDK" );
    pthread_mutex_lock( &provider.mutex );
    current_generation = provider_mapping_generation();
    word = switchyard_fex_admission_load( binding->admission );
    __atomic_store_n( &provider.generation, UINT64_MAX, __ATOMIC_RELEASE );
    check( begin_mutation_locked() == STATUS_INTEGER_OVERFLOW && !provider.mutating &&
           switchyard_fex_admission_load( binding->admission ) == word,
           "mapping generation wrap changed closure/authority" );
    /* Exclusive, quiescent fixture-only restoration, not a runtime operation. */
    __atomic_store_n( &provider.generation, current_generation, __ATOMIC_RELEASE );
    pthread_mutex_unlock( &provider.mutex );
    check( !publish_code( test_base, saved_code, sizeof(saved_code) ),
           "warm native gate did not restore the exact existing fixture page" );
}

static void test_quiescent_invalidation( volatile uint32_t *doorbell )
{
    struct running_test test;
    unsigned char loop[] = {0xeb, 0xfe};
    pthread_t execution_thread, flush_thread;
    int ret;

    memset( &test, 0, sizeof(test) );
    check( !publish_code( loop_address, loop, sizeof(loop) ),
           "cannot publish backward-loop code" );
    init_begin_params( &test.params, loop_address, doorbell );
    test.flush.guest = loop_address;
    test.flush.size = TEST_PAGE;
    atomic_store_explicit( &hold_mutation, 1, memory_order_release );
    atomic_store_explicit( &mutation_quiesced, 0, memory_order_release );
    atomic_store_explicit( &release_mutation, 0, memory_order_release );

    ret = pthread_create( &execution_thread, NULL, run_infinite_guest, &test );
    check( !ret, "cannot create FEX execution thread" );
    if (ret) goto done;
    check( wait_atomic_value( &test.ready, 1, TEST_TIMEOUT_MS ),
           "FEX execution thread did not initialize" );
    ret = pthread_create( &flush_thread, NULL, run_flush, &test );
    check( !ret, "cannot create FEX mutation thread" );
    if (ret)
    {
        __atomic_store_n( (uint32_t *)doorbell, UINT32_MAX, __ATOMIC_RELEASE );
        pthread_join( execution_thread, NULL );
        goto done;
    }
    check( wait_atomic_value( &mutation_quiesced, 1, TEST_TIMEOUT_MS ),
           "mutation did not quiesce active FEX execution" );
    check( __atomic_load_n( (uint32_t *)doorbell, __ATOMIC_ACQUIRE ) ==
               0,
           "internal request was not cleared before backing mutation" );
    {
        struct thread_binding *binding;
        /* The release/acquire hook holds mutation here, so membership is
         * stable. Verify closure is the authority replacing that token. */
        for (binding = provider.bindings; binding; binding = binding->next)
            check( (switchyard_fex_admission_load( binding->admission ) &
                    SWITCHYARD_FEX_ADMISSION_FLAGS) == SWITCHYARD_FEX_ADMISSION_CLOSED,
                   "clear request did not retain CLOSED+idle admission" );
    }
    __atomic_store_n( (uint32_t *)doorbell, UINT32_MAX, __ATOMIC_RELEASE );
    atomic_store_explicit( &release_mutation, 1, memory_order_release );
    pthread_join( flush_thread, NULL );
    pthread_join( execution_thread, NULL );
    check( !test.flush_status && !test.execute_status &&
           test.params.stop_reason == XTAJIT64_STOP_SUSPEND,
           "quiescent invalidation lost the external suspend request" );

done:
    atomic_store_explicit( &release_mutation, 1, memory_order_release );
    atomic_store_explicit( &hold_mutation, 0, memory_order_release );
    __atomic_store_n( (uint32_t *)doorbell, 0, __ATOMIC_RELEASE );
}

static void test_early_wake_replay( volatile uint32_t *doorbell )
{
    struct running_test test = {0};
    unsigned char loop[] = {0xeb, 0xfe};
    pthread_t execution_thread, flush_thread;
    int ret;

    check( !publish_code( loop_address, loop, sizeof(loop) ), "cannot publish replay loop" );
    init_begin_params( &test.params, loop_address, doorbell );
    test.flush.guest = loop_address;
    test.flush.size = TEST_PAGE;
    atomic_store_explicit( &prepared_count, 0, memory_order_release );
    atomic_store_explicit( &wake_observed, 0, memory_order_release );
    atomic_store_explicit( &release_wake, 0, memory_order_release );
    atomic_store_explicit( &count_prepared, 1, memory_order_release );
    atomic_store_explicit( &hold_wake, 1, memory_order_release );
    ret = pthread_create( &execution_thread, NULL, run_infinite_guest, &test );
    check( !ret, "cannot create early-wake executor" );
    if (ret) goto done;
    check( wait_atomic_value( &prepared_count, 1, TEST_TIMEOUT_MS ), "execution was not admitted" );
    ret = pthread_create( &flush_thread, NULL, run_flush, &test );
    check( !ret, "cannot create early-wake mutator" );
    if (ret) goto stop;
    check( wait_atomic_value( &wake_observed, 1, TEST_TIMEOUT_MS ), "shared closure did not wake mutator" );
    /* Hold the executor AFTER releasing admission but BEFORE provider stop
     * classification. The mutator clears its transient pause ownership first. */
    pthread_join( flush_thread, NULL );
    check( !test.flush_status && !__atomic_load_n( (uint32_t *)doorbell, __ATOMIC_ACQUIRE ),
           "mutation did not finish before executor completion" );
    atomic_store_explicit( &release_wake, 1, memory_order_release );
    check( wait_atomic_value( &prepared_count, 2, TEST_TIMEOUT_MS ),
           "early wake leaked an internal suspension instead of replaying" );
stop:
    atomic_store_explicit( &release_wake, 1, memory_order_release );
    __atomic_store_n( (uint32_t *)doorbell, UINT32_MAX, __ATOMIC_RELEASE );
    pthread_join( execution_thread, NULL );
    check( !test.execute_status && test.params.stop_reason == XTAJIT64_STOP_SUSPEND,
           "replayed execution did not honor later external suspension" );
done:
    atomic_store_explicit( &count_prepared, 0, memory_order_release );
    atomic_store_explicit( &hold_wake, 0, memory_order_release );
    __atomic_store_n( (uint32_t *)doorbell, 0, __ATOMIC_RELEASE );
}

static void test_fork_contract( const struct xtajit64_process_init_params *params,
                                volatile uint32_t *doorbell )
{
    struct xtajit64_begin_params begin;
    struct xtajit64_process_init_params child_params;
    pid_t child;
    int child_status;
    NTSTATUS status;

    child = fork();
    check( child >= 0, "fork failed" );
    if (!child)
    {
        child_params = *params;
        child_params.enabled_capabilities = 0;
        child_params.native_page_size = 0;
        status = process_init( &child_params );
        _exit( status == STATUS_NOT_SUPPORTED ? 0 : 1 );
    }
    if (child < 0) return;
    check( waitpid( child, &child_status, 0 ) == child &&
           WIFEXITED( child_status ) && !WEXITSTATUS( child_status ),
           "fork child reused inherited FEX process state" );

    init_begin_params( &begin, test_base, doorbell );
    status = begin_simulation( &begin );
    check( status == STATUS_ILLEGAL_INSTRUCTION &&
           begin.stop_reason == XTAJIT64_STOP_INVALID_INSTRUCTION,
           "fork parent did not retain a usable provider" );
}

int main(void)
{
    struct xtajit64_control_stack_params control_params;
    struct xtajit64_process_init_params process_params;
    struct xtajit64_process_init_params invalid_params;
    struct xtajit64_memory_translate_params translation;
    volatile uint32_t *doorbell;
    uint64_t highest, bitmap_words;
    size_t i;
    NTSTATUS status;

    signal( SIGALRM, timeout_handler );
    test_custom_dispatch = !!getenv( "XTAJIT64_FEX_TEST_CUSTOM_DISPATCH" );
    test_direct_dispatch = !!getenv( "XTAJIT64_FEX_TEST_DIRECT_DISPATCH" );
    if (test_custom_dispatch)
    {
        if (__builtin_available(macOS 26.4, *))
        {
            test_set_custom_x18 = os_set_custom_x18_abi_enabled;
            test_custom_x18_enabled = os_custom_x18_abi_enabled;
        }
        else return 77;
    }
    alarm( 45 );
    test_guest_exception_mapping();
    test_state_input_initialization();
    test_state_output_publication();
    test_pages = alloc_pages_at( TEST_IDENTITY_BASE, TEST_PAGE_COUNT );
    if (!test_pages) test_pages = alloc_pages_at( TEST_FALLBACK_BASE, TEST_PAGE_COUNT );
    test_kuser = alloc_pages_at( TEST_KUSER_BASE, 1 );
    check( test_pages && test_kuser, "cannot allocate deterministic test pages" );
    if (!test_pages || !test_kuser) return 1;

    test_base = (uintptr_t)test_pages;
    target_address = test_base + TEST_PAGE;
    dispatcher_address = test_base + 2 * TEST_PAGE;
    teb_address = test_base + 3 * TEST_PAGE;
    stack_address = test_base + 4 * TEST_PAGE;
    loop_address = test_base + 5 * TEST_PAGE;
    doorbell = (volatile uint32_t *)(uintptr_t)(stack_address + 64);
    highest = test_base + TEST_PAGE_COUNT * TEST_PAGE - 1;
    bitmap_words = (highest / TEST_PAGE) / 64 + 1;
    check( bitmap_words <= SIZE_MAX / sizeof(*ec_bitmap),
           "EC bitmap size overflow" );
    ec_bitmap_size = (size_t)bitmap_words * sizeof(*ec_bitmap);
    ec_bitmap = calloc( 1, ec_bitmap_size );
    check( !!ec_bitmap, "cannot allocate EC bitmap" );
    if (!ec_bitmap) return 1;

    memset( &process_params, 0, sizeof(process_params) );
    process_params.ec_bitmap = (uintptr_t)ec_bitmap;
    process_params.highest_user_address = highest;
    process_params.guest_kuser = XTAJIT64_GUEST_KUSER;
    process_params.host_kuser = (uintptr_t)test_kuser;
    process_params.kuser_size = TEST_PAGE;
    process_params.rtl_exit_user_thread = target_address;
    process_params.abi_version = XTAJIT64_PROCESS_ABI_VERSION;
    process_params.abi_size = sizeof(process_params);
    process_params.required_capabilities = XTAJIT64_CAPABILITIES;
    process_params.x64_syscall_dispatcher = dispatcher_address;
    process_params.x64_syscall_count = TEST_SYSCALL_COUNT;
    if (test_custom_dispatch) process_params.reserved = XTAJIT64_PROCESS_CUSTOM_DISPATCH;
    invalid_params = process_params;
    invalid_params.abi_version--;
    check( process_init( &invalid_params ) == STATUS_REVISION_MISMATCH &&
           !provider.initialized, "obsolete process ABI was accepted" );
    invalid_params = process_params;
    invalid_params.abi_size = offsetof(struct xtajit64_process_init_params, native_page_size);
    check( process_init( &invalid_params ) == STATUS_REVISION_MISMATCH &&
           !provider.initialized, "truncated process ABI was accepted" );
    invalid_params = process_params;
    invalid_params.native_page_size = XTAJIT64_GUEST_PAGE_SIZE;
    check( process_init( &invalid_params ) == STATUS_REVISION_MISMATCH &&
           !provider.initialized, "PE-supplied native page size was accepted" );
    /* IDs were once diagnostic-only. They now authenticate execution tokens
     * and must fail closed rather than recycling an older process/binding. */
    provider.instance = UINT64_MAX;
    check( process_init( &process_params ) == STATUS_INTEGER_OVERFLOW &&
           !provider.initialized && !provider.process && !provider.ranges.count,
           "process generation wrap was accepted or leaked initialization" );
    provider.instance = 0; /* fixture-only restoration before first publication */
    status = process_init( &process_params );
    if (status)
        fprintf( stderr, "process init status %#x capabilities %#x poison %#x "
                 "observers %u/%u\n", (unsigned int)status,
                 process_params.enabled_capabilities,
                 (unsigned int)provider.poison_status,
                 provider.observer_active, provider.code_observer_active );
    check( !status && process_params.enabled_capabilities == XTAJIT64_CAPABILITIES,
           "FEX provider process initialization failed" );
    check( !status && process_params.native_page_size == (uint64_t)sysconf( _SC_PAGESIZE ),
           "FEX provider did not report the native protection granularity" );
    if (status) return 1;

    test_control_allocation = mmap( NULL, XTAJIT64_CONTROL_STACK_SIZE,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    check( test_control_allocation != MAP_FAILED, "cannot allocate native control-stack fixture" );
    if (test_control_allocation == MAP_FAILED) return 1;
    control_params.allocation = (uintptr_t)test_control_allocation;
    control_params.size = XTAJIT64_CONTROL_STACK_SIZE;
    check( !control_stack_protect( &control_params ), "cannot protect owned native control stack" );
    check( !munmap( test_control_allocation, XTAJIT64_CONTROL_STACK_SIZE ), "cannot release native control-stack fixture" );
    test_control_allocation = NULL;

    test_observer_reentrancy();
    test_competing_observers();
    test_low_observer_mapping();
    for (i = 0; i < TEST_PAGE_COUNT; ++i)
    {
        unsigned int protect = PAGE_READWRITE;

        if (i == 0 || i == 1 || i == 2 || i == 5)
            protect = PAGE_EXECUTE_READWRITE;
        status = map_identity_page( test_base + i * TEST_PAGE, protect );
        check( !status, "cannot register identity test page" );
    }
    set_ec_page( target_address, TRUE );
    set_ec_page( dispatcher_address, TRUE );
    check( !flush_code_page( target_address ) &&
           !flush_code_page( dispatcher_address ),
           "cannot publish EC bitmap classification" );
    test_identity_registry_mutations();
    test_code_observer_invalidation();

    memset( &translation, 0, sizeof(translation) );
    translation.address = stack_address + TEST_PAGE - 4;
    translation.size = 8;
    translation.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                        XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ;
    check( memory_translate( &translation ) == STATUS_INVALID_ADDRESS,
           "cross-page translation escaped the 16-KiB mapped boundary" );

    provider.next_binding_id = UINT64_MAX;
    check( thread_init( NULL ) == STATUS_INTEGER_OVERFLOW && !provider.bindings,
           "binding generation wrap was accepted or published a partial thread" );
    provider.next_binding_id = 0; /* fixture-only restoration at quiescence */
    status = thread_init( NULL );
    check( !status, "cannot create main FEX thread" );
    if (!status)
    {
        test_direct_binding();
        test_unaligned_signal_bridge( doorbell );
        test_execution_boundaries( doorbell );
        test_warm_native_gate( doorbell );
        test_quiescent_invalidation( doorbell );
        test_early_wake_replay( doorbell );
        test_fork_contract( &process_params, doorbell );
        test_retirement_failure();
        check( !thread_term( NULL ), "main FEX thread teardown failed" );
    }
    check( !process_term( NULL ), "FEX process teardown failed" );

    munmap( test_pages, TEST_PAGE_COUNT * TEST_PAGE );
    munmap( test_kuser, TEST_PAGE );
    free( ec_bitmap );
    alarm( 0 );
    check( test_custom_dispatch ? !atomic_load( &fused_initial_calls ) :
           !!atomic_load( &fused_initial_calls ),
           "fused ordinary entry/custom isolation was not exercised" );
    check( test_custom_dispatch ? !atomic_load( &completion_calls ) :
           !!atomic_load( &completion_calls ),
           "owned completion/custom isolation was not exercised" );
    check( test_custom_dispatch ? !atomic_load( &native_gate_calls ) :
           !!atomic_load( &native_gate_calls ),
           "Unix-owned native gate/custom isolation was not exercised" );
    if (failures) return 1;
    printf( "XTAJIT64_FEX_UNIXLIB_CONTRACT_PASS\n" );
    return 0;
}
