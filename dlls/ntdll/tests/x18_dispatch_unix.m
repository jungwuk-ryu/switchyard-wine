/*
 * Darwin ARM64 system-x18 dispatcher test helper
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <sched.h>
#include <string.h>

#if defined(__APPLE__) && defined(__aarch64__) && defined(HAVE_OS_CUSTOM_X18_ABI)
# include <os/arch/arm64.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#undef WIN32_NO_STATUS
#include "winternl.h"
#include "wine/asm.h"

#include "../unixlib.h"
#include "x18_dispatch_test.h"

static LONG wow64_unixlib_zero_args_calls;

#define WOW64_UNIXLIB_LIFECYCLE_VARIANTS 3

struct wow64_unixlib_lifecycle_native_state
{
    LONG sequence;
    LONG quiesce_calls;
    LONG unbind_calls;
    LONG entry_sequence;
    LONG quiesce_sequence;
    LONG exit_sequence;
    LONG unbind_sequence;
    LONG flags;
    NTSTATUS quiesce_status;
    NTSTATUS unbind_status;
    NTSTATUS reenter_status;
    UINT64 release_event;
};

static struct wow64_unixlib_lifecycle_native_state
    wow64_unixlib_lifecycle_states[WOW64_UNIXLIB_LIFECYCLE_VARIANTS];
static const struct wine_unixlib_dispatch_source_v2
    wow64_unixlib_lifecycle_sources[WOW64_UNIXLIB_LIFECYCLE_VARIANTS];
static NTSTATUS wow64_unixlib_lifecycle_quiesce_0(void);
static NTSTATUS wow64_unixlib_lifecycle_quiesce_1(void);
static NTSTATUS wow64_unixlib_lifecycle_quiesce_2(void);
static NTSTATUS wow64_unixlib_lifecycle_unbind_0(void);
static NTSTATUS wow64_unixlib_lifecycle_unbind_1(void);
static NTSTATUS wow64_unixlib_lifecycle_unbind_2(void);
extern NTSTATUS x18_dispatch_illegal_instruction_func( void *args );

static NTSTATUS (*const wow64_unixlib_lifecycle_quiesce_funcs[])(void) =
{
    wow64_unixlib_lifecycle_quiesce_0,
    wow64_unixlib_lifecycle_quiesce_1,
    wow64_unixlib_lifecycle_quiesce_2,
};

static NTSTATUS (*const wow64_unixlib_lifecycle_unbind_funcs[])(void) =
{
    wow64_unixlib_lifecycle_unbind_0,
    wow64_unixlib_lifecycle_unbind_1,
    wow64_unixlib_lifecycle_unbind_2,
};

static LONG wow64_unixlib_lifecycle_next_sequence(
    struct wow64_unixlib_lifecycle_native_state *state )
{
    return __atomic_add_fetch( &state->sequence, 1, __ATOMIC_SEQ_CST );
}

static NTSTATUS wow64_unixlib_lifecycle_zero_args_func( void *args )
{
    struct ntdll_wow64_unixlib_call_context context;
    NTSTATUS status;

    if (args) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_get_unixlib_call_context( &context ))) return status;
    if (context.guest_args || context.args_size ||
        context.flags != WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED)
        return STATUS_INVALID_PARAMETER;
    __atomic_add_fetch( &wow64_unixlib_zero_args_calls, 1, __ATOMIC_RELAXED );
    return STATUS_SUCCESS;
}

static NTSTATUS x18_dispatch_get_zero_count_func( void *args )
{
    struct wow64_unixlib_zero_count_result *result = args;

    if (!result) return STATUS_INVALID_PARAMETER;
    result->count = __atomic_load_n( &wow64_unixlib_zero_args_calls, __ATOMIC_RELAXED );
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_unixlib_lifecycle_context_func( void *args )
{
    const struct wow64_unixlib_context_params *params = args;
    struct ntdll_wow64_unixlib_call_context context;
    struct wow64_unixlib_context_result result;
    void *result_host;
    NTSTATUS status;

    if (!params) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_get_unixlib_call_context( &context ))) return status;
    if ((ULONG_PTR)context.guest_args > MAXDWORD) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_guest32_to_host( params->result, &result_host ))) return status;
    result.guest_args = (ULONG_PTR)context.guest_args;
    result.args_size = context.args_size;
    result.flags = context.flags;
    result.value = params->value;
    return ntdll_wow64_atomic_write_user( result_host, &result, sizeof(result) );
}

static NTSTATUS wow64_unixlib_lifecycle_checked_fault_func( void *args )
{
    const struct wow64_unixlib_checked_fault_params *params = args;
    struct ntdll_wow64_unixlib_call_context before, after;
    struct wow64_unixlib_checked_fault_result result;
    void *noaccess_host, *result_host;
    BYTE value;
    NTSTATUS status;

    if (!params) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_get_unixlib_call_context( &before ))) return status;
    if ((status = ntdll_wow64_guest32_to_host( params->noaccess, &noaccess_host ))) return status;
    if ((status = ntdll_wow64_guest32_to_host( params->result, &result_host ))) return status;
    result.fault_status = ntdll_wow64_copy_from_user( &value, noaccess_host, sizeof(value) );
    status = ntdll_wow64_get_unixlib_call_context( &after );
    result.guest_args_before = (ULONG_PTR)before.guest_args;
    result.guest_args_after = status ? 0 : (ULONG_PTR)after.guest_args;
    result.context_preserved = !status && before.guest_args == after.guest_args &&
                               before.args_size == after.args_size &&
                               before.flags == after.flags;
    return ntdll_wow64_atomic_write_user( result_host, &result, sizeof(result) );
}

static NTSTATUS wow64_unixlib_lifecycle_block_func( void *args )
{
    const struct wow64_unixlib_block_params *params = args;
    struct wow64_unixlib_lifecycle_native_state *state =
        &wow64_unixlib_lifecycle_states[0];
    HANDLE entered_event, release_event;
    NTSTATUS status;

    if (!params) return STATUS_INVALID_PARAMETER;
    entered_event = (HANDLE)(ULONG_PTR)params->entered_event;
    release_event = (HANDLE)(ULONG_PTR)params->release_event;
    if (!entered_event || !release_event) return STATUS_INVALID_HANDLE;

    __atomic_store_n( &state->release_event, params->release_event, __ATOMIC_RELEASE );
    __atomic_store_n( &state->entry_sequence,
                      wow64_unixlib_lifecycle_next_sequence( state ), __ATOMIC_RELEASE );
    if ((status = NtSetEvent( entered_event, NULL ))) return status;
    status = NtWaitForSingleObject( release_event, FALSE, NULL );
    __atomic_store_n( &state->exit_sequence,
                      wow64_unixlib_lifecycle_next_sequence( state ), __ATOMIC_RELEASE );
    __atomic_store_n( &state->release_event, 0, __ATOMIC_RELEASE );
    return status;
}

static NTSTATUS wow64_unixlib_lifecycle_self_unload_func( void *args )
{
    const struct wow64_unixlib_self_unload_params *params = args;

    if (!params) return STATUS_INVALID_PARAMETER;
    return NtQueryVirtualMemory( NtCurrentProcess(), &params->module,
                                 MemoryWineUnloadUnixLib, NULL, 0, NULL );
}

static NTSTATUS wow64_unixlib_lifecycle_self_unregister_func( void *args )
{
    const struct wow64_unixlib_self_unregister_params *params = args;

    if (!params) return STATUS_INVALID_PARAMETER;
    return ntdll_wow64_unregister_unixlib_dispatch( params->handle );
}

static NTSTATUS wow64_unixlib_lifecycle_self_unregister_fault_func( void *args )
{
    NTSTATUS status = wow64_unixlib_lifecycle_self_unregister_func( args );

    if (status) return status;
    return x18_dispatch_illegal_instruction_func( NULL );
}

static NTSTATUS wow64_unixlib_lifecycle_quiesce_common( UINT32 variant )
{
    struct wow64_unixlib_lifecycle_native_state *state;
    unixlib_handle_t handle = 0;
    UINT64 release_event;
    LONG flags;

    if (variant >= WOW64_UNIXLIB_LIFECYCLE_VARIANTS) return STATUS_INVALID_PARAMETER;
    state = &wow64_unixlib_lifecycle_states[variant];
    __atomic_add_fetch( &state->quiesce_calls, 1, __ATOMIC_SEQ_CST );
    __atomic_store_n( &state->quiesce_sequence,
                      wow64_unixlib_lifecycle_next_sequence( state ), __ATOMIC_RELEASE );
    flags = __atomic_load_n( &state->flags, __ATOMIC_ACQUIRE );
    if (flags & WOW64_UNIXLIB_LIFECYCLE_QUIESCE_REENTER)
    {
        NTSTATUS status = ntdll_wow64_register_unixlib_dispatch_v2(
            &wow64_unixlib_lifecycle_sources[variant],
            wow64_unixlib_lifecycle_sources[variant].funcs,
            wow64_unixlib_lifecycle_quiesce_funcs[variant],
            wow64_unixlib_lifecycle_unbind_funcs[variant], &handle );

        __atomic_store_n( &state->reenter_status, status, __ATOMIC_RELEASE );
        if (!status) ntdll_wow64_unregister_unixlib_dispatch( handle );
    }
    release_event = __atomic_load_n( &state->release_event, __ATOMIC_ACQUIRE );
    if (release_event) NtSetEvent( (HANDLE)(ULONG_PTR)release_event, NULL );
    if (flags & WOW64_UNIXLIB_LIFECYCLE_QUIESCE_FAULT)
        *(volatile LONG *)(ULONG_PTR)0 = 0;
    return __atomic_load_n( &state->quiesce_status, __ATOMIC_ACQUIRE );
}

static NTSTATUS wow64_unixlib_lifecycle_unbind_common( UINT32 variant )
{
    struct wow64_unixlib_lifecycle_native_state *state;
    LONG flags;

    if (variant >= WOW64_UNIXLIB_LIFECYCLE_VARIANTS) return STATUS_INVALID_PARAMETER;
    state = &wow64_unixlib_lifecycle_states[variant];
    __atomic_add_fetch( &state->unbind_calls, 1, __ATOMIC_SEQ_CST );
    __atomic_store_n( &state->unbind_sequence,
                      wow64_unixlib_lifecycle_next_sequence( state ), __ATOMIC_RELEASE );
    flags = __atomic_load_n( &state->flags, __ATOMIC_ACQUIRE );
    if (flags & WOW64_UNIXLIB_LIFECYCLE_UNBIND_FAULT)
        *(volatile LONG *)(ULONG_PTR)0 = 0;
    return __atomic_load_n( &state->unbind_status, __ATOMIC_ACQUIRE );
}

static NTSTATUS wow64_unixlib_lifecycle_quiesce_0(void)
{
    return wow64_unixlib_lifecycle_quiesce_common( 0 );
}

static NTSTATUS wow64_unixlib_lifecycle_quiesce_1(void)
{
    return wow64_unixlib_lifecycle_quiesce_common( 1 );
}

static NTSTATUS wow64_unixlib_lifecycle_quiesce_2(void)
{
    return wow64_unixlib_lifecycle_quiesce_common( 2 );
}

static NTSTATUS wow64_unixlib_lifecycle_unbind_0(void)
{
    return wow64_unixlib_lifecycle_unbind_common( 0 );
}

static NTSTATUS wow64_unixlib_lifecycle_unbind_1(void)
{
    return wow64_unixlib_lifecycle_unbind_common( 1 );
}

static NTSTATUS wow64_unixlib_lifecycle_unbind_2(void)
{
    return wow64_unixlib_lifecycle_unbind_common( 2 );
}

static NTSTATUS x18_dispatch_lifecycle_configure_func( void *args )
{
    const struct wow64_unixlib_lifecycle_config *config = args;
    struct wow64_unixlib_lifecycle_native_state *state;

    if (!config || config->variant >= WOW64_UNIXLIB_LIFECYCLE_VARIANTS)
        return STATUS_INVALID_PARAMETER;
    state = &wow64_unixlib_lifecycle_states[config->variant];
    memset( state, 0, sizeof(*state) );
    state->flags = config->flags;
    state->quiesce_status = config->quiesce_status;
    state->unbind_status = config->unbind_status;
    state->reenter_status = STATUS_NOT_IMPLEMENTED;
    return STATUS_SUCCESS;
}

static NTSTATUS x18_dispatch_lifecycle_register_func( void *args )
{
    struct wow64_unixlib_lifecycle_registration *registration = args;
    unixlib_handle_t handle = 0;
    NTSTATUS status;

    if (!registration || registration->variant >= WOW64_UNIXLIB_LIFECYCLE_VARIANTS)
        return STATUS_INVALID_PARAMETER;
    status = ntdll_wow64_register_unixlib_dispatch_v2(
        &wow64_unixlib_lifecycle_sources[registration->variant],
        wow64_unixlib_lifecycle_sources[registration->variant].funcs,
        wow64_unixlib_lifecycle_quiesce_funcs[registration->variant],
        wow64_unixlib_lifecycle_unbind_funcs[registration->variant], &handle );
    registration->handle = status ? 0 : handle;
    return status;
}

static NTSTATUS x18_dispatch_lifecycle_unregister_func( void *args )
{
    const struct wow64_unixlib_lifecycle_unregistration *unregistration = args;

    if (!unregistration) return STATUS_INVALID_PARAMETER;
    return ntdll_wow64_unregister_unixlib_dispatch( unregistration->handle );
}

static NTSTATUS x18_dispatch_lifecycle_get_state_func( void *args )
{
    struct wow64_unixlib_lifecycle_state *result = args;
    struct wow64_unixlib_lifecycle_native_state *state;

    if (!result || result->variant >= WOW64_UNIXLIB_LIFECYCLE_VARIANTS)
        return STATUS_INVALID_PARAMETER;
    state = &wow64_unixlib_lifecycle_states[result->variant];
    result->quiesce_calls = __atomic_load_n( &state->quiesce_calls, __ATOMIC_ACQUIRE );
    result->unbind_calls = __atomic_load_n( &state->unbind_calls, __ATOMIC_ACQUIRE );
    result->entry_sequence = __atomic_load_n( &state->entry_sequence, __ATOMIC_ACQUIRE );
    result->quiesce_sequence = __atomic_load_n( &state->quiesce_sequence, __ATOMIC_ACQUIRE );
    result->exit_sequence = __atomic_load_n( &state->exit_sequence, __ATOMIC_ACQUIRE );
    result->unbind_sequence = __atomic_load_n( &state->unbind_sequence, __ATOMIC_ACQUIRE );
    result->reenter_status = __atomic_load_n( &state->reenter_status, __ATOMIC_ACQUIRE );
    result->reserved = 0;
    result->reserved2 = 0;
    return STATUS_SUCCESS;
}

#if defined(__APPLE__) && defined(__aarch64__) && defined(HAVE_OS_CUSTOM_X18_ABI)

static NTSTATUS x18_dispatch_inner_probe_func( void *args )
{
    struct x18_dispatch_test_state *state = args;
    BOOL custom;

    if (__builtin_available( macOS 26.4, * )) custom = os_custom_x18_abi_enabled();
    else return STATUS_NOT_SUPPORTED;

    state->system_target_custom = custom;
    state->inner_called++;
    return custom ? STATUS_INVALID_DEVICE_STATE : STATUS_SUCCESS;
}

extern NTSTATUS x18_dispatch_direct_bridge_impl( void *args );
extern NTSTATUS x18_dispatch_illegal_instruction_func( void *args );

static bool (*custom_x18_abi_enabled_func)(void);
static void (*set_custom_x18_abi_enabled_func)(bool enabled);

static BOOL __attribute__((used,noinline)) x18_dispatch_custom_x18_abi_enabled(void)
{
    return custom_x18_abi_enabled_func();
}

static void __attribute__((used,noinline)) x18_dispatch_set_custom_x18_abi_enabled( BOOL enabled )
{
    set_custom_x18_abi_enabled_func( enabled );
}

static NTSTATUS x18_dispatch_direct_bridge_func( void *args )
{
    ULONG_PTR saved_x18;
    NTSTATUS status;

    /* The availability helper is itself a Darwin call and may clobber x18 on
     * an older runtime that cannot preserve the Windows ABI. */
    __asm__ volatile( "mov %0, x18" : "=r" (saved_x18) );
    if (__builtin_available( macOS 26.4, * ))
    {
        custom_x18_abi_enabled_func = os_custom_x18_abi_enabled;
        set_custom_x18_abi_enabled_func = os_set_custom_x18_abi_enabled;
        status = x18_dispatch_direct_bridge_impl( args );
    }
    else status = STATUS_NOT_SUPPORTED;
    __asm__ volatile( "mov x18, %0" :: "r" (saved_x18) );
    return status;
}

