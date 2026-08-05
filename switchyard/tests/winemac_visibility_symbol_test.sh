#!/usr/bin/env bash
set -euo pipefail

runtime_or_driver="${1:-}"

if [ -z "$runtime_or_driver" ]; then
  echo "usage: $0 RUNTIME_OR_WINEMAC_DRIVER" >&2
  exit 2
fi

if [ -d "$runtime_or_driver" ]; then
  winemac_driver="$runtime_or_driver/lib/wine/x86_64-unix/winemac.so"
else
  winemac_driver="$runtime_or_driver"
fi

if [ ! -f "$winemac_driver" ]; then
  echo "winemac driver is missing: $winemac_driver" >&2
  exit 1
fi

if ! symbols="$(/usr/bin/nm -m "$winemac_driver" 2>&1)"; then
  echo "failed to inspect winemac driver symbols: $winemac_driver" >&2
  echo "$symbols" >&2
  exit 1
fi

if printf '%s\n' "$symbols" |
   /usr/bin/grep -E '(^|[[:space:]])_IsWindowVisible([[:space:]]|$|\()' >/dev/null; then
  echo "winemac driver resolves IsWindowVisible through the macOS Carbon API." >&2
  exit 1
fi

echo "winemac_visibility_symbols=ok"
