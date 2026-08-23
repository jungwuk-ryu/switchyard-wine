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

  /bin/cp /bin/echo "$runtime/lib/wine/aarch64-unix/wine"
  /bin/cp /bin/echo "$runtime/bin/wine.switchyard-real"
  /bin/cp /bin/echo "$runtime/bin/wineserver"
  /bin/cp /bin/echo "$runtime/lib/wine/aarch64-unix/helper.dylib"
  /bin/chmod 0755 \
    "$runtime/lib/wine/aarch64-unix/wine" \
    "$runtime/bin/wine.switchyard-real" \
    "$runtime/bin/wineserver" \
    "$runtime/lib/wine/aarch64-unix/helper.dylib"
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
+    /usr/bin/printf '%s\n' \
+      'CodeDirectory v=20500 size=1 flags=0x10000(runtime) hashes=1+0 location=embedded' \
+      'Signature size=9000' \
+      'Authority=Developer ID Application: Fixture (M3CULMDKU3)' \
+      'TeamIdentifier=M3CULMDKU3' \
+      'Runtime Version=26.0.0'
+    ;;
+  entitlements)
+    /bin/cat "$SWITCHYARD_TEST_NATIVE_ENTITLEMENTS"
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

event_line() {
  local event="$1"
  local log="$2"

  /usr/bin/awk -v wanted="$event" '$0 == wanted { print NR; exit }' "$log"
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
  make_runtime "$runtime" "$([ "$failure_mode" = unknown-source ] && printf 1 || printf 0)"
  if [ "$failure_mode" = missing-entry ]; then
    /usr/bin/printf 'not a Mach-O entry\n' >"$runtime/bin/wine.switchyard-real"
  fi
  expected_digest="$(/usr/bin/python3 -I "$DIGEST_HELPER" digest "$runtime")"
  : >"$log"
  /usr/bin/printf '0\n' >"$sign_count"
  make_fake_codesign "$fake_codesign"

  set +e
  (
    export HOME="$case_root/home"
    /bin/mkdir -m 700 "$HOME"
    export SWITCHYARD_TEST_ORDER_LOG="$log"
    export SWITCHYARD_TEST_SIGN_COUNT="$sign_count"
    export SWITCHYARD_TEST_NATIVE_ENTITLEMENTS="$NATIVE_ENTITLEMENTS"
    if [ "$failure_mode" = signing ]; then
      export SWITCHYARD_TEST_FAIL_SIGN_AT=2
    fi

    # shellcheck disable=SC1090 # Fixed worktree release script.
    source "$RELEASE_SCRIPT"
    CODESIGN_TOOL="$fake_codesign"

    switchyard_publish_native_release_file() {
      publish_call_count=$((publish_call_count + 1))
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
    }
    switchyard_create_native_release_archive() {
      /usr/bin/printf 'archive\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$1" "$2"
    }

    profile_call_count=0
    packaging_call_count=0
    publish_call_count=0
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
text = json.dumps(value, ensure_ascii=False)
if any("\uac00" <= character <= "\ud7a3" for character in text):
    raise SystemExit("Korean release text")
PY

[ "$(/usr/bin/grep -c '^sign:' "$success_log")" -eq 4 ] ||
  fail "native release did not sign every Mach-O exactly once"
[ "$(/usr/bin/grep -c '^sign:entry$' "$success_log")" -eq 2 ] ||
  fail "native release did not attach entitlements to exactly two entries"
last_sign="$(/usr/bin/awk '/^sign:/ { line=NR } END { print line }' "$success_log")"
refresh_line="$(event_line refresh "$success_log")"
smoke_line="$(event_line smoke "$success_log")"
digest_line="$(event_line outer-digest "$success_log")"
archive_line="$(event_line archive "$success_log")"
[ "$last_sign" -lt "$refresh_line" ] &&
  [ "$(event_line profile:2 "$success_log")" -gt "$refresh_line" ] &&
  [ "$(event_line packaging:2 "$success_log")" -lt "$smoke_line" ] &&
  [ "$(event_line packaging:3 "$success_log")" -gt "$smoke_line" ] &&
  [ "$digest_line" -gt "$(event_line packaging:3 "$success_log")" ] &&
  [ "$archive_line" -gt "$digest_line" ] ||
  fail "native release ordering contract changed"

run_case signing-failure signing 1
run_case missing-entry missing-entry 1
run_case refresh-unknown-field refresh-unknown 1
run_case smoke-failure smoke 73
run_case validator-failure post-smoke-validator 1
run_case source-unknown-field unknown-source 1
run_case partial-publication partial-publication 1
run_case malformed-notary malformed-notary 1

SWITCHYARD_DEVELOPER_TEAM_ID='not-a-team-id' \
  run_case malformed-team-id none 2

echo "release runtime tests passed"
