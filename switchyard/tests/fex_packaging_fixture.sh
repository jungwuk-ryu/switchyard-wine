#!/usr/bin/env bash
# Shared native fixtures. Callers own one private mktemp runtime and cleanup.

switchyard_fixture_stage_fex_sdk() {
  [ "$#" -eq 1 ] || return 2
  local runtime="$1"
  FEX_CACHE="${SWITCHYARD_FEX_FIXTURE_CACHE:-${HOME}/.switchyard/deps/cpu-provider/fex-${SWITCHYARD_FEX_SOURCE_REVISION:0:12}-build${SWITCHYARD_FEX_BUILD_CONTRACT_VERSION}-ec-arm64-macos-${SWITCHYARD_FEX_MINIMUM_MACOS}}"
  FEX_PACKAGE="$runtime/$SWITCHYARD_NATIVE_FEX_ROOT"
  switchyard_validate_fex_development_sdk "$FEX_CACHE" || return 1
  [ ! -e "$FEX_PACKAGE" ] && [ ! -L "$FEX_PACKAGE" ] || return 1
  /usr/bin/ditto --noextattr --noqtn "$FEX_CACHE" "$FEX_PACKAGE" || return 1
  switchyard_validate_fex_development_sdk "$FEX_PACKAGE" || return 1
  FIXTURE_PAYLOAD_DIGEST="$SWITCHYARD_FEX_DEVELOPMENT_CACHE_DIGEST"
}

switchyard_fixture_build_fex_provider() {
  [ "$#" -ge 2 ] && [ "$#" -le 3 ] || return 2
  local runtime="$1" output="$2" omitted="${3:-}" test_dir
  local options=()
  case "$omitted" in
    '') ;;
    omit-api) options=(-DSWITCHYARD_FIXTURE_OMIT_API) ;;
    *) return 2 ;;
  esac
  test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
  /usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/xtajit64.so \
    -Wl,-rpath,@loader_path/ -Wl,-rpath,"$SWITCHYARD_NATIVE_FEX_RPATH" \
    -I"$runtime/$SWITCHYARD_NATIVE_FEX_ROOT/include" \
    "-DSWITCHYARD_FIXTURE_ABI_IDENTITY=\"$SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY\"" \
    ${options[@]+"${options[@]}"} "$test_dir/fex_provider_fixture.c" \
    "$runtime/lib/wine/aarch64-unix/ntdll.so" \
    "$runtime/$SWITCHYARD_NATIVE_FEX_LIBRARY" -o "$output"
}
