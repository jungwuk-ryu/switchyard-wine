#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
PROVIDER_LIBRARY="$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
PROCESS_PROBE_SOURCE="$ROOT_DIR/switchyard/tests/native_no_rosetta_process_probe.c"

TIMEOUT_SECONDS=60
MAX_LOG_BYTES=$((16 * 1024 * 1024))
EVIDENCE_REQUEST=""
EVIDENCE_ROOT=""
PREFIX=""
PREFIX_ID=""
RUN_PID=""
LOGGER_PID=""
CLEANUP_PID=""
WINESERVER=""
PREFIX_CLEANED=0
RUNTIME_LOG=""
LOG_FIFO=""
LOG_CONTROL_FIFO=""
LOG_TRUNCATED=""
POST_OBSERVATION_EVIDENCE=""
LOG_WRITER_OPEN=0
LOG_CONTROL_OPEN=0
LOG_OBSERVATION_ENDED=0
PROCESS_PROBE=""
PROBE_OUTPUT=""
PROBE_ERROR=""
PROCESS_EVIDENCE=""
LOADED_IMAGE_OUTPUT=""
LOADED_IMAGE_ERROR=""
LOADED_IMAGE_EVIDENCE=""
LOADED_IMAGE_PROVED=0
LOADED_IMAGE_PROOF_PID=""
LOADED_IMAGE_RESULT=""
PROCESS_IDENTITY=""
TEST_PE_INPUT=""
TEST_PE_SNAPSHOT=""
TEST_PE_SNAPSHOT_ID=""
TEST_PE_IMMUTABLE=0
ARCHITECTURE_SAMPLES=0
MAX_RECORDED_ARCHITECTURE_SAMPLES=32
MAX_PROBE_OUTPUT_BYTES=$((256 * 1024))
NATIVE_RUNTIME_ENV=(
    /usr/bin/env
    -u WINEARCH
    -u WINEDEBUG
    -u WINEDLLPATH
    -u WINELOADER
    -u WINESERVER
    -u DISABLE_GPTK_OVERLAY
    -u GPTK_PATH
    -u SWITCHYARD_DISABLE_GPTK_OVERLAY
    -u SWITCHYARD_GPTK_PATH
    -u SWITCHYARD_GPTK_DLL_NT_PATH
    -u DYLD_LIBRARY_PATH
    -u DYLD_FALLBACK_LIBRARY_PATH
    -u DYLD_FRAMEWORK_PATH
    -u DYLD_FALLBACK_FRAMEWORK_PATH
    -u DYLD_INSERT_LIBRARIES
)

usage()
{
    cat >&2 <<EOF
usage: $0 RUNTIME TEST_PE TEST_SUITE TEST_SELECTOR [options]

options:
  --timeout-seconds N  bound the complete guest run to 1..600 seconds
  --max-log-bytes N    retain at most 4096..268435456 runtime-log bytes
  --evidence-dir PATH  create this new absolute directory for preserved evidence
EOF
}

fail()
{
    echo "native i386 UI acceptance failed: $1" >&2
    if [ -n "$EVIDENCE_ROOT" ]; then
        echo "evidenceRoot=$EVIDENCE_ROOT" >&2
    fi
    exit 1
}

fail_usage()
{
    echo "$1" >&2
    usage
    exit 2
}

path_has_line_break()
{
    case "$1" in
        *$'\n'*|*$'\r'*) return 0 ;;
        *) return 1 ;;
    esac
}

file_size()
{
    /usr/bin/stat -f '%z' "$1"
}

sha256_file()
{
    local value

    value="$(/usr/bin/shasum -a 256 "$1")"
    printf '%s\n' "${value%% *}"
}

regular_file_identity()
{
    local path="$1"

    [ -f "$path" ] && [ ! -L "$path" ] || return 1
    /usr/bin/stat -f '%d:%i' "$path"
}

process_has_exited()
{
    local pid="$1"
    local state

    if ! /bin/kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    state="$(/bin/ps -p "$pid" -o stat= 2>/dev/null || true)"
    case "$state" in
        *Z*) return 0 ;;
    esac
    return 1
}

wait_for_process_exit()
{
    local pid="$1"
    local attempts="$2"
    local count

    for ((count = 0; count < attempts; count++)); do
        process_has_exited "$pid" && return 0
        /bin/sleep 0.1
    done
    return 1
}

stop_exact_run_process()
{
    [ -n "$RUN_PID" ] || return 0
    if ! process_has_exited "$RUN_PID"; then
        /bin/kill -TERM "$RUN_PID" 2>/dev/null || true
        if ! wait_for_process_exit "$RUN_PID" 30; then
            /bin/kill -KILL "$RUN_PID" 2>/dev/null || true
            wait_for_process_exit "$RUN_PID" 30 || true
        fi
    fi
    wait "$RUN_PID" 2>/dev/null || true
    RUN_PID=""
}

clear_test_pe_immutable()
{
    [ "$TEST_PE_IMMUTABLE" -eq 1 ] || return 0
    if [ -n "$TEST_PE_SNAPSHOT" ] && [ -f "$TEST_PE_SNAPSHOT" ] &&
       [ ! -L "$TEST_PE_SNAPSHOT" ]; then
        [ "$(regular_file_identity "$TEST_PE_SNAPSHOT" 2>/dev/null || true)" = \
            "$TEST_PE_SNAPSHOT_ID" ] || return 1
        /usr/bin/chflags nouchg "$TEST_PE_SNAPSHOT" 2>/dev/null || return 1
    fi
    TEST_PE_IMMUTABLE=0
}

prefix_identity_is_unchanged()
{
    local current

    [ -n "$PREFIX" ] && [ -d "$PREFIX" ] && [ ! -L "$PREFIX" ] || return 1
    case "$PREFIX" in
        "$EVIDENCE_ROOT/prefix") ;;
        *) return 1 ;;
    esac
    current="$(/usr/bin/stat -f '%d:%i' "$PREFIX" 2>/dev/null || true)"
    [ -n "$current" ] && [ "$current" = "$PREFIX_ID" ]
}

run_bounded_prefix_command()
{
    local operation="$1"
    local status=0
    local timed_out=0

    "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" \
        "$WINESERVER" "$operation" >/dev/null 2>&1 &
    CLEANUP_PID=$!
    if ! wait_for_process_exit "$CLEANUP_PID" 50; then
        timed_out=1
        /bin/kill -TERM "$CLEANUP_PID" 2>/dev/null || true
        if ! wait_for_process_exit "$CLEANUP_PID" 20; then
            /bin/kill -KILL "$CLEANUP_PID" 2>/dev/null || true
            wait_for_process_exit "$CLEANUP_PID" 20 || true
        fi
    fi
    set +e
    wait "$CLEANUP_PID"
    status=$?
    set -e
    CLEANUP_PID=""
    [ "$timed_out" -eq 0 ] && [ "$status" -eq 0 ]
}

