#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && /bin/pwd -P)"
# shellcheck source=lib/runtime_profile.sh
source "$ROOT_DIR/switchyard/lib/runtime_profile.sh"
# shellcheck source=lib/directory_safety.sh
source "$ROOT_DIR/switchyard/lib/directory_safety.sh"

OUTPUT=""
SOURCE_DIR="${SWITCHYARD_UNICORN_SOURCE_DIR:-}"
BUILD_DIR="${SWITCHYARD_UNICORN_BUILD_DIR:-}"
MINIMUM_MACOS="26.5"
CACHE_ROOT="${SWITCHYARD_UNICORN_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Unicorn}"
JOBS="${JOBS:-4}"
STAGING=""
BUILD_WORK_DIR=""
PATCHED_SOURCE_DIR=""
PATCH_SNAPSHOT=""
SWAP_HELPER_DIR=""
SOURCE_PATCH="$ROOT_DIR/switchyard/patches/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"

usage() {
  echo "usage: $0 --output PATH [--source-dir PATH] [--build-dir PATH] [--minimum-macos 26.5]" >&2
  exit 2
}

fail() {
  echo "Unicorn runtime: $*" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --output)
      [ "$#" -ge 2 ] || usage
      OUTPUT="$2"
      shift 2
      ;;
    --source-dir)
      [ "$#" -ge 2 ] || usage
      SOURCE_DIR="$2"
      shift 2
      ;;
    --build-dir)
      [ "$#" -ge 2 ] || usage
      BUILD_DIR="$2"
      shift 2
      ;;
    --minimum-macos)
      [ "$#" -ge 2 ] || usage
      MINIMUM_MACOS="$2"
      shift 2
      ;;
    *) usage ;;
  esac
done

