#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
MACHO_SIGNING_LIBRARY="$ROOT_DIR/switchyard/lib/macho_signing.sh"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-font-preparation-test.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && /bin/pwd -P)"
PREPARED_ROOT=""
ATTACK_LINK=""

cleanup()
{
    if [ -n "$PREPARED_ROOT" ] && [ -e "$PREPARED_ROOT" ]; then
        remove_prepared_font_runtime "$PREPARED_ROOT" || true
    fi
    if [ -n "$ATTACK_LINK" ] && [ -L "$ATTACK_LINK" ]; then
        /bin/rm -f -- "$ATTACK_LINK"
    fi
    case "$TEST_ROOT" in
        /private/tmp/switchyard-font-preparation-test.??????)
            [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
            ;;
        *) echo "refusing to clean unexpected font preparation test root $TEST_ROOT" >&2 ;;
    esac
}
trap cleanup EXIT

fail()
{
    echo "Native font runtime preparation test: $*" >&2
    exit 1
}

snapshot_prepared_font_roots()
{
    /usr/bin/python3 -I - <<'PY'
import os
import re
import stat

pattern = re.compile(r"switchyard-font-runtime[.][A-Za-z0-9]{6}\Z")
for entry in sorted(os.scandir("/private/tmp"), key=lambda item: item.name):
    if pattern.fullmatch(entry.name):
        info = entry.stat(follow_symlinks=False)
        print(entry.name, info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode))
PY
}

if [ "$(/usr/bin/uname -s)" != Darwin ] ||
   [ "$(/usr/bin/uname -m)" != arm64 ]; then
    echo "Native font runtime preparation test skipped outside native macOS ARM64"
    exit 0
fi

/bin/chmod 0700 "$TEST_ROOT"
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
export NATIVE_CPU_PROVIDER_ENABLED=1
export SWITCHYARD_RUNTIME_PROFILE=preview-native-arm64-fex
export HOST_DEPENDENCY_ARCH=arm64
export SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS=26.5
HELPER_FILE="$TEST_ROOT/native-font-runtime-preparation-helpers.sh"
/usr/bin/python3 -I - "$BUILD_SCRIPT" "$HELPER_FILE" <<'PY'
import pathlib
import re
import sys

source_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
source = source_path.read_text(encoding="utf-8")
names = (
    "remove_prepared_font_runtime",
    "cleanup_temporary_paths",
    "cleanup_temporary_paths_on_signal",
    "sha256_file",
    "short_sha256_stream",
    "content_tree_digest",
    "write_content_tree_digest",
    "content_tree_is_verified",
    "validate_extracted_tree_links",
    "verify_host_macho_tree_arches",
    "adhoc_sign_host_macho_tree",
    "verify_host_macho_tree_signatures",
    "adhoc_sign_and_verify_host_macho_tree",
    "verify_native_macho_tree_macos_compatibility",
    "verify_runtime_relative_macho_tree",
    "font_deps_match_profile_architecture",
    "relocate_font_deps_for_runtime",
    "prepare_font_runtime_for_install",
)
declarations = list(
    re.finditer(
        r"^(?P<name>[A-Za-z_][A-Za-z0-9_]*)\(\)[ \t]*(?:\{\n|\n\{\n)",
        source,
        re.MULTILINE,
    )
)
fragments = []
for name in names:
    matches = [match for match in declarations if match.group("name") == name]
    if len(matches) != 1:
        raise SystemExit(f"expected one {name} definition, found {len(matches)}")
    match = matches[0]
    closing = re.search(r"^}\n", source[match.end():], re.MULTILINE)
    if closing is None:
        raise SystemExit(f"could not find the end of {name}")
    end = match.end() + closing.end()
    fragment = source[match.start():end].rstrip()
    if not fragment.endswith("}"):
        raise SystemExit(f"extracted {name} definition is incomplete")
    if name == "prepare_font_runtime_for_install":
        allocation = fragment.find(
            "/usr/bin/mktemp -d /private/tmp/switchyard-font-runtime.XXXXXX"
        )
        validations = (
            fragment.find('validate_extracted_tree_links "$source_prefix"'),
            fragment.find('validate_extracted_tree_links "$font_assets_prefix"'),
        )
        if (
            allocation < 0
            or any(position < 0 or position > allocation for position in validations)
        ):
            raise SystemExit(
                "font input link validation must precede prepared-root allocation"
            )
    fragments.append(fragment)
