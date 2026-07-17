#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT_DIR/switchyard/tls-deps.tsv"

fail() {
  echo "TLS package verification failed: $*" >&2
  exit 1
}

[ -f "$MANIFEST" ] || fail "missing $MANIFEST"

package_count=0
seen_names=""
has_gnutls=0
while IFS=$'\t' read -r name version build filename sha256 extra; do
  case "$name" in
    ''|'#'*) continue ;;
  esac

  [ -z "${extra:-}" ] || fail "unexpected extra field for $name"
  [[ "$name" =~ ^[a-z0-9][a-z0-9+._-]*$ ]] || fail "invalid package name $name"
  [[ "$version" =~ ^[0-9][A-Za-z0-9.+_-]*$ ]] || fail "invalid version for $name"
  [[ "$build" =~ ^[A-Za-z0-9._-]+$ ]] || fail "invalid build for $name"
  [[ "$filename" == "$name-$version-$build.conda" || "$filename" == "$name-$version-$build.tar.bz2" ]] ||
    fail "filename does not match package identity for $name"
  [[ "$sha256" =~ ^[0-9a-f]{64}$ ]] || fail "invalid sha256 for $name"
  if printf '%s\n' "$seen_names" | grep -Fx "$name" >/dev/null 2>&1; then
    fail "duplicate package $name"
  fi

  seen_names="${seen_names}${seen_names:+$'\n'}$name"
  package_count=$((package_count + 1))
  [ "$name" = "gnutls" ] && has_gnutls=1
done < "$MANIFEST"

[ "$package_count" -ge 8 ] || fail "expected a complete TLS dependency closure"
[ "$has_gnutls" -eq 1 ] || fail "manifest does not pin GnuTLS"

echo "verified $package_count pinned redistributable TLS packages"
