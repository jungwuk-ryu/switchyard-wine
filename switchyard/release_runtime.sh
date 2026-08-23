#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/switchyard/lib/runtime_profile.sh"
# Native release orchestration deliberately consumes the producer-owned
# manifest and validators instead of duplicating their signed identity schema.
source "$ROOT_DIR/switchyard/lib/native_arm64_packaging.sh"
source "$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
source "$ROOT_DIR/switchyard/lib/dxmt_artifact.sh"
source "$ROOT_DIR/switchyard/lib/macho_signing.sh"
source "$ROOT_DIR/switchyard/lib/runtime_prefix.sh"
ENTITLEMENTS=""
EXPECTED_TEAM_ID="${SWITCHYARD_DEVELOPER_TEAM_ID:-M3CULMDKU3}"
RUNTIME=""
OUTPUT_DIR=""
IDENTITY="${SWITCHYARD_CODESIGN_IDENTITY:-}"
NOTARY_PROFILE="${SWITCHYARD_NOTARY_PROFILE:-}"
EXPECTED_RUNTIME_DIGEST="${SWITCHYARD_RUNTIME_CONTENT_SHA256:-}"
manifest_snapshot=""
mach_o_list=""
verification_log=""
prefix=""
signed_runtime=""
runtime_profile=""
entitlements_snapshot_fd=""
native_release_private_root=""
native_release_complete=0
native_archive_public_path=""
native_archive_public_identity=""
native_checksum_public_path=""
native_checksum_public_identity=""
native_manifest_public_path=""
native_manifest_public_identity=""
CODESIGN_TOOL="/usr/bin/codesign"
NATIVE_SMOKE_TOOL="$ROOT_DIR/switchyard/tests/native_no_rosetta_process_test.sh"

remove_owned_native_release_output() {
  local path="$1"
  local expected_identity="$2"

  [ -n "$path" ] && [ -n "$expected_identity" ] || return 0
  /usr/bin/python3 -I - "$path" "$expected_identity" <<'PY'
import os
import stat
import sys

path, expected = sys.argv[1:]
try:
    expected_device, expected_inode = (int(item) for item in expected.split(":"))
except (TypeError, ValueError):
    raise SystemExit(1)
try:
    current = os.lstat(path)
except FileNotFoundError:
    raise SystemExit(0)
if (
    not stat.S_ISREG(current.st_mode)
    or (current.st_dev, current.st_ino) != (expected_device, expected_inode)
):
    raise SystemExit(1)
os.unlink(path)
PY
}

remove_native_release_private_root() {
  [ -n "$native_release_private_root" ] || return 0
  case "$native_release_private_root" in
    "$OUTPUT_DIR"/.switchyard-native-release.??????)
      [ -d "$native_release_private_root" ] &&
        [ ! -L "$native_release_private_root" ] || return 1
      /bin/rm -rf -- "$native_release_private_root"
      native_release_private_root=""
      signed_runtime=""
      ;;
    *)
      echo "refusing to clean unexpected native release staging root: $native_release_private_root" >&2
      return 1
      ;;
  esac
}

cleanup() {
  if [ "${runtime_profile:-}" = preview-native-arm64-fex ]; then
    if [ -n "$entitlements_snapshot_fd" ]; then
      close_validated_entitlements_snapshot "$entitlements_snapshot_fd" >/dev/null 2>&1 ||
        exec 19<&-
      entitlements_snapshot_fd=""
    fi
    if [ -n "$prefix" ]; then
      if switchyard_stop_native_release_wineserver "$signed_runtime" "$prefix" &&
         switchyard_remove_runtime_prefix_offline preview-native-arm64-fex "$prefix"; then
        prefix=""
      else
        echo "native release cleanup could not safely remove its prepared prefix" >&2
      fi
    fi
    if [ "$native_release_complete" -eq 0 ]; then
      remove_owned_native_release_output \
        "$native_archive_public_path" "$native_archive_public_identity" || true
      remove_owned_native_release_output \
        "$native_checksum_public_path" "$native_checksum_public_identity" || true
      remove_owned_native_release_output \
        "$native_manifest_public_path" "$native_manifest_public_identity" || true
    fi
    if [ -z "$prefix" ]; then
      remove_native_release_private_root || true
    fi
  elif [ -n "$prefix" ]; then
    if [ -n "$signed_runtime" ]; then
      WINEPREFIX="$prefix" "$signed_runtime/bin/wineserver" -k >/dev/null 2>&1 || true
    fi
    /bin/rm -rf "$prefix"
  fi
  [ -z "$manifest_snapshot" ] || /bin/rm -f "$manifest_snapshot"
  [ -z "$mach_o_list" ] || /bin/rm -f "$mach_o_list"
  [ -z "$verification_log" ] || /bin/rm -f "$verification_log"
}

