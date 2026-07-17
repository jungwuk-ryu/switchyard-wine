#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_DIR="$ROOT_DIR"
SOURCE_REPOSITORY="${SWITCHYARD_WINE_SOURCE_REPOSITORY:-https://github.com/jungwuk-ryu/switchyard-wine}"
UPSTREAM_BASE_FILE="$ROOT_DIR/switchyard/upstream-base.txt"
MODE="${1:-build}"

case "$MODE" in
  build|--ensure|--source-info|--verify-tls) ;;
  *)
    echo "usage: $0 [--ensure|--source-info|--verify-tls]" >&2
    exit 2
    ;;
esac

BUILD_PROFILE="switchyard-wow64-pe"
PE_ARCHS=("i386" "x86_64")
WINE_GRAPHICS_FALLBACK_MODULES=("d3d10" "d3d11" "d3d12" "dcomp" "dwmapi" "dxgi" "wined3d")
WINE_MONO_VERSION="11.2.0"
WINE_MONO_ARCH="x86"
WINE_MONO_SHA256="b4525679e7da30d4658ceb85739cbc55c771791054abbb4b3152fe96ded0b897"
WINE_MONO_FILE="wine-mono-${WINE_MONO_VERSION}-${WINE_MONO_ARCH}.msi"
WINE_MONO_URL="https://dl.winehq.org/wine/wine-mono/${WINE_MONO_VERSION}/${WINE_MONO_FILE}"
WINE_MONO_CACHE_DIR="${WINE_MONO_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Wine/addons/mono}"
VULKAN_LOADER_VERSION="1.4.350.1"
VULKAN_LOADER_BOTTLE="vulkan-loader--${VULKAN_LOADER_VERSION}.tahoe.bottle.tar.gz"
VULKAN_LOADER_REPOSITORY="homebrew/core/vulkan-loader"
VULKAN_LOADER_LAYER_SHA256="03185dd14f4a4501875b38cac7b69f11a2dd6921df4deaf7436aed74d62186e0"
VULKAN_LOADER_MANIFEST_DIGEST="sha256:ecfcd7a2cb9fd52f60b200e9feaa7448057435de86ed504bfa44ac22a7d38149"
VULKAN_HEADERS_VERSION="1.4.350.1"
VULKAN_HEADERS_BOTTLE="vulkan-headers--${VULKAN_HEADERS_VERSION}.tahoe.bottle.tar.gz"
VULKAN_HEADERS_REPOSITORY="homebrew/core/vulkan-headers"
VULKAN_HEADERS_LAYER_SHA256="b482fc6a2e4831ae1b572370791cffb91f44ba08908885ee579d44fdfe1f43d0"
VULKAN_HEADERS_MANIFEST_DIGEST="sha256:c7f375dee3dc83d989457e74db0636eef966d79deb57ed98dafe8b44e07bc56b"
MOLTENVK_VERSION="1.4.1"
MOLTENVK_BOTTLE="molten-vk--${MOLTENVK_VERSION}.sonoma.bottle.tar.gz"
MOLTENVK_REPOSITORY="homebrew/core/molten-vk"
MOLTENVK_LAYER_SHA256="9bb2d88ee0ed7cd035f982a59a2e9c5878237c9f4df88117172ccdbc5127f6d9"
MOLTENVK_MANIFEST_DIGEST="sha256:6facac52c2f0f948cf185cf97f5f941c4d2f55a75e5e19d7e259e807597afd94"
VULKAN_CACHE_DIR="${VULKAN_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Vulkan}"
VULKAN_DEPS_PREFIX="${VULKAN_DEPS_PREFIX:-${HOME}/.switchyard/deps/vulkan/x86_64-loader-${VULKAN_LOADER_VERSION}-moltenvk-${MOLTENVK_VERSION}}"
FONT_DEPS_CACHE_DIR="${FONT_DEPS_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Fonts/deps}"
FONT_DEPS_PREFIX="${FONT_DEPS_PREFIX:-${HOME}/.switchyard/deps/fonts/x86_64-freetype-2.14.3-fontconfig-2.18.1}"
FONT_DLOPEN_FREETYPE="@loader_path/../../switchyard-fonts/lib/libfreetype.6.dylib"
FONT_DLOPEN_FONTCONFIG="@loader_path/../../switchyard-fonts/lib/libfontconfig.1.dylib"
FONT_DEPS_NAMES=("freetype" "fontconfig" "libpng" "gettext" "libunistring")
FONT_DEPS_VERSIONS=("2.14.3" "2.18.1" "1.6.58" "1.0" "1.4.2")
FONT_DEPS_REPOSITORIES=("homebrew/core/freetype" "homebrew/core/fontconfig" "homebrew/core/libpng" "homebrew/core/gettext" "homebrew/core/libunistring")
FONT_DEPS_LAYER_SHA256=(
  "c266877a4676016b189131c87355f3e9be0d5e0edbe3a464b5b6ef039945f199"
  "9550776a54e32d8340966173a5d30d337a9f9984030bbdf7233eed792ad5d69c"
  "c74a40635359b753e614fb0a69a32149179a27f79d3338d5c5b685f66e223967"
  "2cc112cce103be3beb13cc8ba67f521d4e972c4082fd69868d34920d63120c09"
  "fbb3a7908a19f306823dbd51b417705c73f710a9a1fb1e34ba7aa67a3c966094"
)
FONT_ASSET_SET_VERSION="noto-monthly-release-2026.07.01-cjk-2.004-aliases-1"
FONT_ASSET_MANIFEST="$ROOT_DIR/switchyard/font-assets.tsv"
FONT_ASSET_DOWNLOAD_CACHE_DIR="${FONT_ASSET_DOWNLOAD_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Fonts/assets/noto-monthly-release-2026.07.01}"
FONT_ASSET_PREFIX="${FONT_ASSET_PREFIX:-${HOME}/.switchyard/deps/fonts/assets-${FONT_ASSET_SET_VERSION}}"
FONT_ALIAS_SCRIPT="$ROOT_DIR/switchyard/make_font_alias.py"
FONT_ALIAS_SOURCE="NotoSansCJK-Regular.ttc"
FONT_ALIAS_FILE="ArialUnicodeMS.otf"
FONT_ALIAS_FAMILY="Arial Unicode MS"
FONT_ALIAS_POSTSCRIPT="ArialUnicodeMS"
FONT_ALIAS_FACE_INDEX=1
FONT_ALIAS_SHA256="ccdd3bd646d95b31513e10ad9c975d878c0ef8b25ff2d92f2e635b50218b128e"
TLS_PACKAGE_MANIFEST="$ROOT_DIR/switchyard/tls-deps.tsv"
TLS_PACKAGE_BASE_URL="https://conda.anaconda.org/conda-forge/osx-64"
TLS_RUNTIME_LAYOUT_VERSION="3"
TLS_PACKAGE_CACHE_DIR="${TLS_PACKAGE_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/TLS/packages}"
TLS_DEPS_CACHE_DIR="${TLS_DEPS_CACHE_DIR:-${HOME}/.switchyard/deps/tls}"
USER_SET_WINE_BUILD_DIR="${WINE_BUILD_DIR+x}"
WINE_BUILD_DIR="${WINE_BUILD_DIR:-}"
USER_SET_WINE_INSTALL_PREFIX="${WINE_INSTALL_PREFIX+x}"
DISABLE_GPTK_OVERLAY="${SWITCHYARD_DISABLE_GPTK_OVERLAY:-0}"
case "$DISABLE_GPTK_OVERLAY" in
  0) GPTK_PATH="${GPTK_PATH:-$(defaults read dev.switchyard.Switchyard gptkPath 2>/dev/null || true)}" ;;
  1) GPTK_PATH="" ;;
  *)
    echo "SWITCHYARD_DISABLE_GPTK_OVERLAY must be 0 or 1." >&2
    exit 2
    ;;
esac
TLS_DLOPEN_NAME="@loader_path/../../switchyard-tls/lib/libgnutls.dylib"
MAX_JOBS=13
JOBS="${JOBS:-$MAX_JOBS}"
if [ "$JOBS" -gt "$MAX_JOBS" ]; then
  JOBS="$MAX_JOBS"
elif [ "$JOBS" -lt 1 ]; then
  JOBS=1
fi
RECONFIGURE="${RECONFIGURE:-0}"
INSTALL_STAGE_ROOT=""
SWAP_HELPER_DIR=""

cleanup_temporary_paths() {
  if [ -n "$INSTALL_STAGE_ROOT" ]; then
    rm -rf "$INSTALL_STAGE_ROOT"
  fi
  if [ -n "$SWAP_HELPER_DIR" ]; then
    rm -rf "$SWAP_HELPER_DIR"
  fi
}
trap cleanup_temporary_paths EXIT

macos_no_huge_supported() {
  local temporary_dir

  temporary_dir="$(mktemp -d)"
  cat >"$temporary_dir/conftest.c" <<'EOF'
int main(void) { return 0; }
EOF
  if arch -x86_64 clang -arch x86_64 "$temporary_dir/conftest.c" -o "$temporary_dir/conftest" -Wl,-no_huge >/dev/null 2>&1; then
    rm -rf "$temporary_dir"
    return 0
  fi
  rm -rf "$temporary_dir"
  return 1
}

MACOS_NO_HUGE_SUPPORTED=0
if macos_no_huge_supported; then
  MACOS_NO_HUGE_SUPPORTED=1
fi

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1" >&2
    return 1
  fi
}

require_command brew

BREW_PREFIX="$(brew --prefix)"
export PATH="${BREW_PREFIX}/opt/bison/bin:${BREW_PREFIX}/opt/flex/bin:${BREW_PREFIX}/opt/pkgconf/bin:${BREW_PREFIX}/bin:${PATH}"

require_command bison
require_command flex
require_command pkg-config
require_command i686-w64-mingw32-gcc
require_command x86_64-w64-mingw32-gcc
require_command shasum
require_command perl
require_command curl
require_command tar
require_command unzip
require_command zstd
require_command install_name_tool

sha256_file() {
  shasum -a 256 "$1" | awk '{print $1}'
}

short_sha256_stream() {
  shasum -a 256 | awk '{print substr($1, 1, 12)}'
}

content_tree_digest() {
  local root="$1"

  (
    cd "$root"
    find . \( -type f -o -type l \) ! -path './.switchyard-content-sha256' -print |
      LC_ALL=C sort |
      while IFS= read -r path; do
        if [ -L "$path" ]; then
          printf 'link %s %s\n' "$path" "$(readlink "$path")"
        else
          printf 'file %s %s\n' "$path" "$(sha256_file "$path")"
        fi
      done
  ) | short_sha256_stream
}

write_content_tree_digest() {
  local root="$1"
  content_tree_digest "$root" > "$root/.switchyard-content-sha256"
}

