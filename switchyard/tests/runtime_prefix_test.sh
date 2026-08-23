#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && /bin/pwd -P)"
LIBRARY="$ROOT_DIR/switchyard/lib/runtime_prefix.sh"
TEST_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-runtime-prefix-test.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && /bin/pwd -P)"
ORIGINAL_HOME="$HOME"
HOME="$TEST_ROOT/home"
SWITCHYARD_MANAGED_PREFIX_ROOT="$TEST_ROOT/managed-prefixes"
failure_index=0
background_pids=()

# shellcheck disable=SC2329 # Invoked through the EXIT and signal traps.
cleanup() {
  local pid

  for pid in "${background_pids[@]-}"; do
    [ -n "$pid" ] || continue
    /bin/kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  HOME="$ORIGINAL_HOME"
  case "$TEST_ROOT" in
    /private/tmp/switchyard-runtime-prefix-test.??????)
      [ -d "$TEST_ROOT" ] && [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected runtime-prefix test root: $TEST_ROOT" >&2 ;;
  esac
}
trap cleanup EXIT HUP INT TERM

fail() {
  echo "$1" >&2
  exit 1
}

expect_failure() {
  local label="$1"
  local expected_status="$2"
  local output error actual_status
  shift 2

  failure_index=$((failure_index + 1))
  output="$TEST_ROOT/failure-$failure_index.out"
  error="$TEST_ROOT/failure-$failure_index.err"
  set +e
  "$@" >"$output" 2>"$error"
  actual_status=$?
  set -e
  [ "$actual_status" -eq "$expected_status" ] || {
    /bin/cat "$output" >&2
    /bin/cat "$error" >&2
    fail "$label returned $actual_status instead of $expected_status"
  }
}

assert_marker() {
  local prefix="$1"
  local profile="$2"
  local marker="$prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"

  [ -f "$marker" ] && [ ! -L "$marker" ] || fail "marker is missing at $prefix"
  [ "$(/usr/bin/stat -f '%Lp' "$marker")" = 600 ] || fail "marker mode is not 0600"
  [ "$(/bin/cat "$marker")" = \
    "{\"runtimeFamily\":\"$profile\",\"schemaVersion\":1}" ] ||
    fail "marker bytes do not identify $profile"
}

tree_fingerprint() {
  /usr/bin/python3 -I - "$1" <<'PY'
import hashlib
import os
import stat
import sys

root = sys.argv[1]
rows = []
for directory, directories, files in os.walk(root, followlinks=False):
    names = sorted(directories + files, key=os.fsencode)
    for name in names:
        path = os.path.join(directory, name)
        relative = os.path.relpath(path, root)
        info = os.lstat(path)
        mode = stat.S_IMODE(info.st_mode)
        if stat.S_ISLNK(info.st_mode):
            rows.append(f"link {relative} {mode:o} {os.readlink(path)}")
        elif stat.S_ISDIR(info.st_mode):
            rows.append(f"dir {relative} {mode:o}")
        elif stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
            rows.append(f"file {relative} {mode:o} {digest}")
        else:
            rows.append(f"special {relative} {mode:o}")
print(hashlib.sha256("\n".join(rows).encode()).hexdigest())
PY
}

assert_no_staging_tree() {
  local parent="$1"

  [ -z "$(/usr/bin/find "$parent" -maxdepth 1 \
      -name '.switchyard-prefix-migration.*' -print -quit)" ] ||
    fail "migration left a private staging tree in $parent"
}

assert_no_removal_tombstone() {
  local parent="$1"

  [ -z "$(/usr/bin/find "$parent" -maxdepth 1 \
      -name '.switchyard-prefix-removal.*' -print -quit)" ] ||
    fail "prefix removal left a tombstone in $parent"
}

populate_many_files() {
  local root="$1"
  local count="$2"
  local index

  for ((index = 0; index < count; index++)); do
    /usr/bin/printf 'fixture-%04d\n' "$index" >"$root/file-$(printf '%04d' "$index")"
  done
}

/bin/bash -n "$LIBRARY"
/bin/bash -n "$0"
/bin/mkdir -m 0700 "$HOME"

# shellcheck disable=SC1090 # The library path is fixed above.
source "$LIBRARY"