switchyard_validate_native_release_output_root() {
  /usr/bin/python3 -I - "$1" <<'PY'
import os
import stat
import sys

path = sys.argv[1]
if (
    not os.path.isabs(path)
    or os.path.normpath(path) != path
    or path == "/"
    or os.path.realpath(path) != path
):
    raise SystemExit("native release output is not a bounded canonical absolute path")

descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
try:
    for component in path.split("/")[1:]:
        child = os.open(
            component,
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
            dir_fd=descriptor,
        )
        info = os.fstat(child)
        if (
            info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and not info.st_mode & stat.S_ISVTX
        ):
            os.close(child)
            raise SystemExit("native release output has an unsafe writable ancestor")
        os.close(descriptor)
        descriptor = child
    info = os.fstat(descriptor)
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise SystemExit("native release output has an unsafe owner or mode")
finally:
    os.close(descriptor)
PY
}

switchyard_native_release_file_identity() {
  /usr/bin/python3 -I - "$1" <<'PY'
import os
import stat
import sys

info = os.lstat(sys.argv[1])
if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
    raise SystemExit(1)
print(f"{info.st_dev}:{info.st_ino}")
PY
}

switchyard_publish_native_release_file_atomically() {
  /usr/bin/python3 -I - "$1" "$2" <<'PY'
import ctypes
import errno
import os
import stat
import sys

source, destination = sys.argv[1:]
RENAME_EXCL = 0x00000004
flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC

if any(
    not os.path.isabs(path)
    or os.path.normpath(path) != path
    or path == "/"
    or os.path.realpath(os.path.dirname(path)) != os.path.dirname(path)
    for path in (source, destination)
):
    raise SystemExit("native release publication path is not canonical")

def open_directory(path):
    descriptor = os.open("/", flags)
    try:
        for component in path.split("/")[1:]:
            child = os.open(component, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise

source_parent, source_name = os.path.split(source)
destination_parent, destination_name = os.path.split(destination)
if not source_name or not destination_name:
    raise SystemExit("native release publication filename is invalid")
source_fd = open_directory(source_parent)
destination_fd = open_directory(destination_parent)
try:
    source_info = os.stat(source_name, dir_fd=source_fd, follow_symlinks=False)
    if (
        not stat.S_ISREG(source_info.st_mode)
        or source_info.st_uid != os.geteuid()
        or source_info.st_nlink != 1
    ):
        raise SystemExit("native release publication source is unsafe")
    try:
        os.stat(destination_name, dir_fd=destination_fd, follow_symlinks=False)
    except FileNotFoundError:
        pass
    else:
        raise SystemExit("native release publication destination already exists")

    libc = ctypes.CDLL(None, use_errno=True)
    renameatx_np = libc.renameatx_np
    renameatx_np.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    renameatx_np.restype = ctypes.c_int
    if renameatx_np(
        source_fd,
        os.fsencode(source_name),
        destination_fd,
        os.fsencode(destination_name),
        RENAME_EXCL,
    ) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), destination)
    os.fsync(destination_fd)
    published = os.stat(destination_name, dir_fd=destination_fd, follow_symlinks=False)
    if (published.st_dev, published.st_ino) != (source_info.st_dev, source_info.st_ino):
        raise SystemExit("native release publication changed file identity")
finally:
    os.close(destination_fd)
    os.close(source_fd)
PY
}

switchyard_publish_native_release_file() {
  switchyard_publish_native_release_file_atomically "$@"
}

switchyard_validate_native_release_runtime() {
  local runtime_root="$1"
  local runtime_manifest="$2"

  switchyard_validate_runtime_manifest_profile \
    "$runtime_manifest" preview-native-arm64-fex "$runtime_root" &&
    switchyard_validate_native_arm64_runtime_packaging \
      "$runtime_root" "$runtime_manifest" "$ROOT_DIR"
}

switchyard_verify_native_release_team() {
  local item="$1"
  local signing_details

  signing_details="$("$CODESIGN_TOOL" -d --verbose=4 "$item" 2>&1)" || {
    echo "cannot inspect native release signature: $item" >&2
    return 1
  }
  [ "$(/usr/bin/printf '%s\n' "$signing_details" |
      /usr/bin/grep -Fxc "TeamIdentifier=$EXPECTED_TEAM_ID")" -eq 1 ] || {
    echo "native release Mach-O has an unexpected Developer Team ID: $item" >&2
    return 1
  }
}

