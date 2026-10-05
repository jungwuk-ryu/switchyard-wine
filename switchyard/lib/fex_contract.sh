#!/usr/bin/env bash
# shellcheck disable=SC2034 # Shared, read-only SDK and native runtime policy.

# This is the compiled Switchyard-owned Darwin port, not a Linux FEX package.
# Keep the SDK ABI separate from Wine's PE/Unix process-boundary ABI.
SWITCHYARD_FEX_VERSION="6"
SWITCHYARD_FEX_SOURCE_REPOSITORY="https://github.com/FEX-Emu/FEX.git"
SWITCHYARD_FEX_SOURCE_REVISION="73dc3b3eddaf72745c743fe8ec76ed66f87636dc"
SWITCHYARD_FEX_SOURCE_PATCH_BASENAME="fex-73dc3b3-darwin-core.patch"
SWITCHYARD_FEX_SOURCE_PATCH_SHA256="8afdc678bc873f0e2fc5e51b5e0dca2487a71244e9392615d35142223f493f2e"
SWITCHYARD_FEX_SOURCE_DEPS_SHA256="b31cd5e74e5e0e33fe64bdf8d935bf73ed4d513e95f2e43ddeb7ebff91764271"
SWITCHYARD_FEX_BUILD_CONTRACT_VERSION="6"
SWITCHYARD_FEX_ADAPTER_SHA256="8cdfddd69944d28974f10dc27909e11eabd41626c3bffc4edb594264b11c9a52"
SWITCHYARD_FEX_LIBRARY_SHA256="1fb4df450ee9f376715ca010bd780015fac6be62220356baf9cc803667492d1c"
SWITCHYARD_FEX_DEVELOPMENT_CACHE_DIGEST="0450775dee4986112f4877450d5e9bbc10921f99e94f7f7ae421606e353a5b59"
# Excludes only the signed library, nested JSON and content marker. The
# source, headers, notices, paths, modes and alias targets remain pinned when
# a Developer-ID signature changes the final library bytes.
SWITCHYARD_FEX_IMMUTABLE_PAYLOAD_DIGEST="328e889d2161a77e21b2f7dca5dc320f017ae18004a1f00bc5b93b01c840a8e9"
SWITCHYARD_FEX_TOOLCHAIN_SHA256="ff120be2bb2db74e795c5a00cbf496e22ef30ea057d8fa872c148d1ec10b9b28"
SWITCHYARD_FEX_PROVIDER_IDENTITY="switchyard-fex-provider-abi-v6-darwin-low-shadow-external-stops-signal-repair-stack-query-exec-range-detached-shared-admission-register-window"
SWITCHYARD_FEX_MINIMUM_MACOS="26.5"

SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY="lib/wine/aarch64-unix/xtajit64.so"
SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY="lib/wine/aarch64-windows/xtajit64.dll"
SWITCHYARD_NATIVE_XTAJIT64_ABI_VERSION="19"
SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY="switchyard-xtajit64-fex-provider-abi-v19-flight-bind-process-init-104-begin-472-doorbell-reconstruct-1248-jit-signal-stack-query-exec-range-dispatch-512-direct-64-native-stack-protect"
SWITCHYARD_NATIVE_FEX_ROOT="lib/switchyard-fex"
SWITCHYARD_NATIVE_FEX_LIBRARY="$SWITCHYARD_NATIVE_FEX_ROOT/lib/libswitchyard-fex.6.0.0.dylib"
SWITCHYARD_NATIVE_FEX_INSTALL_NAME='@rpath/libswitchyard-fex.6.dylib'
SWITCHYARD_NATIVE_FEX_RPATH='@loader_path/../../switchyard-fex/lib'
SWITCHYARD_NATIVE_FEX_SOURCE_PATCH="$SWITCHYARD_NATIVE_FEX_ROOT/share/src/switchyard-fex/$SWITCHYARD_FEX_SOURCE_PATCH_BASENAME"

# Only the x64 guest path is qualified. The Linux core's 32-bit capability
# does not provide Wine i386 dispatch on Darwin; no TCG provider is packaged.
SWITCHYARD_NATIVE_FEX_PROVIDER_UNIXLIBS=("$SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY")
SWITCHYARD_NATIVE_FEX_PROVIDER_PE_LIBS=("$SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY")
SWITCHYARD_NATIVE_FEX_PROVIDER_GUEST_ARCHS=("x86_64")

