#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE_SOURCE="$ROOT_DIR/switchyard/tests/native_no_rosetta_process_probe.c"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
PREFIX_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_prefix.sh"
CONVERGENCE_TIMEOUT_SECONDS=60
MARKER_TIMEOUT_SECONDS=60
SUPERVISOR_TERM_GRACE_SECONDS=2
SUPERVISOR_OUTPUT_LIMIT_BYTES=65536
SUPERVISOR_TERM_WAIT_ATTEMPTS=50
SUPERVISOR_KILL_WAIT_ATTEMPTS=20
ARCHITECTURE_OBSERVATION_SAMPLES=10
ARCHITECTURE_OBSERVATION_INTERVAL_SECONDS=0.05
PYTHON3=""
PROCESS_PROBE_EXECUTABLE=""
SUPERVISOR_CLEANUP_UNCONFIRMED=0
USE_PREPARED_PREFIX=0
PREPARED_PREFIX=""
PROCESS_SUPERVISOR_SOURCE='
import errno
import ctypes
import os
import selectors
import select
import signal
import subprocess
import sys
import time

TIMEOUT_STATUS = 124
OUTPUT_LIMIT_STATUS = 125
SUPERVISOR_FAILURE_STATUS = 126
CLEANUP_FAILURE_STATUS = 127
MAX_GROUP_PIDS = 65536
ZERO_ERRNO_FIXTURE = sys.argv[1:] == ["--regression-fixture-zero-errno"]

if ZERO_ERRNO_FIXTURE:
    mode = None
    timeout = None
    term_grace = 1.0
    output_limit = 1
    stdout_path = stderr_path = cleanup_confirmation_path = None
    pid_fifo = "-"
    command = []
else:
    if len(sys.argv) < 10:
        raise SystemExit(SUPERVISOR_FAILURE_STATUS)
    mode = sys.argv[1]
    timeout = None if sys.argv[2] == "-" else float(sys.argv[2])
    term_grace = float(sys.argv[3])
    output_limit = int(sys.argv[4])
    stdout_path = sys.argv[5]
    stderr_path = sys.argv[6]
    pid_fifo = sys.argv[7]
    cleanup_confirmation_path = sys.argv[8]
    try:
        separator = sys.argv.index("--", 9)
    except ValueError:
        raise SystemExit(SUPERVISOR_FAILURE_STATUS)
    command = sys.argv[separator + 1:]
    if (mode not in ("bounded", "sustained") or not command or term_grace <= 0 or
            output_limit <= 0 or not os.path.isabs(cleanup_confirmation_path)):
        raise SystemExit(SUPERVISOR_FAILURE_STATUS)
    if mode == "bounded" and (timeout is None or timeout <= 0):
        raise SystemExit(SUPERVISOR_FAILURE_STATUS)

requested_signal = 0

def request_termination(signum, _frame):
    global requested_signal
    requested_signal = signum

for handled_signal in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(handled_signal, request_termination)

def signal_group(pgid, signum):
    try:
        os.killpg(pgid, signum)
    except ProcessLookupError:
        pass

libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
proc_listpgrppids = libproc.proc_listpgrppids
proc_listpgrppids.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
proc_listpgrppids.restype = ctypes.c_int

def group_members(pgid):
    capacity = 64
    while capacity <= MAX_GROUP_PIDS:
        buffer = (ctypes.c_int * capacity)()
        ctypes.set_errno(0)
        count = proc_listpgrppids(pgid, buffer, ctypes.sizeof(buffer))
        call_errno = ctypes.get_errno()
        if count < 0 or (count == 0 and call_errno != 0):
            raise OSError(call_errno or errno.EIO, "proc_listpgrppids failed")
        if count < capacity:
            return [pid for pid in buffer[:count] if pid > 0]
        capacity *= 2
    raise RuntimeError("process group exceeds the supervisor PID bound")

if ZERO_ERRNO_FIXTURE:
    def zero_with_errno(_pgid, _buffer, _buffer_size):
        ctypes.set_errno(errno.EIO)
        return 0

    proc_listpgrppids = zero_with_errno
    try:
        group_members(424242)
    except OSError as error:
        if error.errno != errno.EIO:
            raise SystemExit(SUPERVISOR_FAILURE_STATUS)
    else:
        raise SystemExit(SUPERVISOR_FAILURE_STATUS)
    raise SystemExit(0)

process_events = select.kqueue()
leader_exit_observed = False

def leader_has_exited(process):
    global leader_exit_observed
    if leader_exit_observed:
        return True
    events = process_events.control(None, 1, 0)
    if events:
        leader_exit_observed = True
    return leader_exit_observed

def wait_for_leader_exit(process, duration, honor_requested_signal=False):
    global leader_exit_observed
    deadline = None if duration is None else time.monotonic() + duration
    while not leader_has_exited(process):
        if honor_requested_signal and requested_signal:
            return False
        remaining = None if deadline is None else deadline - time.monotonic()
        if remaining is not None and remaining <= 0:
            return False
        events = process_events.control(
            None, 1, 0.25 if remaining is None else min(remaining, 0.25)
        )
        if events:
            leader_exit_observed = True
    leader_exit_observed = True
    return True

def group_is_quiescent(process):
    if not leader_has_exited(process):
        return False
    return not any(pid != process.pid for pid in group_members(process.pid))

def signal_until_group_quiescent(process, signum, duration):
    deadline = time.monotonic() + duration
    while not group_is_quiescent(process):
        signal_group(process.pid, signum)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(remaining, 0.01))
    return True

def terminate_group(process):
    if not signal_until_group_quiescent(process, signal.SIGTERM, term_grace):
        if not signal_until_group_quiescent(process, signal.SIGKILL, term_grace):
            return False
    return True

def emergency_terminate_group(process):
    # Keep the leader waitable so its process-group ID cannot be recycled before
    # the final kill.  This is the last-resort path if group enumeration fails.
    signal_group(process.pid, signal.SIGKILL)
    if not wait_for_leader_exit(process, term_grace):
        signal_group(process.pid, signal.SIGKILL)
        return False
    signal_group(process.pid, signal.SIGKILL)
    return True

def publish_cleanup_confirmation(process):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(cleanup_confirmation_path, flags, 0o600)
    payload = f"cleanup-confirmed:{process.pid}\n".encode("ascii")
    try:
        os.fchmod(descriptor, 0o600)
        offset = 0
        while offset < len(payload):
            written = os.write(descriptor, payload[offset:])
            if written <= 0:
                raise OSError(errno.EIO, "cleanup confirmation write made no progress")
            offset += written
        os.fsync(descriptor)
    finally:
        os.close(descriptor)

leader_reaped = False

