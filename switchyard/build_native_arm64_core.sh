#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && /bin/pwd -P)"
CONFIGURE_SCRIPT="$ROOT_DIR/configure"

HOST_CC=/usr/bin/clang
HOST_CXX=/usr/bin/clang++
MAKE=/usr/bin/make
FLEX=/usr/bin/flex
BISON=/opt/homebrew/opt/bison/bin/bison
LLVM_BIN=/opt/homebrew/opt/llvm/bin
LLD_BIN=/opt/homebrew/opt/lld/bin
MINGW_CLANG="$LLVM_BIN/clang"
LLVM_READOBJ="$LLVM_BIN/llvm-readobj"
TOOL_PATH="$LLVM_BIN:$LLD_BIN:/opt/homebrew/opt/bison/bin:/usr/bin:/bin:/usr/sbin:/sbin"

usage() {
  cat <<'EOF'
Usage: switchyard/build_native_arm64_core.sh --build-dir DIR [options]

Configure and build the isolated native ARM64 Wine core probe. DIR must be an
existing, empty, user-owned directory outside the Wine source tree.

Options:
  --build-dir DIR    Empty out-of-tree build directory (required).
  --configure-only   Configure the four-architecture tree without building it.
  --jobs N           Parallel build jobs, from 1 through 64 (default: 4).
  -h, --help         Show this help.

This probe never downloads or vendors FEX and never installs, packages,
promotes, signs, or activates a Wine runtime.
EOF
}

fail() {
  echo "native ARM64 core probe: $*" >&2
  exit 1
}

require_executable() {
  local path=$1
  local description=$2

  [ -x "$path" ] || fail "$description is required at $path"
}

build_dir=
configure_only=false
jobs=4
jobs_set=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir)
      [ "$#" -ge 2 ] || fail "--build-dir requires a value"
      [ -z "$build_dir" ] || fail "--build-dir may be specified only once"
      build_dir=$2
      shift 2
      ;;
    --configure-only)
      [ "$configure_only" = false ] || fail "--configure-only may be specified only once"
      configure_only=true
      shift
      ;;
    --jobs)
      [ "$#" -ge 2 ] || fail "--jobs requires a value"
      [ "$jobs_set" = false ] || fail "--jobs may be specified only once"
      jobs=$2
      jobs_set=true
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

[ -n "$build_dir" ] || fail "--build-dir is required"
case "$jobs" in
  ''|*[!0-9]*|0) fail "--jobs must be an integer from 1 through 64" ;;
esac
if [ "$jobs" -gt 64 ]; then
  fail "--jobs must be an integer from 1 through 64"