stable_prefix=unchanged
switchyard_prepare_runtime_prefix stable-x86_64-rosetta '' stable_prefix
[ "$stable_prefix" = "$HOME/.wine" ] || fail "stable default prefix changed"
[ ! -e "$stable_prefix" ] && [ ! -L "$stable_prefix" ] ||
  fail "stable prefix preparation created the legacy default"
stable_explicit='relative stable prefix with spaces'
stable_result=
switchyard_prepare_runtime_prefix \
  stable-x86_64-rosetta "$stable_explicit" stable_result
[ "$stable_result" = "$stable_explicit" ] || fail "stable explicit prefix was normalized"
expect_failure "invalid output variable" 2 \
  switchyard_prepare_runtime_prefix stable-x86_64-rosetta '' 'invalid-output'
expect_failure "unknown preparation profile" 2 \
  switchyard_prepare_runtime_prefix unknown-profile '' ignored

preview_prefix=
switchyard_prepare_runtime_prefix preview-native-arm64-fex '' preview_prefix
[ "$preview_prefix" = \
  "$SWITCHYARD_MANAGED_PREFIX_ROOT/preview-native-arm64-fex/default" ] ||
  fail "preview default is not profile scoped"
[ "$(/usr/bin/stat -f '%Lp' "$preview_prefix")" = 700 ] ||
  fail "preview default prefix mode is not 0700"
assert_marker "$preview_prefix" preview-native-arm64-fex
preview_identity="$(/usr/bin/stat -f '%d:%i' "$preview_prefix")"
preview_marker_identity="$(/usr/bin/stat -f '%d:%i:%m:%c:%z:%Lp' \
  "$preview_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER")"
validated_preview=
switchyard_validate_prepared_runtime_prefix \
  preview-native-arm64-fex "$preview_prefix" validated_preview
[ "$validated_preview" = "$preview_prefix" ] ||
  fail "read-only prepared-prefix validation changed the resolved path"
[ "$(/usr/bin/stat -f '%d:%i:%m:%c:%z:%Lp' \
  "$preview_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER")" = \
  "$preview_marker_identity" ] ||
  fail "read-only prepared-prefix validation changed the marker"
expect_failure "invalid prepared-prefix output variable" 2 \
  switchyard_validate_prepared_runtime_prefix \
  preview-native-arm64-fex "$preview_prefix" 'invalid-output'
preview_again=
switchyard_prepare_runtime_prefix preview-native-arm64-fex '' preview_again
[ "$preview_again" = "$preview_prefix" ] || fail "preview default changed on reuse"
[ "$(/usr/bin/stat -f '%d:%i' "$preview_again")" = "$preview_identity" ] ||
  fail "preview reuse replaced the existing prefix"

explicit_parent="$TEST_ROOT/explicit-parent"
/bin/mkdir -m 0700 "$explicit_parent"
explicit_prefix="$explicit_parent/new-prefix"
prepared_explicit=
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$explicit_prefix" prepared_explicit
[ "$prepared_explicit" = "$explicit_prefix" ] || fail "explicit preview path changed"
assert_marker "$explicit_prefix" preview-native-arm64-fex

empty_prefix="$explicit_parent/empty-prefix"
/bin/mkdir -m 0700 "$empty_prefix"
prepared_empty=
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$empty_prefix" prepared_empty
assert_marker "$prepared_empty" preview-native-arm64-fex

concurrent_prefix="$explicit_parent/concurrent-prefix"
concurrent_pids=()
for index in 1 2 3 4 5 6 7 8; do
  (
    concurrent_result=
    switchyard_prepare_runtime_prefix \
      preview-native-arm64-fex "$concurrent_prefix" concurrent_result
    [ "$concurrent_result" = "$concurrent_prefix" ]
  ) >"$TEST_ROOT/concurrent-$index.out" \
    2>"$TEST_ROOT/concurrent-$index.err" &
  concurrent_pids+=("$!")
done
for concurrent_pid in "${concurrent_pids[@]}"; do
  wait "$concurrent_pid" || {
    /bin/cat "$TEST_ROOT"/concurrent-*.err >&2
    fail "concurrent preview prefix preparation failed"
  }
done
assert_marker "$concurrent_prefix" preview-native-arm64-fex

