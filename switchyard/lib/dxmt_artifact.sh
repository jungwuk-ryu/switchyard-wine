#!/usr/bin/env bash

# DXMT is an optional native-profile graphics artifact.  This library only
# validates the installed artifact contract; selecting, staging, signing, and
# enabling the native profile remain separate acceptance gates.

switchyard_validate_dxmt_wow64_companion_source() {
  local source_root

  [ "$#" -eq 1 ] || {
    echo "usage: switchyard_validate_dxmt_wow64_companion_source SOURCE" >&2
    return 2
  }
  source_root="$1"
  [ -n "${SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256:-}" ] || {
    echo "DXMT WoW64 companion schema policy is unavailable." >&2
    return 1
  }

  /usr/bin/python3 -I - "$source_root" \
    "$SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256" <<'PY'
import hashlib
import os
import re
import stat
import sys

root, expected_schema_sha256 = sys.argv[1:]
required = (
    "configure.ac",
    "configure",
    "include/wine/unixlib.h",
    "dlls/winemetal-wow64/Makefile.in",
    "dlls/winemetal-wow64/adapter.c",
    "dlls/winemetal-wow64/buffer.h",
    "dlls/winemetal-wow64/buffer.m",
    "dlls/winemetal-wow64/commands.c",
    "dlls/winemetal-wow64/unixlib.c",
    "dlls/winemetal-wow64/winemetal_private.h",
    "dlls/winemetal-wow64/abi-schema-v6.txt",
)


def fail(message):
    raise SystemExit("DXMT WoW64 companion source preflight failed: " + message)


if (
    not os.path.isabs(root)
    or os.path.normpath(root) != root
    or root == "/"
    or os.path.realpath(root) != root
    or any(character in root for character in ("\0", "\n", "\r"))
):
    fail("source root is not a bounded canonical absolute path")
if re.fullmatch(r"[0-9a-f]{64}", expected_schema_sha256) is None:
    fail("configured schema digest is malformed")


def read_file(relative, maximum):
    descriptor = os.open(
        root,
        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0),
    )
    child = None
    try:
        parts = relative.split("/")
        for part in parts[:-1]:
            child = os.open(
                part,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0),
                dir_fd=descriptor,
            )
            os.close(descriptor)
            descriptor = child
            child = None
        child = os.open(
            parts[-1],
            os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0),
            dir_fd=descriptor,
        )
        before = os.fstat(child)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_size <= 0
            or before.st_size > maximum
            or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            fail("required source has an unsafe type, size, or mode: " + relative)
        data = bytearray()
        while block := os.read(child, min(1024 * 1024, before.st_size + 1 - len(data))):
            data.extend(block)
        after = os.fstat(child)
        identity = lambda value: (
            value.st_dev, value.st_ino, value.st_mode, value.st_size,
            value.st_mtime_ns, value.st_ctime_ns,
        )
        if len(data) != before.st_size or identity(before) != identity(after):
            fail("required source changed while reading: " + relative)
        return bytes(data)
    finally:
        if child is not None:
            os.close(child)
        os.close(descriptor)


contents = {relative: read_file(relative, 32 * 1024 * 1024) for relative in required}
schema = contents["dlls/winemetal-wow64/abi-schema-v6.txt"]
if (
    hashlib.sha256(schema).hexdigest() != expected_schema_sha256
    or not schema.endswith(b"\n")
    or b"\r" in schema
    or b"\0" in schema
    or any(byte > 0x7f for byte in schema)
):
    fail("abi-schema-v6.txt is not the exact pinned ASCII/LF schema")
header = contents["include/wine/unixlib.h"]
header_match = re.search(
    rb"^#define WINE_WOW64_UNIXLIB_COMPANION_V6_ABI_SHA256 \\\n"
    rb"((?:[ \t]+.*\n)+)",
    header,
    re.MULTILINE,
)
if header_match is None:
    fail("unixlib.h has no canonical companion v6 ABI digest")
header_digest_bytes = re.findall(rb"0x([0-9a-fA-F]{2})(?![0-9a-fA-F])", header_match.group(1))
if len(header_digest_bytes) != 32 or b"".join(header_digest_bytes).decode("ascii").lower() != expected_schema_sha256:
    fail("unixlib.h companion v6 ABI digest does not match abi-schema-v6.txt")
makefile = contents["dlls/winemetal-wow64/Makefile.in"]
if b"UNIXLIB" not in makefile or b"$(WINEMETAL_WOW64_UNIXLIB)" not in makefile:
    fail("companion Makefile does not use the configured Unixlib target")
library_match = re.search(rb"^UNIX_LIBS\s*=\s*\\\n((?:\t[^\n]+(?:\s*\\)?\n)+)", makefile, re.MULTILINE)
if not library_match:
    fail("companion Makefile has no bounded UNIX_LIBS list")
makefile_libraries = [
    line.strip().removesuffix(b"\\").strip().decode("ascii")
    for line in library_match.group(1).splitlines()
]
if makefile_libraries != [
    "-framework Foundation",
    "-framework Metal",
    "-framework CoreFoundation",
]:
    fail("companion Makefile framework closure is not exact")
source_match = re.search(rb"^SOURCES\s*=\s*\\\n((?:\t[^\n]+(?:\s*\\)?\n)+)", makefile, re.MULTILINE)
if not source_match:
    fail("companion Makefile has no bounded SOURCES list")
makefile_sources = {
    line.strip().removesuffix(b"\\").strip().decode("ascii")
    for line in source_match.group(1).splitlines()
}
if makefile_sources != {"adapter.c", "buffer.m", "commands.c", "unixlib.c"}:
    fail("companion Makefile source closure is not exact")
configure_ac = contents["configure.ac"]
configure = contents["configure"]
for data, description in ((configure_ac, "configure.ac"), (configure, "configure")):
    if b"WINEMETAL_WOW64_UNIXLIB" not in data or b"winemetal-wow64.so" not in data:
        fail(description + " does not enable the arm64 companion")
unixlib = contents["dlls/winemetal-wow64/unixlib.c"]
for symbol in (
    b"__wine_unix_call_wow64_funcs",
    b"__wine_unix_call_wow64_companion_v6",
):
    if symbol not in unixlib:
        fail("companion source is missing required export: " + symbol.decode("ascii"))
if unixlib.count(b"WINE_UNIXLIB_DISPATCH_SOURCE_V2(") != 1:
    fail("companion source does not publish exactly one v2 dispatch source")
for version in range(1, 6):
    retired = b"__wine_unix_call_wow64_companion_v" + str(version).encode("ascii")
    if retired in unixlib:
        fail("companion source still exports a retired descriptor")
print(expected_schema_sha256)
PY
}

