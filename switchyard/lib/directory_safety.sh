#!/usr/bin/env bash

# Destructive directory promotion is centralized here so every caller proves
# that an existing destination belongs to Switchyard before RENAME_SWAP makes
# it eligible for removal.

canonical_existing_path() {
  perl -MCwd=abs_path -e '
    my $path = abs_path($ARGV[0]);
    exit 1 unless defined $path;
    print $path;
  ' "$1"
}

path_is_strict_descendant() {
  local path="$1"
  local root="$2"
  local canonical_path
  local canonical_root

  canonical_path="$(canonical_existing_path "$path")" || return 1
  canonical_root="$(canonical_existing_path "$root")" || return 1
  [ "$canonical_path" != "$canonical_root" ] || return 1
  case "$canonical_path" in
    "$canonical_root"/*) return 0 ;;
    *) return 1 ;;
  esac
}

path_is_same_or_ancestor() {
  local candidate="$1"
  local descendant="$2"
  local canonical_candidate
  local canonical_descendant

  canonical_candidate="$(canonical_existing_path "$candidate")" || return 1
  canonical_descendant="$(canonical_existing_path "$descendant")" || return 1
  case "$canonical_descendant" in
    "$canonical_candidate"|"$canonical_candidate"/*) return 0 ;;
    *) return 1 ;;
  esac
}

replacement_target_is_dangerous() {
  local target="$1"
  local canonical_target
  local canonical_home

  canonical_target="$(canonical_existing_path "$target")" || return 0
  canonical_home="$(canonical_existing_path "$HOME")" || return 0
  [ "$canonical_target" != "/" ] || return 0

  # Refuse the home directory and every ancestor that contains it. A project
  # marker must never be enough to make /, /Users, or $HOME removable.
  case "$canonical_home" in
    "$canonical_target"|"$canonical_target"/*) return 0 ;;
    *) return 1 ;;
  esac
}

runtime_directory_is_owned() {
  [ "$#" -ge 1 ] && [ "$#" -le 2 ] || return 2

  local root="$1"
  local expected_runtime_profile="${2:-stable-x86_64-rosetta}"
  local manifest="$root/switchyard-runtime.json"
  local manifest_id
  local manifest_install_prefix
  local manifest_executable

  case "$expected_runtime_profile" in
    stable-x86_64-rosetta)
      # Keep the legacy stable ownership proof unchanged.  Existing external
      # stable runtimes predate runtimeFamily/buildProfile metadata and remain
      # replaceable through the original one-argument API.
      ;;
    preview-native-arm64-fex)
      /usr/bin/python3 -I - "$root" "$manifest" <<'PY' || return 1
import json
import os
import re
import stat
import sys
import unicodedata

MAX_MANIFEST_BYTES = 1024 * 1024
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
BLOCKING_FILE_FLAGS = sum(
    getattr(stat, name, 0)
    for name in ("UF_APPEND", "UF_IMMUTABLE", "SF_APPEND", "SF_IMMUTABLE")
)


def fail(message):
    raise ValueError(message)


def state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


def require_owned_metadata(value, description, require_regular=False):
    if require_regular and not stat.S_ISREG(value.st_mode):
        fail(description + " is not a regular file")
    if value.st_uid != os.geteuid() or value.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        fail(description + " is not owned safely")
    if getattr(value, "st_flags", 0) & BLOCKING_FILE_FLAGS:
        fail(description + " has append-only or immutable filesystem flags")


def require_safe_traversal_directory(value):
    if not stat.S_ISDIR(value.st_mode):
        fail("runtime path ancestor is not a directory")
    if (
        value.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        and (
            not value.st_mode & stat.S_ISVTX
            or value.st_uid not in (0, os.geteuid())
        )
    ):
        fail("runtime path ancestor is writable without sticky protection")


def open_directory(path):
    if (
        not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == os.path.sep
        or "\n" in path
        or "\r" in path
    ):
        fail("runtime root is not a bounded canonical absolute path")
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    try:
        require_safe_traversal_directory(os.fstat(descriptor))
        for component in path.split(os.path.sep):
            if not component:
                continue
            following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = following
            require_safe_traversal_directory(os.fstat(descriptor))
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            fail("duplicate JSON object key")
        result[key] = value
    return result


def reject_nonstandard_constant(_value):
    fail("nonstandard JSON constant")


def require_string(document, key):
    if key not in document or type(document[key]) is not str:
        fail("ownership field is missing or is not a string")
    value = document[key]
    if any(unicodedata.category(character) == "Cc" for character in value):
        fail("ownership field contains a control character")
    return value


root_name, manifest_name = sys.argv[1:]
if manifest_name != root_name + "/switchyard-runtime.json":
    fail("runtime manifest path is not rooted in the runtime")
root_fd = -1
manifest_fd = -1
bin_fd = -1
launcher_fd = -1
try:
    root_fd = open_directory(root_name)
    root_info = os.fstat(root_fd)
    require_owned_metadata(root_info, "runtime root")

    manifest_entry = os.stat(
        "switchyard-runtime.json", dir_fd=root_fd, follow_symlinks=False
    )
    require_owned_metadata(manifest_entry, "runtime manifest", require_regular=True)
    if (
        manifest_entry.st_nlink != 1
        or manifest_entry.st_size <= 0
        or manifest_entry.st_size > MAX_MANIFEST_BYTES
    ):
        fail("runtime manifest is not a bounded regular file")
    manifest_fd = os.open("switchyard-runtime.json", FILE_FLAGS, dir_fd=root_fd)
    manifest_opened = os.fstat(manifest_fd)
    if state(manifest_entry) != state(manifest_opened):
        fail("runtime manifest changed while opening it")
    payload = bytearray()
    while True:
        chunk = os.read(manifest_fd, min(65536, MAX_MANIFEST_BYTES + 1 - len(payload)))
        if not chunk:
            break
        payload.extend(chunk)
        if len(payload) > MAX_MANIFEST_BYTES:
            fail("runtime manifest exceeds its size bound")
    if state(manifest_opened) != state(os.fstat(manifest_fd)):
        fail("runtime manifest changed while reading it")
    document = json.loads(
        bytes(payload).decode("utf-8", "strict"),
        object_pairs_hook=reject_duplicate_keys,
        parse_constant=reject_nonstandard_constant,
    )
    if type(document) is not dict:
        fail("runtime manifest root is not an object")

    manifest_id = require_string(document, "id")
    if re.fullmatch(
        r"switchyard-local-native-arm64-fex-[A-Za-z0-9._-]+",
        manifest_id,
        flags=re.ASCII,
    ) is None:
        fail("runtime id is outside the preview profile namespace")
    if require_string(document, "runtimeFamily") != "preview-native-arm64-fex":
        fail("runtime family does not match the preview profile")
    if require_string(document, "buildProfile") != "switchyard-native-arm64-fex":
        fail("build profile does not match the preview profile")
    if require_string(document, "installPrefix") != root_name:
        fail("runtime install prefix does not match its directory")
    if require_string(document, "executable") != root_name + "/bin/switchyard-wine":
        fail("runtime executable does not match its directory")

    bin_entry = os.stat("bin", dir_fd=root_fd, follow_symlinks=False)
    if not stat.S_ISDIR(bin_entry.st_mode):
        fail("runtime bin path is not a directory")
    require_owned_metadata(bin_entry, "runtime bin directory")
    bin_fd = os.open("bin", DIRECTORY_FLAGS, dir_fd=root_fd)
    bin_opened = os.fstat(bin_fd)
    if state(bin_opened) != state(bin_entry):
        fail("runtime bin directory changed while opening it")
    launcher = os.stat("switchyard-wine", dir_fd=bin_fd, follow_symlinks=False)
    if stat.S_ISLNK(launcher.st_mode):
        if launcher.st_uid != os.geteuid():
            fail("runtime launcher link is not owned safely")
        if os.readlink("switchyard-wine", dir_fd=bin_fd) != "wine":
            fail("runtime launcher link is not the packaged relative target")
        launcher_target_name = "wine"
        launcher_target = os.stat("wine", dir_fd=bin_fd, follow_symlinks=False)
        launcher_fd = os.open("wine", FILE_FLAGS, dir_fd=bin_fd)
    elif stat.S_ISREG(launcher.st_mode):
        launcher_target_name = "switchyard-wine"
        launcher_target = launcher
        launcher_fd = os.open("switchyard-wine", FILE_FLAGS, dir_fd=bin_fd)
    else:
        fail("runtime launcher is not a regular file or packaged link")
    launcher_opened = os.fstat(launcher_fd)
    require_owned_metadata(
        launcher_opened, "runtime launcher target", require_regular=True
    )
    if (
        state(launcher_opened) != state(launcher_target)
        or not launcher_opened.st_mode & 0o111
    ):
        fail("runtime launcher target is not executable")

    current_manifest = os.stat(
        "switchyard-runtime.json", dir_fd=root_fd, follow_symlinks=False
    )
    if state(current_manifest) != state(manifest_opened):
        fail("runtime manifest changed during ownership validation")
    current_launcher = os.stat(
        "switchyard-wine", dir_fd=bin_fd, follow_symlinks=False
    )
    if state(current_launcher) != state(launcher):
        fail("runtime launcher path changed during ownership validation")
    current_launcher_target = os.stat(
        launcher_target_name, dir_fd=bin_fd, follow_symlinks=False
    )
    if state(current_launcher_target) != state(launcher_opened):
        fail("runtime launcher target path changed during ownership validation")
    if state(os.fstat(launcher_fd)) != state(launcher_opened):
        fail("runtime launcher target changed during ownership validation")
    current_bin = os.stat("bin", dir_fd=root_fd, follow_symlinks=False)
    if state(current_bin) != state(bin_opened):
        fail("runtime bin path changed during ownership validation")
    current_root = os.stat(root_name, follow_symlinks=False)
    if (current_root.st_dev, current_root.st_ino) != (root_info.st_dev, root_info.st_ino):
        fail("runtime root changed during ownership validation")
except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
    print(f"preview runtime ownership validation failed: {error}", file=sys.stderr)
    raise SystemExit(1)
finally:
    for descriptor in (launcher_fd, bin_fd, manifest_fd, root_fd):
        if descriptor >= 0:
            os.close(descriptor)
PY
      return
      ;;
    *) return 1 ;;
  esac

  [ -f "$manifest" ] || return 1
  manifest_id="$(/usr/bin/plutil -extract id raw -o - "$manifest" 2>/dev/null || true)"
  manifest_install_prefix="$(/usr/bin/plutil -extract installPrefix raw -o - "$manifest" 2>/dev/null || true)"
  manifest_executable="$(/usr/bin/plutil -extract executable raw -o - "$manifest" 2>/dev/null || true)"

  case "$manifest_id" in
    switchyard-local-wow64-x86_64-*) ;;
    *) return 1 ;;
  esac
  [ "$manifest_install_prefix" = "$root" ] || return 1
  [ "$manifest_executable" = "$root/bin/switchyard-wine" ] || return 1
  [ -x "$root/bin/switchyard-wine" ]
}

preview_runtime_replacement_paths_are_safe() {
  [ "$#" -eq 2 ] || return 2

  /usr/bin/python3 -I - "$1" "$2" <<'PY'
import fcntl
import os
import stat
import sys

DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
BLOCKING_FILE_FLAGS = sum(
    getattr(stat, name, 0)
    for name in ("UF_APPEND", "UF_IMMUTABLE", "SF_APPEND", "SF_IMMUTABLE")
)


def fail(message):
    print("preview runtime replacement path validation failed: " + message, file=sys.stderr)
    raise SystemExit(1)


def validate_path(path):
    if (
        not path
        or not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == os.path.sep
        or "\0" in path
        or "\n" in path
        or "\r" in path
        or len(os.fsencode(path)) > 1023
    ):
        fail("path is not a bounded canonical absolute path")


def require_safe_directory(info, description, require_owner=False):
    if not stat.S_ISDIR(info.st_mode):
        fail(description + " is not a directory")
    if (
        info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        and (
            not info.st_mode & stat.S_ISVTX
            or info.st_uid not in (0, os.geteuid())
        )
    ):
        fail(description + " is writable without sticky protection")
    if require_owner and (
        info.st_uid != os.geteuid()
        or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        fail(description + " is not an owner-controlled directory")
    if getattr(info, "st_flags", 0) & BLOCKING_FILE_FLAGS:
        fail(description + " has append-only or immutable filesystem flags")


def open_checked(path, allow_missing_final=False):
    validate_path(path)
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    components = path.split(os.path.sep)[1:]
    try:
        require_safe_directory(os.fstat(descriptor), "filesystem root")
        for index, component in enumerate(components):
            try:
                following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            except FileNotFoundError:
                if allow_missing_final and index == len(components) - 1:
                    return descriptor, None, component
                raise
            os.close(descriptor)
            descriptor = following
            require_safe_directory(os.fstat(descriptor), "path ancestor")
        return descriptor, descriptor, ""
    except BaseException:
        os.close(descriptor)
        raise


def require_physical_path(path, descriptor):
    payload = fcntl.fcntl(descriptor, fcntl.F_GETPATH, b"\0" * 1024)
    if payload.split(b"\0", 1)[0] != os.fsencode(path):
        fail("opened directory does not retain the requested physical path")


staged, live = sys.argv[1:]
staged_fd = -1
live_fd = -1
live_parent_fd = -1
try:
    staged_fd, _opened, _name = open_checked(staged)
    require_safe_directory(os.fstat(staged_fd), "staged runtime", require_owner=True)
    require_physical_path(staged, staged_fd)

    live_parent_fd, live_fd_value, live_name = open_checked(
        live, allow_missing_final=True
    )
    if live_fd_value is None:
        require_physical_path(os.path.dirname(live), live_parent_fd)
    else:
        live_fd = live_fd_value
        live_parent_fd = -1
        require_safe_directory(os.fstat(live_fd), "live runtime", require_owner=True)
        require_physical_path(live, live_fd)
except (OSError, UnicodeError, ValueError) as error:
    fail(str(error))
finally:
    for descriptor in (live_parent_fd, live_fd, staged_fd):
        if descriptor >= 0:
            os.close(descriptor)
PY
}

cache_directory_is_owned() {
  local marker="$1/.switchyard-content-sha256"

  [ -f "$marker" ] && [ ! -L "$marker" ] &&
    grep -Eq '^[0-9a-f]{12}[[:space:]]*$' "$marker"
}

ensure_swap_helper() {
  if [ -x "$SWAP_HELPER_DIR/switchyard-directory-swap" ]; then
    return 0
  fi

  SWAP_HELPER_DIR="$(mktemp -d)"
  cat > "$SWAP_HELPER_DIR/switchyard-directory-swap.c" <<'EOF'
#include <fcntl.h>
#include <stdio.h>
#include <sys/stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <staged-directory> <live-directory>\n", argv[0]);
        return 2;
    }
    if (renameatx_np(AT_FDCWD, argv[1], AT_FDCWD, argv[2], RENAME_SWAP) != 0)
    {
        perror("renameatx_np(RENAME_SWAP)");
        return 1;
    }
    return 0;
}
EOF
  /usr/bin/clang -Wall -Wextra -Werror \
    "$SWAP_HELPER_DIR/switchyard-directory-swap.c" \
    -o "$SWAP_HELPER_DIR/switchyard-directory-swap"
}

ensure_preview_swap_helper() {
  if [ -x "$SWAP_HELPER_DIR/switchyard-preview-directory-publish" ]; then
    return 0
  fi

  [ -n "${SWAP_HELPER_DIR:-}" ] || SWAP_HELPER_DIR="$(mktemp -d)"
  cat > "$SWAP_HELPER_DIR/switchyard-preview-directory-publish.c" <<'EOF'
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <unistd.h>

static int split_path(const char *path, char parent[PATH_MAX], const char **name)
{
    char *slash;

    if (strlcpy(parent, path, PATH_MAX) >= PATH_MAX) return -1;
    slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return -1;
    *name = path + (slash - parent) + 1;
    if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return 0;
}

static int same_entry(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_mode == right->st_mode;
}

static int named_entry_is(int parent_fd, const char *name, const struct stat *expected)
{
    struct stat current;

    return !fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) &&
           same_entry(&current, expected);
}

static int named_entry_is_missing(int parent_fd, const char *name)
{
    struct stat current;

    errno = 0;
    return fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) == -1 &&
           errno == ENOENT;
}

static int rollback_publication(int staged_parent_fd, const char *staged_name,
                                int live_parent_fd, const char *live_name,
                                const char *mode, const struct stat *staged,
                                const struct stat *live)
{
    if (!strcmp(mode, "exclusive"))
    {
        if (!named_entry_is(live_parent_fd, live_name, staged) ||
            !named_entry_is_missing(staged_parent_fd, staged_name) ||
            renameatx_np(live_parent_fd, live_name, staged_parent_fd, staged_name,
                         RENAME_EXCL))
            return 0;
        return named_entry_is(staged_parent_fd, staged_name, staged) &&
               named_entry_is_missing(live_parent_fd, live_name);
    }
    if (!named_entry_is(live_parent_fd, live_name, staged) ||
        !named_entry_is(staged_parent_fd, staged_name, live) ||
        renameatx_np(staged_parent_fd, staged_name, live_parent_fd, live_name,
                     RENAME_SWAP))
        return 0;
    return named_entry_is(staged_parent_fd, staged_name, staged) &&
           named_entry_is(live_parent_fd, live_name, live);
}

int main(int argc, char **argv)
{
    char staged_parent[PATH_MAX], live_parent[PATH_MAX];
    const char *staged_name, *live_name;
    struct stat staged_opened, live_opened, named, published;
    int staged_parent_fd = -1, live_parent_fd = -1;
    int staged_fd = -1, live_fd = -1;
    int failure_errno = 0;
    const char *injected_failure;
    unsigned int flags;

    if (argc != 4 || (strcmp(argv[3], "exclusive") && strcmp(argv[3], "swap")))
    {
        fprintf(stderr, "usage: %s <staged> <live> <exclusive|swap>\n", argv[0]);
        return 2;
    }
    if (split_path(argv[1], staged_parent, &staged_name) ||
        split_path(argv[2], live_parent, &live_name))
    {
        fprintf(stderr, "preview publication paths are invalid\n");
        return 1;
    }
    staged_parent_fd = open(staged_parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    live_parent_fd = open(live_parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (staged_parent_fd < 0 || live_parent_fd < 0) goto error;
    staged_fd = openat(staged_parent_fd, staged_name,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (staged_fd < 0 || fstat(staged_fd, &staged_opened) ||
        fstatat(staged_parent_fd, staged_name, &named, AT_SYMLINK_NOFOLLOW) ||
        !same_entry(&staged_opened, &named))
        goto changed;

    if (!strcmp(argv[3], "exclusive"))
    {
        if (!fstatat(live_parent_fd, live_name, &named, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
            goto changed;
        flags = RENAME_EXCL;
    }
    else
    {
        live_fd = openat(live_parent_fd, live_name,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (live_fd < 0 || fstat(live_fd, &live_opened) ||
            fstatat(live_parent_fd, live_name, &named, AT_SYMLINK_NOFOLLOW) ||
            !same_entry(&live_opened, &named))
            goto changed;
        flags = RENAME_SWAP;
    }

    if (renameatx_np(staged_parent_fd, staged_name,
                     live_parent_fd, live_name, flags))
        goto error;
    injected_failure = getenv("SWITCHYARD_PREVIEW_PUBLISH_TEST_FAILURE");
    if (injected_failure && !strcmp(injected_failure, "verify"))
    {
        failure_errno = ESTALE;
        goto post_publish_failure;
    }
    if (fstatat(live_parent_fd, live_name, &published, AT_SYMLINK_NOFOLLOW) ||
        !same_entry(&staged_opened, &published))
    {
        failure_errno = errno ? errno : ESTALE;
        goto post_publish_failure;
    }
    if (!strcmp(argv[3], "exclusive"))
    {
        if (!fstatat(staged_parent_fd, staged_name, &named, AT_SYMLINK_NOFOLLOW) ||
            errno != ENOENT)
        {
            failure_errno = errno ? errno : ESTALE;
            goto post_publish_failure;
        }
    }
    else if (fstatat(staged_parent_fd, staged_name, &published, AT_SYMLINK_NOFOLLOW) ||
             !same_entry(&live_opened, &published))
    {
        failure_errno = errno ? errno : ESTALE;
        goto post_publish_failure;
    }
    if (injected_failure && !strcmp(injected_failure, "fsync"))
    {
        failure_errno = EIO;
        goto post_publish_failure;
    }
    if (fsync(staged_parent_fd) ||
        (live_parent_fd != staged_parent_fd && fsync(live_parent_fd)))
    {
        failure_errno = errno ? errno : EIO;
        goto post_publish_failure;
    }
    if (live_fd >= 0) close(live_fd);
    close(staged_fd);
    close(live_parent_fd);
    close(staged_parent_fd);
    return 0;

post_publish_failure:
    if (rollback_publication(staged_parent_fd, staged_name,
                             live_parent_fd, live_name, argv[3],
                             &staged_opened, &live_opened))
    {
        if (fsync(staged_parent_fd) ||
            (live_parent_fd != staged_parent_fd && fsync(live_parent_fd)))
            fprintf(stderr, "preview publication rollback restored identities but directory fsync failed\n");
        errno = failure_errno;
        perror("preview publication failed after rename and was rolled back");
        return 1;
    }
    errno = failure_errno;
    perror("preview publication fail-stop after rename; rollback was ambiguous, preserving both paths");
    return 3;

changed:
    fprintf(stderr, "preview publication path changed during fd-pinned validation\n");
    return 1;
error:
    perror("preview runtime directory publication");
    return 1;
}
EOF
  /usr/bin/clang -Wall -Wextra -Werror \
    "$SWAP_HELPER_DIR/switchyard-preview-directory-publish.c" \
    -o "$SWAP_HELPER_DIR/switchyard-preview-directory-publish"
}

atomic_replace_directory() {
  [ "$#" -eq 3 ] || [ "$#" -eq 4 ] || {
    echo "usage: atomic_replace_directory STAGED LIVE KIND [RUNTIME_PROFILE]" >&2
    return 2
  }

  local staged_directory="$1"
  local live_directory="$2"
  local ownership_kind="$3"
  local expected_runtime_profile="stable-x86_64-rosetta"
  local managed_runtime_root="${SWITCHYARD_MANAGED_RUNTIME_ROOT:-${HOME}/.switchyard/runtimes}"

  if [ "$#" -eq 4 ]; then
    expected_runtime_profile="$4"
  fi
  if [ "$ownership_kind" = runtime ]; then
    case "$expected_runtime_profile" in
      stable-x86_64-rosetta|preview-native-arm64-fex) ;;
      *)
        echo "Runtime directory replacement requires a known runtime profile." >&2
        return 1
        ;;
    esac
  elif [ "$#" -eq 4 ]; then
    echo "A runtime profile may be supplied only for runtime replacement." >&2
    return 2
  fi

  case "$staged_directory:$live_directory" in
    /*:/*) ;;
    *)
      echo "Atomic directory replacement requires absolute paths." >&2
      return 1
      ;;
  esac
  if [ ! -d "$staged_directory" ] || [ -L "$staged_directory" ]; then
    echo "Refusing to promote a staged path that is not a real directory: $staged_directory" >&2
    return 1
  fi
  if [ "$ownership_kind" = runtime ] &&
     [ "$expected_runtime_profile" = preview-native-arm64-fex ]; then
    preview_runtime_replacement_paths_are_safe \
      "$staged_directory" "$live_directory" || return 1
  fi

  if [ ! -e "$live_directory" ] && [ ! -L "$live_directory" ]; then
    if [ "$ownership_kind" = runtime ] &&
       [ "$expected_runtime_profile" = preview-native-arm64-fex ]; then
      ensure_preview_swap_helper
      "$SWAP_HELPER_DIR/switchyard-preview-directory-publish" \
        "$staged_directory" "$live_directory" exclusive
      return
    fi
    mv "$staged_directory" "$live_directory"
    return 0
  fi
  if [ ! -d "$live_directory" ] || [ -L "$live_directory" ]; then
    echo "Refusing to replace a non-directory or symbolic-link destination: $live_directory" >&2
    return 1
  fi
  if replacement_target_is_dangerous "$live_directory"; then
    echo "Refusing to replace a dangerous ancestor directory: $live_directory" >&2
    return 1
  fi

  case "$ownership_kind" in
    cache)
      # The content digest decides whether a cache may be reused, but the
      # regular marker decides ownership. A damaged owned cache must remain
      # replaceable so integrity verification can self-heal it.
      if ! cache_directory_is_owned "$live_directory"; then
        echo "Refusing to replace an unowned Switchyard cache directory: $live_directory" >&2
        return 1
      fi
      ;;
    runtime)
      mkdir -p "$managed_runtime_root"
      if path_is_same_or_ancestor "$live_directory" "$managed_runtime_root"; then
        echo "Refusing to replace the managed runtime root or one of its ancestors: $live_directory" >&2
        return 1
      fi
      if ! path_is_strict_descendant "$live_directory" "$managed_runtime_root" &&
         ! runtime_directory_is_owned "$live_directory" "$expected_runtime_profile"; then
        echo "Refusing to replace an unmanaged runtime directory: $live_directory" >&2
        return 1
      fi
      ;;
    *)
      echo "Unknown directory ownership kind: $ownership_kind" >&2
      return 1
      ;;
  esac

  if [ "$ownership_kind" = runtime ] &&
     [ "$expected_runtime_profile" = preview-native-arm64-fex ]; then
    # Reopen and revalidate every ancestor immediately before the fd-relative
    # swap.  Non-sticky writable ancestors were already rejected, so only a
    # same-euid actor can mutate these paths after this point.
    preview_runtime_replacement_paths_are_safe \
      "$staged_directory" "$live_directory" || return 1
    ensure_preview_swap_helper
    "$SWAP_HELPER_DIR/switchyard-preview-directory-publish" \
      "$staged_directory" "$live_directory" swap || return $?
    if rm -rf "$staged_directory"; then
      return 0
    fi
    echo "Preview runtime publication committed but old-runtime cleanup failed; preserving the staged path." >&2
    return 3
  fi

  ensure_swap_helper
  "$SWAP_HELPER_DIR/switchyard-directory-swap" "$staged_directory" "$live_directory"
  rm -rf "$staged_directory"
}
