#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Raw-log classification must not confuse an exit code with acceptance."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("switchyard_runtime_diagnostics",
    Path(__file__).resolve().parents[1] / "runtime_diagnostics.py")
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class Contracts(unittest.TestCase):
    def test_legacy_categories(self):
        for name, message in (
            ("unhandled_exception", b"wine: Unhandled page fault"),
            ("assertion", b"Assertion failed"),
            ("watchdog", b"diagnostic violation"),
            ("dispatcher_stack_overflow", b"virtual_setup_exception stack overflow"),
            ("driver_load", b"ZwLoadDriver failed to create driver"),
            ("provider_abort", b"terminal transition abort"),
            ("provider_load", b"failed to load provider xtajit64.dll"),
        ):
            with self.subTest(name=name):
                self.assertEqual(diagnostics.failure_categories(message), [name])

    def test_loader_failure_is_independent_of_unhandled_exception(self):
        message = b"0024:err:module:loader_init Importing dlls for program.exe failed, status c0000135\n"
        self.assertEqual(diagnostics.failure_categories(message), ["loader_initialization"])

    def test_dependency_failure_forms_and_alternate_names(self):
        for message in (
            b"002c:err:module:import_dll Loading library anything.dll (which is needed) failed (error c000007b).\n",
            b"00b0:err:module:import_dll Library arbitrary.dll (which is needed) not found\r\n",
            b"err:module:import_dll Library other.dll not found\n",
        ):
            self.assertEqual(diagnostics.failure_categories(message), ["import_dependency"])

    def test_successful_or_expected_allocation_fallback_is_not_a_loader_failure(self):
        message = (b"0024:trace:loaddll:build_module Loaded library at address: builtin\n"
            b"0024:err:virtual:try_map_free_area mmap() error Cannot allocate memory\n"
            b"SWITCHYARD_LLVM_MAIN_PHASE_V2 wall_ticks=10 frequency=10 user_100ns=0 kernel_100ns=0\n")
        self.assertEqual(diagnostics.failure_categories(message), [])

    def test_case_crlf_and_duplicate_events(self):
        message = b"0024:ERR:MODULE:LOADER_INIT IMPORTING DLLS FAILED, STATUS C0000135\r\n"
        self.assertEqual(diagnostics.failure_categories(message * 2), ["loader_initialization"])

    def test_debug_pid_tid_timestamp_prefixes_do_not_hide_failure(self):
        for prefix in (b"0024:", b"0024:002c:", b"12345.678:0024:002c:"):
            message = prefix + b"err:module:loader_init Importing dlls failed, status c0000135\n"
            self.assertEqual(diagnostics.failure_categories(message), ["loader_initialization"])

    def test_provider_marker_does_not_join_separate_lines(self):
        self.assertEqual(diagnostics.failure_categories(b"failed to load other.dll\nxtajit64\n"), [])
        self.assertEqual(diagnostics.failure_categories(b"xtajit64 failed to load something\n"), [])
        self.assertEqual(diagnostics.failure_categories(b"failed to load a failed to load b xtajit64\n"), ["provider_load"])

    def test_repeated_nonmatching_prefixes_remain_bounded(self):
        message = b"failed to load " * 40000 + b"\nxtajit64\n"
        self.assertEqual(diagnostics.failure_categories(message), [])

    def test_empty_and_malformed_bytes_do_not_require_unicode_decode(self):
        self.assertEqual(diagnostics.failure_categories(b""), [])
        self.assertEqual(diagnostics.failure_categories(b"\xff\x00\x80"), [])

    def test_input_type_and_size_are_explicit(self):
        for value in ("not raw bytes", bytearray(b"raw"), None):
            with self.subTest(value=value), self.assertRaises(TypeError):
                diagnostics.failure_categories(value)
        with self.assertRaises(ValueError):
            diagnostics.failure_categories(b"x" * (diagnostics.MAX_LOG_BYTES + 1))


if __name__ == "__main__":
    unittest.main()
