#!/usr/bin/env bash

# Produce a content-sensitive identity for the source state consumed by a
# runtime build. Ignored files are intentionally excluded because build caches
# and local tooling output must not invalidate an otherwise reproducible build.
switchyard_source_state_fingerprint() {
  local root="$1"
  local revision

  (
    cd "$root" || exit 1
    revision="$(git rev-parse --verify HEAD)"
    printf 'head\0%s\0' "$revision"
    git diff --binary --no-ext-diff HEAD --
    git ls-files --others --exclude-standard -z |
      while IFS= read -r -d '' path; do
        printf 'untracked\0%s\0' "$path"
        if [ -L "$path" ]; then
          printf 'link\0%s\0' "$(readlink "$path")"
        elif [ -f "$path" ]; then
          printf 'file\0'
          shasum -a 256 "$path" | awk '{printf "%s%c", $1, 0}'
        else
          printf 'special\0%s\0' "$(stat -f '%HT %p' "$path")"
        fi
      done
  ) | shasum -a 256 | awk '{print $1}'
}
