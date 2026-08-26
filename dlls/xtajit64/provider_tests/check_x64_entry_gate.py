#!/usr/bin/env python3
"""Verify that every xtajit64 x64-entry path passes the ntdll resync gate."""

from pathlib import Path
import re
import sys


def function_body(source: str, name: str) -> str:
    pattern = re.compile(
        rf"(?m)^[A-Za-z_][A-Za-z0-9_\s*(),]*\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        re.DOTALL,
    )
    match = pattern.search(source)
    if not match:
        raise AssertionError(f"missing function {name}")
    start = source.find("{", match.start())
    depth = 0
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if not depth:
                return source[start : offset + 1]
    raise AssertionError(f"unterminated function {name}")


def require(body: str, token: str, context: str, count: int = 1) -> None:
    actual = body.count(token)
    if actual != count:
        raise AssertionError(f"{context} has {actual} {token!r} references, expected {count}")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CPU_C", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding="utf-8")

    run = function_body(source, "run_x64_simulation")
    gate = "__wine_arm64ec_prepare_x64_execution()"
    begin = "XTAJIT64_CALL( begin_simulation"
    require(run, gate, "run_x64_simulation")
    if run.find(gate) > run.find(begin):
        raise AssertionError("run_x64_simulation reaches the provider before the ntdll gate")

    begin = function_body(source, "BeginSimulation")
    require(begin, r'b \"#begin_simulation_on_control_stack\"', "BeginSimulation")
    control = function_body(source, "begin_simulation_on_control_stack")
    require(control, "run_x64_simulation( state )",
            "begin_simulation_on_control_stack")
    for token in ("state != get_thread_state()",
                  "state->magic != XTAJIT64_THREAD_STATE_MAGIC",
                  "discard_unwound_transition_frames"):
        require(control, token, "begin_simulation_on_control_stack")
    require(function_body(source, "xtajit64_transition_from_native"),
            "run_x64_simulation( state )", "xtajit64_transition_from_native", 3)
    require(function_body(source, "xtajit64_capture_native"),
            r'b \"#xtajit64_transition_from_native\"', "xtajit64_capture_native")
    require(function_body(source, "capture_transition"),
            r'b \"#xtajit64_capture_native\"', "capture_transition")
    for thunk in ("DispatchJump", "RetToEntryThunk", "ExitToX64"):
        require(function_body(source, thunk), r'b \"#capture_transition\"', thunk)

    write_guest = function_body(source, "write_guest_u64")
    require(write_guest, "XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE",
            "write_guest_u64")
    if "XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ" in write_guest:
        raise AssertionError("write_guest_u64 accepts a read-only guest mapping")

    resolver = function_body(source, "resolve_arm64ec_export")
    require(resolver, "!metadata->RedirectionMetadataCount",
            "resolve_arm64ec_export")

    print("xtajit64 x64-entry resync gate coverage verified")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"x64-entry gate check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
