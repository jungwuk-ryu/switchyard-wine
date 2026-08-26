#!/usr/bin/env python3
"""Run fail-closed, attributable Switchyard Wine experiments on macOS ARM64."""

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import time


_DIGEST_MODULE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "runtime_content_digest.py")
_DIGEST_SPEC = importlib.util.spec_from_file_location("switchyard_runtime_content_digest", _DIGEST_MODULE_PATH)
if _DIGEST_SPEC is None or _DIGEST_SPEC.loader is None:
    raise RuntimeError("cannot load the Switchyard runtime content verifier")
_DIGEST_MODULE = importlib.util.module_from_spec(_DIGEST_SPEC)
_DIGEST_SPEC.loader.exec_module(_DIGEST_MODULE)
verify_marker = _DIGEST_MODULE.verify_marker


SCHEMA_VERSION = 1
MAX_MANIFEST_BYTES = 64 * 1024
MAX_PATH_BYTES = 16 * 1024
MAX_STRING_BYTES = 16 * 1024
MAX_ARGUMENTS = 64
MAX_ENVIRONMENT = 32
MAX_TOOL_OUTPUT = 64 * 1024 * 1024
EXPECTED_ENTITLEMENTS = {
    "com.apple.security.cs.allow-dyld-environment-variables": True,
    "com.apple.security.cs.allow-jit": True,
    "com.apple.security.cs.allow-unsigned-executable-memory": True,
    "com.apple.security.custom-x18-abi-toggle": True,
}
ALLOWED_ENVIRONMENT = {
    "DXMT_LOG_LEVEL",
    "DXMT_LOG_PATH",
    "SteamAppId",
    "SteamGameId",
    "WINEDEBUG",
    "WINE_XTAJIT64_DIAGNOSTICS",
}
IDENTIFIER_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
BUNDLE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9.-]{2,127}\Z")
DISPLAY_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}\Z")
HEX_RE = re.compile(r"[0-9a-f]{64}\Z")


class ExperimentError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb", buffering=0) as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def require_exact_keys(value, expected, description):
    if type(value) is not dict or set(value) != set(expected):
        raise ExperimentError(f"{description} must contain exactly {sorted(expected)!r}")


def require_string(value, description, pattern=None):
    if type(value) is not str or not value or "\x00" in value:
        raise ExperimentError(f"{description} must be a non-empty string")
    if len(value.encode("utf-8")) > MAX_STRING_BYTES:
        raise ExperimentError(f"{description} exceeds {MAX_STRING_BYTES} bytes")
    if pattern is not None and pattern.fullmatch(value) is None:
        raise ExperimentError(f"{description} has an invalid format")
    return value


def require_digest(value, description):
    return require_string(value, description, HEX_RE)


def canonical_existing_path(value, description, directory=False, regular=False):
    path = require_string(value, description)
    if len(os.fsencode(path)) > MAX_PATH_BYTES:
        raise ExperimentError(f"{description} exceeds {MAX_PATH_BYTES} bytes")
    if not os.path.isabs(path) or os.path.normpath(path) != path or os.path.realpath(path) != path:
        raise ExperimentError(f"{description} is not a canonical absolute path")
    try:
        info = os.lstat(path)
    except OSError as error:
        raise ExperimentError(f"cannot inspect {description}: {error}") from error
    if directory and not stat.S_ISDIR(info.st_mode):
        raise ExperimentError(f"{description} is not a real directory")
    if regular and not stat.S_ISREG(info.st_mode):
        raise ExperimentError(f"{description} is not a regular file")
    return path


def canonical_new_directory(value, description):
    path = require_string(value, description)
    if len(os.fsencode(path)) > MAX_PATH_BYTES:
        raise ExperimentError(f"{description} exceeds {MAX_PATH_BYTES} bytes")
    if not os.path.isabs(path) or os.path.normpath(path) != path or path == os.path.sep:
        raise ExperimentError(f"{description} is not a bounded canonical absolute path")
    if os.path.lexists(path):
        canonical_existing_path(path, description, directory=True)
        info = os.lstat(path)
        if info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise ExperimentError(f"{description} is not private and user-owned")
        return path
    parent = canonical_existing_path(os.path.dirname(path), description + " parent", directory=True)
    if os.path.realpath(os.path.join(parent, os.path.basename(path))) != path:
        raise ExperimentError(f"{description} has a non-canonical destination")
    parent_info = os.stat(parent)
    if parent_info.st_uid != os.geteuid() or parent_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise ExperimentError(f"{description} parent is not private and user-owned")
    return path


