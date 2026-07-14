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
  local root="$1"
  local manifest="$root/switchyard-runtime.json"
  local manifest_id
  local manifest_install_prefix
  local manifest_executable

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

atomic_replace_directory() {
  local staged_directory="$1"
  local live_directory="$2"
  local ownership_kind="$3"
  local managed_runtime_root="${SWITCHYARD_MANAGED_RUNTIME_ROOT:-${HOME}/.switchyard/runtimes}"

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

  if [ ! -e "$live_directory" ] && [ ! -L "$live_directory" ]; then
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
         ! runtime_directory_is_owned "$live_directory"; then
        echo "Refusing to replace an unmanaged runtime directory: $live_directory" >&2
        return 1
      fi
      ;;
    *)
      echo "Unknown directory ownership kind: $ownership_kind" >&2
      return 1
      ;;
  esac

  ensure_swap_helper
  "$SWAP_HELPER_DIR/switchyard-directory-swap" "$staged_directory" "$live_directory"
  rm -rf "$staged_directory"
}
