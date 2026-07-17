#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT_DIR/switchyard/tls-deps.tsv"
SOURCE_MANIFEST="$ROOT_DIR/switchyard/tls-source-deps.tsv"

fail() {
  echo "TLS package verification failed: $*" >&2
  exit 1
}

[ -f "$MANIFEST" ] || fail "missing $MANIFEST"
[ -f "$SOURCE_MANIFEST" ] || fail "missing $SOURCE_MANIFEST"

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

source_count=0
while IFS=$'\t' read -r name version filename url sha256 extra; do
  case "$name" in
    ''|'#'*) continue ;;
  esac

  [ -z "${extra:-}" ] || fail "unexpected extra source field for $name"
  [[ "$name" =~ ^[a-z0-9][a-z0-9+._-]*$ ]] || fail "invalid source package name $name"
  [[ "$version" =~ ^[0-9][A-Za-z0-9.+_-]*$ ]] || fail "invalid source version for $name"
  [ "$filename" = "$name-$version.tar.gz" ] || fail "source filename does not match $name"
  [[ "$url" == https://* ]] || fail "source URL must use HTTPS for $name"
  [[ "$sha256" =~ ^[0-9a-f]{64}$ ]] || fail "invalid source sha256 for $name"
  source_count=$((source_count + 1))
done < "$SOURCE_MANIFEST"

[ "$source_count" -eq 1 ] || fail "expected one pinned source-built TLS dependency"

echo "verified $package_count pinned redistributable TLS packages and $source_count source build"