for expected in (
    "trap cleanup_temporary_paths EXIT",
    "trap 'cleanup_temporary_paths_on_signal 129' HUP",
    "trap 'cleanup_temporary_paths_on_signal 130' INT",
    "trap 'cleanup_temporary_paths_on_signal 143' TERM",
):
    if source.count(expected) != 1:
        raise SystemExit("missing exact build-runtime signal cleanup trap: " + expected)
output_path.write_text("\n\n".join(fragments) + "\n", encoding="utf-8")
PY

# shellcheck disable=SC1090 # Libraries are resolved from the worktree root.
source "$PROFILE_LIBRARY"
# shellcheck disable=SC1090
source "$MACHO_SIGNING_LIBRARY"
# build_runtime.sh has no source-only mode. Use its exact extracted function
# definitions so this test cannot silently diverge from the production closure.
# shellcheck disable=SC1090
source "$HELPER_FILE"

native_homebrew=/opt/homebrew/bin/brew
[ -f "$native_homebrew" ] && [ ! -L "$native_homebrew" ] &&
    [ -x "$native_homebrew" ] || fail "physical Apple Silicon Homebrew is unavailable"
llvm_prefix="$("$native_homebrew" --prefix llvm)"
llvm_bin="$(cd "$llvm_prefix/bin" && /bin/pwd -P)"
switchyard_qualify_native_llvm_compilers "$llvm_bin" ||
    fail "native LLVM compiler qualification failed"
switchyard_qualify_native_macos_sdk 26.5 /usr/bin/xcrun ||
    fail "native macOS SDK qualification failed"

SOURCE_PREFIX="$TEST_ROOT/source-font-prefix"
ASSET_PREFIX="$TEST_ROOT/font-assets"
FONTCONFIG_ASSET_FRAGMENT="$TEST_ROOT/50-switchyard-font-assets.conf"
/bin/mkdir -p "$SOURCE_PREFIX/lib" "$SOURCE_PREFIX/etc/fonts" \
    "$SOURCE_PREFIX/var/cache/fontconfig" \
    "$ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets"

dep_source="$TEST_ROOT/dependency.c"
font_source="$TEST_ROOT/font.c"
/usr/bin/printf '%s\n' \
    'int switchyard_font_dependency(void) { return 11; }' >"$dep_source"
/usr/bin/printf '%s\n' \
    'extern int switchyard_font_dependency(void);' \
    'int switchyard_font_fixture(void) { return switchyard_font_dependency(); }' \
    >"$font_source"

dependency="$SOURCE_PREFIX/lib/libswitchyard-font-dependency.1.dylib"
font_library="$SOURCE_PREFIX/lib/libswitchyard-font-fixture.1.dylib"
SDKROOT="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT" \
MACOSX_DEPLOYMENT_TARGET=26.5 \
"$SWITCHYARD_QUALIFIED_NATIVE_CLANG" \
    "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
    -arch arm64 "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG" \
    -mmacosx-version-min=26.5 -dynamiclib -Wl,-headerpad_max_install_names \
    -Wl,-install_name,"$dependency" "$dep_source" -o "$dependency"
