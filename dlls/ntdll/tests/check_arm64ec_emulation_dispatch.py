#!/usr/bin/env python3
"""Verify provenance and async-safe lifetime of ARM64EC guest-return routing."""

from pathlib import Path
import sys


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function {signature}")
    start = source.find("{", start + len(signature))
    if start < 0:
        raise AssertionError(f"missing body for {signature}")
    depth = 0
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if not depth:
                return source[start : offset + 1]
    raise AssertionError(f"unterminated function {signature}")


def source_section(source: str, start_token: str, end_token: str) -> str:
    start = source.find(start_token)
    if start < 0:
        raise AssertionError(f"missing section start {start_token}")
    end = source.find(end_token, start + len(start_token))
    if end < 0:
        raise AssertionError(f"missing section end {end_token}")
    return source[start:end]


def require_order(source: str, tokens: tuple[str, ...], description: str) -> None:
    position = -1
    for token in tokens:
        next_position = source.find(token, position + 1)
        if next_position < 0:
            raise AssertionError(f"{description} lost required token: {token}")
        position = next_position


def verify(
    signal_source: str,
    pe_source: str,
    unwind_source: str,
    provider_source: str,
) -> None:
    include = '#include "arm64ec_emulation_dispatch.h"'
    if signal_source.count(include) != 1:
        raise AssertionError("signal source must include the ownership policy exactly once")
    if signal_source.count("WINE_DECLARE_DEBUG_CHANNEL(arm64ec_susp)") != 1:
        raise AssertionError("signal source must declare the low-volume suspend route channel")

    helper = function_body(
        signal_source, "static BOOL arm64ec_signal_return_requires_emulation_dispatch"
    )
    for token in (
        "restore_flags = frame->restore_flags",
        "arm64ec_consume_emulation_dispatch_request(",
        "&frame->restore_flags, RESTORE_FLAGS_EMULATION",
        "target_is_ec_code = is_ec_code( frame->pc )",
        "arm64ec_emulation_dispatch_pending(",
        "arm64ec, emulation_requested, target_is_ec_code",
        "TRACE_(arm64ec_return)",
        "frame->x[16]",
        "frame->x[17]",
        "restore_flags, frame->restore_flags",
        "arm64ec, emulation_requested",
        "InSimulation",
        "InSyscallCallback",
    ):
        if token not in helper:
            raise AssertionError(f"ownership helper lost required token: {token}")

    call = "arm64ec_signal_return_requires_emulation_dispatch"
    usr2 = function_body(signal_source, "static void usr2_handler")
    if usr2.count(call) != 1:
        raise AssertionError("SIGUSR2 must use the ownership helper exactly once")
    require_order(
        usr2,
        (
            call,
            "PC_sig(sigcontext) = (ULONG_PTR)pKiUserEmulationDispatcher",
            "PC_sig(sigcontext) = frame->pc",
        ),
        "SIGUSR2 route",
    )

    set_context = function_body(signal_source, "NTSTATUS WINAPI NtSetContextThread")
    require_order(
        set_context,
        (
            "guest_return_requested = flags & CONTEXT_ARM64_RET_TO_GUEST",
            "simulation_active = cpu &&",
            "flags &= ~CONTEXT_ARM64_RET_TO_GUEST",
            "arm64ec_emulation_dispatch_required(",
            "TRUE, guest_return_requested, simulation_active",
            "is_ec_code( frame->pc )",
            "flags |= RESTORE_FLAGS_EMULATION",
            "frame->restore_flags &= ~RESTORE_FLAGS_EMULATION",
        ),
        "NtSetContextThread guest-return provenance",
    )
    if "if (!is_ec_code( frame->pc ))" in set_context:
        raise AssertionError("raw non-EC classification bypasses guest-return provenance")

    set_full_context = function_body(
        signal_source, "NTSTATUS signal_set_full_context"
    )
    if set_full_context.count("arm64ec_suspend_handoff_ready(") != 2:
        raise AssertionError("suspend-return handoff must be checked before and after signal blocking")
    if set_full_context.count("guest_return_requested );") != 2:
        raise AssertionError("both suspend-return handoff checks must require guest provenance")
    require_order(
        set_full_context,
        (
            "guest_return_requested =",
            "!!(context->ContextFlags & CONTEXT_ARM64_RET_TO_GUEST)",
            "handoff_ready = cpu_area && cpu_area->SuspendDoorbell &&",
            "arm64ec_suspend_handoff_ready(",
            "if (handoff_ready)",
            "pthread_sigmask( SIG_BLOCK, &server_block_set, &old_set )",
            "handoff_ready = cpu_area->SuspendDoorbell &&",
            "arm64ec_suspend_handoff_ready(",
            "if (handoff_ready)",
            "simulation_active =",
            "doorbell_value =",
            "if (simulation_active) cpu_area->InSimulation = 0",
            "*cpu_area->SuspendDoorbell = 0",
            "arm64_data->suspend_pending = FALSE",
            "wait_suspend( context )",
            "status = NtSetContextThread( GetCurrentThread(), context )",
            "if (simulation_active) cpu_area->InSimulation = 1",
            "TRACE_(arm64ec_susp)",
            '"suspend return handoff pc %p sp %p simulation %u->%u->%u "',
            "pthread_sigmask( SIG_SETMASK, &old_set, NULL )",
        ),
        "signal-blocked suspend-return ownership handoff",
    )
    if set_full_context.count("cpu_area->InSimulation = 0") != 1:
        raise AssertionError("suspend-return handoff must publish one quiescent state")
    if set_full_context.count("cpu_area->InSimulation = 1") != 1:
        raise AssertionError("suspend-return handoff must reclaim simulation exactly once")

    dispatcher = source_section(
        signal_source,
        "__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher,",
        "__ASM_GLOBAL_FUNC( __wine_unix_call_dispatcher,",
    )
    require_order(
        dispatcher,
        (
            '"ldr x10, [x18, #0x378]\\n\\t"',
            '"stp x18, x19, [x10, #0x90]\\n\\t"',
        ),
        "regular syscall frame capture",
    )
    if '"str wzr, [x10, #0x10c]\\n\\t"' in dispatcher:
        raise AssertionError("regular syscall entry erases asynchronously published restore flags")

    usr1 = function_body(signal_source, "static void usr1_handler")
    require_order(
        usr1,
        (
            "save_context( &context, sigcontext )",
            "wait_suspend( &context )",
            "This ucontext was captured directly from live AArch64 execution",
            "TRACE_(arm64ec_susp)",
            '"sigusr1 native route pc %p sp %p simulation %u callback %u "',
            '"doorbell %p value %#x pending %u\\n"',
            "restore_context( &context, sigcontext )",
        ),
        "SIGUSR1 native live-context route",
    )
    for token in (
        "chpe->InSimulation",
        "chpe->InSyscallCallback",
        "chpe->SuspendDoorbell",
        "arm64_thread_data( data )->suspend_pending",
    ):
        if token not in usr1:
            raise AssertionError(f"SIGUSR1 route diagnostics lost field: {token}")
    for forbidden in (
        "pKiUserEmulationDispatcher",
        "is_ec_code( context.Pc )",
        "chpe->InSimulation = 1",
    ):
        if forbidden in usr1:
            raise AssertionError(
                f"SIGUSR1 live native context regained raw guest routing: {forbidden}"
            )

    ownership_header = Path(__file__).resolve().parents[1] / "unix" / "arm64ec_emulation_dispatch.h"
    ownership_source = ownership_header.read_text(encoding="utf-8")
    consume = function_body(
        ownership_source,
        "static inline bool arm64ec_consume_emulation_dispatch_request",
    )
    require_order(
        consume,
        (
            "flags = *restore_flags",
            "*restore_flags = flags & ~request_flag",
            "return !!(flags & request_flag)",
        ),
        "one-shot emulation-dispatch request",
    )
    suspend_policy = function_body(
        ownership_source, "static inline bool arm64ec_suspend_handoff_ready"
    )
    require_order(
        suspend_policy,
        (
            "if (!suspend_pending || syscall_callback_active) return false",
            "if (!simulation_active) return true",
            "return arm64ec && guest_return_requested",
        ),
        "suspend-return ownership policy",
    )

    conversion = function_body(
        unwind_source, "static inline void context_x64_to_arm_guest_return"
    )
    require_order(
        conversion,
        (
            "context_x64_to_arm( arm_ctx, ec_ctx )",
            "arm_ctx->ContextFlags |= CONTEXT_ARM64_RET_TO_GUEST",
        ),
        "ARM64EC guest-return conversion",
    )

    wrappers = (
        "NTSTATUS SYSCALL_API NtContinue( CONTEXT *context",
        "NTSTATUS SYSCALL_API NtContinueEx( CONTEXT *context",
        "NTSTATUS SYSCALL_API NtRaiseException( EXCEPTION_RECORD *rec",
        "NTSTATUS SYSCALL_API NtSetContextThread( HANDLE handle",
    )
    for signature in wrappers:
        body = function_body(pe_source, signature)
        if body.count("context_x64_to_arm_guest_return(") != 1:
            raise AssertionError(f"guest-return syscall lost provenance: {signature}")
        if "context_x64_to_arm( " in body:
            raise AssertionError(f"guest-return syscall uses unmarked conversion: {signature}")

    continue_suspend = function_body(
        provider_source,
        "static DECLSPEC_NORETURN void __attribute__((used, noinline)) "
        "continue_suspended_context",
    )
    require_order(
        continue_suspend,
        (
            "context->AMD64_Context.ContextFlags |= CONTEXT_AMD64_FULL",
            "status = NtContinue( &context->AMD64_Context, FALSE )",
            "abort_simulation(",
        ),
        "provider suspend continuation",
    )
    if "InSimulation" in continue_suspend:
        raise AssertionError("provider relinquishes simulation before ntdll blocks signals")

    run_simulation = function_body(
        provider_source,
        "static DECLSPEC_NORETURN void run_x64_simulation",
    )
    if run_simulation.count("continue_suspended_context( state, ec_context,") != 3:
        raise AssertionError("all three provider suspend exits must use the ownership handoff")
    provider_stop = source_section(
        run_simulation,
        "if (!status && params.stop_reason == XTAJIT64_STOP_SUSPEND)",
        "if (status != STATUS_RETRY",
    )
    if "InSimulation = 0" in provider_stop:
        raise AssertionError("provider stop clears simulation before suspend-return handoff")
    mapping_reconcile = source_section(
        run_simulation,
        "/* begin_simulation has captured the unchanged faulting context",
        "mapping_reconciled = TRUE;",
    )
    if "InSimulation = 0" in mapping_reconcile:
        raise AssertionError("mapping reconciliation relinquishes provider stack ownership")
    if run_simulation.count(
        "xtajit64_restore_native( state, ec_context, cpu, params.unicorn_error )"
    ) != 2:
        raise AssertionError("both native returns must use the assembly ownership handoff")
    if "cpu->InSimulation = 0" in run_simulation:
        raise AssertionError("C code relinquishes simulation before the native stack switch")

    begin_wrapper = function_body(
        provider_source,
        "void WINAPI __attribute__((naked)) BeginSimulation",
    )
    if begin_wrapper.count('"stlrb w15, [x9]\\n\\t"') != 1:
        raise AssertionError("BeginSimulation must acquire simulation exactly once")
    require_order(
        begin_wrapper,
        (
            '"str x18, [x0, #0x870]\\n\\t"',
            '"ldr x9, [x18, #0x1788]\\n\\t"',
            '"mov w15, #1\\n\\t"',
            '"stlrb w15, [x9]\\n\\t"',
            '"mov sp, x16\\n\\t"',
            '"b \\"#begin_simulation_on_control_stack\\"\\n\\t"',
        ),
        "BeginSimulation control-stack ownership acquisition",
    )
    begin_control = function_body(
        provider_source,
        "static DECLSPEC_NORETURN void __attribute__((used)) "
        "begin_simulation_on_control_stack",
    )
    require_order(
        begin_control,
        (
            "flight_validate_recorder_layout( state )",
            "flight_start_transition( state )",
            "require_control_stack_simulation_ownership(",
            '"BeginSimulation entered its control stack without simulation ownership"',
            "discard_unwound_transition_frames( state,",
        ),
        "BeginSimulation control-stack ownership watchdog",
    )
    ownership_watchdog = function_body(
        provider_source,
        "static void require_control_stack_simulation_ownership",
    )
    require_order(
        ownership_watchdog,
        (
            "*(const volatile BOOLEAN *)&cpu->InSimulation",
            "XTAJIT64_FLIGHT_EVENT_WATCHDOG_VIOLATION",
            "XTAJIT64_FLIGHT_REASON_SIMULATION_OWNERSHIP",
            "flight_dump_if_frozen( state, boundary )",
            "abort_transition( state, STATUS_INVALID_DEVICE_STATE, boundary )",
        ),
        "shared control-stack ownership watchdog",
    )
    native_capture = function_body(
        provider_source,
        "static void __attribute__((used, naked)) xtajit64_capture_native",
    )
    if native_capture.count('"stlrb w10, [x17]\\n\\t"') != 1:
        raise AssertionError("native capture must acquire simulation exactly once")
    require_order(
        native_capture,
        (
            '"ldr x9, [x17, #0x18]\\n\\t"',
            '"cbz x9, 1f\\n\\t"',
            '"mov w10, #1\\n\\t"',
            '"stlrb w10, [x17]\\n\\t"',
            '"mov x17, x9\\n\\t"',
            '"mov sp, x17\\n\\t"',
            '"b \\"#xtajit64_transition_from_native\\"\\n\\t"',
        ),
        "native-capture control-stack ownership acquisition",
    )
    native_transition = function_body(
        provider_source,
        "static void __attribute__((used, noreturn)) xtajit64_transition_from_native",
    )
    require_order(
        native_transition,
        (
            "flight_start_transition( state )",
            "require_control_stack_simulation_ownership(",
            '"native capture entered its control stack without simulation ownership"',
            "capture_fp_state( &ec_context->AMD64_Context )",
        ),
        "native-capture control-stack ownership watchdog",
    )
    continuation_exit = source_section(
        run_simulation, "if (frame)", "if (continuation_target_seen)"
    )
    require_order(
        continuation_exit,
        (
            "if (suspend_doorbell_is_set( cpu ))",
            "continue_suspended_context( state, ec_context,",
            "xtajit64_restore_native( state, ec_context, cpu, params.unicorn_error )",
        ),
        "EC continuation suspend ownership",
    )
    entry_start = run_simulation.find("if ((status = resolve_ec_entry_thunk(")
    if entry_start < 0:
        raise AssertionError("missing EC entry suspend-return section")
    entry_exit = run_simulation[entry_start:]
    require_order(
        entry_exit,
        (
            "if (suspend_doorbell_is_set( cpu ))",
            "continue_suspended_context( state, ec_context,",
            "xtajit64_restore_native( state, ec_context, cpu, params.unicorn_error )",
        ),
        "EC entry suspend ownership",
    )

    for token in (
        "offsetof(CHPE_V2_CPU_AREA_INFO, InSimulation) == 0x00",
        "offsetof(CHPE_V2_CPU_AREA_INFO, SuspendDoorbell) == 0x20",
        "offsetof(struct xtajit64_thread_state, control_stack_top) == 0x10",
        "XTAJIT64_STOP_SUSPEND == 8",
    ):
        if token not in provider_source:
            raise AssertionError(f"native handoff lost ABI assertion: {token}")
    restore_native = function_body(
        provider_source,
        "static DECLSPEC_NORETURN void __attribute__((naked)) xtajit64_restore_native",
    )
    if restore_native.count('"strb wzr, [x2]\\n\\t"') != 1:
        raise AssertionError("native handoff must relinquish simulation exactly once")
    require_order(
        restore_native,
        (
            '"mov x16, x1\\n\\t"',
            '"ldr x15, [x16, #0x98]\\n\\t"',
            '"mov sp, x15\\n\\t"',
            '"strb wzr, [x2]\\n\\t"',
            '"dmb ish\\n\\t"',
            '"ldr x15, [x2, #0x20]\\n\\t"',
            '"ldr w15, [x15]\\n\\t"',
            '"cbnz w15, 1f\\n\\t"',
            '"br x17\\n\\t"',
            '"1:\\n\\t"',
            '"strb w15, [x2]\\n\\t"',
            '"ldr x15, [x0, #0x10]\\n\\t"',
            '"mov sp, x15\\n\\t"',
            '"mov x1, x16\\n\\t"',
            '"mov w2, #8\\n\\t"',
            '"b \\\"#continue_suspended_context\\\"\\n\\t"',
        ),
        "native-stack simulation ownership handoff",
    )


