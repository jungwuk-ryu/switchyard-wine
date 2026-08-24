#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
HELPER="$ROOT_DIR/switchyard/lib/fd_tree_copy.py"
DIGEST="$ROOT_DIR/switchyard/runtime_content_digest.py"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-fd-tree-copy.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
RACE_PID=

cleanup() {
  local status=$?
  trap - EXIT HUP INT TERM
  if [ -n "$RACE_PID" ]; then
    /bin/kill "$RACE_PID" 2>/dev/null || true
    wait "$RACE_PID" 2>/dev/null || true
  fi
  exec 17<&- 2>/dev/null || true
  exec 18<&- 2>/dev/null || true
  case "$TEST_ROOT" in
    /private/tmp/switchyard-fd-tree-copy.??????)
      [ ! -L "$TEST_ROOT" ] && /usr/bin/chflags -R nouchg "$TEST_ROOT" 2>/dev/null || true
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected fd-tree-copy fixture root $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "fd-tree-copy fixture failed: $1" >&2
  exit 1
}

run_copy() {
  /usr/bin/env -i \
    PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    LC_ALL=C LANG=C TMPDIR=/private/tmp \
    /usr/bin/python3 -I "$HELPER" "$@"
}

open_roots() {
  exec 17<"$1"
  exec 18<"$2"
}

close_roots() {
  exec 17<&-
  exec 18<&-
}

expect_rejected() {
  local description="$1"
  shift
  if run_copy "$@" >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
    fail "$description was accepted"
  fi
}

make_large_file() {
  /bin/dd if=/dev/zero of="$1" bs=1048576 count="$2" 2>/dev/null
}

[ "$#" -eq 0 ] || { echo "usage: $0" >&2; exit 2; }
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "fixtures require macOS"
[ -f "$HELPER" ] && [ ! -L "$HELPER" ] || fail "fd tree copy helper is missing or unsafe"
[ -f "$DIGEST" ] && [ ! -L "$DIGEST" ] || fail "runtime digest helper is missing or unsafe"

# The ordinary case covers modes, empty files, nested directories, relative
# links (including .. that remains inside the root), allowed provenance, and
# exact compatibility with the runtime content-tree digest.
/bin/mkdir -m 755 "$TEST_ROOT/exact-source"
/bin/mkdir -m 755 "$TEST_ROOT/exact-source/bin"
/bin/mkdir -m 705 "$TEST_ROOT/exact-source/lib"
/usr/bin/printf 'runtime payload\n' >"$TEST_ROOT/exact-source/bin/runtime"
/bin/chmod 751 "$TEST_ROOT/exact-source/bin/runtime"
/usr/bin/touch "$TEST_ROOT/exact-source/lib/empty"
/bin/chmod 640 "$TEST_ROOT/exact-source/lib/empty"
/usr/bin/printf 'root payload\n' >"$TEST_ROOT/exact-source/root-file"
/bin/ln -s runtime "$TEST_ROOT/exact-source/bin/current"
/bin/ln -s bin/current "$TEST_ROOT/exact-source/top-link"
/bin/ln -s ../root-file "$TEST_ROOT/exact-source/lib/up-link"
/usr/bin/xattr -w com.apple.provenance fixture-provenance \
  "$TEST_ROOT/exact-source/bin/runtime"
/bin/mkdir -m 700 "$TEST_ROOT/exact-destination"
open_roots "$TEST_ROOT/exact-source" "$TEST_ROOT/exact-destination"
run_copy 17 18 || fail "ordinary descriptor-relative tree copy failed"
close_roots
source_digest="$(/usr/bin/python3 -I "$DIGEST" digest "$TEST_ROOT/exact-source")"
destination_digest="$(/usr/bin/python3 -I "$DIGEST" digest "$TEST_ROOT/exact-destination")"
[ "$source_digest" = "$destination_digest" ] || fail "copied runtime digest differs"
[ "$(/bin/cat "$TEST_ROOT/exact-destination/bin/runtime")" = "runtime payload" ] ||
  fail "copied file content differs"
[ "$(/usr/bin/stat -f %Lp "$TEST_ROOT/exact-destination")" = 755 ] ||
  fail "root mode was not preserved"
[ "$(/usr/bin/stat -f %Lp "$TEST_ROOT/exact-destination/bin/runtime")" = 751 ] ||
  fail "file mode was not preserved"
[ "$(/usr/bin/readlink "$TEST_ROOT/exact-destination/top-link")" = bin/current ] ||
  fail "nested symlink target was not preserved"
[ "$(/usr/bin/xattr -px com.apple.provenance \
    "$TEST_ROOT/exact-destination/bin/runtime")" = \
  "$(/usr/bin/xattr -px com.apple.provenance \
    "$TEST_ROOT/exact-source/bin/runtime")" ] ||
  fail "allowed provenance metadata was not preserved exactly"

