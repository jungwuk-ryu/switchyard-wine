#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"
TEST_PARENT="${TMPDIR:-/tmp}"
TEST_ROOT="$(mktemp -d "$TEST_PARENT/switchyard-build-failure-propagation.XXXXXX")"
chmod 0700 "$TEST_ROOT"

cleanup() {
  case "${TEST_ROOT:-}" in
    "$TEST_PARENT"/switchyard-build-failure-propagation.*)
      rm -rf -- "$TEST_ROOT"
      ;;
    *)
      echo "refusing to remove unexpected test path: ${TEST_ROOT:-<empty>}" >&2
      ;;
  esac
}
trap cleanup EXIT

fail() {
  echo "$1" >&2
  exit 1
}

bash -n "$BUILD_SCRIPT"

helper_definition="$(sed -n '/^capture_required_output()/,/^}/p' "$BUILD_SCRIPT")"
[ -n "$helper_definition" ] || fail "could not load the required-output capture helper"
eval "$helper_definition"

if ! /usr/bin/python3 - "$BUILD_SCRIPT" <<'PY'
import re
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    script = stream.read()

unsafe_capture = re.search(
    r"[$][(]\s*(?:download|prepare|stage)_[A-Za-z0-9_]+\b",
    script,
)
if unsafe_capture:
    line = script.count("\n", 0, unsafe_capture.start()) + 1
    raise SystemExit(
        f"side-effecting function still uses raw command substitution at line {line}"
    )
PY
then
  fail "build runtime command-substitution contract is incomplete"
fi

nested_side_effect="$TEST_ROOT/nested-continued"
download_side_effect="$TEST_ROOT/download-continued"
stage_side_effect="$TEST_ROOT/stage-continued"
build_side_effect="$TEST_ROOT/build-continued"

nested_partial_failure() {
  printf '%s\n' "$TEST_ROOT/partial-download"
  false
  : >"$nested_side_effect"
}

download_fixture() {
  local asset

  capture_required_output asset nested_partial_failure || return $?
  : >"$download_side_effect"
  printf '%s\n' "$asset"
}

stage_fixture() {
  local asset

  capture_required_output asset download_fixture || return $?
  : >"$stage_side_effect"
  printf '%s\n' "$asset"
}

build_fixture() {
  local staged_runtime

  capture_required_output staged_runtime stage_fixture || return $?
  : >"$build_side_effect"
  printf '%s\n' "$staged_runtime"
}

captured_output="unchanged"
set +e
capture_required_output captured_output build_fixture
status=$?
set -e

[ "$status" -eq 1 ] || fail "nested failure returned $status instead of 1"
[ "$captured_output" = "unchanged" ] || fail "partial output escaped a failed capture"
[ ! -e "$nested_side_effect" ] || fail "nested command continued after its failure"
[ ! -e "$download_side_effect" ] || fail "download continued after a nested failure"
[ ! -e "$stage_side_effect" ] || fail "staging continued after a download failure"
[ ! -e "$build_side_effect" ] || fail "build work continued after a staging failure"

echo "build runtime nested failure propagation verified"