fi
case "$build_dir" in
  /*) ;;
  *) fail "--build-dir must be an absolute path" ;;
esac
case "$build_dir" in
  *$'\n'*|*$'\r'*) fail "--build-dir must not contain newline characters" ;;
esac
[ ! -L "$build_dir" ] || fail "--build-dir must not be a symbolic link"
[ -d "$build_dir" ] || fail "--build-dir must name an existing directory"

build_dir="$(cd "$build_dir" && /bin/pwd -P)" ||
  fail "cannot resolve --build-dir"
[ "$build_dir" != / ] || fail "--build-dir must not be the filesystem root"
case "$build_dir/" in
  "$ROOT_DIR"/|"$ROOT_DIR"/*)
    fail "--build-dir must be outside the Wine source tree"
    ;;
esac

[ -r "$build_dir" ] && [ -w "$build_dir" ] && [ -x "$build_dir" ] ||
  fail "--build-dir must be readable, writable, and searchable"

build_owner="$(/usr/bin/stat -f '%u' "$build_dir")" ||
  fail "cannot inspect --build-dir ownership"
[ "$build_owner" = "$(/usr/bin/id -u)" ] ||
  fail "--build-dir must be owned by the current user"

build_mode="$(/usr/bin/stat -f '%Lp' "$build_dir")" ||
  fail "cannot inspect --build-dir permissions"
case "$build_mode" in
  ''|*[!0-7]*) fail "cannot validate --build-dir permissions" ;;
esac
if (( (8#$build_mode & 8#022) != 0 )); then
  fail "--build-dir must not be writable by group or other users"
fi

first_build_entry="$(/usr/bin/find "$build_dir" -mindepth 1 -maxdepth 1 -print -quit)" ||
  fail "cannot inspect --build-dir contents"
if [ -n "$first_build_entry" ]; then
  fail "--build-dir must be empty"
fi

[ "$(/usr/bin/uname -s)" = Darwin ] ||
  fail "this probe requires Darwin"
[ "$(/usr/bin/uname -m)" = arm64 ] ||
  fail "this probe requires a native arm64 process"
if translated="$(/usr/sbin/sysctl -in sysctl.proc_translated 2>/dev/null)"; then
  [ "$translated" != 1 ] || fail "this probe must not run through Rosetta"
fi

[ -x "$CONFIGURE_SCRIPT" ] || fail "Wine configure script is missing at $CONFIGURE_SCRIPT"
require_executable "$HOST_CC" "Apple clang"
require_executable "$HOST_CXX" "Apple clang++"
require_executable "$MAKE" "GNU make"
require_executable "$FLEX" "flex"
require_executable "$BISON" "Bison 3 or newer"
require_executable "$MINGW_CLANG" "Homebrew LLVM clang"
require_executable "$LLVM_BIN/llvm-ar" "Homebrew llvm-ar"
require_executable "$LLVM_BIN/llvm-dlltool" "Homebrew llvm-dlltool"
require_executable "$LLVM_BIN/llvm-ranlib" "Homebrew llvm-ranlib"
require_executable "$LLVM_BIN/llvm-rc" "Homebrew llvm-rc"
require_executable "$LLVM_BIN/llvm-strip" "Homebrew llvm-strip"
require_executable "$LLVM_READOBJ" "Homebrew llvm-readobj"
require_executable "$LLD_BIN/ld.lld" "Homebrew ELF/MinGW LLD linker"
require_executable "$LLD_BIN/lld-link" "Homebrew LLD linker"

bison_version="$($BISON --version)" || fail "cannot query Bison at $BISON"
if ! /usr/bin/awk 'NR == 1 { split($NF, v, "."); exit(v[1] >= 3 ? 0 : 1) }' <<<"$bison_version"; then
  fail "Bison 3 or newer is required at $BISON"
fi

sdk_path="$(/usr/bin/xcrun --sdk macosx --show-sdk-path 2>/dev/null)" ||
  fail "a selected Xcode macOS SDK is required"
[ -d "$sdk_path" ] || fail "Xcode reported a missing macOS SDK at $sdk_path"

source_date_epoch="$(/usr/bin/git -C "$ROOT_DIR" log -1 --format=%ct HEAD 2>/dev/null)" ||
  fail "cannot determine the source revision timestamp"
case "$source_date_epoch" in
  ''|*[!0-9]*) fail "source revision timestamp is invalid" ;;
esac

configure_options=(
  "--build=aarch64-apple-darwin"
  "--host=aarch64-apple-darwin"
  "--enable-archs=aarch64,arm64ec,x86_64,i386"
  --with-mingw="$MINGW_CLANG"
  --disable-tests
  --without-x
  --without-freetype
  --without-gnutls
  --without-gstreamer
)

core_targets=(
  loader/wine
  server/wineserver
  dlls/ntdll/ntdll.so
  dlls/ntdll/aarch64-windows/ntdll.dll
  programs/cmd/aarch64-windows/cmd.exe
  dlls/wow64/aarch64-windows/wow64.dll
  dlls/xtajit64/aarch64-windows/xtajit64.dll
)

echo "Configuring native ARM64 Wine core probe in $build_dir"
echo "Source: $ROOT_DIR"
echo "PE architectures: aarch64,arm64ec,x86_64,i386"
echo "SDK: $sdk_path"

(
  cd "$build_dir"
  /usr/bin/env -i \
    PATH="$TOOL_PATH" \
    LC_ALL=C \
    LANG=C \
    TZ=UTC \
    CONFIG_SHELL=/bin/sh \
    SDKROOT="$sdk_path" \
    SOURCE_DATE_EPOCH="$source_date_epoch" \
    ZERO_AR_DATE=1 \
    CC="$HOST_CC" \
    CXX="$HOST_CXX" \
    CFLAGS='-g -O2' \
    CXXFLAGS='-g -O2' \
    CROSSCFLAGS='-g -O2' \
    BISON="$BISON" \
    FLEX="$FLEX" \
    /bin/sh "$CONFIGURE_SCRIPT" "${configure_options[@]}"
)

if [ "$configure_only" = true ]; then
  echo "Configure-only probe complete; no build, install, packaging, or activation was performed."
  exit 0
fi

echo "Building minimal native ARM64 core targets with $jobs jobs"
(
  cd "$build_dir"
  /usr/bin/env -i \
    PATH="$TOOL_PATH" \
    LC_ALL=C \
    LANG=C \
    TZ=UTC \
    SDKROOT="$sdk_path" \
    SOURCE_DATE_EPOCH="$source_date_epoch" \
    ZERO_AR_DATE=1 \
    CONFIG_SHELL=/bin/sh \
    CC="$HOST_CC" \
    CXX="$HOST_CXX" \
    CFLAGS='-g -O2' \
    CXXFLAGS='-g -O2' \
    CROSSCFLAGS='-g -O2' \
    BISON="$BISON" \
    FLEX="$FLEX" \
    "$MAKE" -j "$jobs" "${core_targets[@]}"
)

for target in "${core_targets[@]}"; do
  [ -f "$build_dir/$target" ] || fail "expected build output is missing: $target"
done

check_format() {
  local relative_path=$1
  local expected_format=$2
  local headers

  headers="$($LLVM_READOBJ --file-headers "$build_dir/$relative_path")" ||
    fail "cannot inspect build output: $relative_path"
  /usr/bin/grep -Fqx "Format: $expected_format" <<<"$headers" ||
    fail "$relative_path does not have expected format $expected_format"
}

check_format loader/wine 'Mach-O arm64'
check_format server/wineserver 'Mach-O arm64'
check_format dlls/ntdll/ntdll.so 'Mach-O arm64'
check_format dlls/ntdll/aarch64-windows/ntdll.dll 'COFF-ARM64X'
check_format programs/cmd/aarch64-windows/cmd.exe 'COFF-ARM64X'
check_format dlls/wow64/aarch64-windows/wow64.dll 'COFF-ARM64'
check_format dlls/xtajit64/aarch64-windows/xtajit64.dll 'COFF-ARM64EC'

echo "Native ARM64 core build probe complete in $build_dir"
echo "No FEX source was downloaded or vendored, and no runtime was installed, promoted, or activated."
