#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIGEST_TOOL="$ROOT_DIR/switchyard/runtime_content_digest.py"
TEST_ROOT="$(mktemp -d)"
PYTHON3="${PYTHON3:-python3}"

cleanup() {
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

runtime="$TEST_ROOT/runtime"
mkdir -p "$runtime/bin" "$runtime/empty"
printf 'runtime\n' > "$runtime/bin/wine"
chmod 0755 "$runtime/bin/wine"
ln -s 'wine' "$runtime/bin/wine64"

"$PYTHON3" "$DIGEST_TOOL" write "$runtime" >/dev/null
"$PYTHON3" "$DIGEST_TOOL" verify "$runtime"
original="$("$PYTHON3" "$DIGEST_TOOL" digest "$runtime")"
[ "${#original}" -eq 64 ]

staging="$TEST_ROOT/staging"
cp -R "$runtime" "$staging"
"$PYTHON3" "$DIGEST_TOOL" verify "$staging"
printf 'copy race\n' >> "$staging/bin/wine"
if "$PYTHON3" "$DIGEST_TOOL" verify "$staging"; then
  echo "expected modified release staging content to fail verification" >&2
  exit 1
fi

printf 'tampered\n' >> "$runtime/bin/wine"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected modified runtime content to fail verification" >&2
  exit 1
fi
"$PYTHON3" "$DIGEST_TOOL" write "$runtime" >/dev/null
"$PYTHON3" "$DIGEST_TOOL" verify "$runtime"
[ "$("$PYTHON3" "$DIGEST_TOOL" digest "$runtime")" != "$original" ]
printf 'runtime\n' > "$runtime/bin/wine"
chmod 0755 "$runtime/bin/wine"
"$PYTHON3" "$DIGEST_TOOL" write "$runtime" >/dev/null
"$PYTHON3" "$DIGEST_TOOL" verify "$runtime"

chmod 0777 "$runtime"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected modified runtime root permissions to fail verification" >&2
  exit 1
fi
chmod 0755 "$runtime"

chmod 0666 "$runtime/.switchyard-content-sha256"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected unsafe digest marker permissions to fail verification" >&2
  exit 1
fi
chmod 0644 "$runtime/.switchyard-content-sha256"

chmod 0644 "$runtime/bin/wine"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected modified runtime permissions to fail verification" >&2
  exit 1
fi
chmod 0755 "$runtime/bin/wine"

rm -rf "$runtime/empty"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected a removed empty directory to fail verification" >&2
  exit 1
fi
mkdir "$runtime/empty"

rm "$runtime/bin/wine64"
ln -s $'wine\nfile injected' "$runtime/bin/wine64"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected a changed newline-containing link target to fail verification" >&2
  exit 1
fi
rm "$runtime/bin/wine64"
ln -s 'wine' "$runtime/bin/wine64"

newline_file="$runtime/bin/line"$'\n'"file"
printf 'newline\n' > "$newline_file"
newline_digest="$("$PYTHON3" "$DIGEST_TOOL" digest "$runtime")"
[ "$newline_digest" != "$original" ]
rm "$newline_file"

mkfifo "$runtime/unsupported"
if "$PYTHON3" "$DIGEST_TOOL" digest "$runtime" >/dev/null 2>&1; then
  echo "expected a special file to be rejected" >&2
  exit 1
fi
rm "$runtime/unsupported"

printf '%064d\nextra\n' 0 > "$runtime/.switchyard-content-sha256"
if "$PYTHON3" "$DIGEST_TOOL" verify "$runtime"; then
  echo "expected a malformed marker to fail verification" >&2
  exit 1
fi

echo "runtime content digest tests passed"
