#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENTITLEMENTS="$ROOT_DIR/switchyard/wine-runtime.entitlements"
EXPECTED_TEAM_ID="${SWITCHYARD_DEVELOPER_TEAM_ID:-M3CULMDKU3}"
RUNTIME=""
OUTPUT_DIR=""
IDENTITY="${SWITCHYARD_CODESIGN_IDENTITY:-}"
NOTARY_PROFILE="${SWITCHYARD_NOTARY_PROFILE:-}"

usage() {
  cat >&2 <<EOF
usage: $0 --runtime PATH --output DIR --identity IDENTITY [--notary-profile PROFILE]
EOF
  exit 2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --runtime)
      [ "$#" -ge 2 ] || usage
      RUNTIME="$2"
      shift 2
      ;;
    --output)
      [ "$#" -ge 2 ] || usage
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --identity)
      [ "$#" -ge 2 ] || usage
      IDENTITY="$2"
      shift 2
      ;;
    --notary-profile)
      [ "$#" -ge 2 ] || usage
      NOTARY_PROFILE="$2"
      shift 2
      ;;
    *) usage ;;
  esac
done

[ -n "$RUNTIME" ] || usage
[ -n "$OUTPUT_DIR" ] || usage
[ -n "$IDENTITY" ] || usage
[ -d "$RUNTIME" ] || { echo "runtime does not exist: $RUNTIME" >&2; exit 1; }
[ ! -L "$RUNTIME" ] || { echo "runtime path must not be a symbolic link" >&2; exit 1; }
[ -f "$RUNTIME/switchyard-runtime.json" ] || { echo "runtime manifest is missing" >&2; exit 1; }
[ -f "$ENTITLEMENTS" ] || { echo "runtime signing entitlements are missing" >&2; exit 1; }

manifest_value() {
  /usr/bin/plutil -extract "$1" raw -o - "$2" 2>/dev/null || true
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

content_tree_digest() {
  local root="$1"
  (
    cd "$root"
    /usr/bin/find . \( -type f -o -type l \) ! -path './.switchyard-content-sha256' -print |
      LC_ALL=C /usr/bin/sort |
      while IFS= read -r item; do
        if [ -L "$item" ]; then
          /usr/bin/printf 'link %s %s\n' "$item" "$(/usr/bin/readlink "$item")"
        else
          /usr/bin/printf 'file %s %s\n' "$item" "$(sha256_file "$item")"
        fi
      done
  ) | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
}

manifest="$RUNTIME/switchyard-runtime.json"
runtime_id="$(manifest_value id "$manifest")"
source_revision="$(manifest_value sourceRevision "$manifest")"
source_dirty="$(manifest_value sourceDirty "$manifest")"
gptk_path="$(manifest_value gptkPath "$manifest")"
gptk_digest="$(manifest_value gptkRedistDigest "$manifest")"
current_revision="$(git -C "$ROOT_DIR" rev-parse HEAD)"

[ -n "$runtime_id" ] || { echo "runtime manifest has no id" >&2; exit 1; }
[ "$source_revision" = "$current_revision" ] || {
  echo "runtime source $source_revision does not match current source $current_revision" >&2
  exit 1
}
[ "$source_dirty" = "false" ] || { echo "release runtime was built from a dirty source tree" >&2; exit 1; }
[ -z "$gptk_path" ] || { echo "release runtime records a user-provided GPTK path" >&2; exit 1; }
[ "$gptk_digest" = "no-gptk" ] || { echo "release runtime contains a GPTK overlay" >&2; exit 1; }

for required_notice in \
  share/doc/switchyard-wine/LICENSE \
  share/doc/switchyard-wine/COPYING.LIB \
  share/doc/switchyard-wine/AUTHORS \
  share/doc/switchyard-wine/CORRESPONDING-SOURCE.txt \
  lib/switchyard-tls/share/doc/switchyard-tls/packages.tsv \
  lib/switchyard-tls/share/doc/switchyard-tls/sources.tsv; do
  [ -f "$RUNTIME/$required_notice" ] || {
    echo "release runtime is missing notice $required_notice" >&2
    exit 1
  }
done

if /usr/bin/find "$RUNTIME" \( -iname '*d3dmetal*' -o -iname '*metalirconverter*' -o -iname 'libd3dshared*' \) -print -quit |
   /usr/bin/grep -q .; then
  echo "release runtime contains a user-provided Apple graphics component" >&2
  exit 1
fi
if /usr/bin/grep -R -I -l -E '/Users/[^/]+/.+(Game.Porting.Toolkit|heroic)' \
     "$RUNTIME/switchyard-runtime.json" "$RUNTIME/lib/switchyard-tls/share/doc" 2>/dev/null |
   /usr/bin/grep -q .; then
  echo "release runtime contains a user-local toolkit provenance path" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"
release_short_revision="${source_revision:0:12}"
release_root_name="Switchyard-Wine-Runtime-${release_short_revision}-macos-x86_64"
signed_runtime="$OUTPUT_DIR/$release_root_name"
archive_name="${release_root_name}.zip"
archive="$OUTPUT_DIR/$archive_name"
release_manifest="$OUTPUT_DIR/switchyard-runtime-release.json"
checksum_file="$OUTPUT_DIR/${archive_name}.sha256"

for destination in "$signed_runtime" "$archive" "$release_manifest" "$checksum_file"; do
  [ ! -e "$destination" ] || {
    echo "release output already exists: $destination" >&2
    exit 1
  }
done

echo "cloning runtime into release staging"
/bin/cp -cR "$RUNTIME" "$signed_runtime"
portable_manifest="$signed_runtime/switchyard-runtime.json"
/usr/bin/plutil -replace installPrefix -string "." "$portable_manifest"
/usr/bin/plutil -replace executable -string "bin/switchyard-wine" "$portable_manifest"

mach_o_list="$(/usr/bin/mktemp)"
verification_log="$(/usr/bin/mktemp)"
prefix=""
cleanup() {
  if [ -n "$prefix" ]; then
    WINEPREFIX="$prefix" "$signed_runtime/bin/switchyard-wineserver" -k >/dev/null 2>&1 || true
    /bin/rm -rf "$prefix"
  fi
  /bin/rm -f "$mach_o_list" "$verification_log"
}
trap cleanup EXIT

/usr/bin/find "$signed_runtime" -type f -print0 |
while IFS= read -r -d '' item; do
  if /usr/bin/file -b "$item" | /usr/bin/grep -q 'Mach-O'; then
    /usr/bin/printf '%s\0' "$item"
  fi
done > "$mach_o_list"

mach_o_count="$(/usr/bin/python3 - "$mach_o_list" <<'PY'
import sys
print(open(sys.argv[1], 'rb').read().count(bytes([0])))
PY
)"
[ "$mach_o_count" -gt 0 ] || { echo "release runtime has no Mach-O files" >&2; exit 1; }

