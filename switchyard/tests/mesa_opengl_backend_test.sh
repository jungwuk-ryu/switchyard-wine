#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"

if [ -z "$RUNTIME" ]; then
  echo "usage: $0 RUNTIME" >&2
  exit 2
fi
[ -x "$RUNTIME/bin/switchyard-wine" ] || {
  echo "runtime launcher is missing: $RUNTIME/bin/switchyard-wine" >&2
  exit 1
}
[ -f "$RUNTIME/lib/switchyard-mesa/x86_64-windows/opengl32.dll" ] || {
  echo "runtime Mesa opengl32.dll is missing" >&2
  exit 1
}
[ -f "$RUNTIME/lib/switchyard-mesa/x86_64-windows/libgallium_wgl.dll" ] || {
  echo "runtime Mesa Gallium WGL library is missing" >&2
  exit 1
}
[ -f "$RUNTIME/lib/switchyard-mesa/i386-windows/opengl32.dll" ] || {
  echo "runtime i386 Mesa opengl32.dll is missing" >&2
  exit 1
}
[ -f "$RUNTIME/lib/switchyard-mesa/i386-windows/libgallium_wgl.dll" ] || {
  echo "runtime i386 Mesa Gallium WGL library is missing" >&2
  exit 1
}
command -v i686-w64-mingw32-gcc >/dev/null || {
  echo "i686-w64-mingw32-gcc is required" >&2
  exit 1
}
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-mesa-opengl.XXXXXX)"
prefix="$work/prefix"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -O2 -Wall -Wextra \
  "$ROOT_DIR/switchyard/tests/mesa_opengl_backend.c" \
  -o "$work/opengl-probe-x86_64.exe" -lopengl32 -lgdi32 -luser32
i686-w64-mingw32-gcc -O2 -Wall -Wextra \
  "$ROOT_DIR/switchyard/tests/mesa_opengl_backend.c" \
  -o "$work/opengl-probe-i386.exe" -lopengl32 -lgdi32 -luser32

invalid_status=0
WINEPREFIX="$prefix" WINE_OPENGL_DRIVER=invalid \
  "$RUNTIME/bin/switchyard-wine" --version >"$work/invalid-driver.log" 2>&1 ||
  invalid_status=$?
if [ "$invalid_status" -ne 2 ]; then
  echo "invalid OpenGL driver selection exited with $invalid_status instead of 2" >&2
  cat "$work/invalid-driver.log" >&2
  exit 1
fi
grep -F 'Supported values are wine and llvmpipe.' "$work/invalid-driver.log" >/dev/null || {
  echo "invalid OpenGL driver selection did not report its supported values" >&2
  cat "$work/invalid-driver.log" >&2
  exit 1
}

default_output="$(
  WINEPREFIX="$prefix" WINEDEBUG=-all env -u WINE_OPENGL_DRIVER \
    "$RUNTIME/bin/switchyard-wine" "$work/opengl-probe-x86_64.exe"
)"
wine_output="$(
  WINEPREFIX="$prefix" WINEDEBUG=-all WINE_OPENGL_DRIVER=wine \
    "$RUNTIME/bin/switchyard-wine" "$work/opengl-probe-x86_64.exe"
)"
for builtin_output in "$default_output" "$wine_output"; do
  if grep -F 'renderer=' <<<"$builtin_output" | grep -F 'llvmpipe' >/dev/null; then
    echo "Mesa backend was selected without an explicit llvmpipe option" >&2
    exit 1
  fi
done

mesa_x86_64_output="$(
  WINEPREFIX="$prefix" WINEDEBUG=-all WINE_OPENGL_DRIVER=llvmpipe \
    "$RUNTIME/bin/switchyard-wine" "$work/opengl-probe-x86_64.exe" --require-llvmpipe
)"
mesa_i386_output="$(
  WINEPREFIX="$prefix" WINEDEBUG=-all WINE_OPENGL_DRIVER=llvmpipe \
    "$RUNTIME/bin/switchyard-wine" "$work/opengl-probe-i386.exe" --require-llvmpipe
)"
for mesa_output in "$mesa_x86_64_output" "$mesa_i386_output"; do
  grep -E '^version=(4\.[3-9]|[5-9]\.)' <<<"$mesa_output" >/dev/null || {
    echo "Mesa backend did not report OpenGL 4.3 or newer:" >&2
    printf '%s\n' "$mesa_output" >&2
    exit 1
  }
  grep -E '^renderer=.*llvmpipe' <<<"$mesa_output" >/dev/null || {
    echo "Mesa backend did not report llvmpipe:" >&2
    printf '%s\n' "$mesa_output" >&2
    exit 1
  }
done

printf '%s\n' "$default_output"
printf '%s\n' "$wine_output"
printf '%s\n' "$mesa_x86_64_output"
printf '%s\n' "$mesa_i386_output"
echo "Mesa OpenGL backend tests passed"
