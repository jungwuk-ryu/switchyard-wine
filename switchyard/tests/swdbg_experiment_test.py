#!/usr/bin/env python3
import copy
import hashlib
import importlib.util
import json
import os
import pathlib
import plistlib
import stat
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "switchyard"))
SPEC = importlib.util.spec_from_file_location("swdbg", ROOT / "switchyard" / "swdbg.py")
SWDBG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SWDBG)


class SwdbgExperimentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="switchyard-swdbg-test.")
        os.chmod(self.temporary.name, 0o700)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.runtime = self.root / "runtime"
        self.loader = self.runtime / "lib" / "wine" / "aarch64-unix" / "wine"
        self.loader.parent.mkdir(parents=True)
        self.loader.write_bytes(b"arm64-loader-fixture")
        os.chmod(self.loader, 0o700)
        for relative in (
            "bin/wine",
            "lib/wine/aarch64-unix/ntdll.so",
            "lib/wine/aarch64-unix/xtajit64.so",
            "lib/wine/aarch64-unix/winemetal.so",
            "lib/switchyard-unicorn/lib/libunicorn.2.dylib",
        ):
            path = self.runtime / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
            os.chmod(path, 0o700)
        self.prefix = self.root / "prefix"
        self.working = self.prefix / "drive_c" / "Game"
        self.working.mkdir(parents=True)
        (self.working / "game.exe").write_bytes(b"PE fixture")
        revision = "a" * 40
        loader_sha = hashlib.sha256(self.loader.read_bytes()).hexdigest()
        runtime_manifest = {
            "cpuProvider": {"buildContractVersion": 12},
            "graphicsBackend": "dxmt-metal",
            "host": {"architecture": "arm64", "requiresRosetta": False},
            "installPrefix": str(self.runtime),
            "integrity": {"wineUnixSha256": loader_sha},
            "sourceDirty": False,
            "sourceRevision": revision,
            "wineRevision": revision,
        }
        runtime_manifest_path = self.runtime / "switchyard-runtime.json"
        runtime_manifest_path.write_text(json.dumps(runtime_manifest), encoding="utf-8")
        os.chmod(runtime_manifest_path, 0o600)
        self.document = {
            "app": {"bundleIdentifier": "io.switchyard.swdbg.fixture", "displayName": "Swdbg Fixture"},
            "argv": ["./game.exe"],
            "environment": {"WINEDEBUG": "-all", "WINE_XTAJIT64_DIAGNOSTICS": "1"},
            "experimentId": "fixture-1",
            "experimentRoot": str(self.root / "experiment"),
            "prefix": str(self.prefix),
            "runtime": {
                "contentSha256": "b" * 64,
                "loaderSha256": loader_sha,
                "providerContractVersion": 12,
                "root": str(self.runtime),
                "sourceRevision": revision,
            },
            "schemaVersion": 1,
            "workingDirectory": str(self.working),
        }

    def tearDown(self):
        self.temporary.cleanup()

    def validate(self, document=None):
        path = self.root / "experiment.json"
        if path.exists():
            path.unlink()
        path.write_text(json.dumps(document or self.document), encoding="utf-8")
        os.chmod(path, 0o600)
        return SWDBG.load_experiment(str(path), verify_content=False, platform_checks=False)

    def assert_rejected(self, document, text):
        with self.assertRaisesRegex(SWDBG.ExperimentError, text):
            self.validate(document)

    def test_accepts_closed_identity(self):
        experiment = self.validate()
        self.assertEqual(experiment["runtime"]["providerContractVersion"], 12)
        self.assertEqual(experiment["runtime"]["loaderSha256"], self.document["runtime"]["loaderSha256"])

    def test_rejects_stale_source_revision(self):
        document = copy.deepcopy(self.document)
        document["runtime"]["sourceRevision"] = "c" * 40
        self.assert_rejected(document, "source revision")

    def test_rejects_dirty_runtime(self):
        manifest = self.runtime / "switchyard-runtime.json"
        value = json.loads(manifest.read_text(encoding="utf-8"))
        value["sourceDirty"] = True
        manifest.write_text(json.dumps(value), encoding="utf-8")
        os.chmod(manifest, 0o600)
        with self.assertRaisesRegex(SWDBG.ExperimentError, "clean source tree"):
            self.validate()

    def test_rejects_loader_digest_mismatch(self):
        document = copy.deepcopy(self.document)
        document["runtime"]["loaderSha256"] = "d" * 64
        self.assert_rejected(document, "loader digest")

    def test_rejects_provider_contract_mismatch(self):
        document = copy.deepcopy(self.document)
        document["runtime"]["providerContractVersion"] = 11
        self.assert_rejected(document, "CPU-provider contract")

    def test_rejects_symlinked_prefix(self):
        linked = self.root / "linked-prefix"
        linked.symlink_to(self.prefix, target_is_directory=True)
        document = copy.deepcopy(self.document)
        document["prefix"] = str(linked)
        self.assert_rejected(document, "canonical absolute path")

    def test_rejects_runtime_environment_override(self):
        document = copy.deepcopy(self.document)
        document["environment"]["DYLD_INSERT_LIBRARIES"] = "/tmp/injected.dylib"
        self.assert_rejected(document, "not allowlisted")

    def test_requires_exact_x18_entitlements(self):
        good = plistlib.dumps(SWDBG.EXPECTED_ENTITLEMENTS)
        self.assertEqual(SWDBG.parse_entitlements(good), SWDBG.EXPECTED_ENTITLEMENTS)
        missing = dict(SWDBG.EXPECTED_ENTITLEMENTS)
        del missing["com.apple.security.custom-x18-abi-toggle"]
        with self.assertRaisesRegex(SWDBG.ExperimentError, "exact native ARM64 allowlist"):
            SWDBG.parse_entitlements(plistlib.dumps(missing))

    def test_generated_header_keeps_arguments_as_data(self):
        experiment = self.validate()
        experiment["argv"] = ["./game.exe", "quote\" newline\n unicode-한글"]
        header = SWDBG.generate_experiment_header(experiment, str(self.root / "Evidence"))
        self.assertNotIn(b'quote" newline', header)
        self.assertIn(b"static char swdbg_argument_1[]", header)

    def test_rejects_shared_existing_experiment_root(self):
        pathlib.Path(self.document["experimentRoot"]).mkdir()
        os.chmod(self.document["experimentRoot"], 0o755)
        self.assert_rejected(self.document, "private and user-owned")

    def test_detects_wine_preloaders(self):
        output = b"  123 /runtime/wine64-preloader /runtime/wine64-preloader game.exe\n"
        result = mock.Mock(stdout=output, returncode=0)
        with mock.patch.object(SWDBG.subprocess, "run", return_value=result):
            self.assertEqual(SWDBG.active_wine_processes(), [output.decode().strip()])

    def test_external_tools_receive_closed_environment(self):
        result = mock.Mock(stdout=b"", returncode=0)
        with mock.patch.object(SWDBG.subprocess, "run", return_value=result) as run:
            SWDBG.run_tool(["/usr/bin/true"], "fixture")
        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["PATH"], "/usr/bin:/bin:/usr/sbin:/sbin")
        self.assertEqual(set(environment), {"LANG", "LC_ALL", "PATH", "TMPDIR"})

    def test_launcher_clears_ambient_environment(self):
        source = (ROOT / "switchyard" / "swdbg_launcher.c").read_text(encoding="utf-8")
        self.assertIn("clear_environment()", source)
        self.assertIn("_NSGetEnviron()", source)
        self.assertNotIn("DYLD_INSERT_LIBRARIES", source)

    def test_remaining_timeout_never_extends_deadline(self):
        with mock.patch.object(SWDBG.time, "monotonic", return_value=100.0):
            self.assertEqual(SWDBG.remaining_timeout(105.0, 30, "fixture"), 5.0)
            self.assertEqual(SWDBG.remaining_timeout(105.0, 2, "fixture"), 2)
        with mock.patch.object(SWDBG.time, "monotonic", return_value=105.0):
            with self.assertRaisesRegex(SWDBG.ExperimentError, "fixture timed out"):
                SWDBG.remaining_timeout(105.0, 30, "fixture")


if __name__ == "__main__":
    unittest.main()
