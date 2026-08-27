#!/usr/bin/env python3
"""Apply the measured xtajit64 translated-block doorbell optimization."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
UNIXLIB = ROOT / "dlls/xtajit64/unixlib.c"
CHECKER = ROOT / "dlls/xtajit64/provider_tests/check_apple_silicon_hotpaths.py"
RUNNER = ROOT / "dlls/xtajit64/provider_tests/run_apple_silicon_hotpaths.sh"
BENCHMARK = ROOT / "dlls/xtajit64/provider_tests/apple_silicon_block_poll.c"


def replace_function(source: str, name: str, replacement: str) -> str:
    marker = f"static void {name}("
    start = source.find(marker)
    if start < 0:
        raise SystemExit(f"cannot find {name}()")
    brace = source.find("{", start)
    if brace < 0:
        raise SystemExit(f"cannot find body of {name}()")

    depth = 0
    end = -1
    for index in range(brace, len(source)):
        character = source[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end < 0:
        raise SystemExit(f"unterminated {name}()")
    return source[:start] + replacement.rstrip() + source[end:]


def replace_once(source: str, old: str, new: str, label: str) -> str:
    if source.count(old) != 1:
        raise SystemExit(f"{label} changed unexpectedly")
    return source.replace(old, new, 1)


def patch_unixlib() -> None:
    source = UNIXLIB.read_text(encoding="utf-8")
    replacement = r'''static void block_hook( uc_engine *uc, uint64_t address, uint32_t size, void *user )
{
    struct thread_engine *engine = user;
    uint32_t doorbell;

    (void)size;
    if (is_ec_code( address ))
    {
#ifdef XTAJIT64_UNIXLIB_TEST
        if (atomic_load_explicit( &test_hold_ec_hook, memory_order_acquire ))
        {
            atomic_store_explicit( &test_ec_hook_entered, 1, memory_order_release );
            while (!atomic_load_explicit( &test_release_ec_hook, memory_order_acquire ))
                sched_yield();
        }
#endif
        stop_at_ec_target( engine, uc, address );
        return;
    }

#ifdef XTAJIT64_UNIXLIB_TEST
    if (atomic_load_explicit( &test_hold_non_ec_hook, memory_order_acquire ))
    {
        atomic_store_explicit( &test_non_ec_hook_entered, 1, memory_order_release );
        while (!atomic_load_explicit( &test_release_non_ec_hook, memory_order_acquire ))
            sched_yield();
    }
#endif

    /* begin_simulation publishes this identity-mapped doorbell before setting
     * engine->running and entering uc_emu_start(), and release clears it only
     * after emulation has returned.  A running block callback therefore always
     * has a valid pointer.  Remove the redundant nullable-pointer branch while
     * preserving doorbell-first stop priority and the acquire pause poll. */
    doorbell = *engine->suspend_doorbell;
    if (doorbell)
    {
        engine->stop_reason = XTAJIT64_STOP_SUSPEND;
        if (engine->flight_recorder)
            flight_record_engine_event( engine,
                                        XTAJIT64_FLIGHT_EVENT_SUSPEND_ACKNOWLEDGED,
                                        XTAJIT64_FLIGHT_REASON_NONE,
                                        XTAJIT64_STOP_SUSPEND, address,
                                        XTAJIT64_FLIGHT_UNKNOWN_U64 );
        stop_at_instruction_boundary( engine, uc );
        return;
    }
    if (!atomic_load_explicit( &engine->pause_requested, memory_order_acquire )) return;
    if (engine->flight_recorder)
        flight_record_engine_event( engine, XTAJIT64_FLIGHT_EVENT_SUSPEND_ACKNOWLEDGED,
                                    XTAJIT64_FLIGHT_REASON_NONE,
                                    XTAJIT64_FLIGHT_UNKNOWN_U32,
                                    address, XTAJIT64_FLIGHT_UNKNOWN_U64 );
#ifdef XTAJIT64_UNIXLIB_TEST
    if (!pthread_equal( engine->owner, pthread_self() ))
        atomic_store_explicit( &test_pause_stop_owner_violation, 1, memory_order_relaxed );
    atomic_fetch_add_explicit( &test_pause_stop_count, 1, memory_order_relaxed );
#endif
    stop_at_instruction_boundary( engine, uc );
}
'''
    source = replace_function(source, "block_hook", replacement)
    UNIXLIB.write_text(source, encoding="utf-8")


def patch_checker() -> None:
    source = CHECKER.read_text(encoding="utf-8")
    marker = '    print("xtajit64 Apple Silicon hot-path contracts passed")\n'
    if source.count(marker) != 1:
        raise SystemExit("checker completion marker changed unexpectedly")
    insertion = r'''    block_hook = function_body(source, "block_hook")
    begin_simulation = function_body(source, "begin_simulation")
    if "engine->suspend_doorbell &&" in block_hook:
        fail("block_hook() retained the per-block nullable-doorbell branch")
    if "doorbell = *engine->suspend_doorbell;" not in block_hook:
        fail("block_hook() does not use the running-engine doorbell invariant")
    poll_start = block_hook.find("doorbell = *engine->suspend_doorbell;")
    pause_poll = block_hook.find(
        "if (!atomic_load_explicit( &engine->pause_requested, memory_order_acquire )) return;"
    )
    if poll_start < 0 or pause_poll <= poll_start:
        fail("block_hook() does not retain the measured acquire early-return poll")
    if "memory_order_relaxed" in block_hook[poll_start:pause_poll]:
        fail("block_hook() unexpectedly weakened the common stop ordering")
    if "if (!(doorbell |" in block_hook:
        fail("block_hook() uses the rejected fused common-return branch")
    if block_hook.find("if (doorbell)", poll_start) > pause_poll:
        fail("block_hook() no longer preserves doorbell-first stop priority")
    publish = begin_simulation.find("engine->suspend_doorbell = suspend_doorbell;")
    running = begin_simulation.find("engine->running = TRUE;")
    emulate = begin_simulation.find("uc_emu_start(")
    if publish < 0 or running <= publish or emulate <= running:
        fail("begin_simulation() no longer publishes the doorbell before emulation")

'''
    source = source.replace(marker, insertion + marker, 1)
    CHECKER.write_text(source, encoding="utf-8")


def patch_runner() -> None:
    source = RUNNER.read_text(encoding="utf-8")
    marker = '/usr/bin/arch -arm64 "$benchmark"\n'
    if source.count(marker) != 1:
        raise SystemExit("benchmark runner completion changed unexpectedly")
    addition = r'''

block_poll_benchmark="$work_dir/apple_silicon_block_poll"
/usr/bin/clang -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$unicorn_source/include" \
    "$source_dir/dlls/xtajit64/provider_tests/apple_silicon_block_poll.c" \
    -L"$library_dir" -Wl,-rpath,"$library_dir" -lunicorn \
    -o "$block_poll_benchmark"

/usr/bin/file "$block_poll_benchmark"
/usr/bin/arch -arm64 "$block_poll_benchmark"
'''
    source = source.replace(marker, marker + addition, 1)
    RUNNER.write_text(source, encoding="utf-8")


def patch_benchmark() -> None:
    source = BENCHMARK.read_text(encoding="utf-8")
    source = replace_once(
        source,
        "#define ITERATIONS UINT64_C(500000)",
        "#define ITERATIONS UINT64_C(2000000)",
        "block-poll iteration count",
    )
    source = replace_once(
        source,
        "           direct_relaxed_ns, baseline_ns / direct_relaxed_ns);",
        "           direct_relaxed_ns, baseline_ns / direct_acquire_ns);",
        "block-poll speedup target",
    )
    source = replace_once(
        source,
        '           "pause_order=acquire->relaxed common_path=negative-early-return\\n");',
        '           "pause_order=acquire common_path=negative-early-return\\n");',
        "block-poll contract output",
    )
    source = replace_once(
        source,
        "    if (!(direct_relaxed_ns < baseline_ns))\n"
        "    {\n"
        "        fprintf(stderr, \"direct relaxed block poll did not beat baseline\\n\");",
        "    if (!(direct_acquire_ns < baseline_ns))\n"
        "    {\n"
        "        fprintf(stderr, \"direct acquire block poll did not beat baseline\\n\");",
        "block-poll performance gate",
    )
    BENCHMARK.write_text(source, encoding="utf-8")


def main() -> None:
    patch_unixlib()
    patch_checker()
    patch_runner()
    patch_benchmark()


if __name__ == "__main__":
    main()
