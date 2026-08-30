#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM_BASE_FILE="$ROOT_DIR/switchyard/upstream-base.txt"
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "source verification failed: $*" >&2
  exit 1
}

if [ ! -f "$UPSTREAM_BASE_FILE" ]; then
  fail "missing $UPSTREAM_BASE_FILE"
fi

upstream_base="$(tr -d '[:space:]' < "$UPSTREAM_BASE_FILE")"
git -C "$ROOT_DIR" cat-file -e "${upstream_base}^{commit}" 2>/dev/null ||
  fail "upstream base $upstream_base is unavailable; fetch enough history to include it"
git -C "$ROOT_DIR" merge-base --is-ancestor "$upstream_base" HEAD ||
  fail "HEAD is not descended from upstream base $upstream_base"

check_source_whitespace() {
  # This DXMT source patch is a hash-pinned corresponding-source material for a
  # closed runtime artifact.  Its patch payload may need to preserve upstream
  # whitespace exactly, so the DXMT artifact/source-material validators own that
  # file's byte-for-byte contract instead of the generic source whitespace gate.
  git -C "$ROOT_DIR" diff --check "$@" -- . \
    ':(exclude)switchyard/patches/0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch'
}

check_source_whitespace "$upstream_base"..HEAD
check_source_whitespace
check_source_whitespace --cached

added_files="$({
  git -C "$ROOT_DIR" log --format= --name-only --diff-filter=A "$upstream_base"..HEAD
  git -C "$ROOT_DIR" diff --name-only --diff-filter=A "$upstream_base"..HEAD
  git -C "$ROOT_DIR" diff --name-only --diff-filter=A
  git -C "$ROOT_DIR" diff --cached --name-only --diff-filter=A
  git -C "$ROOT_DIR" ls-files --others --exclude-standard
} | LC_ALL=C sort -u)"
# These source-only smoke tests exercise an external runtime interface and do not
# contain or distribute toolkit artifacts.
toolkit_candidate_files="$(printf '%s\n' "$added_files" |
  grep -Ev '^switchyard/tests/d3dmetal_(d3d12|dxgi_resource)_smoke(\.c|_test\.sh)$' || true)"
if printf '%s\n' "$toolkit_candidate_files" | grep -Eiq '(^|/)(Game[[:space:]_-]*Porting[[:space:]_-]*Toolkit|GPTK)(/|$)|d3dmetal|libd3dshared|metalirconverter'; then
  fail "tracked file names suggest proprietary Apple Game Porting Toolkit content"
fi
if printf '%s\n' "$added_files" | grep -Eiq '\.(dmg|pkg|metallib|dylib)$'; then
  fail "unexpected prebuilt runtime artifact added to source history"
fi

"$ROOT_DIR/switchyard/verify_font_assets.sh"
"$ROOT_DIR/switchyard/verify_tls_packages.sh"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/ntdll/tests/check_arm64ec_low_guest_dispatch.py" \
  "$ROOT_DIR/dlls/ntdll/signal_arm64ec.c" \
  "$ROOT_DIR/include/wine/low_va.h"
"$ROOT_DIR/dlls/ntdll/tests/run_arm64ec_low_guest_decode.sh"
"$ROOT_DIR/dlls/ntdll/tests/run_arm64ec_emulation_dispatch.sh"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/ntdll/tests/check_arm64ec_emulation_dispatch.py" \
  "$ROOT_DIR/dlls/ntdll/unix/signal_arm64.c" \
  "$ROOT_DIR/dlls/ntdll/signal_arm64ec.c" \
  "$ROOT_DIR/dlls/ntdll/unwind.h" \
  "$ROOT_DIR/dlls/xtajit64/cpu.c"
"$ROOT_DIR/dlls/xtajit64/provider_tests/run_tb_history.sh"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/xtajit64/provider_tests/check_tb_history.py" \
  "$ROOT_DIR/dlls/xtajit64/unixlib.c"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/xtajit64/provider_tests/check_trap_diagnostics.py" \
  "$ROOT_DIR/dlls/xtajit64/unixlib.c"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/ntdll/tests/check_arm64ec_low_guest_access.py" \
  "$ROOT_DIR/dlls/ntdll/unix/signal_arm64.c" \
  "$ROOT_DIR/dlls/ntdll/unix/virtual.c"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/ntdll/tests/check_arm64ec_low_instruction_cache.py" \
  "$ROOT_DIR/dlls/ntdll/unix/virtual.c"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/ntdll/tests/check_arm64ec_low_memory_v1.py" \
  "$ROOT_DIR/include/wine/low_va.h" \
  "$ROOT_DIR/dlls/ntdll/unix/virtual.c" \
  "$ROOT_DIR/server/mapping.c"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
  "$ROOT_DIR/dlls/ntdll/tests/check_creation_transactions.py" \
  "$ROOT_DIR/server/process.c" \
  "$ROOT_DIR/server/thread.c"
"$ROOT_DIR/switchyard/tests/build_runtime_failure_propagation_test.sh"
"$ROOT_DIR/switchyard/tests/native_macos_sdk_policy_test.sh"
"$ROOT_DIR/switchyard/tests/native_dependency_signing_test.sh"
"$ROOT_DIR/switchyard/tests/native_font_runtime_preparation_test.sh"
"$ROOT_DIR/switchyard/tests/macho_tree_validation_test.sh"
"$ROOT_DIR/switchyard/tests/provider_source_models_test.sh"
"$ROOT_DIR/switchyard/tests/arm64x_leaf_thunk_patch_test.sh"
"$ROOT_DIR/switchyard/tests/darwin_arm64_x18_availability_source_test.sh"
"$ROOT_DIR/switchyard/tests/darwin_arm64_private_valloc_wx_source_test.sh"
"$ROOT_DIR/switchyard/tests/swdbg_experiment_test.sh"

echo "source history verified from $upstream_base through $(git -C "$ROOT_DIR" rev-parse HEAD)"
