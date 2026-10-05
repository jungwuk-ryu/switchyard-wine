#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && /bin/pwd -P)"
source "$ROOT_DIR/switchyard/lib/fex_contract.sh"
FEX_REPOSITORY="$SWITCHYARD_FEX_SOURCE_REPOSITORY"
FEX_REVISION="$SWITCHYARD_FEX_SOURCE_REVISION"
FEX_PATCH="$ROOT_DIR/switchyard/patches/$SWITCHYARD_FEX_SOURCE_PATCH_BASENAME"
FEX_PATCH_SHA256="$SWITCHYARD_FEX_SOURCE_PATCH_SHA256"
FEX_SOURCE_DEPS="$ROOT_DIR/switchyard/fex/source-deps.tsv"
FEX_SOURCE_DEPS_SHA256="$SWITCHYARD_FEX_SOURCE_DEPS_SHA256"
FEX_BUILD_CONTRACT_VERSION="$SWITCHYARD_FEX_BUILD_CONTRACT_VERSION"
FEX_ADAPTER_SHA256="$SWITCHYARD_FEX_ADAPTER_SHA256"
FEX_LIBRARY_SHA256="$SWITCHYARD_FEX_LIBRARY_SHA256"
FEX_RUNTIME_DIGEST="$SWITCHYARD_FEX_DEVELOPMENT_CACHE_DIGEST"
FEX_TOOLCHAIN_SHA256="$SWITCHYARD_FEX_TOOLCHAIN_SHA256"
FEX_PROVIDER_IDENTITY="$SWITCHYARD_FEX_PROVIDER_IDENTITY"
MINIMUM_MACOS="$SWITCHYARD_FEX_MINIMUM_MACOS"
CACHE_ROOT="${SWITCHYARD_FEX_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/FEX}"
SOURCE_DIR="${SWITCHYARD_FEX_SOURCE_DIR:-}"
BUILD_ROOT="${SWITCHYARD_FEX_BUILD_DIR:-}"
TEST_REPETITIONS="${SWITCHYARD_FEX_TEST_REPETITIONS:-25}"
JOBS="${JOBS:-4}"
OUTPUT=""
STAGING=""
PATCHED_SOURCE=""
ADAPTER_SOURCE=""
BUILD_DIR=""

# Keep the build and distributed corresponding source identical. Do not copy a
# tests directory with an unchecked glob: new dependencies must enter this list
# and the pinned adapter digest before they can affect a published SDK.
ADAPTER_FILES=(
  CMakeLists.txt README.md exports.txt source-deps.tsv
  include/switchyard_fex.h include/switchyard_fex_admission.h
  src/switchyard_fex.cpp
  tests/admission_contract.h tests/admission_test.h tests/allocator_test.cpp
  tests/c_api_test.c tests/darwin_abi_test.cpp tests/detached_dispatch_test.h
  tests/extract_wine_x18.cmake tests/shared_admission_test.h
  tests/register_window_test.c tests/register_window_state_test.cpp
  tests/register_window_reference.h
  tests/vector_state_test.h tests/verify_test_suite.py
  tests/wine_signal_wrapper.c tests/wine_signal_wrapper.h
)
WINE_TEST_FILES=(
  COPYING.LIB LICENSE dlls/ntdll/unix/signal_arm64.c include/wine/asm.h
)

SUBMODULE_PATHS=(
  "External/fmt"
  "External/range-v3"
  "External/unordered_dense"
  "External/xxhash"
)
SUBMODULE_REPOSITORIES=(
  "https://github.com/fmtlib/fmt.git"
  "https://github.com/ericniebler/range-v3.git"
  "https://github.com/martinus/unordered_dense.git"
  "https://github.com/Cyan4973/xxHash.git"
)
SUBMODULE_REVISIONS=(
  "1be298e1bd68957e4cd352e1f676f00e07dcfb57"
  "ca1388fb9da8e69314dda222dc7b139ca84e092f"
  "3234af2c03549bc85656bfd3a86993bf1cd8aef1"
  "e626a72bc2321cd320e953a0ccf1584cad60f363"
)

