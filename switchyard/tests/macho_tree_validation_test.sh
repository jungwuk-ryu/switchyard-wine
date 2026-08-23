#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"

fail()
{
    echo "Mach-O tree validation test: $*" >&2
    exit 1
}

if [[ "$(/usr/bin/uname -s)" != Darwin || "$(/usr/bin/uname -m)" != arm64 ]]; then
    echo "Mach-O tree validation test skipped outside native macOS ARM64"
    exit 0
fi

TEST_ROOT="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/macho-tree-validation.XXXXXX")"
trap 'rm -rf -- "$TEST_ROOT"' EXIT

FUNCTION_FILE="$TEST_ROOT/verify-runtime-relative-macho-tree.sh"
/usr/bin/python3 -I - "$BUILD_SCRIPT" "$FUNCTION_FILE" <<'PY'
import pathlib
import re
import sys

source_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
source = source_path.read_text(encoding="utf-8")
declaration = re.compile(
    r"^verify_runtime_relative_macho_tree\(\) \{\n", re.MULTILINE
)
matches = list(declaration.finditer(source))
if len(matches) != 1:
    raise SystemExit(
        f"expected one verify_runtime_relative_macho_tree definition, found {len(matches)}"
    )
start = matches[0].start()
next_declaration = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_]*\(\) \{\n", re.MULTILINE
).search(source, matches[0].end())
if next_declaration is None:
    raise SystemExit("could not find the function following Mach-O tree validation")
function_source = source[start : next_declaration.start()]
if not function_source.rstrip().endswith("}"):
    raise SystemExit("extracted Mach-O tree validator is incomplete")
output_path.write_text(function_source, encoding="utf-8")
PY

run_with_watchdog()
{
    local tree_root="$1"
    local label="$2"
    local stdout_file="$3"
    local stderr_file="$4"

    /usr/bin/python3 -I - \
        "$FUNCTION_FILE" "$tree_root" "$label" "$stdout_file" "$stderr_file" <<'PY'
import os
import signal
import subprocess
import sys

function_file, tree_root, label, stdout_path, stderr_path = sys.argv[1:]
runner = r'''
set -euo pipefail
function_file=$1
tree_root=$2
label=$3
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
NATIVE_CPU_PROVIDER_ENABLED=1
source "$function_file"
ulimit -n 256
validation_output="$(verify_runtime_relative_macho_tree "$tree_root" "$label")"
test -z "$validation_output"
'''

with open(stdout_path, "wb") as stdout_file, open(stderr_path, "wb") as stderr_file:
    process = subprocess.Popen(
        ["/bin/bash", "-c", runner, "macho-tree-validation", function_file,
         tree_root, label],
        stdout=stdout_file,
        stderr=stderr_file,
        start_new_session=True,
    )
    try:
        status = process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        print("Mach-O validation watchdog expired after 20 seconds", file=sys.stderr)
        status = 124
raise SystemExit(status)
PY
}

tiny_macho="$TEST_ROOT/tiny-macho"
printf '%s\n' 'int main(void) { return 0; }' |
    /usr/bin/clang -arch arm64 -Wl,-headerpad_max_install_names \
        -Wl,-rpath,@loader_path/runtime -x c - -o "$tiny_macho"
/usr/bin/file -b "$tiny_macho" | /usr/bin/grep -q 'Mach-O.*arm64' ||
    fail "compiler did not produce an ARM64 Mach-O fixture"

valid_root="$TEST_ROOT/valid"
/bin/mkdir -p "$valid_root"
for ((index = 0; index < 140; ++index)); do
    /bin/ln "$tiny_macho" "$valid_root/tiny-$index"
done

valid_stdout="$TEST_ROOT/valid.stdout"
valid_stderr="$TEST_ROOT/valid.stderr"
if ! run_with_watchdog "$valid_root" "valid fd-pressure tree" \
        "$valid_stdout" "$valid_stderr"; then
    /bin/cat "$valid_stderr" >&2
    fail "valid tree failed under the 256-descriptor ceiling"
fi
[[ ! -s "$valid_stdout" ]] || fail "valid tree validation produced unexpected output"
if [[ -s "$valid_stderr" ]]; then
    /bin/cat "$valid_stderr" >&2
    fail "valid tree validation produced unexpected diagnostics"
fi

absolute_root="$TEST_ROOT/absolute-rpath"
/bin/mkdir -p "$absolute_root"
/bin/cp "$tiny_macho" "$absolute_root/tiny"
/usr/bin/install_name_tool -add_rpath /private/tmp/switchyard-macho-outside \
    "$absolute_root/tiny"
absolute_stdout="$TEST_ROOT/absolute.stdout"
absolute_stderr="$TEST_ROOT/absolute.stderr"
if run_with_watchdog "$absolute_root" "absolute rpath tree" \
        "$absolute_stdout" "$absolute_stderr"; then
    fail "absolute Mach-O rpath was accepted"
fi
/usr/bin/grep -Fq \
    'absolute rpath tree retains a non-runtime rpath:' "$absolute_stderr" || {
    /bin/cat "$absolute_stderr" >&2
    fail "absolute Mach-O rpath did not produce the expected diagnostic"
}

traversal_root="$TEST_ROOT/traversing-dependency"
/bin/mkdir -p "$traversal_root"
/bin/cp "$tiny_macho" "$traversal_root/tiny"
/usr/bin/install_name_tool -change /usr/lib/libSystem.B.dylib \
    '@loader_path/../escape.dylib' "$traversal_root/tiny"
traversal_stdout="$TEST_ROOT/traversal.stdout"
traversal_stderr="$TEST_ROOT/traversal.stderr"
if run_with_watchdog "$traversal_root" "traversing dependency tree" \
        "$traversal_stdout" "$traversal_stderr"; then
    fail "traversing Mach-O dependency was accepted"
fi
/usr/bin/grep -Fq \
    'traversing dependency tree contains a traversing Mach-O dependency:' \
    "$traversal_stderr" || {
    /bin/cat "$traversal_stderr" >&2
    fail "traversing Mach-O dependency did not produce the expected diagnostic"
}

echo "Mach-O tree validation descriptor and path checks verified"