unmarked_prefix="$explicit_parent/unmarked-prefix"
/bin/mkdir -m 0700 "$unmarked_prefix"
/usr/bin/printf 'keep\n' >"$unmarked_prefix/sentinel"
expect_failure "nonempty unmarked preview prefix" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$unmarked_prefix" ignored
[ "$(/bin/cat "$unmarked_prefix/sentinel")" = keep ] &&
  [ ! -e "$unmarked_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER" ] ||
  fail "unmarked rejection changed the prefix"

wrong_marker_prefix="$explicit_parent/wrong-marker-prefix"
/bin/mkdir -m 0700 "$wrong_marker_prefix"
/usr/bin/printf '%s\n' \
  '{"runtimeFamily":"stable-x86_64-rosetta","schemaVersion":1}' \
  >"$wrong_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/chmod 0600 "$wrong_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
expect_failure "cross-profile preview marker" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$wrong_marker_prefix" ignored

extra_marker_prefix="$explicit_parent/extra-marker-prefix"
/bin/mkdir -m 0700 "$extra_marker_prefix"
/usr/bin/printf '%s\n' \
  '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1,"unexpected":true}' \
  >"$extra_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/chmod 0600 "$extra_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
expect_failure "extra preview marker field" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$extra_marker_prefix" ignored

unsafe_mode_prefix="$explicit_parent/unsafe-mode-prefix"
/bin/mkdir -m 0700 "$unsafe_mode_prefix"
/usr/bin/printf '%s\n' \
  '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1}' \
  >"$unsafe_mode_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/chmod 0644 "$unsafe_mode_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
expect_failure "unsafe preview marker mode" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$unsafe_mode_prefix" ignored

hardlink_marker_prefix="$explicit_parent/hardlink-marker-prefix"
/bin/mkdir -m 0700 "$hardlink_marker_prefix"
/usr/bin/printf '%s\n' \
  '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1}' \
  >"$hardlink_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/chmod 0600 "$hardlink_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/ln "$hardlink_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER" \
  "$TEST_ROOT/marker-hardlink"
expect_failure "hard-linked preview marker" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$hardlink_marker_prefix" ignored

marker_referent="$TEST_ROOT/marker-referent"
/usr/bin/printf 'outside\n' >"$marker_referent"
symlink_marker_prefix="$explicit_parent/symlink-marker-prefix"
/bin/mkdir -m 0700 "$symlink_marker_prefix"
/bin/ln -s "$marker_referent" \
  "$symlink_marker_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
expect_failure "symbolic-link preview marker" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$symlink_marker_prefix" ignored
[ "$(/bin/cat "$marker_referent")" = outside ] ||
  fail "marker rejection changed its referent"

prefix_referent="$TEST_ROOT/prefix-referent"
/bin/mkdir -m 0700 "$prefix_referent"
/bin/ln -s "$prefix_referent" "$explicit_parent/prefix-link"
expect_failure "symbolic-link preview prefix" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$explicit_parent/prefix-link" ignored
[ -z "$(/usr/bin/find "$prefix_referent" -mindepth 1 -print -quit)" ] ||
  fail "prefix-link rejection wrote through the link"

linked_parent_referent="$TEST_ROOT/linked-parent-referent"
/bin/mkdir -m 0700 "$linked_parent_referent"
/bin/ln -s "$linked_parent_referent" "$TEST_ROOT/linked-parent"
expect_failure "symbolic-link preview parent" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$TEST_ROOT/linked-parent/prefix" ignored
[ -z "$(/usr/bin/find "$linked_parent_referent" -mindepth 1 -print -quit)" ] ||
  fail "parent-link rejection wrote through the link"

file_prefix="$explicit_parent/file-prefix"
/usr/bin/printf 'keep\n' >"$file_prefix"
expect_failure "regular-file preview prefix" 1 \
  switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$file_prefix" ignored
[ "$(/bin/cat "$file_prefix")" = keep ] ||
  fail "file-prefix rejection changed the file"