switchyard_validate_dxmt_runtime_manifest() {
  local runtime_root runtime_manifest

  [ "$#" -eq 2 ] || {
    echo "usage: switchyard_validate_dxmt_runtime_manifest RUNTIME MANIFEST" >&2
    return 2
  }
  runtime_root="$1"
  runtime_manifest="$2"
  case "$runtime_root:$runtime_manifest" in
    /*:/*) ;;
    *)
      echo "DXMT runtime and manifest paths must be absolute." >&2
      return 2
      ;;
  esac
  [ "$runtime_root" != / ] || {
    echo "DXMT runtime root must not be the filesystem root." >&2
    return 1
  }
  [ -d "$runtime_root" ] && [ ! -L "$runtime_root" ] || {
    echo "DXMT runtime root is missing or is a symbolic link." >&2
    return 1
  }

  /usr/bin/python3 -I - "$runtime_root" "$runtime_manifest" <<'PY'
import hashlib
import json
import os
import re
import signal
import stat
import struct
import subprocess
import sys
import tempfile


SOURCE_REPOSITORY = "https://github.com/3Shain/dxmt.git"
SOURCE_BASE_TREE = "22fa93d36867f175c0283b36cd3628a4df94876e"
SOURCE_REVISION = "856d9f35789679ef00c1ba01a6353438df84b66f"
SOURCE_TREE = "a8c397f9b03dcb3592f6b0204ae6dbda5492990d"
SOURCE_PATCH = "0001-fix-dxmt-use-owned-buffer-backing-for-i386.patch"
SOURCE_PATCH_SHA256 = "5491ef13f2adfd611c12df30f191ac0ffd0083bcb246c5ab81ef1d29a8baa852"
ARTIFACT_BUILD_IDENTITY = "f02a37f5b7c8022941712a7cf9415ac9d1925442"
ARTIFACT_NAME = f"dxmt-{ARTIFACT_BUILD_IDENTITY}.tar.gz"
ARTIFACT_SHA256 = "4bf4f0bd654a92c0feb6a8e5b960307be53d62ef45f9ed32fdcbf37c418b8a3c"
PACKAGE_WORKFLOW = ".github/workflows/ci.yml"
PACKAGE_WORKFLOW_SHA256 = "fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851"
PACKAGE_BUILD = (
    "gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+"
    "clang-release-arm64ec-windows-cross"
)
LICENSE_EXPRESSION = "LGPL-2.1-or-later"
# These are distinct contracts: the runtime profile requires macOS 26.5,
# while the pinned upstream Mach-O was linked with LC_BUILD_VERSION minOS 15.0.
HOST_MINIMUM_MACOS = "26.5"
MACHO_MINIMUM_MACOS = "15.0"
MACHO_SDK = "15.1"
COMPANION_PATH = "lib/wine/aarch64-unix/winemetal-wow64.so"
COMPANION_SCHEMA_PATH = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v6.txt"
SOURCE_PATCH_PATH = "lib/switchyard-dxmt/share/src/switchyard-dxmt/" + SOURCE_PATCH
COMPANION_SCHEMA_SHA256 = "0051bd8c0bc3e3ce261e9d5007665342ac2d28a643576744d8ec71896af856f1"
COMPANION_ENTRY_COUNT = 138
COMPANION_MINIMUM_MACOS = "26.5"
COMPANION_SDK = "26.5"
COMPANION_RPATHS = ["@loader_path/"]
COMPANION_LOAD_COMMANDS = [
    {"command": "LC_LOAD_DYLIB", "path": "@rpath/ntdll.so"},
    {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"},
    {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"},
    {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libSystem.B.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libobjc.A.dylib"},
]

MODULE_SOURCES = [
    ("lib/wine/aarch64-unix/winemetal.so", "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0", "mach-o-dylib", "arm64"),
    ("lib/wine/aarch64-windows/d3d10core.dll", "0ca52517ce266d63b85310a8aae940e92b0a05392d1d03698dbc4156ce28a959", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/d3d11.dll", "bb74a3835c731d7dfe19e9d928cf20a4eef6d37c88edddfcf112557408a01fc6", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/dxgi.dll", "9c374cc1896dca4129fd5c810c09e8dce9df6b04398ddb1207da6bce01e15e3c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvapi64.dll", "f4e1cf79244d378c660b5d9b6c98923e29f2bd30e9073dadf62ac1879ffd9f02", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvngx.dll", "b8ddc2d81dcf4306b58398b486299f31067617e4f5e66cd64c8e5eacde2a0c0c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/winemetal.dll", "64007d8901b691bd91aac8218bddb12e2cce272fbdaab8a7bdc3f0ca6fe3eb99", "pe-dll", "arm64ec"),
    ("lib/wine/i386-windows/d3d10core.dll", "2408d249cfe0ea8cb333a816dc833725ae85d76a94a414b31272b2d53807a1a6", "pe-dll", "i386"),
    ("lib/wine/i386-windows/d3d11.dll", "35be5a26db509ca206b6521bb79dbb16b49dd8ec79e863fac5a1eb8d572700d4", "pe-dll", "i386"),
    ("lib/wine/i386-windows/dxgi.dll", "eea621daefc1e1d811eb780372af639a2229ab4596ca049e7d8554f96595feb3", "pe-dll", "i386"),
    ("lib/wine/i386-windows/winemetal.dll", "a4da600c7f33eee3b5cd74bd763c5df9dc08ca543ad8537bfd8e845463a38db0", "pe-dll", "i386"),
    ("lib/wine/x86_64-windows/d3d10core.dll", "4910ce0b1960a627c61114b019869057be8e1bf2edddd2ecb348c434bb98e5e0", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/d3d11.dll", "26b88098961e936b3bfe0ad984d3ad2a4568f10b04a4e6f7fa54711a9c17b583", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/dxgi.dll", "19ffb16b5dd22c944b284d9ea6d7b301e2ad96ef68f65ebdb642db49c55a9491", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/nvapi64.dll", "6e1bb14e6fb6c6f64d30e67aa351550d85d7d32d43ae429831f9ca49550ed323", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/nvngx.dll", "97e48d69a527e82b4269f50b1e1d5041e594e5a1dac5b51fce43008d372733d6", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/winemetal.dll", "34c66a7e56d1c0315f160775be009cf92efc56ec9396c2d61b6f03c307abefed", "pe-dll", "x86_64"),
]

DOCUMENTS = [
    ("lib/switchyard-dxmt/share/doc/switchyard-dxmt/files.sha256", None),
    ("lib/switchyard-dxmt/share/doc/switchyard-dxmt/LICENSE", "b87c35aef7b2cf14de854118ca55ce5c4b284c85b5f002421fb8d46d868c2d17"),
    ("lib/switchyard-dxmt/share/doc/switchyard-dxmt/COPYING.LIB", "e237fa56668030e928551ddd60f05df5fe957f75eab874bbd017e085ed722e7c"),
    ("lib/switchyard-dxmt/share/doc/switchyard-dxmt/CORRESPONDING-SOURCE.txt", "972485701e4d189475644657c6a35a1380484e0e252995a80f3b3ad17311327c"),
    (COMPANION_SCHEMA_PATH, COMPANION_SCHEMA_SHA256),
]
SOURCE_MATERIALS = [
    (SOURCE_PATCH_PATH, SOURCE_PATCH_SHA256, "patch"),
]

MACHO_RPATHS = ["@loader_path/", "@loader_path/../../"]
MACHO_LOAD_COMMANDS = [
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

DXMT_BASENAMES = {
    "d3d10core.dll", "d3d11.dll", "dxgi.dll", "nvapi64.dll", "nvngx.dll",
    "winemetal.dll", "winemetal.so", "winemetal-wow64.so",
}
EXPECTED_MODULE_PATHS = {item[0] for item in MODULE_SOURCES} | {COMPANION_PATH}
DIGEST_PATTERN = re.compile(r"[0-9a-f]{64}")


class ValidationError(Exception):
    pass


def reject(message):
    raise ValidationError(message)


def object_without_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            reject("runtime manifest contains a duplicate JSON object key")
        value[key] = item
    return value


def reject_nonstandard_constant(value):
    reject("runtime manifest contains a non-standard JSON constant: " + value)


def require_exact_type(container, key, wanted_type, context):
    if key not in container or type(container[key]) is not wanted_type:
        reject(f"{context}.{key} has an unexpected JSON type")
    return container[key]


def require_exact_keys(value, keys, context):
    if type(value) is not dict or set(value) != set(keys):
        reject(f"{context} has an unexpected field set")


def validate_relative_path(relative):
    if type(relative) is not str or not relative or "\0" in relative:
        reject("DXMT contract contains an invalid relative path")
    if relative.startswith("/") or relative.endswith("/"):
        reject("DXMT contract path is not a normalized relative path: " + relative)
    components = relative.split("/")
    if any(component in ("", ".", "..") for component in components):
        reject("DXMT contract path is not a normalized relative path: " + relative)
    return components


def file_identity(metadata):
    return (
        metadata.st_dev,
        metadata.st_ino,
        stat.S_IFMT(metadata.st_mode),
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )


class RuntimeTree:
    def __init__(self, root):
        if (
            not os.path.isabs(root)
            or os.path.normpath(root) != root
            or root == "/"
            or "\n" in root
            or "\r" in root
            or "\0" in root
        ):
            reject("DXMT runtime root is not a normalized bounded absolute path")
        if os.path.realpath(root) != root:
            reject("DXMT runtime root contains a symbolic-link component")
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open("/", flags)
        try:
            for component in root.split("/")[1:]:
                child = os.open(component, flags, dir_fd=descriptor)
                try:
                    metadata = os.fstat(child)
                except Exception:
                    os.close(child)
                    raise
                if not stat.S_ISDIR(metadata.st_mode):
                    os.close(child)
                    reject("DXMT runtime root component is not a directory")
                os.close(descriptor)
                descriptor = child
        except Exception:
            os.close(descriptor)
            raise
        if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            os.close(descriptor)
            reject("DXMT runtime root is group/world writable")
        self.root = root
        self.root_fd = descriptor
        self.root_identity = metadata.st_dev, metadata.st_ino

    def close(self):
        if self.root_fd is not None:
            os.close(self.root_fd)
            self.root_fd = None

    def validate_root_path(self):
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open("/", flags)
        try:
            for component in self.root.split("/")[1:]:
                child = os.open(component, flags, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
            metadata = os.fstat(descriptor)
            if not stat.S_ISDIR(metadata.st_mode):
                reject("DXMT runtime root path is no longer a directory")
            if (metadata.st_dev, metadata.st_ino) != self.root_identity:
                reject("DXMT runtime root changed during validation")
        finally:
            os.close(descriptor)

    def open_directory(self, relative):
        components = validate_relative_path(relative)
        descriptor = os.dup(self.root_fd)
        try:
            for component in components:
                flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
                child = os.open(component, flags, dir_fd=descriptor)
                try:
                    metadata = os.fstat(child)
                except Exception:
                    os.close(child)
                    raise
                if not stat.S_ISDIR(metadata.st_mode):
                    os.close(child)
                    reject("DXMT path component is not a directory: " + relative)
                if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                    os.close(child)
                    reject("DXMT path component is group/world writable: " + relative)
                os.close(descriptor)
                descriptor = child
            return descriptor
        except Exception:
            os.close(descriptor)
            raise

    def open_file(self, relative, maximum_size):
        components = validate_relative_path(relative)
        descriptor = os.dup(self.root_fd)
        try:
            for component in components[:-1]:
                flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
                child = os.open(component, flags, dir_fd=descriptor)
                try:
                    metadata = os.fstat(child)
                except Exception:
                    os.close(child)
                    raise
                if not stat.S_ISDIR(metadata.st_mode):
                    os.close(child)
                    reject("DXMT path component is not a directory: " + relative)
                if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                    os.close(child)
                    reject("DXMT path component is group/world writable: " + relative)
                os.close(descriptor)
                descriptor = child
            flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
            flags |= getattr(os, "O_NOFOLLOW", 0)
            file_descriptor = os.open(components[-1], flags, dir_fd=descriptor)
            try:
                metadata = os.fstat(file_descriptor)
            except Exception:
                os.close(file_descriptor)
                raise
            if not stat.S_ISREG(metadata.st_mode):
                os.close(file_descriptor)
                reject("DXMT path is not a regular file: " + relative)
            if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                os.close(file_descriptor)
                reject("DXMT path is group/world writable: " + relative)
            if metadata.st_size <= 0 or metadata.st_size > maximum_size:
                os.close(file_descriptor)
                reject("DXMT file size is outside its validation bound: " + relative)
            return file_descriptor, metadata
        finally:
            os.close(descriptor)

    def read_file(self, relative, maximum_size):
        descriptor, metadata = self.open_file(relative, maximum_size)
        try:
            data = bytearray()
            while len(data) <= metadata.st_size:
                block = os.read(descriptor, min(1024 * 1024, metadata.st_size + 1 - len(data)))
                if not block:
                    break
                data.extend(block)
            if len(data) != metadata.st_size:
                reject("DXMT file changed or was truncated while being read: " + relative)
            after = os.fstat(descriptor)
            if file_identity(metadata) != file_identity(after):
                reject("DXMT file identity changed while being read: " + relative)
            replacement, replacement_metadata = self.open_file(relative, maximum_size)
            os.close(replacement)
            if file_identity(after) != file_identity(replacement_metadata):
                reject("DXMT path was replaced while being read: " + relative)
            return data, metadata
        finally:
            os.close(descriptor)

    def require_entries(self, relative, expected):
        descriptor = self.open_directory(relative)
        try:
            actual = set()
            with os.scandir(descriptor) as entries:
                for entry in entries:
                    actual.add(entry.name)
                    if len(actual) > len(expected):
                        reject(f"DXMT directory has an unexpected entry set: {relative}")
        finally:
            os.close(descriptor)
        if actual != set(expected):
            reject(f"DXMT directory has an unexpected entry set: {relative}")

    def discover_owned_modules(self):
        wine_descriptor = self.open_directory("lib/wine")
        discovered = set()
        try:
            with os.scandir(wine_descriptor) as entries:
                directory_entry_count = 0
                for entry in entries:
                    directory_entry_count += 1
                    if directory_entry_count > 100000:
                        reject("DXMT Wine directory exceeds the validation entry bound")
                    if entry.name.lower() in DXMT_BASENAMES:
                        discovered.add("lib/wine/" + entry.name)
                    if entry.is_symlink():
                        if entry.name.lower().endswith(("-unix", "-windows")):
                            reject("DXMT refuses a symbolic-link Wine architecture directory: " + entry.name)
                        continue
                    if not entry.is_dir(follow_symlinks=False):
                        continue
                    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                    flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
                    child = os.open(entry.name, flags, dir_fd=wine_descriptor)
                    try:
                        with os.scandir(child) as child_entries:
                            child_entry_count = 0
                            for child_entry in child_entries:
                                child_entry_count += 1
                                if child_entry_count > 100000:
                                    reject("DXMT Wine architecture directory exceeds the validation entry bound")
                                if child_entry.name.lower() in DXMT_BASENAMES:
                                    discovered.add(f"lib/wine/{entry.name}/{child_entry.name}")
                    finally:
                        os.close(child)
        finally:
            os.close(wine_descriptor)
        if discovered != EXPECTED_MODULE_PATHS:
            missing = sorted(EXPECTED_MODULE_PATHS - discovered)
            extra = sorted(discovered - EXPECTED_MODULE_PATHS)
            reject(f"DXMT runtime module closure mismatch; missing={missing}, extra={extra}")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def macho_version(raw):
    major = raw >> 16
    minor = (raw >> 8) & 0xFF
    patch = raw & 0xFF
    if patch:
        return f"{major}.{minor}.{patch}"
    return f"{major}.{minor}"


def macho_string(data, command_offset, command_size, string_offset, context):
    if string_offset < 8 or string_offset >= command_size:
        reject("DXMT Mach-O has an invalid load-command string offset: " + context)
    start = command_offset + string_offset
    end = data.find(b"\0", start, command_offset + command_size)
    if end < 0:
        reject("DXMT Mach-O has an unterminated load-command string: " + context)
    try:
        return data[start:end].decode("ascii")
    except UnicodeDecodeError:
        reject("DXMT Mach-O load-command string is not ASCII: " + context)


def validate_macho(data):
    if len(data) < 32:
        reject("DXMT Unix module has a truncated Mach-O header")
    magic, cpu_type, cpu_subtype, file_type, command_count, command_bytes, _flags, _reserved = struct.unpack_from(
        "<IiiIIIII", data, 0
    )
    if magic != 0xFEEDFACF or cpu_type != 0x0100000C or cpu_subtype != 0 or file_type != 0x6:
        reject("DXMT Unix module is not a thin arm64 MH_DYLIB")
    if command_count == 0 or command_count > 1024 or command_bytes > len(data) - 32:
        reject("DXMT Unix module has invalid Mach-O load-command bounds")

    command_offset = 32
    command_end = 32 + command_bytes
    install_names = []
    build_versions = []
    rpaths = []
    dylib_loads = []
    code_signatures = []
    dylib_commands = {
        0x0000000C: "LC_LOAD_DYLIB",
        0x80000018: "LC_LOAD_WEAK_DYLIB",
        0x8000001F: "LC_REEXPORT_DYLIB",
        0x00000020: "LC_LAZY_LOAD_DYLIB",
        0x80000023: "LC_LOAD_UPWARD_DYLIB",
    }
    forbidden_path_commands = {
        0x00000006: "LC_LOADFVMLIB",
        0x00000007: "LC_IDFVMLIB",
        0x0000000E: "LC_LOAD_DYLINKER",
        0x0000000F: "LC_ID_DYLINKER",
        0x00000010: "LC_PREBOUND_DYLIB",
        0x00000012: "LC_SUB_FRAMEWORK",
        0x00000013: "LC_SUB_UMBRELLA",
        0x00000014: "LC_SUB_CLIENT",
        0x00000015: "LC_SUB_LIBRARY",
        0x00000027: "LC_DYLD_ENVIRONMENT",
    }
    for _index in range(command_count):
        if command_offset > command_end - 8:
            reject("DXMT Unix module has a truncated Mach-O load command")
        command, command_size = struct.unpack_from("<II", data, command_offset)
        if command_size < 8 or command_size % 8 or command_offset > command_end - command_size:
            reject("DXMT Unix module has an invalid Mach-O load-command size")
        if command == 0x0000000D:
            if command_size < 24:
                reject("DXMT Unix module has a truncated LC_ID_DYLIB")
            string_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
            install_names.append(macho_string(data, command_offset, command_size, string_offset, "LC_ID_DYLIB"))
        elif command in dylib_commands:
            if command_size < 24:
                reject("DXMT Unix module has a truncated dylib load command")
            string_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
            dylib_loads.append({
                "command": dylib_commands[command],
                "path": macho_string(data, command_offset, command_size, string_offset, dylib_commands[command]),
            })
        elif command == 0x8000001C:
            if command_size < 12:
                reject("DXMT Unix module has a truncated LC_RPATH")
            string_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
            rpaths.append(macho_string(data, command_offset, command_size, string_offset, "LC_RPATH"))
        elif command == 0x00000032:
            if command_size < 24:
                reject("DXMT Unix module has a truncated LC_BUILD_VERSION")
            platform, minimum_os, sdk, tool_count = struct.unpack_from("<IIII", data, command_offset + 8)
            if tool_count > (command_size - 24) // 8:
                reject("DXMT Unix module has invalid LC_BUILD_VERSION tool bounds")
            build_versions.append((platform, macho_version(minimum_os), macho_version(sdk)))
        elif command == 0x0000001D:
            if command_size < 16:
                reject("DXMT Unix module has a truncated LC_CODE_SIGNATURE")
            data_offset, data_size = struct.unpack_from("<II", data, command_offset + 8)
            if data_size == 0 or data_offset > len(data) - data_size:
                reject("DXMT Unix module has invalid code-signature bounds")
            code_signatures.append((data_offset, data_size))
        elif command in forbidden_path_commands:
            reject("DXMT Unix module contains a forbidden " + forbidden_path_commands[command])
        command_offset += command_size
    if command_offset != command_end:
        reject("DXMT Unix module load-command bytes do not close exactly")
    if install_names != ["@rpath/winemetal.so"]:
        reject("DXMT Unix module has an unexpected install name")
    if build_versions != [(1, MACHO_MINIMUM_MACOS, MACHO_SDK)]:
        reject("DXMT Unix module has an unexpected platform, minOS, or SDK")
    if rpaths != MACHO_RPATHS:
        reject("DXMT Unix module has an unexpected LC_RPATH closure")
    if dylib_loads != MACHO_LOAD_COMMANDS:
        reject("DXMT Unix module has an unexpected dylib/framework load-command closure")
    if len(code_signatures) != 1:
        reject("DXMT Unix module must contain exactly one embedded code signature")
    signature_offset, signature_size = code_signatures[0]
    if signature_offset < command_end or signature_offset + signature_size != len(data):
        reject("DXMT Unix module code signature is not the exact terminal file region")


def validate_companion_macho(data):
    if len(data) < 32:
        reject("DXMT WoW64 companion has a truncated Mach-O header")
    magic, cpu_type, cpu_subtype, file_type, command_count, command_bytes, _flags, _reserved = struct.unpack_from(
        "<IiiIIIII", data, 0
    )
    if magic != 0xFEEDFACF or cpu_type != 0x0100000C or cpu_subtype != 0 or file_type != 0x6:
        reject("DXMT WoW64 companion is not a thin arm64 MH_DYLIB")
    if command_count == 0 or command_count > 1024 or command_bytes > len(data) - 32:
        reject("DXMT WoW64 companion has invalid Mach-O load-command bounds")
    command_offset = 32
    command_end = 32 + command_bytes
    install_names = []
    build_versions = []
    rpaths = []
    dylib_loads = []
    code_signatures = []
    dylib_commands = {
        0x0000000C: "LC_LOAD_DYLIB",
        0x80000018: "LC_LOAD_WEAK_DYLIB",
        0x8000001F: "LC_REEXPORT_DYLIB",
        0x00000020: "LC_LAZY_LOAD_DYLIB",
        0x80000023: "LC_LOAD_UPWARD_DYLIB",
    }
    forbidden_path_commands = {
        0x00000006: "LC_LOADFVMLIB",
        0x00000007: "LC_IDFVMLIB",
        0x0000000E: "LC_LOAD_DYLINKER",
        0x0000000F: "LC_ID_DYLINKER",
        0x00000010: "LC_PREBOUND_DYLIB",
        0x00000012: "LC_SUB_FRAMEWORK",
        0x00000013: "LC_SUB_UMBRELLA",
        0x00000014: "LC_SUB_CLIENT",
        0x00000015: "LC_SUB_LIBRARY",
        0x00000027: "LC_DYLD_ENVIRONMENT",
    }
    for _index in range(command_count):
        if command_offset > command_end - 8:
            reject("DXMT WoW64 companion has a truncated load command")
        command, command_size = struct.unpack_from("<II", data, command_offset)
        if command_size < 8 or command_size % 8 or command_offset > command_end - command_size:
            reject("DXMT WoW64 companion has an invalid load-command size")
        if command == 0x0D:
            if command_size < 24:
                reject("DXMT WoW64 companion has a truncated LC_ID_DYLIB")
            string_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
            install_names.append(
                macho_string(data, command_offset, command_size, string_offset, "companion LC_ID_DYLIB")
            )
        elif command in dylib_commands:
            if command_size < 24:
                reject("DXMT WoW64 companion has a truncated dylib load command")
            string_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
            dylib_loads.append({
                "command": dylib_commands[command],
                "path": macho_string(
                    data, command_offset, command_size, string_offset,
                    "companion " + dylib_commands[command],
                ),
            })
        elif command == 0x8000001C:
            if command_size < 12:
                reject("DXMT WoW64 companion has a truncated LC_RPATH")
            string_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
            rpaths.append(
                macho_string(data, command_offset, command_size, string_offset, "companion LC_RPATH")
            )
        elif command == 0x00000032:
            if command_size < 24:
                reject("DXMT WoW64 companion has a truncated LC_BUILD_VERSION")
            platform, minimum_os, sdk, tool_count = struct.unpack_from("<IIII", data, command_offset + 8)
            if tool_count > (command_size - 24) // 8:
                reject("DXMT WoW64 companion has invalid LC_BUILD_VERSION tool bounds")
            build_versions.append((platform, macho_version(minimum_os), macho_version(sdk)))
        elif command == 0x1D:
            if command_size < 16:
                reject("DXMT WoW64 companion has a truncated LC_CODE_SIGNATURE")
            data_offset, data_size = struct.unpack_from("<II", data, command_offset + 8)
            if data_size == 0 or data_offset > len(data) - data_size:
                reject("DXMT WoW64 companion has invalid code-signature bounds")
            code_signatures.append((data_offset, data_size))
        elif command in forbidden_path_commands:
            reject("DXMT WoW64 companion contains a forbidden " + forbidden_path_commands[command])
        command_offset += command_size
    if command_offset != command_end or len(code_signatures) != 1:
        reject("DXMT WoW64 companion load-command or signature closure is invalid")
    if install_names != ["@rpath/winemetal-wow64.so"]:
        reject("DXMT WoW64 companion has an unexpected install name")
    if build_versions != [(1, COMPANION_MINIMUM_MACOS, COMPANION_SDK)]:
        reject("DXMT WoW64 companion has an unexpected platform, minOS, or SDK")
    if rpaths != COMPANION_RPATHS:
        reject("DXMT WoW64 companion has an unexpected LC_RPATH closure")
    if dylib_loads != COMPANION_LOAD_COMMANDS:
        reject("DXMT WoW64 companion has an unexpected dylib/framework load-command closure")
    signature_offset, signature_size = code_signatures[0]
    if signature_offset < command_end or signature_offset + signature_size != len(data):
        reject("DXMT WoW64 companion code signature is not the terminal file region")


def pe_rva_to_offset(data, pe_offset, rva, size):
    section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    section_table = pe_offset + 24 + optional_size
    if section_count == 0 or section_count > 96 or section_table > len(data) - section_count * 40:
        return None
    for index in range(section_count):
        section = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, section + 8)
        extent = max(virtual_size, raw_size)
        if rva >= virtual_address and size <= extent and rva - virtual_address <= extent - size:
            section_offset = rva - virtual_address
            if size > raw_size or section_offset > raw_size - size:
                continue
            offset = raw_offset + section_offset
            if raw_offset <= len(data) and offset <= len(data) - size:
                return offset
    return None


def is_arm64ec_image(data, pe_offset):
    optional = pe_offset + 24
    if optional > len(data) - 112 - 11 * 8:
        return False
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        return False
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    load_config_rva, load_config_size = struct.unpack_from("<II", data, optional + 112 + 10 * 8)
    if load_config_size < 0xD0:
        return False
    load_config = pe_rva_to_offset(data, pe_offset, load_config_rva, 0xD0)
    if load_config is None or struct.unpack_from("<I", data, load_config)[0] < 0xD0:
        return False
    metadata_address = struct.unpack_from("<Q", data, load_config + 0xC8)[0]
    if metadata_address < image_base:
        return False
    metadata = pe_rva_to_offset(data, pe_offset, metadata_address - image_base, 12)
    if metadata is None:
        return False
    version, code_map_rva, code_map_count = struct.unpack_from("<III", data, metadata)
    if version not in (1, 2) or code_map_count == 0 or code_map_count > 1024 * 1024:
        return False
    code_map = pe_rva_to_offset(data, pe_offset, code_map_rva, code_map_count * 8)
    if code_map is None:
        return False
    return any(struct.unpack_from("<I", data, code_map + index * 8)[0] & 1 for index in range(code_map_count))


def validate_pe(data, architecture):
    if len(data) < 0x40 or data[:2] != b"MZ":
        reject("DXMT PE module has no DOS header")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset > len(data) - 24 or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        reject("DXMT PE module has no valid PE header")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    characteristics = struct.unpack_from("<H", data, pe_offset + 22)[0]
    if optional_size < 2 or pe_offset + 24 + optional_size > len(data):
        reject("DXMT PE module has invalid optional-header bounds")
    optional_magic = struct.unpack_from("<H", data, pe_offset + 24)[0]
    if characteristics & 0x2002 != 0x2002:
        reject("DXMT PE input is not an executable DLL")
    arm64ec = machine == 0xA641 or (machine == 0x8664 and is_arm64ec_image(data, pe_offset))
    if architecture == "arm64ec":
        if optional_magic != 0x20B or not arm64ec:
            reject("DXMT aarch64-windows module is not ARM64EC")
    elif architecture == "x86_64":
        if machine != 0x8664 or optional_magic != 0x20B or arm64ec:
            reject("DXMT x86_64-windows module has the wrong PE machine")
    elif architecture == "i386":
        if machine != 0x014C or optional_magic != 0x10B:
            reject("DXMT i386-windows module has the wrong PE machine")
    else:
        reject("DXMT PE contract contains an unknown architecture")


def test_aba_barrier():
    # A SIGSTOP barrier lets the regression test perform an exact two-phase
    # live-path ABA swap around codesign.  It only changes timing and cannot
    # make a rejected snapshot pass validation.
    if os.environ.get("SWITCHYARD_DXMT_TEST_ABA_BARRIERS") == "1":
        os.kill(os.getpid(), signal.SIGSTOP)


def verify_codesign(runtime_root, relative, metadata, data):
    if len(data) <= 0 or len(data) > 64 * 1024 * 1024:
        reject("DXMT Unix module snapshot is outside its code-signing size bound")
    temporary_directory = tempfile.mkdtemp(prefix="switchyard-dxmt-codesign.", dir="/private/tmp")
    snapshot = os.path.join(temporary_directory, "winemetal.so")
    descriptor = None
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(snapshot, flags, 0o600)
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                reject("short write while materializing the DXMT code-signing snapshot")
            view = view[written:]
        os.fchmod(descriptor, 0o600)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = None
        result = subprocess.run(
            ["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", snapshot],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
            check=False,
        )
        test_aba_barrier()
    finally:
        if descriptor is not None:
            os.close(descriptor)
        try:
            os.unlink(snapshot)
        except FileNotFoundError:
            pass
        os.rmdir(temporary_directory)
    if result.returncode != 0:
        reject("DXMT Unix module does not have a valid embedded code signature")
    path = os.path.join(runtime_root, *relative.split("/"))
    after = os.lstat(path)
    if file_identity(metadata) != file_identity(after) or stat.S_ISLNK(after.st_mode):
        reject("DXMT Unix module changed while its code signature was verified")


def verify_companion(runtime_root, relative, metadata, data):
    temporary_directory = tempfile.mkdtemp(prefix="switchyard-dxmt-companion.", dir="/private/tmp")
    snapshot = os.path.join(temporary_directory, "winemetal-wow64.so")
    descriptor = None
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(snapshot, flags, 0o500)
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                reject("short write while materializing the companion inspection snapshot")
            view = view[written:]
        os.fchmod(descriptor, 0o500)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = None
        signature = subprocess.run(
            ["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", snapshot],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False,
        )
        slices = subprocess.run(
            ["/usr/bin/lipo", "-archs", snapshot],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=30, check=False,
        )
        symbols = subprocess.run(
            ["/usr/bin/nm", "-gjU", snapshot],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=30, check=False,
        )
        symbol_details = subprocess.run(
            ["/usr/bin/nm", "-nm", snapshot],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=30, check=False,
        )
    finally:
        if descriptor is not None:
            os.close(descriptor)
        try:
            os.unlink(snapshot)
        except FileNotFoundError:
            pass
        os.rmdir(temporary_directory)
    if signature.returncode:
        reject("DXMT WoW64 companion does not have a valid strict code signature")
    if slices.returncode or slices.stdout.strip() != "arm64":
        reject("DXMT WoW64 companion is not arm64-only")
    if symbols.returncode:
        reject("cannot inspect DXMT WoW64 companion exports")
    expected = {
        "___wine_unix_call_wow64_funcs",
        "___wine_unix_call_wow64_dispatch_v2",
        "___wine_unix_call_wow64_companion_v6",
    }
    exported = symbols.stdout.splitlines()
    if sorted(exported) != sorted(expected):
        reject("DXMT WoW64 companion export set is not exact")
    if symbol_details.returncode:
        reject("cannot inspect DXMT WoW64 companion export locations")
    symbol_addresses = {}
    for symbol in expected:
        matches = re.findall(
            r"^([0-9a-fA-F]{16}) \(__DATA_CONST,__const\) external "
            + re.escape(symbol) + r"$",
            symbol_details.stdout,
            re.MULTILINE,
        )
        if len(matches) != 1:
            reject("DXMT WoW64 companion export is not uniquely immutable: " + symbol)
        symbol_addresses[symbol] = int(matches[0], 16)

    descriptor_address = symbol_addresses["___wine_unix_call_wow64_companion_v6"]
    descriptor_offset = None
    command_count, command_bytes = struct.unpack_from("<II", data, 16)
    command_offset = 32
    command_end = 32 + command_bytes
    for _index in range(command_count):
        command, command_size = struct.unpack_from("<II", data, command_offset)
        if command == 0x19 and command_size >= 72:
            _name, vm_address, _vm_size, file_offset, file_size = struct.unpack_from(
                "<16sQQQQ", data, command_offset + 8
            )
            if (
                descriptor_address >= vm_address
                and descriptor_address - vm_address <= file_size
                and 72 <= file_size - (descriptor_address - vm_address)
            ):
                descriptor_offset = file_offset + descriptor_address - vm_address
                break
        command_offset += command_size
    if descriptor_offset is None or descriptor_offset > len(data) - 72:
        reject("DXMT WoW64 companion descriptor is outside its file-backed segment")
    version, size, entry_count, flags = struct.unpack_from("<IIII", data, descriptor_offset)
    if (version, size, entry_count, flags) != (6, 72, COMPANION_ENTRY_COUNT, 0):
        reject("DXMT WoW64 companion descriptor header is not exact")
    if data[descriptor_offset + 16:descriptor_offset + 48].hex() != COMPANION_SCHEMA_SHA256:
        reject("DXMT WoW64 companion descriptor ABI digest is not exact")
    path = os.path.join(runtime_root, *relative.split("/"))
    after = os.lstat(path)
    if file_identity(metadata) != file_identity(after) or stat.S_ISLNK(after.st_mode):
        reject("DXMT WoW64 companion changed while it was inspected")


def validate_manifest(tree, manifest_relative):
    manifest_data, _metadata = tree.read_file(manifest_relative, 1024 * 1024)
    try:
        manifest_text = manifest_data.decode("utf-8")
        value = json.loads(
            manifest_text,
            object_pairs_hook=object_without_duplicates,
            parse_constant=reject_nonstandard_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        reject("DXMT runtime manifest is not valid UTF-8 JSON: " + str(error))
    if type(value) is not dict:
        reject("DXMT runtime manifest root is not an object")
    if value.get("manifestVersion") != 2 or type(value.get("manifestVersion")) is not int:
        reject("DXMT validation requires runtime manifest version 2")
    if value.get("runtimeFamily") != "preview-native-arm64-fex":
        reject("DXMT validation only applies to preview-native-arm64-fex")
    if value.get("graphicsBackend") != "dxmt-metal":
        reject("native DXMT runtime must select graphicsBackend dxmt-metal")
    host = require_exact_type(value, "host", dict, "runtime manifest")
    expected_host = {
        "platform": "macos",
        "architecture": "arm64",
        "wineUnixArchitecture": "aarch64",
        "minimumMacOS": HOST_MINIMUM_MACOS,
        "requiresRosetta": False,
    }
    for key, expected in expected_host.items():
        if host.get(key) != expected or type(host.get(key)) is not type(expected):
            reject("native DXMT runtime host field is invalid: " + key)

    dxmt = require_exact_type(value, "dxmt", dict, "runtime manifest")
    require_exact_keys(
        dxmt,
        {"contractVersion", "implementation", "graphicsApi", "hostBackend", "provenance", "license", "sourceMaterials", "modules", "wow64Companion", "documents"},
        "runtime manifest.dxmt",
    )
    exact_scalars = {
        "contractVersion": 2,
        "implementation": "dxmt",
        "graphicsApi": "d3d11",
        "hostBackend": "metal",
        "license": LICENSE_EXPRESSION,
    }
    for key, expected in exact_scalars.items():
        if dxmt.get(key) != expected or type(dxmt.get(key)) is not type(expected):
            reject("runtime manifest.dxmt field is invalid: " + key)

    provenance = require_exact_type(dxmt, "provenance", dict, "runtime manifest.dxmt")
    expected_provenance = {
        "sourceRepository": SOURCE_REPOSITORY,
        "sourceBaseTree": SOURCE_BASE_TREE,
        "sourceRevision": SOURCE_REVISION,
        "sourceTree": SOURCE_TREE,
        "sourcePatch": SOURCE_PATCH_PATH,
        "sourcePatchSha256": SOURCE_PATCH_SHA256,
        "artifactBuildIdentity": ARTIFACT_BUILD_IDENTITY,
        "artifactName": ARTIFACT_NAME,
        "artifactSha256": ARTIFACT_SHA256,
        "packageWorkflow": PACKAGE_WORKFLOW,
        "packageWorkflowSha256": PACKAGE_WORKFLOW_SHA256,
        "packageBuild": PACKAGE_BUILD,
    }
    require_exact_keys(provenance, expected_provenance, "runtime manifest.dxmt.provenance")
    for key, expected in expected_provenance.items():
        if provenance.get(key) != expected or type(provenance.get(key)) is not str:
            reject("runtime manifest.dxmt.provenance field is invalid: " + key)

    source_materials = require_exact_type(dxmt, "sourceMaterials", list, "runtime manifest.dxmt")
    if len(source_materials) != len(SOURCE_MATERIALS):
        reject("runtime manifest.dxmt.sourceMaterials has an unexpected length")
    for item, (path, pinned_digest, material_type) in zip(source_materials, SOURCE_MATERIALS):
        require_exact_keys(item, {"path", "sha256", "type"}, "runtime manifest.dxmt.sourceMaterials item")
        if item != {"path": path, "sha256": pinned_digest, "type": material_type}:
            reject("runtime manifest.dxmt.sourceMaterials is not the exact ordered allowlist")
        data, _metadata = tree.read_file(path, 1024 * 1024)
        if sha256(data) != pinned_digest:
            reject("runtime DXMT source material differs from the pinned source: " + path)

    modules = require_exact_type(dxmt, "modules", list, "runtime manifest.dxmt")
    if len(modules) != len(MODULE_SOURCES):
        reject("runtime manifest.dxmt.modules has an unexpected length")
    host_data = None
    host_metadata = None
    for item, (path, source_digest, file_format, architecture) in zip(modules, MODULE_SOURCES):
        fields = {"path", "sha256", "sourceSha256", "format", "architecture"}
        if file_format == "mach-o-dylib":
            fields |= {"platform", "minimumMacOS", "sdk", "installName", "rpaths", "loadCommands"}
        require_exact_keys(item, fields, "runtime manifest.dxmt.modules item")
        for key in fields:
            if key in ("rpaths", "loadCommands"):
                continue
            if type(item.get(key)) is not str:
                reject("runtime manifest.dxmt.modules item has a non-string field: " + key)
        if item["path"] != path or item["sourceSha256"] != source_digest:
            reject("runtime manifest.dxmt.modules is not the pinned ordered source allowlist")
        if item["format"] != file_format or item["architecture"] != architecture:
            reject("runtime manifest.dxmt.modules format or architecture is invalid: " + path)
        if DIGEST_PATTERN.fullmatch(item["sha256"]) is None:
            reject("runtime manifest.dxmt.modules has a malformed final digest: " + path)
        if file_format == "pe-dll" and item["sha256"] != source_digest:
            reject("DXMT PE module differs from its pinned artifact input: " + path)
        if file_format == "mach-o-dylib":
            exact_macho = {
                "platform": "macos",
                "minimumMacOS": MACHO_MINIMUM_MACOS,
                "sdk": MACHO_SDK,
                "installName": "@rpath/winemetal.so",
            }
            for key, expected in exact_macho.items():
                if item[key] != expected:
                    reject("runtime manifest DXMT Mach-O metadata is invalid: " + key)
            if type(item["rpaths"]) is not list or item["rpaths"] != MACHO_RPATHS:
                reject("runtime manifest DXMT Mach-O rpaths are not the exact allowlist")
            if type(item["loadCommands"]) is not list:
                reject("runtime manifest DXMT Mach-O loadCommands is not a list")
            for command in item["loadCommands"]:
                require_exact_keys(command, {"command", "path"}, "runtime manifest DXMT Mach-O load command")
                if type(command["command"]) is not str or type(command["path"]) is not str:
                    reject("runtime manifest DXMT Mach-O load command has an invalid type")
            if item["loadCommands"] != MACHO_LOAD_COMMANDS:
                reject("runtime manifest DXMT Mach-O load commands are not the exact allowlist")

        data, metadata = tree.read_file(path, 64 * 1024 * 1024)
        if sha256(data) != item["sha256"]:
            reject("runtime manifest DXMT module digest mismatch: " + path)
        if file_format == "mach-o-dylib":
            validate_macho(data)
            host_data = data
            host_metadata = metadata
        else:
            validate_pe(data, architecture)

    companion = require_exact_type(dxmt, "wow64Companion", dict, "runtime manifest.dxmt")
    require_exact_keys(
        companion,
        {
            "path", "sha256", "format", "architecture", "minimumMacOS", "sdk",
            "installName", "rpaths", "loadCommands", "originalUnixLibrary",
            "originalSha256", "abiSchema", "abiSchemaSha256", "entryCount",
            "dispatchSourceVersion", "bindingVersion", "codeSignature",
        },
        "runtime manifest.dxmt.wow64Companion",
    )
    exact_companion = {
        "path": COMPANION_PATH,
        "format": "mach-o-dylib",
        "architecture": "arm64",
        "minimumMacOS": COMPANION_MINIMUM_MACOS,
        "sdk": COMPANION_SDK,
        "installName": "@rpath/winemetal-wow64.so",
        "rpaths": COMPANION_RPATHS,
        "loadCommands": COMPANION_LOAD_COMMANDS,
        "originalUnixLibrary": MODULE_SOURCES[0][0],
        "abiSchema": COMPANION_SCHEMA_PATH,
        "abiSchemaSha256": COMPANION_SCHEMA_SHA256,
        "entryCount": COMPANION_ENTRY_COUNT,
        "dispatchSourceVersion": 2,
        "bindingVersion": 4,
        "codeSignature": "strict",
    }
    for key, expected in exact_companion.items():
        if companion.get(key) != expected or type(companion.get(key)) is not type(expected):
            reject("runtime manifest DXMT WoW64 companion field is invalid: " + key)
    for key in ("sha256", "originalSha256"):
        if type(companion.get(key)) is not str or DIGEST_PATTERN.fullmatch(companion[key]) is None:
            reject("runtime manifest DXMT WoW64 companion digest is malformed: " + key)
    if companion["originalSha256"] != modules[0]["sha256"]:
        reject("DXMT WoW64 companion is not bound to the installed original Unix library")
    companion_data, companion_metadata = tree.read_file(COMPANION_PATH, 64 * 1024 * 1024)
    if sha256(companion_data) != companion["sha256"]:
        reject("runtime manifest DXMT WoW64 companion digest mismatch")
    validate_companion_macho(companion_data)

    documents = require_exact_type(dxmt, "documents", list, "runtime manifest.dxmt")
    if len(documents) != len(DOCUMENTS):
        reject("runtime manifest.dxmt.documents has an unexpected length")
    document_data = {}
    for item, (path, pinned_digest) in zip(documents, DOCUMENTS):
        require_exact_keys(item, {"path", "sha256"}, "runtime manifest.dxmt.documents item")
        if type(item.get("path")) is not str or type(item.get("sha256")) is not str:
            reject("runtime manifest.dxmt.documents item has an invalid type")
        if item["path"] != path or DIGEST_PATTERN.fullmatch(item["sha256"]) is None:
            reject("runtime manifest.dxmt.documents is not the exact ordered allowlist")
        data, _metadata = tree.read_file(path, 1024 * 1024)
        actual_digest = sha256(data)
        if item["sha256"] != actual_digest:
            reject("runtime manifest DXMT document digest mismatch: " + path)
        if pinned_digest is not None and actual_digest != pinned_digest:
            reject("runtime DXMT legal/provenance document differs from the pinned source: " + path)
        document_data[path] = data

    expected_files_manifest = "".join(
        f"{item['sha256']}  {item['path']}\n" for item in modules
    )
    expected_files_manifest += f"{companion['sha256']}  {COMPANION_PATH}\n"
    expected_files_manifest += f"{COMPANION_SCHEMA_SHA256}  {COMPANION_SCHEMA_PATH}\n"
    expected_files_manifest += f"{SOURCE_PATCH_SHA256}  {SOURCE_PATCH_PATH}\n"
    expected_files_manifest = expected_files_manifest.encode("ascii")
    files_manifest_path = DOCUMENTS[0][0]
    if document_data[files_manifest_path] != expected_files_manifest:
        reject("runtime DXMT files.sha256 is not the exact ordered module closure")

    tree.require_entries("lib/switchyard-dxmt", {"share"})
    tree.require_entries("lib/switchyard-dxmt/share", {"doc", "src"})
    tree.require_entries("lib/switchyard-dxmt/share/doc", {"switchyard-dxmt"})
    tree.require_entries(
        "lib/switchyard-dxmt/share/doc/switchyard-dxmt",
        {"files.sha256", "LICENSE", "COPYING.LIB", "CORRESPONDING-SOURCE.txt", "abi-schema-v6.txt"},
    )
    tree.require_entries("lib/switchyard-dxmt/share/src", {"switchyard-dxmt"})
    tree.require_entries("lib/switchyard-dxmt/share/src/switchyard-dxmt", {SOURCE_PATCH})
    tree.discover_owned_modules()

    host_path = MODULE_SOURCES[0][0]
    if host_data is None or host_metadata is None:
        reject("internal DXMT host-module policy is inconsistent")
    test_aba_barrier()
    verify_codesign(tree.root, host_path, host_metadata, host_data)
    verify_companion(tree.root, COMPANION_PATH, companion_metadata, companion_data)
    tree.validate_root_path()


def main():
    runtime_root, runtime_manifest = sys.argv[1:]
    expected_manifest = os.path.join(runtime_root, "switchyard-runtime.json")
    if runtime_manifest != expected_manifest:
        reject("DXMT manifest must be the runtime's canonical switchyard-runtime.json")
    tree = RuntimeTree(runtime_root)
    try:
        validate_manifest(tree, "switchyard-runtime.json")
    finally:
        tree.close()


try:
    main()
except ValidationError as error:
    print("DXMT artifact validation failed: " + str(error), file=sys.stderr)
    raise SystemExit(1)
except (OSError, ValueError, RecursionError, struct.error, subprocess.SubprocessError) as error:
    print("DXMT artifact validation failed safely: " + str(error), file=sys.stderr)
    raise SystemExit(1)
PY
}