switchyard_sign_native_release_runtime() {
  local entry_count=0
  local item
  local mach_o_count
  local wine_entry="$signed_runtime/lib/wine/aarch64-unix/wine"
  local fallback_entry="$signed_runtime/bin/wine.switchyard-real"

  mach_o_list="$native_release_private_root/mach-o.list"
  verification_log="$native_release_private_root/codesign-verification.log"
  : >"$mach_o_list"
  : >"$verification_log"

  if ! /usr/bin/find "$signed_runtime" -type f -print0 |
      while IFS= read -r -d '' item; do
        if /usr/bin/file -b "$item" | /usr/bin/grep -q 'Mach-O'; then
          /usr/bin/printf '%s\0' "$item"
        fi
      done >"$mach_o_list"; then
    echo "cannot enumerate native release Mach-O files" >&2
    return 1
  fi
  mach_o_count="$(/usr/bin/python3 -I - "$mach_o_list" <<'PY'
import sys
print(open(sys.argv[1], "rb").read().count(b"\0"))
PY
)"
  [ "$mach_o_count" -gt 0 ] || {
    echo "native release runtime has no Mach-O files" >&2
    return 1
  }

  echo "signing $mach_o_count native ARM64 Mach-O files"
  while IFS= read -r -d '' item; do
    case "$item" in
      "$wine_entry"|"$fallback_entry")
        sign_release_macho_atomically \
          "$CODESIGN_TOOL" "$item" "$IDENTITY" \
          --entitlements "/dev/fd/$entitlements_snapshot_fd" || return 1
        verify_macho_entitlements_snapshot \
          "$CODESIGN_TOOL" preview-native-arm64-fex \
          "$item" "$entitlements_snapshot_fd" || return 1
        entry_count=$((entry_count + 1))
        ;;
      *)
        sign_release_macho_atomically \
          "$CODESIGN_TOOL" "$item" "$IDENTITY" || return 1
        ;;
    esac
    switchyard_verify_native_release_team "$item" || return 1
  done <"$mach_o_list"
  [ "$entry_count" -eq 2 ] || {
    echo "native release does not contain exactly two signed process entries" >&2
    return 1
  }
}

switchyard_run_native_release_smoke() {
  local runtime_root="$1"
  local prepared_prefix="$2"

  "$NATIVE_SMOKE_TOOL" --prefix "$prepared_prefix" \
    "$runtime_root" \
    "$runtime_root/bin/switchyard-wine" \
    "$runtime_root/bin/wineserver" \
    "$runtime_root/lib/wine/aarch64-windows/cmd.exe" \
    SWITCHYARD_NATIVE_RELEASE_OK
}

switchyard_stop_native_release_wineserver() {
  local runtime_root="$1"
  local prepared_prefix="$2"
  local wineserver="$runtime_root/bin/wineserver"
  local clean_environment=(
    /usr/bin/env
    -u WINEARCH -u WINEDEBUG -u WINEDLLPATH -u WINELOADER -u WINESERVER
    -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH
    -u DYLD_FRAMEWORK_PATH -u DYLD_FALLBACK_FRAMEWORK_PATH
    -u DYLD_INSERT_LIBRARIES
  )

  [ -x "$wineserver" ] && [ ! -L "$wineserver" ] || return 1
  if "${clean_environment[@]}" WINEPREFIX="$prepared_prefix" \
      "$wineserver" -k >/dev/null 2>&1; then
    "${clean_environment[@]}" WINEPREFIX="$prepared_prefix" \
      "$wineserver" -w >/dev/null 2>&1
  else
    # The strict process harness owns the first stop/wait pass.  When it has
    # already drained this exact prefix, wineserver -k reports failure because
    # no server remains, but the idempotent wait still succeeds.
    "${clean_environment[@]}" WINEPREFIX="$prepared_prefix" \
      "$wineserver" -w >/dev/null 2>&1
  fi
}

switchyard_publish_native_outer_digest() {
  write_runtime_content_tree_digest "$1"
}

switchyard_create_native_release_archive() {
  /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$1" "$2"
}

switchyard_submit_native_release_notary() {
  local release_archive="$1"
  local keychain_profile="$2"
  local result_path="$3"

  /usr/bin/xcrun notarytool submit "$release_archive" \
    --keychain-profile "$keychain_profile" \
    --wait --output-format json >"$result_path"
}

switchyard_release_text_has_no_korean() {
  /usr/bin/python3 -I - "$@" <<'PY'
import sys

for name in sys.argv[1:]:
    with open(name, "r", encoding="utf-8") as stream:
        data = stream.read()
    if any(
        "\u1100" <= character <= "\u11ff"
        or "\u3130" <= character <= "\u318f"
        or "\uac00" <= character <= "\ud7a3"
        for character in data
    ):
        raise SystemExit("native release metadata contains Korean text")
PY
}