content_tree_is_verified() {
  local root="$1"
  local marker="$root/.switchyard-content-sha256"
  local expected

  [ -f "$marker" ] || return 1
  expected="$(tr -d '[:space:]' < "$marker")"
  [ -n "$expected" ] && [ "$(content_tree_digest "$root")" = "$expected" ]
}

SWITCHYARD_MANAGED_RUNTIME_ROOT="${HOME}/.switchyard/runtimes"
source "$ROOT_DIR/switchyard/lib/directory_safety.sh"

source_tree_digest() {
  {
    git -C "$WINE_DIR" diff --binary HEAD --
    git -C "$WINE_DIR" ls-files --others --exclude-standard | LC_ALL=C sort | while IFS= read -r path; do
      printf 'untracked %s\n' "$path"
      sha256_file "$WINE_DIR/$path"
    done
  } | short_sha256_stream
}

json_string() {
  printf '%s' "$1" | perl -0pe 'BEGIN { print "\"" } s/\\/\\\\/g; s/"/\\"/g; s/\n/\\n/g; s/\r/\\r/g; s/\t/\\t/g; s/[\x00-\x08\x0b\x0c\x0e-\x1f]/sprintf("\\u%04x", ord($&))/ge; END { print "\"" }'
}

download_wine_mono() {
  local cached_file="$WINE_MONO_CACHE_DIR/$WINE_MONO_FILE"
  local actual_hash
  local temporary_file

  mkdir -p "$WINE_MONO_CACHE_DIR"

  if [ -f "$cached_file" ]; then
    actual_hash="$(sha256_file "$cached_file")"
    if [ "$actual_hash" = "$WINE_MONO_SHA256" ]; then
      printf '%s\n' "$cached_file"
      return 0
    fi
    echo "cached Wine Mono has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  temporary_file="${cached_file}.tmp.$$"
  echo "downloading Wine Mono $WINE_MONO_VERSION from WineHQ" >&2
  curl -fL --retry 3 --connect-timeout 20 -o "$temporary_file" "$WINE_MONO_URL"
  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$WINE_MONO_SHA256" ]; then
    rm -f "$temporary_file"
    echo "Wine Mono sha256 mismatch: expected $WINE_MONO_SHA256, got $actual_hash" >&2
    exit 1
  fi
  mv "$temporary_file" "$cached_file"
  printf '%s\n' "$cached_file"
}

download_font_asset() {
  local name="$1"
  local expected_hash="$2"
  local url="$3"
  local cached_file="$FONT_ASSET_DOWNLOAD_CACHE_DIR/$name"
  local temporary_file="$cached_file.tmp.$$"
  local actual_hash

  mkdir -p "$FONT_ASSET_DOWNLOAD_CACHE_DIR"
  if [ -f "$cached_file" ]; then
    actual_hash="$(sha256_file "$cached_file")"
    if [ "$actual_hash" = "$expected_hash" ]; then
      printf '%s\n' "$cached_file"
      return 0
    fi
    echo "cached font asset $name has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  rm -f "$temporary_file"
  curl -fL --retry 3 --connect-timeout 20 -o "$temporary_file" "$url"
  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$expected_hash" ]; then
    rm -f "$temporary_file"
    echo "font asset $name sha256 mismatch: expected $expected_hash, got $actual_hash" >&2
    exit 1
  fi
  mv "$temporary_file" "$cached_file"
  printf '%s\n' "$cached_file"
}