stable_source="$TEST_ROOT/stable-source"
migration_parent="$TEST_ROOT/migration-parent"
outside="$TEST_ROOT/outside"
/bin/mkdir -m 0700 "$stable_source" "$migration_parent" "$outside"
/bin/mkdir -m 0755 "$stable_source/drive_c"
/usr/bin/printf 'registry\n' >"$stable_source/system.reg"
/usr/bin/printf 'program\n' >"$stable_source/drive_c/program.exe"
/bin/chmod 0755 "$stable_source/drive_c/program.exe"
/usr/bin/printf 'outside remains\n' >"$outside/sentinel"
/bin/ln -s "$outside" "$stable_source/drive_c/outside-link"
stable_before="$(tree_fingerprint "$stable_source")"
migrated_preview="$migration_parent/preview-prefix"
switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$stable_source" \
  preview-native-arm64-fex "$migrated_preview"
[ "$(tree_fingerprint "$stable_source")" = "$stable_before" ] ||
  fail "migration mutated the stable source"
[ ! -e "$stable_source/$SWITCHYARD_RUNTIME_PREFIX_MARKER" ] ||
  fail "migration retagged the legacy stable source"
[ "$(/usr/bin/stat -f '%Lp' "$migrated_preview")" = 700 ] ||
  fail "migrated preview root is not owner-only"
assert_marker "$migrated_preview" preview-native-arm64-fex
/usr/bin/cmp -s "$stable_source/system.reg" "$migrated_preview/system.reg" ||
  fail "migrated regular file differs"
[ "$(/usr/bin/readlink "$migrated_preview/drive_c/outside-link")" = "$outside" ] ||
  fail "migration followed or changed a symbolic link"
[ "$(/bin/cat "$outside/sentinel")" = 'outside remains' ] ||
  fail "migration changed a symbolic-link referent"

preview_source="$TEST_ROOT/preview-source"
/bin/mkdir -m 0700 "$preview_source"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$preview_source" prepared_preview_source
/usr/bin/printf 'preview data\n' >"$preview_source/user.reg"
preview_before="$(tree_fingerprint "$preview_source")"
stable_target="$migration_parent/stable-prefix"
switchyard_migrate_runtime_prefix_offline \
  preview-native-arm64-fex "$preview_source" \
  stable-x86_64-rosetta "$stable_target"
[ "$(tree_fingerprint "$preview_source")" = "$preview_before" ] ||
  fail "reverse migration mutated the preview source"
assert_marker "$stable_target" stable-x86_64-rosetta
assert_marker "$preview_source" preview-native-arm64-fex

expect_failure "same-profile migration" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$stable_source" \
  stable-x86_64-rosetta "$migration_parent/same-profile"
[ ! -e "$migration_parent/same-profile" ]

unmarked_preview_source="$TEST_ROOT/unmarked-preview-source"
/bin/mkdir -m 0700 "$unmarked_preview_source"
/usr/bin/printf 'data\n' >"$unmarked_preview_source/file"
expect_failure "unmarked preview migration source" 1 \
  switchyard_migrate_runtime_prefix_offline \
  preview-native-arm64-fex "$unmarked_preview_source" \
  stable-x86_64-rosetta "$migration_parent/from-unmarked-preview"
[ ! -e "$migration_parent/from-unmarked-preview" ]

wrong_stable_source="$TEST_ROOT/wrong-stable-source"
/bin/mkdir -m 0700 "$wrong_stable_source"
/usr/bin/printf '%s\n' \
  '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1}' \
  >"$wrong_stable_source/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/chmod 0600 "$wrong_stable_source/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
expect_failure "cross-profile stable migration marker" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$wrong_stable_source" \
  preview-native-arm64-fex "$migration_parent/from-wrong-stable"
[ ! -e "$migration_parent/from-wrong-stable" ]

existing_target="$migration_parent/existing-target"
/bin/mkdir -m 0700 "$existing_target"
/usr/bin/printf 'keep\n' >"$existing_target/sentinel"
expect_failure "existing migration target" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$stable_source" \
  preview-native-arm64-fex "$existing_target"
[ "$(/bin/cat "$existing_target/sentinel")" = keep ] ||
  fail "existing-target rejection changed the target"

expect_failure "migration target inside source" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$stable_source" \
  preview-native-arm64-fex "$stable_source/target"
[ ! -e "$stable_source/target" ]

source_link="$TEST_ROOT/source-link"
/bin/ln -s "$stable_source" "$source_link"
expect_failure "symbolic-link migration source" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$source_link" \
  preview-native-arm64-fex "$migration_parent/from-source-link"