switchyard_release_preview_native() {
  local archive_private
  local checksum_private
  local manifest_private
  local notary_result
  local portable_manifest
  local release_architecture_command=""
  local release_pe_architectures=""
  local smoke_status
  local prefix_candidate
  local pe_architecture
  local command_part

  [[ "$EXPECTED_TEAM_ID" =~ ^[A-Z0-9]{10}$ ]] || {
    echo "native release Developer Team ID is malformed" >&2
    return 2
  }
  mkdir -p "$OUTPUT_DIR"
  OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd -P)"
  switchyard_validate_native_release_output_root "$OUTPUT_DIR" || return 1

  archive_name="${release_root_name}.zip"
  archive="$OUTPUT_DIR/$archive_name"
  release_manifest="$OUTPUT_DIR/switchyard-runtime-release.json"
  checksum_file="$OUTPUT_DIR/${archive_name}.sha256"
  for destination in "$archive" "$release_manifest" "$checksum_file"; do
    [ ! -e "$destination" ] && [ ! -L "$destination" ] || {
      echo "release output already exists: $destination" >&2
      return 1
    }
  done

  native_release_private_root="$(
    /usr/bin/mktemp -d "$OUTPUT_DIR/.switchyard-native-release.XXXXXX"
  )"
  native_release_private_root="$(cd "$native_release_private_root" && pwd -P)"
  /bin/chmod 0700 "$native_release_private_root"
  signed_runtime="$native_release_private_root/$release_root_name"
  archive_private="$native_release_private_root/$archive_name"
  checksum_private="$native_release_private_root/${archive_name}.sha256"
  manifest_private="$native_release_private_root/switchyard-runtime-release.json"

  echo "cloning native ARM64 runtime into private release staging"
  /bin/cp -cR "$RUNTIME" "$signed_runtime"
  [ "$(cd "$signed_runtime" && pwd -P)" = "$signed_runtime" ] || {
    echo "native release staging root is not physically canonical" >&2
    return 1
  }
  runtime_content_tree_is_verified "$signed_runtime" || {
    echo "native release staging content does not match the build-time runtime digest" >&2
    return 1
  }
  [ "$(runtime_content_tree_digest "$signed_runtime")" = "$EXPECTED_RUNTIME_DIGEST" ] || {
    echo "native release staging does not match the separately recorded build digest" >&2
    return 1
  }

  portable_manifest="$signed_runtime/switchyard-runtime.json"
  [ -f "$portable_manifest" ] && [ ! -L "$portable_manifest" ] || {
    echo "native release staging manifest is missing or unsafe" >&2
    return 1
  }
  /usr/bin/plutil -replace installPrefix -string "." "$portable_manifest"
  /usr/bin/plutil -replace executable -string "bin/switchyard-wine" "$portable_manifest"

  create_validated_entitlements_snapshot \
    preview-native-arm64-fex "$ENTITLEMENTS" \
    "$native_release_private_root" entitlements_snapshot_fd || return 1
  switchyard_sign_native_release_runtime || return 1
  close_validated_entitlements_snapshot "$entitlements_snapshot_fd" || return 1
  entitlements_snapshot_fd=""

  # This producer operation is consuming and single-shot.  Any failure leaves
  # the private clone unusable; cleanup discards the whole staging root.
  switchyard_refresh_native_arm64_signed_runtime_manifest \
    "$signed_runtime" "$portable_manifest" || return 1
  switchyard_validate_native_release_runtime \
    "$signed_runtime" "$portable_manifest" || return 1

  prefix_candidate="$native_release_private_root/smoke-prefix"
  switchyard_prepare_runtime_prefix \
    preview-native-arm64-fex "$prefix_candidate" prefix || return 1
  if switchyard_run_native_release_smoke "$signed_runtime" "$prefix"; then
    smoke_status=0
  else
    smoke_status=$?
  fi
  # The canonical harness already stops and waits this exact wineserver on all
  # returns.  Repeating the exact stop/wait makes release cleanup independently
  # fail closed before the fd-relative prefix removal.
  switchyard_stop_native_release_wineserver "$signed_runtime" "$prefix" || return 1
  switchyard_remove_runtime_prefix_offline \
    preview-native-arm64-fex "$prefix" || return 1
  prefix=""
  [ "$smoke_status" -eq 0 ] || {
    echo "native release failed the strict no-Rosetta smoke test" >&2
    return "$smoke_status"
  }

  switchyard_validate_native_release_runtime \
    "$signed_runtime" "$portable_manifest" || return 1

  # The outer marker is the sole allowed runtime mutation after the signed
  # manifest refresh.  Everything below this point is read-only for the tree.
  switchyard_publish_native_outer_digest "$signed_runtime" || return 1
  runtime_content_tree_is_verified "$signed_runtime" || {
    echo "native release outer content digest did not verify" >&2
    return 1
  }

  echo "creating $archive_name"
  switchyard_create_native_release_archive \
    "$signed_runtime" "$archive_private" || return 1
  archive_sha256="$(sha256_file "$archive_private")"
  archive_size="$(/usr/bin/stat -f '%z' "$archive_private")"
  /usr/bin/printf '%s  %s\n' "$archive_sha256" "$archive_name" >"$checksum_private"

  notary_status="not-submitted"
  notary_id=""
  if [ -n "$NOTARY_PROFILE" ]; then
    notary_result="$native_release_private_root/notary-result.json"
    switchyard_submit_native_release_notary \
      "$archive_private" "$NOTARY_PROFILE" "$notary_result" || return 1
    notary_status="$(manifest_value status "$notary_result")"
    notary_id="$(manifest_value id "$notary_result")"
    /bin/rm -f "$notary_result"
    [ "$notary_status" = "Accepted" ] || {
      echo "Apple notarization did not accept the native runtime archive: $notary_status" >&2
      return 1
    }
    [[ "$notary_id" =~ ^[A-Za-z0-9._-]+$ ]] || {
      echo "Apple notarization returned a malformed submission ID" >&2
      return 1
    }
  fi

  for pe_architecture in "${SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}"; do
    [ -z "$release_pe_architectures" ] || release_pe_architectures+=", "
    release_pe_architectures+="\"$pe_architecture\""
  done
  for command_part in "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}"; do
    [ -z "$release_architecture_command" ] || release_architecture_command+=", "
    release_architecture_command+="\"$command_part\""
  done

  cat >"$manifest_private" <<EOF
{
  "schemaVersion": 1,
  "runtimeManifestVersion": $SWITCHYARD_RUNTIME_MANIFEST_VERSION,
  "runtimeID": "$runtime_id",
  "runtimeProfile": "$SWITCHYARD_RUNTIME_PROFILE",
  "sourceRevision": "$source_revision",
  "archive": "$archive_name",
  "archiveSha256": "$archive_sha256",
  "archiveSize": $archive_size,
  "platform": "macos",
  "hostArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH",
  "wineUnixArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH",
  "buildTriplet": "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET",
  "hostTriplet": "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET",
  "architectureCommand": [$release_architecture_command],
  "requiresRosetta": $SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA,
  "minimumMacOS": "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS",
  "gstreamerRegistryArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH",
  "peArchitectures": [$release_pe_architectures],
  "developerTeamID": "$EXPECTED_TEAM_ID",
  "notarizationStatus": "$notary_status",
  "notarizationID": "$notary_id"
}
EOF
  switchyard_release_text_has_no_korean "$manifest_private" || return 1

  native_archive_public_path="$archive"
  native_archive_public_identity="$(
    switchyard_native_release_file_identity "$archive_private"
  )"
  switchyard_publish_native_release_file "$archive_private" "$archive" || return 1
  native_checksum_public_path="$checksum_file"
  native_checksum_public_identity="$(
    switchyard_native_release_file_identity "$checksum_private"
  )"
  switchyard_publish_native_release_file \
    "$checksum_private" "$checksum_file" || return 1
  native_manifest_public_path="$release_manifest"
  native_manifest_public_identity="$(
    switchyard_native_release_file_identity "$manifest_private"
  )"
  switchyard_publish_native_release_file \
    "$manifest_private" "$release_manifest" || return 1

  remove_native_release_private_root || return 1
  native_release_complete=1
  echo "runtime release archive: $archive"
  echo "runtime release manifest: $release_manifest"
  echo "runtime archive sha256: $archive_sha256"
  echo "runtime notarization: $notary_status${notary_id:+ ($notary_id)}"
}

