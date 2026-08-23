#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

fail()
{
    echo "Darwin ARM64 x18 availability source test: $*" >&2
    exit 1
}

/bin/sh -n "$ROOT_DIR/configure" || fail "generated configure script is invalid"

/usr/bin/python3 - "$ROOT_DIR" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])


def read(relative: str) -> str:
    path = root / relative
    if not path.is_file():
        raise SystemExit(f"Darwin ARM64 x18 availability source test: missing {path}")
    return path.read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"Darwin ARM64 x18 availability source test: {message}")


configure_ac = read("configure.ac")
configure = read("configure")
config_h = read("include/config.h.in")
signal = read("dlls/ntdll/unix/signal_arm64.c")
server = read("dlls/ntdll/unix/server.c")
unix_private = read("dlls/ntdll/unix/unix_private.h")
helper = read("dlls/ntdll/tests/x18_dispatch_unix.m")
exception = read("dlls/ntdll/tests/exception.c")

for text, name in ((configure_ac, "configure.ac"), (configure, "configure")):
    for token in (
        "os/arch/arm64.h",
        "wine_cv_have_os_custom_x18_abi",
        "os_custom_x18_abi_enabled",
        "os_set_custom_x18_abi_enabled",
        "HAVE_OS_CUSTOM_X18_ABI",
    ):
        require(token in text, f"{name} is missing {token}")
    require(
        re.search(r"else\s+wine_cv_have_os_custom_x18_abi=no\s+fi", text) is not None,
        f"{name} must disable custom x18 support when the SDK header is absent",
    )

require(
    "AC_LINK_IFELSE" in configure_ac
    and "__builtin_available(macOS 26.4, *)" in configure_ac,
    "configure.ac must link both custom-x18 symbols behind the 26.4 gate",
)
require(
    'ac_fn_c_check_header_compile "$LINENO" "os/arch/arm64.h"' in configure
    and 'printf \'%s\\n\' "#define HAVE_OS_CUSTOM_X18_ABI 1"' in configure,
    "generated configure is out of sync with the x18 checks",
)
for macro in ("HAVE_OS_ARCH_ARM64_H", "HAVE_OS_CUSTOM_X18_ABI"):
    require(f"#undef {macro}" in config_h, f"config.h.in is missing {macro}")

require(
    re.search(
        r"#ifdef HAVE_OS_CUSTOM_X18_ABI\s*# include <os/arch/arm64\.h>\s*#endif",
        signal,
    ) is not None,
    "signal_arm64.c must guard the SDK header with the configure result",
)
compat_start = signal.find("#if defined(HAVE_OS_CUSTOM_X18_ABI) && \\")
compat_end = signal.find("#define NTDLL_DWARF_H_NO_UNWINDER", compat_start)
require(compat_start >= 0 and compat_end > compat_start, "could not isolate x18 compatibility code")
compat = signal[compat_start:compat_end]
remainder = signal[compat_end:]
require("__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260400" in compat,
        "the deployment-target fast path is missing")
require(
    re.search(
        r"if \(!custom_x18_abi_enabled\(\)\)\s*\{\s*"
        r"set_custom_x18_abi_enabled\( TRUE \);\s*"
        r"if \(!custom_x18_abi_enabled\(\)\) return FALSE;\s*\}\s*"
        r"set_custom_x18_abi_enabled\( FALSE \);\s*"
        r"return !custom_x18_abi_enabled\(\);",
        compat,
    ) is not None,
    "startup must prove that the task can enter and leave custom-x18 mode",
)
for symbol in ("os_custom_x18_abi_enabled", "os_set_custom_x18_abi_enabled"):
    require(symbol not in remainder, f"production call to {symbol} bypasses the capability wrapper")

init_start = signal.find("BOOL signal_init_process( TEB *teb )")
init_end = signal.find("void syscall_dispatcher_return_slowpath", init_start)
process_init = signal[init_start:init_end]
require(
    0 <= process_init.find("if (!init_custom_x18_abi()) return FALSE;")
       < process_init.find("alloc_syscall_frame(")
       < process_init.find("sigaction("),
    "runtime capability must fail closed before installing execution state",
)
require("extern BOOL signal_init_process( TEB *teb );" in unix_private,
        "the signal initializer must return its capability status")
require(
    re.search(r"if \(!signal_init_process\( data->teb \)\)\s*fatal_error\(", server) is not None,
    "process startup must stop before PE entry when custom-x18 is unavailable",
)

for function in (
    "enter_system_x18_abi",
    "enter_windows_x18_abi",
    "prepare_unix_dispatcher_entry",
    "dispatch_signal_with_system_x18",
):
    require(function in signal, f"missing {function} transition")
require("CONTEXT_FULL | CONTEXT_ARM64_X18" in signal,
        "initial thread context must publish x18")
require("needed_flags & CONTEXT_ARM64_X18" in signal,
        "thread context operations must preserve explicit x18 requests")

feature_guard = "#if defined(__APPLE__) && defined(__aarch64__) && defined(HAVE_OS_CUSTOM_X18_ABI)"
require(helper.count(feature_guard) >= 2,
        "the native test helper must use the configure feature guard")
require("__ASM_GLOBAL_FUNC( x18_dispatch_direct_bridge_impl," in helper,
        "the direct system/custom-x18 bridge is missing")
require("x18_dispatch_illegal_instruction_func" in helper,
        "the signal-escape x18 target is missing")
require(
    re.search(r"status = bridge\( &state \);\s*if \(status == STATUS_NOT_SUPPORTED\)", exception),
    "the PE regression must skip cleanly when the runtime API is unavailable",
)
for field in (
    "observed_opaque_x18",
    "system_target_custom",
    "observed_system_x18",
    "observed_teb_x18",
    "final_custom",
    "inner_called",
):
    require(field in exception, f"the PE regression does not assert {field}")
PY

echo "Darwin ARM64 custom-x18 availability contract verified"
