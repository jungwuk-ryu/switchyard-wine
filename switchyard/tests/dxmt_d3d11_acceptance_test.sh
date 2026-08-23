#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
GUEST_SELECTION="${2:-all}"

build_acceptance_guest() {
  local guest="$1"
  local output="$2"
  local compiler

  case "$guest" in
    x86_64) compiler=x86_64-w64-mingw32-gcc ;;
    i386) compiler=i686-w64-mingw32-gcc ;;
    *) return 2 ;;
  esac
  command -v "$compiler" >/dev/null 2>&1 || {
    echo "missing required DXMT acceptance compiler: $compiler" >&2
    return 1
  }
  "$compiler" -Wall -Wextra -Werror -O2 \
    -o "$output" \
    "$ROOT_DIR/switchyard/tests/dxmt_d3d11_acceptance.c" \
    -ld3d11 -ldxgi -luuid -lgdi32 -luser32 -lpsapi
}

build_loaded_image_probe() {
  local output="$1"

  /usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
    "$ROOT_DIR/switchyard/tests/native_no_rosetta_process_probe.c" \
    -o "$output"
}

run_regression_fixtures() (
  local fixture_root probe

  fixture_root="$(/usr/bin/mktemp -d /private/tmp/switchyard-dxmt-libproc-fixture.XXXXXX)"
  fixture_root="$(cd "$fixture_root" && pwd -P)"
  cleanup_fixture() {
    local status=$?

    trap - EXIT HUP INT TERM
    case "$fixture_root" in
      /private/tmp/switchyard-dxmt-libproc-fixture.??????)
        [ ! -L "$fixture_root" ] && /bin/rm -rf -- "$fixture_root"
        ;;
      *) echo "refusing to clean unexpected DXMT libproc fixture root $fixture_root" >&2 ;;
    esac
    exit "$status"
  }
  trap cleanup_fixture EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
  probe="$fixture_root/loaded-image-probe"
  build_loaded_image_probe "$probe"
  "$probe" --regression-fixture-loaded-images
  "$probe" --regression-self-loaded-image
  build_acceptance_guest x86_64 "$fixture_root/dxmt-d3d11-x86_64.exe"
  build_acceptance_guest i386 "$fixture_root/dxmt-d3d11-i386.exe"
  echo "DXMT loaded-image and PE build regression fixtures passed"
)

if [ "$#" -eq 1 ] && [ "$1" = "--regression-fixtures" ]; then
  run_regression_fixtures
  exit 0
fi

[ "$#" -le 2 ] || {
  echo "usage: $0 RUNTIME [all|x86_64|i386]" >&2
  exit 2
}