SDKROOT="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT" \
MACOSX_DEPLOYMENT_TARGET=26.5 \
"$SWITCHYARD_QUALIFIED_NATIVE_CLANG" \
    "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
    -arch arm64 "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG" \
    -mmacosx-version-min=26.5 -dynamiclib -Wl,-headerpad_max_install_names \
    -Wl,-install_name,"$font_library" "$font_source" "$dependency" \
    -o "$font_library"

/usr/bin/file -b "$dependency" | /usr/bin/grep -q 'Mach-O.*arm64' ||
    fail "qualified toolchain did not produce an ARM64 dependency"
/usr/bin/otool -L "$font_library" | /usr/bin/awk 'NR > 1 { print $1 }' |
    /usr/bin/grep -Fqx "$dependency" ||
    fail "fixture did not begin with an absolute source dependency"

{
    /usr/bin/printf '%s\n' '<?xml version="1.0"?>' '<fontconfig>'
    /usr/bin/printf '  <cachedir>%s/var/cache/fontconfig</cachedir>\n' \
        "$SOURCE_PREFIX"
    /usr/bin/printf '%s\n' \
        '  <cachedir>/tmp/switchyard-preserved-font-cache</cachedir>' \
        '</fontconfig>'
} >"$SOURCE_PREFIX/etc/fonts/fonts.conf"
/usr/bin/printf '%s\n' \
    '<?xml version="1.0"?>' \
    '<fontconfig><dir>share/wine/fonts</dir></fontconfig>' \
    >"$FONTCONFIG_ASSET_FRAGMENT"
/usr/bin/printf '%s\n' 'fixture font asset license' > \
    "$ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/LICENSE.txt"
/usr/bin/printf '%s\n' 'fixture font asset provenance' > \
    "$ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/SOURCES.txt"

write_content_tree_digest "$SOURCE_PREFIX"
content_tree_is_verified "$SOURCE_PREFIX" ||
    fail "source fixture content marker is invalid"
source_digest="$(/usr/bin/tr -d '[:space:]' < \
    "$SOURCE_PREFIX/.switchyard-content-sha256")"

UNSAFE_SOURCE_PREFIX="$TEST_ROOT/unsafe-source-font-prefix"
OUTSIDE_SOURCE_ROOT="$TEST_ROOT/outside-source-root"
/bin/mkdir "$UNSAFE_SOURCE_PREFIX" "$OUTSIDE_SOURCE_ROOT"
/usr/bin/printf '%s\n' 'must not be copied' >"$OUTSIDE_SOURCE_ROOT/sentinel"
/bin/ln -s "$OUTSIDE_SOURCE_ROOT" "$UNSAFE_SOURCE_PREFIX/escaping-link"
if validate_extracted_tree_links "$UNSAFE_SOURCE_PREFIX" >/dev/null 2>&1; then
    fail "production tree validator accepted an escaping input symlink"
fi
prepared_roots_before="$(snapshot_prepared_font_roots)"
unsafe_stdout="$TEST_ROOT/unsafe-preparation.stdout"
unsafe_stderr="$TEST_ROOT/unsafe-preparation.stderr"
if prepare_font_runtime_for_install \
        "$UNSAFE_SOURCE_PREFIX" "$ASSET_PREFIX" \
        >"$unsafe_stdout" 2>"$unsafe_stderr"; then
    fail "production font preparation accepted an escaping input symlink"
fi
[ ! -s "$unsafe_stdout" ] ||
    fail "rejected font preparation returned a prepared runtime path"
[ -s "$unsafe_stderr" ] ||
    fail "rejected font preparation did not explain the unsafe input"
prepared_roots_after="$(snapshot_prepared_font_roots)"
[ "$prepared_roots_after" = "$prepared_roots_before" ] ||
    fail "rejected unsafe input allocated or leaked a prepared font root"
[ -f "$OUTSIDE_SOURCE_ROOT/sentinel" ] ||
    fail "unsafe input validation followed the escaping symlink"