usage() {
  echo "usage: $0 --output PATH [--source-dir PATH] [--build-dir PATH] [--minimum-macos 26.5]" >&2
  exit 2
}

fail() {
  echo "Switchyard FEX runtime: $*" >&2
  exit 1
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk 'NR == 1 { print $1; exit }'
}

resolve_output() {
  local path="$1"
  local parent name

  case "$path" in /*) ;; *) fail "output must be an absolute path" ;; esac
  parent="$(dirname "$path")"
  name="$(basename "$path")"
  case "$name" in ''|.|..) fail "output name is not bounded" ;; esac
  case "$path" in *$'\n'*|*$'\r'*) fail "output contains a control character" ;; esac
  mkdir -p "$parent"
  [ -d "$parent" ] && [ ! -L "$parent" ] || fail "unsafe output parent: $parent"
  parent="$(cd "$parent" && /bin/pwd -P)" || fail "cannot resolve output parent"
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
            target = os.path.realpath(path)
            if os.path.commonpath((root, target)) != root:
                raise SystemExit("tree link escapes its root: " + path)
        elif not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
            raise SystemExit("unsupported tree entry: " + path)
PY
}

copy_adapter_file() {
  local source="$1"
  local destination="$2"

  [ -f "$source" ] && [ ! -L "$source" ] || fail "unsafe adapter input: $source"
  install -m 0644 "$source" "$destination"
}

source_is_clean() {
  [ -z "$(git -C "$1" status --porcelain --untracked-files=all)" ] &&
    git -C "$1" diff --quiet &&
    git -C "$1" diff --cached --quiet
}

tree_digest() {
  /usr/bin/python3 -I - "$1" <<'PY'
import hashlib
import os
import stat
import sys

root = os.path.realpath(sys.argv[1])
digest = hashlib.sha256()
for directory, directories, files in os.walk(root, followlinks=False):
    directories.sort()
    files.sort()
    for name in directories + files:
        path = os.path.join(directory, name)
        info = os.lstat(path)
        relative = os.path.relpath(path, root).encode("utf-8")
        digest.update(relative + b"\0")
        digest.update(('%o' % stat.S_IMODE(info.st_mode)).encode("ascii") + b"\0")
        if stat.S_ISLNK(info.st_mode):
            digest.update(b"L" + os.readlink(path).encode("utf-8") + b"\0")
        elif stat.S_ISREG(info.st_mode):
            digest.update(b"F")
            with open(path, "rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
        elif stat.S_ISDIR(info.st_mode):
            digest.update(b"D")
        else:
            raise SystemExit("unsupported adapter source entry: " + path)
        digest.update(b"\0")
print(digest.hexdigest())
PY
}

toolchain_digest() {
  local sdk="$1"
  local compiler

  compiler="$(xcrun --find clang)" || return 1
  [ -f "$compiler" ] && [ -f "$sdk/SDKSettings.json" ] || return 1
  {
    /usr/bin/clang --version
    /usr/bin/clang++ --version
    sha256_file "$compiler"
    sha256_file "$sdk/SDKSettings.json"
    cmake --version
  } | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
}

validate_output() {
  [ "$MINIMUM_MACOS" = "$SWITCHYARD_FEX_MINIMUM_MACOS" ] || return 1
  switchyard_validate_fex_development_sdk "$1"
}

cleanup() {
  local directory

  for directory in "$BUILD_DIR" "$ADAPTER_SOURCE" "$PATCHED_SOURCE" "$STAGING"; do
    if [ -n "$directory" ] && [ -d "$directory" ] && [ ! -L "$directory" ]; then
      /bin/rm -rf "$directory"
    fi
  done
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
      BUILD_ROOT="$2"
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
[ "$MINIMUM_MACOS" = "26.5" ] || fail "the current contract requires macOS 26.5"
case "$JOBS" in ''|*[!0-9]*|0) fail "JOBS must be a positive integer" ;; esac
case "$TEST_REPETITIONS" in ''|*[!0-9]*|0) fail "test repetitions must be positive" ;; esac
[ "$JOBS" -le 64 ] || fail "JOBS must not exceed 64"
[ "$TEST_REPETITIONS" -le 100 ] || fail "test repetitions must not exceed 100"

for command in clang clang++ cmake codesign git gzip lipo nm otool python3 shasum tar vtool xcrun; do
  command -v "$command" >/dev/null 2>&1 || fail "missing required command: $command"
done
[ -f "$FEX_PATCH" ] && [ ! -L "$FEX_PATCH" ] || fail "missing FEX patch"
[ "$(sha256_file "$FEX_PATCH")" = "$FEX_PATCH_SHA256" ] || fail "FEX patch SHA-256 mismatch"
[ -f "$FEX_SOURCE_DEPS" ] && [ ! -L "$FEX_SOURCE_DEPS" ] || fail "missing source dependency manifest"
[ "$(sha256_file "$FEX_SOURCE_DEPS")" = "$FEX_SOURCE_DEPS_SHA256" ] || fail "source dependency manifest SHA-256 mismatch"

OUTPUT="$(resolve_output "$OUTPUT")"
if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
  validate_output "$OUTPUT" || fail "existing output is incomplete or inconsistent: $OUTPUT"
  echo "$OUTPUT"
  exit 0
fi

mkdir -p "$CACHE_ROOT"
[ -d "$CACHE_ROOT" ] && [ ! -L "$CACHE_ROOT" ] || fail "unsafe cache root"
CACHE_ROOT="$(cd "$CACHE_ROOT" && /bin/pwd -P)"
case "$CACHE_ROOT" in /|"$HOME") fail "cache root is too broad" ;; esac
if [ -z "$SOURCE_DIR" ]; then
  SOURCE_DIR="$CACHE_ROOT/source-$FEX_REVISION"
fi
if [ -z "$BUILD_ROOT" ]; then
  BUILD_ROOT="$CACHE_ROOT/build-$FEX_REVISION-contract$FEX_BUILD_CONTRACT_VERSION"
fi
case "$SOURCE_DIR" in /*) ;; *) fail "source directory must be absolute" ;; esac
case "$BUILD_ROOT" in /*) ;; *) fail "build directory must be absolute" ;; esac
[ ! -L "$SOURCE_DIR" ] || fail "source directory must not be a symbolic link"
[ ! -L "$BUILD_ROOT" ] || fail "build directory must not be a symbolic link"

if [ ! -e "$SOURCE_DIR" ]; then
  git clone --filter=blob:none --no-checkout "$FEX_REPOSITORY" "$SOURCE_DIR"
  git -C "$SOURCE_DIR" fetch --depth 1 origin "$FEX_REVISION"
  git -C "$SOURCE_DIR" checkout --detach "$FEX_REVISION"
fi
git -C "$SOURCE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "source is not a Git checkout"
SOURCE_DIR="$(cd "$SOURCE_DIR" && /bin/pwd -P)"
[ "$(git -C "$SOURCE_DIR" rev-parse HEAD)" = "$FEX_REVISION" ] || fail "source is not the pinned FEX revision"
[ "$(git -C "$SOURCE_DIR" config --get remote.origin.url)" = "$FEX_REPOSITORY" ] || fail "source origin does not match the source contract"
source_is_clean "$SOURCE_DIR" || fail "source checkout is dirty before submodule setup"

if [ "$(git -C "$SOURCE_DIR" config --bool core.sparseCheckout 2>/dev/null || true)" = "true" ]; then
  git -C "$SOURCE_DIR" sparse-checkout add External
fi
for index in 0 1 2 3; do
  path="${SUBMODULE_PATHS[$index]}"
  repository="${SUBMODULE_REPOSITORIES[$index]}"
  revision="${SUBMODULE_REVISIONS[$index]}"
  recorded_url="$(git -C "$SOURCE_DIR" config -f .gitmodules --get "submodule.$path.url")" ||
    fail "missing submodule URL for $path"
  [ "$recorded_url" = "$repository" ] || fail "unexpected submodule URL for $path"
  recorded_revision="$(git -C "$SOURCE_DIR" ls-tree HEAD "$path" | /usr/bin/awk '{print $3}')" ||
    fail "missing submodule revision for $path"
  [ "$recorded_revision" = "$revision" ] || fail "unexpected submodule revision for $path"
  git -C "$SOURCE_DIR" submodule update --init --depth 1 -- "$path"
  [ "$(git -C "$SOURCE_DIR/$path" rev-parse HEAD)" = "$revision" ] || fail "submodule checkout mismatch for $path"
  [ "$(git -C "$SOURCE_DIR/$path" config --get remote.origin.url)" = "$repository" ] || fail "submodule origin mismatch for $path"
done
source_is_clean "$SOURCE_DIR" || fail "source checkout is dirty after submodule setup"

sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
sdk_version="$(xcrun --sdk macosx --show-sdk-version)"
[ "$sdk_version" = "$MINIMUM_MACOS" ] || fail "macOS SDK $MINIMUM_MACOS is required, not $sdk_version"
[ -d "$sdk_path" ] || fail "macOS SDK is missing"
[ "$(toolchain_digest "$sdk_path")" = "$FEX_TOOLCHAIN_SHA256" ] || fail "FEX compiler/SDK/CMake identity mismatch"

mkdir -p "$BUILD_ROOT"
[ -d "$BUILD_ROOT" ] && [ ! -L "$BUILD_ROOT" ] || fail "unsafe build root"
BUILD_ROOT="$(cd "$BUILD_ROOT" && /bin/pwd -P)"
case "$BUILD_ROOT" in /|"$HOME"|"$SOURCE_DIR"|"$SOURCE_DIR"/*) fail "unsafe build root" ;; esac
case "$OUTPUT" in "$SOURCE_DIR"|"$SOURCE_DIR"/*|"$BUILD_ROOT"|"$BUILD_ROOT"/*) fail "output overlaps source or build state" ;; esac

umask 022
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
STAGING="$(mktemp -d "$(dirname "$OUTPUT")/.fex-staging.XXXXXX")"
PATCHED_SOURCE="$(mktemp -d "$BUILD_ROOT/.source.XXXXXX")"
ADAPTER_SOURCE="$(mktemp -d "$BUILD_ROOT/.adapter.XXXXXX")"
BUILD_DIR="$(mktemp -d "$BUILD_ROOT/.build.XXXXXX")"

git -C "$SOURCE_DIR" archive --format=tar HEAD | /usr/bin/tar -xf - -C "$PATCHED_SOURCE"
for index in 0 1 2 3; do
  path="${SUBMODULE_PATHS[$index]}"
  mkdir -p "$PATCHED_SOURCE/$path"
  git -C "$SOURCE_DIR/$path" archive --format=tar HEAD | /usr/bin/tar -xf - -C "$PATCHED_SOURCE/$path"
done
validate_tree_links "$PATCHED_SOURCE" || fail "exported FEX source contains an unsafe link"

patch_ceiling="$(dirname "$PATCHED_SOURCE")"
GIT_CEILING_DIRECTORIES="$patch_ceiling" git -C "$PATCHED_SOURCE" apply \
  --check --unidiff-zero --whitespace=error-all "$FEX_PATCH"
GIT_CEILING_DIRECTORIES="$patch_ceiling" git -C "$PATCHED_SOURCE" apply \
  --unidiff-zero --whitespace=error-all "$FEX_PATCH"
GIT_CEILING_DIRECTORIES="$patch_ceiling" git -C "$PATCHED_SOURCE" apply \
  --reverse --check --unidiff-zero --whitespace=error-all "$FEX_PATCH"
validate_tree_links "$PATCHED_SOURCE" || fail "patched FEX source contains an unsafe link"

for file in "${ADAPTER_FILES[@]}"; do
  mkdir -p "$ADAPTER_SOURCE/$(dirname "$file")"
  copy_adapter_file "$ROOT_DIR/switchyard/fex/$file" "$ADAPTER_SOURCE/$file"
done
for file in "${WINE_TEST_FILES[@]}"; do
  mkdir -p "$ADAPTER_SOURCE/wine/$(dirname "$file")"
  copy_adapter_file "$ROOT_DIR/$file" "$ADAPTER_SOURCE/wine/$file"
done
copy_adapter_file "$ROOT_DIR/switchyard/wine-runtime-native-arm64.entitlements" \
  "$ADAPTER_SOURCE/wine-runtime-native-arm64.entitlements"
validate_tree_links "$ADAPTER_SOURCE" || fail "adapter snapshot contains an unsafe link"
adapter_digest="$(tree_digest "$ADAPTER_SOURCE")"
[ "$adapter_digest" = "$FEX_ADAPTER_SHA256" ] ||
  fail "Switchyard FEX adapter SHA-256 $adapter_digest does not match $FEX_ADAPTER_SHA256"

source_date_epoch="$(git -C "$SOURCE_DIR" show -s --format=%ct HEAD)"
export LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH="$source_date_epoch" ZERO_AR_DATE=1
export MACOSX_DEPLOYMENT_TARGET="$MINIMUM_MACOS"
unset CC CFLAGS CPPFLAGS CXXFLAGS LDFLAGS SDKROOT

reproducible_root="/usr/src/FEX-$FEX_REVISION"
common_flags="-ffile-prefix-map=$PATCHED_SOURCE=$reproducible_root"
common_flags+=" -ffile-prefix-map=$ADAPTER_SOURCE=$reproducible_root/SwitchyardFEXAdapter"
common_flags+=" -ffile-prefix-map=$BUILD_DIR=$reproducible_root/.build"
cmake -S "$PATCHED_SOURCE" -B "$BUILD_DIR" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_C_FLAGS="$common_flags" \
  -DCMAKE_CXX_FLAGS="$common_flags" \
  "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG" \
  "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINIMUM_MACOS" \
  -DCMAKE_OSX_SYSROOT="$sdk_path" \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DOVERRIDE_HASH="$FEX_REVISION" \
  -DSWITCHYARD_DARWIN_CORE_ONLY=ON \
  -DSWITCHYARD_FEX_ADAPTER_SOURCE_DIR="$ADAPTER_SOURCE" \
  -DSWITCHYARD_FEX_ENTITLEMENTS="$ADAPTER_SOURCE/wine-runtime-native-arm64.entitlements" \
  -DSWITCHYARD_FEX_WINE_SOURCE_DIR="$ADAPTER_SOURCE/wine" \
  -DSWITCHYARD_FEX_ARM64EC_REGISTER_ABI=ON \
  -DSWITCHYARD_FEX_BUILD_TESTS=ON \
  -DBUILD_TESTING=OFF \
  -DBUILD_FEXCONFIG=OFF \
  -DBUILD_FEX_LINUX_TESTS=OFF \
  -DBUILD_STEAM_SUPPORT=OFF \
  -DBUILD_THUNKS=OFF \
  -DENABLE_CCACHE=OFF \
  -DENABLE_FEX_ALLOCATOR=OFF \
  -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
  -DENABLE_LTO=OFF \
  -DTUNE_ARCH=generic \
  -DTUNE_CPU=none
cmake --build "$BUILD_DIR" --parallel "$JOBS" --target switchyard-fex-c-api-test \
  switchyard-fex-darwin-abi-test switchyard-fex-allocator-test switchyard-fex-allocator-fallback-test \
  switchyard-fex-register-window-test switchyard-fex-register-window-state-test
ctest --test-dir "$BUILD_DIR/SwitchyardFEXAdapter" --show-only=json-v1 > "$BUILD_DIR/test-plan.json"
/usr/bin/python3 -I "$ADAPTER_SOURCE/tests/verify_test_suite.py" plan "$BUILD_DIR/test-plan.json"
ctest --test-dir "$BUILD_DIR/SwitchyardFEXAdapter" --output-on-failure --timeout 30 \
  --output-junit "$BUILD_DIR/test-results.xml"
/usr/bin/python3 -I "$ADAPTER_SOURCE/tests/verify_test_suite.py" results "$BUILD_DIR/test-results.xml"

test_binary="$BUILD_DIR/Bin/switchyard-fex-c-api-test"
expected_output="switchyard_fex_c_api_test result=pass concurrency=8/8 revision=$FEX_REVISION"
for ((iteration = 1; iteration <= TEST_REPETITIONS; ++iteration)); do
  actual_output="$("$test_binary")"
  [ "$actual_output" = "$expected_output" ] || fail "C ABI test output mismatch at repetition $iteration"
done
echo "Switchyard FEX C-ABI gate: $TEST_REPETITIONS exact repetitions passed"

for ((iteration = 1; iteration <= TEST_REPETITIONS; ++iteration)); do
  actual_output="$("$BUILD_DIR/Bin/switchyard-fex-register-window-test")"
  [ "$actual_output" = "register_window_c result=pass geometry=10 alignment=16 concurrency=80000" ] ||
    fail "register-window C output mismatch at repetition $iteration"
  actual_output="$("$BUILD_DIR/Bin/switchyard-fex-register-window-state-test")"
  [ "$actual_output" = "register_window_state result=pass cases=264 core_bytes=1472 data_bytes=408" ] ||
    fail "register-window state output mismatch at repetition $iteration"
done
echo "Switchyard FEX register-window gate: $TEST_REPETITIONS exact repetitions passed"

cmake --install "$BUILD_DIR" --prefix "$STAGING" --component SwitchyardFEXRuntime
dylib="$STAGING/lib/libswitchyard-fex.6.0.0.dylib"
[ -f "$dylib" ] && [ ! -L "$dylib" ] || fail "install did not produce the versioned dylib"
[ "$(lipo -archs "$dylib")" = "arm64" ] || fail "FEX runtime is not thin arm64"
[ "$(otool -D "$dylib" | /usr/bin/tail -n 1)" = "@rpath/libswitchyard-fex.6.dylib" ] || fail "unexpected dylib install name"
while IFS= read -r dependency; do
  case "$dependency" in
    @rpath/libswitchyard-fex.6.dylib|/usr/lib/*|/System/Library/*) ;;
    *) fail "unexpected FEX dylib dependency: $dependency" ;;
  esac
done < <(otool -L "$dylib" | /usr/bin/awk 'NR > 1 { print $1 }')
[ -z "$(otool -l "$dylib" | /usr/bin/awk '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')" ] || fail "FEX dylib contains an unexpected rpath"
diff -u <(/usr/bin/sort "$ADAPTER_SOURCE/exports.txt") <(nm -gjU "$dylib" | /usr/bin/sort) || fail "public symbol set mismatch"
codesign --verify --strict "$dylib" || fail "FEX dylib signature verification failed"
build_info="$(vtool -show-build "$dylib")"
/usr/bin/grep -F 'platform MACOS' <<<"$build_info" >/dev/null || fail "FEX dylib platform mismatch"
/usr/bin/grep -E "^[[:space:]]*minos ${MINIMUM_MACOS//./\\.}([.]0)*$" <<<"$build_info" >/dev/null || fail "FEX dylib deployment target mismatch"
/usr/bin/grep -E "^[[:space:]]*sdk ${MINIMUM_MACOS//./\\.}([.]0)*$" <<<"$build_info" >/dev/null || fail "FEX dylib SDK mismatch"

mkdir -p "$STAGING/share/doc/switchyard-fex" "$STAGING/share/src/switchyard-fex/adapter"
install -m 0644 "$PATCHED_SOURCE/LICENSE" "$STAGING/share/doc/switchyard-fex/FEX-LICENSE"
install -m 0644 "$PATCHED_SOURCE/FEXCore/LICENSE" "$STAGING/share/doc/switchyard-fex/FEXCORE-LICENSE"
install -m 0644 "$PATCHED_SOURCE/External/fmt/LICENSE" "$STAGING/share/doc/switchyard-fex/FMT-LICENSE"
install -m 0644 "$PATCHED_SOURCE/External/range-v3/LICENSE.txt" "$STAGING/share/doc/switchyard-fex/RANGE-V3-LICENSE"
install -m 0644 "$PATCHED_SOURCE/External/unordered_dense/LICENSE" "$STAGING/share/doc/switchyard-fex/UNORDERED-DENSE-LICENSE"
install -m 0644 "$PATCHED_SOURCE/External/xxhash/LICENSE" "$STAGING/share/doc/switchyard-fex/XXHASH-LICENSE"
install -m 0644 "$PATCHED_SOURCE/External/cephes/LICENSE" "$STAGING/share/doc/switchyard-fex/CEPHES-LICENSE"
install -m 0644 "$PATCHED_SOURCE/External/SoftFloat-3e/src/extF80_add.c" "$STAGING/share/doc/switchyard-fex/SOFTFLOAT-LICENSE-SOURCE.c"
install -m 0644 "$FEX_PATCH" "$STAGING/share/src/switchyard-fex/$(basename "$FEX_PATCH")"
for file in "${ADAPTER_FILES[@]}"; do
  mkdir -p "$STAGING/share/src/switchyard-fex/adapter/$(dirname "$file")"
  install -m 0644 "$ADAPTER_SOURCE/$file" "$STAGING/share/src/switchyard-fex/adapter/$file"
done
for file in "${WINE_TEST_FILES[@]}"; do
  mkdir -p "$STAGING/share/src/switchyard-fex/adapter/wine/$(dirname "$file")"
  install -m 0644 "$ADAPTER_SOURCE/wine/$file" "$STAGING/share/src/switchyard-fex/adapter/wine/$file"
done
install -m 0644 "$ADAPTER_SOURCE/wine-runtime-native-arm64.entitlements" \
  "$STAGING/share/src/switchyard-fex/adapter/"

library_sha256="$(sha256_file "$dylib")"
[ "$library_sha256" = "$FEX_LIBRARY_SHA256" ] ||
  fail "FEX dylib SHA-256 $library_sha256 does not match $FEX_LIBRARY_SHA256"
/usr/bin/python3 -I - "$STAGING/switchyard-fex-runtime.json" "$FEX_BUILD_CONTRACT_VERSION" \
  "$FEX_REVISION" "$FEX_PATCH_SHA256" "$adapter_digest" "$library_sha256" "$MINIMUM_MACOS" \
  "$FEX_TOOLCHAIN_SHA256" "$FEX_PROVIDER_IDENTITY" <<'PY'
import json
import sys

path, contract, revision, patch, adapter, library, minimum, toolchain, identity = sys.argv[1:]
document = {
    "arm64ecRegisterABI": True,
    "buildContractVersion": int(contract),
    "hostArchitecture": "arm64",
    "library": "lib/libswitchyard-fex.6.0.0.dylib",
    "librarySha256": library,
    "minimumMacOS": minimum,
    "providerIdentity": identity,
    "sourceRepository": "https://github.com/FEX-Emu/FEX.git",
    "sourceRevision": revision,
    "sourcePatch": {
        "path": "share/src/switchyard-fex/fex-73dc3b3-darwin-core.patch",
        "sha256": patch,
    },
    "switchyardAdapterSha256": adapter,
    "toolchainSha256": toolchain,
}
with open(path, "w", encoding="utf-8", newline="\n") as stream:
    json.dump(document, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

/usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" write "$STAGING" >/dev/null
actual_runtime_digest="$(/usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" digest "$STAGING")"
[ "$actual_runtime_digest" = "$FEX_RUNTIME_DIGEST" ] ||
  fail "FEX runtime digest $actual_runtime_digest does not match $FEX_RUNTIME_DIGEST"
/usr/bin/python3 -I "$ROOT_DIR/switchyard/runtime_content_digest.py" \
  verify "$STAGING" "$FEX_RUNTIME_DIGEST" >/dev/null || fail "FEX runtime content digest mismatch"
validate_output "$STAGING" || fail "staged FEX runtime failed final validation"
# Reuse the fd-pinned RENAME_EXCL publisher. mv -n can instead move the stage
# inside a directory which appeared after preflight, falsely reporting success.
. "$ROOT_DIR/switchyard/lib/directory_safety.sh"
SWAP_HELPER_DIR="$BUILD_DIR"
ensure_preview_swap_helper
publication_status=0
"$SWAP_HELPER_DIR/switchyard-preview-directory-publish" "$STAGING" "$OUTPUT" exclusive || publication_status=$?
if [ "$publication_status" -ne 0 ]; then
  if [ "$publication_status" -eq 3 ]; then STAGING=""; fi
  fail "exclusive FEX publication failed ($publication_status); ambiguous paths are preserved"
fi
STAGING=""
echo "$OUTPUT"
