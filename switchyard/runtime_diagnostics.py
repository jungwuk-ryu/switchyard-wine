#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Bounded, conservative failure categories for raw Wine stderr.

Categories are observations, not program correctness or runtime acceptance.
Callers must separately check exit status, reference output, identities and
cleanup. Never truncate a log to obtain a passing result. No input is printed.
"""
MAX_LOG_BYTES = 16 * 1024 * 1024
SCHEMA = "SWITCHYARD_RUNTIME_FAILURE_CATEGORIES_V1"
_MARKERS = {
    "unhandled_exception": b"unhandled",
    "assertion": b"assertion failed",
    "watchdog": b"diagnostic violation",
    "dispatcher_stack_overflow": b"virtual_setup_exception stack overflow",
    "driver_load": b"zwloaddriver failed to create driver",
}
_ABORT_MARKERS = (b"unsupported x64 simulation boundary", b"terminal transition abort", b"abort_transition")


def _ordered_same_line(log, before, after):
    """Equivalent to before.*after without repeated greedy-regex scans."""
    cursor = 0
    while True:
        start = log.find(before, cursor)
        if start < 0:
            return False
        end = log.find(b"\n", start)
        if end < 0:
            end = len(log)
        if log.rfind(after, start + len(before), end) >= 0:
            return True
        if end == len(log):
            return False
        cursor = end + 1


def failure_categories(log):
    if type(log) is not bytes:
        raise TypeError("raw Wine stderr must be immutable bytes")
    if len(log) > MAX_LOG_BYTES:
        raise ValueError("runtime log exceeds the diagnostic resource limit")
    lower = log.lower()
    found = {name for name, marker in _MARKERS.items() if marker in lower}
    if any(marker in lower for marker in _ABORT_MARKERS):
        found.add("provider_abort")
    if _ordered_same_line(lower, b"failed to load", b"xtajit64"):
        found.add("provider_load")
    if _ordered_same_line(lower, b"err:module:loader_init ", b"failed, status "):
        found.add("loader_initialization")
    if (_ordered_same_line(lower, b"err:module:import_dll loading library ", b" failed") or
        _ordered_same_line(lower, b"err:module:import_dll library ", b" not found")):
        found.add("import_dependency")
    return sorted(found)