# The Python launch boundary clears hostile loader, shell, toolchain, and module
# lookup state before the Apple interpreter launcher gets control.
/usr/bin/printf '%s\n' 'raise RuntimeError("hostile PYTHONPATH module loaded")' \
  >"$TEST_ROOT/hashlib.py"
/bin/mkdir -m 755 "$TEST_ROOT/environment-source"
/usr/bin/printf 'environment\n' >"$TEST_ROOT/environment-source/value"
/bin/mkdir -m 700 "$TEST_ROOT/environment-destination"
open_roots "$TEST_ROOT/environment-source" "$TEST_ROOT/environment-destination"
(
  export BASH_ENV="$TEST_ROOT/hostile-shell"
  export ENV="$TEST_ROOT/hostile-shell"
  export DYLD_INSERT_LIBRARIES="$TEST_ROOT/not-a-library"
  export DEVELOPER_DIR="$TEST_ROOT/not-xcode"
  export PYTHONHOME="$TEST_ROOT"
  export PYTHONPATH="$TEST_ROOT"
  run_copy 17 18
) || fail "sanitized helper launch rejected a valid tree"
close_roots
[ "$(/bin/cat "$TEST_ROOT/environment-destination/value")" = environment ] ||
  fail "sanitized copy returned wrong content"

# Renaming and replacing the public source and destination roots after opening
# them cannot redirect traversal away from the selected inodes.
/bin/mkdir -m 755 "$TEST_ROOT/path-source"
/usr/bin/printf 'pinned original\n' >"$TEST_ROOT/path-source/value"
/bin/mkdir -m 700 "$TEST_ROOT/path-destination"
open_roots "$TEST_ROOT/path-source" "$TEST_ROOT/path-destination"
/bin/mv "$TEST_ROOT/path-source" "$TEST_ROOT/path-source-moved"
/bin/mkdir -m 755 "$TEST_ROOT/path-source"
/usr/bin/printf 'replacement\n' >"$TEST_ROOT/path-source/value"
/bin/mv "$TEST_ROOT/path-destination" "$TEST_ROOT/path-destination-moved"
/bin/mkdir -m 700 "$TEST_ROOT/path-destination"
run_copy 17 18 || fail "copy followed a replaced public root pathname"
close_roots
[ "$(/bin/cat "$TEST_ROOT/path-destination-moved/value")" = "pinned original" ] ||
  fail "copy did not remain bound to the original root descriptors"
[ ! -e "$TEST_ROOT/path-destination/value" ] ||
  fail "copy wrote through the replacement destination pathname"

# A symlink inserted after the destination-empty check wins no overwrite.  The
# large first file keeps the copier busy after publishing its O_EXCL name.
/bin/mkdir -m 755 "$TEST_ROOT/symlink-source"
make_large_file "$TEST_ROOT/symlink-source/aaa" 32
/usr/bin/printf 'source zzz\n' >"$TEST_ROOT/symlink-source/zzz"
/bin/mkdir -m 700 "$TEST_ROOT/symlink-destination"
/usr/bin/printf 'foreign symlink target\n' >"$TEST_ROOT/symlink-foreign"
(
  while [ ! -e "$TEST_ROOT/symlink-destination/aaa" ]; do :; done
  /bin/ln -s "$TEST_ROOT/symlink-foreign" "$TEST_ROOT/symlink-destination/zzz"
) &
RACE_PID=$!
open_roots "$TEST_ROOT/symlink-source" "$TEST_ROOT/symlink-destination"
expect_rejected "destination symlink insertion race" 17 18
close_roots
wait "$RACE_PID" || fail "symlink insertion racer did not win"
RACE_PID=
[ -L "$TEST_ROOT/symlink-destination/zzz" ] || fail "symlink squat was overwritten"
[ "$(/bin/cat "$TEST_ROOT/symlink-foreign")" = "foreign symlink target" ] ||
  fail "foreign symlink target was modified"

# A concurrently inserted hard link is likewise never opened for writing.
/bin/mkdir -m 755 "$TEST_ROOT/hardlink-source"
make_large_file "$TEST_ROOT/hardlink-source/aaa" 32
/usr/bin/printf 'source zzz\n' >"$TEST_ROOT/hardlink-source/zzz"
/bin/mkdir -m 700 "$TEST_ROOT/hardlink-destination"
/usr/bin/printf 'foreign hardlink target\n' >"$TEST_ROOT/hardlink-foreign"
foreign_inode="$(/usr/bin/stat -f %i "$TEST_ROOT/hardlink-foreign")"
(
  while [ ! -e "$TEST_ROOT/hardlink-destination/aaa" ]; do :; done
  /bin/ln "$TEST_ROOT/hardlink-foreign" "$TEST_ROOT/hardlink-destination/zzz"
) &
RACE_PID=$!
open_roots "$TEST_ROOT/hardlink-source" "$TEST_ROOT/hardlink-destination"
expect_rejected "destination hardlink insertion race" 17 18
close_roots
wait "$RACE_PID" || fail "hardlink insertion racer did not win"
RACE_PID=
[ "$(/usr/bin/stat -f %i "$TEST_ROOT/hardlink-destination/zzz")" = "$foreign_inode" ] ||
  fail "hardlink squat identity changed"
