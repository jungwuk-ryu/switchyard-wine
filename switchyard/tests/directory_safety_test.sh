#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_ROOT="$(mktemp -d)"
TEST_ROOT="$(cd "$TEST_ROOT" && /bin/pwd -P)"
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
  local runtime_profile="${5-}"

  if { [ "$#" -eq 4 ] && atomic_replace_directory "$staged" "$live" "$kind"; } ||
     { [ "$#" -eq 5 ] && atomic_replace_directory \
         "$staged" "$live" "$kind" "$runtime_profile"; }; then
    echo "expected rejection for $label" >&2
    exit 1
  fi
  [ -d "$staged" ] || {
    echo "rejected replacement consumed staged data for $label" >&2
    exit 1
  }
}

write_preview_runtime_manifest() {
  local root="$1"
  local manifest_id="${2:-switchyard-local-native-arm64-fex-test}"
  local runtime_family="${3:-preview-native-arm64-fex}"
  local build_profile="${4:-switchyard-native-arm64-fex}"
  local install_prefix="${5:-$root}"
  local executable="${6:-$root/bin/switchyard-wine}"

  cat >"$root/switchyard-runtime.json" <<EOF
{
  "id": "$manifest_id",
  "runtimeFamily": "$runtime_family",
  "buildProfile": "$build_profile",
  "installPrefix": "$install_prefix",
  "executable": "$executable"
}
EOF
}

new_preview_runtime() {
  local root="$1"

  mkdir -p "$root/bin"
  printf '#!/usr/bin/env bash\nexit 0\n' >"$root/bin/switchyard-wine"
  chmod 0755 "$root/bin/switchyard-wine"
  printf 'keep\n' >"$root/sentinel"
  write_preview_runtime_manifest "$root"
}

expect_preview_runtime_rejected() {
  local label="$1"
  local live="$2"
  local stage

  stage="$(new_stage)"
  expect_rejected "$label" "$stage" "$live" runtime preview-native-arm64-fex
  [ "$(cat "$live/sentinel")" = keep ] || {
    echo "rejected preview replacement changed live data for $label" >&2
    exit 1
  }
  rm -rf "$stage"
}

stage="$(new_stage)"
new_live="$TEST_ROOT/new-runtime"
atomic_replace_directory "$stage" "$new_live" runtime
[ -f "$new_live/new-content" ]

stage="$(new_stage)"
unknown_profile_live="$TEST_ROOT/unknown-profile-runtime"
expect_rejected "unknown runtime profile" "$stage" "$unknown_profile_live" \
  runtime unknown-profile
[ ! -e "$unknown_profile_live" ]
rm -rf "$stage"

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

preview_runtime="$TEST_ROOT/preview-runtime"
new_preview_runtime "$preview_runtime"
preview_stage="$(new_stage)"
atomic_replace_directory "$preview_stage" "$preview_runtime" runtime \
  preview-native-arm64-fex
[ -f "$preview_runtime/new-content" ]

preview_link_runtime="$TEST_ROOT/preview-link-runtime"
new_preview_runtime "$preview_link_runtime"
mv "$preview_link_runtime/bin/switchyard-wine" "$preview_link_runtime/bin/wine"
ln -s wine "$preview_link_runtime/bin/switchyard-wine"
preview_link_stage="$(new_stage)"
atomic_replace_directory "$preview_link_stage" "$preview_link_runtime" runtime \
  preview-native-arm64-fex
[ -f "$preview_link_runtime/new-content" ]

cross_profile_runtime="$TEST_ROOT/cross-profile-runtime"
new_preview_runtime "$cross_profile_runtime"
stage="$(new_stage)"
expect_rejected "preview runtime through stable ownership" "$stage" \
  "$cross_profile_runtime" runtime stable-x86_64-rosetta
[ "$(cat "$cross_profile_runtime/sentinel")" = keep ]
rm -rf "$stage"

