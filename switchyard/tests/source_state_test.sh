#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_ROOT="$(mktemp -d)"

cleanup() {
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

source "$ROOT_DIR/switchyard/lib/source_state.sh"

git -C "$TEST_ROOT" init -q -b main
git -C "$TEST_ROOT" config user.name "Switchyard Test"
git -C "$TEST_ROOT" config user.email "switchyard-test.invalid"
printf '*.ignored\n' >"$TEST_ROOT/.gitignore"
printf 'original\n' >"$TEST_ROOT/tracked.txt"
git -C "$TEST_ROOT" add .gitignore tracked.txt
git -C "$TEST_ROOT" commit -q -m initial

baseline="$(switchyard_source_state_fingerprint "$TEST_ROOT")"
case "$baseline" in
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
  *) echo "source-state fingerprint is not hexadecimal" >&2; exit 1 ;;
esac
[ "${#baseline}" -eq 64 ] || {
  echo "source-state fingerprint has the wrong length" >&2
  exit 1
}

printf 'modified\n' >"$TEST_ROOT/tracked.txt"
[ "$(switchyard_source_state_fingerprint "$TEST_ROOT")" != "$baseline" ] || {
  echo "tracked content change did not alter the source-state fingerprint" >&2
  exit 1
}
printf 'original\n' >"$TEST_ROOT/tracked.txt"
[ "$(switchyard_source_state_fingerprint "$TEST_ROOT")" = "$baseline" ] || {
  echo "restoring tracked content did not restore the source-state fingerprint" >&2
  exit 1
}

printf 'untracked\n' >"$TEST_ROOT/untracked.txt"
[ "$(switchyard_source_state_fingerprint "$TEST_ROOT")" != "$baseline" ] || {
  echo "untracked content did not alter the source-state fingerprint" >&2
  exit 1
}
rm -f "$TEST_ROOT/untracked.txt"

printf 'ignored\n' >"$TEST_ROOT/local.ignored"
[ "$(switchyard_source_state_fingerprint "$TEST_ROOT")" = "$baseline" ] || {
  echo "ignored content unexpectedly altered the source-state fingerprint" >&2
  exit 1
}

printf 'committed change\n' >"$TEST_ROOT/tracked.txt"
git -C "$TEST_ROOT" add tracked.txt
git -C "$TEST_ROOT" commit -q -m changed
[ "$(switchyard_source_state_fingerprint "$TEST_ROOT")" != "$baseline" ] || {
  echo "HEAD change did not alter the source-state fingerprint" >&2
  exit 1
}

echo "source-state fingerprint tests passed"
