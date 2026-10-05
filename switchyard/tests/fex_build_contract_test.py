#!/usr/bin/env python3
"""Small fail-closed tests for the clean FEX packaging/test-input contract."""
import ast
import importlib.util
import json
from pathlib import Path
import re
import shlex
import sys
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "fex_test_gate", ROOT / "switchyard/fex/tests/verify_test_suite.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
SDK = None
if len(sys.argv) >= 3 and sys.argv[1] == "--sdk":
    SDK = Path(sys.argv[2]).resolve()
    del sys.argv[1:3]


def results():
    report = ET.Element("testsuite", tests=str(len(gate.EXPECTED)), failures="0", disabled="0", skipped="0")
    for name in sorted(gate.EXPECTED):
        ET.SubElement(report, "testcase", name=name, status="run")
    return report


def require_vector_fallback_outline(source):
    """Source-placement guard, not a substitute for native whole-byte/stack tests."""
    if not isinstance(source, str) or not 0 < len(source) <= 256 * 1024:
        raise ValueError("adapter source is outside the contract bound")
    bodies = {}
    headers = {
        "ImportGenericVectorState": r"static void __attribute__\(\(noinline\)\) ",
        "ExportGenericVectorState": r"static void __attribute__\(\(noinline\)\) ",
        "ExportStateUnlocked": r"static switchyard_fex_result ",
        "switchyard_fex_thread_import_state": r"switchyard_fex_result ",
    }
    for name, header in headers.items():
        matches = re.findall(r"^" + header + name + r"\([^{}]*\{\n(.*?)^\}", source, re.M | re.S)
        if len(matches) != 1:
            raise ValueError("missing/ambiguous vector boundary " + name)
        bodies[name] = matches[0]
    for name in ("ImportGenericVectorState", "ExportGenericVectorState"):
        if any(temporary not in bodies[name] for temporary in (
                "__uint128_t XMM[16] {};", "__uint128_t YMMHigh[16] {};")):
            raise ValueError("generic vector temporaries are not in the owned fallback")
    for name, call in (("switchyard_fex_thread_import_state", "ImportGenericVectorState(Thread, Input);"),
                       ("ExportStateUnlocked", "ExportGenericVectorState(Thread, Output);")):
        if "__uint128_t" in bodies[name] or call not in bodies[name]:
            raise ValueError("generic vector temporaries entered the split-vector boundary")


class TestVectorFallbackPlacement(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "switchyard/fex/src/switchyard_fex.cpp").read_text()

    def test_current_source(self):
        require_vector_fallback_outline(self.source)

    def test_before_inline_temporaries_rejected(self):
        mutant = self.source.replace("ImportGenericVectorState(Thread, Input);",
                                     "__uint128_t XMM[16] {};", 1)
        with self.assertRaises(ValueError):
            require_vector_fallback_outline(mutant)

    def test_inlining_or_missing_fallback_rejected(self):
        for mutant in (self.source.replace("__attribute__((noinline))", "", 1),
                       self.source.replace("ExportGenericVectorState(Thread, Output);", "", 1)):
            with self.subTest(mutant=mutant[:32]), self.assertRaises(ValueError):
                require_vector_fallback_outline(mutant)

    def test_bounded_source(self):
        for bad in (None, "", " " * (256 * 1024 + 1)):
            with self.assertRaises(ValueError):
                require_vector_fallback_outline(bad)