migration_link_parent_referent="$TEST_ROOT/migration-link-parent-referent"
/bin/mkdir -m 0700 "$migration_link_parent_referent"
/bin/ln -s "$migration_link_parent_referent" "$TEST_ROOT/migration-link-parent"
expect_failure "symbolic-link migration target parent" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$stable_source" \
  preview-native-arm64-fex "$TEST_ROOT/migration-link-parent/target"
[ -z "$(/usr/bin/find "$migration_link_parent_referent" -mindepth 1 -print -quit)" ] ||
  fail "migration wrote through a target-parent link"

special_source="$TEST_ROOT/special-source"
/bin/mkdir -m 0700 "$special_source"
/usr/bin/mkfifo "$special_source/unsafe-fifo"
special_parent="$TEST_ROOT/special-parent"
/bin/mkdir -m 0700 "$special_parent"
expect_failure "special-file migration source" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$special_source" \
  preview-native-arm64-fex "$special_parent/target"
[ ! -e "$special_parent/target" ]
assert_no_staging_tree "$special_parent"

# Deterministic source-race fixture: wait until the first copied file appears in
# the private staging tree, then alter that already-copied source inode.  The
# final fd-pinned source-state pass must reject and clean the unpublished tree.
race_source="$TEST_ROOT/race-source"
race_parent="$TEST_ROOT/race-parent"
/bin/mkdir -m 0700 "$race_source" "$race_parent"
populate_many_files "$race_source" 1200
(
  shopt -s nullglob
  while :; do
    for stage in "$race_parent"/.switchyard-prefix-migration.*; do
      if [ -f "$stage/file-0000" ]; then
        /usr/bin/printf 'raced\n' >>"$race_source/file-0000"
        exit 0
      fi
    done
    /bin/sleep 0.001
  done
) &
race_writer_pid=$!
background_pids+=("$race_writer_pid")
expect_failure "source mutation during migration" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$race_source" \
  preview-native-arm64-fex "$race_parent/target"
wait "$race_writer_pid"
background_pids=()
[ ! -e "$race_parent/target" ]
assert_no_staging_tree "$race_parent"
/usr/bin/grep -Fx raced "$race_source/file-0000" >/dev/null ||
  fail "source race fixture did not execute"

# Rename-away/restore of the source directory itself changes its pinned state
# even though the same inode is back at the same path before the final pass.
source_aba="$TEST_ROOT/source-aba"
source_aba_parent="$TEST_ROOT/source-aba-parent"
source_aba_target_parent="$TEST_ROOT/source-aba-target-parent"
/bin/mkdir -m 0700 "$source_aba_parent" "$source_aba_target_parent"
/bin/mkdir -m 0700 "$source_aba"
populate_many_files "$source_aba" 1200
(
  shopt -s nullglob
  while :; do
    for stage in "$source_aba_target_parent"/.switchyard-prefix-migration.*; do
      [ -d "$stage" ] || continue
      /bin/mv "$source_aba" "$source_aba_parent/source-away"
      /bin/mv "$source_aba_parent/source-away" "$source_aba"
      exit 0
    done
    /bin/sleep 0.001
  done
) &
source_aba_pid=$!
background_pids+=("$source_aba_pid")
expect_failure "source path ABA during migration" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$source_aba" \
  preview-native-arm64-fex "$source_aba_target_parent/target"
wait "$source_aba_pid"
background_pids=()
[ -f "$source_aba/file-0000" ] || fail "source-path ABA lost source data"
[ ! -e "$source_aba_target_parent/target" ]
assert_no_staging_tree "$source_aba_target_parent"

# Deterministic target-race fixture: reserve the target after staging starts.
# RENAME_EXCL must retain the racing target and discard only our private stage.
target_race_source="$TEST_ROOT/target-race-source"
target_race_parent="$TEST_ROOT/target-race-parent"
/bin/mkdir -m 0700 "$target_race_source" "$target_race_parent"
populate_many_files "$target_race_source" 1200
(
  shopt -s nullglob
  while :; do
    for stage in "$target_race_parent"/.switchyard-prefix-migration.*; do
      [ -d "$stage" ] || continue
      /bin/mkdir -m 0700 "$target_race_parent/target"
      /usr/bin/printf 'racing target\n' >"$target_race_parent/target/sentinel"
      exit 0
    done
    /bin/sleep 0.001
  done
) &
target_writer_pid=$!
background_pids+=("$target_writer_pid")
expect_failure "target creation during migration" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$target_race_source" \
  preview-native-arm64-fex "$target_race_parent/target"
