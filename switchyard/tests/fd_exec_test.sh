#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
HELPER="$ROOT_DIR/switchyard/lib/fd_exec.py"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-fd-exec.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"

cleanup() {
  local status=$?
  trap - EXIT HUP INT TERM
  exec 17<&- 2>/dev/null || true
  exec 18<&- 2>/dev/null || true
  exec 19<&- 2>/dev/null || true
  case "$TEST_ROOT" in
    /private/tmp/switchyard-fd-exec.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected fd-exec fixture root $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "fd-exec fixture failed: $1" >&2
  exit 1
}

run_helper() {
  /usr/bin/env -i \
    PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    LC_ALL=C LANG=C TMPDIR=/private/tmp \
    /usr/bin/python3 -I "$HELPER" "$@"
}

[ "$#" -eq 0 ] || { echo "usage: $0" >&2; exit 2; }
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "fixtures require macOS"
[ -f "$HELPER" ] && [ ! -L "$HELPER" ] || fail "fd-exec helper is missing or unsafe"

/usr/bin/printf '%s\n' \
  '/usr/bin/printf hostile-ran > "${SWITCHYARD_HOSTILE_MARKER:?}"' \
  >"$TEST_ROOT/hostile-environment"
/bin/mkdir -m 700 "$TEST_ROOT/environment"
exec 17<"$TEST_ROOT/environment"
(
  export BASH_ENV="$TEST_ROOT/hostile-environment"
  export ENV="$TEST_ROOT/hostile-environment"
  export DYLD_INSERT_LIBRARIES="$TEST_ROOT/not-a-library"
  export CODESIGN_ALLOCATE="$TEST_ROOT/not-codesign-allocate"
  export COPYFILE_DISABLE=1
  export COPYFILE_UNPACK=1
  export DEVELOPER_DIR="$TEST_ROOT/not-xcode"
  export LC_ALL=C.UTF-8
  export LANG=C.UTF-8
  export TMPDIR="$TEST_ROOT"
  export PYTHONHOME="$TEST_ROOT"
  export PYTHONPATH="$TEST_ROOT"
  export SWITCHYARD_HOSTILE_MARKER="$TEST_ROOT/hostile-ran"
  run_helper 17 -- /bin/sh -c '
    [ "$PATH" = /usr/bin:/bin:/usr/sbin:/sbin ]
    [ "$LC_ALL" = C ] && [ "$LANG" = C ] && [ "$TMPDIR" = /private/tmp ]
    for name in BASH_ENV ENV DYLD_INSERT_LIBRARIES CODESIGN_ALLOCATE \
      COPYFILE_DISABLE COPYFILE_UNPACK DEVELOPER_DIR PYTHONHOME PYTHONPATH \
      SWITCHYARD_HOSTILE_MARKER; do
      eval "[ -z \"\${$name+x}\" ]" || exit 1
    done
    /usr/bin/env > ./child-environment
  '
) || fail "ambient environment reached the supervised tool"
[ ! -e "$TEST_ROOT/hostile-ran" ] || fail "hostile shell startup file ran"
if /usr/bin/grep -E '^(BASH_ENV|ENV|DYLD_|CODESIGN_ALLOCATE|COPYFILE_|DEVELOPER_DIR|PYTHON)' \
    "$TEST_ROOT/environment/child-environment" >/dev/null; then
  fail "hostile environment name survived sanitization"
fi
exec 17<&-

/bin/mkdir -m 700 "$TEST_ROOT/original"
/usr/bin/printf 'original\n' >"$TEST_ROOT/original/payload"
exec 17<"$TEST_ROOT/original"
run_helper 17 -- /bin/sh -c '
  /usr/bin/printf ready > ./ready
  while [ ! -f ./go ]; do /bin/sleep 0.01; done
  /bin/cat ./payload > ./result
' &
child=$!
for unused in {1..500}; do
  [ -f "$TEST_ROOT/original/ready" ] && break
  /bin/sleep 0.01
done
[ -f "$TEST_ROOT/original/ready" ] || fail "child did not bind the original directory"
/bin/mv "$TEST_ROOT/original" "$TEST_ROOT/moved"
/bin/mkdir -m 700 "$TEST_ROOT/original"
/usr/bin/printf 'replacement\n' >"$TEST_ROOT/original/payload"
/usr/bin/touch "$TEST_ROOT/moved/go"
wait "$child" || fail "fd-bound child failed after pathname replacement"
[ "$(/bin/cat "$TEST_ROOT/moved/result")" = original ] ||
  fail "child did not remain bound to the original inode"
[ ! -e "$TEST_ROOT/original/result" ] || fail "child followed the replacement pathname"
exec 17<&-

# Only explicitly inherited descriptors survive the final exec.  The directory
# descriptor itself is no longer needed once it has selected the child's cwd.
exec 17<"$TEST_ROOT/moved"
exec 18<"$TEST_ROOT/moved/payload"
exec 19<"$TEST_ROOT/original/payload"
run_helper 17 --pass-fd 18 -- /bin/sh -c '
  [ ! -e /dev/fd/17 ]
  [ -e /dev/fd/18 ]
  [ ! -e /dev/fd/19 ]
  /bin/cat /dev/fd/18 > ./inherited-result
' || fail "descriptor allowlist was not enforced"
[ "$(/bin/cat "$TEST_ROOT/moved/inherited-result")" = original ] ||
  fail "explicit inherited descriptor returned wrong content"
exec 18<&-
exec 19<&-

