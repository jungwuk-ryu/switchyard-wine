#!/usr/bin/env bash
# shellcheck disable=SC2034 # Profile globals are consumed by sourcing scripts.

# Runtime profiles are a closed set of trusted build and release policy values.
# Keep host Mach-O, Wine Unix, PE ABI, and installed PE layout names separate:
# they are not interchangeable even when they target the same physical CPU
# family.  Wine links ARM64EC objects into the installed aarch64 ARM64X image,
# so arm64ec is a configure/manifest ABI without a standalone install directory.

SWITCHYARD_DEFAULT_RUNTIME_PROFILE="stable-x86_64-rosetta"
SWITCHYARD_RUNTIME_MANIFEST_VERSION="2"
SWITCHYARD_NATIVE_RUNTIME_CLOSURE_CONTRACT_VERSION="1"
SWITCHYARD_NATIVE_LLVM_VERSION="22.1.8"
SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG="--no-default-config"
SWITCHYARD_RUNTIME_BOOTSTRAP_MAX_PATH="260"
# WINEDATADIR reaches setupapi as \\?\Z:<runtime>/share/wine/wine.inf.
# InstallHinfSectionW stores the command tail in a WCHAR[MAX_PATH] buffer.
SWITCHYARD_RUNTIME_BOOTSTRAP_NT_PREFIX_OVERHEAD="6"
SWITCHYARD_RUNTIME_BOOTSTRAP_INSTALLHINF_PREFIX="DefaultInstall 128 "
SWITCHYARD_RUNTIME_BOOTSTRAP_WINE_INF_RELATIVE_PATH="share/wine/wine.inf"

