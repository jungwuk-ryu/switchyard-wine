#!/usr/bin/env bash
# The fixture replaces functions loaded from release_runtime.sh and validates
# literal source fragments; assignments through output-variable APIs are dynamic.
# shellcheck disable=SC2016,SC2034,SC2154,SC2329
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
RELEASE_SCRIPT="$ROOT_DIR/switchyard/release_runtime.sh"
DIGEST_HELPER="$ROOT_DIR/switchyard/runtime_content_digest.py"
NATIVE_ENTITLEMENTS="$ROOT_DIR/switchyard/wine-runtime-native-arm64.entitlements"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-release-runtime.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
NATIVE_MACHO_FIXTURE="$TEST_ROOT/native-arm64-macho-fixture"
X86_64_MACHO_FIXTURE="$TEST_ROOT/x86_64-macho-fixture"
UNIVERSAL_MACHO_FIXTURE="$TEST_ROOT/universal-macho-fixture"
SOURCE_REVISION="$(/usr/bin/git -C "$ROOT_DIR" rev-parse HEAD)"

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  case "$TEST_ROOT" in
    /private/tmp/switchyard-release-runtime.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected release test root: $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT HUP INT TERM

fail() {
  echo "release runtime fixture failed: $1" >&2
  exit 1
}

make_runtime() {
  local runtime="$1"
  local unknown_field="${2:-0}"
  local required_notice

  /bin/mkdir -m 700 "$runtime"
  /bin/mkdir -p \
    "$runtime/bin" \
    "$runtime/lib/wine/aarch64-unix" \
    "$runtime/lib/wine/aarch64-windows" \
    "$runtime/share/doc/switchyard-wine" \
    "$runtime/lib/switchyard-mesa/share/doc/switchyard-mesa" \
    "$runtime/lib/switchyard-gstreamer/share/doc/switchyard-gstreamer" \
    "$runtime/lib/switchyard-gstreamer/share/licenses/gstreamer-1.0" \
    "$runtime/lib/switchyard-gstreamer/share/licenses/ffmpeg" \
    "$runtime/lib/switchyard-tls/share/doc/switchyard-tls"

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
    /usr/bin/printf 'release fixture notice\n' >"$runtime/$required_notice"
  done

  if [ ! -f "$NATIVE_MACHO_FIXTURE" ]; then
    /usr/bin/printf '%s\n' 'int main(void) { return 0; }' |
      /usr/bin/clang -arch arm64 -x c - -o "$NATIVE_MACHO_FIXTURE"
    /usr/bin/printf '%s\n' 'int main(void) { return 0; }' |
      /usr/bin/clang -arch x86_64 -x c - -o "$X86_64_MACHO_FIXTURE"
    /usr/bin/lipo -create \
      "$X86_64_MACHO_FIXTURE" "$NATIVE_MACHO_FIXTURE" \
      -output "$UNIVERSAL_MACHO_FIXTURE"
    /bin/chmod 0755 "$NATIVE_MACHO_FIXTURE"
    /bin/chmod 0755 "$X86_64_MACHO_FIXTURE" "$UNIVERSAL_MACHO_FIXTURE"
  fi
  /bin/cp "$NATIVE_MACHO_FIXTURE" "$runtime/lib/wine/aarch64-unix/wine"
  /bin/cp "$NATIVE_MACHO_FIXTURE" "$runtime/bin/wine.switchyard-real"
  /bin/cp "$NATIVE_MACHO_FIXTURE" "$runtime/bin/wineserver"
  /bin/cp "$NATIVE_MACHO_FIXTURE" "$runtime/lib/wine/aarch64-unix/helper.dylib"
  /bin/cp "$UNIVERSAL_MACHO_FIXTURE" \
    "$runtime/lib/switchyard-gstreamer/libfixture.dylib"
  /bin/chmod 0755 \
    "$runtime/lib/wine/aarch64-unix/wine" \
    "$runtime/bin/wine.switchyard-real" \
    "$runtime/bin/wineserver" \
    "$runtime/lib/wine/aarch64-unix/helper.dylib" \
    "$runtime/lib/switchyard-gstreamer/libfixture.dylib"
  /bin/ln -s wine.switchyard-real "$runtime/bin/switchyard-wine"
  /usr/bin/printf 'fixture PE command\n' >"$runtime/lib/wine/aarch64-windows/cmd.exe"

  /usr/bin/python3 -I - "$runtime/switchyard-runtime.json" \
    "$SOURCE_REVISION" "$unknown_field" <<'PY'
import json
import sys

output, revision, unknown = sys.argv[1:]
value = {
    "manifestVersion": 2,
    "id": "switchyard-local-native-arm64-fex-release-fixture",
    "runtimeFamily": "preview-native-arm64-fex",
    "buildProfile": "switchyard-native-arm64-fex",
    "host": {
        "platform": "macos",
        "architecture": "arm64",
        "wineUnixArchitecture": "aarch64",
        "buildTriplet": "aarch64-apple-darwin",
        "hostTriplet": "aarch64-apple-darwin",
        "architectureCommand": ["arch", "-arm64"],
        "requiresRosetta": False,
        "minimumMacOS": "26.5",
        "gstreamerRegistryArchitecture": "arm64",
    },
    "peArchitectures": ["aarch64", "arm64ec", "x86_64", "i386"],
    "sourceRevision": revision,
    "sourceDirty": False,
    "gptkPath": "",
    "gptkRedistDigest": "no-gptk",
    "installPrefix": "/build/runtime",
    "executable": "/build/runtime/bin/switchyard-wine",
}
if unknown == "1":
    value["unexpectedReleaseField"] = True
with open(output, "x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
  /bin/chmod 0644 "$runtime/switchyard-runtime.json"
  /usr/bin/python3 -I "$DIGEST_HELPER" write "$runtime" >/dev/null
}

make_fake_codesign() {
  local destination="$1"

  # apply_patch owns the test source; this heredoc creates only an ephemeral
  # executable fixture under the bounded test root.
  /usr/bin/sed 's/^+//' >"$destination" <<'EOF'
+#!/usr/bin/env bash
+set -euo pipefail
+
+operation=sign
+case "${1:-}" in
+  --verify) operation=verify ;;
+  -d)
+    operation=details
+    for argument in "$@"; do
+      [ "$argument" != --entitlements ] || operation=entitlements
+    done
+    ;;
+esac
+
+case "$operation" in
+  sign)
+    count="$(/bin/cat "$SWITCHYARD_TEST_SIGN_COUNT")"
+    count=$((count + 1))
+    /usr/bin/printf '%s\n' "$count" >"$SWITCHYARD_TEST_SIGN_COUNT"
+    entry=plain
+    previous=""
+    for argument in "$@"; do
+      [ "$previous" != --entitlements ] || entry=entry
+      previous="$argument"
+    done
+    /usr/bin/printf 'sign:%s\n' "$entry" >>"$SWITCHYARD_TEST_ORDER_LOG"
+    [ "${SWITCHYARD_TEST_FAIL_SIGN_AT:-0}" -ne "$count" ] || exit 81
+    ;;
+  verify)
+    ;;
+  details)
+    item="${@: -1}"
+    team_id="${SWITCHYARD_TEST_TEAM_ID:-M3CULMDKU3}"
+    case "$item" in
+      "${SWITCHYARD_TEST_EXTRACTED_RUNTIME:-/nonexistent}"/*)
+        team_id="${SWITCHYARD_TEST_EXTRACTED_TEAM_ID:-$team_id}"
+        ;;
+    esac
+    /usr/bin/printf '%s\n' \
+      'CodeDirectory v=20500 size=1 flags=0x10000(runtime) hashes=1+0 location=embedded' \
+      'Signature size=9000' \
+      "Authority=Developer ID Application: Fixture ($team_id)" \
+      "TeamIdentifier=$team_id" \
+      'Runtime Version=26.0.0'
+    ;;
+  entitlements)
+    item="${@: -1}"
+    case "$item" in
+      ./wine|./wine.switchyard-real|*/lib/wine/aarch64-unix/wine|*/bin/wine.switchyard-real) ;;
+      *) exit 1 ;;
+    esac
+    unexpected=0
+    case "$item" in
+      "${SWITCHYARD_TEST_EXTRACTED_RUNTIME:-/nonexistent}"/*)
+        unexpected="${SWITCHYARD_TEST_EXTRACTED_UNEXPECTED_ENTITLEMENT:-0}"
+        ;;
+    esac
+    if [ "$unexpected" = 1 ]; then
+      /usr/bin/sed 's#</dict>#<key>com.apple.security.get-task-allow</key><true/></dict>#' \
+        "$SWITCHYARD_TEST_NATIVE_ENTITLEMENTS"
+    else
+      /bin/cat "$SWITCHYARD_TEST_NATIVE_ENTITLEMENTS"
+    fi
+    ;;
+  *) exit 82 ;;
+esac
EOF
  /bin/chmod 0755 "$destination"
}

assert_no_publication() {
  local output="$1"

  if [ -d "$output" ] &&
     /usr/bin/find "$output" -mindepth 1 -print -quit | /usr/bin/grep -q .; then
    /usr/bin/find "$output" -mindepth 1 -maxdepth 2 -print >&2
    fail "failed release published output or leaked private staging"
  fi
}

assert_native_release_cleanup_preserves_foreign_replacement() {
  local fixture_root="$TEST_ROOT/foreign-cleanup"
  local output="$fixture_root/archive.zip"
  local identity

  /bin/mkdir -m 700 "$fixture_root"
  /usr/bin/printf 'owned archive\n' >"$output"
  /bin/chmod 0644 "$output"
  (
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    identity="$(switchyard_native_release_file_identity "$output")"
    /bin/mv "$output" "$output.away"
    /usr/bin/printf 'foreign replacement\n' >"$output"
    /bin/chmod 0644 "$output"
    if remove_owned_native_release_output "$output" "$identity"; then
      fail "release cleanup removed a foreign replacement"
    fi
  )
  /usr/bin/grep -Fqx 'foreign replacement' "$output" ||
    fail "release cleanup changed a foreign replacement"
}

event_line() {
  local event="$1"
  local log="$2"

  /usr/bin/awk -v wanted="$event" '$0 == wanted { print NR; exit }' "$log"
}

assert_native_release_source_contract() {
  /usr/bin/python3 -I - "$RELEASE_SCRIPT" <<'PY'
import re
import sys


source_name = sys.argv[1]
with open(source_name, encoding="utf-8") as stream:
    source = stream.read()

definition = re.compile(
    r"(?m)^([A-Za-z_][A-Za-z0-9_]*)[ \t]*\([ \t]*\)[ \t]*\{[^\n]*$"
)
definitions = list(definition.finditer(source))


def function_body(name):
    for index, match in enumerate(definitions):
        if match.group(1) != name:
            continue
        end = definitions[index + 1].start() if index + 1 < len(definitions) else len(source)
        return source[match.end():end]
    raise SystemExit(f"native release source contract lost function {name}")


def call_offsets(body, name):
    return [
        match.start()
        for match in re.finditer(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])", body)
    ]


release = function_body("switchyard_release_preview_native")
operations = {
    "sign": call_offsets(release, "switchyard_sign_native_release_runtime"),
    "refresh": call_offsets(
        release, "switchyard_refresh_native_arm64_signed_runtime_manifest"
    ),
    "validate": call_offsets(release, "switchyard_validate_native_release_runtime"),
    "smoke": call_offsets(release, "switchyard_run_native_release_smoke"),
    "outer": call_offsets(release, "switchyard_publish_native_outer_digest"),
    "tree_verify": call_offsets(release, "runtime_content_tree_is_verified"),
    "tree_match": call_offsets(release, "runtime_content_tree_matches_digest"),
    "archive": call_offsets(release, "switchyard_create_native_release_archive"),
    "pin": call_offsets(release, "switchyard_pin_native_release_archive"),
    "extract": call_offsets(release, "switchyard_extract_native_release_archive"),
    "extracted_validate": call_offsets(
        release, "switchyard_validate_extracted_native_release_runtime"
    ),
    "macho_verify": call_offsets(
        release, "switchyard_verify_native_release_macho_tree"
    ),
}
expected_counts = {
    "sign": 1,
    "refresh": 1,
    "validate": 1,
    "smoke": 1,
    "outer": 1,
    "tree_verify": 1,
    "tree_match": 3,
    "archive": 1,
    "pin": 3,
    "extract": 1,
    "extracted_validate": 1,
    "macho_verify": 1,
}
for operation, expected in expected_counts.items():
    actual = len(operations[operation])
    if actual != expected:
        raise SystemExit(
            f"native release source contract has {actual} {operation} calls, expected {expected}"
        )

ordered = [
    operations["sign"][0],
    operations["refresh"][0],
    operations["validate"][0],
    operations["outer"][0],
    operations["tree_match"][0],
    operations["archive"][0],
    operations["pin"][0],
    operations["extract"][0],
    operations["extracted_validate"][0],
    operations["macho_verify"][0],
    operations["tree_match"][1],
    operations["smoke"][0],
    operations["tree_match"][2],
    operations["pin"][1],
    operations["pin"][2],
]
if ordered != sorted(ordered) or len(set(ordered)) != len(ordered):
    raise SystemExit(
        "native release source contract is not sign, refresh, staging validate, "
        "outer digest, archive, archive pin, private extract, extracted validate, Mach-O "
        "verify, smoke"
    )

outer_publisher = function_body("switchyard_publish_native_outer_digest")
if len(call_offsets(outer_publisher, "write_runtime_content_tree_digest")) != 1:
    raise SystemExit("native outer-digest publisher is not the sole one-shot marker writer")
if call_offsets(release, "write_runtime_content_tree_digest"):
    raise SystemExit("native release flow writes the outer marker outside its publisher")
PY
}

assert_native_release_stop_is_idempotent() {
  local stop_runtime="$TEST_ROOT/stop-runtime"
  local stop_prefix="$TEST_ROOT/stop-prefix"
  local stop_log="$TEST_ROOT/stop.log"
  local status
  local stop_pid
  local unrelated_pid

  /bin/mkdir -p "$stop_runtime/bin" "$stop_prefix"
  /usr/bin/sed 's/^+//' >"$stop_runtime/bin/wineserver" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

/usr/bin/printf '%s WINEPREFIX=%s\n' "${1:-}" "${WINEPREFIX:-}" \
  >>"$SWITCHYARD_TEST_STOP_LOG"
case "${1:-}" in
  -k) control_status="${SWITCHYARD_TEST_STOP_K_STATUS:-0}" ;;
  -w) control_status="${SWITCHYARD_TEST_STOP_W_STATUS:-0}" ;;
  *) exit 88 ;;
esac
if [ "$control_status" = hang ]; then
  /usr/bin/printf '%s\n' "$$" >>"$SWITCHYARD_TEST_STOP_PID_FILE"
  trap '' TERM
  exec /bin/sleep 30
fi
exit "$control_status"
EOF
  /bin/chmod 0755 "$stop_runtime/bin/wineserver"

  : >"$stop_log"
  (
    export SWITCHYARD_TEST_STOP_LOG="$stop_log"
    export SWITCHYARD_TEST_STOP_K_STATUS=0
    export SWITCHYARD_TEST_STOP_W_STATUS=0
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_stop_native_release_wineserver "$stop_runtime" "$stop_prefix"
  ) || fail "native release stop rejected a normal stop/wait"
  /usr/bin/grep -Fqx -- "-k WINEPREFIX=$stop_prefix" "$stop_log" &&
    /usr/bin/grep -Fqx -- "-w WINEPREFIX=$stop_prefix" "$stop_log" ||
    fail "native release stop did not use the exact prefix for normal stop/wait"

  : >"$stop_log"
  (
    export SWITCHYARD_TEST_STOP_LOG="$stop_log"
    export SWITCHYARD_TEST_STOP_K_STATUS=1
    export SWITCHYARD_TEST_STOP_W_STATUS=0
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_stop_native_release_wineserver "$stop_runtime" "$stop_prefix"
  ) || fail "native release stop rejected an already-stopped exact prefix"
  [ "$(/usr/bin/awk 'END { print NR }' "$stop_log")" -eq 2 ] &&
    /usr/bin/awk 'NR == 1 { exit ($1 == "-k" ? 0 : 1) }
                 NR == 2 { exit ($1 == "-w" ? 0 : 1) }' "$stop_log" ||
    fail "native release already-stopped path did not fall through to wait"

  : >"$stop_log"
  set +e
  (
    export SWITCHYARD_TEST_STOP_LOG="$stop_log"
    export SWITCHYARD_TEST_STOP_K_STATUS=1
    export SWITCHYARD_TEST_STOP_W_STATUS=1
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_stop_native_release_wineserver "$stop_runtime" "$stop_prefix"
  )
  status=$?
  set -e
  [ "$status" -ne 0 ] ||
    fail "native release stop accepted a failed kill and failed wait"

  : >"$stop_log"
  (
    export SWITCHYARD_TEST_STOP_LOG="$stop_log"
    export SWITCHYARD_TEST_STOP_PID_FILE="$TEST_ROOT/stop-k.pid"
    export SWITCHYARD_TEST_STOP_K_STATUS=hang
    export SWITCHYARD_TEST_STOP_W_STATUS=0
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    NATIVE_RELEASE_WINESERVER_CONTROL_ATTEMPTS=2
    NATIVE_RELEASE_WINESERVER_TERM_ATTEMPTS=2
    NATIVE_RELEASE_WINESERVER_KILL_ATTEMPTS=20
    switchyard_stop_native_release_wineserver "$stop_runtime" "$stop_prefix"
  ) || fail "native release stop did not bound a failed kill before exact-prefix wait"
  while IFS= read -r stop_pid; do
    ! /bin/kill -0 "$stop_pid" 2>/dev/null ||
      fail "native release stop left its timed-out kill control process running"
  done <"$TEST_ROOT/stop-k.pid"
  [ "$(/usr/bin/awk 'END { print NR }' "$stop_log")" -eq 2 ] &&
    /usr/bin/awk 'NR == 1 { exit ($1 == "-k" ? 0 : 1) }
                 NR == 2 { exit ($1 == "-w" ? 0 : 1) }' "$stop_log" ||
    fail "native release stop did not wait after a timed-out kill control"

  : >"$stop_log"
  /bin/sleep 30 &
  unrelated_pid=$!
  set +e
  (
    export SWITCHYARD_TEST_STOP_LOG="$stop_log"
    export SWITCHYARD_TEST_STOP_PID_FILE="$TEST_ROOT/stop-w.pid"
    export SWITCHYARD_TEST_STOP_K_STATUS=1
    export SWITCHYARD_TEST_STOP_W_STATUS=hang
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    NATIVE_RELEASE_WINESERVER_CONTROL_ATTEMPTS=2
    NATIVE_RELEASE_WINESERVER_TERM_ATTEMPTS=2
    NATIVE_RELEASE_WINESERVER_KILL_ATTEMPTS=20
    switchyard_stop_native_release_wineserver "$stop_runtime" "$stop_prefix"
  )
  status=$?
  set -e
  [ "$status" -ne 0 ] ||
    fail "native release stop accepted a timed-out exact-prefix wait"
  while IFS= read -r stop_pid; do
    ! /bin/kill -0 "$stop_pid" 2>/dev/null ||
      fail "native release stop left its timed-out wait control process running"
  done <"$TEST_ROOT/stop-w.pid"
  /bin/kill -0 "$unrelated_pid" 2>/dev/null ||
    fail "native release stop signaled an unrelated process"
  /bin/kill -TERM "$unrelated_pid"
  wait "$unrelated_pid" 2>/dev/null || true
}

make_invalid_native_release_archive() {
  local archive="$1"
  local root_name="$2"
  local scenario="$3"
  local producer_archive="${4:-}"

  /usr/bin/python3 -I - \
    "$archive" "$root_name" "$scenario" "$producer_archive" <<'PY'
import shutil
import os
import stat
import struct
import sys
import unicodedata
import warnings
import zipfile


archive, root, scenario, producer_archive = sys.argv[1:]


def member(name, mode, data=b""):
    value = zipfile.ZipInfo(name)
    value.create_system = 3
    value.compress_type = zipfile.ZIP_STORED
    value.external_attr = mode << 16
    return value, data


entries = None
if scenario in (
    "mode",
    "oversize",
    "encrypted",
    "unsupported-compression",
    "crc",
    "truncated",
    "duplicate",
    "casefold",
    "nfc",
    "escaping-symlink",
):
    if not producer_archive:
        raise SystemExit("producer archive is required for mutation fixtures")
    shutil.copyfile(producer_archive, archive)
elif scenario == "extra-root":
    entries = [member("Other-Root/file", stat.S_IFREG | 0o644, b"other\n"),
               member(root + "/file", stat.S_IFREG | 0o644, b"runtime\n")]
elif scenario == "absolute":
    entries = [member("/absolute", stat.S_IFREG | 0o644, b"bad\n")]
elif scenario == "traversal":
    entries = [member(root + "/../escape", stat.S_IFREG | 0o644, b"bad\n")]
elif scenario == "unsupported-type":
    entries = [member(root + "/pipe", stat.S_IFIFO | 0o600)]
else:
    raise SystemExit("unknown invalid archive fixture: " + scenario)

if entries is not None:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(archive, "x", allowZip64=True) as stream:
            for info, data in entries:
                stream.writestr(info, data)

if scenario in ("duplicate", "casefold", "nfc"):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(archive, "a", allowZip64=True) as stream:
            if scenario == "duplicate":
                original = stream.getinfo(root + "/bin/runtime")
                duplicate, data = member(
                    original.filename, stat.S_IFREG | 0o755,
                    stream.read(original),
                )
            elif scenario == "casefold":
                duplicate, data = member(
                    root + "/bin/Runtime", stat.S_IFREG | 0o644, b"alias\n"
                )
            else:
                composed = unicodedata.normalize(
                    "NFC", "cafe\N{COMBINING ACUTE ACCENT}"
                )
                duplicate, data = member(
                    root + "/bin/" + unicodedata.normalize("NFD", composed),
                    stat.S_IFREG | 0o644,
                    b"alias\n",
                )
            stream.writestr(duplicate, data)
elif scenario == "escaping-symlink":
    replacement = archive + ".replacement"
    with zipfile.ZipFile(archive, "r") as source:
        payloads = [(info, source.read(info)) for info in source.infolist()]
    with zipfile.ZipFile(replacement, "x", allowZip64=True) as destination:
        for original, data in payloads:
            clone = zipfile.ZipInfo(original.filename, original.date_time)
            clone.create_system = original.create_system
            clone.compress_type = original.compress_type
            clone.external_attr = original.external_attr
            clone.internal_attr = original.internal_attr
            clone.extra = original.extra
            clone.comment = original.comment
            if original.filename == root + "/bin/escape-link":
                data = b"../../escape"
            destination.writestr(clone, data)
    os.replace(replacement, archive)

with open(archive, "r+b") as stream:
    data = bytearray(stream.read())
    if scenario in (
        "mode", "oversize", "encrypted", "unsupported-compression"
    ):
        central = 0
        expected = (root + "/bin/runtime").encode("utf-8")
        while True:
            central = data.index(b"PK\x01\x02", central)
            name_length, extra_length, comment_length = struct.unpack_from(
                "<HHH", data, central + 28
            )
            name = bytes(data[central + 46:central + 46 + name_length])
            if name == expected:
                break
            central += 46 + name_length + extra_length + comment_length
        local = struct.unpack_from("<I", data, central + 42)[0]
        if scenario == "mode":
            struct.pack_into("<I", data, central + 38,
                             (stat.S_IFREG | 0o777) << 16)
        elif scenario == "oversize":
            # The central-directory uncompressed-size field is authoritative
            # to ZipFile readers.  No large allocation is needed.
            struct.pack_into("<I", data, central + 24, 0x7FFFFFFF)
        elif scenario == "encrypted":
            central_flags = struct.unpack_from("<H", data, central + 8)[0]
            local_flags = struct.unpack_from("<H", data, local + 6)[0]
            struct.pack_into("<H", data, central + 8, central_flags | 0x1)
            struct.pack_into("<H", data, local + 6, local_flags | 0x1)
        else:
            # BZIP2 is a valid ZIP method, but the release contract permits
            # only stored or deflated members.
            struct.pack_into("<H", data, central + 10, zipfile.ZIP_BZIP2)
            struct.pack_into("<H", data, local + 8, zipfile.ZIP_BZIP2)
    elif scenario == "crc":
        with zipfile.ZipFile(archive) as package:
            target = package.getinfo(root + "/bin/runtime")
            local = target.header_offset
        name_length, extra_length = struct.unpack_from("<HH", data, local + 26)
        payload = local + 30 + name_length + extra_length
        data[payload + target.compress_size // 2] ^= 0x40
    elif scenario == "truncated":
        del data[-12:]
    stream.seek(0)
    stream.write(data)
    stream.truncate()
PY
}

assert_native_release_archive_extractor() {
  local fixture_root="$TEST_ROOT/archive-extractor"
  local source_root_name="Switchyard-Wine-Runtime-fixture-native-arm64"
  local source_runtime="$fixture_root/$source_root_name"
  local archive="$fixture_root/runtime.zip"
  local extraction_container="$fixture_root/extracted"
  local archive_sha256
  local archive_size
  local permission
  local permission_label
  local scenario
  local status

  /bin/mkdir -m 700 "$fixture_root"
  /bin/mkdir -m 755 "$source_runtime"
  /bin/mkdir -m 755 "$source_runtime/bin"
  /usr/bin/printf 'runtime payload\n' >"$source_runtime/bin/runtime"
  /bin/chmod 0755 "$source_runtime/bin/runtime"
  /bin/ln -s runtime "$source_runtime/bin/launcher"
  /bin/ln -s 'safe-target!' "$source_runtime/bin/escape-link"

  (
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_create_native_release_archive "$source_runtime" "$archive"
  ) || fail "cannot create the valid native release archive fixture"
  archive_sha256="$(/usr/bin/shasum -a 256 "$archive" | /usr/bin/awk '{print $1}')"
  archive_size="$(/usr/bin/stat -f '%z' "$archive")"
  /bin/mkdir -m 700 "$extraction_container"
  (
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_extract_native_release_archive \
      "$archive" "$source_runtime" "$extraction_container" \
      "$source_root_name" "$archive_sha256" "$archive_size"
  ) || fail "native release extractor rejected its exact producer archive"
  [ "$(/usr/bin/stat -f '%Lp' "$extraction_container")" = 700 ] ||
    fail "native release extraction container is not private"
  if ! { [ -f "$extraction_container/$source_root_name/bin/runtime" ] &&
         [ "$(/usr/bin/stat -f '%Lp' "$extraction_container/$source_root_name/bin/runtime")" = 755 ] &&
         [ "$(/usr/bin/readlink "$extraction_container/$source_root_name/bin/launcher")" = runtime ] &&
         /usr/bin/cmp -s \
           "$source_runtime/bin/runtime" \
           "$extraction_container/$source_root_name/bin/runtime"; }; then
    fail "native release extractor did not reproduce the exact runtime closure"
  fi

  /bin/ln \
    "$source_runtime/bin/runtime" "$source_runtime/bin/runtime-hardlink"
  extraction_container="$fixture_root/hardlink-extracted"
  /bin/mkdir -m 700 "$extraction_container"
  set +e
  (
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_extract_native_release_archive \
      "$archive" "$source_runtime" "$extraction_container" \
      "$source_root_name" "$archive_sha256" "$archive_size"
  ) >"$fixture_root/hardlink.stdout" 2>"$fixture_root/hardlink.stderr"
  status=$?
  set -e
  /bin/rm -f "$source_runtime/bin/runtime-hardlink"
  [ "$status" -ne 0 ] &&
    /usr/bin/grep -F 'hard-linked file' \
      "$fixture_root/hardlink.stderr" >/dev/null ||
    fail "native release extractor did not reject a staging hard link directly"

  # /private/tmp inherits group wheel on this host; use the caller's primary
  # group so macOS can represent a setgid fixture instead of silently clearing it.
  /usr/bin/chgrp "$(/usr/bin/id -g)" "$source_runtime/bin/runtime"
  for permission_label in setuid setgid sticky; do
    case "$permission_label" in
      setuid) permission=4755 ;;
      setgid) permission=2755 ;;
      sticky) permission=1755 ;;
    esac
    /bin/chmod "$permission" "$source_runtime/bin/runtime"
    extraction_container="$fixture_root/$permission_label-extracted"
    /bin/mkdir -m 700 "$extraction_container"
    set +e
    (
      # shellcheck disable=SC1090 # Fixed worktree release script.
      source "$RELEASE_SCRIPT"
      switchyard_extract_native_release_archive \
        "$archive" "$source_runtime" "$extraction_container" \
        "$source_root_name" "$archive_sha256" "$archive_size"
    ) >"$fixture_root/$permission_label.stdout" \
      2>"$fixture_root/$permission_label.stderr"
    status=$?
    set -e
    /bin/chmod 0755 "$source_runtime/bin/runtime"
    [ "$status" -ne 0 ] &&
      /usr/bin/grep -F 'unsafe permission bits' \
        "$fixture_root/$permission_label.stderr" >/dev/null ||
      fail "native release extractor did not reject staging $permission_label directly"
  done

  for scenario in \
    extra-root absolute traversal duplicate casefold nfc escaping-symlink \
    unsupported-type mode oversize encrypted unsupported-compression crc truncated; do
    archive="$fixture_root/$scenario.zip"
    extraction_container="$fixture_root/$scenario-extracted"
    make_invalid_native_release_archive \
      "$archive" "$source_root_name" "$scenario" "$fixture_root/runtime.zip"
    archive_sha256="$(/usr/bin/shasum -a 256 "$archive" | /usr/bin/awk '{print $1}')"
    archive_size="$(/usr/bin/stat -f '%z' "$archive")"
    /bin/mkdir -m 700 "$extraction_container"
    set +e
    (
      # shellcheck disable=SC1090 # Fixed worktree release script.
      source "$RELEASE_SCRIPT"
      switchyard_extract_native_release_archive \
        "$archive" "$source_runtime" "$extraction_container" \
        "$source_root_name" "$archive_sha256" "$archive_size"
    ) >"$fixture_root/$scenario.stdout" 2>"$fixture_root/$scenario.stderr"
    status=$?
    set -e
    [ "$status" -ne 0 ] ||
      fail "native release extractor accepted the $scenario archive fixture"
    case "$scenario" in
      extra-root)
        /usr/bin/grep -F 'exactly the expected root' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "extra-root fixture did not reach the one-root policy"
        ;;
      duplicate)
        /usr/bin/grep -F 'duplicate member' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "duplicate fixture did not reach the duplicate policy"
        ;;
      casefold)
        /usr/bin/grep -F 'casefold or Unicode path collision' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "casefold fixture did not reach the collision policy"
        ;;
      nfc)
        /usr/bin/grep -F 'non-canonical path component' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "NFC fixture did not reach the Unicode canonicalization policy"
        ;;
      escaping-symlink)
        /usr/bin/grep -F 'symbolic link escapes the runtime root' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "symlink fixture did not reach the escape policy"
        ;;
      encrypted)
        /usr/bin/grep -F 'encrypted member' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "encrypted ZIP fixture did not reach the encryption policy"
        ;;
      unsupported-compression)
        /usr/bin/grep -F 'unsupported compression method' \
          "$fixture_root/$scenario.stderr" >/dev/null ||
          fail "compression fixture did not reach the compression policy"
        ;;
    esac
    if /usr/bin/find "$extraction_container" -mindepth 1 -print -quit |
       /usr/bin/grep -q .; then
      fail "native release extractor left output from the $scenario archive fixture"
    fi
  done

  /bin/mkdir -m 700 "$fixture_root/pin-extracted"
  set +e
  (
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_extract_native_release_archive \
      "$fixture_root/runtime.zip" "$source_runtime" \
      "$fixture_root/pin-extracted" "$source_root_name" \
      "0000000000000000000000000000000000000000000000000000000000000000" \
      "$(/usr/bin/stat -f '%z' "$fixture_root/runtime.zip")"
  ) >/dev/null 2>"$fixture_root/pin.stderr"
  status=$?
  set -e
  [ "$status" -ne 0 ] ||
    fail "native release extractor accepted the wrong pinned archive digest"

  /bin/mkdir -m 700 "$fixture_root/size-pin-extracted"
  archive_sha256="$(/usr/bin/shasum -a 256 "$fixture_root/runtime.zip" |
    /usr/bin/awk '{print $1}')"
  archive_size="$(/usr/bin/stat -f '%z' "$fixture_root/runtime.zip")"
  set +e
  (
    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    switchyard_extract_native_release_archive \
      "$fixture_root/runtime.zip" "$source_runtime" \
      "$fixture_root/size-pin-extracted" "$source_root_name" \
      "$archive_sha256" "$((archive_size + 1))"
  ) >/dev/null 2>"$fixture_root/size-pin.stderr"
  status=$?
  set -e
  [ "$status" -ne 0 ] ||
    fail "native release extractor accepted the wrong pinned archive size"
}

run_case() {
  local label="$1"
  local failure_mode="$2"
  local expected_status="$3"
  local runtime="$TEST_ROOT/$label-runtime"
  local output="$TEST_ROOT/$label-output"
  local case_root="$TEST_ROOT/$label-case"
  local log="$case_root/order.log"
  local sign_count="$case_root/sign-count"
  local fake_codesign="$case_root/fake-codesign"
  local expected_digest
  local -a release_arguments
  local status

  /bin/mkdir -m 700 "$case_root"
  if [ "$failure_mode" = permissive-umask ]; then
    /bin/mkdir -m 700 "$output"
  fi
  make_runtime "$runtime" "$([ "$failure_mode" = unknown-source ] && printf 1 || printf 0)"
  if [ "$failure_mode" = missing-entry ]; then
    /usr/bin/printf 'not a Mach-O entry\n' >"$runtime/bin/wine.switchyard-real"
  fi
  case "$failure_mode" in
    extracted-x86-only)
      /bin/cp "$X86_64_MACHO_FIXTURE" \
        "$runtime/lib/wine/aarch64-unix/helper.dylib"
      /usr/bin/python3 -I "$DIGEST_HELPER" write "$runtime" >/dev/null
      ;;
    extracted-gstreamer-arm64-only)
      /bin/cp "$NATIVE_MACHO_FIXTURE" \
        "$runtime/lib/switchyard-gstreamer/libfixture.dylib"
      /usr/bin/python3 -I "$DIGEST_HELPER" write "$runtime" >/dev/null
      ;;
  esac
  expected_digest="$(/usr/bin/python3 -I "$DIGEST_HELPER" digest "$runtime")"
  : >"$log"
  /usr/bin/printf '0\n' >"$sign_count"
  make_fake_codesign "$fake_codesign"

  set +e
  (
    if [ "$failure_mode" = permissive-umask ]; then
      umask 000
    fi
    export HOME="$case_root/home"
    /bin/mkdir -m 700 "$HOME"
    export SWITCHYARD_TEST_ORDER_LOG="$log"
    export SWITCHYARD_TEST_SIGN_COUNT="$sign_count"
    export SWITCHYARD_TEST_NATIVE_ENTITLEMENTS="$NATIVE_ENTITLEMENTS"
    if [ "$failure_mode" = signing ]; then
      export SWITCHYARD_TEST_FAIL_SIGN_AT=2
    fi
    if [ "$failure_mode" = signing-wrong-team ]; then
      export SWITCHYARD_TEST_TEAM_ID=AAAAAAAAAA
    fi

    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    CODESIGN_TOOL="$fake_codesign"

    eval "$(declare -f switchyard_create_native_release_archive |
      /usr/bin/sed '1s/switchyard_create_native_release_archive/switchyard_create_native_release_archive_original/')"
    eval "$(declare -f switchyard_pin_native_release_archive |
      /usr/bin/sed '1s/switchyard_pin_native_release_archive/switchyard_pin_native_release_archive_original/')"

    switchyard_publish_native_release_file() {
      publish_call_count=$((publish_call_count + 1))
      if [ "$failure_mode" = post-final-pin-mutation ] &&
         [ "$publish_call_count" -eq 1 ]; then
        /usr/bin/printf 'post-pin mutation\n' >>"$1"
      fi
      if [ "$failure_mode" = post-final-pin-path-swap ] &&
         [ "$publish_call_count" -eq 1 ]; then
        /bin/mv "$1" "$1.pinned"
        /usr/bin/printf 'replacement archive\n' >"$1"
        /bin/chmod 0644 "$1"
      fi
      if [ "$failure_mode" = partial-publication ] &&
         [ "$publish_call_count" -eq 2 ]; then
        echo "publication fixture failure" >&2
        return 84
      fi
      switchyard_publish_native_release_file_atomically "$@"
    }
    switchyard_submit_native_release_notary() {
      [ "$failure_mode" = malformed-notary ] || return 85
      /usr/bin/printf '%s\n' \
        '{"status":"Accepted","id":"malformed submission id"}' >"$3"
    }

    switchyard_require_runtime_profile_enabled() { return 0; }
    switchyard_validate_runtime_manifest_profile() {
      profile_call_count=$((profile_call_count + 1))
      /usr/bin/printf 'profile:%s\n' "$profile_call_count" >>"$SWITCHYARD_TEST_ORDER_LOG"
      if /usr/bin/grep -F 'unexpectedReleaseField' "$1" >/dev/null; then
        echo "runtime manifest has an unexpected field" >&2
        return 1
      fi
    }
    switchyard_validate_native_arm64_runtime_packaging() {
      packaging_call_count=$((packaging_call_count + 1))
      /usr/bin/printf 'packaging:%s\n' "$packaging_call_count" \
        >>"$SWITCHYARD_TEST_ORDER_LOG"
      if [ "$failure_mode" = post-smoke-validator ] &&
         [ "$packaging_call_count" -eq 3 ]; then
        echo "post-smoke validator fixture failure" >&2
        return 1
      fi
    }
    switchyard_validate_native_release_runtime() {
      native_validation_call_count=$((native_validation_call_count + 1))
      /usr/bin/printf 'validate:%s\n' "$native_validation_call_count" \
        >>"$SWITCHYARD_TEST_ORDER_LOG"
      switchyard_validate_runtime_manifest_profile \
        "$2" preview-native-arm64-fex "$1" &&
        switchyard_validate_native_arm64_runtime_packaging \
          "$1" "$2" "$ROOT_DIR"
    }
    switchyard_refresh_native_arm64_signed_runtime_manifest() {
      /usr/bin/printf 'refresh\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      if [ "$failure_mode" = refresh-unknown ]; then
        /usr/bin/printf 'tainted\n' \
          >"$1/.switchyard-signed-manifest-refresh-in-progress"
        echo "native signed-manifest refresh rejected an unknown field" >&2
        return 1
      fi
    }
    switchyard_run_native_release_smoke() {
      /usr/bin/printf 'smoke\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      switchyard_validate_prepared_runtime_prefix \
        preview-native-arm64-fex "$2" prepared_prefix
      [ "$prepared_prefix" = "$2" ] || return 1
      [ "$failure_mode" != smoke ] || return 73
    }
    switchyard_stop_native_release_wineserver() {
      /usr/bin/printf 'stop-wait\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      return 0
    }
    switchyard_publish_native_outer_digest() {
      /usr/bin/printf 'outer-digest\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      /usr/bin/python3 -I "$DIGEST_HELPER" write "$1" >/dev/null
      if [ "$failure_mode" = post-outer-dependency-mutation ]; then
        /usr/bin/printf 'stale dependency marker\n' \
          >"$1/lib/switchyard-gstreamer/.switchyard-content-sha256"
      fi
      if [ "$failure_mode" = post-outer-coherent-mutation ]; then
        /usr/bin/printf 'coherent post-validation mutation\n' \
          >>"$1/share/doc/switchyard-wine/LICENSE"
        /usr/bin/python3 -I "$DIGEST_HELPER" write "$1" >/dev/null
      fi
    }
    switchyard_create_native_release_archive() {
      /usr/bin/printf 'archive\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      switchyard_create_native_release_archive_original "$@"
    }
    switchyard_pin_native_release_archive() {
      /usr/bin/printf 'pin\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      switchyard_pin_native_release_archive_original "$@"
    }
    switchyard_extract_native_release_archive() {
      local actual_sha256
      local actual_size
      local extracted_runtime

      /usr/bin/printf 'extract\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      [ "$#" -eq 6 ] || return 90
      actual_sha256="$(/usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}')"
      actual_size="$(/usr/bin/stat -f '%z' "$1")"
      [ "$actual_sha256" = "$5" ] && [ "$actual_size" = "$6" ] || return 91
      [ "$(/usr/bin/stat -f '%Lp' "$3")" = 700 ] || return 92
      [ "$failure_mode" != archive-extract ] || return 74
      extracted_runtime="$3/$4"
      /usr/bin/ditto "$2" "$extracted_runtime"
      export SWITCHYARD_TEST_EXTRACTED_RUNTIME="$extracted_runtime"
      case "$failure_mode" in
        extracted-outer-tamper)
          /usr/bin/printf 'tampered after ZIP extraction\n' \
            >>"$extracted_runtime/share/doc/switchyard-wine/LICENSE"
          ;;
        portable-install-prefix)
          /usr/bin/plutil -replace installPrefix -string '../escape' \
            "$extracted_runtime/switchyard-runtime.json"
          /usr/bin/python3 -I "$DIGEST_HELPER" write "$extracted_runtime" >/dev/null
          ;;
        portable-executable)
          /usr/bin/plutil -replace executable -string '../escape/wine' \
            "$extracted_runtime/switchyard-runtime.json"
          /usr/bin/python3 -I "$DIGEST_HELPER" write "$extracted_runtime" >/dev/null
          ;;
      esac
    }
    profile_call_count=0
    packaging_call_count=0
    native_validation_call_count=0
    publish_call_count=0
    if [ "$failure_mode" = extracted-wrong-team ]; then
      export SWITCHYARD_TEST_EXTRACTED_TEAM_ID=AAAAAAAAAA
    fi
    if [ "$failure_mode" = extracted-unexpected-entitlement ]; then
      export SWITCHYARD_TEST_EXTRACTED_UNEXPECTED_ENTITLEMENT=1
    fi
    release_arguments=(
      --runtime "$runtime" \
      --runtime-content-sha256 "$expected_digest" \
      --output "$output" \
      --identity 'Developer ID Application: Fixture (M3CULMDKU3)'
    )
    if [ "$failure_mode" = malformed-notary ]; then
      release_arguments+=(--notary-profile fixture-notary-profile)
    fi
    release_main "${release_arguments[@]}"
  ) >"$case_root/stdout" 2>"$case_root/stderr"
  status=$?
  set -e

  [ "$status" -eq "$expected_status" ] || {
    /bin/cat "$case_root/stdout" >&2
    /bin/cat "$case_root/stderr" >&2
    fail "$label returned $status instead of $expected_status"
  }
  if [ "$expected_status" -ne 0 ]; then
    assert_no_publication "$output"
  fi
}

[ -f "$RELEASE_SCRIPT" ] && [ ! -L "$RELEASE_SCRIPT" ] ||
  fail "release script is missing or unsafe"
[ "$(/usr/bin/grep -c 'signed_runtime/bin/wineserver' "$RELEASE_SCRIPT")" -eq 2 ] ||
  fail "stable release wineserver behavior changed"
/usr/bin/grep -F 'WINEDEBUG=-all "$signed_runtime/bin/switchyard-wine" cmd /c ver' \
  "$RELEASE_SCRIPT" >/dev/null || fail "stable fresh-prefix smoke changed"
/usr/bin/grep -F '/usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp' \
  "$RELEASE_SCRIPT" >/dev/null || fail "stable Developer-ID/Hardened signing changed"
/usr/bin/grep -F '/bin/chmod 0644 "$archive"' "$RELEASE_SCRIPT" >/dev/null &&
  /usr/bin/grep -F '/bin/chmod 0644 "$checksum_file"' "$RELEASE_SCRIPT" >/dev/null &&
  /usr/bin/grep -F '/bin/chmod 0644 "$release_manifest"' "$RELEASE_SCRIPT" >/dev/null ||
  fail "stable release output mode normalization changed"
assert_native_release_source_contract || fail "native release source ordering contract changed"
assert_native_release_stop_is_idempotent
assert_native_release_cleanup_preserves_foreign_replacement
assert_native_release_archive_extractor

run_case success none 0
success_output="$TEST_ROOT/success-output"
success_log="$TEST_ROOT/success-case/order.log"
archive="$(/usr/bin/find "$success_output" -maxdepth 1 -type f -name '*.zip' -print)"
checksum="$(/usr/bin/find "$success_output" -maxdepth 1 -type f -name '*.zip.sha256' -print)"
release_manifest="$success_output/switchyard-runtime-release.json"
[ -f "$archive" ] && [ -f "$checksum" ] && [ -f "$release_manifest" ] ||
  fail "successful native release did not publish its exact output set"
[ "$(/usr/bin/find "$success_output" -mindepth 1 -maxdepth 1 -print | /usr/bin/wc -l | tr -d ' ')" -eq 3 ] ||
  fail "successful native release published unexpected output"
[ "$(/usr/bin/stat -f '%Lp' "$archive")" = 644 ] &&
  [ "$(/usr/bin/stat -f '%Lp' "$checksum")" = 644 ] &&
  [ "$(/usr/bin/stat -f '%Lp' "$release_manifest")" = 644 ] ||
  fail "successful native release output modes are not exact"
(
  cd "$success_output"
  /usr/bin/shasum -a 256 -c "$(basename "$checksum")" >/dev/null
) || fail "native release checksum does not verify"
/usr/bin/python3 -I - "$release_manifest" <<'PY' || fail "release metadata is invalid"
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
if value["runtimeProfile"] != "preview-native-arm64-fex":
    raise SystemExit("wrong runtime profile")
if value["hostArchitecture"] != "arm64" or value["requiresRosetta"] is not False:
    raise SystemExit("wrong native host policy")
if value["developerTeamID"] != "M3CULMDKU3":
    raise SystemExit("wrong native Developer Team ID")
if value["notarizationStatus"] != "not-submitted" or value["notarizationID"] != "":
    raise SystemExit("absent notary profile is not represented exactly")
text = json.dumps(value, ensure_ascii=False)
if any("\uac00" <= character <= "\ud7a3" for character in text):
    raise SystemExit("Korean release text")
PY

[ "$(/usr/bin/grep -c '^sign:' "$success_log")" -eq 5 ] ||
  fail "native release did not sign every Mach-O exactly once"
[ "$(/usr/bin/grep -c '^sign:entry$' "$success_log")" -eq 2 ] ||
  fail "native release did not attach entitlements to exactly two entries"
last_sign="$(/usr/bin/awk '/^sign:/ { line=NR } END { print line }' "$success_log")"
refresh_line="$(event_line refresh "$success_log")"
first_validation_line="$(event_line validate:1 "$success_log")"
digest_line="$(event_line outer-digest "$success_log")"
archive_line="$(event_line archive "$success_log")"
pin_line="$(event_line pin "$success_log")"
second_pin_line="$(/usr/bin/awk '$0 == "pin" && ++count == 2 { print NR; exit }' "$success_log")"
third_pin_line="$(/usr/bin/awk '$0 == "pin" && ++count == 3 { print NR; exit }' "$success_log")"
extract_line="$(event_line extract "$success_log")"
second_validation_line="$(event_line validate:2 "$success_log")"
smoke_line="$(event_line smoke "$success_log")"
[ "$last_sign" -lt "$refresh_line" ] &&
  [ "$refresh_line" -lt "$first_validation_line" ] &&
  [ "$first_validation_line" -lt "$digest_line" ] &&
  [ "$digest_line" -lt "$archive_line" ] &&
  [ "$archive_line" -lt "$pin_line" ] &&
  [ "$pin_line" -lt "$extract_line" ] &&
  [ "$extract_line" -lt "$second_validation_line" ] &&
  [ "$second_validation_line" -lt "$smoke_line" ] &&
  [ "$smoke_line" -lt "$second_pin_line" ] &&
  [ "$second_pin_line" -lt "$third_pin_line" ] ||
  fail "native release ordering contract changed"

run_case signing-failure signing 1
run_case signing-wrong-team signing-wrong-team 1
run_case missing-entry missing-entry 1
run_case refresh-unknown-field refresh-unknown 1
run_case archive-extract-failure archive-extract 1
run_case extracted-outer-tamper extracted-outer-tamper 1
run_case portable-install-prefix portable-install-prefix 1
run_case portable-executable portable-executable 1
run_case extracted-wrong-team extracted-wrong-team 1
run_case extracted-unexpected-entitlement extracted-unexpected-entitlement 1
run_case extracted-x86-only extracted-x86-only 1
run_case extracted-gstreamer-arm64-only extracted-gstreamer-arm64-only 1
/usr/bin/grep -F \
  'Mach-O architecture set is not exact: lib/switchyard-gstreamer/libfixture.dylib' \
  "$TEST_ROOT/extracted-gstreamer-arm64-only-case/stderr" >/dev/null ||
  fail "GStreamer architecture fixture did not reach the universal-image policy"
run_case extracted-smoke-failure smoke 73
run_case validator-failure post-smoke-validator 1
run_case post-outer-dependency-mutation post-outer-dependency-mutation 1
if /usr/bin/grep -Fqx archive \
    "$TEST_ROOT/post-outer-dependency-mutation-case/order.log"; then
  fail "native release archived a runtime changed after its outer marker"
fi
/usr/bin/grep -F 'native release signed staging changed before archive creation' \
  "$TEST_ROOT/post-outer-dependency-mutation-case/stderr" >/dev/null ||
  fail "post-outer dependency mutation did not reach the final digest gate"
run_case post-outer-coherent-mutation post-outer-coherent-mutation 1
if /usr/bin/grep -Fqx archive \
    "$TEST_ROOT/post-outer-coherent-mutation-case/order.log"; then
  fail "native release archived a coherently mutated signed staging tree"
fi
/usr/bin/grep -F 'native release signed staging changed before archive creation' \
  "$TEST_ROOT/post-outer-coherent-mutation-case/stderr" >/dev/null ||
  fail "coherent post-validation mutation did not reach the pinned digest gate"
run_case post-final-pin-mutation post-final-pin-mutation 1
run_case post-final-pin-path-swap post-final-pin-path-swap 1
run_case permissive-umask permissive-umask 0
for published in "$TEST_ROOT/permissive-umask-output"/*; do
  [ -f "$published" ] && [ ! -L "$published" ] &&
    [ "$(/usr/bin/stat -f '%Lp' "$published")" = 644 ] ||
    fail "permissive umask changed a published release artifact mode"
done
run_case source-unknown-field unknown-source 1
run_case partial-publication partial-publication 1
run_case malformed-notary malformed-notary 1

SWITCHYARD_DEVELOPER_TEAM_ID='not-a-team-id' \
  run_case malformed-team-id none 2

echo "release runtime tests passed"