def reap_confirmed_group(process):
    global leader_reaped
    if not group_is_quiescent(process):
        raise RuntimeError("process group was not quiescent before leader reap")
    status = process.wait()
    leader_reaped = True
    publish_cleanup_confirmation(process)
    return status

def capture_chunk(destination, chunk):
    stream, count = destination
    available = output_limit - count
    if available > 0:
        accepted = chunk[:available]
        stream.write(accepted)
        stream.flush()
        destination[1] += len(accepted)
    return len(chunk) > available

process = None
selector = selectors.DefaultSelector()
reason = None
status = SUPERVISOR_FAILURE_STATUS
stdout_stream = open(stdout_path, "wb")
stderr_stream = open(stderr_path, "wb")
try:
    process = subprocess.Popen(
        command,
        stdin=None if mode == "sustained" else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        close_fds=True,
    )
    try:
        process_events.control([
            select.kevent(
                process.pid,
                filter=select.KQ_FILTER_PROC,
                flags=select.KQ_EV_ADD | select.KQ_EV_ENABLE,
                fflags=select.KQ_NOTE_EXIT,
            )
        ], 0, 0)
    except OSError as error:
        if error.errno != errno.ESRCH:
            raise
        leader_exit_observed = True
    if pid_fifo != "-":
        with open(pid_fifo, "w", encoding="ascii") as stream:
            stream.write(f"{process.pid}\n")
            stream.flush()
    selector.register(process.stdout, selectors.EVENT_READ, [stdout_stream, 0])
    selector.register(process.stderr, selectors.EVENT_READ, [stderr_stream, 0])
    deadline = None if timeout is None else time.monotonic() + timeout

    while selector.get_map():
        if requested_signal:
            reason = "signal"
            break
        remaining = None if deadline is None else deadline - time.monotonic()
        if remaining is not None and remaining <= 0:
            reason = "timeout"
            break
        poll_interval = 0.25 if remaining is None else min(remaining, 0.25)
        events = selector.select(poll_interval)
        if not events:
            if deadline is not None and time.monotonic() >= deadline:
                reason = "timeout"
                break
            if leader_has_exited(process):
                break
            continue
        for key, _mask in events:
            try:
                chunk = os.read(key.fd, 65536)
            except OSError as error:
                if error.errno == errno.EINTR:
                    continue
                raise
            if not chunk:
                selector.unregister(key.fileobj)
                continue
            if capture_chunk(key.data, chunk):
                reason = "output-limit"
                break
        if reason is not None:
            break

    if reason is None:
        remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
        if not wait_for_leader_exit(process, remaining, honor_requested_signal=True):
            reason = "signal" if requested_signal else "timeout"
        if reason is None:
            if not group_is_quiescent(process) and not terminate_group(process):
                reason = "cleanup-failure"
            if reason is None:
                status = reap_confirmed_group(process)
    if reason is not None and process is not None:
        try:
            terminated = terminate_group(process)
        except BaseException:
            terminated = False
        if terminated:
            status = reap_confirmed_group(process)
            if reason == "cleanup-failure":
                reason = "supervisor-failure"
        else:
            reason = "cleanup-unresolved"
except BaseException:
    recovered = process is None
    if process is not None and not leader_reaped:
        try:
            terminated = terminate_group(process)
        except BaseException:
            terminated = False
        if terminated:
            try:
                reap_confirmed_group(process)
                recovered = True
            except BaseException:
                recovered = False
        else:
            try:
                emergency_terminated = emergency_terminate_group(process)
                if emergency_terminated:
                    process.wait()
                    leader_reaped = True
                recovered = False
            except BaseException:
                recovered = False
    reason = "supervisor-failure" if recovered else "cleanup-unresolved"
