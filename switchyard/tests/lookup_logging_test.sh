#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher is missing" >&2; exit 1; }
[ -x "$RUNTIME/bin/wineserver" ] || { echo "runtime wineserver is missing" >&2; exit 1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-lookup-logging.XXXXXX)"
prefix="$work/prefix"
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -Os -s \
  -o "$work/lookup-logging.exe" \
  "$ROOT_DIR/switchyard/tests/lookup_logging.c"

warning_log="$work/warning.log"
unexpected_log="$work/unexpected.log"
WINEPREFIX="$prefix" WINEDLLOVERRIDES="winedbg.exe=d" \
  WINEDEBUG=-all,warn+file,warn+module \
  "$RUNTIME/bin/switchyard-wine" "$work/lookup-logging.exe" \
  >"$warning_log" 2>&1

/usr/bin/grep -F -e "XInputGetDSoundAudioDeviceGuids" -e 'd3d.dll" not found' \
  "$warning_log" >"$unexpected_log" || true
if [ -s "$unexpected_log" ]; then
  /bin/cat "$unexpected_log" >&2
  echo "expected lookup miss was logged as a warning" >&2
  exit 1
fi

trace_log="$work/trace.log"
WINEPREFIX="$prefix" WINEDLLOVERRIDES="winedbg.exe=d" \
  WINEDEBUG=-all,trace+file,trace+module \
  "$RUNTIME/bin/switchyard-wine" "$work/lookup-logging.exe" \
  >"$trace_log" 2>&1

/usr/bin/grep -F 'trace:module:LdrGetProcedureAddress "XInputGetDSoundAudioDeviceGuids"' \
  "$trace_log" >/dev/null
/usr/bin/grep -F 'trace:file:NtCreateFile' "$trace_log" |
  /usr/bin/grep -F 'd3d.dll" not found' >/dev/null

echo "Lookup logging test passed"