for field in id runtimeFamily buildProfile installPrefix executable; do
  malformed_runtime="$TEST_ROOT/preview-malformed-$field"
  new_preview_runtime "$malformed_runtime"
  case "$field" in
    id)
      write_preview_runtime_manifest "$malformed_runtime" \
        switchyard-local-native-arm64-test
      ;;
    runtimeFamily)
      write_preview_runtime_manifest "$malformed_runtime" \
        switchyard-local-native-arm64-fex-test stable-x86_64-rosetta
      ;;
    buildProfile)
      write_preview_runtime_manifest "$malformed_runtime" \
        switchyard-local-native-arm64-fex-test preview-native-arm64-fex \
        switchyard-native-arm64
      ;;
    installPrefix)
      write_preview_runtime_manifest "$malformed_runtime" \
        switchyard-local-native-arm64-fex-test preview-native-arm64-fex \
        switchyard-native-arm64-fex "$malformed_runtime-suffix"
      ;;
    executable)
      write_preview_runtime_manifest "$malformed_runtime" \
        switchyard-local-native-arm64-fex-test preview-native-arm64-fex \
        switchyard-native-arm64-fex "$malformed_runtime" \
        "$malformed_runtime/bin/wine"
      ;;
  esac
  expect_preview_runtime_rejected "preview manifest $field mismatch" \
    "$malformed_runtime"
done

duplicate_manifest_runtime="$TEST_ROOT/preview-duplicate-manifest"
new_preview_runtime "$duplicate_manifest_runtime"
python3 - "$duplicate_manifest_runtime/switchyard-runtime.json" <<'PY'
import sys
path = sys.argv[1]
data = open(path, encoding="utf-8").read()
data = data.replace('  "id":', '  "id": "switchyard-local-native-arm64-fex-duplicate",\n  "id":', 1)
open(path, "w", encoding="utf-8").write(data)
PY
expect_preview_runtime_rejected "duplicate preview manifest key" \
  "$duplicate_manifest_runtime"

symlink_manifest_runtime="$TEST_ROOT/preview-symlink-manifest"
new_preview_runtime "$symlink_manifest_runtime"
mv "$symlink_manifest_runtime/switchyard-runtime.json" \
  "$symlink_manifest_runtime/manifest.real"
ln -s manifest.real "$symlink_manifest_runtime/switchyard-runtime.json"
expect_preview_runtime_rejected "symbolic-link preview manifest" \
  "$symlink_manifest_runtime"

writable_manifest_runtime="$TEST_ROOT/preview-writable-manifest"
new_preview_runtime "$writable_manifest_runtime"
chmod 0666 "$writable_manifest_runtime/switchyard-runtime.json"
expect_preview_runtime_rejected "group/world-writable preview manifest" \
  "$writable_manifest_runtime"

hardlink_manifest_runtime="$TEST_ROOT/preview-hardlink-manifest"
new_preview_runtime "$hardlink_manifest_runtime"
ln "$hardlink_manifest_runtime/switchyard-runtime.json" \
  "$hardlink_manifest_runtime/manifest.hardlink"
expect_preview_runtime_rejected "hard-linked preview manifest" \
  "$hardlink_manifest_runtime"

writable_bin_runtime="$TEST_ROOT/preview-writable-bin"
new_preview_runtime "$writable_bin_runtime"
chmod 0777 "$writable_bin_runtime/bin"
expect_preview_runtime_rejected "group/world-writable preview bin" \
  "$writable_bin_runtime"

writable_launcher_runtime="$TEST_ROOT/preview-writable-launcher"
new_preview_runtime "$writable_launcher_runtime"
chmod 0777 "$writable_launcher_runtime/bin/switchyard-wine"
expect_preview_runtime_rejected "group/world-writable preview launcher" \
  "$writable_launcher_runtime"