usage() {
  cat >&2 <<EOF
usage: $0 --runtime PATH --runtime-content-sha256 SHA256 --output DIR --identity IDENTITY [--notary-profile PROFILE]
EOF
  exit 2
}

release_main() {
trap cleanup EXIT

while [ "$#" -gt 0 ]; do
  case "$1" in
    --runtime)
      [ "$#" -ge 2 ] || usage
      RUNTIME="$2"
      shift 2
      ;;
    --runtime-content-sha256)
      [ "$#" -ge 2 ] || usage
      EXPECTED_RUNTIME_DIGEST="$2"
      shift 2
      ;;
    --output)
      [ "$#" -ge 2 ] || usage
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --identity)
      [ "$#" -ge 2 ] || usage
      IDENTITY="$2"
      shift 2
      ;;
    --notary-profile)
      [ "$#" -ge 2 ] || usage
      NOTARY_PROFILE="$2"
      shift 2
      ;;
    *) usage ;;
  esac
done

[ -n "$RUNTIME" ] || usage
[ -n "$OUTPUT_DIR" ] || usage
[ -n "$IDENTITY" ] || usage
[[ "$EXPECTED_RUNTIME_DIGEST" =~ ^[0-9a-f]{64}$ ]] || {
  echo "runtime content SHA-256 must contain exactly 64 lowercase hexadecimal characters" >&2
  exit 2
}
[ -d "$RUNTIME" ] || { echo "runtime does not exist: $RUNTIME" >&2; exit 1; }
[ ! -L "$RUNTIME" ] || { echo "runtime path must not be a symbolic link" >&2; exit 1; }
[ -f "$RUNTIME/switchyard-runtime.json" ] || { echo "runtime manifest is missing" >&2; exit 1; }
[ ! -L "$RUNTIME/switchyard-runtime.json" ] || { echo "runtime manifest must not be a symbolic link" >&2; exit 1; }
manifest_value() {
  /usr/bin/plutil -extract "$1" raw -o - "$2" 2>/dev/null || true
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

runtime_content_tree_is_verified() {
  local root="$1"
  /usr/bin/python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" verify "$root"
}

runtime_content_tree_digest() {
  local root="$1"
  /usr/bin/python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" digest "$root"
}

write_runtime_content_tree_digest() {
  local root="$1"
  /usr/bin/python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" write "$root" >/dev/null
}

manifest="$RUNTIME/switchyard-runtime.json"
manifest_snapshot="$(/usr/bin/mktemp)"
/bin/cp "$manifest" "$manifest_snapshot"
manifest_snapshot_sha256="$(sha256_file "$manifest_snapshot")"
runtime_profile="$(manifest_value runtimeFamily "$manifest_snapshot")"
switchyard_runtime_profile_is_known "$runtime_profile" || {
  echo "runtime manifest has an unknown runtime profile" >&2
  exit 1
}
switchyard_load_runtime_profile "$runtime_profile" || exit $?
if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  RUNTIME="$(cd "$RUNTIME" && pwd -P)"
  manifest="$RUNTIME/switchyard-runtime.json"
fi
ENTITLEMENTS="$(switchyard_runtime_profile_entitlements_path "$ROOT_DIR")" || exit $?
[ -f "$ENTITLEMENTS" ] && [ ! -L "$ENTITLEMENTS" ] || {
  echo "runtime signing entitlements are missing or invalid" >&2
  exit 1
}
switchyard_require_runtime_profile_enabled || exit $?
if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  switchyard_validate_runtime_manifest_profile \
    "$manifest" "$runtime_profile" "$RUNTIME" || exit 1
else
  switchyard_validate_runtime_manifest_profile "$manifest_snapshot" "$runtime_profile" || exit 1
fi
runtime_id="$(manifest_value id "$manifest_snapshot")"
source_revision="$(manifest_value sourceRevision "$manifest_snapshot")"
source_dirty="$(manifest_value sourceDirty "$manifest_snapshot")"
gptk_path="$(manifest_value gptkPath "$manifest_snapshot")"
gptk_digest="$(manifest_value gptkRedistDigest "$manifest_snapshot")"
current_revision="$(git -C "$ROOT_DIR" rev-parse HEAD)"

[ -n "$runtime_id" ] || { echo "runtime manifest has no id" >&2; exit 1; }
[ "$source_revision" = "$current_revision" ] || {
  echo "runtime source $source_revision does not match current source $current_revision" >&2
  exit 1
}
[ "$source_dirty" = "false" ] || { echo "release runtime was built from a dirty source tree" >&2; exit 1; }
[ -z "$gptk_path" ] || { echo "release runtime records a user-provided GPTK path" >&2; exit 1; }
[ "$gptk_digest" = "no-gptk" ] || { echo "release runtime contains a GPTK overlay" >&2; exit 1; }
runtime_content_tree_is_verified "$RUNTIME" || {
  echo "release runtime content digest is missing or does not match the runtime tree" >&2
  exit 1
}
[ "$(runtime_content_tree_digest "$RUNTIME")" = "$EXPECTED_RUNTIME_DIGEST" ] || {
  echo "release runtime content does not match the separately recorded build digest" >&2
  exit 1
}
[ -f "$manifest" ] && [ ! -L "$manifest" ] || {
  echo "runtime manifest changed type during release validation" >&2
  exit 1
}
[ "$(sha256_file "$manifest")" = "$manifest_snapshot_sha256" ] || {
  echo "runtime manifest changed during release validation" >&2
  exit 1
}
if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  switchyard_validate_native_arm64_runtime_packaging \
    "$RUNTIME" "$manifest" "$ROOT_DIR" || exit 1
fi

for required_notice in \
  share/doc/switchyard-wine/LICENSE \
  share/doc/switchyard-wine/COPYING.LIB \
  share/doc/switchyard-wine/AUTHORS \
  share/doc/switchyard-wine/CORRESPONDING-SOURCE.txt \
  lib/switchyard-mesa/share/doc/switchyard-mesa/README.txt \
  lib/switchyard-mesa/share/doc/switchyard-mesa/MESA-LICENSE.rst \
  lib/switchyard-mesa/share/doc/switchyard-mesa/LLVM-LICENSE.txt \
  lib/switchyard-mesa/share/doc/switchyard-mesa/DISTRIBUTOR-LICENSE.txt \
  lib/switchyard-gstreamer/share/doc/switchyard-gstreamer/README.txt \
  lib/switchyard-gstreamer/share/doc/switchyard-gstreamer/INSTALLER-LICENSE.txt \
  lib/switchyard-gstreamer/share/doc/switchyard-gstreamer/packages.tsv \
  lib/switchyard-gstreamer/share/licenses/gstreamer-1.0/LGPL-2.0-or-later.txt \
  lib/switchyard-gstreamer/share/licenses/ffmpeg/LGPL-2.1-or-later.txt \
  lib/switchyard-tls/share/doc/switchyard-tls/packages.tsv \
  lib/switchyard-tls/share/doc/switchyard-tls/sources.tsv; do
  { [ -f "$RUNTIME/$required_notice" ] &&
    { [ "$runtime_profile" != preview-native-arm64-fex ] ||
      [ ! -L "$RUNTIME/$required_notice" ]; }; } || {
    echo "release runtime is missing notice $required_notice" >&2
    exit 1
  }
done

if /usr/bin/find "$RUNTIME" \( -iname '*d3dmetal*' -o -iname '*metalirconverter*' -o -iname 'libd3dshared*' \) -print -quit |
   /usr/bin/grep -q .; then
  echo "release runtime contains a user-provided Apple graphics component" >&2
  exit 1
fi
if /usr/bin/grep -R -I -q -E '/Users/[^/]+/.+(Game.Porting.Toolkit|heroic)' \
     "$RUNTIME/switchyard-runtime.json" "$RUNTIME/lib/switchyard-gstreamer/share/doc" \
     "$RUNTIME/lib/switchyard-tls/share/doc" 2>/dev/null; then
  echo "release runtime contains a user-local toolkit provenance path" >&2
  exit 1
fi
if /usr/bin/grep -R -I -q -E '/Users/[^/]+' \
     "$RUNTIME/lib/switchyard-gstreamer" 2>/dev/null; then
  echo "release runtime contains a user-local GStreamer build path" >&2
  exit 1
fi

if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  release_short_revision="${source_revision:0:12}"
  release_root_name="Switchyard-Wine-Runtime-${release_short_revision}-${SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX}"
  switchyard_release_preview_native
  return
fi

mkdir -p "$OUTPUT_DIR"
release_short_revision="${source_revision:0:12}"
release_root_name="Switchyard-Wine-Runtime-${release_short_revision}-${SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX}"
signed_runtime="$OUTPUT_DIR/$release_root_name"
archive_name="${release_root_name}.zip"
archive="$OUTPUT_DIR/$archive_name"
release_manifest="$OUTPUT_DIR/switchyard-runtime-release.json"
checksum_file="$OUTPUT_DIR/${archive_name}.sha256"

for destination in "$signed_runtime" "$archive" "$release_manifest" "$checksum_file"; do
  [ ! -e "$destination" ] || {
    echo "release output already exists: $destination" >&2
    exit 1
  }
done

echo "cloning runtime into release staging"
/bin/cp -cR "$RUNTIME" "$signed_runtime"
runtime_content_tree_is_verified "$signed_runtime" || {
  echo "release staging content does not match the build-time runtime digest" >&2
  exit 1
}
[ "$(runtime_content_tree_digest "$signed_runtime")" = "$EXPECTED_RUNTIME_DIGEST" ] || {
  echo "release staging content does not match the separately recorded build digest" >&2
  exit 1
}
portable_manifest="$signed_runtime/switchyard-runtime.json"
/usr/bin/plutil -replace installPrefix -string "." "$portable_manifest"
/usr/bin/plutil -replace executable -string "bin/switchyard-wine" "$portable_manifest"
wine_unix_executable="$signed_runtime/lib/wine/${SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH}-unix/wine"

mach_o_list="$(/usr/bin/mktemp)"
verification_log="$(/usr/bin/mktemp)"

/usr/bin/find "$signed_runtime" -type f -print0 |
while IFS= read -r -d '' item; do
  if /usr/bin/file -b "$item" | /usr/bin/grep -q 'Mach-O'; then
    /usr/bin/printf '%s\0' "$item"
  fi
done > "$mach_o_list"

mach_o_count="$(/usr/bin/python3 - "$mach_o_list" <<'PY'
import sys
print(open(sys.argv[1], 'rb').read().count(bytes([0])))
PY
)"
[ "$mach_o_count" -gt 0 ] || { echo "release runtime has no Mach-O files" >&2; exit 1; }