switchyard_validate_fex_development_sdk() {
  [ "$#" -eq 1 ] || return 2
  local root="$1" library helper library_dir

  [ -d "$root" ] && [ ! -L "$root" ] || return 1
  library_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
  helper="$(cd "$library_dir/.." && pwd -P)/runtime_content_digest.py"
  [ -f "$helper" ] && [ ! -L "$helper" ] || return 1
  # The complete SDK pin covers every file, directory mode and alias target,
  # including its source/test snapshot, export list and nested manifest.
  /usr/bin/python3 -I "$helper" verify "$root" \
    "$SWITCHYARD_FEX_DEVELOPMENT_CACHE_DIGEST" >/dev/null || return 1
  library="$root/lib/libswitchyard-fex.6.0.0.dylib"
  [ -f "$library" ] && [ ! -L "$library" ] || return 1
  [ "$(/usr/bin/lipo -archs "$library")" = arm64 ] || return 1
  /usr/bin/codesign --verify --strict "$library" >/dev/null 2>&1
}

switchyard_native_configured_fex_policy_is_exact() {
  [ "$#" -eq 3 ] || return 2
  # Read configuration as bounded data, never source or execute a cached
  # config.status. Both its reconfigure arguments and the effective make
  # assignments must agree; a legacy i386/TCG tree is not a reusable build.
  /usr/bin/python3 -I - "$1" "$2" "$3" <<'PY'
import os
import re
import shlex
import stat
import sys

makefile, statusfile, sdk = sys.argv[1:]
def read_regular(path, maximum):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ValueError("configuration is not a bounded regular file")
        data = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
        identity = lambda info: (info.st_dev, info.st_ino, info.st_size,
                                 info.st_mtime_ns, info.st_ctime_ns)
        if identity(before) != identity(after) or len(data) != before.st_size:
            raise ValueError("configuration changed during inspection")
        return data.decode("utf-8")

try:
    if not sdk.startswith("/") or "\n" in sdk or "\r" in sdk:
        raise ValueError("SDK path is not absolute single-line data")
    expected = {
        "PE_ARCHS": "aarch64 arm64ec x86_64",
        "SWITCHYARD_FEX_CFLAGS": "-I" + sdk + "/include",
        "SWITCHYARD_FEX_LIBS": "-L" + sdk + "/lib -lswitchyard-fex",
        "XTAJIT64_UNIXLIB": "xtajit64.so",
        "XTAJIT64_PE_CFLAGS": "-DHAVE_SWITCHYARD_FEX",
        "XTAJIT_UNIXLIB": "",
        "XTAJIT_PE_CFLAGS": "",
        "WINEMETAL_WOW64_UNIXLIB": "winemetal-wow64.so",
    }
    # A complete three-architecture Wine Makefile is about 71 MB. Keep a
    # separate bounded allowance for generated rules; config.status is tiny.
    status = read_regular(statusfile, 16 * 1024 * 1024)
    effective = read_regular(makefile, 128 * 1024 * 1024)
    if re.search(r"(?m)^dlls/xtajit/(?:aarch64-windows/xtajit\.dll|xtajit\.so):", effective):
        raise ValueError("configuration still builds the retired i386 provider")
    for text in (effective, status):
        assignments = {name: [] for name in expected}
        for line in text.splitlines():
            match = re.fullmatch(r"([A-Z_0-9]+)[ \t]*=[ \t]*(.*)", line)
            if match and match[1] in assignments:
                assignments[match[1]].append(match[2].strip())
        for name, value in expected.items():
            actual = assignments[name]
            if name == "PE_ARCHS":
                actual = [" ".join(item.split()) for item in actual]
            if actual != [value]:
                raise ValueError("configuration does not have one exact " + name)
    lines = [line for line in status.splitlines()
             if line.startswith("ac_cs_config=")]
    if len(lines) != 1:
        raise ValueError("reconfigure arguments are ambiguous")
    outer = shlex.split(lines[0].partition("=")[2])
    if len(outer) != 1:
        raise ValueError("reconfigure arguments are malformed")
    arguments = shlex.split(outer[0])
    i386_options = [item for item in arguments if item == "--disable-xtajit" or
                    item.startswith(("--disable-xtajit=", "--enable-xtajit"))]
    if i386_options != ["--disable-xtajit"]:
        raise ValueError("reconfigure arguments do not disable the retired i386 provider")
    for option, wanted in (("--enable-archs=", "aarch64,arm64ec,x86_64"),
                           ("--with-fex=", sdk)):
        if [item for item in arguments if item.startswith(option)] != [option + wanted]:
            raise ValueError("reconfigure arguments do not bind exact " + option)
    if any(item.startswith(("--with-unicorn", "--without-fex")) for item in arguments):
        raise ValueError("reconfigure arguments retain a retired provider")
except (OSError, UnicodeError, ValueError) as error:
    print("Native FEX configure policy rejected: " + str(error), file=sys.stderr)
    raise SystemExit(1)
PY
}