stage_font_assets() {
  local temporary_prefix
  local kind
  local name
  local expected_hash
  local url
  local extra
  local asset
  local font_count=0
  local license_count=0

  if [ ! -f "$FONT_ASSET_MANIFEST" ]; then
    echo "missing font asset manifest: $FONT_ASSET_MANIFEST" >&2
    exit 1
  fi

  if content_tree_is_verified "$FONT_ASSET_PREFIX" &&
     cmp -s "$FONT_ASSET_MANIFEST" \
       "$FONT_ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv" &&
     [ -f "$FONT_ASSET_PREFIX/share/wine/fonts/$FONT_ALIAS_FILE" ] &&
     [ "$(sha256_file "$FONT_ASSET_PREFIX/share/wine/fonts/$FONT_ALIAS_FILE")" = "$FONT_ALIAS_SHA256" ]; then
    printf '%s\n' "$FONT_ASSET_PREFIX"
    return 0
  fi

  temporary_prefix="${FONT_ASSET_PREFIX}.tmp.$$"
  rm -rf "$temporary_prefix"
  mkdir -p "$temporary_prefix/share/wine/fonts" \
    "$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets"

  while IFS=$'\t' read -r kind name expected_hash url extra; do
    case "$kind" in
      ''|'#'*) continue ;;
      font|license) ;;
      *)
        echo "unsupported font asset type '$kind' in $FONT_ASSET_MANIFEST" >&2
        exit 1
        ;;
    esac
    if [ -n "${extra:-}" ] || [ -z "$name" ] || [ -z "$expected_hash" ] || [ -z "$url" ]; then
      echo "invalid font asset manifest row for $name" >&2
      exit 1
    fi
    case "$name" in
      */*|.*|'')
        echo "unsafe font asset file name: $name" >&2
        exit 1
        ;;
    esac
    if ! printf '%s\n' "$expected_hash" | grep -Eq '^[0-9a-f]{64}$'; then
      echo "invalid sha256 for font asset $name" >&2
      exit 1
    fi
    case "$url" in
      https://*) ;;
      *)
        echo "font asset URL must use HTTPS: $url" >&2
        exit 1
        ;;
    esac

    asset="$(download_font_asset "$name" "$expected_hash" "$url")"
    if [ "$kind" = "font" ]; then
      case "$name" in
        *.ttf|*.ttc|*.otf) ;;
        *)
          echo "unsupported font file extension: $name" >&2
          exit 1
          ;;
      esac
      install -m 0644 "$asset" "$temporary_prefix/share/wine/fonts/$name"
      font_count=$((font_count + 1))
    else
      install -m 0644 "$asset" \
        "$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/$name"
      license_count=$((license_count + 1))
    fi
  done < "$FONT_ASSET_MANIFEST"

  if [ "$font_count" -lt 1 ] || [ "$license_count" -lt 1 ]; then
    echo "font asset manifest did not provide both fonts and license notices" >&2
    exit 1
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to generate the bundled font compatibility alias" >&2
    exit 1
  fi
  if [ ! -f "$FONT_ALIAS_SCRIPT" ]; then
    echo "missing font compatibility alias generator: $FONT_ALIAS_SCRIPT" >&2
    exit 1
  fi
  python3 "$FONT_ALIAS_SCRIPT" \
    "$temporary_prefix/share/wine/fonts/$FONT_ALIAS_SOURCE" \
    "$temporary_prefix/share/wine/fonts/$FONT_ALIAS_FILE" \
    --face-index "$FONT_ALIAS_FACE_INDEX" \
    --family "$FONT_ALIAS_FAMILY" \
    --postscript "$FONT_ALIAS_POSTSCRIPT"
  if [ "$(sha256_file "$temporary_prefix/share/wine/fonts/$FONT_ALIAS_FILE")" != "$FONT_ALIAS_SHA256" ]; then
    echo "generated font compatibility alias has an unexpected sha256" >&2
    exit 1
  fi
  font_count=$((font_count + 1))

  install -m 0644 "$FONT_ASSET_MANIFEST" \
    "$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv"
  cat >"$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/README.txt" <<EOF
Switchyard Wine redistributable font set $FONT_ASSET_SET_VERSION

The Noto font binaries are installed under share/wine/fonts so every Wine
prefix has deterministic multilingual text fallback. ArialUnicodeMS.otf is an
OFL-licensed compatibility alias generated from the Korean face of
NotoSansCJK-Regular.ttc; it is not a Microsoft font. Preserve the included
license notices and manifest when distributing the runtime.
EOF

  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$FONT_ASSET_PREFIX" cache
  printf '%s\n' "$FONT_ASSET_PREFIX"
}

download_homebrew_oci_blob() {
  local repository="$1"
  local digest="$2"
  local file_name="$3"
  local cache_dir="${4:-$VULKAN_CACHE_DIR}"
  local expected_sha="${digest#sha256:}"
  local cached_file="$cache_dir/$file_name"
  local actual_hash
  local temporary_file
  local token

  mkdir -p "$cache_dir"

  if [ -f "$cached_file" ]; then
    actual_hash="$(sha256_file "$cached_file")"
    if [ "$actual_hash" = "$expected_sha" ]; then
      printf '%s\n' "$cached_file"
      return 0
    fi
    echo "cached Vulkan bottle $file_name has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  token="$(
    curl -fsSL "https://ghcr.io/token?service=ghcr.io&scope=repository:${repository}:pull" |
      perl -0ne 'print $1 if /"token":"([^"]+)"/'
  )"
  if [ -z "$token" ]; then
    echo "could not acquire GHCR pull token for $repository" >&2
    exit 1
  fi

  temporary_file="${cached_file}.tmp.$$"
  echo "downloading $file_name from GHCR Homebrew bottle registry" >&2
  curl -fL --retry 3 --connect-timeout 20 \
    -H "Authorization: Bearer $token" \
    -o "$temporary_file" \
    "https://ghcr.io/v2/${repository}/blobs/${digest}"

  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$expected_sha" ]; then
    rm -f "$temporary_file"
    echo "$file_name sha256 mismatch: expected $expected_sha, got $actual_hash" >&2
    exit 1
  fi

  mv "$temporary_file" "$cached_file"
  printf '%s\n' "$cached_file"
}

stage_vulkan_deps() {
  local loader_archive
  local headers_archive
  local moltenvk_archive
  local staging_dir
  local temporary_prefix
  local lib_dir="$VULKAN_DEPS_PREFIX/lib"
  local icd_file="$VULKAN_DEPS_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json"

  if content_tree_is_verified "$VULKAN_DEPS_PREFIX" &&
     [ -f "$lib_dir/libvulkan.1.4.350.dylib" ] &&
     [ -f "$lib_dir/libMoltenVK.dylib" ] &&
     [ -f "$icd_file" ] &&
     file "$lib_dir/libvulkan.1.4.350.dylib" | grep -q "x86_64" &&
     file "$lib_dir/libMoltenVK.dylib" | grep -q "x86_64"; then
    printf '%s\n' "$VULKAN_DEPS_PREFIX"
    return 0
  fi

  loader_archive="$(download_homebrew_oci_blob "$VULKAN_LOADER_REPOSITORY" "sha256:$VULKAN_LOADER_LAYER_SHA256" "$VULKAN_LOADER_BOTTLE")"
  headers_archive="$(download_homebrew_oci_blob "$VULKAN_HEADERS_REPOSITORY" "sha256:$VULKAN_HEADERS_LAYER_SHA256" "$VULKAN_HEADERS_BOTTLE")"
  moltenvk_archive="$(download_homebrew_oci_blob "$MOLTENVK_REPOSITORY" "sha256:$MOLTENVK_LAYER_SHA256" "$MOLTENVK_BOTTLE")"

  staging_dir="$(mktemp -d)"
  temporary_prefix="${VULKAN_DEPS_PREFIX}.tmp.$$"
  rm -rf "$temporary_prefix"
  mkdir -p "$temporary_prefix/lib" "$temporary_prefix/include" "$temporary_prefix/etc/vulkan/icd.d" \
    "$temporary_prefix/lib/pkgconfig" "$temporary_prefix/share/doc/vulkan-loader" \
    "$temporary_prefix/share/doc/vulkan-headers" "$temporary_prefix/share/doc/molten-vk"

  tar -xzf "$loader_archive" -C "$staging_dir" \
    "vulkan-loader/${VULKAN_LOADER_VERSION}/lib/libvulkan.1.4.350.dylib" \
    "vulkan-loader/${VULKAN_LOADER_VERSION}/LICENSE.txt" \
    "vulkan-loader/${VULKAN_LOADER_VERSION}/README.md"
  tar -xzf "$headers_archive" -C "$staging_dir" "vulkan-headers/${VULKAN_HEADERS_VERSION}/include"
  tar -xzf "$moltenvk_archive" -C "$staging_dir" \
    "molten-vk/${MOLTENVK_VERSION}/lib/libMoltenVK.dylib" \
    "molten-vk/${MOLTENVK_VERSION}/etc/vulkan/icd.d/MoltenVK_icd.json" \
    "molten-vk/${MOLTENVK_VERSION}/LICENSE" \
    "molten-vk/${MOLTENVK_VERSION}/README.md"

  ditto "$staging_dir/vulkan-headers/${VULKAN_HEADERS_VERSION}/include" "$temporary_prefix/include"
  install -m 0644 "$staging_dir/vulkan-loader/${VULKAN_LOADER_VERSION}/lib/libvulkan.1.4.350.dylib" \
    "$temporary_prefix/lib/libvulkan.1.4.350.dylib"
  ln -sf "libvulkan.1.4.350.dylib" "$temporary_prefix/lib/libvulkan.1.dylib"
  ln -sf "libvulkan.1.dylib" "$temporary_prefix/lib/libvulkan.dylib"
  install -m 0644 "$staging_dir/molten-vk/${MOLTENVK_VERSION}/lib/libMoltenVK.dylib" \
    "$temporary_prefix/lib/libMoltenVK.dylib"
  install -m 0644 "$staging_dir/molten-vk/${MOLTENVK_VERSION}/etc/vulkan/icd.d/MoltenVK_icd.json" \
    "$temporary_prefix/etc/vulkan/icd.d/MoltenVK_icd.json"
  install -m 0644 "$staging_dir/vulkan-loader/${VULKAN_LOADER_VERSION}/LICENSE.txt" \
    "$temporary_prefix/share/doc/vulkan-loader/LICENSE.txt"
  install -m 0644 "$staging_dir/vulkan-loader/${VULKAN_LOADER_VERSION}/README.md" \
    "$temporary_prefix/share/doc/vulkan-loader/README.md"
  install -m 0644 "$staging_dir/molten-vk/${MOLTENVK_VERSION}/LICENSE" \
    "$temporary_prefix/share/doc/molten-vk/LICENSE"
  install -m 0644 "$staging_dir/molten-vk/${MOLTENVK_VERSION}/README.md" \
    "$temporary_prefix/share/doc/molten-vk/README.md"

  install_name_tool -id "@rpath/libvulkan.1.dylib" "$temporary_prefix/lib/libvulkan.1.4.350.dylib"
  install_name_tool -id "@rpath/libMoltenVK.dylib" "$temporary_prefix/lib/libMoltenVK.dylib"

  {
    printf 'prefix=%s\n' "$VULKAN_DEPS_PREFIX"
    printf 'exec_prefix=${prefix}\n'
    printf 'libdir=${exec_prefix}/lib\n'
    printf 'includedir=${prefix}/include\n\n'
    printf 'Name: Vulkan-Loader\n'
    printf 'Description: Switchyard staged x86_64 Vulkan loader\n'
    printf 'Version: 1.4.350\n'
    printf 'Libs: -L${libdir} -lvulkan\n'
    printf 'Cflags: -I${includedir}\n'
  } >"$temporary_prefix/lib/pkgconfig/vulkan.pc"

  {
    printf '{\n'
    printf '  "architecture": "x86_64",\n'
    printf '  "license": "Apache-2.0",\n'
    printf '  "vulkanLoader": {\n'
    printf '    "version": %s,\n' "$(json_string "$VULKAN_LOADER_VERSION")"
    printf '    "repository": %s,\n' "$(json_string "$VULKAN_LOADER_REPOSITORY")"
    printf '    "manifestDigest": %s,\n' "$(json_string "$VULKAN_LOADER_MANIFEST_DIGEST")"
    printf '    "layerSha256": %s\n' "$(json_string "$VULKAN_LOADER_LAYER_SHA256")"
    printf '  },\n'
    printf '  "vulkanHeaders": {\n'
    printf '    "version": %s,\n' "$(json_string "$VULKAN_HEADERS_VERSION")"
    printf '    "repository": %s,\n' "$(json_string "$VULKAN_HEADERS_REPOSITORY")"
    printf '    "manifestDigest": %s,\n' "$(json_string "$VULKAN_HEADERS_MANIFEST_DIGEST")"
    printf '    "layerSha256": %s\n' "$(json_string "$VULKAN_HEADERS_LAYER_SHA256")"
    printf '  },\n'
    printf '  "moltenVK": {\n'
    printf '    "version": %s,\n' "$(json_string "$MOLTENVK_VERSION")"
    printf '    "repository": %s,\n' "$(json_string "$MOLTENVK_REPOSITORY")"
    printf '    "manifestDigest": %s,\n' "$(json_string "$MOLTENVK_MANIFEST_DIGEST")"
    printf '    "layerSha256": %s\n' "$(json_string "$MOLTENVK_LAYER_SHA256")"
    printf '  },\n'
    printf '  "icdFile": "etc/vulkan/icd.d/MoltenVK_icd.json"\n'
    printf '}\n'
  } >"$temporary_prefix/switchyard-vulkan-runtime.json"

  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$VULKAN_DEPS_PREFIX" cache
  rm -rf "$staging_dir"

  printf '%s\n' "$VULKAN_DEPS_PREFIX"
}

stage_font_deps() {
  local lib_dir="$FONT_DEPS_PREFIX/lib"
  local temporary_prefix
  local staging_dir
  local index
  local name
  local version
  local repository
  local sha
  local bottle
  local archive
  local formula_root
  local library
  local dependency
  local dependency_name
  local test_source
  local test_binary

  if content_tree_is_verified "$FONT_DEPS_PREFIX" &&
     [ -f "$lib_dir/libfreetype.6.dylib" ] &&
     [ -f "$lib_dir/libfontconfig.1.dylib" ] &&
     [ -f "$FONT_DEPS_PREFIX/lib/pkgconfig/freetype2.pc" ] &&
     [ -f "$FONT_DEPS_PREFIX/lib/pkgconfig/fontconfig.pc" ] &&
     [ -f "$FONT_DEPS_PREFIX/etc/fonts/fonts.conf" ] &&
     file "$lib_dir/libfreetype.6.dylib" | grep -q "x86_64" &&
     file "$lib_dir/libfontconfig.1.dylib" | grep -q "x86_64"; then
    printf '%s\n' "$FONT_DEPS_PREFIX"
    return 0
  fi

  staging_dir="$(mktemp -d)"
  temporary_prefix="${FONT_DEPS_PREFIX}.tmp.$$"
  rm -rf "$temporary_prefix"
  mkdir -p "$temporary_prefix/lib" "$temporary_prefix/include" "$temporary_prefix/lib/pkgconfig" \
    "$temporary_prefix/etc" "$temporary_prefix/share/doc/switchyard-font-deps"

  for index in "${!FONT_DEPS_NAMES[@]}"; do
    name="${FONT_DEPS_NAMES[$index]}"
    version="${FONT_DEPS_VERSIONS[$index]}"
    repository="${FONT_DEPS_REPOSITORIES[$index]}"
    sha="${FONT_DEPS_LAYER_SHA256[$index]}"
    bottle="${name}--${version}.sonoma.bottle.tar.gz"
    archive="$(download_homebrew_oci_blob "$repository" "sha256:$sha" "$bottle" "$FONT_DEPS_CACHE_DIR")"

    tar -xzf "$archive" -C "$staging_dir"
    formula_root="$staging_dir/$name/$version"
    if [ ! -d "$formula_root" ]; then
      echo "Homebrew bottle $bottle did not contain expected root $name/$version." >&2
      exit 1
    fi

    [ ! -d "$formula_root/include" ] || ditto "$formula_root/include" "$temporary_prefix/include"
    [ ! -d "$formula_root/lib" ] || ditto "$formula_root/lib" "$temporary_prefix/lib"
    [ ! -d "$formula_root/etc" ] || ditto "$formula_root/etc" "$temporary_prefix/etc"
    [ ! -d "$formula_root/.bottle/etc" ] || ditto "$formula_root/.bottle/etc" "$temporary_prefix/etc"
    [ ! -d "$formula_root/.bottle/var" ] || ditto "$formula_root/.bottle/var" "$temporary_prefix/var"
    [ ! -d "$formula_root/share/fontconfig" ] || ditto "$formula_root/share/fontconfig" "$temporary_prefix/share/fontconfig"

    mkdir -p "$temporary_prefix/share/doc/$name"
    for notice in "$formula_root"/LICENSE* "$formula_root"/COPYING* "$formula_root"/README*; do
      [ -e "$notice" ] || continue
      install -m 0644 "$notice" "$temporary_prefix/share/doc/$name/$(basename "$notice")"
    done
  done

  chmod -R u+rwX "$temporary_prefix"
  mkdir -p "$temporary_prefix/lib/pkgconfig"

  for library in "$temporary_prefix"/lib/*.dylib; do
    [ -e "$library" ] || continue
    if [ -L "$library" ]; then
      continue
    fi
    install_name_tool -id "$FONT_DEPS_PREFIX/lib/$(basename "$library")" "$library"
  done

  for library in "$temporary_prefix"/lib/*.dylib; do
    [ -e "$library" ] || continue
    if [ -L "$library" ]; then
      continue
    fi
    while IFS= read -r dependency; do
      case "$dependency" in
        "$temporary_prefix"/lib/*|"$FONT_DEPS_PREFIX"/lib/*|/usr/local/*|/opt/homebrew/*|@@HOMEBREW_PREFIX@@/*|@@HOMEBREW_CELLAR@@/*|@loader_path/*.dylib|@rpath/*.dylib|*.dylib)
          dependency_name="${dependency##*/}"
          if [ -e "$temporary_prefix/lib/$dependency_name" ]; then
            install_name_tool -change "$dependency" "$FONT_DEPS_PREFIX/lib/$dependency_name" "$library"
          fi
          ;;
      esac
    done < <(otool -L "$library" | awk 'NR > 1 { print $1 }')
  done

  cat >"$temporary_prefix/lib/pkgconfig/freetype2.pc" <<EOF