wait "$target_writer_pid"
background_pids=()
[ "$(/bin/cat "$target_race_parent/target/sentinel")" = 'racing target' ] ||
  fail "exclusive publication replaced the racing target"
assert_no_staging_tree "$target_race_parent"

# Renaming the target parent away and back keeps its inode/path spelling but
# changes its captured directory state.  Publication must still fail closed.
parent_aba_source="$TEST_ROOT/parent-aba-source"
parent_aba_holder="$TEST_ROOT/parent-aba-holder"
parent_aba_target_parent="$TEST_ROOT/parent-aba-target-parent"
/bin/mkdir -m 0700 \
  "$parent_aba_source" "$parent_aba_holder" "$parent_aba_target_parent"
populate_many_files "$parent_aba_source" 1200
(
  shopt -s nullglob
  while :; do
    for stage in "$parent_aba_target_parent"/.switchyard-prefix-migration.*; do
      [ -d "$stage" ] || continue
      /bin/mv "$parent_aba_target_parent" "$parent_aba_holder/parent-away"
      /bin/mv "$parent_aba_holder/parent-away" "$parent_aba_target_parent"
      exit 0
    done
    /bin/sleep 0.001
  done
) &
parent_aba_pid=$!
background_pids+=("$parent_aba_pid")
expect_failure "target-parent path ABA during migration" 1 \
  switchyard_migrate_runtime_prefix_offline \
  stable-x86_64-rosetta "$parent_aba_source" \
  preview-native-arm64-fex "$parent_aba_target_parent/target"
wait "$parent_aba_pid"
background_pids=()
[ ! -e "$parent_aba_target_parent/target" ]
assert_no_staging_tree "$parent_aba_target_parent"

removal_parent="$TEST_ROOT/removal-parent"
removal_outside="$TEST_ROOT/removal-outside"
/bin/mkdir -m 0700 "$removal_parent" "$removal_outside"
/usr/bin/printf 'outside remains\n' >"$removal_outside/sentinel"

removable_prefix="$removal_parent/removable-prefix"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$removable_prefix" removable_result
/usr/bin/printf 'remove me\n' >"$removable_prefix/user.reg"
/bin/ln -s "$removal_outside" "$removable_prefix/outside-link"
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removable_prefix"
[ ! -e "$removable_prefix" ] && [ ! -L "$removable_prefix" ] ||
  fail "valid marked prefix was not removed"
[ "$(/bin/cat "$removal_outside/sentinel")" = 'outside remains' ] ||
  fail "prefix removal followed a symbolic-link referent"
assert_no_removal_tombstone "$removal_parent"

unmarked_stable_removal="$removal_parent/unmarked-stable"
/bin/mkdir -m 0700 "$unmarked_stable_removal"
/usr/bin/printf 'keep\n' >"$unmarked_stable_removal/sentinel"
expect_failure "unmarked stable prefix removal" 1 \
  switchyard_remove_runtime_prefix_offline \
  stable-x86_64-rosetta "$unmarked_stable_removal"
[ "$(/bin/cat "$unmarked_stable_removal/sentinel")" = keep ] ||
  fail "unmarked stable removal changed the prefix"

wrong_removal_profile="$removal_parent/wrong-removal-profile"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$wrong_removal_profile" ignored
expect_failure "cross-profile prefix removal" 1 \
  switchyard_remove_runtime_prefix_offline \
  stable-x86_64-rosetta "$wrong_removal_profile"
assert_marker "$wrong_removal_profile" preview-native-arm64-fex
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$wrong_removal_profile"

expect_failure "home directory prefix removal" 1 \
  switchyard_remove_runtime_prefix_offline preview-native-arm64-fex "$HOME"
expect_failure "managed root prefix removal" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$SWITCHYARD_MANAGED_PREFIX_ROOT"

