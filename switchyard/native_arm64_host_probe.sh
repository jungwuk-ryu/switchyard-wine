#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT_DIR/switchyard/native_arm64_host_probe.c"
ENTITLEMENTS="$ROOT_DIR/switchyard/native_arm64_host_probe.entitlements"
MODE=strict
MINIMUM_MACOS=26.5
MODE_SEEN=0
MINIMUM_SEEN=0

usage()
{
    echo "usage: $0 [--strict | --probe] [--minimum-macos VERSION]" >&2
}

fail_usage()
{
    echo "$1" >&2
    usage
    exit 2
}

valid_version()
{
    [[ "$1" =~ ^[0-9]{1,5}([.][0-9]{1,5}){0,2}$ ]]
}

version_at_least()
{
    local actual="$1"
    local required="$2"
    local actual_major actual_minor actual_patch
    local required_major required_minor required_patch

    IFS=. read -r actual_major actual_minor actual_patch <<<"$actual"
    IFS=. read -r required_major required_minor required_patch <<<"$required"
    actual_minor="${actual_minor:-0}"
    actual_patch="${actual_patch:-0}"
    required_minor="${required_minor:-0}"
    required_patch="${required_patch:-0}"

    (( 10#$actual_major > 10#$required_major )) && return 0
    (( 10#$actual_major < 10#$required_major )) && return 1
    (( 10#$actual_minor > 10#$required_minor )) && return 0
    (( 10#$actual_minor < 10#$required_minor )) && return 1
    (( 10#$actual_patch >= 10#$required_patch ))
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --strict|--probe)
            [ "$MODE_SEEN" -eq 0 ] || fail_usage "probe mode may be specified only once"
            MODE="${1#--}"
            MODE_SEEN=1
            shift
            ;;
        --minimum-macos)
            [ "$MINIMUM_SEEN" -eq 0 ] || fail_usage "--minimum-macos may be specified only once"
            [ "$#" -ge 2 ] || fail_usage "--minimum-macos requires a version"
            MINIMUM_MACOS="$2"
            MINIMUM_SEEN=1
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            fail_usage "unknown argument: $1"
            ;;
    esac
done

valid_version "$MINIMUM_MACOS" ||
    fail_usage "invalid minimum macOS version: $MINIMUM_MACOS"

HOST_PLATFORM="$(/usr/bin/uname -s)"
HOST_ARCHITECTURE="$(/usr/bin/uname -m)"
MACOS_VERSION=unavailable
ARCHITECTURE_SUPPORTED=false
OS_FLOOR_SUPPORTED=false
WORK_DIR=

# shellcheck disable=SC2329 # Invoked through the EXIT and signal traps.
cleanup()
{
    case "$WORK_DIR" in
        /tmp/switchyard-native-arm64-probe.??????)
            if [ -d "$WORK_DIR" ] && [ ! -L "$WORK_DIR" ]; then
                /bin/rm -rf -- "$WORK_DIR"
            elif [ -e "$WORK_DIR" ] || [ -L "$WORK_DIR" ]; then
                echo "refusing to clean an unsafe native ARM64 probe path: $WORK_DIR" >&2
            fi
            ;;
        '') ;;
        *) echo "refusing to clean an unexpected native ARM64 probe path: $WORK_DIR" >&2 ;;
    esac
}
trap cleanup EXIT HUP INT TERM

if [ "$HOST_PLATFORM" = Darwin ]; then
    MACOS_VERSION="$(/usr/bin/sw_vers -productVersion)"
fi
if [ "$HOST_PLATFORM" = Darwin ] && [ "$HOST_ARCHITECTURE" = arm64 ]; then
    ARCHITECTURE_SUPPORTED=true
fi
if valid_version "$MACOS_VERSION" && version_at_least "$MACOS_VERSION" "$MINIMUM_MACOS"; then
    OS_FLOOR_SUPPORTED=true
fi

printf 'hostPlatform=%s\n' "$HOST_PLATFORM"
printf 'hostArchitecture=%s\n' "$HOST_ARCHITECTURE"
printf 'hostArchitectureSupported=%s\n' "$ARCHITECTURE_SUPPORTED"
printf 'macOSVersion=%s\n' "$MACOS_VERSION"
printf 'minimumMacOS=%s\n' "$MINIMUM_MACOS"
printf 'macOSFloorSupported=%s\n' "$OS_FLOOR_SUPPORTED"

if [ "$ARCHITECTURE_SUPPORTED" != true ]; then
    echo "native ARM64 Wine requires an arm64 process on an Apple Silicon macOS host; detected $HOST_PLATFORM/$HOST_ARCHITECTURE" >&2
    printf '%s\n' \
        'kuserSharedDataMappable=not-probed' \
        'kuserSharedDataCleanupSucceeded=not-probed' \
        'customX18ApiAvailable=not-probed' \
        'customX18SignatureVerified=not-probed' \
        'customX18HardenedRuntime=not-probed' \
        'customX18EntitlementEmbedded=not-probed' \
        'customX18EntitlementRequired=true' \
        'customX18EntitlementName=com.apple.security.custom-x18-abi-toggle' \
        'customX18EntitlementState=not-requested' \
        'customX18PreemptionObserved=not-probed' \
        'customX18Preserved=not-probed' \
        'customX18PreservationProven=not-probed' \
        'nativeArm64HostSupported=false'
    [ "$MODE" = probe ] && exit 0
    exit 1
fi

if [ "$OS_FLOOR_SUPPORTED" != true ]; then
    echo "native ARM64 Wine requires macOS $MINIMUM_MACOS or newer; detected $MACOS_VERSION" >&2
fi

[ -f "$SOURCE" ] || {
    echo "native ARM64 probe source is missing: $SOURCE" >&2
    exit 2
}
[ -f "$ENTITLEMENTS" ] && [ ! -L "$ENTITLEMENTS" ] || {
    echo "native ARM64 probe entitlements are missing: $ENTITLEMENTS" >&2
    exit 2
}
[ -x /usr/bin/xcrun ] || {
    echo "xcrun is required to build the native ARM64 host probe" >&2
    exit 2
}
[ -x /usr/bin/codesign ] && [ -x /usr/bin/plutil ] || {
    echo "codesign and plutil are required to verify the public custom-x18 entitlement" >&2
    exit 2
}

WORK_DIR="$(/usr/bin/mktemp -d /tmp/switchyard-native-arm64-probe.XXXXXX)"
PROBE_BINARY="$WORK_DIR/native-arm64-host-probe"
PROBE_OUTPUT="$WORK_DIR/probe.out"
PROBE_ERROR="$WORK_DIR/probe.err"
EXPECTED_ENTITLEMENTS='{"com.apple.security.custom-x18-abi-toggle":true}'
if [ "$OS_FLOOR_SUPPORTED" = true ]; then
    PROBE_DEPLOYMENT_TARGET="$MINIMUM_MACOS"
else
    PROBE_DEPLOYMENT_TARGET="$MACOS_VERSION"
fi
if ! version_at_least "$PROBE_DEPLOYMENT_TARGET" 26.4; then
    PROBE_DEPLOYMENT_TARGET=26.4
fi

if ! SOURCE_ENTITLEMENTS_JSON="$(/usr/bin/plutil -convert json -o - "$ENTITLEMENTS")" ||
   [ "$SOURCE_ENTITLEMENTS_JSON" != "$EXPECTED_ENTITLEMENTS" ]; then
    echo "native ARM64 probe entitlements must contain only com.apple.security.custom-x18-abi-toggle=true" >&2
    exit 2
fi

if ! /usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
        -mmacosx-version-min="$PROBE_DEPLOYMENT_TARGET" -Wall -Wextra -Werror \
        -Wno-inline-asm \
        "$SOURCE" -o "$PROBE_BINARY" >"$WORK_DIR/clang.out" 2>"$WORK_DIR/clang.err"; then
    /bin/cat "$WORK_DIR/clang.err" >&2
    echo "failed to build the native ARM64 host probe" >&2
    exit 2
fi

if ! /usr/bin/codesign --force --sign - --options runtime \
        --entitlements "$ENTITLEMENTS" "$PROBE_BINARY" \
        >"$WORK_DIR/codesign.out" 2>"$WORK_DIR/codesign.err"; then
    /bin/cat "$WORK_DIR/codesign.err" >&2
    echo "failed to ad-hoc sign the custom-x18 probe with hardened runtime" >&2
    exit 2
fi
if ! /usr/bin/codesign --verify --strict --verbose=2 "$PROBE_BINARY" \
        >"$WORK_DIR/codesign-verify.out" 2>"$WORK_DIR/codesign-verify.err"; then
    /bin/cat "$WORK_DIR/codesign-verify.err" >&2
    echo "custom-x18 probe signature verification failed" >&2
    exit 2
fi
if ! /usr/bin/codesign -d --verbose=4 "$PROBE_BINARY" \
        >"$WORK_DIR/codesign-details.out" 2>"$WORK_DIR/codesign-details.err"; then
    /bin/cat "$WORK_DIR/codesign-details.err" >&2
    echo "cannot inspect the custom-x18 probe signature" >&2
    exit 2
fi
/usr/bin/grep -E '^CodeDirectory .*flags=.*runtime' \
    "$WORK_DIR/codesign-details.err" >/dev/null || {
    echo "custom-x18 probe signature is missing hardened runtime" >&2
    exit 2
}
/usr/bin/grep -Fx 'Signature=adhoc' "$WORK_DIR/codesign-details.err" >/dev/null || {
    echo "custom-x18 probe does not have the expected ad-hoc signature" >&2
    exit 2
}
if ! /usr/bin/codesign -d --xml --entitlements - "$PROBE_BINARY" \
        >"$WORK_DIR/embedded-entitlements.plist" \
        2>"$WORK_DIR/embedded-entitlements.err"; then
    /bin/cat "$WORK_DIR/embedded-entitlements.err" >&2
    echo "cannot inspect the embedded custom-x18 entitlement" >&2
    exit 2
fi
if ! EMBEDDED_ENTITLEMENTS_JSON="$(/usr/bin/plutil -convert json -o - \
        "$WORK_DIR/embedded-entitlements.plist")" ||
   [ "$EMBEDDED_ENTITLEMENTS_JSON" != "$EXPECTED_ENTITLEMENTS" ]; then
    echo "signed custom-x18 probe does not contain the exact public entitlement allowlist" >&2
    exit 2
fi

printf '%s\n' \
    'customX18SignatureVerified=true' \
    'customX18HardenedRuntime=true' \
    'customX18EntitlementEmbedded=true' >"$PROBE_OUTPUT"

if ! "$PROBE_BINARY" >>"$PROBE_OUTPUT" 2>"$PROBE_ERROR"; then
    /bin/cat "$PROBE_ERROR" >&2
    echo "native ARM64 helper did not complete its capability probe" >&2
    exit 2
fi
/bin/cat "$PROBE_ERROR" >&2
/bin/cat "$PROBE_OUTPUT"

for required_key in kuserSharedDataMappable kuserSharedDataCleanupSucceeded \
                    customX18ApiAvailable customX18SignatureVerified \
                    customX18HardenedRuntime customX18EntitlementEmbedded \
                    customX18EntitlementRequired customX18PreemptionObserved \
                    customX18Preserved; do
    if [ "$(/usr/bin/grep -c "^${required_key}=" "$PROBE_OUTPUT")" -ne 1 ]; then
        echo "native ARM64 helper returned malformed output for $required_key" >&2
        exit 2
    fi
done

SUPPORTED=true
[ "$OS_FLOOR_SUPPORTED" = true ] || SUPPORTED=false
/usr/bin/grep -Fx 'kuserSharedDataMappable=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'kuserSharedDataCleanupSucceeded=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'customX18ApiAvailable=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'customX18SignatureVerified=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'customX18HardenedRuntime=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'customX18EntitlementEmbedded=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'customX18PreemptionObserved=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false
/usr/bin/grep -Fx 'customX18Preserved=true' "$PROBE_OUTPUT" >/dev/null || SUPPORTED=false

printf 'nativeArm64HostSupported=%s\n' "$SUPPORTED"
if [ "$MODE" = strict ] && [ "$SUPPORTED" != true ]; then
    echo "native ARM64 host requirements are not satisfied; use --probe to collect non-fatal capability evidence" >&2
    exit 1
fi
exit 0