prefix=$FONT_DEPS_PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: FreeType 2
Description: Switchyard staged x86_64 FreeType
Version: ${FONT_DEPS_VERSIONS[0]}
Libs: -L\${libdir} -lfreetype
Cflags: -I\${includedir}/freetype2
EOF

  cat >"$temporary_prefix/lib/pkgconfig/fontconfig.pc" <<EOF
prefix=$FONT_DEPS_PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Fontconfig
Description: Switchyard staged x86_64 fontconfig
Version: ${FONT_DEPS_VERSIONS[1]}
Requires: freetype2
Libs: -L\${libdir} -lfontconfig
Cflags: -I\${includedir}
EOF

  if [ -f "$temporary_prefix/etc/fonts/fonts.conf" ]; then
    mkdir -p "$temporary_prefix/var/cache/fontconfig"
    perl -0pi -e "s#/usr/local/var/cache/fontconfig#$FONT_DEPS_PREFIX/var/cache/fontconfig#g" \
      "$temporary_prefix/etc/fonts/fonts.conf"
  fi

  cat >"$temporary_prefix/share/doc/switchyard-font-deps/README.txt" <<EOF
This directory contains user-local x86_64 FreeType/fontconfig runtime
dependencies staged from Homebrew sonoma bottles for the Switchyard Wine build.

The staged files let Wine's GDI font backend run under Rosetta without linking
against the host arm64 Homebrew prefix. Do not commit these binaries to the
Switchyard repository. Preserve upstream license notices when distributing a
runtime built with these libraries.
EOF

  test_source="$temporary_prefix/freetype-link-test.c"
  test_binary="$temporary_prefix/freetype-link-test"
  cat >"$test_source" <<'EOF'
#include <ft2build.h>
#include FT_FREETYPE_H
int main(void) { FT_Library lib; return FT_Init_FreeType(&lib); }
EOF
  arch -x86_64 clang -arch x86_64 \
    -I"$temporary_prefix/include/freetype2" \
    -L"$temporary_prefix/lib" \
    -Wl,-rpath,"$temporary_prefix/lib" \
    "$test_source" -lfreetype -o "$test_binary"
  env DYLD_LIBRARY_PATH="$temporary_prefix/lib" "$test_binary"
  rm -f "$test_source" "$test_binary"

  test_source="$temporary_prefix/fontconfig-link-test.c"
  test_binary="$temporary_prefix/fontconfig-link-test"
  cat >"$test_source" <<'EOF'
#include <fontconfig/fontconfig.h>
int main(void) { return FcInit() ? 0 : 1; }
EOF
  arch -x86_64 clang -arch x86_64 \
    -I"$temporary_prefix/include" \
    -I"$temporary_prefix/include/freetype2" \
    -L"$temporary_prefix/lib" \
    -Wl,-rpath,"$temporary_prefix/lib" \
    "$test_source" -lfontconfig -lfreetype -lintl -lunistring -lpng16 \
    -o "$test_binary"
  env DYLD_LIBRARY_PATH="$temporary_prefix/lib" \
    FONTCONFIG_FILE="$temporary_prefix/etc/fonts/fonts.conf" \
    FONTCONFIG_PATH="$temporary_prefix/etc/fonts" \
    "$test_binary"
  rm -f "$test_source" "$test_binary"
  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$FONT_DEPS_PREFIX" cache
  rm -rf "$staging_dir"
  printf '%s\n' "$FONT_DEPS_PREFIX"
}

relocate_font_deps_for_runtime() {
  local runtime_font_root="$1"
  local source_prefix="$2"
  local runtime_lib="$runtime_font_root/lib"
  local library
  local dependency
  local dependency_name

  for library in "$runtime_lib"/*.dylib; do
    [ -e "$library" ] || continue
    if [ -L "$library" ]; then
      continue
    fi
    install_name_tool -id "@loader_path/$(basename "$library")" "$library"
  done

  for library in "$runtime_lib"/*.dylib; do
    [ -e "$library" ] || continue
    if [ -L "$library" ]; then
      continue
    fi
    while IFS= read -r dependency; do
      case "$dependency" in
        "$source_prefix"/lib/*|/usr/local/*|/opt/homebrew/*|@@HOMEBREW_PREFIX@@/*|@@HOMEBREW_CELLAR@@/*|@loader_path/*.dylib|@rpath/*.dylib|*.dylib)
          dependency_name="${dependency##*/}"
          if [ -e "$runtime_lib/$dependency_name" ]; then
            install_name_tool -change "$dependency" "@loader_path/$dependency_name" "$library"
          fi
          ;;
      esac
    done < <(otool -L "$library" | awk 'NR > 1 { print $1 }')
  done
}

download_tls_package() {
  local package_name="$1"
  local filename="$2"
  local expected_hash="$3"
  local cached_file="$TLS_PACKAGE_CACHE_DIR/$filename"
  local temporary_file
  local actual_hash

  mkdir -p "$TLS_PACKAGE_CACHE_DIR"
  if [ -f "$cached_file" ]; then
    actual_hash="$(sha256_file "$cached_file")"
    if [ "$actual_hash" = "$expected_hash" ]; then
      printf '%s\n' "$cached_file"
      return 0
    fi
    echo "cached $package_name package has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  temporary_file="${cached_file}.tmp.$$"
  rm -f "$temporary_file"
  curl -fL --retry 3 --retry-delay 1 \
    -o "$temporary_file" "$TLS_PACKAGE_BASE_URL/$filename"
  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$expected_hash" ]; then
    rm -f "$temporary_file"
    echo "$package_name package has unexpected sha256 $actual_hash; expected $expected_hash." >&2
    exit 1
  fi
  mv "$temporary_file" "$cached_file"
  printf '%s\n' "$cached_file"
}

extract_tls_package() {
  local archive="$1"
  local destination="$2"
  local container
  local payload

  mkdir -p "$destination"
  case "$archive" in
    *.conda)
      container="$(mktemp -d)"
      unzip -q "$archive" -d "$container"
      for payload in "$container"/pkg-*.tar.zst "$container"/info-*.tar.zst; do
        [ -f "$payload" ] || continue
        zstd -dc "$payload" | tar -xf - -C "$destination"
      done
      rm -rf "$container"
      ;;
    *.tar.bz2)
      tar -xjf "$archive" -C "$destination"
      ;;
    *)
      echo "unsupported TLS package archive: $archive" >&2
      exit 1
      ;;
  esac
}

