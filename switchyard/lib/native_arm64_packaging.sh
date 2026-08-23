#!/usr/bin/env bash

# Native ARM64 runtime production helpers.  Validation policy lives in the
# dedicated native_cpu_provider.sh and dxmt_artifact.sh libraries; this file
# only stages the pinned DXMT input, emits their frozen manifest contracts, and
# composes the validators at the final packaging boundary.

switchyard_native_arm64_require_packaging_contract() {
  [ "${SWITCHYARD_DXMT_SOURCE_REPOSITORY:-}" = \
      "https://github.com/3Shain/dxmt.git" ] &&
    [ "${SWITCHYARD_DXMT_SOURCE_REVISION:-}" = \
      "856d9f35789679ef00c1ba01a6353438df84b66f" ] &&
    [ "${SWITCHYARD_DXMT_ARTIFACT_NAME:-}" = \
      "dxmt-856d9f35789679ef00c1ba01a6353438df84b66f.tar.gz" ] &&
    [ "${SWITCHYARD_DXMT_ARTIFACT_SHA256:-}" = \
      "8840df7038d7cbffed3652712c86ec4d6d495612aa39306e9a184bd213514acf" ] &&
    [ "${SWITCHYARD_DXMT_PACKAGE_WORKFLOW:-}" = ".github/workflows/ci.yml" ] &&
    [ "${SWITCHYARD_DXMT_PACKAGE_WORKFLOW_SHA256:-}" = \
      "fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851" ] &&
    [ "${SWITCHYARD_DXMT_PACKAGE_BUILD:-}" = \
      "gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross" ] &&
    [ "${SWITCHYARD_DXMT_LICENSE_SHA256:-}" = \
      "b87c35aef7b2cf14de854118ca55ce5c4b284c85b5f002421fb8d46d868c2d17" ] &&
    [ "${SWITCHYARD_DXMT_COPYING_SHA256:-}" = \
      "e237fa56668030e928551ddd60f05df5fe957f75eab874bbd017e085ed722e7c" ] &&
    [ "${SWITCHYARD_DXMT_CORRESPONDING_SOURCE_SHA256:-}" = \
      "40bbbbecb9c48cfd67f5862b0b93878ae80dc3de083790d3ec9dadd98618c89a" ] &&
    [ "${SWITCHYARD_DXMT_WINEMETAL_ORIGINAL_SHA256:-}" = \
      "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0" ] &&
    [ "${SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256:-}" = \
      "7938d56916074f61dce96b43e3f63b47fe52565c6a4c6096c876847f1920d9d3" ] || {
    echo "Native ARM64 DXMT packaging constants do not match the closed policy." >&2
    return 1
  }
}

switchyard_stage_native_arm64_dxmt_artifact() {
  local archive source_root wine_source_root runtime_root schema_sha256

  [ "$#" -eq 4 ] || {
    echo "usage: switchyard_stage_native_arm64_dxmt_artifact ARCHIVE DXMT_SOURCE WINE_SOURCE RUNTIME" >&2
    return 2
  }
  archive="$1"
  source_root="$2"
  wine_source_root="$3"
  runtime_root="$4"
  switchyard_native_arm64_require_packaging_contract || return 1
  declare -F switchyard_validate_dxmt_wow64_companion_source >/dev/null || {
    echo "DXMT WoW64 companion source validator is unavailable." >&2
    return 1
  }
  schema_sha256="$(
    switchyard_validate_dxmt_wow64_companion_source "$wine_source_root"
  )" || return 1
  [ "$schema_sha256" = "$SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256" ] || return 1

  /usr/bin/python3 -I - "$archive" "$source_root" "$wine_source_root" "$runtime_root" \
    "$SWITCHYARD_DXMT_SOURCE_REPOSITORY" \
    "$SWITCHYARD_DXMT_SOURCE_REVISION" \
    "$SWITCHYARD_DXMT_ARTIFACT_NAME" \
    "$SWITCHYARD_DXMT_ARTIFACT_SHA256" \
    "$SWITCHYARD_DXMT_PACKAGE_WORKFLOW" \
    "$SWITCHYARD_DXMT_PACKAGE_WORKFLOW_SHA256" \
    "$SWITCHYARD_DXMT_PACKAGE_BUILD" \
    "$SWITCHYARD_DXMT_LICENSE_SHA256" \
    "$SWITCHYARD_DXMT_COPYING_SHA256" \
    "$SWITCHYARD_DXMT_CORRESPONDING_SOURCE_SHA256" \
    "$SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256" <<'PY'
import hashlib
import os
import stat
import subprocess
import sys
import tarfile

(
    archive_name,
    source_name,
    wine_source_name,
    runtime_name,
    repository,
    revision,
    artifact_name,
    artifact_sha256,
    workflow,
    workflow_sha256,
    package_build,
    license_sha256,
    copying_sha256,
    corresponding_source_sha256,
    companion_schema_sha256,
) = sys.argv[1:]

COMPANION_PATH = "lib/wine/aarch64-unix/winemetal-wow64.so"
COMPANION_SCHEMA_SOURCE = "dlls/winemetal-wow64/abi-schema-v4.txt"
COMPANION_SCHEMA_PATH = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v4.txt"

MAX_ARCHIVE = 64 * 1024 * 1024
MAX_MEMBER = 40 * 1024 * 1024
MAX_EXPANDED = 128 * 1024 * 1024

SOURCE_FILES = {
    "aarch64-unix/winemetal.so": "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0",
    "aarch64-windows/d3d10core.dll": "0ca52517ce266d63b85310a8aae940e92b0a05392d1d03698dbc4156ce28a959",
    "aarch64-windows/d3d11.dll": "bb74a3835c731d7dfe19e9d928cf20a4eef6d37c88edddfcf112557408a01fc6",
    "aarch64-windows/dxgi.dll": "9c374cc1896dca4129fd5c810c09e8dce9df6b04398ddb1207da6bce01e15e3c",
    "aarch64-windows/nvapi64.dll": "f4e1cf79244d378c660b5d9b6c98923e29f2bd30e9073dadf62ac1879ffd9f02",
    "aarch64-windows/nvngx.dll": "b8ddc2d81dcf4306b58398b486299f31067617e4f5e66cd64c8e5eacde2a0c0c",
    "aarch64-windows/winemetal.dll": "64007d8901b691bd91aac8218bddb12e2cce272fbdaab8a7bdc3f0ca6fe3eb99",
    "i386-windows/d3d10core.dll": "77a7c58a8ee649a2959017a91211f5003bf988010a090447b78fa00ca8a7544b",
    "i386-windows/d3d11.dll": "3f42b073b2954d7b27fa00380d4e268b6f8f2216d701b2c57176c9f3c83b49fb",
    "i386-windows/dxgi.dll": "c6ba805aafd21668d487252747fadba3ee4525a55c7bfdf6f65ec26e140a39ff",
    "i386-windows/winemetal.dll": "99db6924a2726d534562f9168692c5c1b4d4651d40a55133a8887e7621c9bc2f",
    "x86_64-windows/d3d10core.dll": "4910ce0b1960a627c61114b019869057be8e1bf2edddd2ecb348c434bb98e5e0",
    "x86_64-windows/d3d11.dll": "26b88098961e936b3bfe0ad984d3ad2a4568f10b04a4e6f7fa54711a9c17b583",
    "x86_64-windows/dxgi.dll": "19ffb16b5dd22c944b284d9ea6d7b301e2ad96ef68f65ebdb642db49c55a9491",
    "x86_64-windows/nvapi64.dll": "6e1bb14e6fb6c6f64d30e67aa351550d85d7d32d43ae429831f9ca49550ed323",
    "x86_64-windows/nvngx.dll": "97e48d69a527e82b4269f50b1e1d5041e594e5a1dac5b51fce43008d372733d6",
    "x86_64-windows/winemetal.dll": "34c66a7e56d1c0315f160775be009cf92efc56ec9396c2d61b6f03c307abefed",
    # This member is verified as part of the exact upstream archive but is
    # deliberately not copied into a native runtime.
    "x86_64-unix/winemetal.so": "9a73df5fc25730a2b19286c6d34f365fade266cb833d0ca69a905d4696ed0b05",
}
STAGED = [
    item for item in SOURCE_FILES if item != "x86_64-unix/winemetal.so"
]
DIRECTORIES = {
    "aarch64-unix",
    "aarch64-windows",
    "i386-windows",
    "x86_64-unix",
    "x86_64-windows",
}


