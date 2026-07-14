#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_ROOT="$(mktemp -d)"
ORIGINAL_HOME="$HOME"
HOME="$TEST_ROOT/home"
SWITCHYARD_MANAGED_RUNTIME_ROOT="$HOME/.switchyard/runtimes"
SWAP_HELPER_DIR=""

cleanup() {
  if [ -n "$SWAP_HELPER_DIR" ]; then
    rm -rf "$SWAP_HELPER_DIR"
  fi
  HOME="$ORIGINAL_HOME"
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

mkdir -p "$HOME" "$SWITCHYARD_MANAGED_RUNTIME_ROOT"

short_sha256_stream() {
  shasum -a 256 | awk '{print substr($1, 1, 12)}'
}

sha256_file() {
  shasum -a 256 "$1" | awk '{print $1}'
}

content_tree_digest() {
  local root="$1"

  (
    cd "$root"
    find . \( -type f -o -type l \) ! -path './.switchyard-content-sha256' -print |
      LC_ALL=C sort |
      while IFS= read -r path; do
        if [ -L "$path" ]; then
          printf 'link %s %s\n' "$path" "$(readlink "$path")"
        else
          printf 'file %s %s\n' "$path" "$(sha256_file "$path")"
        fi
      done
  ) | short_sha256_stream
}

write_content_tree_digest() {
  local root="$1"
  content_tree_digest "$root" > "$root/.switchyard-content-sha256"
}

content_tree_is_verified() {
  local root="$1"
  local marker="$root/.switchyard-content-sha256"
  local expected

  [ -f "$marker" ] || return 1
  expected="$(tr -d '[:space:]' < "$marker")"
  [ -n "$expected" ] && [ "$(content_tree_digest "$root")" = "$expected" ]
}

source "$ROOT_DIR/switchyard/lib/directory_safety.sh"

new_stage() {
  local stage
  stage="$(mktemp -d "$TEST_ROOT/stage.XXXXXX")"
  printf 'new\n' > "$stage/new-content"
  printf '%s\n' "$stage"
}

expect_rejected() {
  local label="$1"
  local staged="$2"
  local live="$3"
  local kind="$4"

  if atomic_replace_directory "$staged" "$live" "$kind"; then
    echo "expected rejection for $label" >&2
    exit 1
  fi
  [ -d "$staged" ] || {
    echo "rejected replacement consumed staged data for $label" >&2
    exit 1
  }
}

stage="$(new_stage)"
new_live="$TEST_ROOT/new-runtime"
atomic_replace_directory "$stage" "$new_live" runtime
[ -f "$new_live/new-content" ]

unmarked_cache="$TEST_ROOT/unmarked-cache"
mkdir -p "$unmarked_cache"
printf 'keep\n' > "$unmarked_cache/sentinel"
stage="$(new_stage)"
expect_rejected "unmarked cache" "$stage" "$unmarked_cache" cache
[ "$(cat "$unmarked_cache/sentinel")" = "keep" ]
rm -rf "$stage"

verified_cache="$TEST_ROOT/verified-cache"
mkdir -p "$verified_cache"
printf 'old\n' > "$verified_cache/old-content"
write_content_tree_digest "$verified_cache"
printf 'tampered\n' >> "$verified_cache/old-content"
if content_tree_is_verified "$verified_cache"; then
  echo "expected the modified cache content digest to fail" >&2
  exit 1
fi
stage="$(new_stage)"
atomic_replace_directory "$stage" "$verified_cache" cache
[ -f "$verified_cache/new-content" ]
[ ! -e "$verified_cache/old-content" ]

unmanaged_runtime="$TEST_ROOT/project-data"
mkdir -p "$unmanaged_runtime"
printf 'keep\n' > "$unmanaged_runtime/sentinel"
stage="$(new_stage)"
expect_rejected "unmanaged runtime" "$stage" "$unmanaged_runtime" runtime
[ "$(cat "$unmanaged_runtime/sentinel")" = "keep" ]
rm -rf "$stage"

managed_runtime="$SWITCHYARD_MANAGED_RUNTIME_ROOT/test-runtime"
mkdir -p "$managed_runtime"
printf 'old\n' > "$managed_runtime/old-content"
stage="$(new_stage)"
atomic_replace_directory "$stage" "$managed_runtime" runtime
[ -f "$managed_runtime/new-content" ]
[ ! -e "$managed_runtime/old-content" ]

stage="$(new_stage)"
expect_rejected "managed runtime root" "$stage" "$SWITCHYARD_MANAGED_RUNTIME_ROOT" runtime
[ -f "$managed_runtime/new-content" ]
rm -rf "$stage"

stage="$(new_stage)"
expect_rejected "home directory" "$stage" "$HOME" runtime
[ -d "$SWITCHYARD_MANAGED_RUNTIME_ROOT" ]
rm -rf "$stage"

external_runtime="$TEST_ROOT/external-runtime"
mkdir -p "$external_runtime/bin"
printf '#!/usr/bin/env bash\nexit 0\n' > "$external_runtime/bin/switchyard-wine"
chmod 0755 "$external_runtime/bin/switchyard-wine"
cat > "$external_runtime/switchyard-runtime.json" <<EOF
{
  "id": "switchyard-local-wow64-x86_64-test",
  "installPrefix": "$external_runtime",
  "executable": "$external_runtime/bin/switchyard-wine"
}
EOF
stage="$(new_stage)"
atomic_replace_directory "$stage" "$external_runtime" runtime
[ -f "$external_runtime/new-content" ]

symlink_backing="$TEST_ROOT/symlink-backing"
symlink_live="$TEST_ROOT/symlink-live"
mkdir -p "$symlink_backing"
printf 'keep\n' > "$symlink_backing/sentinel"
write_content_tree_digest "$symlink_backing"
ln -s "$symlink_backing" "$symlink_live"
stage="$(new_stage)"
expect_rejected "symbolic-link destination" "$stage" "$symlink_live" cache
[ "$(cat "$symlink_backing/sentinel")" = "keep" ]
rm -rf "$stage"

echo "directory replacement safety tests passed"
