#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/check_thread_term_ownership.py" \
    "$ROOT_DIR/dlls/xtajit64/cpu.c"

PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/check_control_stack.py" \
    "$ROOT_DIR/dlls/xtajit64/cpu.c"

PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/check_mapping_snapshot.py" \
    "$ROOT_DIR/dlls/xtajit64/cpu.c"

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/provider-source-models.XXXXXX")"
trap 'rm -rf -- "$TEST_ROOT"' EXIT

fail()
{
    echo "provider source models test: $*" >&2
    exit 1
}

cc_line=${CC:-cc}
read -r -a cc_words <<<"$cc_line"
[[ ${#cc_words[@]} -gt 0 ]] || fail "host compiler is empty"

common_flags=(
    -std=c11 -Wall -Werror -Wdeclaration-after-statement -Wempty-body
    -Wignored-qualifiers -Winit-self -Wpointer-arith -Wstrict-prototypes
    -Wtype-limits -Wunused-but-set-parameter -Wvla -Wwrite-strings
)

for standalone_fixture in \
    "$ROOT_DIR/dlls/xtajit/provider_tests/hook_performance.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/fixed_low.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/fixed_low_import.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/unaligned_tso_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/exception_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/seh_unwind_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/seh_unwind_native_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/suspend_context_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/control_stack_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/control_stack_stress_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/thread_lifecycle_pe.c" \
    "$ROOT_DIR/dlls/ntdll/tests/arm64ec_exception_unwind_pe.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/ec_entry_cache.c" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/fixed_low.spec" \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/fixed_low_import.spec"; do
    grep -Eq '^#[[:space:]]*pragma[[:space:]]+makedep[[:space:]]+standalone([[:space:]]|$)' \
        "$standalone_fixture" ||
        fail "manual provider fixture is not excluded from generated sources: $standalone_fixture"
done

/usr/bin/python3 -I - "$ROOT_DIR/tools/make_makefiles" <<'PY'
import sys

text = open(sys.argv[1], encoding="utf-8").read()
start = text.index(r'elsif ($name =~ /\.spec$/)')
end = text.index(r'elsif ($name =~ /\.nls$/)', start)
block = text[start:end]
reader = "my %flags = get_makedep_flags($file);"
guard = "next if defined $flags{standalone};"
if reader not in block or guard not in block or block.index(reader) > block.index(guard):
    raise SystemExit("make_makefiles does not honor standalone .spec fixtures")
PY

"${cc_words[@]}" "${common_flags[@]}" \
    -I"$ROOT_DIR/dlls/xtajit" \
    "$ROOT_DIR/dlls/xtajit/provider_tests/context_generation.c" \
    -o "$TEST_ROOT/xtajit-context-generation"
"$TEST_ROOT/xtajit-context-generation"

PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/check_x64_entry_gate.py" \
    "$ROOT_DIR/dlls/xtajit64/cpu.c"

PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/dlls/xtajit64/provider_tests/check_ec_entry_cache.py" \
    "$ROOT_DIR/dlls/xtajit64/cpu.c"

PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/dlls/ntdll/tests/check_arm64ec_exception_stack.py" \
    "$ROOT_DIR/dlls/ntdll/unix/signal_arm64.c"

grep -Fqx 'UNIX_CFLAGS = $(SWITCHYARD_FEX_CFLAGS) $(XTAJIT64_PE_CFLAGS)' \
    "$ROOT_DIR/dlls/xtajit64/Makefile.in" ||
    fail "xtajit64 Unixlib does not inherit the configured FEX capability"
grep -Fqx 'UNIX_LIBS   = $(SWITCHYARD_FEX_LIBS)' \
    "$ROOT_DIR/dlls/xtajit64/Makefile.in" ||
    fail "xtajit64 Unixlib does not link the configured FEX provider"
grep -Eq '^[[:space:]]*unixlib_fex[.]c$' \
    "$ROOT_DIR/dlls/xtajit64/Makefile.in" ||
    fail "xtajit64 does not compile the FEX implementation"
if grep -Eq 'UNICORN|^[[:space:]]*unixlib[.]c([[:space:]\\]|$)' \
    "$ROOT_DIR/dlls/xtajit64/Makefile.in"; then
    fail "xtajit64 still selects the retired Unicorn implementation"
fi

if grep -Eq 'arm64ec_owned_backing|WINE_ARM64EC_MEMORY_OBSERVER_V2' \
    "$ROOT_DIR/dlls/xtajit64/cpu.c" \
    "$ROOT_DIR/dlls/xtajit64/unixlib_fex.c" \
    "$ROOT_DIR/dlls/xtajit64/unixlib.h"; then
    fail "xtajit64 still references a retired fixed-low ownership protocol"
fi

echo "provider source-only models verified"