prepared_roots_before="$(snapshot_prepared_font_roots)"
set +e
(
    ditto()
    {
        /bin/sh -c '/bin/kill -TERM "$PPID"'
        return 99
    }
    prepare_font_runtime_for_install "$SOURCE_PREFIX" "$ASSET_PREFIX"
) >"$TEST_ROOT/interrupted-preparation.stdout" \
  2>"$TEST_ROOT/interrupted-preparation.stderr"
interrupted_status=$?
set -e
if [ "$interrupted_status" -eq 0 ]; then
    /bin/cat "$TEST_ROOT/interrupted-preparation.stderr" >&2 || true
    fail "interrupted preparation reported success"
fi
[ ! -s "$TEST_ROOT/interrupted-preparation.stdout" ] ||
    fail "interrupted preparation returned a prepared runtime path"
prepared_roots_after="$(snapshot_prepared_font_roots)"
[ "$prepared_roots_after" = "$prepared_roots_before" ] ||
    fail "signal inside preparation leaked its unpublished private runtime"

PREPARED_ROOT="$(prepare_font_runtime_for_install \
    "$SOURCE_PREFIX" "$ASSET_PREFIX")" ||
    fail "production font runtime preparation failed"
case "$PREPARED_ROOT" in
    /private/tmp/switchyard-font-runtime.??????) ;;
    *) fail "production function returned an unexpected prepared path: $PREPARED_ROOT" ;;
esac
[ -d "$PREPARED_ROOT" ] && [ ! -L "$PREPARED_ROOT" ] ||
    fail "prepared font runtime is missing or unsafe"

content_tree_is_verified "$PREPARED_ROOT" ||
    fail "prepared font runtime marker does not match installed bytes"
prepared_digest="$(content_tree_digest "$PREPARED_ROOT")"
marker_digest="$(/usr/bin/tr -d '[:space:]' < \
    "$PREPARED_ROOT/.switchyard-content-sha256")"
[ "$prepared_digest" = "$marker_digest" ] ||
    fail "prepared marker differs from an independent content digest"
[ "$prepared_digest" != "$source_digest" ] ||
    fail "prepared marker was not refreshed after closure mutation"

prepared_dependency="$PREPARED_ROOT/lib/$(/usr/bin/basename "$dependency")"
prepared_font="$PREPARED_ROOT/lib/$(/usr/bin/basename "$font_library")"
/usr/bin/otool -D "$prepared_dependency" | /usr/bin/awk 'NR > 1 { print $1 }' |
    /usr/bin/grep -Fqx '@loader_path/libswitchyard-font-dependency.1.dylib' ||
    fail "prepared dependency does not have a loader-relative install name"
/usr/bin/otool -D "$prepared_font" | /usr/bin/awk 'NR > 1 { print $1 }' |
    /usr/bin/grep -Fqx '@loader_path/libswitchyard-font-fixture.1.dylib' ||
    fail "prepared font dylib does not have a loader-relative install name"
/usr/bin/otool -L "$prepared_font" | /usr/bin/awk 'NR > 1 { print $1 }' |
    /usr/bin/grep -Fqx '@loader_path/libswitchyard-font-dependency.1.dylib' ||
    fail "prepared font dependency is not loader-relative"
if /usr/bin/otool -L "$prepared_font" | /usr/bin/grep -F "$SOURCE_PREFIX" >/dev/null; then
    fail "prepared font dylib retains its source-prefix dependency"
fi
verify_runtime_relative_macho_tree "$PREPARED_ROOT" "prepared font fixture" ||
    fail "prepared dylibs did not pass the production relative-path validator"

for candidate in "$prepared_dependency" "$prepared_font"; do
    /usr/bin/codesign --verify --strict --verbose=2 \
        "$candidate" >/dev/null 2>&1 ||
        fail "prepared dylib does not have a strict-valid signature: $candidate"
    signing_details="$(/usr/bin/codesign -d --verbose=4 "$candidate" 2>&1)" ||
        fail "prepared dylib signature details are unreadable: $candidate"
    [ "$(/usr/bin/printf '%s\n' "$signing_details" |
        /usr/bin/grep -Fxc 'Signature=adhoc')" -eq 1 ] ||
        fail "prepared dylib is not ad-hoc signed: $candidate"
