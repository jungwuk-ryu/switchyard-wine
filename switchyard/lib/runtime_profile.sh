#!/usr/bin/env bash
# shellcheck disable=SC2034 # Profile globals are consumed by sourcing scripts.

# Runtime profiles are a closed set of trusted build and release policy values.
# Keep host Mach-O, Wine Unix, and PE architecture names separate: they belong
# to different ABIs and are not interchangeable even when they target the same
# physical CPU family.

SWITCHYARD_DEFAULT_RUNTIME_PROFILE="stable-x86_64-rosetta"
SWITCHYARD_RUNTIME_MANIFEST_VERSION="2"

switchyard_runtime_profile_is_known() {
  case "$1" in
    stable-x86_64-rosetta|preview-native-arm64-fex) return 0 ;;
    *) return 1 ;;
  esac
}

switchyard_load_runtime_profile() {
  local profile="$1"

  case "$profile" in
    stable-x86_64-rosetta)
      SWITCHYARD_RUNTIME_PROFILE="$profile"
      SWITCHYARD_RUNTIME_PROFILE_ENABLED="1"
      SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX="switchyard-local-wow64-x86_64-"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_PROFILE="switchyard-wow64-pe"
      SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH="x86_64"
      SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH="x86_64"
      SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS=("i386" "x86_64")
      SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV="i386,x86_64"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET="x86_64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET="x86_64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND=("arch" "-x86_64")
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA="true"
      SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS="14.0"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH="x86_64"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION="universal (x86_64 used by Wine under Rosetta)"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_CACHE_BASENAME="build-wow64-x86_64"
      SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX="macos-x86_64"
      ;;
    preview-native-arm64-fex)
      SWITCHYARD_RUNTIME_PROFILE="$profile"
      SWITCHYARD_RUNTIME_PROFILE_ENABLED="0"
      SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX="switchyard-local-native-arm64-fex-"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_PROFILE="switchyard-native-arm64-fex"
      SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH="arm64"
      SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH="aarch64"
      SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS=("aarch64" "arm64ec" "x86_64" "i386")
      SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV="aarch64,arm64ec,x86_64,i386"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET="aarch64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET="aarch64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND=("arch" "-arm64")
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA="false"
      SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS="26.5"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH="arm64"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION="universal (arm64 used by native Wine)"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_CACHE_BASENAME="build-native-arm64-fex"
      SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX="macos-arm64"
      ;;
    *)
      echo "Unknown runtime profile. Expected stable-x86_64-rosetta or preview-native-arm64-fex." >&2
      return 2
      ;;
  esac
}

switchyard_require_runtime_profile_enabled() {
  if [ "$SWITCHYARD_RUNTIME_PROFILE_ENABLED" = "1" ]; then
    return 0
  fi

  echo "Runtime profile $SWITCHYARD_RUNTIME_PROFILE is recognized but not enabled." >&2
  echo "The native ARM64/FEX build, JIT, provider, signing, and qualification gates have not passed." >&2
  echo "Use --runtime-profile stable-x86_64-rosetta for the supported runtime." >&2
  return 2
}

switchyard_runtime_manifest_value() {
  local key="$1"
  local manifest="$2"

  /usr/bin/plutil -extract "$key" raw -o - "$manifest" 2>/dev/null || true
}

switchyard_runtime_manifest_error() {
  echo "runtime manifest profile metadata is invalid: $1" >&2
  return 1
}

