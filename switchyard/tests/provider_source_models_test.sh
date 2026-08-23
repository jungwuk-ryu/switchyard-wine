#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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

grep -Fqx 'UNIX_CFLAGS = $(UNICORN_CFLAGS) $(XTAJIT64_PE_CFLAGS)' \
    "$ROOT_DIR/dlls/xtajit64/Makefile.in" ||
    fail "xtajit64 Unixlib does not inherit the configured Unicorn capability"

if grep -Eq 'arm64ec_owned_backing|WINE_ARM64EC_MEMORY_OBSERVER_V2' \
    "$ROOT_DIR/dlls/xtajit64/cpu.c" \
    "$ROOT_DIR/dlls/xtajit64/unixlib.c" \
    "$ROOT_DIR/dlls/xtajit64/unixlib.h"; then
    fail "xtajit64 still references a retired fixed-low ownership protocol"
fi

echo "provider source-only models verified"