unsafe_ancestor="$TEST_ROOT/preview-unsafe-ancestor"
mkdir -m 0777 "$unsafe_ancestor"
chmod 0777 "$unsafe_ancestor"
unsafe_ancestor_live="$unsafe_ancestor/live"
new_preview_runtime "$unsafe_ancestor_live"
unsafe_ancestor_stage="$unsafe_ancestor/stage"
mkdir -m 0700 "$unsafe_ancestor_stage"
printf 'new\n' >"$unsafe_ancestor_stage/new-content"
expect_rejected "non-sticky writable preview ancestor" \
  "$unsafe_ancestor_stage" "$unsafe_ancestor_live" runtime \
  preview-native-arm64-fex

sticky_ancestor="$TEST_ROOT/preview-sticky-ancestor"
mkdir -m 1777 "$sticky_ancestor"
chmod 1777 "$sticky_ancestor"
sticky_live="$sticky_ancestor/live"
new_preview_runtime "$sticky_live"
sticky_stage="$sticky_ancestor/stage"
mkdir -m 0700 "$sticky_stage"
printf 'new\n' >"$sticky_stage/new-content"
atomic_replace_directory "$sticky_stage" "$sticky_live" runtime \
  preview-native-arm64-fex
[ -f "$sticky_live/new-content" ]

preview_new_parent="$TEST_ROOT/preview-new-parent"
mkdir -m 0700 "$preview_new_parent"
preview_new_stage="$preview_new_parent/stage"
preview_new_live="$preview_new_parent/live"
mkdir -m 0700 "$preview_new_stage"
printf 'new\n' >"$preview_new_stage/new-content"
atomic_replace_directory "$preview_new_stage" "$preview_new_live" runtime \
  preview-native-arm64-fex
[ -f "$preview_new_live/new-content" ] && [ ! -e "$preview_new_stage" ]

for injected_failure in verify fsync; do
  injected_swap_live="$TEST_ROOT/preview-injected-swap-$injected_failure-live"
  new_preview_runtime "$injected_swap_live"
  injected_swap_stage="$TEST_ROOT/preview-injected-swap-$injected_failure-stage"
  mkdir -m 0700 "$injected_swap_stage"
  printf 'new\n' >"$injected_swap_stage/new-content"
  if SWITCHYARD_PREVIEW_PUBLISH_TEST_FAILURE="$injected_failure" \
      atomic_replace_directory "$injected_swap_stage" "$injected_swap_live" \
        runtime preview-native-arm64-fex; then
    echo "injected $injected_failure swap publication unexpectedly succeeded" >&2
    exit 1
  fi
  [ "$(cat "$injected_swap_live/sentinel")" = keep ] &&
    [ ! -e "$injected_swap_live/new-content" ] &&
    [ -f "$injected_swap_stage/new-content" ] || {
      echo "injected $injected_failure swap publication was not rolled back" >&2
      exit 1
    }

  injected_exclusive_parent="$TEST_ROOT/preview-injected-exclusive-$injected_failure"
  mkdir -m 0700 "$injected_exclusive_parent"
  injected_exclusive_stage="$injected_exclusive_parent/stage"
  injected_exclusive_live="$injected_exclusive_parent/live"
  mkdir -m 0700 "$injected_exclusive_stage"
  printf 'new\n' >"$injected_exclusive_stage/new-content"
  if SWITCHYARD_PREVIEW_PUBLISH_TEST_FAILURE="$injected_failure" \
      atomic_replace_directory "$injected_exclusive_stage" \
        "$injected_exclusive_live" runtime preview-native-arm64-fex; then
    echo "injected $injected_failure exclusive publication unexpectedly succeeded" >&2
    exit 1
  fi
  [ -f "$injected_exclusive_stage/new-content" ] &&
    [ ! -e "$injected_exclusive_live" ] &&
    [ ! -L "$injected_exclusive_live" ] || {
      echo "injected $injected_failure exclusive publication was not rolled back" >&2
      exit 1
    }
done

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
