#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${FONT_ASSET_MANIFEST:-$ROOT_DIR/switchyard/font-assets.tsv}"
MODE="${1:-}"
CACHE_DIR="${FONT_ASSET_DOWNLOAD_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Fonts/assets/noto-monthly-release-2026.07.01}"
ALIAS_SCRIPT="$ROOT_DIR/switchyard/make_font_alias.py"
ALIAS_SOURCE="NotoSansCJK-Regular.ttc"
ALIAS_FAMILY="Arial Unicode MS"
ALIAS_POSTSCRIPT="ArialUnicodeMS"
ALIAS_FACE_INDEX=1
ALIAS_SHA256="ccdd3bd646d95b31513e10ad9c975d878c0ef8b25ff2d92f2e635b50218b128e"

case "$MODE" in
  ''|--download) ;;
  *)
    echo "usage: $0 [--download]" >&2
    exit 2
    ;;
esac

if [ ! -f "$MANIFEST" ]; then
  echo "font asset verification failed: missing $MANIFEST" >&2
  exit 1
fi
if [ ! -f "$ALIAS_SCRIPT" ]; then
  echo "font asset verification failed: missing $ALIAS_SCRIPT" >&2
  exit 1
fi

sha256_file() {
  shasum -a 256 "$1" | awk '{print $1}'
}

font_count=0
license_count=0
seen_names=$'\n'
family_catalog=""

while IFS=$'\t' read -r kind name expected_hash url extra; do
  case "$kind" in
    ''|'#'*) continue ;;
    font) font_count=$((font_count + 1)) ;;
    license) license_count=$((license_count + 1)) ;;
    *)
      echo "font asset verification failed: unsupported type '$kind'" >&2
      exit 1
      ;;
  esac

  if [ -n "${extra:-}" ] || [ -z "$name" ] || [ -z "$expected_hash" ] || [ -z "$url" ]; then
    echo "font asset verification failed: malformed row for '$name'" >&2
    exit 1
  fi
  case "$name" in
    */*|.*|'')
      echo "font asset verification failed: unsafe file name '$name'" >&2
      exit 1
      ;;
  esac
  case "$seen_names" in
    *$'\n'"$name"$'\n'*)
      echo "font asset verification failed: duplicate file '$name'" >&2
      exit 1
      ;;
  esac
  seen_names+="$name"$'\n'

  if ! printf '%s\n' "$expected_hash" | grep -Eq '^[0-9a-f]{64}$'; then
    echo "font asset verification failed: invalid sha256 for '$name'" >&2
    exit 1
  fi
  case "$url" in
    https://raw.githubusercontent.com/*) ;;
    *)
      echo "font asset verification failed: unpinned or non-HTTPS URL for '$name'" >&2
      exit 1
      ;;
  esac
  if [ "$kind" = "font" ]; then
    case "$name" in
      *.ttf|*.ttc|*.otf) ;;
      *)
        echo "font asset verification failed: unsupported font extension for '$name'" >&2
        exit 1
        ;;
    esac
  fi

  if [ "$MODE" = "--download" ]; then
    mkdir -p "$CACHE_DIR"
    asset="$CACHE_DIR/$name"
    if [ ! -f "$asset" ] || [ "$(sha256_file "$asset")" != "$expected_hash" ]; then
      temporary_asset="$asset.tmp.$$"
      rm -f "$temporary_asset"
      curl -fL --retry 3 --connect-timeout 20 -o "$temporary_asset" "$url"
      if [ "$(sha256_file "$temporary_asset")" != "$expected_hash" ]; then
        rm -f "$temporary_asset"
        echo "font asset verification failed: sha256 mismatch for '$name'" >&2
        exit 1
      fi
      mv "$temporary_asset" "$asset"
    fi
    if [ "$kind" = "font" ] && command -v fc-scan >/dev/null 2>&1; then
      scanned_families="$(fc-scan --format '%{family}\n' "$asset")"
      if [ -z "$scanned_families" ]; then
        echo "font asset verification failed: no family metadata in '$name'" >&2
        exit 1
      fi
      family_catalog+="$scanned_families"$'\n'
    fi
  fi
done < "$MANIFEST"

if [ "$font_count" -lt 1 ] || [ "$license_count" -lt 1 ]; then
  echo "font asset verification failed: manifest needs fonts and license notices" >&2
  exit 1
fi

for required_file in \
  NotoSans-Regular.ttf \
  NotoSans-Bold.ttf \
  NotoSerif-Regular.ttf \
  NotoSansMono-Regular.ttf \
  NotoSansCJK-Regular.ttc \
  NotoSansCJK-Bold.ttc \
  NotoSansSymbols2-Regular.ttf; do
  case "$seen_names" in
    *$'\n'"$required_file"$'\n'*) ;;
    *)
      echo "font asset verification failed: required file '$required_file' is absent" >&2
      exit 1
      ;;
  esac
done

if [ "$MODE" = "--download" ] && command -v fc-scan >/dev/null 2>&1; then
  for required_family in \
    "Noto Sans" \
    "Noto Serif" \
    "Noto Sans Mono" \
    "Noto Sans CJK JP" \
    "Noto Sans CJK KR" \
    "Noto Sans CJK SC" \
    "Noto Sans CJK TC" \
    "Noto Sans CJK HK" \
    "Noto Sans Symbols 2"; do
    if ! printf '%s\n' "$family_catalog" | grep -Fqx "$required_family"; then
      echo "font asset verification failed: required family '$required_family' was not found" >&2
      exit 1
    fi
  done
fi

if [ "$MODE" = "--download" ]; then
  alias_directory="$(mktemp -d "${TMPDIR:-/tmp}/switchyard-font-alias.XXXXXX")"
  trap 'rm -rf "$alias_directory"' EXIT
  python3 "$ALIAS_SCRIPT" \
    "$CACHE_DIR/$ALIAS_SOURCE" \
    "$alias_directory/ArialUnicodeMS.otf" \
    --face-index "$ALIAS_FACE_INDEX" \
    --family "$ALIAS_FAMILY" \
    --postscript "$ALIAS_POSTSCRIPT"
  if [ "$(sha256_file "$alias_directory/ArialUnicodeMS.otf")" != "$ALIAS_SHA256" ]; then
    echo "font asset verification failed: generated compatibility alias has an unexpected sha256" >&2
    exit 1
  fi
  if command -v fc-scan >/dev/null 2>&1; then
    alias_families="$(fc-scan --format '%{family}\n' "$alias_directory/ArialUnicodeMS.otf")"
    if ! printf '%s\n' "$alias_families" | grep -Fqx "$ALIAS_FAMILY"; then
      echo "font asset verification failed: compatibility family '$ALIAS_FAMILY' was not found" >&2
      exit 1
    fi
  fi
  rm -rf "$alias_directory"
  trap - EXIT
fi

echo "verified $font_count redistributable source fonts, 1 compatibility alias, and $license_count license notices"