finally:
    while selector.get_map():
        events = selector.select(0)
        if not events:
            break
        for key, _mask in events:
            try:
                chunk = os.read(key.fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                selector.unregister(key.fileobj)
            elif capture_chunk(key.data, chunk) and reason is None:
                reason = "output-limit"
    for registered in list(selector.get_map().values()):
        try:
            selector.unregister(registered.fileobj)
        except Exception:
            pass
    selector.close()
    process_events.close()
    stdout_stream.close()
    stderr_stream.close()

if reason == "timeout":
    raise SystemExit(TIMEOUT_STATUS)
if reason == "output-limit":
    raise SystemExit(OUTPUT_LIMIT_STATUS)
if reason == "signal":
    raise SystemExit(min(255, 128 + requested_signal))
if reason == "cleanup-unresolved":
    raise SystemExit(CLEANUP_FAILURE_STATUS)
if reason is not None:
    raise SystemExit(SUPERVISOR_FAILURE_STATUS)
if status < 0:
    raise SystemExit(min(255, 128 - status))
raise SystemExit(status)
'

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

fail()
{
    echo "$1" >&2
    exit 1
}

locate_supervisor_python()
{
    PYTHON3="$(/usr/bin/xcrun --find python3 2>/dev/null)" || return 1
    [ -f "$PYTHON3" ] && [ -x "$PYTHON3" ]
}

run_supervised_process()
{
    "${NATIVE_RUNTIME_ENV[@]}" -u PYTHONHOME -u PYTHONPATH \
        "$PYTHON3" -I -c "$PROCESS_SUPERVISOR_SOURCE" "$@"
}

marker_output_is_exact()
{
    local marker="$1"
    local output="$2"

    /usr/bin/awk -v marker="$marker" '
        {
            sub(/\r$/, "")
            lines++
            if ($0 != marker) invalid = 1
        }
        END { exit !(lines == 1 && !invalid) }
    ' "$output"
}

cleanup_confirmation_is_valid()
{
    local path="$1"
    local expected_pid="${2:-}"

    [ -f "$path" ] && [ ! -L "$path" ] || return 1
    [ "$(/usr/bin/stat -f '%Lp' "$path")" = 600 ] || return 1
    [ "$(/usr/bin/stat -f '%u' "$path")" = "$(/usr/bin/id -u)" ] || return 1
    /usr/bin/awk -v expected_pid="$expected_pid" '
        BEGIN { prefix = "cleanup-confirmed:" }
        {
            lines++
            if (index($0, prefix) != 1) invalid = 1
            pid = substr($0, length(prefix) + 1)
            if (pid !~ /^[0-9]+$/ || pid == 0) invalid = 1
            if (expected_pid != "" && pid != expected_pid) invalid = 1
        }
        END { exit !(lines == 1 && !invalid) }
    ' "$path"
}

run_marker_execution()
{
    local marker="$1"
    local stdout_path="$2"
    local stderr_path="$3"
    local cleanup_path="${stdout_path}.cleanup-confirmed"
    local status

    [ ! -e "$cleanup_path" ] && [ ! -L "$cleanup_path" ] || return 126
    if run_supervised_process bounded "$MARKER_TIMEOUT_SECONDS" \
        "$SUPERVISOR_TERM_GRACE_SECONDS" "$SUPERVISOR_OUTPUT_LIMIT_BYTES" \
        "$stdout_path" "$stderr_path" - "$cleanup_path" -- \
        "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" WINEDEBUG=-all \
        "$WINE_HOST" "$PE_CMD_EXE" /d /q /c "echo:$marker"; then
        status=0
    else
        status=$?
    fi
    if ! cleanup_confirmation_is_valid "$cleanup_path"; then
        SUPERVISOR_CLEANUP_UNCONFIRMED=1
        return 127
    fi
    [ "$status" -eq 0 ] || return "$status"
    marker_output_is_exact "$marker" "$stdout_path" || return 3
    return 0
}

architecture_violation_is_present()
{
    /usr/bin/grep -E \
        'is Rosetta translated|is not native ARM64|unexpected external helper|native architecture proof is unproven' \
        "$TEST_ROOT/probe.err" >/dev/null 2>&1
}

collect_architecture_proof()
{
    local convergence_deadline
    local probe_status=1
    local sample

    convergence_deadline=$((SECONDS + CONVERGENCE_TIMEOUT_SECONDS))
    while [ "$SECONDS" -lt "$convergence_deadline" ]; do
        /bin/kill -0 "$WINE_PID" 2>/dev/null || return 10
        if "$PROCESS_PROBE_EXECUTABLE" "$RUNTIME_ROOT" "$WINE_PID" \
            "$START_SECONDS" "$PREFIX" >"$TEST_ROOT/probe.out" \
            2>"$TEST_ROOT/probe.err"; then
            probe_status=0
        else
            probe_status=$?
        fi
        architecture_violation_is_present && return 11
        [ "$probe_status" -eq 0 ] && break
        /bin/sleep 0.1
    done
    [ "$probe_status" -eq 0 ] || return 12

    /bin/cp "$TEST_ROOT/probe.out" "$TEST_ROOT/probe.evidence" || return 126
    for ((sample = 0; sample < ARCHITECTURE_OBSERVATION_SAMPLES; sample++)); do
        if [ "$ARCHITECTURE_OBSERVATION_INTERVAL_SECONDS" != 0 ]; then
            /bin/sleep "$ARCHITECTURE_OBSERVATION_INTERVAL_SECONDS"
        fi
        if "$PROCESS_PROBE_EXECUTABLE" "$RUNTIME_ROOT" "$WINE_PID" \
            "$START_SECONDS" "$PREFIX" >"$TEST_ROOT/probe.out" \
            2>"$TEST_ROOT/probe.err"; then
            probe_status=0
        else
            probe_status=$?
        fi
        [ "$probe_status" -eq 0 ] || return 13
        /bin/cat "$TEST_ROOT/probe.out" >>"$TEST_ROOT/probe.evidence" || return 126
    done
    /bin/mv "$TEST_ROOT/probe.evidence" "$TEST_ROOT/probe.out" || return 126
    return 0
}

signal_supervisor_process()
{
    /bin/kill "$1" "$2"
}

supervisor_process_has_exited()
{
    local pid="$1"
    local state

    if state="$(/bin/ps -o state= -p "$pid" 2>/dev/null | \
        /usr/bin/awk 'NR == 1 { print $1 }')"; then
        [ -z "$state" ] || [ "${state#Z}" != "$state" ]
    else
        ! /bin/kill -0 "$pid" 2>/dev/null
    fi
}

wait_for_supervisor_exit_notice()
{
    local pid="$1"
    local attempts="$2"
    local count

    for ((count = 0; count < attempts; count++)); do
        supervisor_process_has_exited "$pid" && return 0
        /bin/sleep 0.1
    done
    return 1
}

STOPPED_SUPERVISOR_STATUS=0
stop_supervisor_process()
{
    local pid="$1"
    local cleanup_confirmation="$2"
    local expected_leader_pid="$3"
    local confirmation_valid=0

    case "$pid" in
        ''|0|*[!0-9]*) return 2 ;;
    esac
    if ! supervisor_process_has_exited "$pid"; then
        signal_supervisor_process -TERM "$pid" 2>/dev/null || true
        if ! wait_for_supervisor_exit_notice "$pid" "$SUPERVISOR_TERM_WAIT_ATTEMPTS"; then
            signal_supervisor_process -KILL "$pid" 2>/dev/null || true
            wait_for_supervisor_exit_notice "$pid" \
                "$SUPERVISOR_KILL_WAIT_ATTEMPTS" || return 2
        fi
    fi
    cleanup_confirmation_is_valid "$cleanup_confirmation" \
        "$expected_leader_pid" && confirmation_valid=1
    if wait "$pid" 2>/dev/null; then
        STOPPED_SUPERVISOR_STATUS=0
    else
        STOPPED_SUPERVISOR_STATUS=$?
    fi
    [ "$confirmation_valid" -eq 1 ]
}

build_process_probe()
{
    local output="$1"

    /usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
        -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
        "$PROBE_SOURCE" -o "$output"
}