def fail(message):
    raise SystemExit("native ARM64 DXMT staging failed: " + message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_absolute(path, description, directory=False):
    if (
        not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == "/"
        or "\0" in path
        or "\n" in path
        or "\r" in path
        or os.path.realpath(path) != path
    ):
        fail(description + " is not a bounded canonical absolute path")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    if directory:
        flags |= getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open("/", flags | getattr(os, "O_DIRECTORY", 0))
    try:
        components = path.split("/")[1:]
        for index, component in enumerate(components):
            child_flags = flags
            if index != len(components) - 1 or directory:
                child_flags |= getattr(os, "O_DIRECTORY", 0)
            child = os.open(component, child_flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        metadata = os.fstat(descriptor)
        if directory and not stat.S_ISDIR(metadata.st_mode):
            fail(description + " is not a directory")
        if not directory and not stat.S_ISREG(metadata.st_mode):
            fail(description + " is not a regular file")
        return descriptor, metadata
    except Exception:
        os.close(descriptor)
        raise


class RuntimeTree:
    def __init__(self, path):
        self.path = path
        self.root_fd, metadata = validate_absolute(path, "runtime root", directory=True)
        if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            self.close()
            fail("runtime root is group/world writable")

    def close(self):
        if self.root_fd is not None:
            os.close(self.root_fd)
            self.root_fd = None

    def open_directory(self, relative, create=False):
        descriptor = os.dup(self.root_fd)
        try:
            for component in relative.split("/"):
                if component in ("", ".", ".."):
                    fail("invalid runtime-relative directory: " + relative)
                flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
                try:
                    child = os.open(component, flags, dir_fd=descriptor)
                except FileNotFoundError:
                    if not create:
                        raise
                    os.mkdir(component, 0o755, dir_fd=descriptor)
                    child = os.open(component, flags, dir_fd=descriptor)
                metadata = os.fstat(child)
                if not stat.S_ISDIR(metadata.st_mode) or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                    os.close(child)
                    fail("unsafe runtime directory: " + relative)
                os.close(descriptor)
                descriptor = child
            return descriptor
        except Exception:
            os.close(descriptor)
            raise

    def exists(self, relative):
        parent, name = relative.rsplit("/", 1)
        descriptor = self.open_directory(parent)
        try:
            try:
                os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            except FileNotFoundError:
                return False
            return True
        finally:
            os.close(descriptor)

    def read_file(self, relative, maximum):
        parent, name = relative.rsplit("/", 1)
        descriptor = self.open_directory(parent)
        source = -1
        try:
            metadata = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            source = os.open(
                name,
                os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=descriptor,
            )
            opened = os.fstat(source)
            identity = lambda value: (
                value.st_dev, value.st_ino, value.st_mode, value.st_size,
                value.st_mtime_ns, value.st_ctime_ns,
            )
            if (
                identity(metadata) != identity(opened)
                or not stat.S_ISREG(opened.st_mode)
                or opened.st_size <= 0
                or opened.st_size > maximum
                or opened.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or not opened.st_mode & 0o111
            ):
                fail("runtime input is unsafe: " + relative)
            data = bytearray()
            while block := os.read(source, min(1024 * 1024, opened.st_size + 1 - len(data))):
                data.extend(block)
            after = os.fstat(source)
            if len(data) != opened.st_size or identity(opened) != identity(after):
                fail("runtime input changed while reading: " + relative)
            return bytes(data)
        finally:
            if source >= 0:
                os.close(source)
            os.close(descriptor)

    def write_file(self, relative, data, mode, replace):
        parent, name = relative.rsplit("/", 1)
        descriptor = self.open_directory(parent, create=True)
        temporary = f".switchyard-dxmt.{os.getpid()}.{name}"
        temporary_created = False
        try:
            try:
                metadata = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            except FileNotFoundError:
                metadata = None
            if metadata is not None:
                if not replace or not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
                    fail("refusing to replace unsafe runtime entry: " + relative)
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
            flags |= getattr(os, "O_NOFOLLOW", 0)
            output = os.open(temporary, flags, mode, dir_fd=descriptor)
            temporary_created = True
            try:
                view = memoryview(data)
                while view:
                    written = os.write(output, view)
                    if written <= 0:
                        fail("short write while staging " + relative)
                    view = view[written:]
                os.fchmod(output, mode)
                os.fsync(output)
            finally:
                os.close(output)
            os.replace(temporary, name, src_dir_fd=descriptor, dst_dir_fd=descriptor)
            temporary_created = False
        finally:
            if temporary_created:
                try:
                    os.unlink(temporary, dir_fd=descriptor)
                except FileNotFoundError:
                    pass
            os.close(descriptor)


def git(*arguments):
    environment = {
        "PATH": "/usr/bin:/bin",
        "LC_ALL": "C",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_OPTIONAL_LOCKS": "0",
    }
    result = subprocess.run(
        ["/usr/bin/git", "-C", source_name, *arguments],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        check=False,
    )
    if result.returncode:
        fail("cannot inspect pinned source with git: " + result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


source_fd, _source_metadata = validate_absolute(source_name, "source root", directory=True)
os.close(source_fd)
wine_source_fd, _wine_source_metadata = validate_absolute(
    wine_source_name, "Wine source root", directory=True
)
os.close(wine_source_fd)
schema_name = os.path.join(wine_source_name, COMPANION_SCHEMA_SOURCE)
schema_fd, schema_metadata = validate_absolute(schema_name, "companion ABI schema")
if schema_metadata.st_size <= 0 or schema_metadata.st_size > 1024 * 1024:
    os.close(schema_fd)
    fail("companion ABI schema is outside its size bound")
with os.fdopen(schema_fd, "rb") as schema_stream:
    schema_data = schema_stream.read(schema_metadata.st_size + 1)
if (
    len(schema_data) != schema_metadata.st_size
    or sha256(schema_data) != companion_schema_sha256
    or not schema_data.endswith(b"\n")
    or b"\r" in schema_data
    or b"\0" in schema_data
    or any(byte > 0x7f for byte in schema_data)
):
    fail("companion ABI schema is not the exact pinned ASCII/LF file")
if git("rev-parse", "--verify", "HEAD^{commit}").decode("ascii").strip() != revision:
    fail("source checkout is not at the pinned revision")
remotes = git("remote", "get-url", "--all", "origin").decode("utf-8").splitlines()
if remotes != [repository]:
    fail("source origin is not the exact pinned repository")


def source_blob(relative, expected, maximum=4 * 1024 * 1024):
    data = git("cat-file", "blob", revision + ":" + relative)
    if not data or len(data) > maximum or sha256(data) != expected:
        fail("pinned source blob is missing or has the wrong digest: " + relative)
    return data


workflow_data = source_blob(workflow, workflow_sha256)
license_data = source_blob("LICENSE", license_sha256)
copying_data = source_blob("COPYING.LIB", copying_sha256)
if not workflow_data or package_build != (
    "gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+"
    "clang-release-arm64ec-windows-cross"
):
    fail("DXMT package build policy is not closed")

archive_fd, archive_metadata = validate_absolute(archive_name, "artifact archive")
if archive_metadata.st_size <= 0 or archive_metadata.st_size > MAX_ARCHIVE:
    os.close(archive_fd)
    fail("artifact archive is outside its size bound")
with os.fdopen(archive_fd, "rb") as archive_stream:
    digest = hashlib.sha256()
    for block in iter(lambda: archive_stream.read(1024 * 1024), b""):
        digest.update(block)
    if digest.hexdigest() != artifact_sha256:
        fail("artifact archive digest does not match the pinned identity")
    archive_stream.seek(0)
    with tarfile.open(fileobj=archive_stream, mode="r:gz") as package:
        members = package.getmembers()
        names = [member.name.rstrip("/") for member in members]
        expected = {revision}
        expected.update(revision + "/" + item for item in DIRECTORIES)
        expected.update(revision + "/" + item for item in SOURCE_FILES)
        if len(names) != len(set(names)) or set(names) != expected or len(names) != 24:
            fail("artifact archive is not the exact 24-member closure")
        data_by_name = {}
        expanded = 0
        for member, normalized in zip(members, names):
            relative = normalized.removeprefix(revision + "/")
            if normalized == revision or relative in DIRECTORIES:
                if not member.isdir() or member.issym() or member.islnk():
                    fail("artifact directory has an unsafe type: " + normalized)
                continue
            if (
                not member.isfile()
                or member.issym()
                or member.islnk()
                or member.size <= 0
                or member.size > MAX_MEMBER
            ):
                fail("artifact module has an unsafe type or size: " + normalized)
            expanded += member.size
            if expanded > MAX_EXPANDED:
                fail("artifact archive exceeds its expansion bound")
            extracted = package.extractfile(member)
            if extracted is None:
                fail("cannot read artifact module: " + normalized)
            data = extracted.read(member.size + 1)
            if len(data) != member.size or sha256(data) != SOURCE_FILES[relative]:
                fail("artifact module digest differs from the pinned source: " + relative)
            data_by_name[relative] = data
if set(data_by_name) != set(SOURCE_FILES):
    fail("artifact module payload is incomplete")

tree = RuntimeTree(runtime_name)
try:
    wine_directory = tree.open_directory("lib/wine")
    os.close(wine_directory)
    if tree.exists("lib/switchyard-dxmt"):
        fail("runtime already contains a DXMT documentation root")
    companion_data = tree.read_file(COMPANION_PATH, 64 * 1024 * 1024)
    for relative in STAGED:
        destination = "lib/wine/" + relative
        mode = 0o755 if relative.endswith(".so") else 0o644
        tree.write_file(destination, data_by_name[relative], mode, replace=True)

    documentation = "lib/switchyard-dxmt/share/doc/switchyard-dxmt"
    corresponding_source = f"""DXMT corresponding source and artifact provenance

Repository: {repository}
Revision: {revision}
Source URL: {repository.removesuffix('.git')}/tree/{revision}
Artifact: {artifact_name}
Artifact SHA-256: {artifact_sha256}
Package workflow: {workflow}
Package workflow SHA-256: {workflow_sha256}
Package build: {package_build}

The PE modules are byte-for-byte files from the pinned artifact. The host
Mach-O module is derived from that artifact and may differ only through the
runtime's validated code-signing step; its final digest is recorded in both
files.sha256 and switchyard-runtime.json. DXMT is licensed under
LGPL-2.1-or-later; LICENSE and COPYING.LIB are retained in this directory.
""".encode("utf-8")
    if sha256(corresponding_source) != corresponding_source_sha256:
        fail("corresponding-source document differs from the frozen contract")
    files_manifest = b"".join(
        (sha256(data_by_name[item]) + "  lib/wine/" + item + "\n").encode("ascii")
        for item in STAGED
    )
    files_manifest += (
        sha256(companion_data) + "  " + COMPANION_PATH + "\n"
        + companion_schema_sha256 + "  " + COMPANION_SCHEMA_PATH + "\n"
    ).encode("ascii")
    tree.write_file(documentation + "/files.sha256", files_manifest, 0o644, replace=False)
    tree.write_file(documentation + "/LICENSE", license_data, 0o644, replace=False)
    tree.write_file(documentation + "/COPYING.LIB", copying_data, 0o644, replace=False)
    tree.write_file(
        documentation + "/CORRESPONDING-SOURCE.txt",
        corresponding_source,
        0o644,
        replace=False,
    )
    tree.write_file(COMPANION_SCHEMA_PATH, schema_data, 0o644, replace=False)
finally:
    tree.close()
PY
}

switchyard_finalize_native_arm64_runtime_manifest() {
  local runtime_root manifest

  [ "$#" -eq 2 ] || {
    echo "usage: switchyard_finalize_native_arm64_runtime_manifest RUNTIME MANIFEST" >&2
    return 2
  }
  runtime_root="$1"
  manifest="$2"
  switchyard_native_arm64_require_packaging_contract || return 1

  /usr/bin/python3 -I - "$runtime_root" "$manifest" \
    "$SWITCHYARD_DXMT_SOURCE_REPOSITORY" \
    "$SWITCHYARD_DXMT_SOURCE_REVISION" \
    "$SWITCHYARD_DXMT_ARTIFACT_NAME" \
    "$SWITCHYARD_DXMT_ARTIFACT_SHA256" \
    "$SWITCHYARD_DXMT_PACKAGE_WORKFLOW" \
    "$SWITCHYARD_DXMT_PACKAGE_WORKFLOW_SHA256" \
    "$SWITCHYARD_DXMT_PACKAGE_BUILD" \
    "$SWITCHYARD_DXMT_WINEMETAL_ORIGINAL_SHA256" \
    "$SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256" <<'PY'
import hashlib
import json
import os
import re
import stat
import sys

(
    root_name,
    manifest_name,
    repository,
    revision,
    artifact_name,
    artifact_sha256,
    workflow,
    workflow_sha256,
    package_build,
    original_winemetal_sha256,
    companion_schema_sha256,
) = sys.argv[1:]

COMPANION_PATH = "lib/wine/aarch64-unix/winemetal-wow64.so"
COMPANION_SCHEMA_PATH = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v4.txt"

MODULE_SOURCES = [
    ("lib/wine/aarch64-unix/winemetal.so", "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0", "mach-o-dylib", "arm64"),
    ("lib/wine/aarch64-windows/d3d10core.dll", "0ca52517ce266d63b85310a8aae940e92b0a05392d1d03698dbc4156ce28a959", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/d3d11.dll", "bb74a3835c731d7dfe19e9d928cf20a4eef6d37c88edddfcf112557408a01fc6", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/dxgi.dll", "9c374cc1896dca4129fd5c810c09e8dce9df6b04398ddb1207da6bce01e15e3c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvapi64.dll", "f4e1cf79244d378c660b5d9b6c98923e29f2bd30e9073dadf62ac1879ffd9f02", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvngx.dll", "b8ddc2d81dcf4306b58398b486299f31067617e4f5e66cd64c8e5eacde2a0c0c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/winemetal.dll", "64007d8901b691bd91aac8218bddb12e2cce272fbdaab8a7bdc3f0ca6fe3eb99", "pe-dll", "arm64ec"),
    ("lib/wine/i386-windows/d3d10core.dll", "77a7c58a8ee649a2959017a91211f5003bf988010a090447b78fa00ca8a7544b", "pe-dll", "i386"),
    ("lib/wine/i386-windows/d3d11.dll", "3f42b073b2954d7b27fa00380d4e268b6f8f2216d701b2c57176c9f3c83b49fb", "pe-dll", "i386"),
    ("lib/wine/i386-windows/dxgi.dll", "c6ba805aafd21668d487252747fadba3ee4525a55c7bfdf6f65ec26e140a39ff", "pe-dll", "i386"),
    ("lib/wine/i386-windows/winemetal.dll", "99db6924a2726d534562f9168692c5c1b4d4651d40a55133a8887e7621c9bc2f", "pe-dll", "i386"),
    ("lib/wine/x86_64-windows/d3d10core.dll", "4910ce0b1960a627c61114b019869057be8e1bf2edddd2ecb348c434bb98e5e0", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/d3d11.dll", "26b88098961e936b3bfe0ad984d3ad2a4568f10b04a4e6f7fa54711a9c17b583", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/dxgi.dll", "19ffb16b5dd22c944b284d9ea6d7b301e2ad96ef68f65ebdb642db49c55a9491", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/nvapi64.dll", "6e1bb14e6fb6c6f64d30e67aa351550d85d7d32d43ae429831f9ca49550ed323", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/nvngx.dll", "97e48d69a527e82b4269f50b1e1d5041e594e5a1dac5b51fce43008d372733d6", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/winemetal.dll", "34c66a7e56d1c0315f160775be009cf92efc56ec9396c2d61b6f03c307abefed", "pe-dll", "x86_64"),
]
LOAD_COMMANDS = [
    {"command": "LC_LOAD_DYLIB", "path": "@rpath/winemac.so"},
    {"command": "LC_LOAD_DYLIB", "path": "@rpath/ntdll.so"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"},
    {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/MetalFX.framework/Versions/A/MetalFX"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libsqlite3.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libSystem.B.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libz.1.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libncurses.5.4.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libxml2.2.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libc++.1.dylib"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/usr/lib/libobjc.A.dylib"},
]
DOCUMENT_PATHS = [
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/files.sha256",
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/LICENSE",
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/COPYING.LIB",
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/CORRESPONDING-SOURCE.txt",
    COMPANION_SCHEMA_PATH,
]
AUDITED = [
    ("crypt32", "lib/wine/aarch64-unix/crypt32.so"),
    ("dwrite", "lib/wine/aarch64-unix/dwrite.so"),
    ("secur32", "lib/wine/aarch64-unix/secur32.so"),
    ("winemac", "lib/wine/aarch64-unix/winemac.so"),
    ("ws2_32", "lib/wine/aarch64-unix/ws2_32.so"),
]


def fail(message):
    raise SystemExit("native ARM64 manifest production failed: " + message)


if original_winemetal_sha256 != MODULE_SOURCES[0][1]:
    fail("DXMT original winemetal source identity is not exact")


def no_duplicates(pairs):
    result = {}
    for key, item in pairs:
        if key in result:
            fail("input manifest contains duplicate object key: " + key)
        result[key] = item
    return result


class RuntimeTree:
    def __init__(self, root):
        if (
            not os.path.isabs(root)
            or os.path.normpath(root) != root
            or root == "/"
            or os.path.realpath(root) != root
            or any(item in root for item in ("\0", "\n", "\r"))
        ):
            fail("runtime root is not a bounded canonical absolute path")
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open("/", flags)
        try:
            for component in root.split("/")[1:]:
                child = os.open(component, flags, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
            metadata = os.fstat(descriptor)
            if not stat.S_ISDIR(metadata.st_mode) or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                fail("runtime root has an unsafe type or mode")
        except Exception:
            os.close(descriptor)
            raise
        self.root_fd = descriptor

    def close(self):
        if self.root_fd is not None:
            os.close(self.root_fd)
            self.root_fd = None

    def open_file(self, relative, maximum=512 * 1024 * 1024):
        parts = relative.split("/")
        if any(part in ("", ".", "..") for part in parts):
            fail("invalid runtime-relative path: " + relative)
        descriptor = os.dup(self.root_fd)
        try:
            for part in parts[:-1]:
                flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
                child = os.open(part, flags, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
            flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
            child = os.open(parts[-1], flags, dir_fd=descriptor)
            metadata = os.fstat(child)
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_size <= 0
                or metadata.st_size > maximum
                or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            ):
                os.close(child)
                fail("runtime file has an unsafe type, size, or mode: " + relative)
            return child, metadata
        finally:
            os.close(descriptor)

    def read(self, relative, maximum=512 * 1024 * 1024):
        descriptor, before = self.open_file(relative, maximum)
        try:
            data = bytearray()
            while block := os.read(descriptor, 1024 * 1024):
                data.extend(block)
            after = os.fstat(descriptor)
            identity = lambda item: (
                item.st_dev,
                item.st_ino,
                stat.S_IFMT(item.st_mode),
                item.st_size,
                item.st_mtime_ns,
                item.st_ctime_ns,
            )
            if identity(before) != identity(after):
                fail("runtime file changed while hashing: " + relative)
            return bytes(data), before
        finally:
            os.close(descriptor)


tree = RuntimeTree(root_name)
try:
    expected_manifest = os.path.join(root_name, "switchyard-runtime.json")
    if manifest_name != expected_manifest:
        fail("manifest is not the runtime-root manifest")
    manifest_data, manifest_metadata = tree.read("switchyard-runtime.json", 1024 * 1024)
    try:
        value = json.loads(
            manifest_data.decode("utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=lambda item: fail("non-standard JSON constant: " + item),
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        fail("cannot parse input runtime manifest: " + str(error))
    if type(value) is not dict:
        fail("input runtime manifest root is not an object")
    if value.get("runtimeFamily") != "preview-native-arm64-fex":
        fail("input runtime manifest is not the disabled native profile")
    for field in ("graphicsBackend", "wow64UnixlibPolicy", "dxmt"):
        if field in value:
            fail("input runtime manifest already contains native field: " + field)

    def digest(relative):
        data, _metadata = tree.read(relative)
        return hashlib.sha256(data).hexdigest()

    if digest(COMPANION_SCHEMA_PATH) != companion_schema_sha256:
        fail("staged companion ABI schema differs from the pinned source contract")

    value["graphicsBackend"] = "dxmt-metal"
    value["wow64UnixlibPolicy"] = {
        "contractVersion": 2,
        "handleEncoding": "generation-tagged-v1",
        "internalDispatch": {"module": "ntdll", "sourceVersion": 1},
        "externalSourceVersion": 2,
        "requiredEntryFlag": "REVIEWED",
        "auditedModules": [
            {"module": module, "unixLibrary": relative, "sha256": digest(relative)}
            for module, relative in AUDITED
        ],
    }
    modules = []
    for relative, source_digest, file_format, architecture in MODULE_SOURCES:
        item = {
            "path": relative,
            "sha256": digest(relative),
            "sourceSha256": source_digest,
            "format": file_format,
            "architecture": architecture,
        }
        if file_format == "mach-o-dylib":
            item.update({
                "platform": "macos",
                "minimumMacOS": "15.0",
                "sdk": "15.1",
                "installName": "@rpath/winemetal.so",
                "rpaths": ["@loader_path/", "@loader_path/../../"],
                "loadCommands": LOAD_COMMANDS,
            })
        modules.append(item)
    value["dxmt"] = {
        "contractVersion": 1,
        "implementation": "dxmt",
        "graphicsApi": "d3d11",
        "hostBackend": "metal",
        "provenance": {
            "sourceRepository": repository,
            "sourceRevision": revision,
            "artifactName": artifact_name,
            "artifactSha256": artifact_sha256,
            "packageWorkflow": workflow,
            "packageWorkflowSha256": workflow_sha256,
            "packageBuild": package_build,
        },
        "license": "LGPL-2.1-or-later",
        "modules": modules,
        "wow64Companion": {
            "path": COMPANION_PATH,
            "sha256": digest(COMPANION_PATH),
            "format": "mach-o-dylib",
            "architecture": "arm64",
            "minimumMacOS": "26.5",
            "sdk": "26.5",
            "installName": "@rpath/winemetal-wow64.so",
            "rpaths": ["@loader_path/"],
            "loadCommands": [
                {"command": "LC_LOAD_DYLIB", "path": "@rpath/ntdll.so"},
                {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"},
                {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"},
                {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libSystem.B.dylib"},
                {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libobjc.A.dylib"},
            ],
            "originalUnixLibrary": MODULE_SOURCES[0][0],
            "originalSha256": digest(MODULE_SOURCES[0][0]),
            "abiSchema": COMPANION_SCHEMA_PATH,
            "abiSchemaSha256": companion_schema_sha256,
            "entryCount": 138,
            "dispatchSourceVersion": 2,
            "bindingVersion": 4,
            "codeSignature": "strict",
        },
        "documents": [
            {"path": relative, "sha256": digest(relative)} for relative in DOCUMENT_PATHS
        ],
    }

    output = (json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(output) > 1024 * 1024:
        fail("produced runtime manifest exceeds its size bound")
    current_data, current_metadata = tree.read("switchyard-runtime.json", 1024 * 1024)
    identity = lambda item: (
        item.st_dev,
        item.st_ino,
        stat.S_IFMT(item.st_mode),
        item.st_size,
        item.st_mtime_ns,
        item.st_ctime_ns,
    )
    if current_data != manifest_data or identity(current_metadata) != identity(manifest_metadata):
        fail("runtime manifest changed while native fields were produced")
    temporary = f".switchyard-runtime.{os.getpid()}.tmp"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    output_fd = os.open(temporary, flags, 0o600, dir_fd=tree.root_fd)
    try:
        view = memoryview(output)
        while view:
            written = os.write(output_fd, view)
            if written <= 0:
                fail("short write while producing runtime manifest")
            view = view[written:]
        os.fchmod(output_fd, 0o644)
        os.fsync(output_fd)
    finally:
        os.close(output_fd)
    try:
        os.replace(
            temporary,
            "switchyard-runtime.json",
            src_dir_fd=tree.root_fd,
            dst_dir_fd=tree.root_fd,
        )
        temporary = None
    finally:
        if temporary is not None:
            try:
                os.unlink(temporary, dir_fd=tree.root_fd)
            except FileNotFoundError:
                pass
finally:
    tree.close()
PY
}

switchyard_validate_native_arm64_signed_manifest_refresh_in_progress() (
  local runtime_root manifest source_root refresh_token
  local taint_device taint_inode capability_token capability_device capability_inode

  [ "$#" -eq 9 ] || {
    echo "usage: switchyard_validate_native_arm64_signed_manifest_refresh_in_progress RUNTIME MANIFEST SOURCE TAINT_TOKEN TAINT_DEVICE TAINT_INODE CAPABILITY_TOKEN CAPABILITY_DEVICE CAPABILITY_INODE" >&2
    return 2
  }
  runtime_root="$1"
  manifest="$2"
  source_root="$3"
  refresh_token="$4"
  taint_device="$5"
  taint_inode="$6"
  capability_token="$7"
  capability_device="$8"
  capability_inode="$9"

  # shellcheck disable=SC2034 # Dynamically consumed by sourced validators.
  local SWITCHYARD_NATIVE_SIGNED_REFRESH_TOKEN="$refresh_token" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_DEVICE="$taint_device" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_INODE="$taint_inode" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_TOKEN="$capability_token" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_FD=18 \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_DEVICE="$capability_device" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_INODE="$capability_inode"
  local SWITCHYARD_RUNTIME_PROFILE_BOUND_ROOT="$runtime_root"
  local validation_failed=0

  if ! switchyard_validate_signed_refresh_capability_fd \
      "$runtime_root" "$refresh_token" "$taint_device" "$taint_inode" \
      "$capability_token" 18 "$capability_device" "$capability_inode"; then
    echo "Native signed-manifest refresh capability is invalid; discard the tainted private runtime." >&2
    return 1
  fi
  switchyard_validate_runtime_manifest_profile \
    "$manifest" preview-native-arm64-fex "$runtime_root" || validation_failed=1
  if [ "$validation_failed" -eq 0 ]; then
    switchyard_validate_native_arm64_runtime_packaging \
      "$runtime_root" "$manifest" "$source_root" || validation_failed=1
  fi
  if [ "$validation_failed" -ne 0 ]; then
    echo "Native signed-manifest validation failed; discard the tainted private runtime." >&2
    return 1
  fi
)

# Emit the single Python implementation used to calculate native dependency
# content-tree digests during both refresh and validation.  Keeping this logic
# shared prevents the producer and consumer from drifting apart while retaining
# the historical build_runtime.sh byte-stream contract.
switchyard_native_arm64_dependency_digest_python_source() {
  /bin/cat <<'PY'
CONTENT_MAX_ENTRIES = 300000
CONTENT_MAX_BYTES = 8 * 1024 * 1024 * 1024
CONTENT_MAX_DEPTH = 256
CONTENT_MAX_METADATA_BYTES = 32 * 1024 * 1024


def dependency_content_tree_digest(tree_relative):
    tree_parent, tree_name = open_parent(tree_relative)
    tree_fd = -1
    lines = []
    entry_count = 0
    byte_count = 0
    metadata_byte_count = 0
    try:
        entry = os.stat(tree_name, dir_fd=tree_parent, follow_symlinks=False)
        tree_fd = os.open(tree_name, DIRECTORY_FLAGS, dir_fd=tree_parent)
        before = os.fstat(tree_fd)
        if identity(entry) != identity(before):
            fail("dependency root changed while opening: " + tree_relative)
        if (
            not stat.S_ISDIR(before.st_mode)
            or before.st_uid != os.geteuid()
            or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            fail("dependency root has an unsafe type, owner, or mode: " + tree_relative)
        remember(tree_relative, before)

        def safe_tree_component(name):
            raw = os.fsencode(name)
            if (
                name in ("", ".", "..")
                or "/" in name
                or any(byte < 0x20 or byte == 0x7f for byte in raw)
            ):
                fail("dependency tree contains an unsafe path component")
            return raw

        def walk(directory_fd, relative_directory, depth):
            nonlocal entry_count, byte_count, metadata_byte_count
            if depth > CONTENT_MAX_DEPTH:
                fail("dependency tree exceeds its nesting-depth bound: " + tree_relative)
            directory_before = os.fstat(directory_fd)
            with os.scandir(directory_fd) as iterator:
                for entry_view in iterator:
                    entry_count += 1
                    if entry_count > CONTENT_MAX_ENTRIES:
                        fail("dependency tree exceeds its entry-count bound: " + tree_relative)
                    name = entry_view.name
                    safe_tree_component(name)
                    relative = name if not relative_directory else relative_directory + "/" + name
                    relative_bytes = os.fsencode(relative)
                    runtime_relative = tree_relative + "/" + relative
                    metadata_byte_count += len(os.fsencode(runtime_relative))
                    if metadata_byte_count > CONTENT_MAX_METADATA_BYTES:
                        fail("dependency tree exceeds its metadata-byte bound: " + tree_relative)
                    metadata = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                    if metadata.st_uid != os.geteuid():
                        fail("dependency entry has an unsafe owner: " + runtime_relative)
                    if stat.S_ISDIR(metadata.st_mode):
                        if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                            fail("dependency directory is group/world writable: " + runtime_relative)
                        child = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
                        try:
                            opened = os.fstat(child)
                            if identity(opened) != identity(metadata):
                                fail("dependency directory changed while opening: " + runtime_relative)
                            remember(runtime_relative, opened)
                            walk(child, relative, depth + 1)
                            if identity(os.fstat(child)) != identity(opened):
                                fail("dependency directory changed while scanning: " + runtime_relative)
                        finally:
                            os.close(child)
                    elif stat.S_ISREG(metadata.st_mode):
                        if relative == ".switchyard-content-sha256":
                            continue
                        if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                            fail("dependency file is group/world writable: " + runtime_relative)
                        if metadata.st_size > MAX_BINARY:
                            fail("dependency file exceeds its size bound: " + runtime_relative)
                        byte_count += metadata.st_size
                        if byte_count > CONTENT_MAX_BYTES:
                            fail("dependency tree exceeds its total-byte bound: " + tree_relative)
                        descriptor = os.open(name, FILE_FLAGS, dir_fd=directory_fd)
                        try:
                            opened = os.fstat(descriptor)
                            if identity(opened) != identity(metadata):
                                fail("dependency file changed while opening: " + runtime_relative)
                            file_digest = hashlib.sha256()
                            remaining = opened.st_size
                            while remaining:
                                block = os.read(descriptor, min(1024 * 1024, remaining))
                                if not block:
                                    fail("dependency file ended early: " + runtime_relative)
                                file_digest.update(block)
                                remaining -= len(block)
                            if (
                                os.read(descriptor, 1)
                                or identity(os.fstat(descriptor)) != identity(opened)
                            ):
                                fail("dependency file changed while hashing: " + runtime_relative)
                            current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                            if identity(current) != identity(opened):
                                fail("dependency file path changed while hashing: " + runtime_relative)
                            remember(runtime_relative, opened)
                        finally:
                            os.close(descriptor)
                        record = (
                            b"file ./" + relative_bytes + b" "
                            + file_digest.hexdigest().encode("ascii") + b"\n"
                        )
                        metadata_byte_count += len(relative_bytes) + len(record)
                        if metadata_byte_count > CONTENT_MAX_METADATA_BYTES:
                            fail("dependency tree exceeds its metadata-byte bound: " + tree_relative)
                        lines.append((relative_bytes, record))
                    elif stat.S_ISLNK(metadata.st_mode):
                        if relative == ".switchyard-content-sha256":
                            fail("dependency content marker is a symbolic link: " + tree_relative)
                        target = os.readlink(name, dir_fd=directory_fd)
                        target_bytes = os.fsencode(target)
                        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                        if identity(current) != identity(metadata):
                            fail("dependency symbolic link changed while reading: " + runtime_relative)
                        if (
                            not target
                            or target.startswith("/")
                            or any(byte < 0x20 or byte == 0x7f for byte in target_bytes)
                        ):
                            fail("dependency symbolic link has an unsafe target: " + runtime_relative)
                        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(relative), target))
                        if resolved == ".." or resolved.startswith("../"):
                            fail("dependency symbolic link escapes its component root: " + runtime_relative)
                        remember(runtime_relative, metadata)
                        record = b"link ./" + relative_bytes + b" " + target_bytes + b"\n"
                        metadata_byte_count += len(relative_bytes) + len(record)
                        if metadata_byte_count > CONTENT_MAX_METADATA_BYTES:
                            fail("dependency tree exceeds its metadata-byte bound: " + tree_relative)
                        lines.append((relative_bytes, record))
                    else:
                        fail("dependency tree contains an unsupported file type: " + runtime_relative)
            if identity(os.fstat(directory_fd)) != identity(directory_before):
                fail("dependency directory changed while scanning: " + tree_relative)

        walk(tree_fd, "", 0)
        current = os.stat(tree_name, dir_fd=tree_parent, follow_symlinks=False)
        if identity(current) != identity(before) or identity(os.fstat(tree_fd)) != identity(before):
            fail("dependency root changed while scanning: " + tree_relative)
    finally:
        if tree_fd >= 0:
            os.close(tree_fd)
        os.close(tree_parent)
    payload = b"".join(line for _relative, line in sorted(lines))
    return hashlib.sha256(payload).hexdigest()[:12]
PY
}

# Bind the short dependency-tree identities in a signed native manifest to the
# exact private runtime bytes.  These markers intentionally use the historical
# build_runtime.sh content_tree_digest format rather than the outer runtime
# digest format: sorted "file ./... SHA256" and "link ./... TARGET" records,
# truncated to 12 hexadecimal characters.
switchyard_validate_native_arm64_signed_dependency_trees() (
  local runtime_root manifest dependency_digest_source

  [ "$#" -eq 2 ] || {
    echo "usage: switchyard_validate_native_arm64_signed_dependency_trees RUNTIME MANIFEST" >&2
    return 2
  }
  runtime_root="$1"
  manifest="$2"
  dependency_digest_source="$(
    switchyard_native_arm64_dependency_digest_python_source
  )" || return 1

  /usr/bin/python3 -I - \
    "$runtime_root" "$manifest" "$dependency_digest_source" <<'PY'
import hashlib
import json
import os
import posixpath
import re
import stat
import sys

root_name, manifest_name, dependency_digest_source = sys.argv[1:]
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_BINARY = 512 * 1024 * 1024
MAX_MANIFEST = 1024 * 1024
MAX_PATH_WALK_COMPONENTS = 4 * 1024 * 1024
SHORT_SHA256 = re.compile(r"[0-9a-f]{12}")
COMPONENTS = (
    ("gstreamerRuntime", "lib/switchyard-gstreamer", False),
    ("vulkanRuntime", "lib/switchyard-vulkan", False),
    ("fontRuntime", "lib/switchyard-fonts", False),
    ("tlsRuntime", "lib/switchyard-tls", True),
)


def fail(message):
    raise RuntimeError("native signed dependency validation failed: " + message)


def no_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            fail("duplicate JSON object key: " + key)
        value[key] = item
    return value


def identity(info):
    return (
        info.st_dev, info.st_ino, info.st_mode, info.st_nlink, info.st_uid,
        info.st_gid, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
    )


def safe_component(name, description):
    raw = os.fsencode(name)
    if (
        name in ("", ".", "..")
        or "/" in name
        or any(byte < 0x20 or byte == 0x7f for byte in raw)
    ):
        fail(description + " contains an unsafe path component")
    return raw


if (
    not os.path.isabs(root_name)
    or os.path.normpath(root_name) != root_name
    or root_name == "/"
    or os.path.realpath(root_name) != root_name
    or manifest_name != os.path.join(root_name, "switchyard-runtime.json")
):
    fail("runtime root or manifest path is not canonical")

root_fd = os.open("/", DIRECTORY_FLAGS)
records = {}
path_walk_components = 0
try:
    for component in root_name.split("/")[1:]:
        safe_component(component, "runtime root")
        child = os.open(component, DIRECTORY_FLAGS, dir_fd=root_fd)
        os.close(root_fd)
        root_fd = child
    root_info = os.fstat(root_fd)
    root_identity = identity(root_info)
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
        or root_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        fail("runtime root has an unsafe type, owner, or mode")

    def remember(relative, info):
        current = identity(info)
        previous = records.get(relative)
        if previous is not None and previous != current:
            fail("runtime path changed while validating: " + relative)
        records[relative] = current

    def open_parent(relative):
        global path_walk_components
        parts = relative.split("/")
        if any(part in ("", ".", "..") for part in parts):
            fail("invalid runtime-relative path: " + relative)
        descriptor = os.dup(root_fd)
        try:
            walked = []
            for part in parts[:-1]:
                path_walk_components += 1
                if path_walk_components > MAX_PATH_WALK_COMPONENTS:
                    fail("runtime path traversal exceeds its component-work bound")
                safe_component(part, "runtime-relative path")
                child = os.open(part, DIRECTORY_FLAGS, dir_fd=descriptor)
                info = os.fstat(child)
                walked.append(part)
                if (
                    not stat.S_ISDIR(info.st_mode)
                    or info.st_uid != os.geteuid()
                    or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                ):
                    os.close(child)
                    fail(
                        "runtime directory has an unsafe type, owner, or mode: "
                        + "/".join(walked)
                    )
                remember("/".join(walked), info)
                os.close(descriptor)
                descriptor = child
            safe_component(parts[-1], "runtime-relative path")
            return descriptor, parts[-1]
        except BaseException:
            os.close(descriptor)
            raise

    def read_file(relative, maximum):
        parent, name = open_parent(relative)
        descriptor = -1
        try:
            entry = os.stat(name, dir_fd=parent, follow_symlinks=False)
            descriptor = os.open(name, FILE_FLAGS, dir_fd=parent)
            before = os.fstat(descriptor)
            if identity(entry) != identity(before):
                fail("runtime file changed while opening: " + relative)
            if (
                not stat.S_ISREG(before.st_mode)
                or before.st_size > maximum
                or before.st_uid != os.geteuid()
                or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            ):
                fail("runtime file has an unsafe type, size, owner, or mode: " + relative)
            chunks = []
            remaining = before.st_size
            while remaining:
                block = os.read(descriptor, min(1024 * 1024, remaining))
                if not block:
                    fail("runtime file ended early: " + relative)
                chunks.append(block)
                remaining -= len(block)
            if os.read(descriptor, 1) or identity(os.fstat(descriptor)) != identity(before):
                fail("runtime file changed while reading: " + relative)
            current = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if identity(current) != identity(before):
                fail("runtime file path changed while reading: " + relative)
            remember(relative, before)
            return b"".join(chunks), before
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            os.close(parent)

    def path_exists(relative):
        parent, name = open_parent(relative)
        try:
            try:
                os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                return False
            return True
        finally:
            os.close(parent)

    exec(dependency_digest_source, globals())
    manifest_data, manifest_info = read_file("switchyard-runtime.json", MAX_MANIFEST)
    if manifest_info.st_size <= 0:
        fail("runtime manifest is empty")
    try:
        value = json.loads(
            manifest_data.decode("utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=lambda item: fail("non-standard JSON constant: " + item),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail("cannot parse runtime manifest: " + str(error))
    signing = value.get("runtimeSigning") if type(value) is dict else None
    if (
        type(value) is not dict
        or value.get("runtimeFamily") != "preview-native-arm64-fex"
        or type(signing) is not dict
        or signing.get("mode") not in (
            "engineering-adhoc", "developer-id-hardened-runtime"
        )
    ):
        fail("runtime manifest is not a signed preview-native-arm64-fex manifest")

    for field, tree_relative, optional in COMPONENTS:
        component = value.get(field)
        exists = path_exists(tree_relative)
        if component is None:
            if not optional:
                fail("required dependency manifest is absent: " + field)
            if exists:
                fail("optional TLS tree exists without its manifest")
            continue
        if type(component) is not dict or component.get("root") != tree_relative:
            fail("dependency manifest root is not exact: " + field)
        expected = component.get("digest")
        if type(expected) is not str or SHORT_SHA256.fullmatch(expected) is None:
            fail("dependency manifest digest is malformed: " + field)
        if not exists:
            fail("dependency tree is absent: " + tree_relative)
        marker_relative = tree_relative + "/.switchyard-content-sha256"
        marker_data, marker_info = read_file(marker_relative, 13)
        if (
            marker_info.st_size != 13
            or marker_info.st_nlink != 1
            or stat.S_IMODE(marker_info.st_mode) not in (0o600, 0o644)
            or marker_data != (expected + "\n").encode("ascii")
        ):
            fail("dependency marker does not match its manifest: " + tree_relative)
        actual = dependency_content_tree_digest(tree_relative)
        if actual != expected:
            fail("dependency tree does not match its marker and manifest: " + tree_relative)

    for relative, expected in records.items():
        parent, name = open_parent(relative)
        try:
            current = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if identity(current) != expected:
                fail("runtime bytes changed before dependency validation completed: " + relative)
        finally:
            os.close(parent)
    if identity(os.fstat(root_fd)) != root_identity:
        fail("runtime root changed during dependency validation")
except (OSError, RuntimeError, UnicodeError, ValueError) as error:
    print(error, file=sys.stderr)
    raise SystemExit(1)
finally:
    os.close(root_fd)
PY
)

# Refresh every runtime identity that may change when Mach-O signatures are
# applied.  Callers must finish all signing before this function and must keep
# the runtime private until it succeeds.  A transaction marker taints the
# private runtime across the multi-file publication window; any reported or
# interrupted failure requires discarding that runtime, never retrying it.  The
# only permitted later mutation is the outer .switchyard-content-sha256 marker.
switchyard_refresh_native_arm64_signed_runtime_manifest() (
  local runtime_root manifest source_root library_dir digest_helper
  local dependency_digest_source

  [ "$#" -eq 2 ] || {
    echo "usage: switchyard_refresh_native_arm64_signed_runtime_manifest RUNTIME MANIFEST" >&2
    return 2
  }
  runtime_root="$1"
  manifest="$2"
  library_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
  source_root="$(cd "$library_dir/../.." && pwd -P)" || return 1
  digest_helper="$source_root/switchyard/runtime_content_digest.py"
  [ -f "$digest_helper" ] && [ ! -L "$digest_helper" ] && [ -x "$digest_helper" ] || {
    echo "Native signed-manifest content-digest helper is missing or unsafe." >&2
    return 1
  }
  dependency_digest_source="$(
    switchyard_native_arm64_dependency_digest_python_source
  )" || return 1
  /usr/bin/python3 -I - \
    "$runtime_root" "$manifest" "$digest_helper" "$source_root" \
    "$dependency_digest_source" <<'PY' || return 1
import errno
import hashlib
import json
import os
import plistlib
import posixpath
import re
import secrets
import shutil
import stat
import subprocess
import sys

(
    root_name,
    manifest_name,
    digest_helper,
    source_root,
    dependency_digest_source,
) = sys.argv[1:]
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_BINARY = 512 * 1024 * 1024
MAX_MANIFEST = 1024 * 1024
MAX_PATH_WALK_COMPONENTS = 4 * 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}")
SHORT_SHA256 = re.compile(r"[0-9a-f]{12}")
ENTRY_PATHS = [
    "lib/wine/aarch64-unix/wine",
    "bin/wine.switchyard-real",
]
PROVIDER_COMPONENTS = [
    ("i386", "lib/wine/aarch64-unix/xtajit.so", "lib/wine/aarch64-windows/xtajit.dll"),
    ("x86_64", "lib/wine/aarch64-unix/xtajit64.so", "lib/wine/aarch64-windows/xtajit64.dll"),
]
AUDITED_PATHS = [
    ("crypt32", "lib/wine/aarch64-unix/crypt32.so"),
    ("dwrite", "lib/wine/aarch64-unix/dwrite.so"),
    ("secur32", "lib/wine/aarch64-unix/secur32.so"),
    ("winemac", "lib/wine/aarch64-unix/winemac.so"),
    ("ws2_32", "lib/wine/aarch64-unix/ws2_32.so"),
]
TAINT_NAME = ".switchyard-signed-manifest-refresh-in-progress"
EXPECTED_ENTITLEMENTS = {
    "com.apple.security.cs.allow-dyld-environment-variables": True,
    "com.apple.security.cs.allow-jit": True,
    "com.apple.security.cs.allow-unsigned-executable-memory": True,
    "com.apple.security.custom-x18-abi-toggle": True,
}
NESTED_UNICORN_FIELDS = {
    "version", "sourceRepository", "sourceRevision", "buildContractVersion",
    "enabledArchitectures", "hostArchitecture", "minimumMacOS", "library",
    "librarySha256", "sourceArchive", "sourceArchiveSha256", "sourcePatch",
    "license",
}
CONTENT_COMPONENTS = (
    ("gstreamerRuntime", "lib/switchyard-gstreamer", False, "gstreamer-marker"),
    ("vulkanRuntime", "lib/switchyard-vulkan", False, "vulkan-marker"),
    ("fontRuntime", "lib/switchyard-fonts", False, "font-marker"),
    ("tlsRuntime", "lib/switchyard-tls", True, "tls-marker"),
)


def fail(message):
    raise RuntimeError("native signed-manifest refresh failed: " + message)


def no_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            fail("duplicate JSON object key: " + key)
        value[key] = item
    return value


def identity(info):
    return (
        info.st_dev, info.st_ino, info.st_mode, info.st_nlink, info.st_uid,
        info.st_gid, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
    )


def replacement_identity(info):
    return (
        info.st_dev, info.st_ino, info.st_mode, info.st_nlink, info.st_uid,
        info.st_gid, info.st_size,
    )


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=True, indent=2) + "\n").encode("utf-8")


def safe_component(name, description):
    raw = os.fsencode(name)
    if (
        name in ("", ".", "..")
        or "/" in name
        or any(byte < 0x20 or byte == 0x7f for byte in raw)
    ):
        fail(description + " contains an unsafe path component")
    return raw


if (
    not os.path.isabs(root_name)
    or os.path.normpath(root_name) != root_name
    or root_name == "/"
    or os.path.realpath(root_name) != root_name
    or manifest_name != os.path.join(root_name, "switchyard-runtime.json")
):
    fail("runtime root or manifest path is not canonical")
if (
    not os.path.isabs(source_root)
    or os.path.normpath(source_root) != source_root
    or source_root == "/"
    or os.path.realpath(source_root) != source_root
):
    fail("source root is not canonical")

root_fd = os.open("/", DIRECTORY_FLAGS)
stage_fd = -1
stage_name = None
package_copy = None
capability_fd = -1
records = {}
replacement_parent_fds = []
path_walk_components = 0
try:
    for component in root_name.split("/")[1:]:
        safe_component(component, "runtime root")
        child = os.open(component, DIRECTORY_FLAGS, dir_fd=root_fd)
        os.close(root_fd)
        root_fd = child
    root_info = os.fstat(root_fd)
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
        or root_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        fail("runtime root has an unsafe type, owner, or mode")
    try:
        os.stat(TAINT_NAME, dir_fd=root_fd, follow_symlinks=False)
    except FileNotFoundError:
        pass
    else:
        fail("runtime is already tainted by an incomplete signed-manifest refresh")
    try:
        os.fstat(18)
    except OSError as error:
        if error.errno != errno.EBADF:
            raise
    else:
        fail("signed-manifest refresh descriptor 18 is already in use")

    def remember(relative, info):
        current = identity(info)
        previous = records.get(relative)
        if previous is not None and previous != current:
            fail("runtime path changed while recording its identity: " + relative)
        records[relative] = current

    def open_parent(relative):
        global path_walk_components
        parts = relative.split("/")
        if any(part in ("", ".", "..") for part in parts):
            fail("invalid runtime-relative path: " + relative)
        descriptor = os.dup(root_fd)
        try:
            walked = []
            for part in parts[:-1]:
                path_walk_components += 1
                if path_walk_components > MAX_PATH_WALK_COMPONENTS:
                    fail("runtime path traversal exceeds its component-work bound")
                safe_component(part, "runtime-relative path")
                child = os.open(part, DIRECTORY_FLAGS, dir_fd=descriptor)
                info = os.fstat(child)
                walked.append(part)
                if (
                    not stat.S_ISDIR(info.st_mode)
                    or info.st_uid != os.geteuid()
                    or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                ):
                    os.close(child)
                    fail(
                        "runtime directory has an unsafe type, owner, or mode: "
                        + "/".join(walked)
                    )
                remember("/".join(walked), info)
                os.close(descriptor)
                descriptor = child
            safe_component(parts[-1], "runtime-relative path")
            return descriptor, parts[-1]
        except BaseException:
            os.close(descriptor)
            raise

    def read_file(relative, maximum=MAX_BINARY):
        parent, name = open_parent(relative)
        descriptor = -1
        try:
            entry = os.stat(name, dir_fd=parent, follow_symlinks=False)
            descriptor = os.open(name, FILE_FLAGS, dir_fd=parent)
            before = os.fstat(descriptor)
            if identity(entry) != identity(before):
                fail("runtime file changed while opening: " + relative)
            if (
                not stat.S_ISREG(before.st_mode)
                or before.st_size <= 0
                or before.st_size > maximum
                or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            ):
                fail("runtime file has an unsafe type, size, or mode: " + relative)
            chunks = []
            remaining = before.st_size
            while remaining:
                block = os.read(descriptor, min(1024 * 1024, remaining))
                if not block:
                    fail("runtime file ended early: " + relative)
                chunks.append(block)
                remaining -= len(block)
            if os.read(descriptor, 1) or identity(os.fstat(descriptor)) != identity(before):
                fail("runtime file changed while reading: " + relative)
            current = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if identity(current) != identity(before):
                fail("runtime file path changed while reading: " + relative)
            remember(relative, before)
            return b"".join(chunks), before
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            os.close(parent)

    def digest(relative):
        return hashlib.sha256(read_file(relative)[0]).hexdigest()

    def path_exists(relative):
        parent, name = open_parent(relative)
        try:
            try:
                os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                return False
            return True
        finally:
            os.close(parent)

    exec(dependency_digest_source, globals())
    manifest_data, _manifest_info = read_file("switchyard-runtime.json", MAX_MANIFEST)
    try:
        value = json.loads(
            manifest_data.decode("utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=lambda item: fail("non-standard JSON constant: " + item),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail("cannot parse runtime manifest: " + str(error))
    if type(value) is not dict or value.get("runtimeFamily") != "preview-native-arm64-fex":
        fail("runtime manifest is not preview-native-arm64-fex")

    signing = value.get("runtimeSigning")
    provider = value.get("cpuProvider")
    policy = value.get("wow64UnixlibPolicy")
    dxmt = value.get("dxmt")
    integrity = value.get("integrity")
    if type(signing) is not dict or set(signing) != {"mode", "processEntryMachOs"}:
        fail("runtimeSigning schema is not exact")
    if type(provider) is not dict or type(policy) is not dict or type(dxmt) is not dict:
        fail("native provider, v2 policy, or DXMT identity is absent")
    if provider.get("kuserSharedDataModel") != "translated-shadow":
        fail("CPU-provider KUSER_SHARED_DATA model is not translated-shadow")
    if (
        provider.get("runtimeRoot") != "lib/switchyard-unicorn"
        or provider.get("library")
        != "lib/switchyard-unicorn/lib/libunicorn.2.dylib"
        or provider.get("manifest")
        != "lib/switchyard-unicorn/switchyard-unicorn-runtime.json"
    ):
        fail("CPU-provider runtime paths are not the exact allowlist")
    if type(integrity) is not dict or set(integrity) != {
        "wineUnixSha256", "i386NtdllSha256", "x86_64NtdllSha256"
    }:
        fail("runtime integrity schema is not exact")

    dependency_markers = []
    for field, tree_relative, optional, staged_name in CONTENT_COMPONENTS:
        component = value.get(field)
        exists = path_exists(tree_relative)
        if component is None:
            if not optional:
                fail("required dependency manifest is absent: " + field)
            if exists:
                fail("optional TLS tree exists without its manifest")
            continue
        if type(component) is not dict or component.get("root") != tree_relative:
            fail("dependency manifest root is not exact: " + field)
        previous_digest = component.get("digest")
        if type(previous_digest) is not str or SHORT_SHA256.fullmatch(previous_digest) is None:
            fail("dependency manifest digest is malformed: " + field)
        if not exists:
            fail("dependency tree is absent: " + tree_relative)
        marker_relative = tree_relative + "/.switchyard-content-sha256"
        marker_data, marker_info = read_file(marker_relative, 13)
        marker_mode = stat.S_IMODE(marker_info.st_mode)
        if (
            marker_info.st_size != 13
            or marker_info.st_nlink != 1
            or marker_mode not in (0o600, 0o644)
            or marker_data != (previous_digest + "\n").encode("ascii")
        ):
            fail("dependency marker does not match its manifest: " + tree_relative)
        refreshed_digest = dependency_content_tree_digest(tree_relative)
        component["digest"] = refreshed_digest
        dependency_markers.append((
            staged_name,
            marker_relative,
            (refreshed_digest + "\n").encode("ascii"),
            marker_mode,
        ))

    def signing_mode(relative):
        path = os.path.join(root_name, relative)
        strict = subprocess.run(
            ["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if strict.returncode:
            fail("process entry failed strict signature verification: " + relative)
        details_result = subprocess.run(
            ["/usr/bin/codesign", "-d", "--verbose=4", path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if details_result.returncode:
            fail("cannot inspect process-entry signature: " + relative)
        details = (details_result.stdout + details_result.stderr).decode("utf-8", "strict")
        if (
            details.splitlines().count("Signature=adhoc") == 1
            and len(re.findall(r"\bflags=0x2\(adhoc\)(?:\s|$)", details)) == 1
            and "Runtime Version=" not in details
            and re.search(r"\bflags=.*(?:\(|,)runtime(?:,|\)|\s|$)", details) is None
        ):
            mode = "engineering-adhoc"
        elif (
            "Signature=adhoc" not in details
            and "Runtime Version=" in details
            and any(
                line.startswith("Authority=Developer ID Application:")
                for line in details.splitlines()
            )
        ):
            mode = "developer-id-hardened-runtime"
        else:
            fail("process-entry signing mode is not allowlisted: " + relative)
        entitlement_result = subprocess.run(
            ["/usr/bin/codesign", "-d", "--xml", "--entitlements", "-", path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if entitlement_result.returncode or len(entitlement_result.stdout) > 65536:
            fail("cannot inspect process-entry entitlements: " + relative)
        try:
            embedded = plistlib.loads(entitlement_result.stdout)
        except plistlib.InvalidFileException as error:
            fail("process-entry entitlements are malformed: " + str(error))
        if (
            type(embedded) is not dict
            or set(embedded) != set(EXPECTED_ENTITLEMENTS)
            or any(
                type(embedded[key]) is not bool
                or embedded[key] is not EXPECTED_ENTITLEMENTS[key]
                for key in EXPECTED_ENTITLEMENTS
            )
        ):
            fail("process-entry entitlements are not the exact allowlist: " + relative)
        return mode

    entry_modes = [signing_mode(relative) for relative in ENTRY_PATHS]
    if len(set(entry_modes)) != 1:
        fail("process entries use different signing modes")
    entries = signing.get("processEntryMachOs")
    if type(entries) is not list or len(entries) != len(ENTRY_PATHS):
        fail("runtimeSigning process-entry set is not exact")
    for item, relative in zip(entries, ENTRY_PATHS):
        if type(item) is not dict or set(item) != {"path", "sha256"} or item.get("path") != relative:
            fail("runtimeSigning process-entry order or schema is invalid")
        item["sha256"] = digest(relative)
    signing["mode"] = entry_modes[0]

    integrity["wineUnixSha256"] = digest("lib/wine/aarch64-unix/wine")
    integrity["i386NtdllSha256"] = digest("lib/wine/i386-windows/ntdll.dll")
    integrity["x86_64NtdllSha256"] = digest("lib/wine/x86_64-windows/ntdll.dll")

    components = provider.get("components")
    if type(components) is not list or len(components) != len(PROVIDER_COMPONENTS):
        fail("CPU-provider component schema is not exact")
    component_fields = {
        "guestArchitecture", "unixLibrary", "unixLibrarySha256",
        "peLibrary", "peLibrarySha256",
    }
    for component, expected in zip(components, PROVIDER_COMPONENTS):
        if (
            type(component) is not dict
            or set(component) != component_fields
            or (
                component.get("guestArchitecture"),
                component.get("unixLibrary"),
                component.get("peLibrary"),
            ) != expected
        ):
            fail("CPU-provider component order or path is not exact")
        component["unixLibrarySha256"] = digest(component["unixLibrary"])
        component["peLibrarySha256"] = digest(component["peLibrary"])

    unicorn_relative = provider.get("library")
    unicorn_digest = digest(unicorn_relative)
    provider["librarySha256"] = unicorn_digest
    nested_relative = provider.get("manifest")
    nested_data, nested_info = read_file(nested_relative, MAX_MANIFEST)
    try:
        nested = json.loads(
            nested_data.decode("utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=lambda item: fail("non-standard nested JSON constant: " + item),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail("cannot parse nested Unicorn manifest: " + str(error))
    if type(nested) is not dict or set(nested) != NESTED_UNICORN_FIELDS:
        fail("nested Unicorn manifest schema is not exact")
    if nested.get("librarySha256") == unicorn_digest:
        # Keep the pinned package manifest byte-for-byte when signing did not
        # change the Unicorn dylib.  Re-serializing an otherwise unchanged
        # manifest would alter its compact array formatting, needlessly change
        # the package content identity, and turn an engineering refresh into a
        # signed-package mutation.
        new_nested_data = nested_data
    else:
        nested["librarySha256"] = unicorn_digest
        new_nested_data = json_bytes(nested)
    if len(new_nested_data) > MAX_MANIFEST:
        fail("nested Unicorn manifest exceeds its size bound")

    audited = policy.get("auditedModules")
    if type(audited) is not list or len(audited) != len(AUDITED_PATHS):
        fail("v2 audited-module order is not exact")
    for item, expected in zip(audited, AUDITED_PATHS):
        if (
            type(item) is not dict
            or set(item) != {"module", "unixLibrary", "sha256"}
            or (item.get("module"), item.get("unixLibrary")) != expected
        ):
            fail("v2 audited-module schema, order, or path is not exact")
        item["sha256"] = digest(item["unixLibrary"])

    modules = dxmt.get("modules")
    if type(modules) is not list or len(modules) != 17:
        fail("DXMT module schema is not exact")
    for item in modules:
        if type(item) is not dict or type(item.get("path")) is not str:
            fail("DXMT module entry is invalid")
        item["sha256"] = digest(item["path"])
        if item.get("format") == "pe-dll" and item["sha256"] != item.get("sourceSha256"):
            fail("DXMT PE module differs from its pinned artifact bytes")
    companion = dxmt.get("wow64Companion")
    companion_fields = {
        "path", "sha256", "format", "architecture", "minimumMacOS", "sdk",
        "installName", "rpaths", "loadCommands", "originalUnixLibrary",
        "originalSha256", "abiSchema", "abiSchemaSha256", "entryCount",
        "dispatchSourceVersion", "bindingVersion", "codeSignature",
    }
    if (
        type(companion) is not dict
        or set(companion) != companion_fields
        or companion.get("path") != "lib/wine/aarch64-unix/winemetal-wow64.so"
        or companion.get("format") != "mach-o-dylib"
        or companion.get("architecture") != "arm64"
        or companion.get("minimumMacOS") != "26.5"
        or companion.get("sdk") != "26.5"
        or companion.get("installName") != "@rpath/winemetal-wow64.so"
        or companion.get("rpaths") != ["@loader_path/"]
        or companion.get("loadCommands") != [
            {"command": "LC_LOAD_DYLIB", "path": "@rpath/ntdll.so"},
            {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"},
            {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"},
            {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libSystem.B.dylib"},
            {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libobjc.A.dylib"},
        ]
        or companion.get("originalUnixLibrary") != modules[0].get("path")
        or companion.get("abiSchema")
        != "lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v4.txt"
        or companion.get("entryCount") != 138
        or companion.get("dispatchSourceVersion") != 2
        or companion.get("bindingVersion") != 4
        or companion.get("codeSignature") != "strict"
    ):
        fail("DXMT WoW64 companion schema is not exact")
    companion["sha256"] = digest(companion["path"])
    companion["originalSha256"] = modules[0]["sha256"]
    schema_digest = digest(companion["abiSchema"])
    if companion.get("abiSchemaSha256") != schema_digest:
        fail("DXMT WoW64 companion ABI schema differs from its manifest binding")
    files_relative = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/files.sha256"
    new_files_text = "".join(
        f"{item['sha256']}  {item['path']}\n" for item in modules
    )
    new_files_text += f"{companion['sha256']}  {companion['path']}\n"
    new_files_text += f"{schema_digest}  {companion['abiSchema']}\n"
    new_files_data = new_files_text.encode("ascii")
    _old_files_data, files_info = read_file(files_relative, MAX_MANIFEST)
    documents = dxmt.get("documents")
    if type(documents) is not list or len(documents) != 5:
        fail("DXMT document schema is not exact")
    for item in documents:
        if type(item) is not dict or set(item) != {"path", "sha256"}:
            fail("DXMT document entry is invalid")
        if item["path"] == files_relative:
            item["sha256"] = hashlib.sha256(new_files_data).hexdigest()
        else:
            item["sha256"] = digest(item["path"])

    for unused in range(128):
        candidate = ".switchyard-signed-manifest." + secrets.token_hex(16)
        try:
            os.mkdir(candidate, 0o700, dir_fd=root_fd)
            stage_name = candidate
            stage_fd = os.open(candidate, DIRECTORY_FLAGS, dir_fd=root_fd)
            break
        except FileExistsError:
            continue
    if stage_fd < 0:
        fail("cannot allocate private refresh staging directory")
    if stat.S_IMODE(os.fstat(stage_fd).st_mode) != 0o700:
        fail("refresh staging directory mode is unsafe")

    package_copy = os.path.join(root_name, stage_name, "unicorn-package")
    package_source = os.path.join(root_name, provider.get("runtimeRoot"))
    shutil.copytree(package_source, package_copy, symlinks=True, copy_function=shutil.copy2)
    nested_copy = os.path.join(package_copy, os.path.relpath(nested_relative, provider["runtimeRoot"]))
    with open(nested_copy, "wb") as stream:
        stream.write(new_nested_data)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(nested_copy, stat.S_IMODE(nested_info.st_mode))
    marker = subprocess.run(
        ["/usr/bin/python3", "-I", digest_helper, "write", package_copy],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    if marker.returncode or SHA256.fullmatch(marker.stdout.strip()) is None:
        if marker.stderr:
            sys.stderr.write(marker.stderr)
        fail("cannot produce refreshed nested Unicorn payload digest")
    provider["runtimePayloadDigest"] = marker.stdout.strip()
    with open(os.path.join(package_copy, ".switchyard-content-sha256"), "rb") as stream:
        new_unicorn_marker = stream.read(66)
    if new_unicorn_marker != (provider["runtimePayloadDigest"] + "\n").encode("ascii"):
        fail("refreshed nested Unicorn marker is inconsistent")

    output = json_bytes(value)
    if len(output) > MAX_MANIFEST:
        fail("refreshed runtime manifest exceeds its size bound")

    staged = {}
    staged_payloads = [
        ("unicorn-manifest", new_nested_data, stat.S_IMODE(nested_info.st_mode)),
        ("unicorn-marker", new_unicorn_marker, 0o644),
        ("dxmt-files", new_files_data, stat.S_IMODE(files_info.st_mode)),
        ("runtime-manifest", output, 0o644),
    ]
    staged_payloads.extend(
        (staged_name, data, mode)
        for staged_name, _relative, data, mode in dependency_markers
    )
    for name, data, mode in staged_payloads:
        descriptor = os.open(
            name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
            0o600,
            dir_fd=stage_fd,
        )
        try:
            offset = 0
            while offset < len(data):
                written = os.write(descriptor, data[offset:])
                if written <= 0:
                    fail("short write while staging refreshed identity")
                offset += written
            os.fchmod(descriptor, mode)
            os.fsync(descriptor)
            staged_info = os.fstat(descriptor)
            if (
                not stat.S_ISREG(staged_info.st_mode)
                or staged_info.st_uid != os.geteuid()
                or staged_info.st_nlink != 1
                or staged_info.st_size != len(data)
                or stat.S_IMODE(staged_info.st_mode) != mode
            ):
                fail("refreshed staged identity has an unsafe type, owner, link count, size, or mode")
        finally:
            os.close(descriptor)
        staged_path_info = os.stat(name, dir_fd=stage_fd, follow_symlinks=False)
        if identity(staged_path_info) != identity(staged_info):
            fail("refreshed staged identity changed after closing: " + name)
        staged[name] = (
            identity(staged_info), replacement_identity(staged_info)
        )
    os.fsync(stage_fd)

    replacements = [
        ("unicorn-manifest", nested_relative),
        ("unicorn-marker", provider["runtimeRoot"] + "/.switchyard-content-sha256"),
        ("dxmt-files", files_relative),
    ]
    replacements.extend(
        (staged_name, relative)
        for staged_name, relative, _data, _mode in dependency_markers
    )
    replacements.append(("runtime-manifest", "switchyard-runtime.json"))
    prepared_replacements = []
    for staged_name, relative in replacements:
        parent, target = open_parent(relative)
        replacement_parent_fds.append(parent)
        target_info = os.stat(target, dir_fd=parent, follow_symlinks=False)
        if (
            not stat.S_ISREG(target_info.st_mode)
            or target_info.st_uid != os.geteuid()
            or target_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            fail("refresh replacement target has an unsafe type, owner, or mode: " + relative)
        remember(relative, target_info)
        prepared_replacements.append((staged_name, relative, parent, target))

    for relative, expected in records.items():
        parent, name = open_parent(relative)
        try:
            current = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if identity(current) != expected:
                fail("runtime bytes changed before refreshed manifest publication: " + relative)
        finally:
            os.close(parent)

    capability_name = None
    for unused in range(128):
        candidate = ".switchyard-signed-manifest.capability." + secrets.token_hex(16)
        try:
            capability_fd = os.open(
                candidate,
                os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                0o600,
                dir_fd=root_fd,
            )
            capability_name = candidate
            break
        except FileExistsError:
            continue
    if capability_fd < 0 or capability_name is None:
        fail("cannot allocate signed-manifest refresh capability")
    os.unlink(capability_name, dir_fd=root_fd)
    os.fsync(root_fd)
    capability_token = secrets.token_hex(32)
    capability_data = (capability_token + "\n").encode("ascii")
    offset = 0
    while offset < len(capability_data):
        written = os.write(capability_fd, capability_data[offset:])
        if written <= 0:
            fail("short write while creating signed-manifest refresh capability")
        offset += written
    os.fchmod(capability_fd, 0o400)
    os.fsync(capability_fd)
    capability_info = os.fstat(capability_fd)
    if (
        not stat.S_ISREG(capability_info.st_mode)
        or capability_info.st_uid != os.geteuid()
        or stat.S_IMODE(capability_info.st_mode) != 0o400
        or capability_info.st_nlink != 0
        or capability_info.st_size != len(capability_data)
    ):
        fail("signed-manifest refresh capability is not private and unlinked")
    refresh_token = hashlib.sha256(
        b"switchyard-signed-refresh-capability-v1\0"
        + capability_token.encode("ascii")
        + b"\0"
        + str(capability_info.st_dev).encode("ascii")
        + b"\0"
        + str(capability_info.st_ino).encode("ascii")
    ).hexdigest()

    taint_fd = os.open(
        TAINT_NAME,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
        0o600,
        dir_fd=root_fd,
    )
    try:
        taint_data = (refresh_token + "\n").encode("ascii")
        written = os.write(taint_fd, taint_data)
        if written != len(taint_data):
            fail("short write while publishing refresh transaction marker")
        os.fchmod(taint_fd, 0o600)
        os.fsync(taint_fd)
    finally:
        os.close(taint_fd)
    os.fsync(root_fd)
    taint_info = os.stat(TAINT_NAME, dir_fd=root_fd, follow_symlinks=False)
    if (
        not stat.S_ISREG(taint_info.st_mode)
        or taint_info.st_uid != os.geteuid()
        or stat.S_IMODE(taint_info.st_mode) != 0o600
        or taint_info.st_nlink != 1
        or taint_info.st_size != len(taint_data)
        or (capability_info.st_dev, capability_info.st_ino)
        == (taint_info.st_dev, taint_info.st_ino)
    ):
        fail("signed-manifest refresh taint is not exact")

    for staged_name, relative, parent, target in prepared_replacements:
        target_current = os.stat(target, dir_fd=parent, follow_symlinks=False)
        if identity(target_current) != records[relative]:
            fail("refresh replacement target changed before publication: " + relative)
        staged_current = os.stat(
            staged_name, dir_fd=stage_fd, follow_symlinks=False
        )
        if identity(staged_current) != staged[staged_name][0]:
            fail("refreshed staged identity changed before publication: " + staged_name)
        os.replace(staged_name, target, src_dir_fd=stage_fd, dst_dir_fd=parent)
        published = os.stat(target, dir_fd=parent, follow_symlinks=False)
        if replacement_identity(published) != staged[staged_name][1]:
            fail("published refreshed identity differs from its staged file: " + relative)
        os.fsync(parent)
    os.fsync(stage_fd)
    for parent in replacement_parent_fds:
        os.close(parent)
    replacement_parent_fds = []
    os.fsync(root_fd)

    shutil.rmtree(package_copy)
    package_copy = None
    for entry in os.scandir(stage_fd):
        if not entry.is_file(follow_symlinks=False):
            fail("refresh staging directory contains an unexpected entry")
        os.unlink(entry.name, dir_fd=stage_fd)
    os.close(stage_fd)
    stage_fd = -1
    os.rmdir(stage_name, dir_fd=root_fd)
    stage_name = None
    os.fsync(root_fd)

    if capability_fd != 18:
        os.dup2(capability_fd, 18, inheritable=True)
        os.close(capability_fd)
        capability_fd = 18
    else:
        os.set_inheritable(capability_fd, True)

    validator_script = r'''
set -euo pipefail
source_root="$1"
for library in \
    "$source_root/switchyard/lib/runtime_profile.sh" \
    "$source_root/switchyard/lib/native_arm64_packaging.sh" \
    "$source_root/switchyard/lib/native_cpu_provider.sh" \
    "$source_root/switchyard/lib/dxmt_artifact.sh"; do
  [ -f "$library" ] && [ ! -L "$library" ] || {
    echo "Native signed-manifest validator is missing or unsafe: $library" >&2
    exit 1
  }
  source "$library"
done
switchyard_validate_native_arm64_signed_manifest_refresh_in_progress \
  "$2" "$3" "$1" "$4" "$5" "$6" "$7" "$8" "$9"
'''
    validator = subprocess.run(
        [
            "/bin/bash", "--noprofile", "--norc", "-c", validator_script, "_",
            source_root,
            root_name,
            manifest_name,
            refresh_token,
            str(taint_info.st_dev),
            str(taint_info.st_ino),
            capability_token,
            str(capability_info.st_dev),
            str(capability_info.st_ino),
        ],
        check=False,
        close_fds=True,
        cwd="/",
        env={
            "HOME": root_name,
            "LANG": "C",
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
            "TMPDIR": "/private/tmp",
        },
        pass_fds=(18,),
    )
    if validator.returncode:
        fail("post-publication validators rejected the tainted private runtime")
    taint_fd = os.open(TAINT_NAME, FILE_FLAGS, dir_fd=root_fd)
    try:
        taint_current = os.fstat(taint_fd)
        taint_path = os.stat(TAINT_NAME, dir_fd=root_fd, follow_symlinks=False)
        capability_current = os.fstat(capability_fd)
        capability_commitment = hashlib.sha256(
            b"switchyard-signed-refresh-capability-v1\0"
            + capability_token.encode("ascii")
            + b"\0"
            + str(capability_current.st_dev).encode("ascii")
            + b"\0"
            + str(capability_current.st_ino).encode("ascii")
        ).hexdigest()
        if (
            identity(taint_current) != identity(taint_info)
            or identity(taint_path) != identity(taint_info)
            or not stat.S_ISREG(taint_current.st_mode)
            or taint_current.st_uid != os.geteuid()
            or stat.S_IMODE(taint_current.st_mode) != 0o600
            or taint_current.st_nlink != 1
            or os.pread(taint_fd, 66, 0) != taint_data
            or identity(capability_current) != identity(capability_info)
            or not stat.S_ISREG(capability_current.st_mode)
            or capability_current.st_uid != os.geteuid()
            or stat.S_IMODE(capability_current.st_mode) != 0o400
            or capability_current.st_nlink != 0
            or os.pread(capability_fd, 66, 0) != capability_data
            or (capability_current.st_dev, capability_current.st_ino)
            == (taint_current.st_dev, taint_current.st_ino)
            or capability_commitment != refresh_token
        ):
            fail("signed-manifest refresh authority changed before commit")
        os.unlink(TAINT_NAME, dir_fd=root_fd)
        os.fsync(root_fd)
    finally:
        os.close(taint_fd)
except (OSError, RuntimeError, UnicodeError, ValueError) as error:
    print(error, file=sys.stderr)
    raise SystemExit(1)
finally:
    for parent in replacement_parent_fds:
        try:
            os.close(parent)
        except OSError:
            pass
    if capability_fd >= 0:
        try:
            os.close(capability_fd)
        except OSError:
            pass
    if package_copy is not None:
        try:
            shutil.rmtree(package_copy)
        except OSError:
            pass
    if stage_fd >= 0:
        try:
            for entry in os.scandir(stage_fd):
                if entry.is_file(follow_symlinks=False):
                    os.unlink(entry.name, dir_fd=stage_fd)
        except OSError:
            pass
        os.close(stage_fd)
    if stage_name is not None:
        try:
            os.rmdir(stage_name, dir_fd=root_fd)
        except OSError:
            pass
    os.close(root_fd)
PY
)

switchyard_validate_native_arm64_runtime_packaging() {
  local runtime_root manifest source_root
  local SWITCHYARD_RUNTIME_PROFILE_BOUND_ROOT=""

  [ "$#" -eq 3 ] || {
    echo "usage: switchyard_validate_native_arm64_runtime_packaging RUNTIME MANIFEST SOURCE" >&2
    return 2
  }
  runtime_root="$1"
  manifest="$2"
  source_root="$3"
  if [ -n "$(switchyard_runtime_manifest_value runtimeSigning.mode "$manifest")" ]; then
    # shellcheck disable=SC2034 # Dynamically consumed by the profile validator.
    SWITCHYARD_RUNTIME_PROFILE_BOUND_ROOT="$runtime_root"
  fi
  for validator in \
      switchyard_validate_native_arm64_signed_dependency_trees \
      switchyard_validate_native_cpu_provider_files \
      switchyard_validate_wow64_unixlib_policy_manifest \
      switchyard_validate_dxmt_wow64_companion_source \
      switchyard_validate_dxmt_runtime_manifest; do
    declare -F "$validator" >/dev/null || {
      echo "Native ARM64 packaging validator is unavailable: $validator" >&2
      return 1
    }
  done
  if [ -n "$SWITCHYARD_RUNTIME_PROFILE_BOUND_ROOT" ]; then
    switchyard_validate_native_arm64_signed_dependency_trees \
      "$runtime_root" "$manifest" || return 1
  fi
  switchyard_validate_native_cpu_provider_files "$manifest" "$runtime_root" || return 1
  switchyard_validate_wow64_unixlib_policy_manifest \
    "$runtime_root" "$manifest" "$source_root" || return 1
  switchyard_validate_dxmt_wow64_companion_source "$source_root" >/dev/null || return 1
  switchyard_validate_dxmt_runtime_manifest "$runtime_root" "$manifest"
}