echo "signing $mach_o_count Mach-O files"
while IFS= read -r -d '' item; do
  /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp "$item"
done < "$mach_o_list"

for launcher in \
  "$wine_unix_executable" \
  "$signed_runtime/bin/wine.switchyard-real"; do
  [ -f "$launcher" ] || { echo "release runtime is missing launcher $launcher" >&2; exit 1; }
  /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" "$launcher"
done

wine_sha256="$(sha256_file "$wine_unix_executable")"
/usr/bin/plutil -replace integrity.wineUnixSha256 -string "$wine_sha256" "$portable_manifest"

while IFS= read -r -d '' item; do
  if ! /usr/bin/codesign --verify --strict --verbose=2 "$item" >"$verification_log" 2>&1; then
    /bin/cat "$verification_log" >&2
    exit 1
  fi
done < "$mach_o_list"

signing_details="$(/usr/bin/codesign -d --verbose=4 "$wine_unix_executable" 2>&1)"
/usr/bin/printf '%s\n' "$signing_details" | /usr/bin/grep -F "TeamIdentifier=$EXPECTED_TEAM_ID" >/dev/null || {
  echo "signed runtime has an unexpected Developer Team ID" >&2
  exit 1
}
/usr/bin/printf '%s\n' "$signing_details" | /usr/bin/grep -F 'Runtime Version=' >/dev/null || {
  echo "signed runtime is missing Hardened Runtime" >&2
  exit 1
}