saved_home="$HOME"
HOME="$TEST_ROOT/protected-home-ancestor/child/home"
expect_failure "home ancestor prefix removal" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$TEST_ROOT/protected-home-ancestor"
HOME="$saved_home"
saved_managed_prefix_root="$SWITCHYARD_MANAGED_PREFIX_ROOT"
SWITCHYARD_MANAGED_PREFIX_ROOT="$TEST_ROOT/protected-managed-ancestor/child/managed"
expect_failure "managed-root ancestor prefix removal" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$TEST_ROOT/protected-managed-ancestor"
SWITCHYARD_MANAGED_PREFIX_ROOT="$saved_managed_prefix_root"

removal_link_referent="$removal_parent/removal-link-referent"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$removal_link_referent" ignored
/bin/ln -s "$removal_link_referent" "$removal_parent/removal-link"
expect_failure "symbolic-link prefix removal target" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removal_parent/removal-link"
assert_marker "$removal_link_referent" preview-native-arm64-fex
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removal_link_referent"
/bin/unlink "$removal_parent/removal-link"

removal_link_parent_referent="$TEST_ROOT/removal-link-parent-referent"
/bin/mkdir -m 0700 "$removal_link_parent_referent"
/bin/ln -s "$removal_link_parent_referent" "$TEST_ROOT/removal-link-parent"
expect_failure "symbolic-link prefix removal parent" 1 \
  switchyard_remove_runtime_prefix_offline preview-native-arm64-fex \
  "$TEST_ROOT/removal-link-parent/prefix"

unsafe_removal_ancestor="$TEST_ROOT/unsafe-removal-ancestor"
unsafe_removal_parent="$unsafe_removal_ancestor/owned-parent"
unsafe_removal_prefix="$unsafe_removal_parent/prefix"
/bin/mkdir -m 0777 "$unsafe_removal_ancestor"
/bin/chmod 0777 "$unsafe_removal_ancestor"
/bin/mkdir -m 0700 "$unsafe_removal_parent" "$unsafe_removal_prefix"
/usr/bin/printf '%s\n' \
  '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1}' \
  >"$unsafe_removal_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
/bin/chmod 0600 \
  "$unsafe_removal_prefix/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
expect_failure "mutable non-sticky removal ancestor" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$unsafe_removal_prefix"
assert_marker "$unsafe_removal_prefix" preview-native-arm64-fex
/bin/chmod 0700 "$unsafe_removal_ancestor"
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$unsafe_removal_prefix"

unsafe_nested_removal="$removal_parent/unsafe-nested-removal"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$unsafe_nested_removal" ignored
/bin/mkdir -m 0777 "$unsafe_nested_removal/mutable-child"
/bin/chmod 0777 "$unsafe_nested_removal/mutable-child"
/usr/bin/printf 'keep\n' >"$unsafe_nested_removal/mutable-child/sentinel"
expect_failure "mutable nested removal directory" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$unsafe_nested_removal"
assert_marker "$unsafe_nested_removal" preview-native-arm64-fex
[ "$(/bin/cat "$unsafe_nested_removal/mutable-child/sentinel")" = keep ] ||
  fail "unsafe nested removal changed the prefix"
/bin/chmod 0700 "$unsafe_nested_removal/mutable-child"
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$unsafe_nested_removal"

# A target created after the exclusive tombstone rename is never replaced or
# removed.  The untouched original remains recoverable at its private name.
removal_target_race="$removal_parent/target-race-prefix"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$removal_target_race" ignored
populate_many_files "$removal_target_race" 4000
removal_target_race_name_file="$TEST_ROOT/removal-target-race-name"
(
  shopt -s nullglob
  while :; do
    for tombstone in "$removal_parent"/.switchyard-prefix-removal.*; do
      [ -d "$tombstone" ] || continue
      /usr/bin/printf '%s\n' "$tombstone" >"$removal_target_race_name_file"
      /bin/mkdir -m 0700 "$removal_target_race"
      /usr/bin/printf 'racing target\n' >"$removal_target_race/sentinel"
      exit 0
    done
    /bin/sleep 0.001
  done
) &
removal_target_race_pid=$!
background_pids+=("$removal_target_race_pid")
expect_failure "target recreation after prefix tombstoning" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removal_target_race"
wait "$removal_target_race_pid"
background_pids=()
removal_target_race_tombstone="$(/bin/cat "$removal_target_race_name_file")"
[ "$(/bin/cat "$removal_target_race/sentinel")" = 'racing target' ] ||
  fail "prefix removal replaced a racing target"
