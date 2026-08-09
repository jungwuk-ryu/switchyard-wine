#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER="$ROOT_DIR/switchyard/native_arm64_host_probe.sh"
SOURCE="$ROOT_DIR/switchyard/native_arm64_host_probe.c"
ENTITLEMENTS="$ROOT_DIR/switchyard/native_arm64_host_probe.entitlements"
TEST_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-native-arm64-probe-test.XXXXXX)"
failure_index=0

# shellcheck disable=SC2329 # Invoked through the EXIT and signal traps.
cleanup()
{
    case "$TEST_ROOT" in
        /tmp/switchyard-native-arm64-probe-test.??????)
            if [ -d "$TEST_ROOT" ] && [ ! -L "$TEST_ROOT" ]; then
                /bin/rm -rf -- "$TEST_ROOT"
            else
                echo "refusing to clean an unsafe native ARM64 test path: $TEST_ROOT" >&2
            fi
            ;;
        *) echo "refusing to clean an unexpected native ARM64 test path: $TEST_ROOT" >&2 ;;
    esac
}
trap cleanup EXIT HUP INT TERM

fail()
{
    echo "$1" >&2
    exit 1
}

run_status()
{
    local output="$1"
    local error="$2"
    shift 2

    set +e
    "$@" >"$output" 2>"$error"
    RUN_STATUS=$?
    set -e
}

expect_status()
{
    local expected="$1"
    local message="$2"
    shift 2

    failure_index=$((failure_index + 1))
    run_status "$TEST_ROOT/status-$failure_index.out" \
               "$TEST_ROOT/status-$failure_index.err" "$@"
    if [ "$RUN_STATUS" -ne "$expected" ]; then
        /bin/cat "$TEST_ROOT/status-$failure_index.out" >&2
        /bin/cat "$TEST_ROOT/status-$failure_index.err" >&2
        fail "$message returned $RUN_STATUS instead of $expected"
    fi
}

/bin/bash -n "$DRIVER"
/bin/bash -n "$0"
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "this test requires macOS"
[ "$(/usr/bin/uname -m)" = arm64 ] || fail "this test requires an arm64 process"

/usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror -Wno-inline-asm \
    "$SOURCE" -o "$TEST_ROOT/native-arm64-host-probe"

helper_output="$TEST_ROOT/helper.out"
helper_error="$TEST_ROOT/helper.err"
"$TEST_ROOT/native-arm64-host-probe" >"$helper_output" 2>"$helper_error"
for key in kuserSharedDataAddress hostPageSize \
           kuserSharedDataReservationSucceeded kuserSharedDataMmapAttempted \
           kuserSharedDataMappable kuserSharedDataCleanupSucceeded \
           customX18ApiCompiled customX18ApiAvailable \
           customX18EntitlementRequired customX18EntitlementName \
           customX18EntitlementState customX18PreemptionObserved \
           customX18Preserved; do
    [ "$(/usr/bin/grep -c "^${key}=" "$helper_output")" -eq 1 ] ||
        fail "helper did not report $key exactly once"
done
/usr/bin/grep -Fx 'kuserSharedDataAddress=0x000000007ffe0000' "$helper_output" >/dev/null ||
    fail "helper did not probe the exact Windows KUSER_SHARED_DATA address"
/usr/bin/grep -Fx 'customX18EntitlementState=not-effective' "$helper_output" >/dev/null ||
    fail "unsigned helper unexpectedly observed an effective x18 entitlement"
/usr/bin/grep -Fx 'customX18EntitlementRequired=true' "$helper_output" >/dev/null ||
    fail "helper did not report the public x18 entitlement requirement"
/usr/bin/grep -Fx \
    'customX18EntitlementName=com.apple.security.custom-x18-abi-toggle' \
    "$helper_output" >/dev/null || fail "helper reported the wrong x18 entitlement"
/usr/bin/grep -Fx 'customX18Preserved=false' "$helper_output" >/dev/null ||
    fail "unsigned helper incorrectly claimed scheduler-preemption preservation of x18"
[ "$(/usr/bin/plutil -convert json -o - "$ENTITLEMENTS")" = \
  '{"com.apple.security.custom-x18-abi-toggle":true}' ] ||
    fail "probe entitlement file contains an unexpected capability"
expect_status 2 "helper argument validation" \
    "$TEST_ROOT/native-arm64-host-probe" unexpected