validate_public_launcher()
{
    local runtime_root="$1"
    local launcher="$2"
    local resolved_launcher

    case "$launcher" in
        "$runtime_root/bin/switchyard-wine"|"$runtime_root/bin/wine") ;;
        *) return 1 ;;
    esac
    [ -x "$launcher" ] || return 1
    resolved_launcher="$(/usr/bin/perl -MCwd=abs_path -e '
        my $path = abs_path($ARGV[0]);
        exit 1 unless defined $path;
        print $path;
    ' "$launcher")" || return 1
    [ -f "$resolved_launcher" ] && [ ! -L "$resolved_launcher" ] &&
        [ -x "$resolved_launcher" ] || return 1
    case "$resolved_launcher" in
        "$runtime_root"/*) ;;
        *) return 1 ;;
    esac
    return 0
}

validate_runtime_file()
{
    local runtime_root="$1"
    local candidate="$2"
    local canonical="$3"
    local resolved

    [ "$candidate" = "$runtime_root/$canonical" ] || return 1
    [ -f "$candidate" ] && [ ! -L "$candidate" ] && [ -x "$candidate" ] || return 1
    resolved="$(/usr/bin/perl -MCwd=abs_path -e '
        my $path = abs_path($ARGV[0]);
        exit 1 unless defined $path;
        print $path;
    ' "$candidate")" || return 1
    case "$resolved" in
        "$runtime_root"/*) ;;
        *) return 1 ;;
    esac
}

validate_runtime_pe_file()
{
    local runtime_root="$1"
    local candidate="$2"
    local resolved

    [ -f "$candidate" ] && [ ! -L "$candidate" ] || return 1
    resolved="$(/usr/bin/perl -MCwd=abs_path -e '
        my $path = abs_path($ARGV[0]);
        exit 1 unless defined $path;
        print $path;
    ' "$candidate")" || return 1
    case "$resolved" in
        "$runtime_root"/*) ;;
        *) return 1 ;;
    esac
}

resolve_process_test_prefix()
{
    if [ "$USE_PREPARED_PREFIX" -eq 1 ]; then
        switchyard_validate_prepared_runtime_prefix \
            preview-native-arm64-fex "$PREPARED_PREFIX" PREFIX
    else
        PREFIX="$TEST_ROOT/prefix"
        mkdir -p "$PREFIX"
    fi
}

remove_no_rosetta_test_root()
{
    local test_root="$1"
    local preserve="$2"

    case "$preserve:$test_root" in
        1:*) ;;
        0:/tmp/switchyard-native-no-rosetta.??????|\
        0:/private/tmp/switchyard-native-no-rosetta.??????)
            if [ -d "$test_root" ] && [ ! -L "$test_root" ]; then
                /bin/rm -rf -- "$test_root"
            fi
            ;;
        *) echo "refusing to clean unexpected no-Rosetta test root $test_root" >&2 ;;
    esac
}

run_regression_fixtures()
{
    local fixture_root

    (
        local fixture_status
        local fixture_supervisor_pid=""
        local fixture_wine_pid=""
        local fixture_cleanup_confirmation=""
        local fixture_preserve=0
        local marker_child_pid

        fixture_root="$(/usr/bin/mktemp -d /tmp/switchyard-native-no-rosetta-fixture.XXXXXX)"
        fixture_root="$(cd "$fixture_root" && pwd -P)"
        fixture_cleanup()
        {
            [ "$SUPERVISOR_CLEANUP_UNCONFIRMED" -eq 0 ] || fixture_preserve=1
            if [ -n "$fixture_supervisor_pid" ]; then
                if ! stop_supervisor_process "$fixture_supervisor_pid" \
                    "$fixture_cleanup_confirmation" "$fixture_wine_pid"; then
                    fixture_preserve=1
                    echo "fixture supervisor cleanup lacks confirmation; preserving $fixture_root" >&2
                fi
                fixture_supervisor_pid=""
            fi
            exec 7>&- 2>/dev/null || true
            [ "$fixture_preserve" -eq 1 ] || /bin/rm -rf -- "$fixture_root"
        }
        trap fixture_cleanup EXIT HUP INT TERM
        locate_supervisor_python || fail "could not locate the Xcode Python process supervisor"
        [ -f "$PREFIX_LIBRARY" ] && [ ! -L "$PREFIX_LIBRARY" ] ||
            fail "runtime-prefix validator fixture dependency is unsafe"
        # shellcheck disable=SC1090 # Fixed repository-relative policy library.
        source "$PREFIX_LIBRARY"

        prepared_fixture="$fixture_root/prepared-prefix"
        prepared_fixture_result=""
        switchyard_prepare_runtime_prefix preview-native-arm64-fex \
            "$prepared_fixture" prepared_fixture_result
        [ "$prepared_fixture_result" = "$prepared_fixture" ] ||
            fail "prepared-prefix fixture changed its path"
        USE_PREPARED_PREFIX=1
        PREPARED_PREFIX="$prepared_fixture"
        PREFIX=""
        TEST_ROOT="$fixture_root/prefix-resolution"
        mkdir -p "$TEST_ROOT"
        resolve_process_test_prefix || fail "exact prepared prefix was rejected"
        [ "$PREFIX" = "$prepared_fixture" ] ||
            fail "prepared prefix resolved to an unexpected path"

        unmarked_fixture="$fixture_root/unmarked-prefix"
        mkdir -m 0700 "$unmarked_fixture"
        printf 'keep\n' >"$unmarked_fixture/sentinel"
        PREPARED_PREFIX="$unmarked_fixture"
        if resolve_process_test_prefix; then
            fail "unmarked supplied prefix was accepted"
        fi
        [ "$(/bin/cat "$unmarked_fixture/sentinel")" = keep ] &&
            [ ! -e "$unmarked_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER" ] ||
            fail "unmarked supplied-prefix rejection mutated the directory"

        cross_profile_fixture="$fixture_root/cross-profile-prefix"
        mkdir -m 0700 "$cross_profile_fixture"
        printf '%s\n' \
            '{"runtimeFamily":"stable-x86_64-rosetta","schemaVersion":1}' \
            >"$cross_profile_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
        chmod 0600 "$cross_profile_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
        PREPARED_PREFIX="$cross_profile_fixture"
        if resolve_process_test_prefix; then
            fail "cross-profile supplied prefix was accepted"
        fi

        forged_fixture="$fixture_root/forged-prefix"
        mkdir -m 0700 "$forged_fixture"
        printf '%s\n' \
            '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1,"forged":true}' \
            >"$forged_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
        chmod 0600 "$forged_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
        forged_before="$(shasum -a 256 \
            "$forged_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER")"
        PREPARED_PREFIX="$forged_fixture"
        if resolve_process_test_prefix; then
            fail "forged supplied-prefix marker was accepted"
        fi
        [ "$(shasum -a 256 \
            "$forged_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER")" = "$forged_before" ] ||
            fail "forged marker rejection rewrote supplied bytes"

        readonly_fixture="$fixture_root/readonly-marker-prefix"
        mkdir -m 0700 "$readonly_fixture"
        printf '%s\n' \
            '{"runtimeFamily":"preview-native-arm64-fex","schemaVersion":1}' \
            >"$readonly_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
        chmod 0400 "$readonly_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER"
        PREPARED_PREFIX="$readonly_fixture"
        if resolve_process_test_prefix; then
            fail "read-only supplied-prefix marker was accepted"
        fi
        [ "$(/usr/bin/stat -f '%Lp' \
            "$readonly_fixture/$SWITCHYARD_RUNTIME_PREFIX_MARKER")" = 400 ] ||
            fail "read-only marker rejection changed its mode"

        symlink_fixture="$fixture_root/symlink-prefix"
        /bin/ln -s "$prepared_fixture" "$symlink_fixture"
        PREPARED_PREFIX="$symlink_fixture"
        if resolve_process_test_prefix; then
            fail "symbolic-link supplied prefix was accepted"
        fi

        PREPARED_PREFIX="$prepared_fixture"
        success_logs="$(/usr/bin/mktemp -d \
            /tmp/switchyard-native-no-rosetta.XXXXXX)"
        success_logs="$(cd "$success_logs" && pwd -P)"
        TEST_ROOT="$success_logs"
        resolve_process_test_prefix || fail "prepared prefix failed success cleanup fixture"
        remove_no_rosetta_test_root "$success_logs" 0
        [ ! -e "$success_logs" ] &&
            [ -d "$prepared_fixture" ] && [ ! -L "$prepared_fixture" ] ||
            fail "success cleanup removed the supplied prefix or retained its log root"
        switchyard_validate_prepared_runtime_prefix preview-native-arm64-fex \
            "$prepared_fixture" verified_after_success ||
            fail "success cleanup changed the supplied marker"

        failure_logs="$(/usr/bin/mktemp -d \
            /tmp/switchyard-native-no-rosetta.XXXXXX)"
        failure_logs="$(cd "$failure_logs" && pwd -P)"
        TEST_ROOT="$failure_logs"
        resolve_process_test_prefix || fail "prepared prefix failed failure cleanup fixture"
        remove_no_rosetta_test_root "$failure_logs" 1
        [ -d "$failure_logs" ] && [ -d "$prepared_fixture" ] ||
            fail "failure cleanup removed logs or the supplied prefix"
        switchyard_validate_prepared_runtime_prefix preview-native-arm64-fex \
            "$prepared_fixture" verified_after_failure ||
            fail "failure cleanup changed the supplied marker"
        remove_no_rosetta_test_root "$failure_logs" 0

        legacy_logs="$(/usr/bin/mktemp -d \
            /tmp/switchyard-native-no-rosetta.XXXXXX)"
        legacy_logs="$(cd "$legacy_logs" && pwd -P)"
        USE_PREPARED_PREFIX=0
        PREPARED_PREFIX=""
        TEST_ROOT="$legacy_logs"
        PREFIX=""
        resolve_process_test_prefix || fail "legacy prefix fixture could not create its prefix"
        [ "$PREFIX" = "$legacy_logs/prefix" ] && [ -d "$PREFIX" ] ||
            fail "legacy prefix fixture changed its owned temporary path"
        remove_no_rosetta_test_root "$legacy_logs" 0
        [ ! -e "$legacy_logs" ] || fail "legacy prefix fixture did not clean its test root"

        "${NATIVE_RUNTIME_ENV[@]}" -u PYTHONHOME -u PYTHONPATH \
            "$PYTHON3" -I -c "$PROCESS_SUPERVISOR_SOURCE" \
            --regression-fixture-zero-errno ||
            fail "zero-plus-errno process-group inspection fixture did not fail closed"
        mkdir -p "$fixture_root/runtime/bin"
        printf '#!/usr/bin/env bash\nexit 0\n' >"$fixture_root/runtime/bin/wine"
        chmod 0755 "$fixture_root/runtime/bin/wine"
        /bin/ln -s wine "$fixture_root/runtime/bin/switchyard-wine"
        validate_public_launcher "$fixture_root/runtime" \
            "$fixture_root/runtime/bin/switchyard-wine" ||
            fail "safe public launcher symlink fixture was rejected"
        /bin/rm "$fixture_root/runtime/bin/switchyard-wine"
        /bin/ln -s /bin/sh "$fixture_root/runtime/bin/switchyard-wine"
        if validate_public_launcher "$fixture_root/runtime" \
            "$fixture_root/runtime/bin/switchyard-wine"; then
            fail "external public launcher symlink fixture was accepted"
        fi

        printf '%s\n' \
            '#!/usr/bin/env bash' \
            'set -euo pipefail' \
            'last_argument="${!#}"' \
            'marker="${last_argument#echo:}"' \
            'case "${SWITCHYARD_MARKER_FIXTURE:-}" in' \
            '    correct) printf "%s\\r\\n" "$marker" ;;' \
            '    missing) : ;;' \
            '    wrong) printf "WRONG_MARKER\\r\\n" ;;' \
            '    timeout)' \
            '        /bin/sleep 30 &' \
            '        printf "%s\\n" "$!" >"$SWITCHYARD_MARKER_CHILD_PID_FILE"' \
            '        wait' \
            '        ;;' \
            '    *) exit 70 ;;' \
            'esac' >"$fixture_root/fake-wine"
        chmod 0755 "$fixture_root/fake-wine"
        printf 'fixture PE command\n' >"$fixture_root/cmd.exe"
        PREFIX="$fixture_root/marker-prefix"
        WINE_HOST="$fixture_root/fake-wine"
        PE_CMD_EXE="$fixture_root/cmd.exe"
        mkdir -p "$PREFIX"
        export SWITCHYARD_MARKER_FIXTURE=correct
        run_marker_execution FIXTURE_MARKER "$fixture_root/marker.out" \
            "$fixture_root/marker.err" ||
            fail "bounded marker execution rejected exact /c output"
        export SWITCHYARD_MARKER_FIXTURE=missing
        if run_marker_execution FIXTURE_MARKER "$fixture_root/missing.out" \
            "$fixture_root/missing.err"; then
            fail "bounded marker execution accepted missing /c output"
        else
            fixture_status=$?
        fi
        [ "$fixture_status" -eq 3 ] ||
            fail "missing /c marker did not fail exact-output validation"
        export SWITCHYARD_MARKER_FIXTURE=wrong
        if run_marker_execution FIXTURE_MARKER "$fixture_root/wrong.out" \
            "$fixture_root/wrong.err"; then
            fail "bounded marker execution accepted wrong /c output"
        else
            fixture_status=$?
        fi
        [ "$fixture_status" -eq 3 ] ||
            fail "wrong /c marker did not fail exact-output validation"
        MARKER_TIMEOUT_SECONDS=1
        export SWITCHYARD_MARKER_FIXTURE=timeout
        export SWITCHYARD_MARKER_CHILD_PID_FILE="$fixture_root/marker-child.pid"
        if run_marker_execution FIXTURE_MARKER "$fixture_root/timeout.out" \
            "$fixture_root/timeout.err"; then
            fail "bounded marker execution did not time out"
        else
            fixture_status=$?
        fi
        [ "$fixture_status" -eq 124 ] ||
            fail "bounded marker timeout did not return status 124"
        [ -s "$SWITCHYARD_MARKER_CHILD_PID_FILE" ] ||
            fail "bounded marker timeout fixture did not launch its descendant"
        marker_child_pid="$(/bin/cat "$SWITCHYARD_MARKER_CHILD_PID_FILE")"
        case "$marker_child_pid" in
            ''|*[!0-9]*) fail "bounded marker timeout recorded an invalid descendant PID" ;;
        esac
        cleanup_confirmation_is_valid \
            "$fixture_root/timeout.out.cleanup-confirmed" ||
            fail "bounded marker timeout lacked descendant-cleanup confirmation"
        unset SWITCHYARD_MARKER_FIXTURE SWITCHYARD_MARKER_CHILD_PID_FILE

        if ! (
            exec 2>"$fixture_root/supervisor-loss.stderr"
            loss_audit="$fixture_root/supervisor-loss-signals"
            loss_ready="$fixture_root/supervisor-loss-ready"
            loss_confirmation="$fixture_root/supervisor-loss-confirmation"
            signal_supervisor_process()
            {
                printf '%s %s\n' "$1" "$2" >>"$loss_audit"
                /bin/kill "$1" "$2"
            }
            "${NATIVE_RUNTIME_ENV[@]}" -u PYTHONHOME -u PYTHONPATH \
                "$PYTHON3" -I -c '
import os
import signal
import sys
signal.signal(signal.SIGTERM, signal.SIG_IGN)
descriptor = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
os.close(descriptor)
while True:
    signal.pause()
' "$loss_ready" &
            loss_supervisor_pid=$!
            for _ in {1..100}; do
                [ -f "$loss_ready" ] && break
                /bin/sleep 0.01
            done
            [ -f "$loss_ready" ] || fail "supervisor-loss fixture did not become ready"
            SUPERVISOR_TERM_WAIT_ATTEMPTS=1
            if stop_supervisor_process "$loss_supervisor_pid" "$loss_confirmation" \
                999999; then
                fail "supervisor loss was accepted without cleanup confirmation"
            else
                loss_status=$?
            fi
            [ "$loss_status" -eq 1 ] ||
                fail "supervisor loss did not fail solely on missing cleanup confirmation"
            /usr/bin/awk -v pid="$loss_supervisor_pid" '
                {
                    lines++
                    if (($1 != "-TERM" && $1 != "-KILL") || $2 != pid || $2 ~ /^-/)
                        invalid = 1
                    if ($1 == "-TERM") term = 1
                    if ($1 == "-KILL") kill = 1
                }
                END { exit !(lines == 2 && term && kill && !invalid) }
            ' "$loss_audit" ||
                fail "supervisor-loss cleanup signaled outside direct supervisor authority"
        ); then
            /bin/cat "$fixture_root/supervisor-loss.stderr" >&2
            fail "supervisor-loss regression fixture failed"
        fi

        build_process_probe "$fixture_root/process-probe"
        "$fixture_root/process-probe" --regression-fixture-exited-matched-process \
            >"$fixture_root/fixture.out" 2>"$fixture_root/fixture.err" ||
            fail "exited matched-process regression fixture did not reject its evidence"
        /usr/bin/grep -F 'is Rosetta translated' "$fixture_root/fixture.err" >/dev/null ||
            fail "exited matched-process fixture did not inspect captured Rosetta evidence"
        /usr/bin/grep -F 'unexpected external helper' "$fixture_root/fixture.err" >/dev/null ||
            fail "exited matched-process fixture did not inspect captured external-helper evidence"
        /usr/bin/grep -F 'native architecture proof is unproven' "$fixture_root/fixture.err" >/dev/null ||
            fail "exited matched-process fixture did not fail closed"
        "$fixture_root/process-probe" --regression-fixture-wrapper-bootstrap \
            >"$fixture_root/wrapper.out" 2>"$fixture_root/wrapper.err" ||
            fail "wrapper-bootstrap regression fixture did not preserve its narrow exception"
        /usr/bin/grep -F 'wrapperBootstrap=true' "$fixture_root/wrapper.out" >/dev/null ||
            fail "wrapper-bootstrap fixture did not report its explicit exception"
        /usr/bin/grep -F 'translated=true' "$fixture_root/wrapper.out" >/dev/null ||
            fail "wrapper-bootstrap fixture did not exercise captured translation evidence"
        /usr/bin/grep -F 'is Rosetta translated' "$fixture_root/wrapper.err" >/dev/null ||
            fail "wrapper-bootstrap fixture bypassed captured Rosetta rejection"
        /usr/bin/grep -F 'is not native ARM64' "$fixture_root/wrapper.err" >/dev/null ||
            fail "wrapper-bootstrap fixture bypassed PIDARCHINFO rejection"
        if /usr/bin/grep -F 'unexpected external helper' "$fixture_root/wrapper.err" >/dev/null; then
            fail "trusted wrapper-bootstrap fixture was still treated as an external helper"
        fi
        "$fixture_root/process-probe" --regression-fixture-process-list-capacity \
            >"$fixture_root/capacity.out" 2>"$fixture_root/capacity.err" ||
            fail "process-list allocation bound regression fixture failed"

        printf '%s\n' \
            '#!/usr/bin/env bash' \
            'printf "%s\\n" "pid=$2 cpuType=0x100000c translated=false runtime=true descendant=true prefix=true"' \
            >"$fixture_root/native-probe"
        chmod 0755 "$fixture_root/native-probe"
        printf '%s\n' \
            '#!/usr/bin/env bash' \
            'printf "%s\\n" "pid=$2 cpuType=0x1000007 translated=true runtime=true descendant=true prefix=true"' \
            'printf "%s\\n" "pid $2 is Rosetta translated" >&2' \
            'exit 1' >"$fixture_root/translated-probe"
        chmod 0755 "$fixture_root/translated-probe"
        TEST_ROOT="$fixture_root/sustained"
        PREFIX="$TEST_ROOT/prefix"
        RUNTIME_ROOT="$fixture_root/runtime"
        START_SECONDS="$(/bin/date +%s)"
        PROCESS_PROBE_EXECUTABLE="$fixture_root/native-probe"
        CONVERGENCE_TIMEOUT_SECONDS=2
        ARCHITECTURE_OBSERVATION_SAMPLES=1
        ARCHITECTURE_OBSERVATION_INTERVAL_SECONDS=0
        mkdir -p "$TEST_ROOT" "$PREFIX"
        /usr/bin/mkfifo "$TEST_ROOT/pid"
        exec 7<>"$TEST_ROOT/pid"
        fixture_cleanup_confirmation="$TEST_ROOT/cleanup-confirmed"
        "${NATIVE_RUNTIME_ENV[@]}" -u PYTHONHOME -u PYTHONPATH \
            "$PYTHON3" -I -c "$PROCESS_SUPERVISOR_SOURCE" sustained - \
            "$SUPERVISOR_TERM_GRACE_SECONDS" "$SUPERVISOR_OUTPUT_LIMIT_BYTES" \
            "$TEST_ROOT/wine.out" "$TEST_ROOT/wine.err" "$TEST_ROOT/pid" \
            "$fixture_cleanup_confirmation" -- \
            /bin/sh -c '/bin/sleep 2; printf "BUFFERED_K_OUTPUT\\n"; /bin/sleep 30' &
        fixture_supervisor_pid=$!
        if ! IFS= read -r -t 5 WINE_PID <&7; then
            fail "sustained supervisor did not publish its process authority"
        fi
        case "$WINE_PID" in
            ''|*[!0-9]*) fail "sustained supervisor published an invalid process authority" ;;
        esac
        fixture_wine_pid="$WINE_PID"
        collect_architecture_proof ||
            fail "buffered sustained stdout blocked valid architecture proof"
        [ ! -s "$TEST_ROOT/wine.out" ] ||
            fail "buffered sustained stdout fixture emitted before architecture proof"
        PROCESS_PROBE_EXECUTABLE="$fixture_root/translated-probe"
        if collect_architecture_proof; then
            fail "architecture proof accepted a translated process"
        else
            fixture_status=$?
        fi
        [ "$fixture_status" -eq 11 ] ||
            fail "architecture violation did not fail the sustained proof"
        stop_supervisor_process "$fixture_supervisor_pid" \
            "$fixture_cleanup_confirmation" "$fixture_wine_pid" ||
            fail "sustained supervisor did not confirm complete group cleanup"
        [ "$STOPPED_SUPERVISOR_STATUS" -eq 143 ] ||
            fail "sustained supervisor returned an unexpected termination status"
        fixture_supervisor_pid=""
        fixture_wine_pid=""
        fixture_cleanup_confirmation=""
        exec 7>&-
    )
}

if [ "$#" -eq 1 ] && [ "$1" = "--regression-fixtures" ]; then
    [ "$(/usr/bin/uname -s)" = Darwin ] || fail "no-Rosetta regression fixtures require macOS"
    [ "$(/usr/bin/uname -m)" = arm64 ] || fail "no-Rosetta regression fixtures require native arm64 shell"
    run_regression_fixtures
    echo "native no-Rosetta regression fixtures passed"
    exit 0
fi

if [ "${1:-}" = "--prefix" ]; then
    [ "$#" -ge 2 ] || {
        echo "usage: $0 [--prefix PREPARED_PREFIX] RUNTIME_ROOT WINE_HOST WINESERVER PE_CMD_EXE [OUTPUT_MARKER]" >&2
        exit 2
    }
    USE_PREPARED_PREFIX=1
    PREPARED_PREFIX="$2"
    shift 2
fi

if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 [--prefix PREPARED_PREFIX] RUNTIME_ROOT WINE_HOST WINESERVER PE_CMD_EXE [OUTPUT_MARKER]" >&2
    exit 2
fi

RUNTIME_ROOT="$1"
WINE_HOST="$2"
WINESERVER="$3"
PE_CMD_EXE="$4"
OUTPUT_MARKER="${5:-}"
TEST_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-native-no-rosetta.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
PREFIX=""
FIFO="$TEST_ROOT/cmd-input"
PID_FIFO="$TEST_ROOT/cmd-pid"
CLEANUP_CONFIRMATION="$TEST_ROOT/cmd-cleanup-confirmed"
WINE_SUPERVISOR_PID=""
WINE_PID=""
START_SECONDS=""
PRESERVE_TEST_ROOT=0
PREFIX_WINESERVER_STOPPED=0
WINESERVER_VALIDATED=0
# Keep execution evidence separate from the sustained architecture sample.
# cmd.exe may buffer redirected interactive output during fresh-prefix startup,
# while /c has a complete exit status and flush boundary.  The harmless `rem`
# command enters /k without making stdout part of the liveness contract.
CMD_ARGUMENTS=(/d /q /k rem)

# shellcheck disable=SC2329 # Invoked through the EXIT and signal traps.
cleanup()
{
    [ "$SUPERVISOR_CLEANUP_UNCONFIRMED" -eq 0 ] || PRESERVE_TEST_ROOT=1
    exec 9>&- 2>/dev/null || true
    exec 8>&- 2>/dev/null || true
    if [ -n "$WINE_SUPERVISOR_PID" ]; then
        if ! stop_supervisor_process "$WINE_SUPERVISOR_PID" \
            "$CLEANUP_CONFIRMATION" "$WINE_PID"; then
            echo "native ARM64 cmd cleanup lacks supervisor confirmation; preserving $TEST_ROOT" >&2
            PRESERVE_TEST_ROOT=1
        fi
        WINE_SUPERVISOR_PID=""
    fi
    if [ "$WINESERVER_VALIDATED" -eq 1 ] &&
       [ "$PREFIX_WINESERVER_STOPPED" -eq 0 ] && [ -d "$PREFIX" ]; then
        if "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" \
            "$WINESERVER" -k 2>/dev/null &&
           "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" \
            "$WINESERVER" -w 2>/dev/null; then
            PREFIX_WINESERVER_STOPPED=1
        else
            echo "native ARM64 prefix wineserver did not stop cleanly; preserving $TEST_ROOT" >&2
            PRESERVE_TEST_ROOT=1
        fi
    fi
    remove_no_rosetta_test_root "$TEST_ROOT" "$PRESERVE_TEST_ROOT"
}
trap cleanup EXIT HUP INT TERM

[ "$(/usr/bin/uname -s)" = Darwin ] || fail "no-Rosetta process test requires macOS"
[ "$(/usr/bin/uname -m)" = arm64 ] || fail "no-Rosetta process test requires native arm64 shell"
locate_supervisor_python || fail "could not locate the Xcode Python process supervisor"
[ -d "$RUNTIME_ROOT" ] && [ ! -L "$RUNTIME_ROOT" ] || fail "runtime root is missing or unsafe"
RUNTIME_ROOT="$(cd "$RUNTIME_ROOT" && pwd -P)"
[ "$RUNTIME_ROOT" != / ] || fail "runtime root is unbounded"
[ -f "$PROFILE_LIBRARY" ] && [ ! -L "$PROFILE_LIBRARY" ] ||
    fail "runtime-profile validator is missing or unsafe"
# shellcheck disable=SC1090 # Fixed repository-relative policy library.
source "$PROFILE_LIBRARY"
[ -f "$PREFIX_LIBRARY" ] && [ ! -L "$PREFIX_LIBRARY" ] ||
    fail "runtime-prefix validator is missing or unsafe"
# shellcheck disable=SC1090 # Fixed repository-relative policy library.
source "$PREFIX_LIBRARY"
RUNTIME_MANIFEST="$RUNTIME_ROOT/switchyard-runtime.json"
switchyard_validate_runtime_manifest_profile \
    "$RUNTIME_MANIFEST" preview-native-arm64-fex "$RUNTIME_ROOT" ||
    fail "runtime manifest does not qualify as preview-native-arm64-fex"
validate_public_launcher "$RUNTIME_ROOT" "$WINE_HOST" ||
    fail "Wine host is not a safe public runtime launcher"
validate_runtime_file "$RUNTIME_ROOT" "$WINESERVER" bin/wineserver ||
    fail "wineserver is not the exact safe runtime executable"
WINESERVER_VALIDATED=1
validate_runtime_pe_file "$RUNTIME_ROOT" "$PE_CMD_EXE" ||
    fail "PE cmd.exe is not a safe runtime file"
case "$OUTPUT_MARKER" in
    "") ;;
    *[!A-Z0-9_-]*) fail "output marker contains a command-unsafe character" ;;
esac
[ "${#OUTPUT_MARKER}" -le 64 ] || fail "output marker exceeds 64 characters"

build_process_probe "$TEST_ROOT/process-probe"
PROCESS_PROBE_EXECUTABLE="$TEST_ROOT/process-probe"
resolve_process_test_prefix || fail "runtime prefix is not an exact prepared preview prefix"
START_SECONDS="$(/bin/date +%s)"

if [ -n "$OUTPUT_MARKER" ]; then
    # This same-prefix phase proves PE execution before the markerless process
    # is kept alive for host-architecture inspection.
    if run_marker_execution "$OUTPUT_MARKER" "$TEST_ROOT/marker.out" \
        "$TEST_ROOT/marker.err"; then
        marker_status=0
    else
        marker_status=$?
    fi
    if [ "$marker_status" -ne 0 ]; then
        /bin/cat "$TEST_ROOT/marker.out" >&2 2>/dev/null || true
        /bin/cat "$TEST_ROOT/marker.err" >&2 2>/dev/null || true
        case "$marker_status" in
            3) fail "native PE cmd.exe did not emit exactly its requested /c marker" ;;
            124) fail "native PE cmd.exe marker execution timed out" ;;
            125) fail "native PE cmd.exe marker execution exceeded its output bound" ;;
            126) fail "native PE cmd.exe marker supervisor failed closed" ;;
            127) fail "native PE cmd.exe marker cleanup failed closed" ;;
            *) fail "native PE cmd.exe marker execution exited with $marker_status" ;;
        esac
    fi
fi

/usr/bin/mkfifo "$FIFO"
/usr/bin/mkfifo "$PID_FIFO"
exec 9<>"$FIFO"
exec 8<>"$PID_FIFO"
"${NATIVE_RUNTIME_ENV[@]}" -u PYTHONHOME -u PYTHONPATH \
    "$PYTHON3" -I -c "$PROCESS_SUPERVISOR_SOURCE" sustained - \
    "$SUPERVISOR_TERM_GRACE_SECONDS" "$SUPERVISOR_OUTPUT_LIMIT_BYTES" \
    "$TEST_ROOT/wine.out" "$TEST_ROOT/wine.err" "$PID_FIFO" \
    "$CLEANUP_CONFIRMATION" -- \
    "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" WINEDEBUG=-all \
    "$WINE_HOST" "$PE_CMD_EXE" "${CMD_ARGUMENTS[@]}" <"$FIFO" &
WINE_SUPERVISOR_PID=$!
if ! IFS= read -r -t 10 WINE_PID <&8; then
    fail "native ARM64 cmd supervisor did not publish process authority"
fi
case "$WINE_PID" in
    ''|*[!0-9]*) fail "native ARM64 cmd supervisor published invalid process authority" ;;
esac

if collect_architecture_proof; then
    architecture_status=0
else
    architecture_status=$?
fi
if [ "$architecture_status" -ne 0 ]; then
    /bin/cat "$TEST_ROOT/probe.out" >&2 2>/dev/null || true
    /bin/cat "$TEST_ROOT/probe.err" >&2 2>/dev/null || true
    /bin/cat "$TEST_ROOT/wine.out" >&2 2>/dev/null || true
    /bin/cat "$TEST_ROOT/wine.err" >&2 2>/dev/null || true
    case "$architecture_status" in
        10) fail "native ARM64 Wine process exited before architecture inspection" ;;
        11) fail "runtime process architecture inspection found a forbidden host process" ;;
        12) fail "runtime process architecture proof did not converge" ;;
        13) fail "runtime process architecture changed during the observation window" ;;
        *) fail "runtime process architecture supervisor failed closed" ;;
    esac
fi
/usr/bin/grep -F 'translated=true' "$TEST_ROOT/probe.out" >/dev/null &&
    fail "runtime architecture evidence contains a Rosetta-translated process"
/usr/bin/grep -F 'cpuType=0x100000c ' "$TEST_ROOT/probe.out" >/dev/null ||
    fail "runtime architecture evidence contains no native ARM64 process"

printf 'exit\r\n' >&9
exec 9>&-
if ! wait_for_supervisor_exit_notice "$WINE_SUPERVISOR_PID" 100; then
    /bin/cat "$TEST_ROOT/wine.out" >&2
    /bin/cat "$TEST_ROOT/wine.err" >&2
    fail "native ARM64 cmd supervisor did not confirm cleanup within 10 seconds"
fi
if ! cleanup_confirmation_is_valid "$CLEANUP_CONFIRMATION" "$WINE_PID"; then
    PRESERVE_TEST_ROOT=1
    unconfirmed_supervisor_pid="$WINE_SUPERVISOR_PID"
    WINE_SUPERVISOR_PID=""
    wait "$unconfirmed_supervisor_pid" 2>/dev/null || true
    fail "native ARM64 cmd supervisor did not confirm complete process-group cleanup"
fi
completed_supervisor_pid="$WINE_SUPERVISOR_PID"
WINE_SUPERVISOR_PID=""
set +e
wait "$completed_supervisor_pid"
wine_status=$?
set -e
WINE_PID=""
if [ "$wine_status" -ne 0 ]; then
    /bin/cat "$TEST_ROOT/wine.out" >&2
    /bin/cat "$TEST_ROOT/wine.err" >&2
    fail "native ARM64 cmd host exited with $wine_status"
fi

if "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" \
    "$WINESERVER" -k 2>/dev/null &&
   "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$PREFIX" \
    "$WINESERVER" -w 2>/dev/null; then
    PREFIX_WINESERVER_STOPPED=1
else
    PRESERVE_TEST_ROOT=1
    fail "native ARM64 prefix wineserver did not stop and quiesce"
fi

[ -z "$OUTPUT_MARKER" ] || printf '%s\n' "$OUTPUT_MARKER"
/bin/cat "$TEST_ROOT/probe.out"
echo "Native ARM64 runtime process set contains no Rosetta-translated host process."