def expect_rejected(
    signal_source: str,
    pe_source: str,
    unwind_source: str,
    provider_source: str,
    description: str,
) -> None:
    try:
        verify(signal_source, pe_source, unwind_source, provider_source)
    except AssertionError:
        return
    raise AssertionError(f"source contract accepted mutation: {description}")


def main() -> int:
    if len(sys.argv) != 5:
        print(
            f"usage: {sys.argv[0]} SIGNAL_ARM64_C SIGNAL_ARM64EC_C UNWIND_H XTAJIT64_CPU_C",
            file=sys.stderr,
        )
        return 2
    signal_source = Path(sys.argv[1]).read_text(encoding="utf-8")
    pe_source = Path(sys.argv[2]).read_text(encoding="utf-8")
    unwind_source = Path(sys.argv[3]).read_text(encoding="utf-8")
    provider_source = Path(sys.argv[4]).read_text(encoding="utf-8")
    verify(signal_source, pe_source, unwind_source, provider_source)

    mutated = signal_source.replace(
        "arm64ec_signal_return_requires_emulation_dispatch( data, frame )",
        "is_arm64ec() && !is_ec_code( frame->pc )",
        1,
    )
    if mutated == signal_source:
        raise AssertionError("could not construct raw non-EC route mutation")
    expect_rejected(mutated, pe_source, unwind_source, provider_source,
                    "raw non-EC SIGUSR2 route")

    load = '"ldr x10, [x18, #0x378]\\n\\t" /* thread_data->syscall_frame */\n'
    clear = '                   "str wzr, [x10, #0x10c]\\n\\t"\n'
    mutated = signal_source.replace(load, load + clear, 1)
    if mutated == signal_source:
        raise AssertionError("could not construct async restore-flags erasure mutation")
    expect_rejected(mutated, pe_source, unwind_source, provider_source,
                    "async restore-flags erasure")

    mutated = signal_source.replace(
        '"doorbell %p value %#x pending %u\\n"',
        '"doorbell %p pending %u\\n"',
        1,
    )
    if mutated == signal_source:
        raise AssertionError("could not construct incomplete SIGUSR1 route mutation")
    expect_rejected(mutated, pe_source, unwind_source, provider_source,
                    "incomplete SIGUSR1 route evidence")

    native_restore = (
        "            arm64_thread_data( data )->suspend_pending );\n"
        "        restore_context( &context, sigcontext );"
    )
    raw_usr1_route = (
        "            arm64_thread_data( data )->suspend_pending );\n"
        "        if (is_arm64ec() && !is_ec_code( context.Pc ))\n"
        "            context.Pc = (ULONG_PTR)pKiUserEmulationDispatcher;\n"
        "        restore_context( &context, sigcontext );"
    )
    mutated = signal_source.replace(native_restore, raw_usr1_route, 1)
    if mutated == signal_source:
        raise AssertionError("could not construct raw SIGUSR1 guest-route mutation")
    expect_rejected(mutated, pe_source, unwind_source, provider_source,
                    "raw SIGUSR1 live-context guest route")

    mutated = pe_source.replace(
        "context_x64_to_arm_guest_return( &arm_ctx,",
        "context_x64_to_arm( &arm_ctx,",
        1,
    )
    if mutated == pe_source:
        raise AssertionError("could not construct unmarked guest-return mutation")
    expect_rejected(signal_source, mutated, unwind_source, provider_source,
                    "unmarked ARM64EC guest return")

    mutated = signal_source.replace(
        "TRUE, guest_return_requested, simulation_active,",
        "TRUE, guest_return_requested, TRUE,",
        1,
    )
    if mutated == signal_source:
        raise AssertionError("could not construct missing Unix guest-return mutation")
    expect_rejected(signal_source=mutated, pe_source=pe_source,
                    unwind_source=unwind_source,
                    provider_source=provider_source,
                    description="missing Unix guest-return provenance")

    mutated = signal_source.replace(
        "guest_return_requested );",
        "FALSE );",
        1,
    )
    if mutated == signal_source:
        raise AssertionError("could not construct unproven suspend handoff mutation")
    expect_rejected(mutated, pe_source, unwind_source, provider_source,
                    "unproven suspend-return handoff")

    reacquire = "            if (simulation_active) cpu_area->InSimulation = 1;\n"
    mutated = signal_source.replace(
        reacquire, "", 1
    )
    if mutated == signal_source:
        raise AssertionError("could not construct missing simulation reacquire mutation")
    expect_rejected(mutated, pe_source, unwind_source, provider_source,
                    "missing ntdll simulation reacquire")

    marker = "    context->AMD64_Context.ContextFlags |= CONTEXT_AMD64_FULL |\n"
    early_clear = "    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 0;\n"
    mutated = provider_source.replace(marker, early_clear + marker, 1)
    if mutated == provider_source:
        raise AssertionError("could not construct early simulation relinquish mutation")
    expect_rejected(signal_source, pe_source, unwind_source, mutated,
                    "early provider simulation relinquish")

    native_return = (
        "        xtajit64_restore_native( state, ec_context, cpu, "
        "params.unicorn_error );\n"
    )
    early_return_clear = "        cpu->InSimulation = 0;\n"
    mutated = provider_source.replace(
        native_return, early_return_clear + native_return, 1
    )
    if mutated == provider_source:
        raise AssertionError("could not construct pre-switch native-return mutation")
    expect_rejected(signal_source, pe_source, unwind_source, mutated,
                    "pre-switch native-return simulation relinquish")

    native_stack = '        "mov sp, x15\\n\\t"\n'
    early_assembly_clear = '        "strb wzr, [x2]\\n\\t"\n'
    mutated = provider_source.replace(
        native_stack, early_assembly_clear + native_stack, 1
    )
    if mutated == provider_source:
        raise AssertionError("could not construct pre-stack assembly mutation")
    expect_rejected(signal_source, pe_source, unwind_source, mutated,
                    "pre-stack assembly simulation relinquish")

    mutated = provider_source.replace('"dmb ish\\n\\t"', '"nop\\n\\t"', 1)
    if mutated == provider_source:
        raise AssertionError("could not construct missing handoff barrier mutation")
    expect_rejected(signal_source, pe_source, unwind_source, mutated,
                    "missing native handoff barrier")

    ownership_acquire = '        "stlrb w15, [x9]\\n\\t"'
    mutated = provider_source.replace(ownership_acquire, '        "nop\\n\\t"', 1)
    if mutated == provider_source:
        raise AssertionError("could not construct missing BeginSimulation acquire mutation")
    expect_rejected(signal_source, pe_source, unwind_source, mutated,
                    "missing BeginSimulation simulation ownership acquire")

    capture_ownership_acquire = '        "stlrb w10, [x17]\\n\\t"'
    mutated = provider_source.replace(
        capture_ownership_acquire, '        "nop\\n\\t"', 1
    )
    if mutated == provider_source:
        raise AssertionError("could not construct missing native-capture acquire mutation")
    expect_rejected(signal_source, pe_source, unwind_source, mutated,
                    "missing native-capture simulation ownership acquire")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"arm64ec emulation-dispatch source check failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