if /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
    non_arm64_output="$TEST_ROOT/non-arm64.out"
    non_arm64_error="$TEST_ROOT/non-arm64.err"
    run_status "$non_arm64_output" "$non_arm64_error" \
        /usr/bin/arch -x86_64 /bin/bash "$DRIVER" --probe --minimum-macos 26.5
    [ "$RUN_STATUS" -eq 0 ] || fail "non-arm64 probe mode returned $RUN_STATUS"
    /usr/bin/grep -Fx 'customX18PreemptionObserved=not-probed' \
        "$non_arm64_output" >/dev/null ||
        fail "non-arm64 probe omitted the x18 preemption field"
    /usr/bin/grep -Fx 'nativeArm64HostSupported=false' "$non_arm64_output" >/dev/null ||
        fail "non-arm64 probe did not fail the aggregate capability gate"
fi

probe_output="$TEST_ROOT/probe.out"
probe_error="$TEST_ROOT/probe.err"
run_status "$probe_output" "$probe_error" \
    /bin/bash "$DRIVER" --probe --minimum-macos 26.5
[ "$RUN_STATUS" -eq 0 ] || {
    /bin/cat "$probe_error" >&2
    fail "non-fatal probe mode returned $RUN_STATUS"
}
/usr/bin/grep -Fx 'hostArchitectureSupported=true' "$probe_output" >/dev/null ||
    fail "driver did not recognize the native arm64 host"
/usr/bin/grep -Fx 'macOSFloorSupported=true' "$probe_output" >/dev/null ||
    fail "driver did not accept the qualified macOS floor"
/usr/bin/grep -Fx 'customX18SignatureVerified=true' "$probe_output" >/dev/null ||
    fail "driver did not verify the ad-hoc probe signature"
/usr/bin/grep -Fx 'customX18HardenedRuntime=true' "$probe_output" >/dev/null ||
    fail "driver did not verify hardened runtime on the probe"
/usr/bin/grep -Fx 'customX18EntitlementEmbedded=true' "$probe_output" >/dev/null ||
    fail "driver did not verify the embedded public x18 entitlement"
/usr/bin/grep -Fx 'customX18EntitlementState=effective' "$probe_output" >/dev/null ||
    fail "signed public x18 entitlement was not effective"
/usr/bin/grep -Fx 'customX18PreemptionObserved=true' "$probe_output" >/dev/null ||
    fail "signed helper did not observe a scheduler preemption"
/usr/bin/grep -Fx 'customX18Preserved=true' "$probe_output" >/dev/null ||
    fail "signed helper did not preserve x18 across scheduler preemption"

supported=false
if /usr/bin/grep -Fx 'nativeArm64HostSupported=true' "$probe_output" >/dev/null; then
    supported=true
fi

strict_output="$TEST_ROOT/strict.out"
strict_error="$TEST_ROOT/strict.err"
run_status "$strict_output" "$strict_error" \
    /bin/bash "$DRIVER" --strict --minimum-macos 26.5
if [ "$supported" = true ]; then
    [ "$RUN_STATUS" -eq 0 ] || fail "strict mode rejected a supported host"
else
    [ "$RUN_STATUS" -eq 1 ] || fail "strict mode did not reject an unsupported host"
fi

if /usr/bin/grep -Fx 'kuserSharedDataMappable=false' "$probe_output" >/dev/null; then
    echo "Observed expected unsupported low-VA evidence on this host."
    /usr/bin/grep -Fx 'nativeArm64HostSupported=false' "$probe_output" >/dev/null ||
        fail "low-VA rejection did not fail the aggregate capability gate"
fi

future_output="$TEST_ROOT/future.out"
future_error="$TEST_ROOT/future.err"
run_status "$future_output" "$future_error" \
    /bin/bash "$DRIVER" --probe --minimum-macos 9999.0
[ "$RUN_STATUS" -eq 0 ] || fail "probe mode treated an unsupported OS floor as a command failure"
/usr/bin/grep -Fx 'macOSFloorSupported=false' "$future_output" >/dev/null ||
    fail "future minimum OS did not fail the OS-floor capability"
/usr/bin/grep -Fx 'nativeArm64HostSupported=false' "$future_output" >/dev/null ||
    fail "unsupported future OS floor did not fail the aggregate capability"
expect_status 1 "strict unsupported OS floor" \
    /bin/bash "$DRIVER" --strict --minimum-macos 9999.0
expect_status 2 "invalid OS floor" \
    /bin/bash "$DRIVER" --probe --minimum-macos 26.x
expect_status 2 "duplicate mode" \
    /bin/bash "$DRIVER" --probe --strict

echo "Native ARM64 host probe tests passed."