def read_regular_file(path, maximum, description):
    path = os.path.abspath(path)
    if os.path.realpath(path) != path:
        raise ExperimentError(f"{description} path is not canonical")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ExperimentError(f"cannot open {description}: {error}") from error
    try:
        opened = os.fstat(descriptor)
        entry = os.lstat(path)
        identity = lambda value: (
            value.st_dev,
            value.st_ino,
            value.st_mode,
            value.st_size,
            value.st_mtime_ns,
            value.st_ctime_ns,
        )
        if identity(opened) != identity(entry) or not stat.S_ISREG(opened.st_mode):
            raise ExperimentError(f"{description} changed while opening")
        if opened.st_uid != os.geteuid() or opened.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise ExperimentError(f"{description} is not user-owned and protected from shared writes")
        if opened.st_size <= 0 or opened.st_size > maximum:
            raise ExperimentError(f"{description} has an invalid size")
        chunks = []
        remaining = maximum + 1
        while remaining:
            chunk = os.read(descriptor, min(16384, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        if len(data) != opened.st_size:
            raise ExperimentError(f"{description} changed while reading")
        if identity(os.fstat(descriptor)) != identity(opened):
            raise ExperimentError(f"{description} changed after reading")
        return path, data
    finally:
        os.close(descriptor)


def load_json_document(path, description):
    path, data = read_regular_file(path, MAX_MANIFEST_BYTES, description)
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ExperimentError(f"{description} is not valid UTF-8 JSON: {error}") from error
    return path, data, value


def parse_entitlements(data):
    start = data.find(b"<?xml")
    end = data.rfind(b"</plist>")
    if start == -1 or end == -1:
        raise ExperimentError("codesign did not return an entitlement plist")
    try:
        value = plistlib.loads(data[start : end + len(b"</plist>")])
    except plistlib.InvalidFileException as error:
        raise ExperimentError(f"loader entitlements are invalid: {error}") from error
    if type(value) is not dict or value != EXPECTED_ENTITLEMENTS:
        raise ExperimentError("loader entitlements do not match the exact native ARM64 allowlist")
    return value


def run_tool(arguments, description, timeout=30):
    environment = {
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "TMPDIR": "/tmp",
    }
    try:
        result = subprocess.run(
            arguments,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ExperimentError(f"cannot run {description}: {error}") from error
    if len(result.stdout) > MAX_TOOL_OUTPUT:
        raise ExperimentError(f"{description} output exceeds {MAX_TOOL_OUTPUT} bytes")
    if result.returncode:
        detail = result.stdout.decode("utf-8", "replace").strip()
        raise ExperimentError(f"{description} failed ({result.returncode}): {detail}")
    return result.stdout


def validate_platform_loader(loader):
    architecture = run_tool(["/usr/bin/lipo", "-archs", loader], "inspect loader architecture")
    if architecture.decode("ascii", "replace").split() != ["arm64"]:
        raise ExperimentError("runtime loader is not an arm64-only Mach-O")
    run_tool(["/usr/bin/codesign", "--verify", "--strict", "--verbose=4", loader],
             "verify loader signature")
    entitlements = run_tool(
        ["/usr/bin/codesign", "-d", "--entitlements", ":-", loader],
        "inspect loader entitlements",
    )
    parse_entitlements(entitlements)


def validate_runtime(runtime, verify_content, platform_checks):
    require_exact_keys(
        runtime,
        {"contentSha256", "loaderSha256", "providerContractVersion", "root", "sourceRevision"},
        "runtime",
    )
    root = canonical_existing_path(runtime["root"], "runtime root", directory=True)
    source_revision = require_string(runtime["sourceRevision"], "runtime source revision")
    if re.fullmatch(r"[0-9a-f]{40}", source_revision) is None:
        raise ExperimentError("runtime source revision must be a full lowercase Git object ID")
    content_sha = require_digest(runtime["contentSha256"], "runtime content digest")
    loader_sha = require_digest(runtime["loaderSha256"], "runtime loader digest")
    provider_contract = runtime["providerContractVersion"]
    if type(provider_contract) is not int or provider_contract <= 0:
        raise ExperimentError("runtime provider contract version must be a positive integer")

    manifest_path = os.path.join(root, "switchyard-runtime.json")
    _manifest_path, _manifest_data, packaged = load_json_document(manifest_path, "runtime manifest")
    if packaged.get("sourceRevision") != source_revision or packaged.get("wineRevision") != source_revision:
        raise ExperimentError("runtime manifest source revision does not match the experiment")
    if packaged.get("sourceDirty") is not False:
        raise ExperimentError("runtime manifest is not from a clean source tree")
    if packaged.get("installPrefix") != root:
        raise ExperimentError("runtime manifest install prefix does not match the selected root")
    host = packaged.get("host")
    if type(host) is not dict or host.get("architecture") != "arm64" or host.get("requiresRosetta") is not False:
        raise ExperimentError("runtime host contract is not native arm64 without Rosetta")
    provider = packaged.get("cpuProvider")
    if type(provider) is not dict or provider.get("buildContractVersion") != provider_contract:
        raise ExperimentError("runtime CPU-provider contract does not match the experiment")

    loader = canonical_existing_path(
        os.path.join(root, "lib/wine/aarch64-unix/wine"), "runtime loader", regular=True
    )
    if sha256_file(loader) != loader_sha:
        raise ExperimentError("runtime loader digest does not match the experiment")
    integrity = packaged.get("integrity")
    if type(integrity) is not dict or integrity.get("wineUnixSha256") != loader_sha:
        raise ExperimentError("runtime manifest loader digest does not match the experiment")
    if verify_content and not verify_marker(root, content_sha):
        raise ExperimentError("runtime content tree failed its exact digest verification")
    if platform_checks:
        if sys.platform != "darwin" or os.uname().machine != "arm64":
            raise ExperimentError("platform attestation requires native macOS arm64")
        validate_platform_loader(loader)

    required_libraries = [
        os.path.join(root, "lib/wine/aarch64-unix/ntdll.so"),
        os.path.join(root, "lib/wine/aarch64-unix/xtajit64.so"),
        os.path.join(root, "lib/switchyard-unicorn/lib/libunicorn.2.dylib"),
    ]
    if packaged.get("graphicsBackend") == "dxmt-metal":
        required_libraries.append(os.path.join(root, "lib/wine/aarch64-unix/winemetal.so"))
    for library in required_libraries:
        canonical_existing_path(library, "required runtime library", regular=True)
    wrapper = canonical_existing_path(
        os.path.join(root, "bin/wine"), "runtime wrapper", regular=True
    )
    return {
        "root": root,
        "wrapper": wrapper,
        "loader": loader,
        "requiredLibraries": required_libraries,
        "sourceRevision": source_revision,
        "contentSha256": content_sha,
        "loaderSha256": loader_sha,
        "providerContractVersion": provider_contract,
    }


def validate_experiment_document(document, verify_content=True, platform_checks=True):
    require_exact_keys(
        document,
        {
            "app",
            "argv",
            "environment",
            "experimentId",
            "experimentRoot",
            "prefix",
            "runtime",
            "schemaVersion",
            "workingDirectory",
        },
        "experiment manifest",
    )
    if document["schemaVersion"] != SCHEMA_VERSION:
        raise ExperimentError(f"unsupported experiment schema version {document['schemaVersion']!r}")
    experiment_id = require_string(document["experimentId"], "experiment ID", IDENTIFIER_RE)
    experiment_root = canonical_new_directory(document["experimentRoot"], "experiment root")
    runtime = validate_runtime(document["runtime"], verify_content, platform_checks)
    prefix = canonical_existing_path(document["prefix"], "Wine prefix", directory=True)
    working_directory = canonical_existing_path(
        document["workingDirectory"], "guest working directory", directory=True
    )
    try:
        if os.path.commonpath((prefix, working_directory)) != prefix:
            raise ExperimentError("guest working directory is outside the selected prefix")
    except ValueError as error:
        raise ExperimentError("guest working directory and prefix are on incompatible roots") from error

    arguments = document["argv"]
    if type(arguments) is not list or not arguments or len(arguments) > MAX_ARGUMENTS:
        raise ExperimentError(f"argv must contain between 1 and {MAX_ARGUMENTS} strings")
    arguments = [require_string(value, f"argv[{index}]") for index, value in enumerate(arguments)]
    if arguments[0].startswith("./"):
        guest_executable = os.path.join(working_directory, arguments[0][2:])
        canonical_existing_path(guest_executable, "guest executable", regular=True)

    environment = document["environment"]
    if type(environment) is not dict or len(environment) > MAX_ENVIRONMENT:
        raise ExperimentError(f"environment must be an object with at most {MAX_ENVIRONMENT} entries")
    validated_environment = {}
    for name, value in environment.items():
        if type(name) is not str or name not in ALLOWED_ENVIRONMENT:
            raise ExperimentError(f"environment variable {name!r} is not allowlisted")
        validated_environment[name] = require_string(value, f"environment variable {name}")

    app = document["app"]
    require_exact_keys(app, {"bundleIdentifier", "displayName"}, "app")
    display_name = require_string(app["displayName"], "app display name", DISPLAY_RE)
    bundle_identifier = require_string(app["bundleIdentifier"], "app bundle identifier", BUNDLE_RE)
    return {
        "app": {"bundleIdentifier": bundle_identifier, "displayName": display_name},
        "argv": arguments,
        "environment": validated_environment,
        "experimentId": experiment_id,
        "experimentRoot": experiment_root,
        "prefix": prefix,
        "runtime": runtime,
        "workingDirectory": working_directory,
    }


def load_experiment(path, verify_content=True, platform_checks=True):
    manifest_path, manifest_data, document = load_json_document(path, "experiment manifest")
    validated = validate_experiment_document(document, verify_content, platform_checks)
    validated["manifestPath"] = manifest_path
    validated["manifestSha256"] = hashlib.sha256(manifest_data).hexdigest()
    return validated


def c_byte_array(name, value):
    encoded = value.encode("utf-8")
    values = ", ".join(str(byte) for byte in encoded + b"\0")
    return f"static char {name}[] = {{{values}}};\n"


def generate_experiment_header(experiment, evidence_directory):
    fragments = [
        "#ifndef SWDBG_EXPERIMENT_GENERATED_H\n#define SWDBG_EXPERIMENT_GENERATED_H\n\n",
        "struct swdbg_environment_entry { const char *name; const char *value; };\n\n",
        c_byte_array("swdbg_experiment_id", experiment["experimentId"]),
        c_byte_array("swdbg_runtime_executable", experiment["runtime"]["wrapper"]),
        c_byte_array("swdbg_prefix", experiment["prefix"]),
        c_byte_array("swdbg_working_directory", experiment["workingDirectory"]),
        c_byte_array("swdbg_log_path", os.path.join(evidence_directory, "launcher.log")),
        c_byte_array("swdbg_pid_path", os.path.join(evidence_directory, "pid")),
    ]
    argument_names = []
    for index, argument in enumerate(experiment["argv"]):
        name = f"swdbg_argument_{index}"
        fragments.append(c_byte_array(name, argument))
        argument_names.append(name)
    fragments.append(
        "static char *const swdbg_arguments[] = {swdbg_runtime_executable, "
        + ", ".join(argument_names)
        + ", NULL};\n"
    )
    environment_names = []
    for index, (name, value) in enumerate(sorted(experiment["environment"].items())):
        name_symbol = f"swdbg_environment_name_{index}"
        value_symbol = f"swdbg_environment_value_{index}"
        fragments.append(c_byte_array(name_symbol, name))
        fragments.append(c_byte_array(value_symbol, value))
        environment_names.append((name_symbol, value_symbol))
    if environment_names:
        fragments.append("static const struct swdbg_environment_entry swdbg_environment[] = {\n")
        for name_symbol, value_symbol in environment_names:
            fragments.append(f"    {{{name_symbol}, {value_symbol}}},\n")
        fragments.append("};\n")
    else:
        fragments.append("static const struct swdbg_environment_entry swdbg_environment[1] = {{0}};\n")
    fragments.append(f"static const size_t swdbg_environment_count = {len(environment_names)};\n\n#endif\n")
    return "".join(fragments).encode("utf-8")


def create_new_file(path, data, mode=0o600):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open(path, flags, mode)
    try:
        offset = 0
        while offset < len(data):
            try:
                written = os.write(descriptor, data[offset:])
            except InterruptedError:
                continue
            if written <= 0:
                raise ExperimentError(f"short write while creating {path}")
            offset += written
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def prepare_experiment(experiment):
    root = experiment["experimentRoot"]
    if os.path.lexists(root):
        raise ExperimentError("experiment root already exists; experiment IDs are one-shot")
    os.mkdir(root, 0o700)
    try:
        apps_directory = os.path.join(root, "Apps")
        evidence_directory = os.path.join(root, "Evidence")
        build_directory = os.path.join(root, "Build")
        for directory in (apps_directory, evidence_directory, build_directory):
            os.mkdir(directory, 0o700)
        app_path = os.path.join(apps_directory, experiment["app"]["displayName"] + ".app")
        contents = os.path.join(app_path, "Contents")
        macos = os.path.join(contents, "MacOS")
        os.mkdir(app_path, 0o700)
        os.mkdir(contents, 0o700)
        os.mkdir(macos, 0o700)

        header_path = os.path.join(build_directory, "swdbg_experiment.h")
        create_new_file(header_path, generate_experiment_header(experiment, evidence_directory))
        info = {
            "CFBundleDisplayName": experiment["app"]["displayName"],
            "CFBundleExecutable": "SwdbgLauncher",
            "CFBundleIdentifier": experiment["app"]["bundleIdentifier"],
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": experiment["app"]["displayName"],
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": "1.0",
            "CFBundleVersion": "1",
            "LSMinimumSystemVersion": "15.0",
            "NSHighResolutionCapable": True,
        }
        create_new_file(os.path.join(contents, "Info.plist"), plistlib.dumps(info, sort_keys=True))
        launcher = os.path.join(macos, "SwdbgLauncher")
        source = os.path.join(os.path.dirname(os.path.abspath(__file__)), "swdbg_launcher.c")
        run_tool(
            [
                "/usr/bin/xcrun",
                "clang",
                "-arch",
                "arm64",
                "-mmacosx-version-min=15.0",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                build_directory,
                source,
                "-o",
                launcher,
            ],
            "compile experiment launcher",
            timeout=120,
        )
        run_tool(["/usr/bin/codesign", "--force", "--sign", "-", launcher], "sign experiment launcher")
        launcher_sha = sha256_file(launcher)
        run_tool(["/usr/bin/codesign", "--force", "--sign", "-", app_path], "sign experiment app")
        run_tool(["/usr/bin/codesign", "--verify", "--strict", "--verbose=4", app_path],
                 "verify experiment app")
        run_tool(["/usr/bin/codesign", "--verify", "--strict", "--verbose=4", launcher],
                 "verify experiment launcher")
        if sha256_file(launcher) != launcher_sha:
            raise ExperimentError("outer app signing mutated the already signed launcher")

        prepared = {
            "appPath": app_path,
            "createdAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "experimentId": experiment["experimentId"],
            "launcherSha256": launcher_sha,
            "manifestPath": experiment["manifestPath"],
            "manifestSha256": experiment["manifestSha256"],
            "runtime": experiment["runtime"],
            "schemaVersion": SCHEMA_VERSION,
        }
        create_new_file(os.path.join(root, "prepared.json"), json_bytes(prepared))
        shutil.rmtree(build_directory)
        return prepared
    except BaseException:
        # A failed preparation is evidence in itself. Leave the private root in
        # place so a retry cannot silently reuse a partially built experiment.
        raise


def load_prepared(experiment):
    path = os.path.join(experiment["experimentRoot"], "prepared.json")
    _path, _data, prepared = load_json_document(path, "prepared experiment record")
    require_exact_keys(
        prepared,
        {
            "appPath",
            "createdAt",
            "experimentId",
            "launcherSha256",
            "manifestPath",
            "manifestSha256",
            "runtime",
            "schemaVersion",
        },
        "prepared experiment record",
    )
    if prepared["schemaVersion"] != SCHEMA_VERSION or prepared["experimentId"] != experiment["experimentId"]:
        raise ExperimentError("prepared experiment identity does not match the manifest")
    if prepared["manifestPath"] != experiment["manifestPath"] or prepared["manifestSha256"] != experiment["manifestSha256"]:
        raise ExperimentError("prepared experiment manifest identity has changed")
    if prepared["runtime"] != experiment["runtime"]:
        raise ExperimentError("prepared experiment runtime identity has changed")
    app_path = canonical_existing_path(prepared["appPath"], "prepared app", directory=True)
    expected_app = os.path.join(
        experiment["experimentRoot"], "Apps", experiment["app"]["displayName"] + ".app"
    )
    if app_path != expected_app:
        raise ExperimentError("prepared app path is outside its experiment root")
    launcher = canonical_existing_path(
        os.path.join(app_path, "Contents/MacOS/SwdbgLauncher"), "prepared launcher", regular=True
    )
    if sha256_file(launcher) != require_digest(prepared["launcherSha256"], "prepared launcher digest"):
        raise ExperimentError("prepared launcher digest has changed")
    run_tool(["/usr/bin/codesign", "--verify", "--strict", "--verbose=4", app_path],
             "verify prepared app")
    run_tool(["/usr/bin/codesign", "--verify", "--strict", "--verbose=4", launcher],
             "verify prepared launcher")
    return prepared


def active_wine_processes():
    output = run_tool(["/bin/ps", "-axo", "pid=,comm=,command="], "inspect active processes")
    matches = []
    pattern = re.compile(
        r"(?:^|/)(?:wine(?:64)?(?:-preloader)?|wineserver|xdt\.exe|Heartopia)(?:\s|$)",
        re.IGNORECASE,
    )
    for line in output.decode("utf-8", "replace").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        fields = stripped.split(None, 1)
        if len(fields) == 2 and pattern.search(fields[1]):
            matches.append(stripped)
    return matches


def process_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def read_pid(path):
    _path, data = read_regular_file(path, 64, "experiment pid file")
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise ExperimentError("experiment pid file is not ASCII") from error
    if re.fullmatch(r"[1-9][0-9]{0,9}\n", text) is None:
        raise ExperimentError("experiment pid file has invalid content")
    return int(text)


def atomic_evidence(path, data):
    if len(data) > MAX_TOOL_OUTPUT:
        raise ExperimentError(f"evidence exceeds {MAX_TOOL_OUTPUT} bytes: {path}")
    create_new_file(path, data)


def remaining_timeout(deadline, maximum, description):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise ExperimentError(f"{description} timed out")
    return min(maximum, remaining)


def attest_process(experiment, pid, timeout):
    evidence = os.path.join(experiment["experimentRoot"], "Evidence")
    deadline = time.monotonic() + timeout
    last_vmmap = b""
    missing = list(experiment["runtime"]["requiredLibraries"])
    while time.monotonic() < deadline:
        if not process_alive(pid):
            raise ExperimentError(f"experiment process {pid} exited before runtime attestation")
        try:
            last_vmmap = run_tool(
                ["/usr/bin/vmmap", str(pid)],
                "capture process mappings",
                timeout=remaining_timeout(deadline, 30, "runtime mapping attestation"),
            )
        except ExperimentError:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(1, remaining))
            continue
        decoded = last_vmmap.decode("utf-8", "replace")
        missing = [path for path in experiment["runtime"]["requiredLibraries"] if path not in decoded]
        if not missing:
            break
        time.sleep(min(1, remaining_timeout(deadline, 1, "runtime mapping attestation")))
    if missing:
        raise ExperimentError("runtime mapping attestation timed out; missing: " + ", ".join(missing))

    vmmap_path = os.path.join(evidence, "vmmap.txt")
    atomic_evidence(vmmap_path, last_vmmap)
    sample_path = os.path.join(evidence, "sample.txt")
    if os.path.lexists(sample_path):
        raise ExperimentError("sample evidence path already exists")
    run_tool(["/usr/bin/sample", str(pid), "1", "1", "-file", sample_path],
             "sample experiment process",
             timeout=remaining_timeout(deadline, 30, "runtime process attestation"))
    _sample_path, sample_data = read_regular_file(sample_path, MAX_TOOL_OUTPUT, "process sample")
    sample_text = sample_data.decode("utf-8", "replace")
    if re.search(r"Code Type:\s+ARM64(?:\s|$)", sample_text) is None:
        raise ExperimentError("sample does not attest a native ARM64 process")
    if "Translated" in sample_text:
        raise ExperimentError("sample reports a translated process")
    result = {
        "attestedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "experimentId": experiment["experimentId"],
        "mappedLibraries": experiment["runtime"]["requiredLibraries"],
        "pid": pid,
        "runtimeContentSha256": experiment["runtime"]["contentSha256"],
        "runtimeSourceRevision": experiment["runtime"]["sourceRevision"],
        "sampleSha256": hashlib.sha256(sample_data).hexdigest(),
        "schemaVersion": SCHEMA_VERSION,
        "vmmapSha256": hashlib.sha256(last_vmmap).hexdigest(),
    }
    atomic_evidence(os.path.join(evidence, "attestation.json"), json_bytes(result))
    return result


def launch_experiment(experiment, timeout):
    prepared = load_prepared(experiment)
    active = active_wine_processes()
    if active:
        raise ExperimentError("refusing to mix the experiment with active Wine processes:\n" + "\n".join(active))
    evidence = os.path.join(experiment["experimentRoot"], "Evidence")
    pid_path = os.path.join(evidence, "pid")
    if os.path.lexists(pid_path) or os.listdir(evidence):
        raise ExperimentError("experiment evidence directory is not empty; experiment IDs are one-shot")
    run_tool(["/usr/bin/open", "-n", prepared["appPath"]], "launch experiment app")
    attest_deadline = time.monotonic() + timeout
    launcher_deadline = min(attest_deadline, time.monotonic() + 30)
    while time.monotonic() < launcher_deadline and not os.path.exists(pid_path):
        time.sleep(0.1)
    if not os.path.exists(pid_path):
        raise ExperimentError("launcher did not publish its pid within 30 seconds")
    pid = read_pid(pid_path)
    return attest_process(
        experiment,
        pid,
        remaining_timeout(attest_deadline, timeout, "experiment launch attestation"),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("validate", "prepare"):
        command = subparsers.add_parser(name)
        command.add_argument("manifest")
    launch = subparsers.add_parser("launch")
    launch.add_argument("manifest")
    launch.add_argument("--attest-timeout", type=int, default=90)
    attest = subparsers.add_parser("attest")
    attest.add_argument("manifest")
    attest.add_argument("pid", type=int)
    attest.add_argument("--attest-timeout", type=int, default=90)
    arguments = parser.parse_args()

    try:
        if arguments.command == "validate":
            experiment = load_experiment(arguments.manifest)
            result = {
                "experimentId": experiment["experimentId"],
                "manifestSha256": experiment["manifestSha256"],
                "runtime": experiment["runtime"],
                "status": "validated",
            }
        elif arguments.command == "prepare":
            experiment = load_experiment(arguments.manifest)
            result = prepare_experiment(experiment)
        elif arguments.command == "launch":
            if arguments.attest_timeout < 1 or arguments.attest_timeout > 600:
                raise ExperimentError("attestation timeout must be between 1 and 600 seconds")
            experiment = load_experiment(arguments.manifest)
            result = launch_experiment(experiment, arguments.attest_timeout)
        else:
            if arguments.pid <= 1 or arguments.attest_timeout < 1 or arguments.attest_timeout > 600:
                raise ExperimentError("attest requires a valid pid and a timeout between 1 and 600 seconds")
            experiment = load_experiment(arguments.manifest)
            load_prepared(experiment)
            result = attest_process(experiment, arguments.pid, arguments.attest_timeout)
        print(json.dumps(result, indent=2, sort_keys=True))
    except (ExperimentError, OSError) as error:
        print(f"swdbg: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