__ASM_GLOBAL_FUNC( x18_dispatch_illegal_instruction_func,
                   "hint 34\n\t" /* bti c */
                   "udf #0\n\t"
                   "ret" )

/*
 * Enter directly from PE code while that thread owns custom x18.  The first
 * raw dispatch gives x18 an arbitrary restored-context value and faults in its
 * native target.  Returning STATUS_ILLEGAL_INSTRUCTION through the global
 * dispatcher return proves that neither entry nor fault recovery dereferences
 * that opaque x18.  The bridge then disables custom mode and enters the raw
 * dispatcher again from the free PE user stack, exactly like a Darwin callback.
 * It reestablishes custom mode and the Windows TEB before returning to PE code.
 */
__ASM_GLOBAL_FUNC( x18_dispatch_direct_bridge_impl,
                   "hint 34\n\t" /* bti c */
                   "stp x29, x30, [sp, #-0x30]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0x30\n\t")
                   __ASM_CFI(".cfi_offset 29, -0x30\n\t")
                   __ASM_CFI(".cfi_offset 30, -0x28\n\t")
                   "mov x29, sp\n\t"
                   "stp x19, x20, [sp, #0x10]\n\t"
                   __ASM_CFI(".cfi_offset 19, -0x20\n\t")
                   __ASM_CFI(".cfi_offset 20, -0x18\n\t")
                   "mov x19, x0\n\t"          /* state */

                   "bl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x48]\n\t"
                   "cbz w0, 8f\n\t"
                   "str x18, [sp, #0x20]\n\t" /* PE TEB */

                   /* A restored Windows context may contain any x18 value. */
                   "ldr x18, [x19, #0x28]\n\t"
                   "ldr x16, [x19, #0x00]\n\t"
                   "ldr x0, [x19, #0x08]\n\t"
                   "mov w1, #2\n\t"
                   "mov x2, x19\n\t"
                   "blr x16\n\t"
                   "str w0, [x19, #0x6c]\n\t"
                   "str x18, [x19, #0x30]\n\t"
                   "bl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x4c]\n\t"
                   "cbz w0, 1f\n\t"
                   "ldr x18, [sp, #0x20]\n\t"
                   "mov w0, #0\n\t"
                   "bl " __ASM_NAME("x18_dispatch_set_custom_x18_abi_enabled") "\n"

                   /* Now enter the same dispatcher with Darwin owning x18. */
                   "1:\tbl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x50]\n\t"
                   "cbnz w0, 8f\n\t"
                   "ldr x16, [x19, #0x00]\n\t"
                   "ldr x0, [x19, #0x08]\n\t"
                   "mov w1, #1\n\t"
                   "mov x2, x19\n\t"
                   "blr x16\n\t"
                   "str w0, [x19, #0x70]\n\t"
                   "str x18, [x19, #0x38]\n\t"
                   "bl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x58]\n\t"
                   "cbz w0, 2f\n\t"
                   "mov w0, #0\n\t"
                   "bl " __ASM_NAME("x18_dispatch_set_custom_x18_abi_enabled") "\n"

                   "2:\tbl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x5c]\n\t"
                   "cbnz w0, 7f\n\t"

                   /* Use ntdll's pthread-backed unix_get_current_teb entry. */
                   "ldr x16, [x19, #0x00]\n\t"
                   "ldr x0, [x19, #0x10]\n\t"
                   "mov w1, #3\n\t" /* unix_get_current_teb */
                   "add x2, x19, #0x20\n\t"
                   "blr x16\n\t"
                   "str w0, [x19, #0x74]\n\t"
                   "str x18, [x19, #0x40]\n\t"
                   "bl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x60]\n\t"

                   /* Reproduce a lost Darwin custom-x18 mode at a native PE
                    * TEB load.  The signal bridge must restore the TEB, retry
                    * the unchanged instruction, and resume custom mode. */
                   "ldr x16, [x19, #0x80]\n\t"
                   "cbz x16, 7f\n\t"
                   "mov w0, #0\n\t"
                   "bl " __ASM_NAME("x18_dispatch_set_custom_x18_abi_enabled") "\n\t"
                   "mov x18, xzr\n\t"
                   "ldr x16, [x19, #0x80]\n\t"
                   "blr x16\n\t"
                   "str x0, [x19, #0x88]\n\t"
                   "mov w0, #1\n\t"
                   "str w0, [x19, #0x94]\n\t"
                   "bl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x90]\n\t"
                   "b 7f\n\t"

                   /* The bridge contract requires a PE/custom-mode caller. */
                   "8:\tmovz w0, #0x0184\n\t" /* STATUS_INVALID_DEVICE_STATE */
                   "movk w0, #0xc000, lsl #16\n\t"
                   "str w0, [x19, #0x6c]\n\t"
                   "str w0, [x19, #0x70]\n\t"
                   "str w0, [x19, #0x74]\n\t"

                   /* Always return to the PE caller with a valid custom x18. */
                   "7:\tbl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "cbnz w0, 6f\n\t"
                   "mov w0, #1\n\t"
                   "bl " __ASM_NAME("x18_dispatch_set_custom_x18_abi_enabled") "\n"
                   "6:\tbl " __ASM_NAME("x18_dispatch_custom_x18_abi_enabled") "\n\t"
                   "str w0, [x19, #0x64]\n\t"
                   "ldr w1, [x19, #0x48]\n\t"
                   "cbz w1, 5f\n\t"
                   "ldr x18, [sp, #0x20]\n\t"
                   "b 3f\n"
                   "5:\tldr x18, [x19, #0x18]\n"

                   "3:\tldr w0, [x19, #0x6c]\n\t"
                   "movz w1, #0x001d\n\t" /* STATUS_ILLEGAL_INSTRUCTION */
                   "movk w1, #0xc000, lsl #16\n\t"
                   "cmp w0, w1\n\t"
                   "b.ne 4f\n\t"
                   "ldr w0, [x19, #0x70]\n\t"
                   "cbnz w0, 4f\n\t"
                   "ldr w0, [x19, #0x74]\n"
                   "4:\tldp x19, x20, [sp, #0x10]\n\t"
                   __ASM_CFI(".cfi_restore 19\n\t")
                   __ASM_CFI(".cfi_restore 20\n\t")
                   "ldp x29, x30, [sp], #0x30\n\t"
                   __ASM_CFI(".cfi_restore 29\n\t")
                   __ASM_CFI(".cfi_restore 30\n\t")
                   __ASM_CFI(".cfi_def_cfa_offset 0\n\t")
                   "ret" )

