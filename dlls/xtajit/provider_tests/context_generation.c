/*
 * xtajit i386 context-install writeback tests
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

#include <stdint.h>
#include <stdio.h>

#include "../context_generation.h"
#include "../process_lifecycle.h"

static unsigned int failures;

#define check(condition, ...) \
    do { if (!(condition)) { fprintf( stderr, "not ok: " __VA_ARGS__ ); ++failures; } } while (0)

static uint32_t finish_call( const struct xtajit_context_generation *generation,
                             uint64_t snapshot, int reset_state,
                             uint32_t installed_eax, uint32_t status )
{
    if (!xtajit_context_requires_reload( generation, snapshot, reset_state ))
        installed_eax = status;
    return installed_eax;
}

static void test_rejected_unix_call_return(void)
{
    const uint32_t stack[XTAJIT_I386_UNIX_CALL_FRAME_DWORDS] =
    {
        0x12345678, 0, 0, 0xabcdef01, 0x10203040,
    };
    struct xtajit_i386_unix_call_completion completion;

    /* A handle rejected by the high-shadow pre-dispatch gate cannot install a
     * replacement context: it must complete the already-read five-word
     * stdcall frame as the native dispatcher would and remain non-fatal. */
    completion = xtajit_i386_complete_rejected_unix_call( 0x1000, stack[0],
                                                           0xc000000d );
    check( sizeof(stack) == 20, "Unix call stdcall frame has size %zu\n", sizeof(stack) );
    check( completion.eip == stack[0], "rejected Unix call did not return to caller\n" );
    check( completion.esp == 0x1014,
           "rejected Unix call did not consume its stdcall frame\n" );
    check( completion.eax == 0xc000000d,
           "rejected Unix call returned %#x instead of STATUS_INVALID_PARAMETER\n",
           completion.eax );
    completion = xtajit_i386_complete_rejected_unix_call( UINT32_MAX - 20,
                                                           stack[0], 0xc000000d );
    check( completion.esp == UINT32_MAX,
           "boundary Unix call frame completion wrapped the stack pointer\n" );
}

static void test_returning_callbacks(void)
{
    struct xtajit_context_generation generation = {0};
    const uint32_t syscall_number = 0x136b;
    const uint32_t status = 0;
    uint64_t snapshot = xtajit_context_generation_snapshot( &generation );

    /* Wow64KiUserCallbackDispatcher installs the callback context, runs nested
     * simulation, restores the original context, then restores the saved CPU
     * flags.  Both successful SetContext calls advance the generation. */
    xtajit_context_generation_advance( &generation );
    xtajit_context_generation_advance( &generation );
    check( finish_call( &generation, snapshot, 0, syscall_number, status ) == status,
           "returning callback discarded syscall status\n" );

    snapshot = xtajit_context_generation_snapshot( &generation );
    xtajit_context_generation_advance( &generation );
    xtajit_context_generation_advance( &generation );
    xtajit_context_generation_advance( &generation );
    xtajit_context_generation_advance( &generation );
    check( finish_call( &generation, snapshot, 0, syscall_number, status ) == status,
           "nested returning callbacks discarded syscall status\n" );
}

static void test_nonreturning_context(void)
{
    struct xtajit_context_generation generation = {0};
    const uint32_t installed_eax = 0x12345678;
    const uint32_t status = 0xc0000001;
    uint64_t snapshot = xtajit_context_generation_snapshot( &generation );

    /* wow64_NtContinueEx installs a context and leaves RESET_STATE set. */
    xtajit_context_generation_advance( &generation );
    check( finish_call( &generation, snapshot, 1, installed_eax, status ) == installed_eax,
           "non-returning context install was overwritten\n" );

    /* RESET_STATE by itself is not proof that this call installed a context. */
    snapshot = xtajit_context_generation_snapshot( &generation );
    check( finish_call( &generation, snapshot, 1, installed_eax, status ) == status,
           "stale reset state discarded call status\n" );
}

static void test_generation_wrap(void)
{
    struct xtajit_context_generation generation = { UINT64_MAX };
    uint64_t snapshot = xtajit_context_generation_snapshot( &generation );

    xtajit_context_generation_advance( &generation );
    check( generation.value == 1, "generation wrap did not reserve zero\n" );
    check( xtajit_context_requires_reload( &generation, snapshot, 1 ),
           "wrapped non-returning context install was not detected\n" );
    check( !xtajit_context_requires_reload( &generation, snapshot, 0 ),
           "wrapped returning context install was treated as non-returning\n" );
}

static void test_process_init_failure_state(void)
{
    const uint64_t observer = 0x0000000000000001ull;
    const uint64_t logical_fault = 0x0000000000000002ull;
    const uint64_t required = observer | logical_fault;

    check( xtajit_process_capabilities_satisfied( required, required ),
           "required provider capabilities were not accepted\n" );
    check( !xtajit_process_capabilities_satisfied( required, observer ),
           "missing logical-write-fault capability was accepted\n" );
    check( !xtajit_process_capabilities_satisfied( required, logical_fault ),
           "missing observer capability was accepted\n" );
    check( !xtajit_process_init_failure_must_poison( 0 ),
           "pre-initialization failure requested rollback poisoning\n" );
    check( xtajit_process_init_failure_must_poison( 1 ),
           "post-initialization capability failure skipped poisoning\n" );
}

int main(void)
{
    test_rejected_unix_call_return();
    test_returning_callbacks();
    test_nonreturning_context();
    test_generation_wrap();
    test_process_init_failure_state();

    if (failures)
    {
        fprintf( stderr, "%u xtajit context-generation test failure(s)\n", failures );
        return 1;
    }
    puts( "xtajit context-generation tests passed" );
    return 0;
}