[ -n "$OUTPUT" ] || usage
case "$OUTPUT" in
  /*) ;;
  *) echo "Unicorn output must be an absolute path." >&2; exit 2 ;;
esac
case "$MINIMUM_MACOS" in
  26.5) ;;
  *) echo "native Unicorn runtime requires a macOS 26.5 deployment target" >&2; exit 2 ;;
esac
case "$JOBS" in
  ''|*[!0-9]*|0) echo "Unicorn JOBS must be a positive integer." >&2; exit 2 ;;
esac
[ "$JOBS" -le 64 ] || {
  echo "Unicorn JOBS must not exceed 64." >&2
  exit 2
}
[ ! -L "$OUTPUT" ] || fail "output must not be a symbolic link: $OUTPUT"

for command in clang cmake git gzip lipo make nm otool perl python3 shasum tar vtool xcrun; do
  command -v "$command" >/dev/null 2>&1 || fail "missing required command: $command"
done

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk 'NR == 1 { print $1; exit }'
}

resolve_directory_parent() {
  local path="$1"
  local parent name

  parent="$(dirname "$path")"
  name="$(basename "$path")"
  mkdir -p "$parent"
  [ -d "$parent" ] && [ ! -L "$parent" ] || fail "unsafe path parent: $parent"
  parent="$(cd "$parent" && /bin/pwd -P)" || fail "cannot resolve path parent: $parent"
  printf '%s/%s\n' "$parent" "$name"
}

validate_tree_links() {
  /usr/bin/python3 -I - "$1" <<'PY'
import os
import stat
import sys

root = os.path.realpath(sys.argv[1])
info = os.lstat(root)
if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
    raise SystemExit("tree root is not a real directory")
for directory, directories, files in os.walk(root, followlinks=False):
    for name in directories + files:
        path = os.path.join(directory, name)
        info = os.lstat(path)
        if stat.S_ISLNK(info.st_mode):
            resolved = os.path.realpath(path)
            if os.path.commonpath((root, resolved)) != root:
                raise SystemExit("tree link escapes its root: " + path)
PY
}

validate_source_archive() {
  local archive="$1"

  /usr/bin/python3 -I - "$archive" <<'PY'
import pathlib
import tarfile
import sys

archive = sys.argv[1]
count = 0
total = 0
with tarfile.open(archive, "r:gz") as stream:
    for member in stream:
        count += 1
        total += member.size
        if member.size < 0:
            raise SystemExit("Unicorn source archive contains a negative entry size")
        if count > 200000 or total > 512 * 1024 * 1024:
            raise SystemExit("Unicorn source archive exceeds its resource bounds")
        path = pathlib.PurePosixPath(member.name)
        if (not member.name or path.is_absolute() or "\\" in member.name or
                any(part in ("", ".", "..") for part in path.parts)):
            raise SystemExit("unsafe Unicorn source archive path: " + member.name)
        if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
            raise SystemExit("unsupported Unicorn source archive entry: " + member.name)
        if member.issym() or member.islnk():
            if not member.linkname:
                raise SystemExit("empty Unicorn source archive link: " + member.name)
            target = pathlib.PurePosixPath(member.linkname)
            if target.is_absolute() or "\\" in member.linkname:
                raise SystemExit("unsafe Unicorn source archive link: " + member.name)
            combined = path.parent.joinpath(target) if member.issym() else target
            depth = 0
            for part in combined.parts:
                if part in ("", "."):
                    continue
                if part == "..":
                    depth -= 1
                else:
                    depth += 1
                if depth < 0:
                    raise SystemExit("escaping Unicorn source archive link: " + member.name)
PY
}

validate_source_patch() {
  local source_patch="$1"
  local size

  [ -f "$source_patch" ] && [ ! -L "$source_patch" ] ||
    fail "source patch is missing or unsafe: $source_patch"
  size="$(/usr/bin/stat -f '%z' "$source_patch" 2>/dev/null || true)"
  case "$size" in ''|*[!0-9]*) fail "source patch size is invalid" ;; esac
  [ "$size" -gt 0 ] && [ "$size" -le $((1024 * 1024)) ] ||
    fail "source patch exceeds its resource bound"
  [ "$(sha256_file "$source_patch")" = "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] ||
    fail "source patch does not match the closed-policy SHA-256"
}

source_checkout_is_clean() {
  local source="$1"

  [ -z "$(git -C "$source" status --porcelain --untracked-files=all)" ] &&
    [ -z "$(git -C "$source" ls-files --others --ignored --exclude-standard)" ]
}

write_unicorn_readme() {
  cat <<EOF
Switchyard native CPU-provider build dependency

Unicorn version: $SWITCHYARD_UNICORN_VERSION
Source: $SWITCHYARD_UNICORN_SOURCE_REPOSITORY
Revision: $SWITCHYARD_UNICORN_SOURCE_REVISION
Source patch: share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME
Source patch SHA-256: $SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256
Build contract: $SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION
Enabled emulation architecture: x86 (i386 and x86_64 modes)
Host architecture: arm64
Minimum macOS: $MINIMUM_MACOS

The development headers and pkg-config metadata stay in the private build
cache. Runtime packaging copies only the validated dylib closure and the
license, manifest, and corresponding-source materials.
EOF
}

write_unicorn_corresponding_source() {
  cat <<EOF
Unicorn corresponding source

Repository: $SWITCHYARD_UNICORN_SOURCE_REPOSITORY
Revision: $SWITCHYARD_UNICORN_SOURCE_REVISION
Version: $SWITCHYARD_UNICORN_VERSION
Archive: share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz
SHA-256: $SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256
Patch: share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME
Patch SHA-256: $SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256

The archive is a deterministic pristine export of the pinned upstream Git
revision. Apply the adjacent patch exactly, without fuzz, to reproduce the
source used for libunicorn.2.dylib. Preserve both corresponding-source inputs
and the GPL, GLib, and QEMU notices when redistributing the native CPU-provider
dependency.
EOF
}

validate_output() {
  local root="$1"
  local metadata="$root/switchyard-unicorn-runtime.json"
  local dylib="$root/lib/libunicorn.2.dylib"
  local dylib_link="$root/lib/libunicorn.dylib"
  local source_archive="$root/share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz"
  local source_patch="$root/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"
  local notice_root="$root/share/doc/switchyard-unicorn"
  local actual dependency notice directory development_file

  [ -d "$root" ] && [ ! -L "$root" ] || return 1
  validate_tree_links "$root" || return 1
  for directory in \
      "$root/lib" \
      "$root/lib/pkgconfig" \
      "$root/include" \
      "$root/include/unicorn" \
      "$root/share" \
      "$root/share/doc" \
      "$notice_root" \
      "$root/share/src" \
      "$root/share/src/switchyard-unicorn"; do
    [ -d "$directory" ] && [ ! -L "$directory" ] || return 1
  done
  [ -f "$metadata" ] && [ ! -L "$metadata" ] || return 1
  [ -f "$dylib" ] && [ ! -L "$dylib" ] || return 1
  [ -L "$dylib_link" ] && [ "$(readlink "$dylib_link")" = "libunicorn.2.dylib" ] || return 1
  [ -f "$source_archive" ] && [ ! -L "$source_archive" ] || return 1
  [ -f "$source_patch" ] && [ ! -L "$source_patch" ] || return 1
  [ -z "$(find "$root/lib" -type f \( -name '*.a' -o -name '*.o' \) -print -quit)" ] || return 1
  actual="$(/usr/bin/stat -f '%z' "$dylib" 2>/dev/null || true)"
  case "$actual" in ''|*[!0-9]*) return 1 ;; esac
  [ "$actual" -gt 0 ] && [ "$actual" -le $((128 * 1024 * 1024)) ] || return 1
  actual="$(/usr/bin/stat -f '%z' "$source_archive" 2>/dev/null || true)"
  case "$actual" in ''|*[!0-9]*) return 1 ;; esac
  [ "$actual" -gt 0 ] && [ "$actual" -le $((64 * 1024 * 1024)) ] || return 1
  actual="$(/usr/bin/stat -f '%z' "$source_patch" 2>/dev/null || true)"
  case "$actual" in ''|*[!0-9]*) return 1 ;; esac
  [ "$actual" -gt 0 ] && [ "$actual" -le $((1024 * 1024)) ] || return 1
  for development_file in \
      "$root/include/unicorn/unicorn.h" \
      "$root/include/unicorn/x86.h" \
      "$root/lib/pkgconfig/unicorn.pc"; do
    [ -f "$development_file" ] && [ ! -L "$development_file" ] || return 1
  done
  /usr/bin/grep -Fx '#define UC_SWITCHYARD_INSTRUCTION_BOUNDARY_STOP 1' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  /usr/bin/grep -Fx 'uc_err uc_emu_stop_at_instruction_boundary(uc_engine *uc);' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  /usr/bin/grep -Fx '#define UC_SWITCHYARD_SHARED_MEMORY_ATOMICS 1' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  /usr/bin/grep -Fx '#define UC_SWITCHYARD_SHARED_CODE_COHERENCE 1' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  /usr/bin/grep -Fx '#define UC_SWITCHYARD_SHARED_MEMORY_ATOMIC_TRACE 1' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  /usr/bin/grep -Fx 'uc_err uc_enable_shared_memory_atomics(uc_engine *uc);' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  /usr/bin/grep -Fx 'uc_err uc_set_shared_memory_atomic_callback(' \
    "$root/include/unicorn/unicorn.h" >/dev/null || return 1
  [ "$(nm -gU "$dylib" | /usr/bin/awk \
      '$NF == "_uc_emu_stop_at_instruction_boundary" { count++ } END { print count + 0 }')" -eq 1 ] ||
    return 1
  [ "$(nm -gU "$dylib" | /usr/bin/awk \
      '$NF == "_uc_enable_shared_memory_atomics" { count++ } END { print count + 0 }')" -eq 1 ] ||
    return 1
  [ "$(nm -gU "$dylib" | /usr/bin/awk \
      '$NF == "_uc_set_shared_memory_atomic_callback" { count++ } END { print count + 0 }')" -eq 1 ] ||
    return 1
  /usr/bin/grep -Fx 'libdir=${pcfiledir}/..' "$root/lib/pkgconfig/unicorn.pc" >/dev/null || return 1
  /usr/bin/grep -Fx 'includedir=${pcfiledir}/../../include' \
    "$root/lib/pkgconfig/unicorn.pc" >/dev/null || return 1
  if /usr/bin/grep -E '/Users/|/opt/homebrew|/usr/local' \
       "$root/lib/pkgconfig/unicorn.pc" >/dev/null 2>&1; then
    return 1
  fi
  for notice in README.txt CORRESPONDING-SOURCE.txt COPYING COPYING.LGPL2 COPYING_GLIB \
      QEMU-COPYING QEMU-COPYING.LIB QEMU-LICENSE; do
    [ -f "$notice_root/$notice" ] && [ ! -L "$notice_root/$notice" ] || return 1
    actual="$(/usr/bin/stat -f '%z' "$notice_root/$notice" 2>/dev/null || true)"
    case "$actual" in ''|*[!0-9]*) return 1 ;; esac
    [ "$actual" -gt 0 ] && [ "$actual" -le $((4 * 1024 * 1024)) ] || return 1
  done
  /usr/bin/cmp -s <(write_unicorn_readme) "$notice_root/README.txt" || return 1
  /usr/bin/cmp -s <(write_unicorn_corresponding_source) \
    "$notice_root/CORRESPONDING-SOURCE.txt" || return 1
  /usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" verify "$root" >/dev/null || return 1
  [ "$(/usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" digest "$root")" = \
    "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" ] || return 1
  /usr/bin/python3 -I - "$metadata" "$SWITCHYARD_UNICORN_VERSION" \
      "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" "$SWITCHYARD_UNICORN_SOURCE_REVISION" \
      "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" \
      "$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" \
      "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" \
      "$SWITCHYARD_UNICORN_LIBRARY_SHA256" \
      "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" "$MINIMUM_MACOS" <<'PY' || return 1
import json
import os
import sys

(
    metadata,
    version,
    repository,
    revision,
    archive_sha,
    patch_basename,
    patch_sha,
    library_sha,
    contract,
    minimum,
) = sys.argv[1:]

def object_without_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON object key")
        value[key] = item
    return value

if os.path.getsize(metadata) > 1024 * 1024:
    raise ValueError("Unicorn metadata exceeds the validation size limit")
with open(metadata, "r", encoding="utf-8") as stream:
    value = json.load(stream, object_pairs_hook=object_without_duplicates)
expected = {
    "version": version,
    "sourceRepository": repository,
    "sourceRevision": revision,
    "buildContractVersion": int(contract),
    "enabledArchitectures": ["x86"],
    "hostArchitecture": "arm64",
    "minimumMacOS": minimum,
    "library": "lib/libunicorn.2.dylib",
    "sourceArchive": f"share/src/switchyard-unicorn/unicorn-{revision}.tar.gz",
    "sourceArchiveSha256": archive_sha,
    "sourcePatch": {
        "path": "share/src/switchyard-unicorn/" + patch_basename,
        "sha256": patch_sha,
    },
    "license": "GPL-2.0-only with separately licensed GLib/QEMU components; preserve all included notices and corresponding source",
}
if type(value) is not dict or set(value) != set(expected) | {"librarySha256"}:
    raise ValueError("Unicorn metadata has an unexpected field set")
for key, wanted in expected.items():
    if value.get(key) != wanted or type(value.get(key)) is not type(wanted):
        raise ValueError("Unicorn metadata field is invalid: " + key)
if value.get("librarySha256") != library_sha:
    raise ValueError("Unicorn metadata library digest is invalid")
PY
  actual="$(/usr/bin/plutil -extract librarySha256 raw -o - "$metadata" 2>/dev/null || true)"
  [ "$actual" = "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ] || return 1
  [ "$(sha256_file "$dylib")" = "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ] || return 1
  [ "$(sha256_file "$source_archive")" = "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" ] || return 1
  [ "$(sha256_file "$source_patch")" = "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] || return 1
  /usr/bin/cmp -s "$source_patch" "$SOURCE_PATCH" || return 1
  validate_source_archive "$source_archive" || return 1
  [ "$(/usr/bin/gzip -dc "$source_archive" | git get-tar-commit-id 2>/dev/null || true)" = \
    "$SWITCHYARD_UNICORN_SOURCE_REVISION" ] || return 1
  [ "$(lipo -archs "$dylib")" = "arm64" ] || return 1
  [ "$(otool -D "$dylib" | /usr/bin/tail -n 1)" = "@rpath/libunicorn.2.dylib" ] || return 1
  actual="$(vtool -arch arm64 -show-build "$dylib" 2>/dev/null)" || return 1
  [ "$(/usr/bin/grep -c 'cmd LC_BUILD_VERSION' <<<"$actual")" -eq 1 ] || return 1
  [ "$(/usr/bin/grep -c 'cmd LC_VERSION_MIN_MACOSX' <<<"$actual")" -eq 0 ] || return 1
  /usr/bin/grep -F 'platform MACOS' <<<"$actual" >/dev/null || return 1
  /usr/bin/grep -E "^[[:space:]]*minos ${MINIMUM_MACOS//./\\.}([.]0)*$" <<<"$actual" >/dev/null || return 1
  /usr/bin/grep -E "^[[:space:]]*sdk ${MINIMUM_MACOS//./\\.}([.]0)*$" <<<"$actual" >/dev/null || return 1
  while IFS= read -r dependency; do
    case "$dependency" in
      @rpath/libunicorn.2.dylib|/usr/lib/*|/System/Library/*) ;;
      *) return 1 ;;
    esac
  done < <(otool -L "$dylib" | /usr/bin/awk 'NR > 1 { print $1 }')
  [ -z "$(otool -l "$dylib" | /usr/bin/awk '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')" ] || return 1
}

validate_source_patch "$SOURCE_PATCH"
OUTPUT="$(resolve_directory_parent "$OUTPUT")"
if [ -e "$OUTPUT" ]; then
  if validate_output "$OUTPUT"; then
    echo "$OUTPUT"
    exit 0
  fi
  fail "existing output is incomplete or inconsistent: $OUTPUT"
fi

CACHE_ROOT="$(resolve_directory_parent "$CACHE_ROOT/.cache-root")"
CACHE_ROOT="$(dirname "$CACHE_ROOT")"
if [ -z "$SOURCE_DIR" ]; then
  SOURCE_DIR="$CACHE_ROOT/source-$SWITCHYARD_UNICORN_SOURCE_REVISION"
else
  case "$SOURCE_DIR" in /*) ;; *) fail "source directory must be absolute" ;; esac
  [ ! -L "$SOURCE_DIR" ] || fail "source directory must not be a symbolic link"
fi
if [ -z "$BUILD_DIR" ]; then
  BUILD_DIR="$CACHE_ROOT/build-arm64-x86-${SWITCHYARD_UNICORN_SOURCE_REVISION}-build${SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION}"
else
  case "$BUILD_DIR" in /*) ;; *) fail "build directory must be absolute" ;; esac
  [ ! -L "$BUILD_DIR" ] || fail "build directory must not be a symbolic link"
fi

if [ ! -e "$SOURCE_DIR" ]; then
  git clone --filter=blob:none --depth 1 --branch "v$SWITCHYARD_UNICORN_VERSION" \
    "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" "$SOURCE_DIR"
fi
[ -d "$SOURCE_DIR/.git" ] && [ ! -L "$SOURCE_DIR" ] || fail "source is not a real Git checkout: $SOURCE_DIR"
SOURCE_DIR="$(cd "$SOURCE_DIR" && /bin/pwd -P)" || fail "cannot resolve source directory"
[ "$(git -C "$SOURCE_DIR" rev-parse HEAD)" = "$SWITCHYARD_UNICORN_SOURCE_REVISION" ] ||
  fail "source is not the pinned revision $SWITCHYARD_UNICORN_SOURCE_REVISION"
source_checkout_is_clean "$SOURCE_DIR" ||
  fail "source checkout is dirty: $SOURCE_DIR"
case "$OUTPUT" in
  "$SOURCE_DIR"|"$SOURCE_DIR"/*)
    fail "output must not overlap the pinned source checkout: $OUTPUT"
    ;;
esac
source_date_epoch="$(git -C "$SOURCE_DIR" show -s --format=%ct "$SWITCHYARD_UNICORN_SOURCE_REVISION")"
case "$source_date_epoch" in ''|*[!0-9]*) fail "source commit timestamp is invalid" ;; esac

sdk_path="$(xcrun --sdk macosx --show-sdk-path)" || fail "cannot resolve the macOS SDK"
sdk_version="$(xcrun --sdk macosx --show-sdk-version)" || fail "cannot resolve the macOS SDK version"
[ "$sdk_version" = "$MINIMUM_MACOS" ] || fail "macOS SDK $MINIMUM_MACOS is required, not $sdk_version"
[ -d "$sdk_path" ] || fail "macOS SDK path is missing: $sdk_path"
sdk_path="$(cd "$sdk_path" && /bin/pwd -P)" || fail "cannot resolve macOS SDK path"
[ -d "$sdk_path" ] && [ ! -L "$sdk_path" ] || fail "resolved macOS SDK path is unsafe: $sdk_path"

BUILD_DIR="$(resolve_directory_parent "$BUILD_DIR/.build-root")"
BUILD_DIR="$(dirname "$BUILD_DIR")"
[ -d "$BUILD_DIR" ] && [ ! -L "$BUILD_DIR" ] || fail "build root is unsafe: $BUILD_DIR"
case "$BUILD_DIR" in
  /|"$HOME"|"$SOURCE_DIR"|"$SOURCE_DIR"/*)
    fail "build root is too broad or overlaps the source: $BUILD_DIR"
    ;;
esac

export LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH="$source_date_epoch" ZERO_AR_DATE=1
export MACOSX_DEPLOYMENT_TARGET="$MINIMUM_MACOS"
unset CC CFLAGS CPPFLAGS CXXFLAGS LDFLAGS SDKROOT
umask 022

cleanup() {
  if [ -n "$SWAP_HELPER_DIR" ] && [ -d "$SWAP_HELPER_DIR" ] &&
     [ ! -L "$SWAP_HELPER_DIR" ]; then
    /bin/rm -rf "$SWAP_HELPER_DIR"
  fi
  if [ -n "$PATCH_SNAPSHOT" ] && [ -f "$PATCH_SNAPSHOT" ] && [ ! -L "$PATCH_SNAPSHOT" ]; then
    /bin/rm -f "$PATCH_SNAPSHOT"
  fi
  if [ -n "$PATCHED_SOURCE_DIR" ] && [ -d "$PATCHED_SOURCE_DIR" ] &&
     [ ! -L "$PATCHED_SOURCE_DIR" ]; then
    /bin/rm -rf "$PATCHED_SOURCE_DIR"
  fi
  if [ -n "$BUILD_WORK_DIR" ] && [ -d "$BUILD_WORK_DIR" ] && [ ! -L "$BUILD_WORK_DIR" ]; then
    /bin/rm -rf "$BUILD_WORK_DIR"
  fi
  if [ -n "$STAGING" ] && [ -d "$STAGING" ] && [ ! -L "$STAGING" ]; then
    /bin/rm -rf "$STAGING"
  fi
}
trap cleanup EXIT HUP INT TERM
STAGING="$(mktemp -d "$(dirname "$OUTPUT")/.unicorn-staging.XXXXXX")"
BUILD_WORK_DIR="$(mktemp -d "$BUILD_DIR/.build.XXXXXX")"
PATCHED_SOURCE_DIR="$(mktemp -d "$BUILD_DIR/.source.XXXXXX")"
PATCH_SNAPSHOT="$(mktemp "$BUILD_DIR/.patch.XXXXXX")"
install -m 0600 "$SOURCE_PATCH" "$PATCH_SNAPSHOT"
validate_source_patch "$PATCH_SNAPSHOT"

# Build only from a private export of the pristine pinned checkout.  `git apply`
# rejects malformed paths and fuzzy hunks; the reverse check proves that the
# exact closed patch, and no alternate source state, produced the build tree.
git -C "$SOURCE_DIR" archive --format=tar HEAD | /usr/bin/tar -xf - -C "$PATCHED_SOURCE_DIR"
validate_tree_links "$PATCHED_SOURCE_DIR" || fail "private source export has unsafe links"
# The build root may itself be inside an excluded repository directory.  In
# that case Git otherwise discovers the parent worktree, skips every patch path
# as outside the current prefix, and still exits successfully.  Treat the
# private export as a repository-independent tree so apply and reverse-check
# operate on its files rather than on ambient Git state.
patch_apply_ceiling="$(dirname "$PATCHED_SOURCE_DIR")"
GIT_CEILING_DIRECTORIES="$patch_apply_ceiling" \
  git -C "$PATCHED_SOURCE_DIR" apply --check --whitespace=error-all "$PATCH_SNAPSHOT"
GIT_CEILING_DIRECTORIES="$patch_apply_ceiling" \
  git -C "$PATCHED_SOURCE_DIR" apply --whitespace=error-all "$PATCH_SNAPSHOT"
GIT_CEILING_DIRECTORIES="$patch_apply_ceiling" \
  git -C "$PATCHED_SOURCE_DIR" apply --reverse --check --whitespace=error-all "$PATCH_SNAPSHOT"
/usr/bin/grep -Fx '#define UC_SWITCHYARD_INSTRUCTION_BOUNDARY_STOP 1' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the instruction-boundary stop contract"
/usr/bin/grep -Fx 'uc_err uc_emu_stop_at_instruction_boundary(uc_engine *uc);' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the instruction-boundary stop API"
/usr/bin/grep -Fx '#define UC_SWITCHYARD_SHARED_MEMORY_ATOMICS 1' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the shared-memory atomic contract"
/usr/bin/grep -Fx '#define UC_SWITCHYARD_SHARED_CODE_COHERENCE 1' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the shared-code coherence contract"
/usr/bin/grep -Fx '#define UC_SWITCHYARD_SHARED_MEMORY_ATOMIC_TRACE 1' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the serial-atomic trace contract"
/usr/bin/grep -Fx 'uc_err uc_enable_shared_memory_atomics(uc_engine *uc);' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the shared-memory atomic API"
/usr/bin/grep -Fx 'uc_err uc_set_shared_memory_atomic_callback(' \
  "$PATCHED_SOURCE_DIR/include/unicorn/unicorn.h" >/dev/null ||
  fail "source patch did not add the serial-atomic trace API"
/usr/bin/grep -F '__atomic_load_n(&x, __ATOMIC_RELAXED)' \
  "$PATCHED_SOURCE_DIR/qemu/configure" >/dev/null ||
  fail "source patch did not correct the 64-bit atomic capability probe"
validate_tree_links "$PATCHED_SOURCE_DIR" || fail "patched private source has unsafe links"

reproducible_source_root="/usr/src/unicorn-$SWITCHYARD_UNICORN_SOURCE_REVISION"
reproducible_c_flags="-ffile-prefix-map=${PATCHED_SOURCE_DIR}=${reproducible_source_root}"
reproducible_c_flags+=" -ffile-prefix-map=${BUILD_WORK_DIR}=${reproducible_source_root}/.build"
cmake_args=(
  -S "$PATCHED_SOURCE_DIR"
  -B "$BUILD_WORK_DIR"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=/usr/bin/clang
  -DCMAKE_C_FLAGS="$reproducible_c_flags"
  "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG"
  -DCMAKE_INSTALL_PREFIX="$STAGING"
  -DCMAKE_MAKE_PROGRAM=/usr/bin/make
  -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINIMUM_MACOS"
  -DCMAKE_OSX_SYSROOT="$sdk_path"
  -DUNICORN_ARCH=x86
  -DUNICORN_BUILD_TESTS=OFF
  -DUNICORN_INSTALL=ON
  -G "Unix Makefiles"
)
cmake "${cmake_args[@]}"
cmake --build "$BUILD_WORK_DIR" --parallel "$JOBS"
/usr/bin/grep -Fx '#define CONFIG_ATOMIC64 1' \
  "$BUILD_WORK_DIR/config-host.h" >/dev/null ||
  fail "Unicorn build did not enable the required 64-bit atomic helpers"

# Exercise AArch64 code generation, cross-thread publication,
# instruction-boundary stopping, cross-engine shared-memory atomicity, and
# atomic invalid-memory recovery and cross-engine executable-code publication
# against the exact dylib that will be installed.  The tests cover zero-count
# i32/i64 rotate lowering, LOCK CMPXCHG, CMPXCHG8B, demand mapping, repeated
# shared-code writes, concurrent emulation startup, caller-thread Apple JIT
# state preservation, and an interruptible REP iteration in addition to the
# ordinary cross-thread stop path.
for regression in aarch64_rotl_zero apple_jit_state threaded_emu_stop threaded_emu_stop_atomic shared_memory_atomics atomic_unmapped_hook shared_code_coherence shared_code_start_race shared_code_jit_state; do
  regression_source="$PATCHED_SOURCE_DIR/tests/regress/$regression.c"
  regression_binary="$BUILD_WORK_DIR/switchyard-$regression"
  [ -f "$regression_source" ] && [ ! -L "$regression_source" ] ||
    fail "patched source is missing regression: $regression"
  if [ "$regression" = "apple_jit_state" ]; then
    regression_cflags=(
      -DHAVE_PTHREAD_JIT_PROTECT=1
      -I"$PATCHED_SOURCE_DIR"
      -I"$PATCHED_SOURCE_DIR/qemu/include"
      -I"$PATCHED_SOURCE_DIR/include"
    )
  else
    regression_cflags=(-I"$PATCHED_SOURCE_DIR/include")
  fi
  /usr/bin/clang -arch arm64 -mmacosx-version-min="$MINIMUM_MACOS" \
    -std=c11 -Wall -Wextra -Werror \
    "${regression_cflags[@]}" "$regression_source" \
    -L"$BUILD_WORK_DIR" '-Wl,-rpath,@loader_path' -lunicorn -lpthread \
    -o "$regression_binary"
  "$regression_binary" || fail "Unicorn regression failed: $regression"
done

cmake --install "$BUILD_WORK_DIR"
source_checkout_is_clean "$SOURCE_DIR" ||
  fail "build modified the pinned source checkout"

dylib="$STAGING/lib/libunicorn.2.dylib"
[ -f "$dylib" ] && [ ! -L "$dylib" ] || fail "install did not produce libunicorn.2.dylib"
[ "$(lipo -archs "$dylib")" = "arm64" ] || fail "runtime is not thin arm64"
[ "$(otool -D "$dylib" | /usr/bin/tail -n 1)" = "@rpath/libunicorn.2.dylib" ] ||
  fail "runtime has an unexpected install name"

/bin/rm -f "$STAGING/lib/libunicorn.a" "$STAGING/lib/unicorn.o"
mkdir -p "$STAGING/share/doc/switchyard-unicorn" "$STAGING/share/src/switchyard-unicorn"
for notice in COPYING COPYING.LGPL2 COPYING_GLIB; do
  install -m 0644 "$SOURCE_DIR/$notice" "$STAGING/share/doc/switchyard-unicorn/$notice"
done
install -m 0644 "$SOURCE_DIR/qemu/COPYING" "$STAGING/share/doc/switchyard-unicorn/QEMU-COPYING"
install -m 0644 "$SOURCE_DIR/qemu/COPYING.LIB" "$STAGING/share/doc/switchyard-unicorn/QEMU-COPYING.LIB"
install -m 0644 "$SOURCE_DIR/qemu/LICENSE" "$STAGING/share/doc/switchyard-unicorn/QEMU-LICENSE"

source_archive="$STAGING/share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz"
git -C "$SOURCE_DIR" archive --format=tar \
  --prefix="unicorn-$SWITCHYARD_UNICORN_VERSION/" HEAD | /usr/bin/gzip -n -9 >"$source_archive"
[ "$(sha256_file "$source_archive")" = "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" ] ||
  fail "deterministic source archive does not match the closed-policy SHA-256"
validate_source_archive "$source_archive" || fail "source archive path validation failed"
install -m 0644 "$PATCH_SNAPSHOT" \
  "$STAGING/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"

/usr/bin/perl -0pi -e '
  s{^prefix=.*$}{prefix=\${pcfiledir}/../..}m;
  s{^libdir=.*$}{libdir=\${pcfiledir}/..}m;
  s{^includedir=.*$}{includedir=\${pcfiledir}/../../include}m;
' "$STAGING/lib/pkgconfig/unicorn.pc"

library_sha="$(sha256_file "$dylib")"
[ "$library_sha" = "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ] ||
  fail "built dylib SHA-256 $library_sha does not match the closed policy"
cat >"$STAGING/switchyard-unicorn-runtime.json" <<EOF
{
  "version": "$SWITCHYARD_UNICORN_VERSION",
  "sourceRepository": "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY",
  "sourceRevision": "$SWITCHYARD_UNICORN_SOURCE_REVISION",
  "buildContractVersion": $SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION,
  "enabledArchitectures": ["x86"],
  "hostArchitecture": "arm64",
  "minimumMacOS": "$MINIMUM_MACOS",
  "library": "lib/libunicorn.2.dylib",
  "librarySha256": "$library_sha",
  "sourceArchive": "share/src/switchyard-unicorn/$(basename "$source_archive")",
  "sourceArchiveSha256": "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256",
  "sourcePatch": {
    "path": "share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME",
    "sha256": "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256"
  },
  "license": "GPL-2.0-only with separately licensed GLib/QEMU components; preserve all included notices and corresponding source"
}
EOF
write_unicorn_readme >"$STAGING/share/doc/switchyard-unicorn/README.txt"
write_unicorn_corresponding_source \
  >"$STAGING/share/doc/switchyard-unicorn/CORRESPONDING-SOURCE.txt"

/usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" write "$STAGING" >/dev/null
staged_development_digest="$(
  /usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" digest "$STAGING" 2>/dev/null
)" || fail "cannot compute staged development runtime digest"
[ "$staged_development_digest" = "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" ] ||
  fail "staged development runtime digest $staged_development_digest does not match the closed policy"
validate_output "$STAGING" || fail "staged development runtime failed final validation"
ensure_preview_swap_helper || fail "cannot prepare the exclusive output publisher"
publication_status=0
"$SWAP_HELPER_DIR/switchyard-preview-directory-publish" \
  "$STAGING" "$OUTPUT" exclusive || publication_status=$?
if [ "$publication_status" -ne 0 ]; then
  # Status 3 is fail-stop: rollback could not prove which inode occupies the
  # old staging name, so path-based EXIT cleanup must preserve both names.
  [ "$publication_status" -ne 3 ] || STAGING=""
  fail "exclusive output publication failed: $OUTPUT"
fi
STAGING=""
echo "$OUTPUT"
