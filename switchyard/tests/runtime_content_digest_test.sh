#!/usr/bin/env bash
set -euo pipefail
umask 022

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIGEST_TOOL="$ROOT_DIR/switchyard/runtime_content_digest.py"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-runtime-digest.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
PYTHON3="${PYTHON3:-/usr/bin/python3}"

run_digest() {
  "$PYTHON3" -I "$DIGEST_TOOL" "$@"
}

cleanup() {
  local status=$?

  trap - EXIT
  case "$TEST_ROOT" in
    /private/tmp/switchyard-runtime-digest.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected digest test root: $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT

runtime="$TEST_ROOT/runtime"
mkdir -p "$runtime/bin" "$runtime/empty"
printf 'runtime\n' > "$runtime/bin/wine"
chmod 0755 "$runtime/bin/wine"
ln -s 'wine' "$runtime/bin/wine64"

original="$(run_digest digest "$runtime")"
[ "$original" = "0b813ceebd5c69afaee31eee1dfde460a88caa8c211d32ee08d87b84babd9949" ] || {
  echo "runtime content digest format changed unexpectedly" >&2
  exit 1
}
run_digest write "$runtime" >/dev/null
run_digest verify "$runtime"
run_digest verify "$runtime" "$original"
if run_digest verify "$runtime" \
     0000000000000000000000000000000000000000000000000000000000000000; then
  echo "expected exact digest verification to reject a different digest" >&2
  exit 1
fi
if run_digest verify "$runtime" malformed; then
  echo "expected exact digest verification to reject a malformed digest" >&2
  exit 1
fi
[ "$(run_digest digest "$runtime")" = "$original" ]
[ "${#original}" -eq 64 ]

staging="$TEST_ROOT/staging"
cp -R "$runtime" "$staging"
run_digest verify "$staging"
printf 'copy race\n' >> "$staging/bin/wine"
if run_digest verify "$staging"; then
  echo "expected modified release staging content to fail verification" >&2
  exit 1
fi

printf 'tampered\n' >> "$runtime/bin/wine"
if run_digest verify "$runtime"; then
  echo "expected modified runtime content to fail verification" >&2
  exit 1
fi
run_digest write "$runtime" >/dev/null
run_digest verify "$runtime"
[ "$(run_digest digest "$runtime")" != "$original" ]
printf 'runtime\n' > "$runtime/bin/wine"
chmod 0755 "$runtime/bin/wine"
run_digest write "$runtime" >/dev/null
run_digest verify "$runtime"

chmod 0777 "$runtime"
if run_digest verify "$runtime"; then
  echo "expected modified runtime root permissions to fail verification" >&2
  exit 1
fi
chmod 0755 "$runtime"

chmod 0666 "$runtime/.switchyard-content-sha256"
if run_digest verify "$runtime"; then
  echo "expected unsafe digest marker permissions to fail verification" >&2
  exit 1
fi
chmod 0644 "$runtime/.switchyard-content-sha256"

rm "$runtime/.switchyard-content-sha256"
ln -s bin/wine "$runtime/.switchyard-content-sha256"
if run_digest verify "$runtime"; then
  echo "expected a symbolic-link digest marker to fail verification" >&2
  exit 1
fi
run_digest write "$runtime" >/dev/null
run_digest verify "$runtime"

chmod 0644 "$runtime/bin/wine"
if run_digest verify "$runtime"; then
  echo "expected modified runtime permissions to fail verification" >&2
  exit 1
fi
chmod 0755 "$runtime/bin/wine"

rm -rf "$runtime/empty"
if run_digest verify "$runtime"; then
  echo "expected a removed empty directory to fail verification" >&2
  exit 1
fi
mkdir "$runtime/empty"

rm "$runtime/bin/wine64"
ln -s $'wine\nfile injected' "$runtime/bin/wine64"
if run_digest verify "$runtime"; then
  echo "expected a changed newline-containing link target to fail verification" >&2
  exit 1
fi
rm "$runtime/bin/wine64"
ln -s 'wine' "$runtime/bin/wine64"

newline_file="$runtime/bin/line"$'\n'"file"
printf 'newline\n' > "$newline_file"
newline_digest="$(run_digest digest "$runtime")"
[ "$newline_digest" != "$original" ]
rm "$newline_file"

mkfifo "$runtime/unsupported"
if run_digest digest "$runtime" >/dev/null 2>&1; then
  echo "expected a special file to be rejected" >&2
  exit 1
fi
rm "$runtime/unsupported"

printf '%064d\nextra\n' 0 > "$runtime/.switchyard-content-sha256"
if run_digest verify "$runtime"; then
  echo "expected a malformed marker to fail verification" >&2
  exit 1
fi

real_parent="$TEST_ROOT/real-parent"
mkdir -p "$real_parent/runtime"
printf 'canonical\n' > "$real_parent/runtime/payload"
ln -s real-parent "$TEST_ROOT/symlink-parent"
if run_digest digest "$TEST_ROOT/symlink-parent/runtime" >/dev/null 2>&1; then
  echo "expected a symbolic-link parent component to be rejected" >&2
  exit 1
fi

"$PYTHON3" -I - "$DIGEST_TOOL" "$TEST_ROOT" <<'PY'
import errno
import importlib.util
import os
import sys
import threading


tool_name, test_root = sys.argv[1:]
spec = importlib.util.spec_from_file_location("switchyard_runtime_content_digest", tool_name)
digest_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(digest_tool)
initial_descriptor_count = len(os.listdir("/dev/fd"))


def write_file(path, value):
    with open(path, "wb") as stream:
        stream.write(value)


def expect_digest_error(action, label):
    try:
        action()
    except digest_tool.DigestError:
        return
    raise AssertionError(f"expected {label} to be rejected")


preserved_root = os.path.join(test_root, "preserved-marker-runtime")
os.mkdir(preserved_root)
write_file(os.path.join(preserved_root, "payload"), b"preserved\n")
digest_tool.write_marker(preserved_root)
preserved_marker = os.path.join(preserved_root, ".switchyard-content-sha256")
with open(preserved_marker, "rb") as stream:
    preserved_value = stream.read()
preserved_info = os.stat(preserved_marker, follow_symlinks=False)
original_scandir = digest_tool.os.scandir
scandir_calls = [0]


def fail_second_scandir(path):
    scandir_calls[0] += 1
    if scandir_calls[0] == 2:
        raise OSError(errno.EIO, "injected post-marker verification failure")
    return original_scandir(path)


digest_tool.os.scandir = fail_second_scandir
try:
    expect_digest_error(
        lambda: digest_tool.write_marker(preserved_root),
        "post-recognition marker verification failure",
    )
finally:
    digest_tool.os.scandir = original_scandir
if scandir_calls[0] != 2:
    raise AssertionError("valid-marker failure was not injected after recognition")
with open(preserved_marker, "rb") as stream:
    if stream.read() != preserved_value:
        raise AssertionError("failed write changed a pre-existing valid marker")
if (
    digest_tool.stable_identity(os.stat(preserved_marker, follow_symlinks=False))
    != digest_tool.stable_identity(preserved_info)
):
    raise AssertionError("failed write replaced a pre-existing valid marker inode")
if not digest_tool.verify_marker(preserved_root):
    raise AssertionError("pre-existing valid marker did not survive a failed write")

publish_failure_root = os.path.join(test_root, "publish-failure-runtime")
os.mkdir(publish_failure_root)
write_file(os.path.join(publish_failure_root, "payload"), b"publish failure\n")
original_replace = digest_tool.os.replace


def fail_marker_replace(*_arguments, **_keywords):
    raise OSError(errno.EIO, "injected marker replacement failure")


digest_tool.os.replace = fail_marker_replace
try:
    expect_digest_error(
        lambda: digest_tool.write_marker(publish_failure_root),
        "marker replacement failure",
    )
finally:
    digest_tool.os.replace = original_replace
if os.listdir(publish_failure_root) != ["payload"]:
    raise AssertionError("failed marker publication left a marker or private temporary file")


def run_root_aba(action, live, replacement, label):
    away = live + ".away"
    started = threading.Event()
    finished = threading.Event()
    worker_errors = []
    armed = [True]
    original_scandir = digest_tool.os.scandir

    def blocking_scandir(path):
        if armed[0]:
            armed[0] = False
            started.set()
            if not finished.wait(10):
                raise RuntimeError(f"timed out waiting for {label} swap")
        return original_scandir(path)

    def swap_and_restore():
        if not started.wait(10):
            worker_errors.append(RuntimeError(f"timed out waiting for {label} traversal"))
            finished.set()
            return
        try:
            os.rename(live, away)
            os.rename(replacement, live)
            os.rename(live, replacement)
            os.rename(away, live)
        except BaseException as error:  # Preserve the worker failure for the main thread.
            worker_errors.append(error)
        finally:
            finished.set()

    worker = threading.Thread(target=swap_and_restore)
    digest_tool.os.scandir = blocking_scandir
    worker.start()
    try:
        expect_digest_error(action, label)
    finally:
        finished.set()
        worker.join(10)
        digest_tool.os.scandir = original_scandir
    if worker.is_alive():
        raise AssertionError(f"{label} worker did not terminate")
    if worker_errors:
        raise worker_errors[0]


root_live = os.path.join(test_root, "root-aba-live")
root_replacement = os.path.join(test_root, "root-aba-replacement")
os.mkdir(root_live)
os.mkdir(root_replacement)
write_file(os.path.join(root_live, "payload"), b"original\n")
write_file(os.path.join(root_replacement, "payload"), b"replacement\n")
run_root_aba(
    lambda: digest_tool.content_digest(root_live),
    root_live,
    root_replacement,
    "runtime root swap-and-restore",
)

verify_live = os.path.join(test_root, "verify-aba-live")
verify_replacement = os.path.join(test_root, "verify-aba-replacement")
os.mkdir(verify_live)
os.mkdir(verify_replacement)
write_file(os.path.join(verify_live, "payload"), b"same\n")
write_file(os.path.join(verify_replacement, "payload"), b"same\n")
digest_tool.write_marker(verify_live)
digest_tool.write_marker(verify_replacement)


def verify_during_swap():
    if digest_tool.verify_marker(verify_live):
        raise AssertionError("swapped marker verification succeeded")
    raise digest_tool.DigestError("marker/tree root swap was rejected")


run_root_aba(
    verify_during_swap,
    verify_live,
    verify_replacement,
    "marker/tree root swap-and-restore",
)

def run_file_read_race(root, mutation, label):
    started = threading.Event()
    finished = threading.Event()
    worker_errors = []
    armed = [True]
    original_read = digest_tool.os.read

    def blocking_read(descriptor, size):
        if armed[0] and size == digest_tool.READ_SIZE:
            armed[0] = False
            started.set()
            if not finished.wait(10):
                raise RuntimeError(f"timed out waiting for {label}")
        return original_read(descriptor, size)

    def mutate_and_restore():
        if not started.wait(10):
            worker_errors.append(RuntimeError(f"timed out waiting for {label} hashing"))
            finished.set()
            return
        try:
            mutation()
        except BaseException as error:  # Preserve the worker failure for the main thread.
            worker_errors.append(error)
        finally:
            finished.set()

    worker = threading.Thread(target=mutate_and_restore)
    digest_tool.os.read = blocking_read
    worker.start()
    try:
        expect_digest_error(lambda: digest_tool.content_digest(root), label)
    finally:
        finished.set()
        worker.join(10)
        digest_tool.os.read = original_read
    if worker.is_alive():
        raise AssertionError(f"{label} worker did not terminate")
    if worker_errors:
        raise worker_errors[0]


file_root = os.path.join(test_root, "file-aba-runtime")
os.mkdir(file_root)
payload = os.path.join(file_root, "payload")
replacement = os.path.join(test_root, "file-aba-replacement")
away = os.path.join(test_root, "file-aba-away")
write_file(payload, b"original payload\n")
write_file(replacement, b"replacement payload\n")


def swap_file_and_restore():
    os.rename(payload, away)
    os.rename(replacement, payload)
    os.rename(payload, replacement)
    os.rename(away, payload)


run_file_read_race(
    file_root,
    swap_file_and_restore,
    "runtime file path swap-and-restore while hashing",
)

content_root = os.path.join(test_root, "content-aba-runtime")
os.mkdir(content_root)
content_payload = os.path.join(content_root, "payload")
content_value = b"original bytes\n"
write_file(content_payload, content_value)
content_info = os.stat(content_payload)


def rewrite_file_and_restore():
    write_file(content_payload, b"modified bytes\n")
    write_file(content_payload, content_value)
    os.utime(
        content_payload,
        ns=(content_info.st_atime_ns, content_info.st_mtime_ns),
        follow_symlinks=False,
    )


run_file_read_race(
    content_root,
    rewrite_file_and_restore,
    "runtime file byte rewrite-and-restore while hashing",
)

bounds_root = os.path.join(test_root, "bounds-runtime")
os.mkdir(bounds_root)
write_file(os.path.join(bounds_root, "aa"), b"aa")
write_file(os.path.join(bounds_root, "bb"), b"bb")


def expect_bound(name, value, root=bounds_root):
    original = getattr(digest_tool, name)
    setattr(digest_tool, name, value)
    try:
        expect_digest_error(lambda: digest_tool.content_digest(root), name)
    finally:
        setattr(digest_tool, name, original)


expect_bound("MAX_DIRECTORY_ENTRIES", 1)
expect_bound("MAX_ENTRIES", 1)
expect_bound("MAX_ROOT_COMPONENTS", 0)
expect_bound("MAX_ROOT_PATH_BYTES", 1)
expect_bound("MAX_RELATIVE_PATH_BYTES", 1)
expect_bound("MAX_METADATA_BYTES", 1)
expect_bound("MAX_FILE_BYTES", 1)
expect_bound("MAX_TOTAL_FILE_BYTES", 1)

depth_root = os.path.join(test_root, "depth-runtime")
os.mkdir(depth_root)
os.mkdir(os.path.join(depth_root, "child"))
expect_bound("MAX_DEPTH", 0, depth_root)

leak_root = os.path.join(test_root, "descriptor-cleanup-runtime")
os.mkdir(leak_root)
os.mkfifo(os.path.join(leak_root, "unsupported"))
descriptor_count = len(os.listdir("/dev/fd"))
for _iteration in range(32):
    expect_digest_error(
        lambda: digest_tool.content_digest(leak_root),
        "unsupported entry cleanup",
    )
    expect_digest_error(
        lambda: digest_tool.content_digest(os.path.join(test_root, "missing", "runtime")),
        "partial root-open cleanup",
    )
if len(os.listdir("/dev/fd")) != descriptor_count:
    raise AssertionError("runtime digest failure paths leaked file descriptors")
if len(os.listdir("/dev/fd")) != initial_descriptor_count:
    raise AssertionError("runtime digest ABA/error fixtures leaked directory descriptors")

print("runtime content digest ABA and bound fixtures passed")
PY

echo "runtime content digest tests passed"