done
verify_host_macho_tree_signatures "$PREPARED_ROOT" "prepared font fixture" ||
    fail "prepared dylibs did not pass the production signature validator"

if /usr/bin/grep -F "$SOURCE_PREFIX/var/cache/fontconfig" \
        "$PREPARED_ROOT/etc/fonts/fonts.conf" >/dev/null; then
    fail "prepared fonts.conf retains the source cache path"
fi
/usr/bin/grep -Fqx \
    '  <cachedir>/tmp/switchyard-preserved-font-cache</cachedir>' \
    "$PREPARED_ROOT/etc/fonts/fonts.conf" ||
    fail "preparation removed an unrelated font cache path"
/usr/bin/cmp -s "$FONTCONFIG_ASSET_FRAGMENT" \
    "$PREPARED_ROOT/etc/fonts/conf.d/50-switchyard-font-assets.conf" ||
    fail "prepared closure is missing the exact Fontconfig asset fragment"
/usr/bin/cmp -s \
    "$ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/LICENSE.txt" \
    "$PREPARED_ROOT/share/doc/switchyard-font-assets/LICENSE.txt" ||
    fail "prepared closure is missing font asset documentation"
/usr/bin/cmp -s \
    "$ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/SOURCES.txt" \
    "$PREPARED_ROOT/share/doc/switchyard-font-assets/SOURCES.txt" ||
    fail "prepared closure is missing font asset provenance"

remove_prepared_font_runtime "$PREPARED_ROOT" ||
    fail "production cleanup rejected its owned prepared directory"
[ ! -e "$PREPARED_ROOT" ] || fail "owned prepared directory survived cleanup"
PREPARED_ROOT=""

victim="$TEST_ROOT/cleanup-victim"
/bin/mkdir "$victim"
/usr/bin/printf '%s\n' 'must survive' >"$victim/sentinel"
ATTACK_LINK="$(/usr/bin/mktemp -d /private/tmp/switchyard-font-runtime.XXXXXX)"
/bin/rmdir "$ATTACK_LINK"
/bin/ln -s "$victim" "$ATTACK_LINK"
if remove_prepared_font_runtime "$ATTACK_LINK" >/dev/null 2>&1; then
    fail "production cleanup accepted a symlinked prepared directory"
fi
[ -f "$victim/sentinel" ] || fail "cleanup followed a symlink into another tree"
/bin/rm -f -- "$ATTACK_LINK"
ATTACK_LINK=""

SIGNAL_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-font-runtime.XXXXXX)"
set +e
/bin/bash --noprofile --norc -c '
set -euo pipefail
source "$1"
NATIVE_ENTITLEMENTS_SNAPSHOT_FD=""
INSTALL_STAGE_ROOT=""
SWAP_HELPER_DIR=""
FONT_RUNTIME_PREPARED_ROOT="$2"
trap cleanup_temporary_paths EXIT
trap "cleanup_temporary_paths_on_signal 129" HUP
trap "cleanup_temporary_paths_on_signal 130" INT
trap "cleanup_temporary_paths_on_signal 143" TERM
kill -TERM "$$"
exit 99
' _ "$HELPER_FILE" "$SIGNAL_ROOT"
signal_status=$?
set -e
[ "$signal_status" -eq 143 ] ||
    fail "TERM cleanup fixture exited with $signal_status instead of 143"
[ ! -e "$SIGNAL_ROOT" ] && [ ! -L "$SIGNAL_ROOT" ] ||
    fail "TERM cleanup left the prepared font runtime behind"

echo "Native font runtime final-form digest, relocation, signing, assets, and cleanup verified"