[ -n "$RUNTIME" ] || {
  echo "usage: $0 RUNTIME [all|x86_64|i386]" >&2
  exit 2
}
case "$RUNTIME" in
  /*) ;;
  *) echo "DXMT acceptance runtime path must be absolute." >&2; exit 2 ;;
esac
[ -d "$RUNTIME" ] && [ ! -L "$RUNTIME" ] || {
  echo "DXMT acceptance runtime root is missing or unsafe." >&2
  exit 1
}
RUNTIME="$(cd "$RUNTIME" && pwd -P)"
[ "$RUNTIME" != / ] || {
  echo "DXMT acceptance runtime root is unbounded." >&2
  exit 1
}
[ "$EUID" -ne 0 ] || {
  echo "DXMT acceptance refuses to launch a Windows guest as root." >&2
  exit 1
}
[ "$(/usr/bin/uname -s)" = Darwin ] && [ "$(/usr/bin/uname -m)" = arm64 ] || {
  echo "DXMT acceptance requires a native arm64 macOS shell." >&2
  exit 1
}
case "$GUEST_SELECTION" in
  all|x86_64|i386) ;;
  *) echo "DXMT acceptance guest must be all, x86_64, or i386." >&2; exit 2 ;;
esac

manifest="$RUNTIME/switchyard-runtime.json"
[ -f "$manifest" ] && [ ! -L "$manifest" ] || {
  echo "DXMT acceptance runtime has no regular manifest." >&2
  exit 1
}
[ "$(/usr/bin/plutil -extract runtimeFamily raw -o - "$manifest" 2>/dev/null || true)" = \
  preview-native-arm64-fex ] || {
  echo "DXMT acceptance requires the preview-native-arm64-fex runtime family." >&2
  exit 1
}
[ "$(/usr/bin/plutil -extract graphicsBackend raw -o - "$manifest" 2>/dev/null || true)" = \
  dxmt-metal ] || {
  echo "DXMT acceptance runtime did not select dxmt-metal." >&2
  exit 1
}
[ "$(/usr/bin/plutil -extract host.architecture raw -o - "$manifest" 2>/dev/null || true)" = \
  arm64 ] || {
  echo "DXMT acceptance runtime host is not native arm64." >&2
  exit 1
}

DXMT_LIBRARY="$ROOT_DIR/switchyard/lib/dxmt_artifact.sh"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
PROVIDER_LIBRARY="$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
for policy_library in "$DXMT_LIBRARY" "$PROFILE_LIBRARY" "$PROVIDER_LIBRARY"; do
  [ -f "$policy_library" ] && [ ! -L "$policy_library" ] || {
    echo "DXMT acceptance policy library is missing or unsafe: $policy_library" >&2
    exit 1
  }
done
# shellcheck disable=SC1090 # Fixed repository-relative policy libraries.
source "$DXMT_LIBRARY"
# shellcheck disable=SC1090
source "$PROFILE_LIBRARY"
# shellcheck disable=SC1090
source "$PROVIDER_LIBRARY"
declare -F switchyard_validate_dxmt_runtime_manifest >/dev/null || {
  echo "DXMT artifact validator does not expose its runtime-manifest gate." >&2
  exit 1
}
declare -F switchyard_validate_wow64_unixlib_policy_manifest >/dev/null || {
  echo "Wow64 Unixlib validator does not expose its closure gate." >&2
  exit 1
}
declare -F switchyard_validate_native_cpu_provider_files >/dev/null || {
  echo "Native CPU-provider validator does not expose its file-closure gate." >&2
  exit 1
}
switchyard_validate_runtime_manifest_profile \
  "$manifest" preview-native-arm64-fex "$RUNTIME"
switchyard_validate_dxmt_runtime_manifest "$RUNTIME" "$manifest"
switchyard_validate_native_cpu_provider_files "$manifest" "$RUNTIME"
switchyard_validate_wow64_unixlib_policy_manifest \
  "$RUNTIME" "$manifest" "$ROOT_DIR"
if [ -e "$RUNTIME/lib/wine/x86_64-unix/winemetal.so" ]; then
  echo "Native DXMT runtime unexpectedly stages the Rosetta x86_64 Unix library." >&2
  exit 1
fi

validate_runtime_executable() {
  local candidate="$1"
  local canonical="$2"
  local allow_symlink="$3"
  local resolved

  [ "$candidate" = "$RUNTIME/$canonical" ] || return 1
  [ -x "$candidate" ] || return 1
  if [ "$allow_symlink" != true ]; then
    [ -f "$candidate" ] && [ ! -L "$candidate" ] || return 1
  fi
  resolved="$(/usr/bin/perl -MCwd=abs_path -e '
    my $path = abs_path($ARGV[0]);
    exit 1 unless defined $path;
    print $path;
  ' "$candidate")" || return 1
  [ -f "$resolved" ] && [ ! -L "$resolved" ] && [ -x "$resolved" ] || return 1
  case "$resolved" in
    "$RUNTIME"/*) ;;
    *) return 1 ;;
  esac
}

validate_runtime_executable "$RUNTIME/bin/switchyard-wine" bin/switchyard-wine true || {
  echo "DXMT acceptance launcher is missing or unsafe." >&2
  exit 1
}
validate_runtime_executable "$RUNTIME/bin/wineserver" bin/wineserver false || {
  echo "DXMT acceptance wineserver is missing or unsafe." >&2
  exit 1
}

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
work="$(/usr/bin/mktemp -d /private/tmp/switchyard-dxmt-d3d11.XXXXXX)"
work="$(cd "$work" && pwd -P)"
ACTIVE_GUEST_PID=""
ACTIVE_GUEST_PREFIX=""
ACTIVE_PROBE_PID=""
ACTIVE_CLEANUP_PID=""
ACTIVE_FIFO_OPEN=0

process_has_exited() {
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

wait_for_process_exit() {
  local pid="$1"
  local attempts="$2"
  local count

  for ((count = 0; count < attempts; ++count)); do
    if process_has_exited "$pid"; then
      return 0
    fi
    /bin/sleep 0.1
  done
  return 1
}

run_bounded_wineserver() {
  local prefix="$1"
  local operation="$2"
  local status=0

  "${NATIVE_RUNTIME_ENV[@]}" WINEPREFIX="$prefix" \
    "$RUNTIME/bin/wineserver" "$operation" >/dev/null 2>&1 &
  ACTIVE_CLEANUP_PID=$!
  if ! wait_for_process_exit "$ACTIVE_CLEANUP_PID" 50; then
    /bin/kill -TERM "$ACTIVE_CLEANUP_PID" 2>/dev/null || true
    if ! wait_for_process_exit "$ACTIVE_CLEANUP_PID" 20; then
      /bin/kill -KILL "$ACTIVE_CLEANUP_PID" 2>/dev/null || true
      wait_for_process_exit "$ACTIVE_CLEANUP_PID" 20 || true
    fi
    status=1
  fi
  wait "$ACTIVE_CLEANUP_PID" 2>/dev/null || status=$?
  ACTIVE_CLEANUP_PID=""
  return "$status"
}

cleanup_active_guest() {
  if [ "$ACTIVE_FIFO_OPEN" -eq 1 ]; then
    exec 9>&-
    ACTIVE_FIFO_OPEN=0
  fi
  if [ -n "$ACTIVE_PROBE_PID" ]; then
    if ! process_has_exited "$ACTIVE_PROBE_PID"; then
      /bin/kill -TERM "$ACTIVE_PROBE_PID" 2>/dev/null || true
      wait_for_process_exit "$ACTIVE_PROBE_PID" 20 ||
        /bin/kill -KILL "$ACTIVE_PROBE_PID" 2>/dev/null || true
    fi
    wait "$ACTIVE_PROBE_PID" 2>/dev/null || true
    ACTIVE_PROBE_PID=""
  fi
  if [ -n "$ACTIVE_GUEST_PID" ]; then
    if ! process_has_exited "$ACTIVE_GUEST_PID"; then
      /bin/kill -TERM "$ACTIVE_GUEST_PID" 2>/dev/null || true
      if ! wait_for_process_exit "$ACTIVE_GUEST_PID" 20; then
        /bin/kill -KILL "$ACTIVE_GUEST_PID" 2>/dev/null || true
        wait_for_process_exit "$ACTIVE_GUEST_PID" 20 || true
      fi
    fi
    wait "$ACTIVE_GUEST_PID" 2>/dev/null || true
    ACTIVE_GUEST_PID=""
  fi
  if [ -n "$ACTIVE_CLEANUP_PID" ]; then
    if ! process_has_exited "$ACTIVE_CLEANUP_PID"; then
      /bin/kill -TERM "$ACTIVE_CLEANUP_PID" 2>/dev/null || true
      wait_for_process_exit "$ACTIVE_CLEANUP_PID" 20 || true
    fi
    wait "$ACTIVE_CLEANUP_PID" 2>/dev/null || true
    ACTIVE_CLEANUP_PID=""
  fi
  if [ -n "$ACTIVE_GUEST_PREFIX" ] && [ -d "$ACTIVE_GUEST_PREFIX" ]; then
    run_bounded_wineserver "$ACTIVE_GUEST_PREFIX" -k || true
    run_bounded_wineserver "$ACTIVE_GUEST_PREFIX" -w || true
    ACTIVE_GUEST_PREFIX=""
  fi
}

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  cleanup_active_guest
  case "$work" in
    /tmp/switchyard-dxmt-d3d11.??????|/private/tmp/switchyard-dxmt-d3d11.??????)
      if [ -d "$work" ] && [ ! -L "$work" ]; then
        /bin/rm -rf -- "$work"
      fi
      ;;
    *) echo "refusing to clean unexpected DXMT acceptance root $work" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

build_guest() {
  local guest="$1"

  build_acceptance_guest "$guest" "$work/dxmt-d3d11-$guest.exe"
}

manifest_provider_unix_library() {
  local guest="$1"

  /usr/bin/python3 -I - "$manifest" "$guest" <<'PY'
import json
import re
import sys

manifest, guest = sys.argv[1:]
with open(manifest, "r", encoding="utf-8") as stream:
    value = json.load(stream)
matches = [
    item.get("unixLibrary")
    for item in value["cpuProvider"]["components"]
    if item.get("guestArchitecture") == guest
]
if len(matches) != 1 or type(matches[0]) is not str:
    raise SystemExit("CPU-provider component selection is ambiguous")
relative = matches[0]
if (
    re.fullmatch(r"[A-Za-z0-9._+/-]+", relative) is None
    or relative.startswith("/")
    or any(part in ("", ".", "..") for part in relative.split("/"))
):
    raise SystemExit("CPU-provider Unix path is unsafe")
print(relative)
PY
}

run_guest() {
  local guest="$1"
  local log="$work/$guest.log"
  local prefix="$work/prefix-$guest"
  local fifo="$work/$guest.stdin"
  local probe_log="$work/$guest.loaded-images"
  local probe_error="$work/$guest.loaded-images.err"
  local architecture_log="$work/$guest.architecture"
  local architecture_error="$work/$guest.architecture.err"
  local process_identity wine_executable provider_relative provider_unix
  local ready_nonce ready_marker start_seconds
  local pid status=0 ready=0 probe_status=0 count sample

  /bin/mkdir -m 700 "$prefix"
  /usr/bin/mkfifo "$fifo"
  exec 8<>"$fifo"
  exec 9>"$fifo"
  exec 8>&-
  ACTIVE_FIFO_OPEN=1
  ACTIVE_GUEST_PREFIX="$prefix"
  ready_nonce="$(/usr/bin/uuidgen)"
  ready_marker="DXMT loaded-image inspection ready: $ready_nonce"
  start_seconds="$(/bin/date +%s)"
  "${NATIVE_RUNTIME_ENV[@]}" \
    WINEPREFIX="$prefix" \
    DXMT_LOG_LEVEL=info DXMT_LOG_PATH=none \
    WINEDEBUG=+loaddll \
    "$RUNTIME/bin/switchyard-wine" "$work/dxmt-d3d11-$guest.exe" \
    --wait-for-loaded-image-inspection "$ready_nonce" \
    <"$fifo" 9>&- >"$log" 2>&1 &
  pid=$!
  ACTIVE_GUEST_PID="$pid"
  for ((count = 0; count < 600; ++count)); do
    if /usr/bin/awk -v marker="$ready_marker" '
        { sub(/\r$/, "") }
        $0 == marker { found = 1 }
        END { exit !found }
      ' "$log" >/dev/null 2>&1; then
      ready=1
      break
    fi
    if process_has_exited "$pid"; then
      wait "$pid" || status=$?
      ACTIVE_GUEST_PID=""
      pid=""
      break
    fi
    /bin/sleep 0.1
  done
  if [ "$ready" -ne 1 ]; then
    cleanup_active_guest
    if [ -n "$pid" ]; then
      echo "DXMT $guest D3D11 acceptance did not reach image inspection within 60 seconds." >&2
    else
      echo "DXMT $guest D3D11 acceptance exited before image inspection with status $status." >&2
    fi
    /bin/cat "$log" >&2
    return 1
  fi

  process_has_exited "$pid" && {
    cleanup_active_guest
    echo "DXMT $guest D3D11 acceptance exited before loaded-image inspection." >&2
    /bin/cat "$log" >&2
    return 1
  }
  # The platform /usr/bin/env interpreter strips DYLD diagnostics before the
  # public shell launcher can exec Wine. Bind the ready launcher PID to kernel
  # process and vnode records instead of depending on diagnostic environment.
  process_identity="$("$work/loaded-image-probe" --capture-process-identity "$pid")" || {
    cleanup_active_guest
    echo "DXMT $guest run could not capture its ready public launcher process identity." >&2
    return 1
  }
  : >"$architecture_log"
  for ((sample = 0; sample < 3; ++sample)); do
    if ! "$work/loaded-image-probe" "$RUNTIME" "$pid" "$start_seconds" "$prefix" \
        >"$architecture_error.current" 2>"$architecture_error"; then
      cleanup_active_guest
      echo "DXMT $guest run did not prove a native no-Rosetta process set." >&2
      /bin/cat "$architecture_error" >&2
      return 1
    fi
    /bin/cat "$architecture_error.current" >>"$architecture_log"
    if [ "$(/usr/bin/stat -f '%z' "$architecture_log")" -gt 1048576 ]; then
      cleanup_active_guest
      echo "DXMT $guest architecture evidence exceeded its 1 MiB bound." >&2
      return 1
    fi
    /bin/sleep 0.1
  done
  if /usr/bin/grep -F 'translated=true' "$architecture_log" >/dev/null; then
    cleanup_active_guest
    echo "DXMT $guest architecture evidence contains a translated process." >&2
    return 1
  fi
  wine_executable="$RUNTIME/lib/wine/aarch64-unix/wine"
  provider_relative="$(manifest_provider_unix_library "$guest")" || {
    cleanup_active_guest
    echo "DXMT $guest run could not select its CPU-provider Unix library." >&2
    return 1
  }
  provider_unix="$RUNTIME/$provider_relative"
  "$work/loaded-image-probe" --loaded-images "$pid" "$process_identity" \
    "$wine_executable" \
    "$RUNTIME/lib/wine/aarch64-unix/winemetal.so" \
    "$provider_unix" \
    >"$probe_log" 2>"$probe_error" &
  ACTIVE_PROBE_PID=$!
  for ((count = 0; count < 200; ++count)); do
    if process_has_exited "$ACTIVE_PROBE_PID"; then
      wait "$ACTIVE_PROBE_PID" || probe_status=$?
      ACTIVE_PROBE_PID=""
      break
    fi
    /bin/sleep 0.1
  done
  if [ -n "$ACTIVE_PROBE_PID" ]; then
    cleanup_active_guest
    echo "DXMT $guest loaded-image inspection timed out after 20 seconds." >&2
    return 1
  fi
  if [ "$probe_status" -ne 0 ]; then
    cleanup_active_guest
    echo "DXMT $guest loaded-image inspection failed with status $probe_status." >&2
    echo "The run did not validate its native DXMT and CPU-provider images." >&2
    /bin/cat "$probe_error" >&2
    return 1
  fi
  /bin/cat "$probe_log"
  process_has_exited "$pid" && {
    cleanup_active_guest
    echo "DXMT $guest D3D11 acceptance exited during loaded-image inspection." >&2
    /bin/cat "$log" >&2
    return 1
  }
  if ! /usr/bin/printf '\n' >&9; then
    cleanup_active_guest
    echo "DXMT $guest run lost its loaded-image inspection control channel." >&2
    return 1
  fi
  exec 9>&-
  ACTIVE_FIFO_OPEN=0

  for ((count = 0; count < 600; ++count)); do
    if process_has_exited "$pid"; then
      wait "$pid" || status=$?
      ACTIVE_GUEST_PID=""
      pid=""
      break
    fi
    /bin/sleep 0.1
  done
  if [ -n "$pid" ]; then
    cleanup_active_guest
    echo "DXMT $guest D3D11 acceptance timed out during teardown after 60 seconds." >&2
    /bin/cat "$log" >&2
    return 1
  fi
  if ! run_bounded_wineserver "$prefix" -k ||
     ! run_bounded_wineserver "$prefix" -w; then
    cleanup_active_guest
    echo "DXMT $guest run could not quiesce its private wineserver." >&2
    return 1
  fi
  ACTIVE_GUEST_PREFIX=""
  if [ "$status" -ne 0 ]; then
    echo "DXMT $guest D3D11 acceptance exited with status $status." >&2
    /bin/cat "$log" >&2
    return 1
  fi
  /usr/bin/grep -F 'Loaded DXMT D3D11 provider machine' "$log" >/dev/null || {
    echo "DXMT $guest run did not validate its loaded PE module." >&2
    /bin/cat "$log" >&2
    return 1
  }
  /usr/bin/grep -F 'DXMT D3D11 render/readback/present passed' "$log" >/dev/null || {
    echo "DXMT $guest run did not complete render/readback/present." >&2
    /bin/cat "$log" >&2
    return 1
  }
  /usr/bin/grep -F 'DXMT D3D11 dynamic WRITE_DISCARD/unmap/release passed for 263232 bytes.' \
    "$log" >/dev/null || {
      echo "DXMT $guest run did not complete dynamic WRITE_DISCARD mapping." >&2
      /bin/cat "$log" >&2
      return 1
    }
  /usr/bin/grep -F 'Maximum supported feature level:' "$log" >/dev/null || {
    echo "DXMT $guest run did not emit the DXMT device log." >&2
    /bin/cat "$log" >&2
    return 1
  }
  echo "DXMT $guest D3D11 render/readback/present acceptance passed"
}

guests=()
case "$GUEST_SELECTION" in
  all) guests=(x86_64 i386) ;;
  *) guests=("$GUEST_SELECTION") ;;
esac
for guest in "${guests[@]}"; do
  build_guest "$guest"
done
build_loaded_image_probe "$work/loaded-image-probe"
for guest in "${guests[@]}"; do
  run_guest "$guest"
done
switchyard_validate_runtime_manifest_profile \
  "$manifest" preview-native-arm64-fex "$RUNTIME"
switchyard_validate_dxmt_runtime_manifest "$RUNTIME" "$manifest"
switchyard_validate_native_cpu_provider_files "$manifest" "$RUNTIME"
switchyard_validate_wow64_unixlib_policy_manifest \
  "$RUNTIME" "$manifest" "$ROOT_DIR"
