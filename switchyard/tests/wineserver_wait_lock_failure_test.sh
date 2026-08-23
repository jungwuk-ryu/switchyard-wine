#!/usr/bin/env bash
set -euo pipefail

TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-wineserver-wait.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && /bin/pwd -P)"
PREFIX="$TEST_ROOT/prefix"
SERVER_STARTED=0

fail() {
  echo "wineserver wait-lock failure test: $*" >&2
  exit 1
}

cleanup() {
  local status=$?
  local preserve=0

  trap - EXIT HUP INT TERM
  if [ "$SERVER_STARTED" -eq 1 ]; then
    /usr/bin/env -u DYLD_INSERT_LIBRARIES WINEPREFIX="$PREFIX" \
      "$WINESERVER" -k >/dev/null 2>&1 || true
    if ! /usr/bin/env -u DYLD_INSERT_LIBRARIES WINEPREFIX="$PREFIX" \
        "$WINESERVER" -w >/dev/null 2>&1; then
      echo "wineserver wait-lock failure test could not quiesce its exact prefix" >&2
      preserve=1
      [ "$status" -ne 0 ] || status=1
    fi
  fi
  case "$TEST_ROOT" in
    /private/tmp/switchyard-wineserver-wait.??????)
      if [ "$preserve" -eq 0 ] && [ ! -L "$TEST_ROOT" ]; then
        /bin/rm -rf -- "$TEST_ROOT"
      else
        echo "preserving wineserver wait-lock failure evidence at $TEST_ROOT" >&2
        [ "$status" -ne 0 ] || status=1
      fi
      ;;
    *)
      echo "refusing to remove unexpected test root: $TEST_ROOT" >&2
      [ "$status" -ne 0 ] || status=1
      ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ "$#" -eq 1 ] || fail "usage: $0 BUILD_DIR"
BUILD_DIR="$1"
case "$BUILD_DIR" in
  /*) ;;
  *) fail "build directory must be an absolute path" ;;
esac
[ -d "$BUILD_DIR" ] && [ ! -L "$BUILD_DIR" ] ||
  fail "build directory is missing or unsafe: $BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && /bin/pwd -P)"
WINESERVER="$BUILD_DIR/server/wineserver"
[ -f "$WINESERVER" ] && [ -x "$WINESERVER" ] && [ ! -L "$WINESERVER" ] ||
  fail "build-tree wineserver is missing or unsafe: $WINESERVER"

if [ "$(/usr/bin/uname -s)" != Darwin ] ||
   [ "$(/usr/bin/uname -m)" != arm64 ]; then
  echo "wineserver wait-lock failure test skipped: requires native ARM64 macOS"
  exit 0
fi

/bin/mkdir -m 700 "$PREFIX"
/usr/bin/sed 's/^+//' >"$TEST_ROOT/fail-wait-lock.c" <<'EOF'
+#include <errno.h>
+#include <fcntl.h>
+#include <stdint.h>
+#include <unistd.h>
+
+static int fail_wait_lock( int fd, int command, ... )
+{
+    static const char marker[] = "SWITCHYARD_FCNTL_SETLKW_ENOLCK\n";
+
+    (void)fd;
+    if (command == F_SETLKW)
+    {
+        (void)write( STDERR_FILENO, marker, sizeof(marker) - 1 );
+        errno = ENOLCK;
+        return -1;
+    }
+    errno = EINVAL;
+    return -1;
+}
+
+#define DYLD_INTERPOSE(replacement, replacee) \
+    __attribute__((used)) static const struct \
+    { \
+        const void *replacement_address; \
+        const void *replacee_address; \
+    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = \
+    { \
+        (const void *)(uintptr_t)&replacement, \
+        (const void *)(uintptr_t)&replacee \
+    }
+
+DYLD_INTERPOSE( fail_wait_lock, fcntl );
EOF

/usr/bin/clang -dynamiclib -arch arm64 -std=c11 -Wall -Wextra -Werror \
  -o "$TEST_ROOT/fail-wait-lock.dylib" "$TEST_ROOT/fail-wait-lock.c"

/usr/bin/env -u DYLD_INSERT_LIBRARIES WINEPREFIX="$PREFIX" \
  "$WINESERVER" -p5 >/dev/null 2>&1 || fail "could not start exact-prefix wineserver"
SERVER_STARTED=1
/usr/bin/env -u DYLD_INSERT_LIBRARIES WINEPREFIX="$PREFIX" \
  "$WINESERVER" -k0 >/dev/null 2>&1 || fail "exact-prefix wineserver is not active"

set +e
/usr/bin/env DYLD_INSERT_LIBRARIES="$TEST_ROOT/fail-wait-lock.dylib" \
  WINEPREFIX="$PREFIX" "$WINESERVER" -w \
  >"$TEST_ROOT/wait.out" 2>"$TEST_ROOT/wait.err"
wait_status=$?
set -e

/usr/bin/grep -Fqx 'SWITCHYARD_FCNTL_SETLKW_ENOLCK' "$TEST_ROOT/wait.err" || {
  /bin/cat "$TEST_ROOT/wait.err" >&2
  fail "the F_SETLKW failure interposer did not reach wineserver -w"
}
[ "$wait_status" -eq 1 ] ||
  fail "wineserver -w returned $wait_status for the injected F_SETLKW failure"

/usr/bin/env -u DYLD_INSERT_LIBRARIES WINEPREFIX="$PREFIX" \
  "$WINESERVER" -k >/dev/null 2>&1 || true
/usr/bin/env -u DYLD_INSERT_LIBRARIES WINEPREFIX="$PREFIX" \
  "$WINESERVER" -w >/dev/null 2>&1 || fail "could not quiesce exact-prefix wineserver"
SERVER_STARTED=0

echo "wineserver wait-lock failure propagation passed"