class TestPlan(unittest.TestCase):
    def test_complete(self):
        gate.check_plan(json.dumps({"tests": [{"name": name} for name in gate.EXPECTED]}))

    def test_missing(self):
        with self.assertRaises(ValueError):
            gate.check_plan(json.dumps({"tests": [{"name": name} for name in sorted(gate.EXPECTED)[1:]]}))

    def test_extra(self):
        with self.assertRaises(ValueError):
            gate.check_plan(json.dumps({"tests": [{"name": name} for name in gate.EXPECTED] + [{"name": "unowned"}]}))

    def test_duplicate(self):
        names = sorted(gate.EXPECTED)
        names[-1] = names[0]
        with self.assertRaises(ValueError):
            gate.check_plan(json.dumps({"tests": [{"name": name} for name in names]}))

    def test_no_custom_x18_gate(self):
        with self.assertRaises(ValueError):
            gate.check_plan(json.dumps({"tests": [{"name": name} for name in (
                "switchyard-fex-c-api", "switchyard-fex-allocator", "switchyard-fex-allocator-fallback")]}))

    def test_malformed(self):
        for value in ({}, None, [], {"tests": {}}, {"tests": "13"}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.check_plan(json.dumps(value))


class TestResults(unittest.TestCase):
    def test_complete(self):
        gate.check_results(ET.tostring(results()))

    def test_missing(self):
        report = results()
        report.remove(report[0])
        with self.assertRaises(ValueError):
            gate.check_results(ET.tostring(report))

    def test_failed_skipped_error(self):
        for tag in ("failure", "skipped", "error"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                report = results()
                ET.SubElement(report[0], tag)
                gate.check_results(ET.tostring(report))

    def test_not_run(self):
        for status in ("notrun", "disabled", "", None):
            with self.subTest(status=status), self.assertRaises(ValueError):
                report = results()
                if status is None:
                    del report[0].attrib["status"]
                else:
                    report[0].set("status", status)
                gate.check_results(ET.tostring(report))

    def test_inconsistent_totals(self):
        for field in ("tests", "failures", "disabled", "skipped"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                report = results()
                report.set(field, "12" if field == "tests" else "1")
                gate.check_results(ET.tostring(report))

    def test_bad_root(self):
        report = results()
        report.tag = "testsuites"
        with self.assertRaises(ValueError):
            gate.check_results(ET.tostring(report))


class TestSourceClosure(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.recipe = (ROOT / "switchyard/build_fex_runtime.sh").read_text()

    def test_full_adapter_inputs(self):
        listed = set(shlex.split(re.search(r"^ADAPTER_FILES=\((.*?)^\)", self.recipe,
                                           re.S | re.M)[1]))
        actual = {str(path.relative_to(ROOT / "switchyard/fex"))
                  for path in (ROOT / "switchyard/fex").rglob("*") if path.is_file()}
        self.assertEqual(listed, actual)
        self.assertIn("tests/verify_test_suite.py", listed)

    def test_wine_inputs(self):
        listed = set(shlex.split(re.search(r"^WINE_TEST_FILES=\((.*?)^\)", self.recipe,
                                           re.S | re.M)[1]))
        self.assertEqual(listed, {"COPYING.LIB", "LICENSE", "include/wine/asm.h",
                                  "dlls/ntdll/unix/signal_arm64.c"})
        self.assertIn('-DSWITCHYARD_FEX_WINE_SOURCE_DIR="$ADAPTER_SOURCE/wine"', self.recipe)

    def test_configure_reports_the_required_abi(self):
        for name in ("configure", "configure.ac"):
            source = (ROOT / name).read_text()
            self.assertIn("Switchyard FEX ABI v6 development files with register windows, shared admission", source)
            self.assertNotIn("Switchyard FEX ABI v5 development files", source)
            for symbol in ("switchyard_fex_thread_import_register_window",
                           "switchyard_fex_thread_export_register_window"):
                self.assertIn(symbol, source)

    def test_explicit_ec_layout_and_all_build_targets(self):
        self.assertIn("-DSWITCHYARD_FEX_ARM64EC_REGISTER_ABI=ON", self.recipe)
        for target in ("switchyard-fex-c-api-test", "switchyard-fex-darwin-abi-test",
                       "switchyard-fex-allocator-test", "switchyard-fex-allocator-fallback-test",
                       "switchyard-fex-register-window-test", "switchyard-fex-register-window-state-test"):
            self.assertIn(target, self.recipe)
        self.assertIn('verify_test_suite.py" plan ', self.recipe)
        self.assertIn('verify_test_suite.py" results ', self.recipe)
        self.assertNotIn("libswitchyard-fex.3", self.recipe)

    def test_fixed_logical_prefix_and_exclusive_publication(self):
        self.assertIn("-DCMAKE_INSTALL_PREFIX=/usr/local", self.recipe)
        self.assertIn('ensure_preview_swap_helper', self.recipe)
        self.assertIn('"$STAGING" "$OUTPUT" exclusive', self.recipe)
        self.assertNotIn('/bin/mv -n "$STAGING"', self.recipe)


class TestReportCommand(unittest.TestCase):
    def test_bounded_report(self):
        with tempfile.TemporaryDirectory(prefix="switchyard-fex-report-") as temporary:
            report = Path(temporary) / "report"
            report.write_bytes(b" " * (gate.MAX_REPORT + 1))
            result = subprocess.run([sys.executable, "-I", str(ROOT / "switchyard/fex/tests/verify_test_suite.py"),
                                     "plan", str(report)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exceeds the gate bound", result.stderr)

    def test_missing_report(self):
        with tempfile.TemporaryDirectory(prefix="switchyard-fex-report-") as temporary:
            result = subprocess.run([sys.executable, "-I", str(ROOT / "switchyard/fex/tests/verify_test_suite.py"),
                                     "results", str(Path(temporary) / "absent")], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)


class TestProviderReadBounds(unittest.TestCase):
    def blocks(self, payload, size):
        source = (ROOT / "switchyard/lib/native_cpu_provider.sh").read_text()
        start = source.index("def bounded_blocks(")
        end = source.index("\ndef digest_regular(", start)
        node, = ast.parse(source[start:end]).body
        self.assertIsInstance(node, ast.FunctionDef)

        class Reader:
            offset = 0
            largest = 0

            def read(reader, descriptor, count):
                reader.largest = max(reader.largest, count)
                block = payload[reader.offset:reader.offset + count]
                reader.offset += len(block)
                return block

        reader = Reader()
        def fail(message):
            raise ValueError(message)
        namespace = {"os": reader, "fail": fail}
        exec(compile(ast.Module(body=[node], type_ignores=[]), "provider-read-bound", "exec"), namespace)
        return namespace["bounded_blocks"](0, size, "fixture"), reader

    def test_exact_extent(self):
        payload = b"x" * (1024 * 1024 + 5)
        blocks, reader = self.blocks(payload, len(payload))
        self.assertEqual(b"".join(blocks), payload)
        self.assertLessEqual(reader.largest, 1024 * 1024)

    def test_growing_file_is_bounded(self):
        blocks, reader = self.blocks(b"x" * 1024, 7)
        with self.assertRaisesRegex(ValueError, "grew during inspection"):
            list(blocks)
        self.assertEqual(reader.offset, 8)

    def test_truncated_file(self):
        blocks, reader = self.blocks(b"x" * 7, 1024)
        with self.assertRaisesRegex(ValueError, "ended early"):
            list(blocks)
        self.assertEqual(reader.offset, 7)


class TestWineConfigureClosure(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="switchyard-fex-configure-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.makefile = self.root / "Makefile"
        self.statusfile = self.root / "config.status"
        self.sdk = str(self.root / "sdk with spaces")
        self.assignments = {
            "PE_ARCHS": " aarch64 arm64ec x86_64",
            "SWITCHYARD_FEX_CFLAGS": "-I" + self.sdk + "/include",
            "SWITCHYARD_FEX_LIBS": "-L" + self.sdk + "/lib -lswitchyard-fex",
            "XTAJIT64_UNIXLIB": "xtajit64.so",
            "XTAJIT64_PE_CFLAGS": "-DHAVE_SWITCHYARD_FEX",
            "XTAJIT_UNIXLIB": "",
            "XTAJIT_PE_CFLAGS": "",
            "WINEMETAL_WOW64_UNIXLIB": "winemetal-wow64.so",
        }
        self.arguments = ["--enable-archs=aarch64,arm64ec,x86_64", "--with-fex=" + self.sdk,
                          "--disable-xtajit"]
        self.write()

    def write(self):
        content = "".join(name + " = " + value + "\n"
                          for name, value in self.assignments.items())
        self.makefile.write_text(content)
        self.statusfile.write_text("ac_cs_config=" + shlex.quote(shlex.join(self.arguments)) + "\n" + content)

    def validate(self, expected):
        command = "source " + shlex.quote(str(ROOT / "switchyard/lib/fex_contract.sh")) + \
                  "\nswitchyard_native_configured_fex_policy_is_exact " + \
                  shlex.join((str(self.makefile), str(self.statusfile), self.sdk)) + "\n"
        result = subprocess.run(["/bin/bash", "-s"], input=command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if expected else 1, result.stderr)

    def test_exact(self):
        self.validate(True)

    def test_legacy_effective_architectures(self):
        self.assignments["PE_ARCHS"] += " i386"
        self.write()
        self.validate(False)

    def test_legacy_reconfigure_architectures(self):
        self.arguments[0] += ",i386"
        self.write()
        self.validate(False)

    def test_stub_provider_not_disabled(self):
        self.arguments.remove("--disable-xtajit")
        self.write()
        self.validate(False)

    def test_stub_provider_reenabled_or_ambiguous(self):
        for argument in ("--enable-xtajit", "--enable-xtajit=aarch64",
                         "--disable-xtajit=no", "--disable-xtajit"):
            with self.subTest(argument=argument):
                self.arguments.append(argument)
                self.write()
                self.validate(False)
                self.arguments.pop()

    def test_retired_provider_effective_rule(self):
        with self.makefile.open("a") as stream:
            stream.write("dlls/xtajit/aarch64-windows/xtajit.dll: retired-provider\n")
        self.validate(False)

    def test_wrong_provider_or_abi(self):
        for name, value in (("XTAJIT64_PE_CFLAGS", "-DHAVE_UNICORN"),
                            ("XTAJIT_UNIXLIB", "xtajit.so"),
                            ("SWITCHYARD_FEX_LIBS", "-lswitchyard-fex-old")):
            with self.subTest(name=name):
                original = self.assignments[name]
                self.assignments[name] = value
                self.write()
                self.validate(False)
                self.assignments[name] = original

    def test_missing_or_duplicate_assignment(self):
        for path in (self.makefile, self.statusfile):
            with self.subTest(path=path.name):
                self.write()
                path.write_text(path.read_text() + "XTAJIT_UNIXLIB = \n")
                self.validate(False)
                self.write()
                path.write_text(path.read_text().replace("XTAJIT_UNIXLIB = \n", ""))
                self.validate(False)

    def test_extra_or_wrong_provider_argument(self):
        for argument in ("--with-unicorn=/private/legacy", "--without-fex",
                         "--with-fex=/private/other", "--enable-archs=aarch64,arm64ec,x86_64"):
            with self.subTest(argument=argument):
                self.arguments.append(argument)
                self.write()
                self.validate(False)
                self.arguments.pop()

    def test_symlink_configuration(self):
        target = self.root / "target"
        self.makefile.rename(target)
        self.makefile.symlink_to(target)
        self.validate(False)

    def test_oversized_configuration(self):
        self.statusfile.write_bytes(b" " * (16 * 1024 * 1024 + 1))
        self.validate(False)

    def test_large_generated_makefile(self):
        with self.makefile.open("ab") as stream:
            block = b"# generated rules\n" * 65536
            for _ in range(16):
                stream.write(block)
        self.validate(True)

    def test_oversized_makefile(self):
        with self.makefile.open("ab") as stream:
            stream.truncate(128 * 1024 * 1024 + 1)
        self.validate(False)

    def test_bad_utf8_or_arguments(self):
        self.statusfile.write_bytes(b"\xff")
        self.validate(False)
        self.write()
        self.statusfile.write_text(self.statusfile.read_text().replace("ac_cs_config=", "ac_cs_config='", 1))
        self.validate(False)

    def test_sdk_is_not_executed(self):
        sentinel = self.root / "executed"
        self.sdk += "$(touch " + str(sentinel) + ")"
        self.assignments["SWITCHYARD_FEX_CFLAGS"] = "-I" + self.sdk + "/include"
        self.assignments["SWITCHYARD_FEX_LIBS"] = "-L" + self.sdk + "/lib -lswitchyard-fex"
        self.arguments[1] = "--with-fex=" + self.sdk
        self.write()
        self.validate(True)
        self.assertFalse(sentinel.exists())


class TestFullWineProfileGate(unittest.TestCase):
    def test_native_builder_disables_i386_provider(self):
        source = (ROOT / "switchyard/build_runtime.sh").read_text()
        options = re.search(r"profile_configure_options\+=\((.*?)\n    \)", source, re.S)[1]
        self.assertIn('"--disable-xtajit"', options)

    def test_rosetta_remains_enabled(self):
        command = '. ' + shlex.quote(str(ROOT / "switchyard/lib/runtime_profile.sh")) + \
                  '\nswitchyard_load_runtime_profile stable-x86_64-rosetta\nswitchyard_require_runtime_profile_enabled\n'
        result = subprocess.run(["/bin/bash", "-s"], input=command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_legacy_fex_profile_fails_before_unicorn(self):
        command = '. ' + shlex.quote(str(ROOT / "switchyard/lib/runtime_profile.sh")) + \
                  '\nswitchyard_load_runtime_profile preview-native-arm64-fex\nswitchyard_require_runtime_profile_enabled\n'
        result = subprocess.run(["/bin/bash", "-s"], input=command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("No TCG fallback is permitted", result.stderr)

    def test_builder_stops_before_native_preflight(self):
        result = subprocess.run(["/bin/bash", str(ROOT / "switchyard/build_runtime.sh"),
                                 "--runtime-profile", "preview-native-arm64-fex", "--source-info"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("FEX full-Wine packaging qualification is incomplete", result.stderr)
        self.assertEqual(result.stdout, "")


if SDK is not None:
    class TestPackagedClosure(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            recipe = (ROOT / "switchyard/build_fex_runtime.sh").read_text()
            cls.functions = recipe.split('\nwhile [ "$#" -gt 0 ]; do\n', 1)[0]
            root_line = next(line for line in cls.functions.splitlines() if line.startswith("ROOT_DIR="))
            cls.functions = cls.functions.replace(root_line, "ROOT_DIR=" + shlex.quote(str(ROOT)), 1)
            cls.validate(SDK, True)

        @classmethod
        def validate(cls, path, expected):
            result = subprocess.run(["/bin/bash", "-s"], input=cls.functions +
                                    "\nvalidate_output " + shlex.quote(str(path)) + "\n",
                                    capture_output=True, text=True)
            if (result.returncode == 0) != expected:
                raise AssertionError("unexpected closure validation: " + result.stdout + result.stderr)

        def altered(self, mutation):
            with tempfile.TemporaryDirectory(prefix="switchyard-fex-closure-") as temporary:
                fixture = Path(temporary) / "sdk"
                subprocess.run(["/bin/cp", "-cR", str(SDK), str(fixture)], check=True)
                mutation(fixture, Path(temporary))
                self.validate(fixture, False)

        def manifest(self, key, value):
            def mutate(fixture, temporary):
                path = fixture / "switchyard-fex-runtime.json"
                document = json.loads(path.read_text())
                document[key] = value
                path.write_text(json.dumps(document) + "\n")
            self.altered(mutate)

        def test_reusable_exact_output(self):
            result = subprocess.run(["/bin/bash", str(ROOT / "switchyard/build_fex_runtime.sh"),
                                     "--output", str(SDK)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(SDK))

        def test_missing_library(self):
            self.altered(lambda fixture, temporary: (fixture / "lib/libswitchyard-fex.6.0.0.dylib").unlink())

        def test_missing_admission_header(self):
            self.altered(lambda fixture, temporary: (fixture / "include/switchyard_fex_admission.h").unlink())

        def test_wrong_version_link(self):
            def mutate(fixture, temporary):
                path = fixture / "lib/libswitchyard-fex.dylib"
                path.unlink()
                path.symlink_to("libswitchyard-fex.3.dylib")
            self.altered(mutate)

        def test_escaping_source_link(self):
            self.altered(lambda fixture, temporary: (fixture / "escape").symlink_to(temporary))

        def test_extra_payload(self):
            self.altered(lambda fixture, temporary: (fixture / "unowned").write_bytes(b"extra"))

        def test_corresponding_source_changed(self):
            self.altered(lambda fixture, temporary: (
                fixture / "share/src/switchyard-fex/adapter/src/switchyard_fex.cpp").write_bytes(b"changed"))

        def test_wrong_provider(self):
            self.manifest("providerIdentity", "switchyard-fex-provider-abi-v3-darwin-low-shadow-external-stops")

        def test_generic_layout(self):
            self.manifest("arm64ecRegisterABI", False)

        def test_boolean_string_not_boolean(self):
            self.manifest("arm64ecRegisterABI", "True")

        def test_wrong_toolchain(self):
            self.manifest("toolchainSha256", "0" * 64)

        def test_wrong_library_path(self):
            self.manifest("library", "lib/libswitchyard-fex.3.0.0.dylib")

        def test_malformed_manifest(self):
            self.altered(lambda fixture, temporary: (fixture / "switchyard-fex-runtime.json").write_bytes(b"[]"))

        def test_bounded_manifest(self):
            self.altered(lambda fixture, temporary: (
                fixture / "switchyard-fex-runtime.json").write_bytes(b" " * (1024 * 1024 + 1)))


class TestExclusivePublication(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="switchyard-fex-publish-")
        cls.directory = Path(cls.temporary.name)
        command = '. ' + shlex.quote(str(ROOT / "switchyard/lib/directory_safety.sh")) + \
                  '\nSWAP_HELPER_DIR=' + shlex.quote(str(cls.directory)) + '\nensure_preview_swap_helper\n'
        subprocess.run(["/bin/bash", "-s"], input=command, text=True, check=True)
        cls.helper = cls.directory / "switchyard-preview-directory-publish"

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def fixture(self):
        temporary = tempfile.TemporaryDirectory(prefix="switchyard-fex-publication-")
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        stage = directory / "stage"
        stage.mkdir()
        return stage, directory / "published"

    def test_publish_exact_inode(self):
        stage, target = self.fixture()
        identity = stage.stat().st_ino
        subprocess.run([str(self.helper), str(stage), str(target), "exclusive"], check=True)
        self.assertFalse(stage.exists())
        self.assertEqual(target.stat().st_ino, identity)

    def test_racing_directory_is_not_overwritten_or_nested_into(self):
        stage, target = self.fixture()
        target.mkdir()
        (target / "sentinel").write_bytes(b"preserve")
        identities = stage.stat().st_ino, target.stat().st_ino
        result = subprocess.run([str(self.helper), str(stage), str(target), "exclusive"], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(identities, (stage.stat().st_ino, target.stat().st_ino))
        self.assertEqual(sorted(path.name for path in target.iterdir()), ["sentinel"])

    def test_symlink_is_not_followed(self):
        stage, target = self.fixture()
        target.symlink_to(stage)
        result = subprocess.run([str(self.helper), str(stage), str(target), "exclusive"], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(stage.is_dir())
        self.assertTrue(target.is_symlink())


if __name__ == "__main__":
    unittest.main(verbosity=2)