assert_marker "$removal_target_race_tombstone" preview-native-arm64-fex
/bin/unlink "$removal_target_race/sentinel"
/bin/rmdir "$removal_target_race"
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removal_target_race_tombstone"
assert_no_removal_tombstone "$removal_parent"

# Swap the tombstone path while the helper performs its post-rename full state
# pass.  The pinned original must be left byte-for-byte intact and no entry may
# be deleted from the replacement directory.
removal_swap_prefix="$removal_parent/swap-prefix"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$removal_swap_prefix" ignored
populate_many_files "$removal_swap_prefix" 8000
removal_swap_before="$(tree_fingerprint "$removal_swap_prefix")"
removal_swap_name_file="$TEST_ROOT/removal-swap-name"
removal_swap_original="$removal_parent/swapped-original-prefix"
(
  shopt -s nullglob
  while :; do
    for tombstone in "$removal_parent"/.switchyard-prefix-removal.*; do
      [ -d "$tombstone" ] || continue
      /usr/bin/printf '%s\n' "$tombstone" >"$removal_swap_name_file"
      /bin/mv "$tombstone" "$removal_swap_original"
      /bin/mkdir -m 0700 "$tombstone"
      /usr/bin/printf 'replacement remains\n' >"$tombstone/sentinel"
      exit 0
    done
    /bin/sleep 0.001
  done
) &
removal_swap_pid=$!
background_pids+=("$removal_swap_pid")
expect_failure "tombstone path swap after rename" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removal_swap_prefix"
wait "$removal_swap_pid"
background_pids=()
removal_swap_replacement="$(/bin/cat "$removal_swap_name_file")"
[ "$(tree_fingerprint "$removal_swap_original")" = "$removal_swap_before" ] ||
  fail "post-rename tombstone swap deleted original prefix data"
[ "$(/bin/cat "$removal_swap_replacement/sentinel")" = 'replacement remains' ] ||
  fail "post-rename tombstone swap deleted replacement data"
assert_marker "$removal_swap_original" preview-native-arm64-fex
/bin/unlink "$removal_swap_replacement/sentinel"
/bin/rmdir "$removal_swap_replacement"
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$removal_swap_original"
assert_no_removal_tombstone "$removal_parent"

# Mutation after cleanup begins must stop at the changed inode, retaining the
# marker and remaining tree in a recoverable tombstone.
partial_prefix="$removal_parent/partial-prefix"
switchyard_prepare_runtime_prefix \
  preview-native-arm64-fex "$partial_prefix" ignored
populate_many_files "$partial_prefix" 8000
partial_name_file="$TEST_ROOT/partial-removal-name"
(
  shopt -s nullglob
  while :; do
    for tombstone in "$removal_parent"/.switchyard-prefix-removal.*; do
      [ -d "$tombstone" ] || continue
      [ ! -e "$tombstone/file-0000" ] || continue
      /usr/bin/printf '%s\n' "$tombstone" >"$partial_name_file"
      /bin/unlink "$tombstone/file-7999"
      /usr/bin/printf 'raced cleanup\n' >"$tombstone/file-7999"
      exit 0
    done
    /bin/sleep 0.001
  done
) &
partial_pid=$!
background_pids+=("$partial_pid")
expect_failure "mutation during prefix tombstone cleanup" 1 \
  switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$partial_prefix"
wait "$partial_pid"
background_pids=()
partial_tombstone="$(/bin/cat "$partial_name_file")"
[ ! -e "$partial_prefix" ] || fail "partial cleanup restored a changed prefix"
assert_marker "$partial_tombstone" preview-native-arm64-fex
[ "$(/bin/cat "$partial_tombstone/file-7999")" = 'raced cleanup' ] ||
  fail "partial cleanup removed the racing inode"
switchyard_remove_runtime_prefix_offline \
  preview-native-arm64-fex "$partial_tombstone"
assert_no_removal_tombstone "$removal_parent"

echo "runtime prefix policy tests passed"
