/*
 * Darwin ARM64 system-x18 dispatcher test interface
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NTDLL_TESTS_X18_DISPATCH_TEST_H
#define __NTDLL_TESTS_X18_DISPATCH_TEST_H

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#undef WIN32_NO_STATUS

enum x18_dispatch_test_unix_func
{
    x18_dispatch_direct_bridge,
    x18_dispatch_inner_probe,
    x18_dispatch_illegal_instruction,
    x18_dispatch_get_zero_count,
    x18_dispatch_lifecycle_configure,
    x18_dispatch_lifecycle_register,
    x18_dispatch_lifecycle_unregister,
    x18_dispatch_lifecycle_get_state,
    x18_dispatch_test_unix_func_count,
};

enum wow64_unixlib_lifecycle_test_func
{
    wow64_unixlib_lifecycle_zero_args,
    wow64_unixlib_lifecycle_context,
    wow64_unixlib_lifecycle_checked_fault,
    wow64_unixlib_lifecycle_block,
    wow64_unixlib_lifecycle_self_unload,
    wow64_unixlib_lifecycle_self_unregister,
    wow64_unixlib_lifecycle_self_unregister_fault,
    wow64_unixlib_lifecycle_illegal_instruction,
    wow64_unixlib_lifecycle_test_func_count,
};

struct wow64_unixlib_context_params
{
    UINT32 result;
    UINT32 value;
};

struct wow64_unixlib_zero_count_result
{
    LONG count;
};

struct wow64_unixlib_context_result
{
    UINT32 guest_args;
    UINT32 args_size;
    UINT32 flags;
    UINT32 value;
};

struct wow64_unixlib_block_params
{
    UINT64 entered_event;
    UINT64 release_event;
};

struct wow64_unixlib_checked_fault_params
{
    UINT32 noaccess;
    UINT32 result;
};

struct wow64_unixlib_checked_fault_result
{
    UINT32 guest_args_before;
    UINT32 guest_args_after;
    NTSTATUS fault_status;
    UINT32 context_preserved;
};

struct wow64_unixlib_block_state
{
    LONG entered;
    LONG release;
    LONG exited;
    LONG reserved;
};

struct wow64_unixlib_self_unload_params
{
    UINT64 module;
};

struct wow64_unixlib_self_unregister_params
{
    UINT64 handle;
};

#define WOW64_UNIXLIB_LIFECYCLE_QUIESCE_FAULT    0x00000001u
#define WOW64_UNIXLIB_LIFECYCLE_UNBIND_FAULT     0x00000002u
#define WOW64_UNIXLIB_LIFECYCLE_QUIESCE_REENTER  0x00000004u

struct wow64_unixlib_lifecycle_config
{
    UINT32 variant;
    UINT32 flags;
    NTSTATUS quiesce_status;
    NTSTATUS unbind_status;
};

struct wow64_unixlib_lifecycle_registration
{
    UINT32 variant;
    UINT32 reserved;
    UINT64 handle;
};

struct wow64_unixlib_lifecycle_unregistration
{
    UINT64 handle;
};

struct wow64_unixlib_lifecycle_state
{
    UINT32 variant;
    UINT32 reserved;
    LONG quiesce_calls;
    LONG unbind_calls;
    LONG entry_sequence;
    LONG quiesce_sequence;
    LONG exit_sequence;
    LONG unbind_sequence;
    NTSTATUS reenter_status;
    UINT32 reserved2;
};

C_ASSERT( sizeof(struct wow64_unixlib_context_params) == 8 );
C_ASSERT( sizeof(struct wow64_unixlib_zero_count_result) == 4 );
C_ASSERT( sizeof(struct wow64_unixlib_context_result) == 16 );
C_ASSERT( sizeof(struct wow64_unixlib_checked_fault_params) == 8 );
C_ASSERT( sizeof(struct wow64_unixlib_checked_fault_result) == 16 );
C_ASSERT( sizeof(struct wow64_unixlib_block_params) == 16 );
C_ASSERT( sizeof(struct wow64_unixlib_block_state) == 16 );
C_ASSERT( sizeof(struct wow64_unixlib_self_unload_params) == 8 );
C_ASSERT( sizeof(struct wow64_unixlib_self_unregister_params) == 8 );
C_ASSERT( sizeof(struct wow64_unixlib_lifecycle_config) == 16 );
C_ASSERT( sizeof(struct wow64_unixlib_lifecycle_registration) == 16 );
C_ASSERT( sizeof(struct wow64_unixlib_lifecycle_unregistration) == 8 );
C_ASSERT( sizeof(struct wow64_unixlib_lifecycle_state) == 40 );

/* Keep this pointer-free so the PE test and native helper share one layout. */
struct x18_dispatch_test_state
{
    UINT64 dispatcher;             /* 00: raw __wine_unix_call_dispatcher */
    UINT64 test_handle;            /* 08: this helper's native function table */
    UINT64 ntdll_handle;           /* 10: ntdll's native function table */
    UINT64 expected_teb;           /* 18 */
    UINT64 observed_teb;           /* 20: struct current_teb_params-compatible */
    UINT64 opaque_x18;             /* 28: arbitrary restored CONTEXT_ARM64_X18 */
    UINT64 observed_opaque_x18;    /* 30 */
    UINT64 observed_system_x18;    /* 38 */
    UINT64 observed_teb_x18;       /* 40 */
    UINT32 entry_custom;           /* 48 */
    UINT32 after_opaque_custom;    /* 4c */
    UINT32 before_system_custom;   /* 50 */
    UINT32 system_target_custom;   /* 54 */
    UINT32 after_system_custom;    /* 58 */
    UINT32 before_teb_custom;      /* 5c */
    UINT32 after_teb_custom;       /* 60 */
    UINT32 final_custom;           /* 64 */
    UINT32 inner_called;           /* 68 */
    NTSTATUS opaque_status;        /* 6c */
    NTSTATUS system_status;        /* 70 */
    NTSTATUS teb_status;           /* 74 */
    UINT32 reserved[2];            /* 78 */
};

C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, dispatcher) == 0x00 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, test_handle) == 0x08 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, ntdll_handle) == 0x10 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, expected_teb) == 0x18 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, observed_teb) == 0x20 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, opaque_x18) == 0x28 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, observed_opaque_x18) == 0x30 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, observed_system_x18) == 0x38 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, observed_teb_x18) == 0x40 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, entry_custom) == 0x48 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, after_opaque_custom) == 0x4c );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, before_system_custom) == 0x50 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, system_target_custom) == 0x54 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, after_system_custom) == 0x58 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, before_teb_custom) == 0x5c );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, after_teb_custom) == 0x60 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, final_custom) == 0x64 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, inner_called) == 0x68 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, opaque_status) == 0x6c );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, system_status) == 0x70 );
C_ASSERT( FIELD_OFFSET(struct x18_dispatch_test_state, teb_status) == 0x74 );
C_ASSERT( sizeof(struct x18_dispatch_test_state) == 0x80 );

#endif /* __NTDLL_TESTS_X18_DISPATCH_TEST_H */