# A securely precreated destination directory lets cp populate the selected
# inode without ever resolving its public parent pathname.
/bin/mkdir -m 700 "$TEST_ROOT/source"
/bin/mkdir -m 700 "$TEST_ROOT/moved/copied"
/usr/bin/printf 'copy-source\n' >"$TEST_ROOT/source/value"
exec 18<"$TEST_ROOT/moved/copied"
run_helper 18 -- /bin/cp -cR "$TEST_ROOT/source/." . || fail "fd-bound cp failed"
[ "$(/bin/cat "$TEST_ROOT/moved/copied/value")" = copy-source ] ||
  fail "fd-bound cp copied wrong content"
exec 18<&-

# Darwin exposes a directory itself through /dev/fd, but does not permit path
# traversal below that descriptor.  cp therefore cannot bind both a source tree
# and a destination tree with /dev/fd/N/.; a recursive openat copier or a
# separately pinned archive/extraction phase is required for an untrusted source.
/bin/mkdir -m 700 "$TEST_ROOT/moved/cp-fd-rejected"
exec 18<"$TEST_ROOT/moved/cp-fd-rejected"
exec 19<"$TEST_ROOT/source"
if run_helper 18 --pass-fd 19 -- /bin/cp -cR /dev/fd/19/. . \
    >"$TEST_ROOT/cp-fd.out" 2>"$TEST_ROOT/cp-fd.err"; then
  fail "cp unexpectedly traversed a Darwin directory descriptor"
fi
[ ! -e "$TEST_ROOT/moved/cp-fd-rejected/value" ] ||
  fail "rejected descriptor source left a partial copy"
exec 18<&-
exec 19<&-

# ditto accepts an already-created, held regular-file descriptor as its archive
# destination.  This removes the producer's last pathname-creation window.
/usr/bin/python3 -I - "$TEST_ROOT/moved/held-archive.zip" <<'PY'
import os
import sys

descriptor = os.open(
    sys.argv[1],
    os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
    0o600,
)
os.close(descriptor)
PY
exec 19<>"$TEST_ROOT/moved/held-archive.zip"
run_helper 17 --pass-fd 19 -- /usr/bin/ditto \
  -c -k --norsrc --noextattr --noacl --keepParent ./copied /dev/fd/19 ||
  fail "ditto rejected a held archive destination"
/usr/bin/unzip -t "$TEST_ROOT/moved/held-archive.zip" >/dev/null ||
  fail "ditto wrote an invalid held archive destination"
exec 19>&-

# codesign and ditto both accept cwd-relative operands after the visible root
# has been replaced.  The sleep makes the rename happen before either tool is
# invoked, while the shell's cwd is already the pinned inode.
/bin/cp /bin/echo "$TEST_ROOT/moved/codesign-target"
exec 18<"$TEST_ROOT/moved"
run_helper 18 -- /bin/sh -c '
  /bin/sleep 0.1
  /usr/bin/codesign --force --sign - ./codesign-target >/dev/null 2>&1
  /usr/bin/ditto -c -k --norsrc --noextattr --noacl --keepParent \
    ./copied ./runtime.zip
' &
child=$!
/bin/mv "$TEST_ROOT/moved" "$TEST_ROOT/renamed-again"
/bin/mkdir -m 700 "$TEST_ROOT/moved"
wait "$child" || fail "fd-bound codesign or ditto failed after root replacement"
/usr/bin/codesign --verify --strict "$TEST_ROOT/renamed-again/codesign-target" \
  >/dev/null 2>&1 || fail "codesign did not modify the pinned tree"
[ -s "$TEST_ROOT/renamed-again/runtime.zip" ] || fail "ditto did not write the pinned tree"
[ ! -e "$TEST_ROOT/moved/runtime.zip" ] || fail "ditto followed the replacement pathname"
exec 17<&-
exec 18<&-

/bin/mkdir -m 700 "$TEST_ROOT/unsafe-xattr"
/usr/bin/xattr -w com.switchyard.unexpected unsafe "$TEST_ROOT/unsafe-xattr"
exec 17<"$TEST_ROOT/unsafe-xattr"
accepted=0
if run_helper 17 -- /usr/bin/true >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
  accepted=1
fi
exec 17<&-
/usr/bin/xattr -d com.switchyard.unexpected "$TEST_ROOT/unsafe-xattr"
[ "$accepted" -eq 0 ] || fail "unexpected working-directory xattr was accepted"

/bin/mkdir -m 700 "$TEST_ROOT/unsafe-acl"
/bin/chmod +a 'everyone deny delete' "$TEST_ROOT/unsafe-acl"
exec 17<"$TEST_ROOT/unsafe-acl"
accepted=0
if run_helper 17 -- /usr/bin/true >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
  accepted=1
fi
exec 17<&-
/bin/chmod -a# 0 "$TEST_ROOT/unsafe-acl"
[ "$accepted" -eq 0 ] || fail "extended working-directory ACL was accepted"

/bin/mkdir -m 700 "$TEST_ROOT/unsafe-flags"
/usr/bin/chflags uchg "$TEST_ROOT/unsafe-flags"
exec 17<"$TEST_ROOT/unsafe-flags"
accepted=0
if run_helper 17 -- /usr/bin/true >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
  accepted=1
fi
exec 17<&-
/usr/bin/chflags nouchg "$TEST_ROOT/unsafe-flags"
[ "$accepted" -eq 0 ] || fail "working-directory file flags were accepted"

/bin/chmod 0755 "$TEST_ROOT/moved"
exec 17<"$TEST_ROOT/moved"
if run_helper 17 -- /usr/bin/true >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
  fail "unsafe working directory mode was accepted"
fi
exec 17<&-

echo "fd-exec fixtures passed"
