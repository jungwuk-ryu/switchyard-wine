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
    "archive": call_offsets(release, "switchyard_create_native_release_archive"),
}
expected_counts = {
    "sign": 1,
    "refresh": 1,
    "validate": 2,
    "smoke": 1,
    "outer": 1,
    "tree_verify": 2,
    "archive": 1,
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
    operations["smoke"][0],
    operations["validate"][1],
    operations["outer"][0],
    operations["tree_verify"][1],
    operations["archive"][0],
]
if ordered != sorted(ordered) or len(set(ordered)) != len(ordered):
    raise SystemExit(
        "native release source contract is not sign, refresh, validate, smoke, "
        "validate, outer digest, archive"
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
    }
    switchyard_create_native_release_archive() {
      /usr/bin/printf 'archive\n' >>"$SWITCHYARD_TEST_ORDER_LOG"
      /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$1" "$2"
    }

    profile_call_count=0
    packaging_call_count=0
    native_validation_call_count=0
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
assert_native_release_source_contract || fail "native release source ordering contract changed"
assert_native_release_stop_is_idempotent

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
first_validation_line="$(event_line validate:1 "$success_log")"
smoke_line="$(event_line smoke "$success_log")"
second_validation_line="$(event_line validate:2 "$success_log")"
digest_line="$(event_line outer-digest "$success_log")"
archive_line="$(event_line archive "$success_log")"
[ "$last_sign" -lt "$refresh_line" ] &&
  [ "$refresh_line" -lt "$first_validation_line" ] &&
  [ "$first_validation_line" -lt "$smoke_line" ] &&
  [ "$smoke_line" -lt "$second_validation_line" ] &&
  [ "$second_validation_line" -lt "$digest_line" ] &&
  [ "$archive_line" -gt "$digest_line" ] ||
  fail "native release ordering contract changed"

run_case signing-failure signing 1
run_case missing-entry missing-entry 1
run_case refresh-unknown-field refresh-unknown 1
run_case smoke-failure smoke 73
run_case validator-failure post-smoke-validator 1
run_case post-outer-dependency-mutation post-outer-dependency-mutation 1
if /usr/bin/grep -Fqx archive \
    "$TEST_ROOT/post-outer-dependency-mutation-case/order.log"; then
  fail "native release archived a runtime changed after its outer marker"
fi
/usr/bin/grep -F 'native release outer content digest did not verify' \
  "$TEST_ROOT/post-outer-dependency-mutation-case/stderr" >/dev/null ||
  fail "post-outer dependency mutation did not reach the final digest gate"
run_case source-unknown-field unknown-source 1
run_case partial-publication partial-publication 1
run_case malformed-notary malformed-notary 1

SWITCHYARD_DEVELOPER_TEAM_ID='not-a-team-id' \
  run_case malformed-team-id none 2

echo "release runtime tests passed"