[ "$(/bin/cat "$TEST_ROOT/hardlink-foreign")" = "foreign hardlink target" ] ||
  fail "foreign hardlink target was modified"

# A write through another descriptor during the copy changes ctime and must be
# detected even when the source path itself remains present.
/bin/mkdir -m 755 "$TEST_ROOT/mutation-source"
make_large_file "$TEST_ROOT/mutation-source/big" 64
/bin/mkdir -m 700 "$TEST_ROOT/mutation-destination"
/usr/bin/python3 - "$TEST_ROOT/mutation-source/big" \
  "$TEST_ROOT/mutation-destination/big" "$TEST_ROOT/mutation-stop" <<'PY' &
import os
import sys
import time

source, published, stop = sys.argv[1:]
while not os.path.exists(published):
    time.sleep(0.001)
descriptor = os.open(source, os.O_WRONLY)
try:
    value = 0
    while not os.path.exists(stop):
        os.pwrite(descriptor, bytes((value,)), 4096)
        value = (value + 1) & 0xff
finally:
    os.close(descriptor)
PY
RACE_PID=$!
open_roots "$TEST_ROOT/mutation-source" "$TEST_ROOT/mutation-destination"
expect_rejected "in-place source mutation" 17 18
close_roots
/usr/bin/touch "$TEST_ROOT/mutation-stop"
wait "$RACE_PID" || fail "source mutation racer failed"
RACE_PID=

# Unsupported filesystem objects, hard-linked inputs, unknown metadata, and
# unsafe links all fail before they can become a successful staging tree.
/bin/mkdir -m 755 "$TEST_ROOT/type-source"
/usr/bin/mkfifo "$TEST_ROOT/type-source/pipe"
/bin/mkdir -m 700 "$TEST_ROOT/type-destination"
open_roots "$TEST_ROOT/type-source" "$TEST_ROOT/type-destination"
expect_rejected "FIFO source entry" 17 18
close_roots

/bin/mkdir -m 755 "$TEST_ROOT/source-hardlink-source"
/usr/bin/printf 'hard linked\n' >"$TEST_ROOT/source-hardlink-source/one"
/bin/ln "$TEST_ROOT/source-hardlink-source/one" "$TEST_ROOT/source-hardlink-source/two"
/bin/mkdir -m 700 "$TEST_ROOT/source-hardlink-destination"
open_roots "$TEST_ROOT/source-hardlink-source" "$TEST_ROOT/source-hardlink-destination"
expect_rejected "hard-linked source file" 17 18
close_roots

/bin/mkdir -m 755 "$TEST_ROOT/xattr-source"
/usr/bin/printf 'xattr\n' >"$TEST_ROOT/xattr-source/value"
/usr/bin/xattr -w com.switchyard.unexpected unsafe "$TEST_ROOT/xattr-source/value"
/bin/mkdir -m 700 "$TEST_ROOT/xattr-destination"
open_roots "$TEST_ROOT/xattr-source" "$TEST_ROOT/xattr-destination"
expect_rejected "unknown source xattr" 17 18
close_roots

/bin/mkdir -m 755 "$TEST_ROOT/acl-source"
/usr/bin/printf 'acl\n' >"$TEST_ROOT/acl-source/value"
/bin/chmod +a 'everyone deny delete' "$TEST_ROOT/acl-source/value"
/bin/mkdir -m 700 "$TEST_ROOT/acl-destination"
open_roots "$TEST_ROOT/acl-source" "$TEST_ROOT/acl-destination"
expect_rejected "extended source ACL" 17 18
close_roots
/bin/chmod -a# 0 "$TEST_ROOT/acl-source/value"

/bin/mkdir -m 755 "$TEST_ROOT/flags-source"
/usr/bin/printf 'flags\n' >"$TEST_ROOT/flags-source/value"
/usr/bin/chflags uchg "$TEST_ROOT/flags-source/value"
/bin/mkdir -m 700 "$TEST_ROOT/flags-destination"
open_roots "$TEST_ROOT/flags-source" "$TEST_ROOT/flags-destination"
expect_rejected "source BSD flags" 17 18
close_roots
/usr/bin/chflags nouchg "$TEST_ROOT/flags-source/value"

/bin/mkdir -m 755 "$TEST_ROOT/escape-source"
/bin/ln -s ../outside "$TEST_ROOT/escape-source/bad"
/bin/mkdir -m 700 "$TEST_ROOT/escape-destination"
open_roots "$TEST_ROOT/escape-source" "$TEST_ROOT/escape-destination"
expect_rejected "root-escaping source symlink" 17 18
close_roots

echo "fd-tree-copy fixtures passed"