switchyard_validate_runtime_manifest_profile() {
  local manifest="$1"
  local expected_profile="$2"
  local actual
  local index

  [ -f "$manifest" ] && [ ! -L "$manifest" ] || {
    switchyard_runtime_manifest_error "manifest must be a regular file"
    return 1
  }
  /usr/bin/python3 - "$manifest" >/dev/null 2>&1 <<'PY' || {
import json
import os
import sys

manifest = sys.argv[1]
if os.path.getsize(manifest) > 1024 * 1024:
    raise ValueError("runtime manifest exceeds the validation size limit")

def object_without_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON object key")
        value[key] = item
    return value

def reject_nonstandard_constant(value):
    raise ValueError("non-standard JSON constant: " + value)

with open(manifest, "r", encoding="utf-8") as stream:
    value = json.load(
        stream,
        object_pairs_hook=object_without_duplicates,
        parse_constant=reject_nonstandard_constant,
    )
if not isinstance(value, dict):
    raise ValueError("runtime manifest root is not an object")

def require_exact_type(container, key, expected_type):
    if key not in container or type(container[key]) is not expected_type:
        raise ValueError("runtime manifest field has an unexpected JSON type: " + key)

require_exact_type(value, "manifestVersion", int)
for key in ("id", "runtimeFamily", "buildProfile"):
    require_exact_type(value, key, str)
require_exact_type(value, "peArchitectures", list)
if not all(type(item) is str for item in value["peArchitectures"]):
    raise ValueError("peArchitectures contains a non-string value")
require_exact_type(value, "host", dict)
host = value["host"]
for key in (
    "platform",
    "architecture",
    "wineUnixArchitecture",
    "buildTriplet",
    "hostTriplet",
    "minimumMacOS",
    "gstreamerRegistryArchitecture",
):
    require_exact_type(host, key, str)
require_exact_type(host, "requiresRosetta", bool)
require_exact_type(host, "architectureCommand", list)
if not all(type(item) is str for item in host["architectureCommand"]):
    raise ValueError("architectureCommand contains a non-string value")
PY
    switchyard_runtime_manifest_error "manifest is not valid unambiguous JSON data"
    return 1
  }
  switchyard_runtime_profile_is_known "$expected_profile" || {
    switchyard_runtime_manifest_error "runtimeFamily is not allowlisted"
    return 1
  }
  switchyard_load_runtime_profile "$expected_profile" || return 1

  actual="$(switchyard_runtime_manifest_value manifestVersion "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_MANIFEST_VERSION" ] || {
    switchyard_runtime_manifest_error "manifestVersion must be $SWITCHYARD_RUNTIME_MANIFEST_VERSION"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value runtimeFamily "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE" ] || {
    switchyard_runtime_manifest_error "runtimeFamily does not match the selected profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value buildProfile "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_BUILD_PROFILE" ] || {
    switchyard_runtime_manifest_error "buildProfile does not match the selected profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value id "$manifest")"
  [[ "$actual" =~ ^[A-Za-z0-9._-]+$ ]] || {
    switchyard_runtime_manifest_error "id contains characters outside the safe identifier allowlist"
    return 1
  }
  if [ "${#actual}" -le "${#SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX}" ]; then
    switchyard_runtime_manifest_error "id does not use the selected profile prefix"
    return 1
  fi
  case "$actual" in
    "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX"*) ;;
    *)
      switchyard_runtime_manifest_error "id does not use the selected profile prefix"
      return 1
      ;;
  esac

  actual="$(switchyard_runtime_manifest_value peArchitectures "$manifest")"
  [ "$actual" = "${#SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}" ] || {
    switchyard_runtime_manifest_error "peArchitectures has an unexpected length"
    return 1
  }
  for index in "${!SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}"; do
    actual="$(switchyard_runtime_manifest_value "peArchitectures.$index" "$manifest")"
    [ "$actual" = "${SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[$index]}" ] || {
      switchyard_runtime_manifest_error "peArchitectures is not the exact profile allowlist"
      return 1
    }
  done

  actual="$(switchyard_runtime_manifest_value host.platform "$manifest")"
  [ "$actual" = "macos" ] || {
    switchyard_runtime_manifest_error "host.platform must be macos"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.architecture "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH" ] || {
    switchyard_runtime_manifest_error "host.architecture does not match the Mach-O profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.wineUnixArchitecture "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH" ] || {
    switchyard_runtime_manifest_error "host.wineUnixArchitecture does not match the Wine Unix profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.buildTriplet "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET" ] || {
    switchyard_runtime_manifest_error "host.buildTriplet does not match the selected profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.hostTriplet "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET" ] || {
    switchyard_runtime_manifest_error "host.hostTriplet does not match the selected profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.architectureCommand "$manifest")"
  [ "$actual" = "${#SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}" ] || {
    switchyard_runtime_manifest_error "host.architectureCommand has an unexpected length"
    return 1
  }
  for index in "${!SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}"; do
    actual="$(switchyard_runtime_manifest_value "host.architectureCommand.$index" "$manifest")"
    [ "$actual" = "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[$index]}" ] || {
      switchyard_runtime_manifest_error "host.architectureCommand is not the exact profile allowlist"
      return 1
    }
  done
  actual="$(switchyard_runtime_manifest_value host.requiresRosetta "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA" ] || {
    switchyard_runtime_manifest_error "host.requiresRosetta does not match the selected profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.minimumMacOS "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" ] || {
    switchyard_runtime_manifest_error "host.minimumMacOS does not match the selected profile"
    return 1
  }
  actual="$(switchyard_runtime_manifest_value host.gstreamerRegistryArchitecture "$manifest")"
  [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH" ] || {
    switchyard_runtime_manifest_error "host.gstreamerRegistryArchitecture does not match the selected profile"
    return 1
  }
}