echo "signing $mach_o_count Mach-O files"
while IFS= read -r -d '' item; do
  /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp "$item"
done < "$mach_o_list"

for launcher in \
  "$signed_runtime/lib/wine/x86_64-unix/wine" \
  "$signed_runtime/bin/wine.switchyard-real"; do
  [ -f "$launcher" ] || { echo "release runtime is missing launcher $launcher" >&2; exit 1; }
  /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" "$launcher"
done

wine_sha256="$(sha256_file "$signed_runtime/lib/wine/x86_64-unix/wine")"
/usr/bin/plutil -replace integrity.wineUnixSha256 -string "$wine_sha256" "$portable_manifest"

while IFS= read -r -d '' item; do
  if ! /usr/bin/codesign --verify --strict --verbose=2 "$item" >"$verification_log" 2>&1; then
    /bin/cat "$verification_log" >&2
    exit 1
  fi
done < "$mach_o_list"

signing_details="$(/usr/bin/codesign -d --verbose=4 "$signed_runtime/lib/wine/x86_64-unix/wine" 2>&1)"
/usr/bin/printf '%s\n' "$signing_details" | /usr/bin/grep -F "TeamIdentifier=$EXPECTED_TEAM_ID" >/dev/null || {
  echo "signed runtime has an unexpected Developer Team ID" >&2
  exit 1
}
/usr/bin/printf '%s\n' "$signing_details" | /usr/bin/grep -F 'Runtime Version=' >/dev/null || {
  echo "signed runtime is missing Hardened Runtime" >&2
  exit 1
}

prefix="$(/usr/bin/mktemp -d /tmp/switchyard-release-prefix.XXXXXX)"
smoke_output="$(WINEPREFIX="$prefix" WINEDEBUG=-all "$signed_runtime/bin/switchyard-wine" cmd /c ver)"
/usr/bin/printf '%s' "$smoke_output" | /usr/bin/grep -F 'Microsoft Windows 10.0.19045' >/dev/null || {
  echo "signed runtime failed the fresh-prefix smoke test" >&2
  exit 1
}
WINEPREFIX="$prefix" "$signed_runtime/bin/switchyard-wineserver" -k >/dev/null 2>&1 || true
/bin/sleep 1
/bin/rm -rf "$prefix"
prefix=""

content_tree_digest "$signed_runtime" > "$signed_runtime/.switchyard-content-sha256"
echo "creating $archive_name"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$signed_runtime" "$archive"
archive_sha256="$(sha256_file "$archive")"
archive_size="$(/usr/bin/stat -f '%z' "$archive")"
/usr/bin/printf '%s  %s\n' "$archive_sha256" "$archive_name" > "$checksum_file"

notary_status="not-submitted"
notary_id=""
if [ -n "$NOTARY_PROFILE" ]; then
  notary_result="$(/usr/bin/mktemp)"
  /usr/bin/xcrun notarytool submit "$archive" --keychain-profile "$NOTARY_PROFILE" \
    --wait --output-format json > "$notary_result"
  notary_status="$(manifest_value status "$notary_result")"
  notary_id="$(manifest_value id "$notary_result")"
  /bin/rm -f "$notary_result"
  [ "$notary_status" = "Accepted" ] || {
    echo "Apple notarization did not accept the runtime archive: $notary_status" >&2
    exit 1
  }
fi

cat > "$release_manifest" <<EOF
{
  "schemaVersion": 1,
  "runtimeID": "$runtime_id",
  "sourceRevision": "$source_revision",
  "archive": "$archive_name",
  "archiveSha256": "$archive_sha256",
  "archiveSize": $archive_size,
  "platform": "macos",
  "hostArchitecture": "x86_64",
  "peArchitectures": ["i386", "x86_64"],
  "developerTeamID": "$EXPECTED_TEAM_ID",
  "notarizationStatus": "$notary_status",
  "notarizationID": "$notary_id"
}
EOF

echo "runtime release archive: $archive"
echo "runtime release manifest: $release_manifest"
echo "runtime archive sha256: $archive_sha256"
echo "runtime notarization: $notary_status${notary_id:+ ($notary_id)}"