prefix="$(/usr/bin/mktemp -d /tmp/switchyard-release-prefix.XXXXXX)"
smoke_output="$(WINEPREFIX="$prefix" WINEDEBUG=-all "$signed_runtime/bin/switchyard-wine" cmd /c ver)"
/usr/bin/printf '%s' "$smoke_output" | /usr/bin/grep -F 'Microsoft Windows 10.0.19045' >/dev/null || {
  echo "signed runtime failed the fresh-prefix smoke test" >&2
  exit 1
}
WINEPREFIX="$prefix" "$signed_runtime/bin/wineserver" -k >/dev/null 2>&1 || true
/bin/sleep 1
/bin/rm -rf "$prefix"
prefix=""

write_runtime_content_tree_digest "$signed_runtime"
echo "creating $archive_name"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$signed_runtime" "$archive"
archive_sha256="$(sha256_file "$archive")"
archive_size="$(/usr/bin/stat -f '%z' "$archive")"
/usr/bin/printf '%s  %s\n' "$archive_sha256" "$archive_name" > "$checksum_file"

notary_status="not-submitted"
notary_id=""
if [ -n "$NOTARY_PROFILE" ]; then
  notary_result="$(/usr/bin/mktemp)"
  /usr/bin/xcrun notarytool submit "$archive" --keychain-profile "$NOTARY_PROFILE" \
    --wait --output-format json > "$notary_result"
  notary_status="$(manifest_value status "$notary_result")"
  notary_id="$(manifest_value id "$notary_result")"
  /bin/rm -f "$notary_result"
  [ "$notary_status" = "Accepted" ] || {
    echo "Apple notarization did not accept the runtime archive: $notary_status" >&2
    exit 1
  }