cleanup_prefix_processes()
{
    local strict="${1:-false}"
    local kill_status=0
    local wait_status=0

    [ "$PREFIX_CLEANED" -eq 0 ] || return 0
    [ -n "${WINESERVER:-}" ] || return 0
    if ! prefix_identity_is_unchanged; then
        echo "refusing cleanup because the exact-owned prefix identity changed" >&2
        [ "$strict" = false ] || return 1
        return 0
    fi

    run_bounded_prefix_command -k || kill_status=1
    run_bounded_prefix_command -w || wait_status=1
    PREFIX_CLEANED=1
    if [ -n "$EVIDENCE_ROOT" ]; then
        printf 'prefix=%s\nkillStatus=%s\nwaitStatus=%s\n' \
            "$PREFIX" "$([ "$kill_status" -eq 0 ] && printf passed || printf failed)" \
            "$([ "$wait_status" -eq 0 ] && printf passed || printf failed)" \
            >"$EVIDENCE_ROOT/cleanup.evidence"
    fi
    [ "$strict" = false ] ||
        { [ "$kill_status" -eq 0 ] && [ "$wait_status" -eq 0 ]; }
}

stop_logger()
{
    local strict="${1:-false}"
    local status=0
    local timed_out=0

    [ -n "$LOGGER_PID" ] || return 0
    if ! wait_for_process_exit "$LOGGER_PID" 50; then
        timed_out=1
        /bin/kill -TERM "$LOGGER_PID" 2>/dev/null || true
        if ! wait_for_process_exit "$LOGGER_PID" 20; then
            /bin/kill -KILL "$LOGGER_PID" 2>/dev/null || true
            wait_for_process_exit "$LOGGER_PID" 20 || true
        fi
    fi
    set +e
    wait "$LOGGER_PID" 2>/dev/null
    status=$?
    set -e
    LOGGER_PID=""
    [ "$strict" = false ] || { [ "$timed_out" -eq 0 ] && [ "$status" -eq 0 ]; }
}

end_log_observation()
{
    local status=0

    [ "$LOG_OBSERVATION_ENDED" -eq 0 ] || return 0
    if [ "$LOG_CONTROL_OPEN" -eq 1 ]; then
        set +e
        printf '%s\n' "$MARKER" >&9
        status=$?
        set -e
        exec 9>&-
        LOG_CONTROL_OPEN=0
    fi
    if [ "$LOG_WRITER_OPEN" -eq 1 ]; then
        exec 8>&-
        LOG_WRITER_OPEN=0
    fi
    LOG_OBSERVATION_ENDED=1
    [ "$status" -eq 0 ]
}

cleanup_on_exit()
{
    local status=$?

    trap - EXIT HUP INT TERM
    stop_exact_run_process
    end_log_observation || true
    if [ -n "$CLEANUP_PID" ]; then
        if ! process_has_exited "$CLEANUP_PID"; then
            /bin/kill -TERM "$CLEANUP_PID" 2>/dev/null || true
            wait_for_process_exit "$CLEANUP_PID" 20 || true
        fi
        wait "$CLEANUP_PID" 2>/dev/null || true
        CLEANUP_PID=""
    fi
    cleanup_prefix_processes false || true
    stop_logger false
    clear_test_pe_immutable || true
    if [ -n "$LOG_FIFO" ] && [ -p "$LOG_FIFO" ]; then
        /bin/rm -f "$LOG_FIFO"
    fi
    if [ -n "$LOG_CONTROL_FIFO" ] && [ -p "$LOG_CONTROL_FIFO" ]; then
        /bin/rm -f "$LOG_CONTROL_FIFO"
    fi
    exit "$status"
}

trap cleanup_on_exit EXIT
trap 'exit 130' HUP INT TERM

if [ "$#" -lt 4 ]; then
    usage
    exit 2
fi

RUNTIME="$1"
TEST_PE="$2"
TEST_SUITE="$3"
TEST_SELECTOR="$4"
shift 4

