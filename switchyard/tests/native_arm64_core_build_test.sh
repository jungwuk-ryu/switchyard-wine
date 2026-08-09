#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && /bin/pwd -P)"
DRIVER="$ROOT_DIR/switchyard/build_native_arm64_core.sh"
TEST_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-native-core-build-test.XXXXXX)"

cleanup() {
  case "$TEST_ROOT" in
    /tmp/switchyard-native-core-build-test.*|/private/tmp/switchyard-native-core-build-test.*)
      rm -rf -- "$TEST_ROOT"
      ;;
    *)
      echo "refusing to remove unexpected test directory $TEST_ROOT" >&2
      ;;
  esac
}
trap cleanup EXIT

fail() {
  echo "native ARM64 core build test: $*" >&2
  exit 1
}

expect_failure() {
  local description=$1
  local expected=$2
  local output
  local status
  shift 2

  set +e
  output="$("$DRIVER" "$@" 2>&1)"
  status=$?
  set -e
  [ "$status" -ne 0 ] || fail "$description unexpectedly succeeded"
  case "$output" in
    *"$expected"*) ;;
    *) fail "$description did not report '$expected': $output" ;;
  esac
}

[ -x "$DRIVER" ] || fail "driver is not executable: $DRIVER"

help_output="$($DRIVER --help)"
case "$help_output" in
  *"never downloads or vendors FEX"*"never installs, packages,"*) ;;
  *) fail "help does not describe the probe's non-production boundary" ;;
esac

expect_failure "missing build directory" "--build-dir is required"
expect_failure "missing build directory value" "--build-dir requires a value" --build-dir
expect_failure "unknown option" "unknown argument" --definitely-unknown
expect_failure "relative build directory" "must be an absolute path" --build-dir relative
expect_failure "missing directory" "existing directory" --build-dir "$TEST_ROOT/missing"
expect_failure "source-tree directory" "outside the Wine source tree" --build-dir "$ROOT_DIR"
expect_failure "filesystem root" "must not be the filesystem root" --build-dir /
expect_failure "invalid job count" "integer from 1 through 64" --build-dir "$TEST_ROOT" --jobs 0
expect_failure "excessive job count" "integer from 1 through 64" --build-dir "$TEST_ROOT" --jobs 65
expect_failure "duplicate job count" "only once" --build-dir "$TEST_ROOT" --jobs 1 --jobs 2
expect_failure "duplicate build directory" "only once" --build-dir "$TEST_ROOT" --build-dir "$TEST_ROOT"
expect_failure "duplicate configure-only flag" "only once" --build-dir "$TEST_ROOT" --configure-only --configure-only

mkdir "$TEST_ROOT/real-directory"
ln -s "$TEST_ROOT/real-directory" "$TEST_ROOT/symlink-directory"
expect_failure "symlink build directory" "must not be a symbolic link" \
  --build-dir "$TEST_ROOT/symlink-directory"

mkdir "$TEST_ROOT/nonempty"
printf 'sentinel\n' >"$TEST_ROOT/nonempty/sentinel"
expect_failure "nonempty build directory" "must be empty" --build-dir "$TEST_ROOT/nonempty"

mkdir "$TEST_ROOT/world-writable"
chmod 0777 "$TEST_ROOT/world-writable"
expect_failure "world-writable build directory" "must not be writable by group or other users" \
  --build-dir "$TEST_ROOT/world-writable"

if [ "$(/usr/bin/uname -s)" != Darwin ] || [ "$(/usr/bin/uname -m)" != arm64 ]; then
  echo "native ARM64 core build safety tests passed; configure smoke skipped on this host"
  exit 0
fi

required_tools=(
  /usr/bin/clang
  /usr/bin/clang++
  /usr/bin/make
  /usr/bin/flex
  /opt/homebrew/opt/bison/bin/bison
  /opt/homebrew/opt/llvm/bin/clang
  /opt/homebrew/opt/llvm/bin/llvm-ar
  /opt/homebrew/opt/llvm/bin/llvm-dlltool
  /opt/homebrew/opt/llvm/bin/llvm-ranlib
  /opt/homebrew/opt/llvm/bin/llvm-rc
  /opt/homebrew/opt/llvm/bin/llvm-strip
  /opt/homebrew/opt/llvm/bin/llvm-readobj
  /opt/homebrew/opt/lld/bin/ld.lld
  /opt/homebrew/opt/lld/bin/lld-link
)
for tool in "${required_tools[@]}"; do
  if [ ! -x "$tool" ]; then
    echo "native ARM64 core build safety tests passed; configure smoke skipped because $tool is unavailable"
    exit 0
  fi
done

configure_build="$TEST_ROOT/configure-smoke"
mkdir "$configure_build"
chmod 0700 "$configure_build"
"$DRIVER" --build-dir "$configure_build" --configure-only

[ -f "$configure_build/Makefile" ] || fail "configure smoke did not create Makefile"
[ -f "$configure_build/config.status" ] || fail "configure smoke did not create config.status"

config_command="$(cd "$configure_build" && ./config.status --config)"
for option in \
  --build=aarch64-apple-darwin \
  --host=aarch64-apple-darwin \
  --enable-archs=aarch64,arm64ec,x86_64,i386 \
  --with-mingw=/opt/homebrew/opt/llvm/bin/clang \
  --disable-tests \
  --without-x \
  --without-freetype \
  --without-gnutls \
  --without-gstreamer
do
  case " $config_command " in
    *" $option "*) ;;
    *) fail "configure smoke omitted $option" ;;
  esac
done

core_targets=(
  loader/wine
  server/wineserver
  dlls/ntdll/ntdll.so
  dlls/ntdll/aarch64-windows/ntdll.dll
  programs/cmd/aarch64-windows/cmd.exe
  dlls/wow64/aarch64-windows/wow64.dll
  dlls/xtajit64/aarch64-windows/xtajit64.dll
)

(
  cd "$configure_build"
  PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:/opt/homebrew/opt/bison/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
    /usr/bin/make -n "${core_targets[@]}" >"$TEST_ROOT/make-target-smoke.log"
)

expect_failure "configured directory reuse" "must be empty" \
  --build-dir "$configure_build" --configure-only

echo "native ARM64 core build safety and configure/target smoke tests passed"