fi

release_pe_architectures=""
for pe_architecture in "${SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}"; do
  [ -z "$release_pe_architectures" ] || release_pe_architectures+=", "
  release_pe_architectures+="\"$pe_architecture\""
done
release_architecture_command=""
for command_part in "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}"; do
  [ -z "$release_architecture_command" ] || release_architecture_command+=", "
  release_architecture_command+="\"$command_part\""
done

cat > "$release_manifest" <<EOF
{
  "schemaVersion": 1,
  "runtimeManifestVersion": $SWITCHYARD_RUNTIME_MANIFEST_VERSION,
  "runtimeID": "$runtime_id",
  "runtimeProfile": "$SWITCHYARD_RUNTIME_PROFILE",
  "sourceRevision": "$source_revision",
  "archive": "$archive_name",
  "archiveSha256": "$archive_sha256",
  "archiveSize": $archive_size,
  "platform": "macos",
  "hostArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH",
  "wineUnixArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH",
  "buildTriplet": "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET",
  "hostTriplet": "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET",
  "architectureCommand": [$release_architecture_command],
  "requiresRosetta": $SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA,
  "minimumMacOS": "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS",
  "gstreamerRegistryArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH",
  "peArchitectures": [$release_pe_architectures],
  "developerTeamID": "$EXPECTED_TEAM_ID",
  "notarizationStatus": "$notary_status",
  "notarizationID": "$notary_id"
}
EOF

echo "runtime release archive: $archive"
echo "runtime release manifest: $release_manifest"
echo "runtime archive sha256: $archive_sha256"
echo "runtime notarization: $notary_status${notary_id:+ ($notary_id)}"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  release_main "$@"
fi