while [ "$#" -gt 0 ]; do
    case "$1" in
        --timeout-seconds)
            [ "$#" -ge 2 ] || fail_usage "--timeout-seconds requires a value"
            [[ "$2" =~ ^[0-9]+$ ]] || fail_usage "timeout must be a decimal integer"
            [ "${#2}" -le 9 ] || fail_usage "timeout value is too large"
            TIMEOUT_SECONDS=$((10#$2))
            shift 2
            ;;
        --max-log-bytes)
            [ "$#" -ge 2 ] || fail_usage "--max-log-bytes requires a value"
            [[ "$2" =~ ^[0-9]+$ ]] || fail_usage "maximum log size must be a decimal integer"
            [ "${#2}" -le 9 ] || fail_usage "maximum log-size value is too large"
            MAX_LOG_BYTES=$((10#$2))
            shift 2
            ;;
        --evidence-dir)
            [ "$#" -ge 2 ] || fail_usage "--evidence-dir requires a path"
            [ -z "$EVIDENCE_REQUEST" ] || fail_usage "--evidence-dir may be specified only once"
            EVIDENCE_REQUEST="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *) fail_usage "unknown argument: $1" ;;
    esac
done

[ "$TIMEOUT_SECONDS" -ge 1 ] && [ "$TIMEOUT_SECONDS" -le 600 ] ||
    fail_usage "timeout must be between 1 and 600 seconds"
[ "$MAX_LOG_BYTES" -ge 4096 ] && [ "$MAX_LOG_BYTES" -le 268435456 ] ||
    fail_usage "maximum log size must be between 4096 and 268435456 bytes"
case "$RUNTIME" in
    /*) ;;
    *) fail_usage "native runtime path must be absolute" ;;
esac
case "$TEST_PE" in
    /*) ;;
    *) fail_usage "i386 test PE path must be absolute" ;;
esac
[[ "$TEST_SUITE" =~ ^[A-Za-z0-9_]{1,64}$ ]] ||
    fail_usage "test suite must use 1..64 identifier characters"
[[ "$TEST_SELECTOR" =~ ^test_[A-Za-z0-9_]{1,122}$ ]] ||
    fail_usage "test selector must be one exact test_ identifier"
path_has_line_break "$RUNTIME" && fail_usage "runtime path contains a line break"
path_has_line_break "$TEST_PE" && fail_usage "test PE path contains a line break"

[ "$EUID" -ne 0 ] || fail "refusing to run a Windows test as root"
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "native UI acceptance requires macOS"
[ "$(/usr/bin/uname -m)" = arm64 ] || fail "native UI acceptance requires a native arm64 shell"

[ -d "$RUNTIME" ] && [ ! -L "$RUNTIME" ] || fail "runtime root is missing or unsafe"
RUNTIME="$(cd "$RUNTIME" && pwd -P)"
MANIFEST="$RUNTIME/switchyard-runtime.json"
[ -f "$MANIFEST" ] && [ ! -L "$MANIFEST" ] || fail "runtime has no regular manifest"

# Reuse the closed runtime-family, provider, and Unixlib-policy contracts used
# by build and release.
[ -f "$PROFILE_LIBRARY" ] && [ ! -L "$PROFILE_LIBRARY" ] ||
    fail "runtime-profile validator is missing or unsafe"
[ -f "$PROVIDER_LIBRARY" ] && [ ! -L "$PROVIDER_LIBRARY" ] ||
    fail "native CPU-provider validator is missing or unsafe"
# shellcheck disable=SC1090 # Paths are fixed relative to the repository root.
source "$PROFILE_LIBRARY"
# shellcheck disable=SC1090 # Paths are fixed relative to the repository root.
source "$PROVIDER_LIBRARY"
declare -F switchyard_validate_native_cpu_provider_files >/dev/null ||
    fail "native CPU-provider validator does not expose its closure gate"
declare -F switchyard_validate_wow64_unixlib_policy_manifest >/dev/null ||
    fail "Wow64 Unixlib validator does not expose its closure gate"
switchyard_validate_runtime_manifest_profile "$MANIFEST" preview-native-arm64-fex ||
    fail "runtime manifest does not qualify as preview-native-arm64-fex"
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME" ||
    fail "runtime CPU-provider closure is not qualified"
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR" ||
    fail "runtime Wow64 Unixlib policy closure is not qualified"

LAUNCHER="$RUNTIME/bin/switchyard-wine"
WINESERVER="$RUNTIME/bin/wineserver"
[ -x "$LAUNCHER" ] || fail "runtime switchyard-wine launcher is missing"
RESOLVED_LAUNCHER="$(/usr/bin/perl -MCwd=abs_path -e '
    my $path = abs_path($ARGV[0]);
    exit 1 unless defined $path;
    print $path;
' "$LAUNCHER")" || fail "cannot resolve runtime switchyard-wine launcher"
[ -f "$RESOLVED_LAUNCHER" ] && [ ! -L "$RESOLVED_LAUNCHER" ] &&
    [ -x "$RESOLVED_LAUNCHER" ] || fail "runtime launcher target is unsafe"
case "$RESOLVED_LAUNCHER" in
    "$RUNTIME"/*) ;;
    *) fail "runtime launcher resolves outside the runtime root" ;;
esac
[ -f "$WINESERVER" ] && [ ! -L "$WINESERVER" ] && [ -x "$WINESERVER" ] ||
    fail "runtime wineserver is missing or unsafe"
WINE_EXECUTABLE="$RUNTIME/lib/wine/aarch64-unix/wine"
WINEMAC_IMAGE="$RUNTIME/lib/wine/aarch64-unix/winemac.so"
I386_PROVIDER_IMAGE="$RUNTIME/lib/wine/aarch64-unix/xtajit.so"
[ -f "$WINE_EXECUTABLE" ] && [ ! -L "$WINE_EXECUTABLE" ] &&
    [ -x "$WINE_EXECUTABLE" ] || fail "native Wine executable is missing or unsafe"
for required_image in "$WINEMAC_IMAGE" "$I386_PROVIDER_IMAGE"; do
    [ -f "$required_image" ] && [ ! -L "$required_image" ] ||
        fail "required native i386 UI image is missing or unsafe: $required_image"
done

[ -f "$TEST_PE" ] && [ ! -L "$TEST_PE" ] || fail "i386 test PE is missing or unsafe"
TEST_PE_INPUT="$(cd "$(dirname "$TEST_PE")" && pwd -P)/$(basename "$TEST_PE")"

if [ -n "$EVIDENCE_REQUEST" ]; then
    case "$EVIDENCE_REQUEST" in
        /*) ;;
        *) fail_usage "evidence directory path must be absolute" ;;
    esac
    path_has_line_break "$EVIDENCE_REQUEST" && fail_usage "evidence path contains a line break"
    [ ! -e "$EVIDENCE_REQUEST" ] && [ ! -L "$EVIDENCE_REQUEST" ] ||
        fail "evidence directory already exists"
    /bin/mkdir -m 700 "$EVIDENCE_REQUEST"
    EVIDENCE_ROOT="$(cd "$EVIDENCE_REQUEST" && pwd -P)"
else
    EVIDENCE_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-native-i386-ui.XXXXXX)"
    EVIDENCE_ROOT="$(cd "$EVIDENCE_ROOT" && pwd -P)"
fi
PREFIX="$EVIDENCE_ROOT/prefix"
/bin/mkdir -m 700 "$PREFIX"
PREFIX_ID="$(/usr/bin/stat -f '%d:%i' "$PREFIX")"
RUNTIME_LOG="$EVIDENCE_ROOT/runtime.log"
LOG_FIFO="$EVIDENCE_ROOT/runtime.pipe"
LOG_CONTROL_FIFO="$EVIDENCE_ROOT/runtime-control.pipe"
LOG_TRUNCATED="$EVIDENCE_ROOT/runtime.log.truncated"
POST_OBSERVATION_EVIDENCE="$EVIDENCE_ROOT/runtime-post-observation.evidence"
PROCESS_PROBE="$EVIDENCE_ROOT/native-process-probe"
PROBE_OUTPUT="$EVIDENCE_ROOT/process-probe.current.out"
PROBE_ERROR="$EVIDENCE_ROOT/process-probe.current.err"
PROCESS_EVIDENCE="$EVIDENCE_ROOT/process.evidence"
LOADED_IMAGE_OUTPUT="$EVIDENCE_ROOT/loaded-image.current.out"
LOADED_IMAGE_ERROR="$EVIDENCE_ROOT/loaded-image.current.err"
LOADED_IMAGE_EVIDENCE="$EVIDENCE_ROOT/loaded-image.evidence"
TEST_PE_SNAPSHOT="$EVIDENCE_ROOT/i386-test.exe"
MARKER="SWITCHYARD_NATIVE_I386_UI_$(/usr/bin/uuidgen | /usr/bin/tr '[:lower:]' '[:upper:]')"
RUNTIME_ID="$(switchyard_runtime_manifest_value id "$MANIFEST")"
MANIFEST_SHA256="$(sha256_file "$MANIFEST")"
TEST_PE_SHA256="$(/usr/bin/python3 -I - "$TEST_PE_INPUT" "$TEST_PE_SNAPSHOT" <<'PY'
import hashlib
import os
import stat
import sys

source, destination = sys.argv[1:]
source_fd = destination_fd = -1


def identity(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


try:
    source_fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    before = os.fstat(source_fd)
    named_before = os.lstat(source)
    if (
        not stat.S_ISREG(before.st_mode)
        or not stat.S_ISREG(named_before.st_mode)
        or before.st_dev != named_before.st_dev
        or before.st_ino != named_before.st_ino
        or before.st_size < 0x86
        or before.st_size > 512 * 1024 * 1024
    ):
        raise SystemExit("source is not one bounded stable regular file")
    destination_fd = os.open(
        destination,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
        0o400,
    )
    digest = hashlib.sha256()
    copied = 0
    while True:
        block = os.read(source_fd, 1024 * 1024)
        if not block:
            break
        digest.update(block)
        copied += len(block)
        view = memoryview(block)
        while view:
            written = os.write(destination_fd, view)
            if written <= 0:
                raise SystemExit("snapshot write made no progress")
            view = view[written:]
    os.fsync(destination_fd)
    os.fchmod(destination_fd, 0o400)
    after = os.fstat(source_fd)
    named_after = os.lstat(source)
    snapshot = os.fstat(destination_fd)
    if (
        identity(before) != identity(after)
        or identity(named_before) != identity(named_after)
        or after.st_dev != named_after.st_dev
        or after.st_ino != named_after.st_ino
        or copied != before.st_size
        or not stat.S_ISREG(snapshot.st_mode)
        or snapshot.st_size != copied
        or stat.S_IMODE(snapshot.st_mode) != 0o400
    ):
        raise SystemExit("source changed while its private snapshot was created")
    print(digest.hexdigest())
finally:
    if destination_fd != -1:
        os.close(destination_fd)
    if source_fd != -1:
        os.close(source_fd)
PY
)" || fail "could not create a stable private i386 test snapshot"
[[ "$TEST_PE_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
    fail "private i386 test snapshot returned a malformed digest"
[ "$(sha256_file "$TEST_PE_SNAPSHOT")" = "$TEST_PE_SHA256" ] ||
    fail "private i386 test snapshot digest does not match its source"
TEST_PE_SNAPSHOT_ID="$(regular_file_identity "$TEST_PE_SNAPSHOT")" ||
    fail "private i386 test snapshot is not a safe regular file"
TEST_PE_IMMUTABLE=1
/usr/bin/chflags uchg "$TEST_PE_SNAPSHOT" ||
    fail "could not make the private i386 test snapshot immutable"
case "$(/usr/bin/stat -f '%Sf' "$TEST_PE_SNAPSHOT")" in
    *uchg*) ;;
    *) fail "private i386 test snapshot did not retain its immutable flag" ;;
esac
TEST_PE="$TEST_PE_SNAPSHOT"
/usr/bin/python3 -I - "$TEST_PE" <<'PY' || fail "test snapshot is not an i386 PE image"
import os
import struct
import sys

path = sys.argv[1]
size = os.path.getsize(path)
if size < 0x86 or size > 512 * 1024 * 1024:
    raise SystemExit(1)
with open(path, "rb") as stream:
    header = stream.read(0x40)
    if header[:2] != b"MZ":
        raise SystemExit(1)
    pe_offset = struct.unpack_from("<I", header, 0x3c)[0]
    if pe_offset > size - 6:
        raise SystemExit(1)
    stream.seek(pe_offset)
    if stream.read(4) != b"PE\0\0":
        raise SystemExit(1)
    machine = struct.unpack("<H", stream.read(2))[0]
    if machine != 0x014c:
        raise SystemExit(1)
PY
LAUNCHER_SHA256="$(sha256_file "$RESOLVED_LAUNCHER")"
WINESERVER_SHA256="$(sha256_file "$WINESERVER")"
/bin/cp "$MANIFEST" "$EVIDENCE_ROOT/runtime-manifest.json"
/bin/chmod 0600 "$EVIDENCE_ROOT/runtime-manifest.json"
[ "$(sha256_file "$EVIDENCE_ROOT/runtime-manifest.json")" = "$MANIFEST_SHA256" ] ||
    fail "runtime manifest snapshot does not match the qualified input"

printf '%s\n' \
    "runtime=$RUNTIME" \
    "runtimeId=$RUNTIME_ID" \
    "manifestSha256=$MANIFEST_SHA256" \
    "launcher=$LAUNCHER" \
    "launcherResolved=$RESOLVED_LAUNCHER" \
    "launcherSha256=$LAUNCHER_SHA256" \
    "wineserver=$WINESERVER" \
    "wineserverSha256=$WINESERVER_SHA256" \
    "testPEInput=$TEST_PE_INPUT" \
    "testPESnapshot=$TEST_PE_SNAPSHOT" \
    "testPESha256=$TEST_PE_SHA256" \
    "testSuite=$TEST_SUITE" \
    "testSelector=$TEST_SELECTOR" \
    "invocationMarker=$MARKER" \
    "hostPlatform=Darwin" \
    "hostArchitecture=arm64" \
    "timeoutSeconds=$TIMEOUT_SECONDS" \
    "maxLogBytes=$MAX_LOG_BYTES" \
    "prefix=$PREFIX" >"$EVIDENCE_ROOT/invocation.evidence"

[ -f "$PROCESS_PROBE_SOURCE" ] && [ ! -L "$PROCESS_PROBE_SOURCE" ] ||
    fail "native no-Rosetta process probe source is missing or unsafe"
if ! /usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
        -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
        "$PROCESS_PROBE_SOURCE" -o "$PROCESS_PROBE" \
        >"$EVIDENCE_ROOT/process-probe-build.out" \
        2>"$EVIDENCE_ROOT/process-probe-build.err"; then
    fail "could not build the native no-Rosetta process probe"
fi

printf 'switchyardInvocationMarker=%s\n' "$MARKER" >"$RUNTIME_LOG"
printf 'switchyardTestSuite=%s\n' "$TEST_SUITE" >>"$RUNTIME_LOG"
printf 'switchyardTestSelector=%s\n' "$TEST_SELECTOR" >>"$RUNTIME_LOG"
: >"$PROBE_OUTPUT"
: >"$PROBE_ERROR"
: >"$PROCESS_EVIDENCE"
: >"$LOADED_IMAGE_OUTPUT"
: >"$LOADED_IMAGE_ERROR"
: >"$LOADED_IMAGE_EVIDENCE"
RUNTIME_LOG_ID="$(regular_file_identity "$RUNTIME_LOG")" ||
    fail "runtime log is not a safe regular evidence file"
PROBE_OUTPUT_ID="$(regular_file_identity "$PROBE_OUTPUT")" ||
    fail "process probe output is not a safe regular evidence file"
PROBE_ERROR_ID="$(regular_file_identity "$PROBE_ERROR")" ||
    fail "process probe error is not a safe regular evidence file"
PROCESS_EVIDENCE_ID="$(regular_file_identity "$PROCESS_EVIDENCE")" ||
    fail "process evidence is not a safe regular file"
LOADED_IMAGE_OUTPUT_ID="$(regular_file_identity "$LOADED_IMAGE_OUTPUT")" ||
    fail "loaded-image output is not a safe regular file"
LOADED_IMAGE_ERROR_ID="$(regular_file_identity "$LOADED_IMAGE_ERROR")" ||
    fail "loaded-image error is not a safe regular file"
LOADED_IMAGE_EVIDENCE_ID="$(regular_file_identity "$LOADED_IMAGE_EVIDENCE")" ||
    fail "loaded-image evidence is not a safe regular file"
/usr/bin/mkfifo "$LOG_FIFO"
/usr/bin/mkfifo "$LOG_CONTROL_FIFO"
/usr/bin/python3 -c '
import errno
import fcntl
import os
import select
import sys

maximum = int(sys.argv[1])
destination = sys.argv[2]
truncated = sys.argv[3]
marker_text = sys.argv[4]
marker = ("\nswitchyardObservationEnd=" + marker_text + "\n").encode("ascii")
post_observation = sys.argv[5]
source_path = sys.argv[6]
control_path = sys.argv[7]
written = os.path.getsize(destination)
ended = False
discarded = 0

def retain(block):
    global written
    remaining = max(0, maximum - written)
    if remaining:
        output.write(block[:remaining])
        written += min(len(block), remaining)
    if len(block) > remaining and not os.path.exists(truncated):
        try:
            descriptor = os.open(truncated, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            os.close(descriptor)

source = os.open(source_path, os.O_RDONLY)
control = os.open(control_path, os.O_RDONLY)
control_pending = b""
source_eof = False
try:
    with open(destination, "ab", buffering=0) as output:
        while not source_eof:
            descriptors = [source]
            if not ended:
                descriptors.append(control)
            ready, _, _ = select.select(descriptors, [], [])

            # Retain data already readable from the stopped/exited test before
            # accepting the separate parent-only observation boundary.
            if source in ready:
                block = os.read(source, 65536)
                if not block:
                    source_eof = True
                elif ended:
                    discarded += len(block)
                else:
                    retain(block)

            if not ended and control in ready:
                block = os.read(control, 4096)
                if not block:
                    raise SystemExit("observation control closed without its marker")
                control_pending += block
                if b"\n" not in control_pending:
                    if len(control_pending) > 256:
                        raise SystemExit("observation control marker exceeds its bound")
                    continue
                line, remainder = control_pending.split(b"\n", 1)
                if line != marker_text.encode("ascii") or remainder:
                    raise SystemExit("observation control marker is malformed")

                flags = fcntl.fcntl(source, fcntl.F_GETFL)
                fcntl.fcntl(source, fcntl.F_SETFL, flags | os.O_NONBLOCK)
                while True:
                    try:
                        block = os.read(source, 65536)
                    except OSError as error:
                        if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                            break
                        raise
                    if not block:
                        source_eof = True
                        break
                    retain(block)
                retain(marker)
                ended = True
        if not ended:
            raise SystemExit("runtime log closed without its observation-end marker")
finally:
    os.close(control)
    os.close(source)
with open(post_observation, "x", encoding="ascii") as stream:
    stream.write(f"discardedBytes={discarded}\n")
' "$MAX_LOG_BYTES" "$RUNTIME_LOG" "$LOG_TRUNCATED" "$MARKER" \
    "$POST_OBSERVATION_EVIDENCE" "$LOG_FIFO" "$LOG_CONTROL_FIFO" &
LOGGER_PID=$!
exec 8>"$LOG_FIFO"
LOG_WRITER_OPEN=1
exec 9>"$LOG_CONTROL_FIFO"
LOG_CONTROL_OPEN=1

START_SECONDS="$(/bin/date +%s)"
START_MONOTONIC_SECONDS="$SECONDS"
"${NATIVE_RUNTIME_ENV[@]}" \
    WINEPREFIX="$PREFIX" \
    WINEDEBUG='-all,+loaddll,+xtajit,err+winediag' \
    SWITCHYARD_NATIVE_I386_UI_MARKER="$MARKER" \
    "$RESOLVED_LAUNCHER" "$TEST_PE" "$TEST_SUITE" "$TEST_SELECTOR" \
    >"$LOG_FIFO" 2>&1 8>&- 9>&- &
RUN_PID=$!
printf 'requiredPid=%s\nstartSeconds=%s\n' "$RUN_PID" "$START_SECONDS" \
    >>"$EVIDENCE_ROOT/invocation.evidence"

RUN_STATUS=""
RUN_FAILURE=""
while true; do
    if process_has_exited "$RUN_PID"; then
        set +e
        wait "$RUN_PID"
        RUN_STATUS=$?
        set -e
        RUN_PID=""
        break
    fi
    if [ -e "$LOG_TRUNCATED" ]; then
        RUN_FAILURE="runtime log exceeded the configured byte bound"
        break
    fi
    if [ $((SECONDS - START_MONOTONIC_SECONDS)) -ge "$TIMEOUT_SECONDS" ]; then
        RUN_FAILURE="exact test invocation exceeded ${TIMEOUT_SECONDS} seconds"
        break
    fi

    set +e
    /usr/bin/env -u SWITCHYARD_PROCESS_PROBE_DEBUG \
        "$PROCESS_PROBE" "$RUNTIME" "$RUN_PID" "$START_SECONDS" "$PREFIX" \
        >"$PROBE_OUTPUT" 2>"$PROBE_ERROR"
    PROBE_STATUS=$?
    set -e
    if [ "$(regular_file_identity "$PROBE_OUTPUT" 2>/dev/null || true)" != "$PROBE_OUTPUT_ID" ] ||
       [ "$(regular_file_identity "$PROBE_ERROR" 2>/dev/null || true)" != "$PROBE_ERROR_ID" ] ||
       [ "$(regular_file_identity "$PROCESS_EVIDENCE" 2>/dev/null || true)" != "$PROCESS_EVIDENCE_ID" ]; then
        RUN_FAILURE="process evidence file identity changed during the acceptance run"
        break
    fi
    if [ "$(file_size "$PROBE_OUTPUT")" -gt "$MAX_PROBE_OUTPUT_BYTES" ] ||
       [ "$(file_size "$PROBE_ERROR")" -gt "$MAX_PROBE_OUTPUT_BYTES" ]; then
        RUN_FAILURE="native process probe exceeded its evidence bound"
        break
    fi
    if /usr/bin/grep -E \
        'is Rosetta translated|is not native ARM64|unexpected external helper|native architecture proof is unproven' \
        "$PROBE_ERROR" >/dev/null 2>&1; then
        # The reusable probe deliberately fails closed if a captured process
        # exits before PIDARCHINFO. Once this exact required PID has already
        # produced native evidence, allow only its single terminal snapshot
        # race; every translated, external, non-ARM64, or additional unproven
        # process remains fatal.
        TERMINAL_REQUIRED_EXIT="cannot query captured matched runtime process $RUN_PID; native architecture proof is unproven"
        if [ "$ARCHITECTURE_SAMPLES" -gt 0 ] && process_has_exited "$RUN_PID" &&
           [ "$(/usr/bin/grep -Fxc "$TERMINAL_REQUIRED_EXIT" "$PROBE_ERROR")" -eq 1 ] &&
           [ "$(/usr/bin/grep -Ec 'is Rosetta translated|is not native ARM64|unexpected external helper|native architecture proof is unproven' "$PROBE_ERROR")" -eq 1 ] &&
           ! /usr/bin/grep -E '^pid=[0-9]+ cpuType=0x[0-9A-Fa-f]+ translated=true( |$)' \
                "$PROBE_OUTPUT" >/dev/null 2>&1; then
            printf 'terminalRequiredPidExit=%s\n' "$RUN_PID" >>"$PROCESS_EVIDENCE"
            /bin/sleep 0.1
            continue
        fi
        RUN_FAILURE="runtime process architecture inspection found a forbidden host process"
        break
    fi
    if [ "$PROBE_STATUS" -ne 0 ]; then
        TERMINAL_REQUIRED_ABSENT="required runtime process $RUN_PID was not observed"
        if [ "$ARCHITECTURE_SAMPLES" -gt 0 ] && process_has_exited "$RUN_PID" &&
           [ -s "$PROBE_ERROR" ] &&
           /usr/bin/grep -Fqx "$TERMINAL_REQUIRED_ABSENT" "$PROBE_ERROR" &&
           ! /usr/bin/grep -Ev \
                '^(no process from runtime root .* was observed|required runtime process [0-9]+ was not observed|native wineserver was not observed under runtime root .*)$' \
                "$PROBE_ERROR" >/dev/null 2>&1 &&
           ! /usr/bin/grep -E '^pid=[0-9]+ cpuType=0x[0-9A-Fa-f]+ translated=true( |$)' \
                "$PROBE_OUTPUT" >/dev/null 2>&1; then
            printf 'terminalRequiredPidAbsent=%s\n' "$RUN_PID" >>"$PROCESS_EVIDENCE"
            /bin/sleep 0.1
            continue
        fi
        if [ "$ARCHITECTURE_SAMPLES" -eq 0 ] && [ -s "$PROBE_ERROR" ] &&
           ! /usr/bin/grep -Ev \
                '^(no process from runtime root .* was observed|required runtime process [0-9]+ was not observed|native wineserver was not observed under runtime root .*)$' \
                "$PROBE_ERROR" >/dev/null 2>&1; then
            /bin/sleep 0.1
            continue
        fi
        RUN_FAILURE="native process architecture probe failed outside its startup-pending state"
        break
    fi
    if [ "$PROBE_STATUS" -eq 0 ]; then
        if /usr/bin/grep -E '^pid=[0-9]+ cpuType=0x[0-9A-Fa-f]+ translated=true( |$)' \
                "$PROBE_OUTPUT" >/dev/null 2>&1 ||
           ! /usr/bin/grep -E '^pid=[0-9]+ cpuType=0x100000c translated=false( |$)' \
                "$PROBE_OUTPUT" >/dev/null 2>&1; then
            RUN_FAILURE="native process probe returned malformed no-Rosetta evidence"
            break
        fi
        ARCHITECTURE_SAMPLES=$((ARCHITECTURE_SAMPLES + 1))
        if [ "$ARCHITECTURE_SAMPLES" -le "$MAX_RECORDED_ARCHITECTURE_SAMPLES" ]; then
            printf 'sample=%s\n' "$ARCHITECTURE_SAMPLES" >>"$PROCESS_EVIDENCE"
            /bin/cat "$PROBE_OUTPUT" >>"$PROCESS_EVIDENCE"
        fi
    fi
    # Loaded-image enumeration walks the target VM map.  Sample it only after
    # native process evidence exists and then at 500 ms cadence, rather than
    # adding a full region walk to every 100 ms architecture observation.
    if [ "$LOADED_IMAGE_PROVED" -eq 0 ] && [ "$ARCHITECTURE_SAMPLES" -gt 0 ] &&
       ((ARCHITECTURE_SAMPLES == 1 || ARCHITECTURE_SAMPLES % 5 == 0)); then
        if [ "$(regular_file_identity "$LOADED_IMAGE_OUTPUT" 2>/dev/null || true)" != \
                "$LOADED_IMAGE_OUTPUT_ID" ] ||
           [ "$(regular_file_identity "$LOADED_IMAGE_ERROR" 2>/dev/null || true)" != \
                "$LOADED_IMAGE_ERROR_ID" ] ||
           [ "$(regular_file_identity "$LOADED_IMAGE_EVIDENCE" 2>/dev/null || true)" != \
                "$LOADED_IMAGE_EVIDENCE_ID" ]; then
            RUN_FAILURE="loaded-image evidence file identity changed during the acceptance run"
            break
        fi
        if [ -z "$PROCESS_IDENTITY" ]; then
            set +e
            "$PROCESS_PROBE" --capture-process-identity "$RUN_PID" \
                >"$LOADED_IMAGE_OUTPUT" 2>"$LOADED_IMAGE_ERROR"
            LOADED_IMAGE_STATUS=$?
            set -e
            if [ "$(file_size "$LOADED_IMAGE_OUTPUT")" -gt "$MAX_PROBE_OUTPUT_BYTES" ] ||
               [ "$(file_size "$LOADED_IMAGE_ERROR")" -gt "$MAX_PROBE_OUTPUT_BYTES" ]; then
                RUN_FAILURE="loaded-image process identity probe exceeded its evidence bound"
                break
            fi
            if [ "$LOADED_IMAGE_STATUS" -eq 0 ]; then
                if [ "$(/usr/bin/wc -l <"$LOADED_IMAGE_OUTPUT" | /usr/bin/tr -d ' ')" -ne 1 ] ||
                   ! /usr/bin/grep -Eq \
                        '^[0-9]+:[0-9]+:[0-9]+:[0-9]+:[0-9]+$' \
                        "$LOADED_IMAGE_OUTPUT"; then
                    RUN_FAILURE="loaded-image process identity probe returned malformed evidence"
                    break
                fi
                PROCESS_IDENTITY="$(<"$LOADED_IMAGE_OUTPUT")"
            elif [ "$LOADED_IMAGE_STATUS" -gt 1 ]; then
                RUN_FAILURE="loaded-image process identity probe rejected its exact invocation"
                break
            fi
        fi
        if [ -n "$PROCESS_IDENTITY" ]; then
            set +e
            "$PROCESS_PROBE" --loaded-images "$RUN_PID" "$PROCESS_IDENTITY" \
                "$WINE_EXECUTABLE" "$WINEMAC_IMAGE" "$I386_PROVIDER_IMAGE" \
                >"$LOADED_IMAGE_OUTPUT" 2>"$LOADED_IMAGE_ERROR"
            LOADED_IMAGE_STATUS=$?
            set -e
            if [ "$(file_size "$LOADED_IMAGE_OUTPUT")" -gt "$MAX_PROBE_OUTPUT_BYTES" ] ||
               [ "$(file_size "$LOADED_IMAGE_ERROR")" -gt "$MAX_PROBE_OUTPUT_BYTES" ]; then
                RUN_FAILURE="loaded-image probe exceeded its evidence bound"
                break
            fi
            if [ "$LOADED_IMAGE_STATUS" -eq 0 ]; then
                if [ "$(/usr/bin/grep -Ec \
                        '^Loaded-image proof passed for native ARM64 pid [0-9]+ across [1-9][0-9]* regions[.]$' \
                        "$LOADED_IMAGE_OUTPUT")" -ne 1 ] || [ -s "$LOADED_IMAGE_ERROR" ]; then
                    RUN_FAILURE="loaded-image probe returned malformed success evidence"
                    break
                fi
                LOADED_IMAGE_PROOF_PID="$RUN_PID"
                LOADED_IMAGE_RESULT="$(<"$LOADED_IMAGE_OUTPUT")"
                LOADED_IMAGE_PROVED=1
            elif [ "$LOADED_IMAGE_STATUS" -gt 1 ]; then
                RUN_FAILURE="loaded-image probe rejected its exact invocation"
                break
            fi
        fi
    fi
    /bin/sleep 0.1
done

if [ -n "$RUN_FAILURE" ]; then
    stop_exact_run_process
fi
end_log_observation || fail "could not close the runtime-log observation window"
cleanup_prefix_processes true || fail "exact-prefix wineserver cleanup did not complete"
stop_logger true || fail "bounded runtime-log collector did not complete"
[ ! -p "$LOG_FIFO" ] || /bin/rm -f "$LOG_FIFO"
[ ! -p "$LOG_CONTROL_FIFO" ] || /bin/rm -f "$LOG_CONTROL_FIFO"

[ -z "$RUN_FAILURE" ] || fail "$RUN_FAILURE"
[ -n "$RUN_STATUS" ] || fail "test process ended without a captured status"
[ "$RUN_STATUS" -eq 0 ] || fail "exact test invocation exited with status $RUN_STATUS"
[ ! -e "$LOG_TRUNCATED" ] || fail "runtime log exceeded the configured byte bound"
[ "$(file_size "$RUNTIME_LOG")" -le "$MAX_LOG_BYTES" ] ||
    fail "retained runtime log is larger than the configured byte bound"
[ "$(regular_file_identity "$RUNTIME_LOG" 2>/dev/null || true)" = "$RUNTIME_LOG_ID" ] ||
    fail "runtime log file identity changed during the acceptance run"
[ "$(regular_file_identity "$PROCESS_EVIDENCE" 2>/dev/null || true)" = "$PROCESS_EVIDENCE_ID" ] ||
    fail "process evidence file identity changed during the acceptance run"
[ "$(regular_file_identity "$LOADED_IMAGE_EVIDENCE" 2>/dev/null || true)" = \
    "$LOADED_IMAGE_EVIDENCE_ID" ] ||
    fail "loaded-image evidence file identity changed during the acceptance run"
[ "$ARCHITECTURE_SAMPLES" -gt 0 ] ||
    fail "runtime process set exited before native ARM64 evidence converged"
[ "$LOADED_IMAGE_PROVED" -eq 1 ] ||
    fail "exact native winemac and i386 CPU-provider images were not proven loaded"
[ -n "$LOADED_IMAGE_PROOF_PID" ] && [ -n "$LOADED_IMAGE_RESULT" ] ||
    fail "loaded-image proof state was not retained in the acceptance parent"
printf '%s\n' \
    "pid=$LOADED_IMAGE_PROOF_PID" \
    "processIdentity=$PROCESS_IDENTITY" \
    "wineExecutable=$WINE_EXECUTABLE" \
    "winemacImage=$WINEMAC_IMAGE" \
    "i386ProviderImage=$I386_PROVIDER_IMAGE" \
    "$LOADED_IMAGE_RESULT" >"$LOADED_IMAGE_EVIDENCE"
[ "$(regular_file_identity "$LOADED_IMAGE_EVIDENCE" 2>/dev/null || true)" = \
    "$LOADED_IMAGE_EVIDENCE_ID" ] ||
    fail "loaded-image evidence identity changed while retaining the parent proof"
[ -s "$LOADED_IMAGE_EVIDENCE" ] || fail "loaded-image proof evidence was not retained"
[ -f "$PROCESS_EVIDENCE" ] || fail "native process evidence was not retained"
if /usr/bin/grep -E '^pid=[0-9]+ cpuType=0x[0-9A-Fa-f]+ translated=true( |$)' \
        "$PROCESS_EVIDENCE" >/dev/null 2>&1; then
    fail "retained process evidence contains a Rosetta-translated process"
fi
/usr/bin/grep -E '^pid=[0-9]+ cpuType=0x100000c translated=false( |$)' \
    "$PROCESS_EVIDENCE" >/dev/null ||
    fail "retained process evidence contains no native ARM64 process"

if [ "$(sha256_file "$MANIFEST")" != "$MANIFEST_SHA256" ] ||
   [ "$(sha256_file "$TEST_PE_SNAPSHOT")" != "$TEST_PE_SHA256" ] ||
   [ "$(sha256_file "$RESOLVED_LAUNCHER")" != "$LAUNCHER_SHA256" ] ||
   [ "$(sha256_file "$WINESERVER")" != "$WINESERVER_SHA256" ]; then
    fail "qualified inputs changed while the acceptance run was active"
fi
[ "$(regular_file_identity "$TEST_PE_SNAPSHOT" 2>/dev/null || true)" = \
    "$TEST_PE_SNAPSHOT_ID" ] || fail "private i386 test snapshot identity changed"
case "$(/usr/bin/stat -f '%Sf' "$TEST_PE_SNAPSHOT")" in
    *uchg*) ;;
    *) fail "private i386 test snapshot lost its immutable flag" ;;
esac
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME" ||
    fail "runtime CPU-provider closure changed during the acceptance run"
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR" ||
    fail "runtime Wow64 Unixlib policy closure changed during the acceptance run"

MARKER_COUNT="$({ /usr/bin/grep -Fx "switchyardInvocationMarker=$MARKER" \
    "$RUNTIME_LOG" || true; } | /usr/bin/wc -l | /usr/bin/tr -d ' ')"
[ "$MARKER_COUNT" -eq 1 ] || fail "runtime log does not contain its exact invocation marker once"
OBSERVATION_END_COUNT="$({ /usr/bin/grep -Fx "switchyardObservationEnd=$MARKER" \
    "$RUNTIME_LOG" || true; } | /usr/bin/wc -l | /usr/bin/tr -d ' ')"
[ "$OBSERVATION_END_COUNT" -eq 1 ] ||
    fail "runtime log does not contain its exact observation-end marker once"
WINEMAC_LOADS="$({ /usr/bin/grep -E -i \
    ':trace:loaddll:build_module Loaded .*winemac[.]drv' "$RUNTIME_LOG" || true; } |
    /usr/bin/wc -l | /usr/bin/tr -d ' ')"
WINEMAC_UNLOADS="$({ /usr/bin/grep -E -i \
    ':trace:loaddll:free_modref Unloaded module .*winemac[.]drv' "$RUNTIME_LOG" || true; } |
    /usr/bin/wc -l | /usr/bin/tr -d ' ')"
[ "$WINEMAC_UNLOADS" -eq 0 ] || fail "winemac.drv unloaded during the acceptance run"
if /usr/bin/grep -E -i ':err:winediag:nodrv_' "$RUNTIME_LOG" >/dev/null 2>&1; then
    fail "Wine reported the null display driver"
fi
if /usr/bin/grep -E -i \
    ':err:xtajit:poison_provider|poisoning (the )?i386 provider|i386 provider is poisoned|failed to initialize (the )?i386 provider' \
    "$RUNTIME_LOG" >/dev/null 2>&1; then
    fail "i386 CPU provider poison was observed"
fi
PROVIDER_INITIALIZATIONS="$({ /usr/bin/grep -E -i \
    ':trace:xtajit:(process_init|BTCpuProcessInit).*initialized .*i386 .*provider' \
    "$RUNTIME_LOG" || true; } | /usr/bin/wc -l | /usr/bin/tr -d ' ')"
if /usr/bin/grep -F 'Test failed:' "$RUNTIME_LOG" >/dev/null 2>&1 ||
   /usr/bin/grep -E 'tests executed .* [1-9][0-9]* failures?' "$RUNTIME_LOG" >/dev/null 2>&1; then
    fail "Wine test failure output was observed"
fi
SUMMARY_COUNT="$({ /usr/bin/tr -d '\r' <"$RUNTIME_LOG" | /usr/bin/grep -E \
    "^[0-9A-Fa-f]{4}:${TEST_SUITE}:([0-9]+[.][0-9]{3})? [1-9][0-9]* tests executed [(]0 marked as todo, 0 as flaky, 0 failures[)], 0 skipped[.]$" \
    || true; } | /usr/bin/wc -l | /usr/bin/tr -d ' ')"
[ "$SUMMARY_COUNT" -eq 1 ] ||
    fail "exact selector did not produce one zero-failure, zero-skip Wine summary"

RUNTIME_LOG_SHA256="$(sha256_file "$RUNTIME_LOG")"
PROCESS_EVIDENCE_SHA256="$(sha256_file "$PROCESS_EVIDENCE")"
LOADED_IMAGE_EVIDENCE_SHA256="$(sha256_file "$LOADED_IMAGE_EVIDENCE")"
RUNTIME_LOG_BYTES="$(file_size "$RUNTIME_LOG")"
POST_OBSERVATION_DISCARDED_BYTES="$(/usr/bin/awk -F= \
    '$1 == "discardedBytes" && $2 ~ /^[0-9]+$/ {print $2}' "$POST_OBSERVATION_EVIDENCE")"
[[ "$POST_OBSERVATION_DISCARDED_BYTES" =~ ^[0-9]+$ ]] ||
    fail "bounded runtime-log collector returned malformed post-observation evidence"

printf '%s\n' \
    'result=passed' \
    "runtimeId=$RUNTIME_ID" \
    "testPESha256=$TEST_PE_SHA256" \
    "testSuite=$TEST_SUITE" \
    "testSelector=$TEST_SELECTOR" \
    'hostPlatform=Darwin' \
    'hostArchitecture=arm64' \
    'noRosettaEvidence=true' \
    'loadedImageEvidence=true' \
    "architectureSamples=$ARCHITECTURE_SAMPLES" \
    "winemacLoads=$WINEMAC_LOADS" \
    "winemacUnloads=$WINEMAC_UNLOADS" \
    "providerInitializations=$PROVIDER_INITIALIZATIONS" \
    "runtimeLogBytes=$RUNTIME_LOG_BYTES" \
    "runtimeLogSha256=$RUNTIME_LOG_SHA256" \
    "processEvidenceSha256=$PROCESS_EVIDENCE_SHA256" \
    "loadedImageEvidenceSha256=$LOADED_IMAGE_EVIDENCE_SHA256" \
    "postObservationDiscardedBytes=$POST_OBSERVATION_DISCARDED_BYTES" \
    'testFailures=0' >"$EVIDENCE_ROOT/result.evidence"

printf '%s\n' \
    "evidenceRoot=$EVIDENCE_ROOT" \
    "runtimeId=$RUNTIME_ID" \
    "testPESha256=$TEST_PE_SHA256" \
    "testSuite=$TEST_SUITE" \
    "testSelector=$TEST_SELECTOR" \
    'hostPlatform=Darwin' \
    'hostArchitecture=arm64' \
    'noRosettaEvidence=true' \
    'loadedImageEvidence=true' \
    "winemacLoads=$WINEMAC_LOADS" \
    "winemacUnloads=$WINEMAC_UNLOADS" \
    'testFailures=0' \
    'Native i386 UI acceptance passed.'
