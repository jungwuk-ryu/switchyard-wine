#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM_BASE_FILE="$ROOT_DIR/switchyard/upstream-base.txt"

fail() {
  echo "source verification failed: $*" >&2
  exit 1
}

if [ ! -f "$UPSTREAM_BASE_FILE" ]; then
  fail "missing $UPSTREAM_BASE_FILE"
fi

upstream_base="$(tr -d '[:space:]' < "$UPSTREAM_BASE_FILE")"
git -C "$ROOT_DIR" cat-file -e "${upstream_base}^{commit}" 2>/dev/null ||
  fail "upstream base $upstream_base is unavailable; fetch at least 256 commits"
git -C "$ROOT_DIR" merge-base --is-ancestor "$upstream_base" HEAD ||
  fail "HEAD is not descended from upstream base $upstream_base"

git -C "$ROOT_DIR" diff --check "$upstream_base"..HEAD
git -C "$ROOT_DIR" diff --check
git -C "$ROOT_DIR" diff --cached --check

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

echo "source history verified from $upstream_base through $(git -C "$ROOT_DIR" rev-parse HEAD)"