#else

static NTSTATUS x18_dispatch_inner_probe_func( void *args )
{
    (void)args;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS x18_dispatch_direct_bridge_func( void *args )
{
    (void)args;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS x18_dispatch_illegal_instruction_func( void *args )
{
    (void)args;
    return STATUS_NOT_SUPPORTED;
}

#endif

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    x18_dispatch_direct_bridge_func,
    x18_dispatch_inner_probe_func,
    x18_dispatch_illegal_instruction_func,
    x18_dispatch_get_zero_count_func,
    x18_dispatch_lifecycle_configure_func,
    x18_dispatch_lifecycle_register_func,
    x18_dispatch_lifecycle_unregister_func,
    x18_dispatch_lifecycle_get_state_func,
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_funcs) == x18_dispatch_test_unix_func_count );
C_ASSERT( unix_get_current_teb == 3 );

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_unixlib_lifecycle_zero_args_func,
    wow64_unixlib_lifecycle_context_func,
    wow64_unixlib_lifecycle_checked_fault_func,
    wow64_unixlib_lifecycle_block_func,
    wow64_unixlib_lifecycle_self_unload_func,
    wow64_unixlib_lifecycle_self_unregister_func,
    wow64_unixlib_lifecycle_self_unregister_fault_func,
    x18_dispatch_illegal_instruction_func,
};