stage_tls_deps() {
  local manifest_digest
  local tls_deps_prefix
  local temporary_prefix
  local package_name
  local package_version
  local package_build
  local package_filename
  local package_hash
  local extra
  local package_archive
  local package_root
  local package_notice_root
  local package_pool
  local closure_file
  local previous_file
  local source_library
  local library
  local dependency
  local dependency_name
  local test_source
  local test_binary

  if [ ! -f "$TLS_PACKAGE_MANIFEST" ]; then
    echo "missing pinned TLS package manifest at $TLS_PACKAGE_MANIFEST" >&2
    exit 1
  fi
  manifest_digest="$({ /bin/cat "$TLS_PACKAGE_MANIFEST"; /usr/bin/printf '%s\n' "$TLS_RUNTIME_LAYOUT_VERSION"; } | short_sha256_stream)"
  tls_deps_prefix="$TLS_DEPS_CACHE_DIR/x86_64-conda-gnutls-${manifest_digest}"
  if content_tree_is_verified "$tls_deps_prefix" &&
     [ -f "$tls_deps_prefix/lib/libgnutls.30.dylib" ] &&
     [ -f "$tls_deps_prefix/lib/pkgconfig/gnutls.pc" ] &&
     file "$tls_deps_prefix/lib/libgnutls.30.dylib" | grep -q "x86_64"; then
    printf '%s\n' "$tls_deps_prefix"
    return 0
  fi

  temporary_prefix="${tls_deps_prefix}.tmp.$$"
  package_pool="${temporary_prefix}.pool"
  rm -rf "$temporary_prefix"
  rm -rf "$package_pool"
  mkdir -p "$temporary_prefix/lib" "$temporary_prefix/include" "$temporary_prefix/lib/pkgconfig" \
    "$temporary_prefix/share/doc/switchyard-tls/packages"
  mkdir -p "$package_pool/lib"

  while IFS=$'\t' read -r package_name package_version package_build package_filename package_hash extra; do
    case "$package_name" in
      ''|'#'*) continue ;;
    esac
    if [ -n "${extra:-}" ] || [ -z "$package_hash" ]; then
      echo "invalid TLS package manifest row for $package_name" >&2
      exit 1
    fi

    package_archive="$(download_tls_package "$package_name" "$package_filename" "$package_hash")"
    package_root="$(mktemp -d)"
    extract_tls_package "$package_archive" "$package_root"

    if [ -d "$package_root/lib" ]; then
      ditto "$package_root/lib" "$package_pool/lib"
    fi
    if [ -d "$package_root/include" ]; then
      ditto "$package_root/include" "$temporary_prefix/include"
    fi
    if [ -d "$package_root/etc" ]; then
      mkdir -p "$temporary_prefix/etc"
      ditto "$package_root/etc" "$temporary_prefix/etc"
    fi

    package_notice_root="$temporary_prefix/share/doc/switchyard-tls/packages/$package_name"
    mkdir -p "$package_notice_root"
    if [ ! -d "$package_root/info/licenses" ] ||
       [ -z "$(find "$package_root/info/licenses" -type f -print -quit)" ]; then
      echo "$package_name package does not contain redistributable license notices." >&2
      exit 1
    fi
    ditto "$package_root/info/licenses" "$package_notice_root/licenses"
    for dependency_record in about.json index.json; do
      if [ -f "$package_root/info/$dependency_record" ]; then
        install -m 0644 "$package_root/info/$dependency_record" "$package_notice_root/$dependency_record"
      fi
    done
    printf '%s\t%s\t%s\t%s\t%s\n' \
      "$package_name" "$package_version" "$package_build" "$package_filename" "$package_hash" \
      >> "$temporary_prefix/share/doc/switchyard-tls/manifest.tsv"
    rm -rf "$package_root"
  done < "$TLS_PACKAGE_MANIFEST"

  if [ ! -f "$package_pool/lib/libgnutls.30.dylib" ] ||
     [ ! -f "$temporary_prefix/include/gnutls/gnutls.h" ]; then
    echo "pinned TLS packages did not provide GnuTLS libraries and headers." >&2
    exit 1
  fi

  closure_file="$(mktemp)"
  previous_file="$(mktemp)"
  printf '%s\n' 'libgnutls.30.dylib' > "$closure_file"
  while true; do
    cp "$closure_file" "$previous_file"
    while IFS= read -r dependency_name; do
      source_library="$package_pool/lib/$dependency_name"
      if [ ! -e "$source_library" ]; then
        echo "TLS package closure is missing $dependency_name." >&2
        exit 1
      fi
      while IFS= read -r dependency; do
        case "$dependency" in
          @rpath/*.dylib|@loader_path/*.dylib)
            dependency_name="${dependency##*/}"
            if [ ! -e "$package_pool/lib/$dependency_name" ]; then
              echo "$(basename "$source_library") depends on missing TLS package library $dependency_name." >&2
              exit 1
            fi
            printf '%s\n' "$dependency_name" >> "$closure_file"
            ;;
          /System/*|/usr/lib/*)
            ;;
          *)
            echo "$(basename "$source_library") has non-relocatable package dependency $dependency." >&2
            exit 1
            ;;
        esac
      done < <(otool -L "$source_library" | awk 'NR > 1 { print $1 }')
    done < "$previous_file"
    LC_ALL=C sort -u -o "$closure_file" "$closure_file"
    if cmp -s "$closure_file" "$previous_file"; then
      break
    fi
  done

  while IFS= read -r dependency_name; do
    source_library="$package_pool/lib/$dependency_name"
    runtime_name="$dependency_name"
    if [ "$dependency_name" = "libiconv.2.dylib" ]; then
      # macOS supplies a different libiconv ABI under this leaf name. Keep the
      # conda implementation distinct so libunistring can still bind to the
      # system _iconv symbols while libidn2 and libintl bind to _libiconv.
      runtime_name="libswitchyard-iconv.2.dylib"
    fi
    install -m 0644 "$(realpath "$source_library")" "$temporary_prefix/lib/$runtime_name"
  done < "$closure_file"
  ln -sf libgnutls.30.dylib "$temporary_prefix/lib/libgnutls.dylib"
  rm -f "$closure_file" "$previous_file"
  rm -rf "$package_pool"

  for library in "$temporary_prefix"/lib/*.dylib; do
    [ -e "$library" ] || continue
    [ -L "$library" ] && continue
    if ! file "$library" | grep -q "x86_64"; then
      echo "TLS runtime library is not x86_64: $library" >&2
      exit 1
    fi
    install_name_tool -id "@loader_path/$(basename "$library")" "$library"
    while IFS= read -r dependency; do
      case "$dependency" in
        @rpath/*.dylib|@loader_path/*.dylib)
          dependency_name="${dependency##*/}"
          runtime_dependency_name="$dependency_name"
          if [ "$dependency_name" = "libiconv.2.dylib" ]; then
            runtime_dependency_name="libswitchyard-iconv.2.dylib"
          fi
          if [ ! -e "$temporary_prefix/lib/$runtime_dependency_name" ]; then
            echo "$(basename "$library") depends on missing TLS runtime library $dependency_name." >&2
            exit 1
          fi
          install_name_tool -change "$dependency" "@loader_path/$runtime_dependency_name" "$library"
          ;;
        /System/*|/usr/lib/*)
          ;;
        *)
          echo "$(basename "$library") has non-relocatable TLS dependency $dependency." >&2
          exit 1
          ;;
      esac
    done < <(otool -L "$library" | awk 'NR > 1 { print $1 }')
  done

  cat >"$temporary_prefix/lib/pkgconfig/gnutls.pc" <<EOF
prefix=$tls_deps_prefix
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: GnuTLS
Description: Pinned redistributable x86_64 GnuTLS runtime for Switchyard Wine
Version: 3.8.13
Libs: -L\${libdir} -lgnutls
Cflags: -I\${includedir}
EOF

  cat >"$temporary_prefix/share/doc/switchyard-tls/README.txt" <<EOF
This directory contains the pinned x86_64 macOS GnuTLS dependency closure used
by Switchyard Wine for schannel support. Every package is downloaded from the
conda-forge osx-64 channel and verified against switchyard/tls-deps.tsv before
staging. The package manifest, original package metadata, and license notices
are preserved alongside the libraries for redistribution and source tracing.
EOF
  install -m 0644 "$TLS_PACKAGE_MANIFEST" \
    "$temporary_prefix/share/doc/switchyard-tls/packages.tsv"

  test_source="$temporary_prefix/gnutls-link-test.c"
  test_binary="$temporary_prefix/gnutls-link-test"
  cat >"$test_source" <<'EOF'
#include <gnutls/gnutls.h>
int main(void) { return gnutls_check_version("3.0") ? 0 : 1; }
EOF
  arch -x86_64 clang -arch x86_64 \
    -I"$temporary_prefix/include" \
    -L"$temporary_prefix/lib" \
    -Wl,-rpath,"$temporary_prefix/lib" \
    "$test_source" -lgnutls -o "$test_binary"
  env DYLD_LIBRARY_PATH="$temporary_prefix/lib" "$test_binary"
  rm -f "$test_source" "$test_binary"

  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$tls_deps_prefix" cache
  printf '%s\n' "$tls_deps_prefix"
}

if ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
  echo "Rosetta is required to build and run the x86_64 Switchyard Wine runtime." >&2
  echo "Install it with: softwareupdate --install-rosetta --agree-to-license" >&2
  exit 1
fi

if ! git -C "$WINE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "missing switchyard-wine checkout at $WINE_DIR" >&2
  exit 1
fi

if [ ! -f "$UPSTREAM_BASE_FILE" ]; then
  echo "missing upstream base metadata at $UPSTREAM_BASE_FILE" >&2
  exit 1
fi

upstream_wine_revision="$(tr -d '[:space:]' < "$UPSTREAM_BASE_FILE")"
if ! git -C "$WINE_DIR" cat-file -e "${upstream_wine_revision}^{commit}" 2>/dev/null; then
  echo "upstream Wine base $upstream_wine_revision is unavailable; fetch at least 256 commits of history" >&2
  exit 1
fi
if ! git -C "$WINE_DIR" merge-base --is-ancestor "$upstream_wine_revision" HEAD; then
  echo "current source is not descended from the recorded upstream Wine base $upstream_wine_revision" >&2
  exit 1
fi

wine_revision="$(git -C "$WINE_DIR" rev-parse HEAD)"
source_status="$(git -C "$WINE_DIR" status --porcelain --untracked-files=normal)"
if [ -n "$source_status" ]; then
  source_dirty=true
  source_digest="$(source_tree_digest)"
  source_identity="${wine_revision:0:12}-dirty-${source_digest}"
else
  source_dirty=false
  source_digest="${wine_revision:0:12}"
  source_identity="${wine_revision:0:12}"
fi
patchset_id="switchyard-wine-${wine_revision:0:12}"
if [ -z "$USER_SET_WINE_BUILD_DIR" ]; then
  WINE_BUILD_DIR="${HOME}/Library/Caches/Switchyard/Wine/build-wow64-x86_64-${source_identity}"
fi
if [ "$MODE" = "--source-info" ]; then
  echo "sourceRepository=$SOURCE_REPOSITORY"
  echo "sourceRevision=$wine_revision"
  echo "upstreamWineRevision=$upstream_wine_revision"
  echo "sourceDirty=$source_dirty"
  echo "sourceTreeDigest=$source_digest"
  echo "patchsetID=$patchset_id"
  echo "gptkOverlayDisabled=$DISABLE_GPTK_OVERLAY"
  exit 0
fi

gptk_redist_digest="no-gptk"
if [ -n "$GPTK_PATH" ] && [ -d "$GPTK_PATH/redist/lib" ]; then
  gptk_redist_digest="$(
    (
      cd "$GPTK_PATH/redist/lib"
      find . \( -type f -o -type l \) -print | LC_ALL=C sort | while IFS= read -r relative_file; do
        if [ -L "$relative_file" ]; then
          printf 'link %s %s\n' "$relative_file" "$(readlink "$relative_file")"
        else
          printf 'file %s %s\n' "$relative_file" "$(sha256_file "$relative_file")"
        fi
      done
    ) | short_sha256_stream
  )"
fi

if [ "$MODE" = "--verify-tls" ]; then
  tls_deps_prefix="$(stage_tls_deps)"
  echo "verified pinned x86_64 TLS runtime at $tls_deps_prefix"
  echo "tlsRuntimeDigest=$(content_tree_digest "$tls_deps_prefix")"
  exit 0
fi

wine_mono_path="$(download_wine_mono)"
wine_mono_digest="$(sha256_file "$wine_mono_path")"
vulkan_deps_prefix="$(stage_vulkan_deps)"
vulkan_deps_digest="$(content_tree_digest "$vulkan_deps_prefix")"
font_deps_prefix="$(stage_font_deps)"
font_deps_digest="$(content_tree_digest "$font_deps_prefix")"
font_assets_prefix="$(stage_font_assets)"
font_assets_digest="$(content_tree_digest "$font_assets_prefix")"
font_asset_count="$(awk -F '\t' '$1 == "font" { count++ } END { print count + 1 }' "$FONT_ASSET_MANIFEST")"
tls_deps_prefix="$(stage_tls_deps)"
if [ -n "$tls_deps_prefix" ]; then
  tls_deps_digest="$(content_tree_digest "$tls_deps_prefix")"
  tls_dlopen_digest="$(printf '%s' "$TLS_DLOPEN_NAME" | short_sha256_stream)"
else
  tls_deps_digest="none"
  tls_dlopen_digest="none"
fi

runtime_id="switchyard-local-wow64-x86_64-${source_identity}-${gptk_redist_digest}-${wine_mono_digest:0:12}-${vulkan_deps_digest}-${font_deps_digest}-${font_assets_digest}-${tls_deps_digest}-${tls_dlopen_digest}"
if [ -z "$USER_SET_WINE_INSTALL_PREFIX" ]; then
  WINE_INSTALL_PREFIX="${HOME}/.switchyard/runtimes/$runtime_id"
fi
case "$WINE_INSTALL_PREFIX" in
  /*) ;;
  *)
    echo "WINE_INSTALL_PREFIX must be an absolute path." >&2
    exit 1
    ;;
esac
FINAL_WINE_INSTALL_PREFIX="$WINE_INSTALL_PREFIX"

runtime_is_complete_at() {
  local prefix="$1"
  local manifest="$prefix/switchyard-runtime.json"
  local manifest_id
  local manifest_install_prefix
  local manifest_executable
  local expected_wine_sha
  local expected_i386_ntdll_sha
  local expected_x86_64_ntdll_sha
  local manifest_font_assets_digest
  local kind
  local name
  local expected_hash
  local url
  local extra
  local asset_path

  [ -f "$manifest" ] || return 1
  manifest_id="$(/usr/bin/plutil -extract id raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_id" = "$runtime_id" ] || return 1
  manifest_install_prefix="$(/usr/bin/plutil -extract installPrefix raw -o - "$manifest" 2>/dev/null || true)"
  manifest_executable="$(/usr/bin/plutil -extract executable raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_install_prefix" = "$FINAL_WINE_INSTALL_PREFIX" ] || return 1
  [ "$manifest_executable" = "$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine" ] || return 1
  [ -x "$prefix/bin/switchyard-wine" ] || return 1
  [ "$(readlink "$prefix/bin/switchyard-wine" 2>/dev/null || true)" = "wine" ] || return 1
  [ -x "$prefix/lib/wine/x86_64-unix/wine" ] || return 1
  [ -f "$prefix/lib/wine/i386-windows/ntdll.dll" ] || return 1
  [ -f "$prefix/lib/wine/x86_64-windows/ntdll.dll" ] || return 1
  expected_wine_sha="$(/usr/bin/plutil -extract integrity.wineUnixSha256 raw -o - "$manifest" 2>/dev/null || true)"
  expected_i386_ntdll_sha="$(/usr/bin/plutil -extract integrity.i386NtdllSha256 raw -o - "$manifest" 2>/dev/null || true)"
  expected_x86_64_ntdll_sha="$(/usr/bin/plutil -extract integrity.x86_64NtdllSha256 raw -o - "$manifest" 2>/dev/null || true)"
  [ -n "$expected_wine_sha" ] &&
    [ "$(sha256_file "$prefix/lib/wine/x86_64-unix/wine")" = "$expected_wine_sha" ] || return 1
  [ -n "$expected_i386_ntdll_sha" ] &&
    [ "$(sha256_file "$prefix/lib/wine/i386-windows/ntdll.dll")" = "$expected_i386_ntdll_sha" ] || return 1
  [ -n "$expected_x86_64_ntdll_sha" ] &&
    [ "$(sha256_file "$prefix/lib/wine/x86_64-windows/ntdll.dll")" = "$expected_x86_64_ntdll_sha" ] || return 1
  manifest_font_assets_digest="$(/usr/bin/plutil -extract fontAssets.digest raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_font_assets_digest" = "$font_assets_digest" ] || return 1
  cmp -s "$FONT_ASSET_MANIFEST" \
    "$prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv" || return 1
  while IFS=$'\t' read -r kind name expected_hash url extra; do
    case "$kind" in
      ''|'#'*) continue ;;
      font) asset_path="$prefix/share/wine/fonts/$name" ;;
      license) asset_path="$prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/$name" ;;
      *) return 1 ;;
    esac
    [ -z "${extra:-}" ] || return 1
    [ -f "$asset_path" ] || return 1
    [ "$(sha256_file "$asset_path")" = "$expected_hash" ] || return 1
  done < "$FONT_ASSET_MANIFEST"
  [ -f "$prefix/share/wine/fonts/$FONT_ALIAS_FILE" ] || return 1
  [ "$(sha256_file "$prefix/share/wine/fonts/$FONT_ALIAS_FILE")" = "$FONT_ALIAS_SHA256" ] || return 1
}

runtime_is_complete() {
  runtime_is_complete_at "$FINAL_WINE_INSTALL_PREFIX"
}

if [ "$MODE" = "--ensure" ] && runtime_is_complete; then
  wine_executable="$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine"
  defaults write dev.switchyard.Switchyard winePath "$wine_executable"
  echo "Switchyard Wine runtime is current ($runtime_id): $wine_executable"
  exit 0
fi

if [ "$MODE" = "--ensure" ]; then
  echo "No complete Switchyard Wine runtime matches $runtime_id; building it now."
fi

mkdir -p "$WINE_BUILD_DIR" "$(dirname "$FINAL_WINE_INSTALL_PREFIX")"

configured=0
if [ -f "$WINE_BUILD_DIR/config.status" ]; then
  configured=1
fi

if [ "$configured" -eq 1 ] && ! grep -F "prefix = $WINE_INSTALL_PREFIX" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
  RECONFIGURE=1
fi

if [ "$configured" -eq 1 ]; then
  if grep -F "#define HAVE_WINE_PRELOADER 1" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    if [ "$MACOS_NO_HUGE_SUPPORTED" -eq 1 ]; then
      echo "existing Wine build uses the macOS preloader despite -no_huge support; reconfiguring"
      RECONFIGURE=1
    fi
  elif ! grep -F -- "-no_huge" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
    echo "existing Wine build has neither the macOS preloader nor the -no_huge loader reservation; reconfiguring"
    RECONFIGURE=1
  fi
  if ! grep -F "#define SONAME_LIBFREETYPE \"$FONT_DLOPEN_FREETYPE\"" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    echo "existing Wine build is missing the expected FreeType dlopen name; reconfiguring"
    RECONFIGURE=1
  fi
  if ! grep -F "#define SONAME_LIBFONTCONFIG \"$FONT_DLOPEN_FONTCONFIG\"" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    echo "existing Wine build is missing the expected fontconfig dlopen name; reconfiguring"
    RECONFIGURE=1
  fi
  if [ -n "$tls_deps_prefix" ] &&
     ! grep -F "#define SONAME_LIBGNUTLS \"$TLS_DLOPEN_NAME\"" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    echo "existing Wine build is missing the expected GnuTLS dlopen name; reconfiguring"
    RECONFIGURE=1
  fi
  for dependency_prefix in "$font_deps_prefix" "$vulkan_deps_prefix"; do
    if ! grep -F "$dependency_prefix" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
      echo "existing Wine build does not reference dependency prefix $dependency_prefix; reconfiguring"
      RECONFIGURE=1
    fi
  done
  if [ -n "$tls_deps_prefix" ] &&
     ! grep -F "$tls_deps_prefix" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
    echo "existing Wine build does not reference TLS prefix $tls_deps_prefix; reconfiguring"
    RECONFIGURE=1
  fi
fi

if [ "$RECONFIGURE" = "1" ] && [ "$configured" -eq 1 ]; then
  configured=0
fi

if [ "$configured" -eq 0 ]; then
  echo "configuring Switchyard Wine in $WINE_BUILD_DIR"
  configure_cppflags="-I${font_deps_prefix}/include -I${font_deps_prefix}/include/freetype2 -I${vulkan_deps_prefix}/include"
  configure_ldflags="-L${font_deps_prefix}/lib -Wl,-rpath,${font_deps_prefix}/lib -L${vulkan_deps_prefix}/lib -Wl,-rpath,${vulkan_deps_prefix}/lib"
  configure_pkg_config_path="${font_deps_prefix}/lib/pkgconfig:${vulkan_deps_prefix}/lib/pkgconfig"
  if [ -n "$tls_deps_prefix" ]; then
    configure_cppflags="-I${tls_deps_prefix}/include ${configure_cppflags}"
    configure_ldflags="-L${tls_deps_prefix}/lib -Wl,-rpath,${tls_deps_prefix}/lib ${configure_ldflags}"
    configure_pkg_config_path="${tls_deps_prefix}/lib/pkgconfig:${configure_pkg_config_path}"
  fi
  (
    cd "$WINE_BUILD_DIR"
    export ac_cv_lib_soname_freetype="$FONT_DLOPEN_FREETYPE"
    export ac_cv_lib_soname_fontconfig="$FONT_DLOPEN_FONTCONFIG"
    if [ -n "$tls_deps_prefix" ]; then
      export ac_cv_lib_soname_gnutls="$TLS_DLOPEN_NAME"
    fi
    CPPFLAGS="${configure_cppflags} ${CPPFLAGS:-}" \
    LDFLAGS="${configure_ldflags} ${LDFLAGS:-}" \
    PKG_CONFIG_PATH="${configure_pkg_config_path}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
    arch -x86_64 "$WINE_DIR/configure" \
      --build=x86_64-apple-darwin \
      --host=x86_64-apple-darwin \
      CC="clang -arch x86_64" \
      CXX="clang++ -arch x86_64" \
      OBJC="clang -arch x86_64" \
      --enable-archs=i386,x86_64 \
      --disable-tests \
      --without-alsa \
      --without-capi \
      --without-cups \
      --without-dbus \
      --without-ffmpeg \
      --with-fontconfig \
      --with-freetype \
      --without-gphoto \
      --without-gssapi \
      --without-gstreamer \
      --without-inotify \
      --without-krb5 \
      --without-netapi \
      --without-opencl \
      --without-oss \
      --without-pcap \
      --without-pcsclite \
      --without-pulse \
      --without-sane \
      --without-udev \
      --without-usb \
      --without-v4l2 \
      --without-wayland \
      --without-xcomposite \
      --without-xcursor \
      --without-xfixes \
      --without-xinerama \
      --without-xinput \
      --without-xinput2 \
      --without-xrandr \
      --without-xrender \
      --without-xshape \
      --without-xshm \
      --without-xxf86vm \
      --with-vulkan \
      --prefix="$WINE_INSTALL_PREFIX"
  )
  if ! grep -F "#define SONAME_LIBFREETYPE \"$FONT_DLOPEN_FREETYPE\"" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    echo "Wine configure did not record the expected FreeType dylib name." >&2
    echo "Refusing to build a Wine runtime that would fail to load the staged font backend." >&2
    exit 1
  fi
  if ! grep -F "#define SONAME_LIBFONTCONFIG \"$FONT_DLOPEN_FONTCONFIG\"" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    echo "Wine configure did not record the expected fontconfig dylib name." >&2
    echo "Refusing to build a Wine runtime that would fail to load staged fontconfig." >&2
    exit 1
  fi
  if [ -n "$tls_deps_prefix" ] &&
     ! grep -F "#define SONAME_LIBGNUTLS \"$TLS_DLOPEN_NAME\"" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
    echo "Wine configure did not record the expected GnuTLS dylib name." >&2
    echo "Refusing to build a Wine runtime that would fail to dlopen GnuTLS for schannel." >&2
    exit 1
  fi
fi

echo "building Switchyard Wine with $JOBS jobs"
make -C "$WINE_BUILD_DIR" -j"$JOBS"

runtime_parent="$(dirname "$FINAL_WINE_INSTALL_PREFIX")"
runtime_name="$(basename "$FINAL_WINE_INSTALL_PREFIX")"
INSTALL_STAGE_ROOT="$(mktemp -d "$runtime_parent/.${runtime_name}.staging.XXXXXX")"
staged_wine_install_prefix="${INSTALL_STAGE_ROOT}${FINAL_WINE_INSTALL_PREFIX}"
mkdir -p "$(dirname "$staged_wine_install_prefix")"

echo "installing Switchyard Wine to a temporary runtime staging directory"
make -C "$WINE_BUILD_DIR" install DESTDIR="$INSTALL_STAGE_ROOT"
if [ ! -d "$staged_wine_install_prefix" ]; then
  echo "Wine install did not create the expected staged prefix at $staged_wine_install_prefix." >&2
  exit 1
fi
WINE_INSTALL_PREFIX="$staged_wine_install_prefix"

if ! grep -F "#define HAVE_WINE_PRELOADER 1" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
  echo "removing stale Wine preloader files from no-preloader runtime"
  find "$WINE_INSTALL_PREFIX" \( -type f -o -type l \) \( -name wine-preloader -o -name wine64-preloader \) -exec rm -f {} +
fi

for pe_arch in "${PE_ARCHS[@]}"; do
  pe_ntdll="$WINE_INSTALL_PREFIX/lib/wine/$pe_arch-windows/ntdll.dll"
  if [ ! -f "$pe_ntdll" ]; then
    echo "Wine install is missing $pe_arch PE ntdll.dll at $pe_ntdll." >&2
    echo "The official Steam bootstrap is 32-bit, so Switchyard requires a WoW64 PE runtime." >&2
    exit 1
  fi
done

wine_graphics_fallback_root="$WINE_INSTALL_PREFIX/lib/switchyard-wine-graphics"
echo "preserving Wine graphics modules under $wine_graphics_fallback_root"
rm -rf "$wine_graphics_fallback_root"
for pe_arch in "${PE_ARCHS[@]}"; do
  mkdir -p "$wine_graphics_fallback_root/$pe_arch-windows"
  mkdir -p "$wine_graphics_fallback_root/$pe_arch-unix"
  for module in "${WINE_GRAPHICS_FALLBACK_MODULES[@]}"; do
    pe_module="$WINE_INSTALL_PREFIX/lib/wine/$pe_arch-windows/$module.dll"
    unix_module="$WINE_INSTALL_PREFIX/lib/wine/$pe_arch-unix/$module.so"
    if [ -f "$pe_module" ]; then
      install -m 0644 "$pe_module" "$wine_graphics_fallback_root/$pe_arch-windows/$module.dll"
    fi
    if [ -e "$unix_module" ]; then
      cp -p "$unix_module" "$wine_graphics_fallback_root/$pe_arch-unix/$module.so"
    fi
  done
done

if [ -n "$GPTK_PATH" ] && [ -d "$GPTK_PATH/redist/lib" ]; then
  echo "overlaying GPTK redist libraries from $GPTK_PATH"
  if [ -e "$GPTK_PATH/redist/lib/switchyard-wine-graphics" ]; then
    echo "GPTK redist unexpectedly contains lib/switchyard-wine-graphics." >&2
    echo "Refusing to overlay user-provided GPTK files onto the Wine-only graphics fallback directory." >&2
    exit 1
  fi
  ditto "$GPTK_PATH/redist/lib" "$WINE_INSTALL_PREFIX/lib"
elif [ "$DISABLE_GPTK_OVERLAY" = "1" ]; then
  echo "GPTK overlay explicitly disabled for this runtime build"
else
  echo "GPTK redist was not found; leaving Wine runtime without GPTK overlay." >&2
fi

echo "installing Wine Mono addon $WINE_MONO_FILE"
mkdir -p "$WINE_INSTALL_PREFIX/share/wine/mono"
install -m 0644 "$wine_mono_path" "$WINE_INSTALL_PREFIX/share/wine/mono/$WINE_MONO_FILE"

echo "installing x86_64 Vulkan loader and MoltenVK runtime"
rm -rf "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan"
mkdir -p "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan"
ditto "$vulkan_deps_prefix" "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan"

echo "installing x86_64 FreeType and fontconfig runtime libraries"
runtime_font_root="$WINE_INSTALL_PREFIX/lib/switchyard-fonts"
rm -rf "$runtime_font_root"
mkdir -p "$runtime_font_root"
ditto "$font_deps_prefix" "$runtime_font_root"
relocate_font_deps_for_runtime "$runtime_font_root" "$font_deps_prefix"
if [ -f "$runtime_font_root/etc/fonts/fonts.conf" ]; then
  perl -0pi -e "s#\\n\\s*<cachedir>\\Q${font_deps_prefix}/var/cache/fontconfig\\E</cachedir>##g" \
    "$runtime_font_root/etc/fonts/fonts.conf"
fi

echo "installing $font_asset_count redistributable font files"
mkdir -p "$WINE_INSTALL_PREFIX/share/wine/fonts" \
  "$runtime_font_root/share/doc/switchyard-font-assets"
ditto "$font_assets_prefix/share/wine/fonts" "$WINE_INSTALL_PREFIX/share/wine/fonts"
ditto "$font_assets_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets" \
  "$runtime_font_root/share/doc/switchyard-font-assets"

if [ -n "$tls_deps_prefix" ]; then
  echo "installing x86_64 GnuTLS runtime libraries"
  rm -rf "$WINE_INSTALL_PREFIX/lib/switchyard-tls"
  mkdir -p "$WINE_INSTALL_PREFIX/lib/switchyard-tls"
  ditto "$tls_deps_prefix" "$WINE_INSTALL_PREFIX/lib/switchyard-tls"
fi

echo "installing Wine license, source, and replacement notices"
wine_notice_root="$WINE_INSTALL_PREFIX/share/doc/switchyard-wine"
mkdir -p "$wine_notice_root"
install -m 0644 "$ROOT_DIR/LICENSE" "$wine_notice_root/LICENSE"
install -m 0644 "$ROOT_DIR/COPYING.LIB" "$wine_notice_root/COPYING.LIB"
install -m 0644 "$ROOT_DIR/AUTHORS" "$wine_notice_root/AUTHORS"
install -m 0644 "$ROOT_DIR/docs/building.md" "$wine_notice_root/BUILDING.md"
install -m 0644 "$ROOT_DIR/docs/provenance.md" "$wine_notice_root/PROVENANCE.md"
cat >"$wine_notice_root/CORRESPONDING-SOURCE.txt" <<EOF
Switchyard Wine runtime corresponding source

Repository: $SOURCE_REPOSITORY
Revision: $wine_revision
Source URL: $SOURCE_REPOSITORY/tree/$wine_revision

The repository at the revision above contains the complete Switchyard Wine
source and the scripts used to build this replaceable runtime. Wine and the
Switchyard modifications are licensed under LGPL-2.1-or-later; see LICENSE and
COPYING.LIB in this directory. Runtime dependency package identities, hashes,
metadata, and license notices are retained under the component documentation
directories. The Switchyard app launches this external runtime and permits the
user to select a rebuilt or replacement Wine executable independently.
EOF

rm -rf "$WINE_INSTALL_PREFIX/bin/.switchyard-real"

for wine_binary_name in wine wine64; do
  wine_binary_path="$WINE_INSTALL_PREFIX/bin/$wine_binary_name"
  wine_real_path="$WINE_INSTALL_PREFIX/bin/$wine_binary_name.switchyard-real"
  rm -f "$wine_real_path"
  if [ -f "$wine_binary_path" ] && [ ! -L "$wine_binary_path" ]; then
    mv "$wine_binary_path" "$wine_real_path"
  fi
done

if [ ! -x "$WINE_INSTALL_PREFIX/bin/wine.switchyard-real" ] &&
   [ ! -x "$WINE_INSTALL_PREFIX/bin/wine64.switchyard-real" ]; then
  echo "Wine executable was not installed under $WINE_INSTALL_PREFIX/bin." >&2
  exit 1
fi

cat >"$WINE_INSTALL_PREFIX/bin/wine" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

bin_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runtime_dir="$(cd "$bin_dir/.." && pwd)"
vulkan_root="$runtime_dir/lib/switchyard-vulkan"
vulkan_lib="$vulkan_root/lib"
vulkan_icd="$vulkan_root/etc/vulkan/icd.d/MoltenVK_icd.json"
tls_root="$runtime_dir/lib/switchyard-tls"
tls_lib="$tls_root/lib"
font_root="$runtime_dir/lib/switchyard-fonts"
font_lib="$font_root/lib"

prepend_path() {
  local value="$1"
  local current="${2:-}"
  if [ -z "$current" ]; then
    printf '%s\n' "$value"
  else
    printf '%s:%s\n' "$value" "$current"
  fi
}

export DYLD_LIBRARY_PATH="$(prepend_path "$vulkan_lib" "${DYLD_LIBRARY_PATH:-}")"
export DYLD_FALLBACK_LIBRARY_PATH="$(prepend_path "$vulkan_lib" "${DYLD_FALLBACK_LIBRARY_PATH:-}")"
if [ -d "$font_lib" ]; then
  export DYLD_LIBRARY_PATH="$(prepend_path "$font_lib" "${DYLD_LIBRARY_PATH:-}")"
  export DYLD_FALLBACK_LIBRARY_PATH="$(prepend_path "$font_lib" "${DYLD_FALLBACK_LIBRARY_PATH:-}")"
fi
if [ -d "$tls_lib" ]; then
  export DYLD_LIBRARY_PATH="$(prepend_path "$tls_lib" "${DYLD_LIBRARY_PATH:-}")"
  export DYLD_FALLBACK_LIBRARY_PATH="$(prepend_path "$tls_lib" "${DYLD_FALLBACK_LIBRARY_PATH:-}")"
fi
if [ -f "$font_root/etc/fonts/fonts.conf" ]; then
  export FONTCONFIG_FILE="${FONTCONFIG_FILE:-$font_root/etc/fonts/fonts.conf}"
  export FONTCONFIG_PATH="$(prepend_path "$font_root/etc/fonts" "${FONTCONFIG_PATH:-}")"
fi
if [ -n "${VK_ICD_FILENAMES:-}" ]; then
  export SWITCHYARD_HOST_VK_ICD_FILENAMES="$VK_ICD_FILENAMES"
fi
export VK_ICD_FILENAMES="$vulkan_icd"

invoked_name="$(basename "$0")"
invoked_path="$bin_dir/$invoked_name"
if [ "$invoked_name" = "switchyard-wine" ]; then
  invoked_name="wine"
  invoked_path="$bin_dir/wine"
fi

real_executable="$runtime_dir/lib/wine/x86_64-unix/wine"
if [ "$invoked_name" = "wine64" ]; then
  real_executable="$runtime_dir/lib/wine/x86_64-unix/wine"
fi

if [ ! -x "$real_executable" ]; then
  if [ -x "$bin_dir/wine.switchyard-real" ]; then
    real_executable="$bin_dir/wine.switchyard-real"
  elif [ -x "$bin_dir/wine64.switchyard-real" ]; then
    real_executable="$bin_dir/wine64.switchyard-real"
  else
    echo "Switchyard Wine runtime is missing its preserved Wine executable under $bin_dir." >&2
    exit 127
  fi
fi

exec -a "$invoked_path" "$real_executable" "$@"
EOF
chmod 0755 "$WINE_INSTALL_PREFIX/bin/wine"
if [ -x "$WINE_INSTALL_PREFIX/bin/wine64.switchyard-real" ]; then
  cp "$WINE_INSTALL_PREFIX/bin/wine" "$WINE_INSTALL_PREFIX/bin/wine64"
  chmod 0755 "$WINE_INSTALL_PREFIX/bin/wine64"
fi

staged_wine_executable="$WINE_INSTALL_PREFIX/bin/switchyard-wine"
ln -sf wine "$staged_wine_executable"
wine_executable="$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine"
wine_unix_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/lib/wine/x86_64-unix/wine")"
i386_ntdll_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/lib/wine/i386-windows/ntdll.dll")"
x86_64_ntdll_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/lib/wine/x86_64-windows/ntdll.dll")"

{
  printf '{\n'
  printf '  "id": %s,\n' "$(json_string "$runtime_id")"
  printf '  "buildProfile": %s,\n' "$(json_string "$BUILD_PROFILE")"
  printf '  "peArchitectures": [\n'
  for index in "${!PE_ARCHS[@]}"; do
    if [ "$index" -lt "$((${#PE_ARCHS[@]} - 1))" ]; then
      printf '    %s,\n' "$(json_string "${PE_ARCHS[$index]}")"
    else
      printf '    %s\n' "$(json_string "${PE_ARCHS[$index]}")"
    fi
  done
  printf '  ],\n'
  printf '  "wineSource": %s,\n' "$(json_string "$SOURCE_REPOSITORY")"
  printf '  "wineRevision": %s,\n' "$(json_string "$wine_revision")"
  printf '  "upstreamWineRevision": %s,\n' "$(json_string "$upstream_wine_revision")"
  printf '  "sourceRepository": %s,\n' "$(json_string "$SOURCE_REPOSITORY")"
  printf '  "sourceRevision": %s,\n' "$(json_string "$wine_revision")"
  printf '  "sourceDirty": %s,\n' "$source_dirty"
  printf '  "sourceTreeDigest": %s,\n' "$(json_string "$source_digest")"
  printf '  "patchsetID": %s,\n' "$(json_string "$patchset_id")"
  printf '  "installPrefix": %s,\n' "$(json_string "$FINAL_WINE_INSTALL_PREFIX")"
  printf '  "executable": %s,\n' "$(json_string "$wine_executable")"
  printf '  "integrity": {\n'
  printf '    "wineUnixSha256": %s,\n' "$(json_string "$wine_unix_sha256")"
  printf '    "i386NtdllSha256": %s,\n' "$(json_string "$i386_ntdll_sha256")"
  printf '    "x86_64NtdllSha256": %s\n' "$(json_string "$x86_64_ntdll_sha256")"
  printf '  },\n'
  printf '  "gptkPath": %s,\n' "$(json_string "$GPTK_PATH")"
  printf '  "gptkRedistDigest": %s,\n' "$(json_string "$gptk_redist_digest")"
  printf '  "wineGraphicsFallback": {\n'
  printf '    "root": "lib/switchyard-wine-graphics",\n'
  printf '    "modules": [\n'
  for index in "${!WINE_GRAPHICS_FALLBACK_MODULES[@]}"; do
    if [ "$index" -lt "$((${#WINE_GRAPHICS_FALLBACK_MODULES[@]} - 1))" ]; then
      printf '      %s,\n' "$(json_string "${WINE_GRAPHICS_FALLBACK_MODULES[$index]}")"
    else
      printf '      %s\n' "$(json_string "${WINE_GRAPHICS_FALLBACK_MODULES[$index]}")"
    fi
  done
  printf '    ],\n'
  printf '    "purpose": "Wine-built graphics modules preserved before the user-provided GPTK overlay for Chromium/CEF GPU-helper fallback."\n'
  printf '  },\n'
  printf '  "wineMono": {\n'
  printf '    "version": %s,\n' "$(json_string "$WINE_MONO_VERSION")"
  printf '    "file": %s,\n' "$(json_string "share/wine/mono/$WINE_MONO_FILE")"
  printf '    "sha256": %s,\n' "$(json_string "$wine_mono_digest")"
  printf '    "source": %s\n' "$(json_string "$WINE_MONO_URL")"
  printf '  },\n'
  printf '  "fontRuntime": {\n'
  printf '    "root": "lib/switchyard-fonts",\n'
  printf '    "digest": %s,\n' "$(json_string "$font_deps_digest")"
  printf '    "architecture": "x86_64",\n'
  printf '    "freetypeDlopenName": %s,\n' "$(json_string "$FONT_DLOPEN_FREETYPE")"
  printf '    "fontconfigDlopenName": %s,\n' "$(json_string "$FONT_DLOPEN_FONTCONFIG")"
  printf '    "license": "FreeType License/GPL dual-license, MIT-style fontconfig, libpng License, LGPL/GPL gettext components; preserve upstream notices when distributing",\n'
  printf '    "sourceNote": "lib/switchyard-fonts/share/doc/switchyard-font-deps/README.txt",\n'
  printf '    "formulae": [\n'
  for index in "${!FONT_DEPS_NAMES[@]}"; do
    printf '      {\n'
    printf '        "name": %s,\n' "$(json_string "${FONT_DEPS_NAMES[$index]}")"
    printf '        "version": %s,\n' "$(json_string "${FONT_DEPS_VERSIONS[$index]}")"
    printf '        "repository": %s,\n' "$(json_string "${FONT_DEPS_REPOSITORIES[$index]}")"
    printf '        "layerSha256": %s\n' "$(json_string "${FONT_DEPS_LAYER_SHA256[$index]}")"
    if [ "$index" -lt "$((${#FONT_DEPS_NAMES[@]} - 1))" ]; then
      printf '      },\n'
    else
      printf '      }\n'
    fi
  done
  printf '    ]\n'
  printf '  },\n'
  printf '  "fontAssets": {\n'
  printf '    "root": "share/wine/fonts",\n'
  printf '    "documentation": "lib/switchyard-fonts/share/doc/switchyard-font-assets",\n'
  printf '    "setVersion": %s,\n' "$(json_string "$FONT_ASSET_SET_VERSION")"
  printf '    "digest": %s,\n' "$(json_string "$font_assets_digest")"
  printf '    "fontCount": %s,\n' "$font_asset_count"
  printf '    "license": "SIL Open Font License 1.1",\n'
  printf '    "compatibilityAlias": {"file": %s, "family": %s, "source": %s, "faceIndex": %s, "sha256": %s},\n' \
    "$(json_string "$FONT_ALIAS_FILE")" "$(json_string "$FONT_ALIAS_FAMILY")" \
    "$(json_string "$FONT_ALIAS_SOURCE")" "$FONT_ALIAS_FACE_INDEX" "$(json_string "$FONT_ALIAS_SHA256")"
  printf '    "manifest": "lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv"\n'
  printf '  },\n'
  if [ -n "$tls_deps_prefix" ]; then
    printf '  "tlsRuntime": {\n'
    printf '    "root": "lib/switchyard-tls",\n'
    printf '    "digest": %s,\n' "$(json_string "$tls_deps_digest")"
    printf '    "architecture": "x86_64",\n'
    printf '    "dlopenName": %s,\n' "$(json_string "$TLS_DLOPEN_NAME")"
    printf '    "license": "redistributable conda-forge GnuTLS dependency closure with package notices",\n'
    printf '    "packageManifest": "lib/switchyard-tls/share/doc/switchyard-tls/packages.tsv",\n'
    printf '    "sourceNote": "lib/switchyard-tls/share/doc/switchyard-tls/README.txt"\n'
    printf '  },\n'
  else
    printf '  "tlsRuntime": null,\n'
  fi
  printf '  "vulkanRuntime": {\n'
  printf '    "root": "lib/switchyard-vulkan",\n'
  printf '    "digest": %s,\n' "$(json_string "$vulkan_deps_digest")"
  printf '    "architecture": "x86_64",\n'
  printf '    "license": "Apache-2.0",\n'
  printf '    "icdFile": "lib/switchyard-vulkan/etc/vulkan/icd.d/MoltenVK_icd.json",\n'
  printf '    "vulkanLoader": {\n'
  printf '      "version": %s,\n' "$(json_string "$VULKAN_LOADER_VERSION")"
  printf '      "repository": %s,\n' "$(json_string "$VULKAN_LOADER_REPOSITORY")"
  printf '      "manifestDigest": %s,\n' "$(json_string "$VULKAN_LOADER_MANIFEST_DIGEST")"
  printf '      "layerSha256": %s\n' "$(json_string "$VULKAN_LOADER_LAYER_SHA256")"
  printf '    },\n'
  printf '    "vulkanHeaders": {\n'
  printf '      "version": %s,\n' "$(json_string "$VULKAN_HEADERS_VERSION")"
  printf '      "repository": %s,\n' "$(json_string "$VULKAN_HEADERS_REPOSITORY")"
  printf '      "manifestDigest": %s,\n' "$(json_string "$VULKAN_HEADERS_MANIFEST_DIGEST")"
  printf '      "layerSha256": %s\n' "$(json_string "$VULKAN_HEADERS_LAYER_SHA256")"
  printf '    },\n'
  printf '    "moltenVK": {\n'
  printf '      "version": %s,\n' "$(json_string "$MOLTENVK_VERSION")"
  printf '      "repository": %s,\n' "$(json_string "$MOLTENVK_REPOSITORY")"
  printf '      "manifestDigest": %s,\n' "$(json_string "$MOLTENVK_MANIFEST_DIGEST")"
  printf '      "layerSha256": %s\n' "$(json_string "$MOLTENVK_LAYER_SHA256")"
  printf '    }\n'
  printf '  }\n'
  printf '}\n'
} >"$WINE_INSTALL_PREFIX/switchyard-runtime.json"

if ! runtime_is_complete_at "$WINE_INSTALL_PREFIX"; then
  echo "Refusing to publish an incomplete or internally inconsistent Wine runtime." >&2
  exit 1
fi

atomic_replace_directory "$WINE_INSTALL_PREFIX" "$FINAL_WINE_INSTALL_PREFIX" runtime
WINE_INSTALL_PREFIX="$FINAL_WINE_INSTALL_PREFIX"
rm -rf "$INSTALL_STAGE_ROOT"
INSTALL_STAGE_ROOT=""

if [ "$MODE" = "--ensure" ]; then
  defaults write dev.switchyard.Switchyard winePath "$wine_executable"
  echo "configured Switchyard winePath=$wine_executable"
else
  echo "built Switchyard Wine runtime at $FINAL_WINE_INSTALL_PREFIX"
fi