switchyard_qualify_native_llvm_compilers() {
  [ "$#" -eq 1 ] || {
    echo "usage: switchyard_qualify_native_llvm_compilers LLVM_BIN" >&2
    return 2
  }

  local requested_bin="$1"
  local physical_bin
  local clang
  local clangxx
  local clang_real
  local clangxx_real
  local clang_version_output
  local clangxx_version_output
  local clang_version
  local clangxx_version
  local target
  local expected
  local actual
  local clang_sha256
  local clangxx_sha256

  case "$requested_bin" in
    /*) ;;
    *) echo "Native LLVM bin path must be absolute." >&2; return 1 ;;
  esac
  [ -d "$requested_bin" ] && [ ! -L "$requested_bin" ] || {
    echo "Native LLVM bin path is missing or is not physical: $requested_bin" >&2
    return 1
  }
  physical_bin="$(cd "$requested_bin" && /bin/pwd -P)" || return 1
  [ "$physical_bin" = "$requested_bin" ] || {
    echo "Native LLVM bin path is not its physical Homebrew location: $requested_bin" >&2
    return 1
  }
  case "$physical_bin" in
    *[[:space:]]*)
      echo "Native LLVM bin path must not contain whitespace: $physical_bin" >&2
      return 1
      ;;
  esac

  clang="$physical_bin/clang"
  clangxx="$physical_bin/clang++"
  [ -f "$clang" ] && [ -x "$clang" ] &&
    [ -f "$clangxx" ] && [ -x "$clangxx" ] || {
    echo "Native ARM64 runtime requires Homebrew LLVM clang and clang++." >&2
    return 1
  }
  clang_real="$(/usr/bin/python3 -I -c \
    'import os, sys; print(os.path.realpath(sys.argv[1]))' "$clang")" || return 1
  clangxx_real="$(/usr/bin/python3 -I -c \
    'import os, sys; print(os.path.realpath(sys.argv[1]))' "$clangxx")" || return 1
  case "$clang_real:$clangxx_real" in
    "$physical_bin"/*:"$physical_bin"/*) ;;
    *)
      echo "Native LLVM compiler links escape the physical Homebrew LLVM bin directory." >&2
      return 1
      ;;
  esac

  clang_version_output="$("$clang" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" --version)" ||
    return 1
  clangxx_version_output="$("$clangxx" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" --version)" ||
    return 1
  clang_version="${clang_version_output%%$'\n'*}"
  clangxx_version="${clangxx_version_output%%$'\n'*}"
  expected="Homebrew clang version $SWITCHYARD_NATIVE_LLVM_VERSION"
  [ "$clang_version" = "$expected" ] && [ "$clangxx_version" = "$expected" ] || {
    echo "Native ARM64 runtime requires $expected." >&2
    return 1
  }
  for target in arm64-apple-darwin aarch64-w64-mingw32 \
      arm64ec-w64-mingw32 x86_64-w64-mingw32 i686-w64-mingw32; do
    case "$target" in
      arm64-apple-darwin) expected="arm64-apple-darwin" ;;
      aarch64-w64-mingw32) expected="aarch64-w64-windows-gnu" ;;
      arm64ec-w64-mingw32) expected="arm64ec-w64-windows-gnu" ;;
      x86_64-w64-mingw32) expected="x86_64-w64-windows-gnu" ;;
      i686-w64-mingw32) expected="i686-w64-windows-gnu" ;;
    esac
    actual="$("$clang" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
      "--target=$target" --print-target-triple)" || return 1
    [ "$actual" = "$expected" ] || {
      echo "Native Homebrew LLVM clang cannot target $target (reported $actual)." >&2
      return 1
    }
  done
  actual="$("$clangxx" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
    --print-target-triple)" || return 1
  case "$actual" in
    arm64-apple-darwin*) ;;
    *)
      echo "Native Homebrew LLVM clang++ has the wrong machine target: $actual" >&2
      return 1
      ;;
  esac

  clang_sha256="$(/usr/bin/shasum -a 256 "$clang" | /usr/bin/awk '{print $1}')" ||
    return 1
  clangxx_sha256="$(/usr/bin/shasum -a 256 "$clangxx" | /usr/bin/awk '{print $1}')" ||
    return 1
  SWITCHYARD_QUALIFIED_NATIVE_LLVM_BIN="$physical_bin"
  SWITCHYARD_QUALIFIED_NATIVE_CLANG="$clang"
  SWITCHYARD_QUALIFIED_NATIVE_CLANGXX="$clangxx"
  SWITCHYARD_QUALIFIED_NATIVE_CLANG_REAL="$clang_real"
  SWITCHYARD_QUALIFIED_NATIVE_CLANGXX_REAL="$clangxx_real"
  SWITCHYARD_QUALIFIED_NATIVE_CLANG_SHA256="$clang_sha256"
  SWITCHYARD_QUALIFIED_NATIVE_CLANGXX_SHA256="$clangxx_sha256"
  SWITCHYARD_QUALIFIED_NATIVE_COMPILER_IDENTITY="$(
    /usr/bin/printf '%s\0%s\0%s\0%s\0%s\0%s\0%s\0' \
      "$SWITCHYARD_NATIVE_LLVM_VERSION" "$clang" "$clang_real" "$clang_sha256" \
      "$clangxx" "$clangxx_real" "$clangxx_sha256" |
      /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
  )" || return 1
}

switchyard_validate_qualified_native_llvm_compilers() {
  local expected_bin="${SWITCHYARD_QUALIFIED_NATIVE_LLVM_BIN:-}"
  local expected_clang="${SWITCHYARD_QUALIFIED_NATIVE_CLANG:-}"
  local expected_clangxx="${SWITCHYARD_QUALIFIED_NATIVE_CLANGXX:-}"
  local expected_identity="${SWITCHYARD_QUALIFIED_NATIVE_COMPILER_IDENTITY:-}"

  [ -n "$expected_bin" ] && [ -n "$expected_identity" ] || {
    echo "Native LLVM compiler policy was not qualified." >&2
    return 1
  }
  switchyard_qualify_native_llvm_compilers "$expected_bin" || return 1
  [ "$SWITCHYARD_QUALIFIED_NATIVE_CLANG" = "$expected_clang" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_CLANGXX" = "$expected_clangxx" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_COMPILER_IDENTITY" = "$expected_identity" ] || {
    echo "Qualified native LLVM compiler identity changed during the build." >&2
    return 1
  }
}

switchyard_qualify_native_macos_sdk() {
  [ "$#" -ge 1 ] && [ "$#" -le 2 ] || {
    echo "usage: switchyard_qualify_native_macos_sdk EXPECTED_VERSION [XCRUN]" >&2
    return 2
  }

  local expected_version="$1"
  local xcrun_command="${2:-/usr/bin/xcrun}"
  local reported_sdk
  local resolved_sdk
  local sdk_version
  local sdk_build_version
  local sdk_settings
  local sdk_settings_version
  local sdk_settings_deployment
  local sdk_settings_sha256
  local sdk_identity

  case "$expected_version" in
    ''|*[!0-9.]*)
      echo "Native macOS SDK version contract is invalid: $expected_version" >&2
      return 1
      ;;
  esac
  [ -f "$xcrun_command" ] && [ ! -L "$xcrun_command" ] &&
    [ -x "$xcrun_command" ] || {
    echo "Native macOS SDK resolver is missing or unsafe: $xcrun_command" >&2
    return 1
  }
  reported_sdk="$("$xcrun_command" --sdk macosx --show-sdk-path)" || return 1
  sdk_version="$("$xcrun_command" --sdk macosx --show-sdk-version)" || return 1
  sdk_build_version="$(
    "$xcrun_command" --sdk macosx --show-sdk-build-version
  )" || return 1
  [ "$sdk_version" = "$expected_version" ] || {
    echo "Native Wine build requires macOS SDK $expected_version, not $sdk_version." >&2
    return 1
  }
  case "$sdk_build_version" in
    ''|*[!0-9A-Za-z.]*)
      echo "Native macOS SDK build version is invalid: $sdk_build_version" >&2
      return 1
      ;;
  esac
  [ -d "$reported_sdk" ] || {
    echo "Native Wine build could not resolve the macOS SDK directory: $reported_sdk" >&2
    return 1
  }
  resolved_sdk="$(cd "$reported_sdk" && /bin/pwd -P)" || return 1
  case "$resolved_sdk" in
    /*) ;;
    *) echo "Native macOS SDK path is not absolute: $resolved_sdk" >&2; return 1 ;;
  esac
  case "$resolved_sdk" in
    *[[:space:]]*|*[[:cntrl:]]*)
      echo "Native Wine build requires a macOS SDK path without whitespace or control characters: $resolved_sdk" >&2
      return 1
      ;;
  esac
  sdk_settings="$resolved_sdk/SDKSettings.plist"
  [ -f "$sdk_settings" ] && [ ! -L "$sdk_settings" ] || {
    echo "Native macOS SDK settings are missing or unsafe: $sdk_settings" >&2
    return 1
  }
  sdk_settings_version="$(
    /usr/bin/plutil -extract Version raw -o - "$sdk_settings"
  )" || return 1
  sdk_settings_deployment="$(
    /usr/bin/plutil -extract DefaultProperties.MACOSX_DEPLOYMENT_TARGET raw \
      -o - "$sdk_settings"
  )" || return 1
  [ "$sdk_settings_version" = "$sdk_version" ] &&
    [ "$sdk_settings_deployment" = "$expected_version" ] || {
    echo "Native macOS SDK settings do not match the selected $sdk_version SDK." >&2
    return 1
  }
  sdk_settings_sha256="$(
    /usr/bin/shasum -a 256 "$sdk_settings" | /usr/bin/awk '{print $1}'
  )" || return 1
  sdk_identity="$(
    /usr/bin/printf '%s\0%s\0%s\0' \
      "$sdk_version" "$sdk_build_version" "$sdk_settings_sha256" |
      /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
  )" || return 1

  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT="$resolved_sdk"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_VERSION="$sdk_version"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_BUILD_VERSION="$sdk_build_version"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_SETTINGS_SHA256="$sdk_settings_sha256"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG="-isysroot$resolved_sdk"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY="$sdk_identity"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_XCRUN="$xcrun_command"
  SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_EXPECTED_VERSION="$expected_version"
}

switchyard_validate_qualified_native_macos_sdk() {
  local expected_root="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT:-}"
  local expected_version="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_VERSION:-}"
  local expected_build_version="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_BUILD_VERSION:-}"
  local expected_settings_sha256="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_SETTINGS_SHA256:-}"
  local expected_flag="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG:-}"
  local expected_identity="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY:-}"
  local expected_xcrun="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_XCRUN:-}"
  local contract_version="${SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_EXPECTED_VERSION:-}"

  [ -n "$expected_root" ] && [ -n "$expected_identity" ] &&
    [ -n "$expected_xcrun" ] && [ -n "$contract_version" ] || {
    echo "Native macOS SDK policy was not qualified." >&2
    return 1
  }
  switchyard_qualify_native_macos_sdk "$contract_version" "$expected_xcrun" ||
    return 1
  [ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT" = "$expected_root" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_VERSION" = "$expected_version" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_BUILD_VERSION" = "$expected_build_version" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_SETTINGS_SHA256" = "$expected_settings_sha256" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG" = "$expected_flag" ] &&
    [ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY" = "$expected_identity" ] || {
    echo "Qualified native macOS SDK identity changed during the build." >&2
    return 1
  }
}

switchyard_native_configured_compiler_policy_is_exact() {
  [ "$#" -eq 5 ] || return 2

  local makefile="$1"
  local clang="$2"
  local clangxx="$3"
  local architecture="$4"
  local no_default_config_flag="$5"
  local expected_cc="$clang $no_default_config_flag -arch $architecture"
  local expected_cc_c23="$expected_cc -std=gnu23"
  local expected_cxx="$clangxx $no_default_config_flag -arch $architecture"

  [ -f "$makefile" ] && [ ! -L "$makefile" ] || return 1
  # AC_PROG_CC's C23 probe may append its canonical language-mode switch to CC
  # after qualification.  It is configure-owned rather than ambient policy,
  # but no other compiler suffix is permitted without a contract update.
  /usr/bin/awk -v expected_cc="$expected_cc" \
      -v expected_cc_c23="$expected_cc_c23" -v expected_cxx="$expected_cxx" '
    function assignment(line, variable) {
      return line ~ ("^[[:space:]]*" variable "[[:space:]]*[:+?!]?=")
    }
    function directive(line, variable) {
      return line ~ ("^[[:space:]]*(override|export|private|unexport)[[:space:]]+" \
                     variable "([[:space:]]|[:+?!]?=|$)") ||
             line ~ ("^[[:space:]]*(define|undefine)[[:space:]]+" \
                     variable "([[:space:]]|$)")
    }
    /^[[:space:]]*#/ { next }
    {
      if (assignment($0, "CC") || directive($0, "CC")) {
        cc_count++
        if ($0 == "CC = " expected_cc || $0 == "CC = " expected_cc_c23) cc_exact++
      }
      if (assignment($0, "CXX") || directive($0, "CXX")) {
        cxx_count++
        if ($0 == "CXX = " expected_cxx) cxx_exact++
      }
      if (assignment($0, "OBJC") || directive($0, "OBJC")) {
        objc_count++
        if ($0 == "OBJC = " expected_cc) objc_exact++
      }
    }
    END {
      exit !(cc_count == 1 && cc_exact == 1 &&
             cxx_count == 1 && cxx_exact == 1 &&
             objc_count == 1 && objc_exact == 1)
    }
  ' "$makefile"
}

switchyard_native_makefile_assignment_has_exact_target_policy() {
  [ "$#" -eq 8 ] || return 2

  local makefile="$1"
  local variable="$2"
  local expected_deployment="$3"
  local expected_sdk="$4"
  local required_deployment_count="$5"
  local required_sdk_count="$6"
  local required_arch_count="$7"
  local required_config_count="$8"

  [ -f "$makefile" ] && [ ! -L "$makefile" ] || return 1
  /usr/bin/awk -v variable="$variable" \
      -v expected_deployment="$expected_deployment" -v expected_sdk="$expected_sdk" \
      -v required_deployment_count="$required_deployment_count" \
      -v required_sdk_count="$required_sdk_count" \
      -v required_arch_count="$required_arch_count" \
      -v required_config_count="$required_config_count" '
    function normalize(value) {
      gsub(/["\\\047]/, "", value)
      return value
    }
    function has_unsafe_metachar(value) {
      return value ~ /[\\$`;|&<>*?\[\]{}()#]/ ||
             index(value, "\"") || index(value, "\047")
    }
    function is_deployment(value) {
      return value ~ /-m[[:alnum:]_-]*version-min/ || index(value, "-mtargetos")
    }
    function is_sdk(value) {
      return index(value, "-isysroot") || index(value, "--sysroot") ||
             index(value, "-syslibroot")
    }
    function is_other_override(value) {
      return index(value, "-target") ||
             index(value, "apple-macos") || index(value, "apple-darwin") ||
             index(value, "-platform_version") ||
             value ~ /-[[:alnum:]_-]*_version_min/ ||
             index(value, "-sdk_version") || index(value, "-target-sdk-version") ||
             index(value, "-Xarch_") || value ~ /-m(cpu|arch|tune|abi)(=|$)/ ||
             index(value, "-Wl,@") || index(value, "-Xclang") ||
             index(value, "-Xlinker") || index(value, "--driver-mode") ||
             index(value, "/clang:") ||
             index(value, "-fuse-ld") || index(value, "--ld-path") ||
             value ~ /^-B/ || index(value, "-ccc-install-dir") ||
             index(value, "-ccc-gcc-name") ||
             index(value, "-resource-dir") ||
             index(value, "--config") == 1 || substr(value, 1, 1) == "@"
    }
    $1 == variable && $2 == "=" {
      assignments++
      if ($0 ~ /\\[[:space:]]*$/) invalid = 1
      for (field = 3; field <= NF; field++)
      {
        raw = $field
        value = normalize(raw)
        if (has_unsafe_metachar(raw)) invalid = 1
        if (is_deployment(value)) {
          deployment_count++
          if (raw != expected_deployment) invalid = 1
        }
        if (is_sdk(value)) {
          sdk_count++
          if (raw != expected_sdk) invalid = 1
        }
        if (index(value, "-arch")) {
          arch_count++
          if (raw != "-arch" || normalize($(field + 1)) != "arm64") invalid = 1
        }
        if (value == "--no-default-config") {
          config_count++
          if (raw != "--no-default-config") invalid = 1
        }
        else if (is_other_override(value)) invalid = 1
      }
    }
    END {
      valid = assignments == 1 && deployment_count == required_deployment_count &&
              sdk_count == required_sdk_count && arch_count == required_arch_count &&
              config_count == required_config_count && !invalid
      exit !valid
    }
  ' "$makefile"
}

switchyard_native_makefile_assignment_uses_exact_compiler() {
  [ "$#" -eq 3 ] || return 2

  local makefile="$1"
  local variable="$2"
  local expected_compiler="$3"

  [ -f "$makefile" ] && [ ! -L "$makefile" ] || return 1
  /usr/bin/awk -v variable="$variable" -v expected_compiler="$expected_compiler" '
    $1 == variable && $2 == "=" { assignments++; if ($3 == expected_compiler) matches++ }
    END { exit !(assignments == 1 && matches == 1) }
  ' "$makefile"
}

switchyard_native_configured_host_target_policy_is_exact() {
  [ "$#" -eq 5 ] || return 2

  local makefile="$1"
  local deployment_flag="$2"
  local sdk_flag="$3"
  local host_clang="$4"
  local host_clangxx="$5"
  local variable

  switchyard_native_makefile_assignment_uses_exact_compiler \
    "$makefile" CC "$host_clang" || return 1
  switchyard_native_makefile_assignment_uses_exact_compiler \
    "$makefile" CXX "$host_clangxx" || return 1
  switchyard_native_makefile_assignment_uses_exact_compiler \
    "$makefile" OBJC "$host_clang" || return 1
  for variable in CC CXX OBJC; do
    switchyard_native_makefile_assignment_has_exact_target_policy \
      "$makefile" "$variable" "$deployment_flag" "$sdk_flag" 0 0 1 1 || return 1
  done
  switchyard_native_makefile_assignment_has_exact_target_policy \
    "$makefile" CPPFLAGS "$deployment_flag" "$sdk_flag" 0 0 0 0 || return 1
  for variable in CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS; do
    switchyard_native_makefile_assignment_has_exact_target_policy \
      "$makefile" "$variable" "$deployment_flag" "$sdk_flag" 1 1 0 0 || return 1
  done
}

switchyard_native_runtime_closure_digest() {
  [ "$#" -eq 17 ] || {
    echo "Native runtime closure requires the exact 17-input contract." >&2
    return 2
  }

  /usr/bin/python3 -I - "$@" <<'PY'
import hashlib
import struct
import sys

labels = (
    "sourceIdentity",
    "sourceRevision",
    "sourceDirty",
    "sourceTreeDigest",
    "gptkRedistDigest",
    "wineMonoDigest",
    "gstreamerRuntimeDigest",
    "vulkanRuntimeDigest",
    "mesaWindowsDigest",
    "fontRuntimeDigest",
    "fontAssetsDigest",
    "tlsRuntimeDigest",
    "tlsDlopenName",
    "tlsDlopenDigest",
    "unicornRuntimeDigest",
    "dxmtArtifactSha256",
    "nativeCompilerIdentity",
)
values = sys.argv[1:]
if len(values) != len(labels):
    raise SystemExit(2)
if values[2] not in ("true", "false"):
    raise SystemExit("native runtime closure sourceDirty is invalid")
if values[4] != "no-gptk":
    raise SystemExit("native runtime closure must not bind a GPTK overlay")
hex64 = set("0123456789abcdef")
for index in (5, 6, 7, 8, 9, 10, 14, 15, 16):
    if len(values[index]) != 64 or set(values[index]) - hex64:
        raise SystemExit("native runtime closure input is not a full SHA-256: " + labels[index])
for index in (11,):
    if values[index] != "none" and (len(values[index]) != 64 or set(values[index]) - hex64):
        raise SystemExit("native runtime closure TLS digest is invalid")
if values[12] == "none" or any(ord(character) < 0x20 for character in values[12]):
    raise SystemExit("native runtime closure TLS dlopen name is invalid")
if values[13] == "none" or len(values[13]) != 64 or set(values[13]) - hex64:
    raise SystemExit("native runtime closure TLS dlopen digest is invalid")
if not 12 <= len(values[1]) <= 64 or set(values[1]) - hex64:
    raise SystemExit("native runtime closure source revision is invalid")
if len(values[3]) != 64 or set(values[3]) - hex64:
    raise SystemExit("native runtime closure source state digest is invalid")

digest = hashlib.sha256()
digest.update(b"switchyard-preview-native-arm64-fex-runtime-closure-v1\0")
for label, value in zip(labels, values):
    key = label.encode("utf-8", "strict")
    payload = value.encode("utf-8", "strict")
    digest.update(struct.pack(">Q", len(key)))
    digest.update(key)
    digest.update(struct.pack(">Q", len(payload)))
    digest.update(payload)
print(digest.hexdigest())
PY
}

switchyard_native_runtime_id_from_closure_digest() {
  [ "$#" -eq 4 ] || return 2

  local profile_prefix="$1"
  local source_revision="$2"
  local source_dirty="$3"
  local closure_digest="$4"
  local source_marker

  [ "$profile_prefix" = "switchyard-local-native-arm64-fex-" ] || {
    echo "Native runtime ID prefix is not the preview profile prefix." >&2
    return 1
  }
  [[ "$source_revision" =~ ^[0-9a-f]{12,64}$ ]] || {
    echo "Native runtime source revision is invalid." >&2
    return 1
  }
  [[ "$closure_digest" =~ ^[0-9a-f]{64}$ ]] || {
    echo "Native runtime closure digest must be a full SHA-256." >&2
    return 1
  }
  source_marker="${source_revision:0:12}"
  case "$source_dirty" in
    true) source_marker="${source_marker}-dirty" ;;
    false) ;;
    *) echo "Native runtime source dirty marker is invalid." >&2; return 1 ;;
  esac
  /usr/bin/printf '%s%s-%s\n' "$profile_prefix" "$source_marker" "$closure_digest"
}

switchyard_canonical_runtime_install_prefix() {
  [ "$#" -eq 1 ] || return 2

  /usr/bin/python3 -I - "$1" <<'PY'
import os
import sys

path = sys.argv[1]
if not os.path.isabs(path):
    raise SystemExit("Runtime install prefix must be an absolute path.")
if any(ord(character) < 0x20 for character in path):
    raise SystemExit("Runtime install prefix contains a control character.")
print(os.path.realpath(os.path.normpath(path)))
PY
}

switchyard_utf16_code_unit_count() {
  [ "$#" -eq 1 ] || return 2

  /usr/bin/python3 -I - "$1" <<'PY'
import sys

try:
    encoded = sys.argv[1].encode("utf-16-le", "strict")
except UnicodeEncodeError:
    raise SystemExit(1)
print(len(encoded) // 2)
PY
}

switchyard_validate_native_runtime_prefix_bootstrap_budget() {
  [ "$#" -eq 1 ] || return 2

  local runtime_root="$1"
  local canonical_root
  local runtime_root_units
  local installhinf_tail_units
  local max_visible_path

  canonical_root="$(switchyard_canonical_runtime_install_prefix "$runtime_root")" || return 1
  [ "$canonical_root" = "$runtime_root" ] || {
    echo "Native runtime install prefix must use its canonical physical path: $canonical_root" >&2
    return 1
  }
  runtime_root_units="$(switchyard_utf16_code_unit_count "$runtime_root")" || {
    echo "Native runtime install prefix must be representable as UTF-16." >&2
    return 1
  }
  installhinf_tail_units=$((
    ${#SWITCHYARD_RUNTIME_BOOTSTRAP_INSTALLHINF_PREFIX} +
    SWITCHYARD_RUNTIME_BOOTSTRAP_NT_PREFIX_OVERHEAD + runtime_root_units + 1 +
    ${#SWITCHYARD_RUNTIME_BOOTSTRAP_WINE_INF_RELATIVE_PATH}
  ))
  max_visible_path=$((SWITCHYARD_RUNTIME_BOOTSTRAP_MAX_PATH - 1))
  if [ "$installhinf_tail_units" -gt "$max_visible_path" ]; then
    echo "Native runtime install prefix is too long for fresh Wine prefix setup." >&2
    echo "InstallHinfSection would publish a $installhinf_tail_units-UTF-16-code-unit wine.inf command tail; maximum is $max_visible_path." >&2
    return 1
  fi
}

# Native CPU-provider inputs are a closed build policy.  The development tree
# is used only while configuring Wine; runtime packaging selects the validated
# dylib and its redistributable provenance materials from that tree.
SWITCHYARD_UNICORN_VERSION="2.1.4"
SWITCHYARD_UNICORN_SOURCE_REPOSITORY="https://github.com/unicorn-engine/unicorn.git"
SWITCHYARD_UNICORN_SOURCE_REVISION="8028ec436f2d9376525352dd38ed9ed6b9f6be10"
SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256="d3859317cc562ad9d172a32a4e4c2e62613df494b1155a0bf58dd0581fc1675e"
SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME="unicorn-2.1.4-threaded-emu-stop.patch"
SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256="68f7df756eec731ec2143d63b8e454b7d1d538ccee8fa2205563fb26c3b995d6"
SWITCHYARD_UNICORN_LIBRARY_SHA256="60b4c1e2cec6459c8d5bf7aafa11b43f79e029ec85b199e116dd29a4f0636b07"
SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION="3"
SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST="7944bfa5710dfec5183ac1b8460c9dfce5f7c4b0af40675ce6afb893a3e38e87"
SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST="a9a853d25af1274fde256ac3dff2b83ba563d8cbe70c5c7a754ca93b94ee0486"

# The native graphics input is a closed local artifact.  There is no mutable
# download URL in the build policy: callers may select a local path, but both
# the archive and the corresponding Git source are verified against every
# identity below before any file reaches a runtime staging tree.
SWITCHYARD_DXMT_SOURCE_REPOSITORY="https://github.com/3Shain/dxmt.git"
SWITCHYARD_DXMT_SOURCE_REVISION="856d9f35789679ef00c1ba01a6353438df84b66f"
SWITCHYARD_DXMT_ARTIFACT_NAME="dxmt-${SWITCHYARD_DXMT_SOURCE_REVISION}.tar.gz"
SWITCHYARD_DXMT_ARTIFACT_SHA256="8840df7038d7cbffed3652712c86ec4d6d495612aa39306e9a184bd213514acf"
SWITCHYARD_DXMT_PACKAGE_WORKFLOW=".github/workflows/ci.yml"
SWITCHYARD_DXMT_PACKAGE_WORKFLOW_SHA256="fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851"
SWITCHYARD_DXMT_PACKAGE_BUILD="gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross"
SWITCHYARD_DXMT_LICENSE_SHA256="b87c35aef7b2cf14de854118ca55ce5c4b284c85b5f002421fb8d46d868c2d17"
SWITCHYARD_DXMT_COPYING_SHA256="e237fa56668030e928551ddd60f05df5fe957f75eab874bbd017e085ed722e7c"
SWITCHYARD_DXMT_CORRESPONDING_SOURCE_SHA256="40bbbbecb9c48cfd67f5862b0b93878ae80dc3de083790d3ec9dadd98618c89a"

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
      SWITCHYARD_RUNTIME_PROFILE_INSTALLED_PE_ARCHS=("i386" "x86_64")
      SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET="x86_64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET="x86_64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND=("arch" "-x86_64")
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA="true"
      SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS="14.0"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH="x86_64"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION="universal (x86_64 used by Wine under Rosetta)"
      SWITCHYARD_RUNTIME_PROFILE_HOST_DEPENDENCY_ARCH="x86_64"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_MACHO_ARCHS=("x86_64" "arm64")
      SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG="sonoma"
      SWITCHYARD_RUNTIME_PROFILE_VULKAN_LOADER_BOTTLE_TAG="tahoe"
      SWITCHYARD_RUNTIME_PROFILE_VULKAN_HEADERS_BOTTLE_TAG="tahoe"
      SWITCHYARD_RUNTIME_PROFILE_MOLTENVK_BOTTLE_TAG="sonoma"
      SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_SUBDIR="osx-64"
      SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_MANIFEST_BASENAME="tls-deps.tsv"
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_UNICORN="false"
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_DXMT="false"
      SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL="direct"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_CACHE_BASENAME="build-wow64-x86_64"
      SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX="macos-x86_64"
      SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME="wine-runtime.entitlements"
      ;;
    preview-native-arm64-fex)
      SWITCHYARD_RUNTIME_PROFILE="$profile"
      SWITCHYARD_RUNTIME_PROFILE_ENABLED="1"
      SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX="switchyard-local-native-arm64-fex-"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_PROFILE="switchyard-native-arm64-fex"
      SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH="arm64"
      SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH="aarch64"
      SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS=("aarch64" "arm64ec" "x86_64" "i386")
      SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV="aarch64,arm64ec,x86_64,i386"
      SWITCHYARD_RUNTIME_PROFILE_INSTALLED_PE_ARCHS=("aarch64" "x86_64" "i386")
      SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET="aarch64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET="aarch64-apple-darwin"
      SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND=("arch" "-arm64")
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA="false"
      SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS="26.5"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH="arm64"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION="universal (arm64 used by native Wine)"
      SWITCHYARD_RUNTIME_PROFILE_HOST_DEPENDENCY_ARCH="arm64"
      SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_MACHO_ARCHS=("x86_64" "arm64")
      SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG="arm64_sonoma"
      SWITCHYARD_RUNTIME_PROFILE_VULKAN_LOADER_BOTTLE_TAG="arm64_tahoe"
      SWITCHYARD_RUNTIME_PROFILE_VULKAN_HEADERS_BOTTLE_TAG="arm64_tahoe"
      SWITCHYARD_RUNTIME_PROFILE_MOLTENVK_BOTTLE_TAG="arm64_tahoe"
      SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_SUBDIR="osx-arm64"
      SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_MANIFEST_BASENAME="tls-deps-arm64.tsv"
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_UNICORN="true"
      SWITCHYARD_RUNTIME_PROFILE_REQUIRES_DXMT="true"
      SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL="translated-shadow"
      SWITCHYARD_RUNTIME_PROFILE_BUILD_CACHE_BASENAME="build-native-arm64-fex"
      SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX="macos-arm64"
      SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME="wine-runtime-native-arm64.entitlements"
      ;;
    *)
      echo "Unknown runtime profile. Expected stable-x86_64-rosetta or preview-native-arm64-fex." >&2
      return 2
      ;;
  esac
}

switchyard_runtime_profile_entitlements_path() {
  local root="$1"

  case "${SWITCHYARD_RUNTIME_PROFILE:-}:${SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME:-}" in
    stable-x86_64-rosetta:wine-runtime.entitlements|\
    preview-native-arm64-fex:wine-runtime-native-arm64.entitlements)
      /usr/bin/printf '%s/switchyard/%s\n' \
        "$root" "$SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME"
      ;;
    *)
      echo "Runtime profile signing entitlements are not allowlisted." >&2
      return 2
      ;;
  esac
}

switchyard_require_runtime_profile_enabled() {
  if [ "$SWITCHYARD_RUNTIME_PROFILE_ENABLED" = "1" ]; then
    return 0
  fi

  echo "Runtime profile $SWITCHYARD_RUNTIME_PROFILE is recognized but not enabled." >&2
  echo "The native ARM64 CPU-provider, JIT, signing, and qualification gates have not passed." >&2
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

switchyard_validate_signed_refresh_capability_fd() {
  local runtime_root="$1"
  local refresh_token="$2"
  local taint_device="$3"
  local taint_inode="$4"
  local capability_token="$5"
  local capability_fd="$6"
  local capability_device="$7"
  local capability_inode="$8"

  [ "$#" -eq 8 ] || return 2
  /usr/bin/python3 -I - \
    "$runtime_root" "$refresh_token" "$taint_device" "$taint_inode" \
    "$capability_token" "$capability_fd" \
    "$capability_device" "$capability_inode" <<'PY'
import hashlib
import os
import re
import stat
import sys

(
    root_name,
    refresh_token,
    taint_device_text,
    taint_inode_text,
    capability_token,
    descriptor_text,
    capability_device_text,
    capability_inode_text,
) = sys.argv[1:]
name = ".switchyard-signed-manifest-refresh-in-progress"
directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
file_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
if (
    not os.path.isabs(root_name)
    or os.path.normpath(root_name) != root_name
    or root_name == "/"
    or os.path.realpath(root_name) != root_name
    or re.fullmatch(r"[0-9a-f]{64}", refresh_token) is None
    or re.fullmatch(r"[0-9a-f]{64}", capability_token) is None
    or descriptor_text != "18"
    or any(
        re.fullmatch(r"[1-9][0-9]*", item) is None
        for item in (
            taint_device_text,
            taint_inode_text,
            capability_device_text,
            capability_inode_text,
        )
    )
):
    raise SystemExit(1)
descriptor = int(descriptor_text)
taint_identity = int(taint_device_text), int(taint_inode_text)
capability_identity = int(capability_device_text), int(capability_inode_text)
if taint_identity == capability_identity:
    raise SystemExit(1)

root_fd = os.open("/", directory_flags)
taint_fd = -1
try:
    for component in root_name.split("/")[1:]:
        child = os.open(component, directory_flags, dir_fd=root_fd)
        os.close(root_fd)
        root_fd = child
    root = os.fstat(root_fd)
    if (
        not stat.S_ISDIR(root.st_mode)
        or root.st_uid != os.geteuid()
        or root.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise SystemExit(1)
    taint_fd = os.open(name, file_flags, dir_fd=root_fd)
    taint = os.fstat(taint_fd)
    current_taint = os.stat(name, dir_fd=root_fd, follow_symlinks=False)
    if (
        not stat.S_ISREG(taint.st_mode)
        or taint.st_uid != os.geteuid()
        or stat.S_IMODE(taint.st_mode) != 0o600
        or taint.st_nlink != 1
        or taint.st_size != 65
        or (taint.st_dev, taint.st_ino) != taint_identity
        or os.pread(taint_fd, 66, 0) != (refresh_token + "\n").encode("ascii")
        or (
            current_taint.st_dev, current_taint.st_ino, current_taint.st_mode,
            current_taint.st_nlink, current_taint.st_uid, current_taint.st_size,
        ) != (
            taint.st_dev, taint.st_ino, taint.st_mode,
            taint.st_nlink, taint.st_uid, taint.st_size,
        )
    ):
        raise SystemExit(1)
    capability = os.fstat(descriptor)
    capability_commitment = hashlib.sha256(
        b"switchyard-signed-refresh-capability-v1\0"
        + capability_token.encode("ascii")
        + b"\0"
        + str(capability.st_dev).encode("ascii")
        + b"\0"
        + str(capability.st_ino).encode("ascii")
    ).hexdigest()
    if (
        not stat.S_ISREG(capability.st_mode)
        or capability.st_uid != os.geteuid()
        or stat.S_IMODE(capability.st_mode) != 0o400
        or capability.st_nlink != 0
        or capability.st_size != 65
        or (capability.st_dev, capability.st_ino) != capability_identity
        or os.pread(descriptor, 66, 0)
        != (capability_token + "\n").encode("ascii")
        or (capability.st_dev, capability.st_ino) == taint_identity
        or capability_commitment != refresh_token
    ):
        raise SystemExit(1)
finally:
    if taint_fd >= 0:
        os.close(taint_fd)
    os.close(root_fd)
PY
}

switchyard_validate_native_runtime_bound_identity() {
  local manifest="$1"
  local runtime_root="$2"
  local library_dir digest_helper

  library_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
  digest_helper="$(cd "$library_dir/.." && pwd -P)/runtime_content_digest.py"
  [ -f "$digest_helper" ] && [ ! -L "$digest_helper" ] && [ -x "$digest_helper" ] || {
    switchyard_runtime_manifest_error \
      "runtime-bound native identity helper is missing or unsafe"
    return 1
  }

  /usr/bin/python3 -I - "$manifest" "$runtime_root" "$digest_helper" <<'PY'
import hashlib
import json
import os
import plistlib
import re
import stat
import subprocess
import sys

manifest_name, root_name, digest_helper = sys.argv[1:]
MAX_BINARY = 512 * 1024 * 1024
MAX_MANIFEST = 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}")
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC


def fail(message):
    raise SystemExit("runtime-bound native profile identity is invalid: " + message)


def no_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            fail("duplicate JSON object key: " + key)
        value[key] = item
    return value


def stable(info):
    return (
        info.st_dev, info.st_ino, info.st_mode, info.st_nlink, info.st_uid,
        info.st_gid, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
    )


if (
    not os.path.isabs(root_name)
    or os.path.normpath(root_name) != root_name
    or root_name == "/"
    or os.path.realpath(root_name) != root_name
    or manifest_name != os.path.join(root_name, "switchyard-runtime.json")
):
    fail("runtime root or manifest path is not canonical")

root_fd = os.open("/", DIRECTORY_FLAGS)
try:
    for component in root_name.split("/")[1:]:
        child = os.open(component, DIRECTORY_FLAGS, dir_fd=root_fd)
        os.close(root_fd)
        root_fd = child
    root_info = os.fstat(root_fd)
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
        or root_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        fail("runtime root has an unsafe type, owner, or mode")

    def read_file(relative, maximum=MAX_BINARY):
        parts = relative.split("/")
        if any(part in ("", ".", "..") for part in parts):
            fail("runtime-relative identity path is invalid: " + relative)
        descriptor = os.dup(root_fd)
        try:
            for part in parts[:-1]:
                child = os.open(part, DIRECTORY_FLAGS, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
            child = os.open(parts[-1], FILE_FLAGS, dir_fd=descriptor)
        finally:
            os.close(descriptor)
        try:
            before = os.fstat(child)
            if (
                not stat.S_ISREG(before.st_mode)
                or before.st_size <= 0
                or before.st_size > maximum
                or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            ):
                fail("runtime identity file has an unsafe type, size, or mode: " + relative)
            chunks = []
            remaining = before.st_size
            while remaining:
                block = os.read(child, min(1024 * 1024, remaining))
                if not block:
                    fail("runtime identity file ended early: " + relative)
                chunks.append(block)
                remaining -= len(block)
            if os.read(child, 1) or stable(os.fstat(child)) != stable(before):
                fail("runtime identity file changed while reading: " + relative)
            return b"".join(chunks)
        finally:
            os.close(child)

    manifest_data = read_file("switchyard-runtime.json", MAX_MANIFEST)
    try:
        value = json.loads(
            manifest_data.decode("utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=lambda item: fail("non-standard JSON constant: " + item),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail("cannot parse runtime manifest: " + str(error))
    if type(value) is not dict or value.get("runtimeFamily") != "preview-native-arm64-fex":
        fail("manifest is not the native preview profile")

    provider = value.get("cpuProvider")
    signing = value.get("runtimeSigning")
    if type(provider) is not dict or type(signing) is not dict:
        fail("signed runtime provider or signing identity is absent")
    if set(signing) != {"mode", "processEntryMachOs"}:
        fail("runtimeSigning has an unexpected field set")
    mode = signing.get("mode")
    if mode not in ("engineering-adhoc", "developer-id-hardened-runtime"):
        fail("runtimeSigning mode is not allowlisted")
    entries = signing.get("processEntryMachOs")
    expected_entries = [
        "lib/wine/aarch64-unix/wine",
        "bin/wine.switchyard-real",
    ]
    if type(entries) is not list or len(entries) != len(expected_entries):
        fail("runtimeSigning process-entry set is not exact")
    expected_entitlements = {
        "com.apple.security.cs.allow-dyld-environment-variables": True,
        "com.apple.security.cs.allow-jit": True,
        "com.apple.security.cs.allow-unsigned-executable-memory": True,
        "com.apple.security.custom-x18-abi-toggle": True,
    }

    for item, relative in zip(entries, expected_entries):
        if type(item) is not dict or set(item) != {"path", "sha256"} or item.get("path") != relative:
            fail("runtimeSigning process-entry order or schema is invalid")
        expected_digest = item.get("sha256")
        data = read_file(relative)
        if type(expected_digest) is not str or SHA256.fullmatch(expected_digest) is None:
            fail("runtimeSigning process-entry digest is malformed")
        if hashlib.sha256(data).hexdigest() != expected_digest:
            fail("runtimeSigning digest does not match bytes: " + relative)
        path = os.path.join(root_name, relative)
        strict = subprocess.run(
            ["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if strict.returncode:
            fail("runtime process entry failed strict signature validation: " + relative)
        details_result = subprocess.run(
            ["/usr/bin/codesign", "-d", "--verbose=4", path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if details_result.returncode:
            fail("cannot inspect runtime process-entry signature: " + relative)
        details = (details_result.stdout + details_result.stderr).decode("utf-8", "strict")
        if mode == "engineering-adhoc":
            if (
                details.splitlines().count("Signature=adhoc") != 1
                or len(re.findall(r"\bflags=0x2\(adhoc\)(?:\s|$)", details)) != 1
                or "Runtime Version=" in details
                or re.search(r"\bflags=.*(?:\(|,)runtime(?:,|\)|\s|$)", details)
            ):
                fail("runtime process entry is not exact engineering ad-hoc mode: " + relative)
        else:
            if (
                "Signature=adhoc" in details
                or "Runtime Version=" not in details
                or not any(
                    line.startswith("Authority=Developer ID Application:")
                    for line in details.splitlines()
                )
            ):
                fail("runtime process entry is not Developer-ID Hardened Runtime mode: " + relative)
        entitlements_result = subprocess.run(
            ["/usr/bin/codesign", "-d", "--xml", "--entitlements", "-", path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if entitlements_result.returncode or len(entitlements_result.stdout) > 65536:
            fail("cannot inspect runtime process-entry entitlements: " + relative)
        try:
            embedded = plistlib.loads(entitlements_result.stdout)
        except plistlib.InvalidFileException as error:
            fail("runtime process-entry entitlements are malformed: " + str(error))
        if (
            type(embedded) is not dict
            or set(embedded) != set(expected_entitlements)
            or any(
                type(embedded[key]) is not bool
                or embedded[key] is not expected_entitlements[key]
                for key in expected_entitlements
            )
        ):
            fail("runtime process-entry entitlements are not the exact allowlist: " + relative)

    for component in provider.get("components", []):
        for path_key, digest_key in (
            ("unixLibrary", "unixLibrarySha256"),
            ("peLibrary", "peLibrarySha256"),
        ):
            relative = component[path_key]
            if hashlib.sha256(read_file(relative)).hexdigest() != component[digest_key]:
                fail("CPU-provider component identity is not runtime-bound: " + relative)

    unicorn_relative = provider.get("library")
    unicorn_digest = provider.get("librarySha256")
    if hashlib.sha256(read_file(unicorn_relative)).hexdigest() != unicorn_digest:
        fail("Unicorn library identity is not runtime-bound")
    nested = json.loads(
        read_file(provider.get("manifest"), MAX_MANIFEST).decode("utf-8"),
        object_pairs_hook=no_duplicates,
    )
    if type(nested) is not dict or nested.get("librarySha256") != unicorn_digest:
        fail("nested Unicorn manifest is not bound to the final library")

    package_root = os.path.join(root_name, provider.get("runtimeRoot"))
    verify = subprocess.run(
        ["/usr/bin/python3", "-I", digest_helper, "verify", package_root],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    digest = subprocess.run(
        ["/usr/bin/python3", "-I", digest_helper, "digest", package_root],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    if verify.returncode or digest.returncode or digest.stdout.strip() != provider.get("runtimePayloadDigest"):
        fail("Unicorn payload identity is not runtime-bound")
finally:
    os.close(root_fd)
PY
}

switchyard_validate_runtime_manifest_profile() {
  local manifest="$1"
  local expected_profile="$2"
  local runtime_root="${3:-${SWITCHYARD_RUNTIME_PROFILE_BOUND_ROOT:-}}"
  local actual
  local index

  [ "$#" -eq 2 ] || [ "$#" -eq 3 ] || {
    echo "usage: switchyard_validate_runtime_manifest_profile MANIFEST PROFILE [RUNTIME]" >&2
    return 2
  }
  if [ -n "$runtime_root" ] &&
     { [ -e "$runtime_root/.switchyard-signed-manifest-refresh-in-progress" ] ||
       [ -L "$runtime_root/.switchyard-signed-manifest-refresh-in-progress" ]; }; then
    if [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_TOKEN:-}" ] ||
       [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_DEVICE:-}" ] ||
       [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_INODE:-}" ] ||
       [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_TOKEN:-}" ] ||
       [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_FD:-}" ] ||
       [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_DEVICE:-}" ] ||
       [ -z "${SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_INODE:-}" ] ||
       ! switchyard_validate_signed_refresh_capability_fd \
         "$runtime_root" "$SWITCHYARD_NATIVE_SIGNED_REFRESH_TOKEN" \
         "$SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_DEVICE" \
         "$SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_INODE" \
         "$SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_TOKEN" \
         "$SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_FD" \
         "$SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_DEVICE" \
         "$SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_INODE" >/dev/null 2>&1; then
      switchyard_runtime_manifest_error \
        "runtime is tainted by an incomplete signed-manifest refresh"
      return 1
    fi
  fi

  [ -f "$manifest" ] && [ ! -L "$manifest" ] || {
    switchyard_runtime_manifest_error "manifest must be a regular file"
    return 1
  }
  /usr/bin/python3 -I - "$manifest" "$expected_profile" >/dev/null 2>&1 <<'PY' || {
import json
import os
import re
import sys

manifest, expected_profile = sys.argv[1:]
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
if expected_profile == "preview-native-arm64-fex":
    require_exact_type(value, "cpuProvider", dict)
    provider = value["cpuProvider"]
    string_fields = (
        "implementation",
        "version",
        "sourceRepository",
        "sourceRevision",
        "sourceArchive",
        "sourceArchiveSha256",
        "hostArchitecture",
        "kuserSharedDataModel",
        "developmentCacheDigest",
        "runtimeRoot",
        "runtimePayloadDigest",
        "library",
        "librarySha256",
        "runtimeRpath",
        "manifest",
    )
    expected_fields = set(string_fields) | {
        "buildContractVersion",
        "components",
        "emulatedArchitectures",
        "providerUnixLibraries",
        "sourcePatch",
    }
    if set(provider) != expected_fields:
        raise ValueError("cpuProvider has an unexpected field set")
    for key in string_fields:
        require_exact_type(provider, key, str)
    require_exact_type(provider, "buildContractVersion", int)
    require_exact_type(provider, "sourcePatch", dict)
    if set(provider["sourcePatch"]) != {"path", "sha256"}:
        raise ValueError("cpuProvider.sourcePatch has an unexpected field set")
    for key in ("path", "sha256"):
        require_exact_type(provider["sourcePatch"], key, str)
    if re.fullmatch(r"[0-9a-f]{64}", provider["sourcePatch"]["sha256"]) is None:
        raise ValueError("cpuProvider source patch digest is malformed")
    for key in ("emulatedArchitectures", "providerUnixLibraries"):
        require_exact_type(provider, key, list)
        if not all(type(item) is str for item in provider[key]):
            raise ValueError("cpuProvider list contains a non-string value: " + key)
    require_exact_type(provider, "components", list)
    if len(provider["components"]) != 2:
        raise ValueError("cpuProvider.components has an unexpected length")
    component_fields = {
        "guestArchitecture",
        "unixLibrary",
        "unixLibrarySha256",
        "peLibrary",
        "peLibrarySha256",
    }
    for component in provider["components"]:
        if type(component) is not dict or set(component) != component_fields:
            raise ValueError("cpuProvider component has an unexpected field set")
        for key in component_fields:
            if type(component[key]) is not str:
                raise ValueError("cpuProvider component field has an unexpected type: " + key)
        for key in ("unixLibrarySha256", "peLibrarySha256"):
            if re.fullmatch(r"[0-9a-f]{64}", component[key]) is None:
                raise ValueError("cpuProvider component digest is malformed: " + key)
    for key in (
        "sourceArchiveSha256",
        "developmentCacheDigest",
        "runtimePayloadDigest",
        "librarySha256",
    ):
        if re.fullmatch(r"[0-9a-f]{64}", provider[key]) is None:
            raise ValueError("cpuProvider digest is malformed: " + key)
    if "runtimeSigning" in value:
        signing = value["runtimeSigning"]
        if type(signing) is not dict or set(signing) != {"mode", "processEntryMachOs"}:
            raise ValueError("runtimeSigning has an unexpected field set")
        if signing.get("mode") not in (
            "engineering-adhoc",
            "developer-id-hardened-runtime",
        ):
            raise ValueError("runtimeSigning mode is not allowlisted")
        entries = signing.get("processEntryMachOs")
        expected_paths = [
            "lib/wine/aarch64-unix/wine",
            "bin/wine.switchyard-real",
        ]
        if type(entries) is not list or len(entries) != len(expected_paths):
            raise ValueError("runtimeSigning process-entry set is not exact")
        for item, path in zip(entries, expected_paths):
            if (
                type(item) is not dict
                or set(item) != {"path", "sha256"}
                or item.get("path") != path
                or type(item.get("sha256")) is not str
                or re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) is None
            ):
                raise ValueError("runtimeSigning process-entry identity is invalid")
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
  if [ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_UNICORN" = "true" ]; then
    actual="$(switchyard_runtime_manifest_value cpuProvider.implementation "$manifest")"
    [ "$actual" = "unicorn" ] || {
      switchyard_runtime_manifest_error "cpuProvider.implementation must identify Unicorn"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.version "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_VERSION" ] || {
      switchyard_runtime_manifest_error "cpuProvider.version is not the pinned Unicorn version"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.sourceRepository "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" ] || {
      switchyard_runtime_manifest_error "cpuProvider.sourceRepository is not the pinned repository"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.sourceRevision "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_SOURCE_REVISION" ] || {
      switchyard_runtime_manifest_error "cpuProvider.sourceRevision is not the pinned revision"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.sourceArchive "$manifest")"
    [ "$actual" = "lib/switchyard-unicorn/share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz" ] || {
      switchyard_runtime_manifest_error "cpuProvider.sourceArchive is not runtime-relative"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.sourceArchiveSha256 "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" ] || {
      switchyard_runtime_manifest_error "cpuProvider.sourceArchiveSha256 is not pinned"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.sourcePatch.path "$manifest")"
    [ "$actual" = "lib/switchyard-unicorn/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" ] || {
      switchyard_runtime_manifest_error "cpuProvider.sourcePatch.path is not runtime-relative"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.sourcePatch.sha256 "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] || {
      switchyard_runtime_manifest_error "cpuProvider.sourcePatch.sha256 is not pinned"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.buildContractVersion "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" ] || {
      switchyard_runtime_manifest_error "cpuProvider.buildContractVersion is not pinned"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.hostArchitecture "$manifest")"
    [ "$actual" = "arm64" ] || {
      switchyard_runtime_manifest_error "cpuProvider.hostArchitecture must be arm64"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.kuserSharedDataModel "$manifest")"
    [ "$actual" = "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL" ] &&
      [ "$actual" = "translated-shadow" ] || {
      switchyard_runtime_manifest_error \
        "cpuProvider.kuserSharedDataModel does not match the native profile"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.emulatedArchitectures "$manifest")"
    [ "$actual" = "2" ] || {
      switchyard_runtime_manifest_error "cpuProvider.emulatedArchitectures has an unexpected length"
      return 1
    }
    [ "$(switchyard_runtime_manifest_value cpuProvider.emulatedArchitectures.0 "$manifest")" = "i386" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.emulatedArchitectures.1 "$manifest")" = "x86_64" ] || {
      switchyard_runtime_manifest_error "cpuProvider.emulatedArchitectures is not the exact allowlist"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.developmentCacheDigest "$manifest")"
    [ "$actual" = "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" ] || {
      switchyard_runtime_manifest_error "cpuProvider.developmentCacheDigest is not pinned"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.runtimePayloadDigest "$manifest")"
    if [ "$actual" != "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] &&
       [ -z "$runtime_root" ]; then
      switchyard_runtime_manifest_error \
        "cpuProvider.runtimePayloadDigest requires runtime-bound validation"
      return 1
    fi
    actual="$(switchyard_runtime_manifest_value cpuProvider.runtimeRoot "$manifest")"
    [ "$actual" = "lib/switchyard-unicorn" ] || {
      switchyard_runtime_manifest_error "cpuProvider.runtimeRoot is not runtime-relative"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.library "$manifest")"
    [ "$actual" = "lib/switchyard-unicorn/lib/libunicorn.2.dylib" ] || {
      switchyard_runtime_manifest_error "cpuProvider.library is not runtime-relative"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.librarySha256 "$manifest")"
    if [ "$actual" != "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ] &&
       [ -z "$runtime_root" ]; then
      switchyard_runtime_manifest_error \
        "cpuProvider.librarySha256 requires runtime-bound validation"
      return 1
    fi
    actual="$(switchyard_runtime_manifest_value cpuProvider.providerUnixLibraries "$manifest")"
    [ "$actual" = "2" ] || {
      switchyard_runtime_manifest_error "cpuProvider.providerUnixLibraries has an unexpected length"
      return 1
    }
    [ "$(switchyard_runtime_manifest_value cpuProvider.providerUnixLibraries.0 "$manifest")" = \
        "lib/wine/aarch64-unix/xtajit.so" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.providerUnixLibraries.1 "$manifest")" = \
        "lib/wine/aarch64-unix/xtajit64.so" ] || {
      switchyard_runtime_manifest_error "cpuProvider.providerUnixLibraries is not the exact allowlist"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.components "$manifest")"
    [ "$actual" = "2" ] || {
      switchyard_runtime_manifest_error "cpuProvider.components has an unexpected length"
      return 1
    }
    [ "$(switchyard_runtime_manifest_value cpuProvider.components.0.guestArchitecture "$manifest")" = "i386" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.components.0.unixLibrary "$manifest")" = \
        "lib/wine/aarch64-unix/xtajit.so" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.components.0.peLibrary "$manifest")" = \
        "lib/wine/aarch64-windows/xtajit.dll" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.components.1.guestArchitecture "$manifest")" = "x86_64" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.components.1.unixLibrary "$manifest")" = \
        "lib/wine/aarch64-unix/xtajit64.so" ] &&
      [ "$(switchyard_runtime_manifest_value cpuProvider.components.1.peLibrary "$manifest")" = \
        "lib/wine/aarch64-windows/xtajit64.dll" ] || {
      switchyard_runtime_manifest_error "cpuProvider.components is not the exact provider layout"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.runtimeRpath "$manifest")"
    [ "$actual" = '@loader_path/../../switchyard-unicorn/lib' ] || {
      switchyard_runtime_manifest_error "cpuProvider.runtimeRpath is not runtime-relative"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value cpuProvider.manifest "$manifest")"
    [ "$actual" = "lib/switchyard-unicorn/switchyard-unicorn-runtime.json" ] || {
      switchyard_runtime_manifest_error "cpuProvider.manifest is not runtime-relative"
      return 1
    }
    actual="$(switchyard_runtime_manifest_value runtimeSigning.mode "$manifest")"
    if [ -n "$runtime_root" ] && [ -z "$actual" ]; then
      switchyard_runtime_manifest_error \
        "runtimeSigning is required for runtime-bound native validation"
      return 1
    fi
    if [ -n "$actual" ] ||
       [ "$(switchyard_runtime_manifest_value cpuProvider.runtimePayloadDigest "$manifest")" != \
         "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] ||
       [ "$(switchyard_runtime_manifest_value cpuProvider.librarySha256 "$manifest")" != \
         "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ]; then
      [ -n "$runtime_root" ] || {
        switchyard_runtime_manifest_error \
          "signed native identities require an explicit runtime root"
        return 1
      }
      switchyard_validate_native_runtime_bound_identity \
        "$manifest" "$runtime_root" || return 1
    fi
  fi
}