static const struct wine_unixlib_dispatch_entry_v2 wow64_unixlib_lifecycle_metadata[] =
{
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_unixlib_context_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_unixlib_checked_fault_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_unixlib_block_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_unixlib_self_unload_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_unixlib_self_unregister_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wow64_unixlib_self_unregister_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_wow64_funcs) ==
          wow64_unixlib_lifecycle_test_func_count );
WINE_UNIXLIB_DISPATCH_SOURCE_V2( __wine_unix_call_wow64_funcs,
                                 wow64_unixlib_lifecycle_metadata );

#define WOW64_UNIXLIB_LIFECYCLE_FUNCS_INIT \
    { wow64_unixlib_lifecycle_zero_args_func, \
      wow64_unixlib_lifecycle_context_func, \
      wow64_unixlib_lifecycle_checked_fault_func, \
      wow64_unixlib_lifecycle_block_func, \
      wow64_unixlib_lifecycle_self_unload_func, \
      wow64_unixlib_lifecycle_self_unregister_func, \
      wow64_unixlib_lifecycle_self_unregister_fault_func, \
      x18_dispatch_illegal_instruction_func }

static const unixlib_entry_t wow64_unixlib_lifecycle_variant_funcs
    [WOW64_UNIXLIB_LIFECYCLE_VARIANTS][wow64_unixlib_lifecycle_test_func_count] =
{
    WOW64_UNIXLIB_LIFECYCLE_FUNCS_INIT,
    WOW64_UNIXLIB_LIFECYCLE_FUNCS_INIT,
    WOW64_UNIXLIB_LIFECYCLE_FUNCS_INIT,
};

#define WOW64_UNIXLIB_LIFECYCLE_SOURCE_INIT(table) \
    { WINE_UNIXLIB_DISPATCH_SOURCE_V2_VERSION, \
      sizeof(struct wine_unixlib_dispatch_source_v2), \
      ARRAY_SIZE(table), \
      sizeof(struct wine_unixlib_dispatch_entry_v2), \
      table, wow64_unixlib_lifecycle_metadata, 0, 0 }

static const struct wine_unixlib_dispatch_source_v2
    wow64_unixlib_lifecycle_sources[WOW64_UNIXLIB_LIFECYCLE_VARIANTS] =
{
    WOW64_UNIXLIB_LIFECYCLE_SOURCE_INIT(wow64_unixlib_lifecycle_variant_funcs[0]),
    WOW64_UNIXLIB_LIFECYCLE_SOURCE_INIT(wow64_unixlib_lifecycle_variant_funcs[1]),
    WOW64_UNIXLIB_LIFECYCLE_SOURCE_INIT(wow64_unixlib_lifecycle_variant_funcs[2]),
};
