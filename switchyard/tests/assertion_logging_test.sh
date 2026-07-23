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

work="$(/usr/bin/mktemp -d /tmp/switchyard-assertion-logging.XXXXXX)"
prefix="$work/prefix"
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -Os -s \
  -o "$work/assertion-logging.exe" \
  "$ROOT_DIR/switchyard/tests/assertion_logging.c"

handled_log="$work/handled.log"
WINEPREFIX="$prefix" WINEDLLOVERRIDES="winedbg.exe=d" WINEDEBUG=-all,err+seh \
  "$RUNTIME/bin/switchyard-wine" "$work/assertion-logging.exe" handled \
  >"$handled_log" 2>&1

/usr/bin/grep -F "continued" "$handled_log" >/dev/null
if /usr/bin/grep -F "err:seh:dispatch_exception assertion failure exception" \
  "$handled_log" >/dev/null; then
  /bin/cat "$handled_log" >&2
  echo "handled assertion was logged as an error" >&2
  exit 1
fi

unhandled_log="$work/unhandled.log"
set +e
WINEPREFIX="$prefix" WINEDLLOVERRIDES="winedbg.exe=d" WINEDEBUG=-all,err+seh \
  "$RUNTIME/bin/switchyard-wine" "$work/assertion-logging.exe" unhandled \
  >"$unhandled_log" 2>&1
unhandled_status=$?
set -e

if [ "$unhandled_status" -eq 0 ]; then
  /bin/cat "$unhandled_log" >&2
  echo "unhandled assertion unexpectedly continued" >&2
  exit 1
fi
/usr/bin/grep -F "err:seh:dispatch_exception assertion failure exception" \
  "$unhandled_log" >/dev/null

echo "Assertion logging test passed"
