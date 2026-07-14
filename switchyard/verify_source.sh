#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM_BASE_FILE="$ROOT_DIR/switchyard/upstream-base.txt"
IMPORTED_PATCHES_FILE="$ROOT_DIR/switchyard/imported-patches.txt"
EXPECTED_IMPORTED_PATCHES=106

fail() {
  echo "source verification failed: $*" >&2
  exit 1
}

if [ ! -f "$UPSTREAM_BASE_FILE" ]; then
  fail "missing $UPSTREAM_BASE_FILE"
fi
if [ ! -f "$IMPORTED_PATCHES_FILE" ]; then
  fail "missing $IMPORTED_PATCHES_FILE"
fi

upstream_base="$(tr -d '[:space:]' < "$UPSTREAM_BASE_FILE")"
git -C "$ROOT_DIR" cat-file -e "${upstream_base}^{commit}" 2>/dev/null ||
  fail "upstream base $upstream_base is unavailable; fetch at least 256 commits"
git -C "$ROOT_DIR" merge-base --is-ancestor "$upstream_base" HEAD ||
  fail "HEAD is not descended from upstream base $upstream_base"

git -C "$ROOT_DIR" diff --check "$upstream_base"..HEAD
git -C "$ROOT_DIR" diff --check
git -C "$ROOT_DIR" diff --cached --check

patch_ids="$({
  git -C "$ROOT_DIR" log --format='%B' "$upstream_base"..HEAD |
    sed -n 's/^Switchyard-Patch: //p'
} || true)"
patch_count="$(printf '%s\n' "$patch_ids" | sed '/^$/d' | wc -l | tr -d '[:space:]')"
unique_patch_count="$(printf '%s\n' "$patch_ids" | sed '/^$/d' | LC_ALL=C sort -u | wc -l | tr -d '[:space:]')"
declared_patch_count="$(sed '/^[[:space:]]*$/d' "$IMPORTED_PATCHES_FILE" | wc -l | tr -d '[:space:]')"
unique_declared_patch_count="$(sed '/^[[:space:]]*$/d' "$IMPORTED_PATCHES_FILE" | LC_ALL=C sort -u | wc -l | tr -d '[:space:]')"

if [ "$patch_count" -ne "$EXPECTED_IMPORTED_PATCHES" ]; then
  fail "expected $EXPECTED_IMPORTED_PATCHES imported patch trailers, found $patch_count"
fi
if [ "$unique_patch_count" -ne "$patch_count" ]; then
  fail "duplicate Switchyard-Patch trailers found"
fi
if [ "$declared_patch_count" -ne "$EXPECTED_IMPORTED_PATCHES" ] ||
   [ "$unique_declared_patch_count" -ne "$declared_patch_count" ]; then
  fail "imported patch manifest must contain $EXPECTED_IMPORTED_PATCHES unique IDs"
fi
if ! diff -u \
    <(sed '/^[[:space:]]*$/d' "$IMPORTED_PATCHES_FILE" | LC_ALL=C sort) \
    <(printf '%s\n' "$patch_ids" | sed '/^$/d' | LC_ALL=C sort); then
  fail "commit trailers do not match $IMPORTED_PATCHES_FILE"
fi

patch_commit_count=0
while IFS= read -r commit; do
  commit_patch_count="$(
    git -C "$ROOT_DIR" show -s --format='%B' "$commit" |
      sed -n 's/^Switchyard-Patch: //p' |
      sed '/^$/d' |
      wc -l |
      tr -d '[:space:]'
  )"
  if [ "$commit_patch_count" -gt 1 ]; then
    fail "commit $commit contains more than one Switchyard-Patch trailer"
  fi
  if [ "$commit_patch_count" -eq 1 ]; then
    patch_commit_count=$((patch_commit_count + 1))
  fi
done < <(git -C "$ROOT_DIR" rev-list "$upstream_base"..HEAD)
if [ "$patch_commit_count" -ne "$EXPECTED_IMPORTED_PATCHES" ]; then
  fail "expected one commit per imported patch; found $patch_commit_count patch commits"
fi

added_files="$({
  git -C "$ROOT_DIR" log --format= --name-only --diff-filter=A "$upstream_base"..HEAD
  git -C "$ROOT_DIR" diff --name-only --diff-filter=A "$upstream_base"..HEAD
  git -C "$ROOT_DIR" diff --name-only --diff-filter=A
  git -C "$ROOT_DIR" diff --cached --name-only --diff-filter=A
  git -C "$ROOT_DIR" ls-files --others --exclude-standard
} | LC_ALL=C sort -u)"
if printf '%s\n' "$added_files" | grep -Eiq '(^|/)(Game[[:space:]_-]*Porting[[:space:]_-]*Toolkit|GPTK)(/|$)|d3dmetal|libd3dshared|metalirconverter'; then
  fail "tracked file names suggest proprietary Apple Game Porting Toolkit content"
fi
if printf '%s\n' "$added_files" | grep -Eiq '\.(dmg|pkg|metallib|dylib)$'; then
  fail "unexpected prebuilt runtime artifact added to source history"
fi

echo "source history verified from $upstream_base through $(git -C "$ROOT_DIR" rev-parse HEAD)"
echo "verified $patch_count unique imported Switchyard patch commits"
