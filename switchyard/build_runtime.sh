#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_DIR="$ROOT_DIR"
SOURCE_REPOSITORY="${SWITCHYARD_WINE_SOURCE_REPOSITORY:-https://github.com/jungwuk-ryu/switchyard-wine}"
UPSTREAM_BASE_FILE="$ROOT_DIR/switchyard/upstream-base.txt"
source "$ROOT_DIR/switchyard/lib/runtime_profile.sh"

runtime_build_usage() {
  echo "usage: $0 [--runtime-profile PROFILE] [build|--ensure|--source-info|--verify-media|--verify-mesa|--verify-tls]" >&2
  exit 2
}

MODE="build"
mode_was_set=0
requested_runtime_profile="$SWITCHYARD_DEFAULT_RUNTIME_PROFILE"
runtime_profile_was_set=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --runtime-profile)
      [ "$runtime_profile_was_set" -eq 0 ] || {
        echo "--runtime-profile may be specified only once." >&2
        runtime_build_usage
      }
      [ "$#" -ge 2 ] && [[ "$2" != --* ]] || {
        echo "--runtime-profile requires a profile name." >&2
        runtime_build_usage
      }
      requested_runtime_profile="$2"
      runtime_profile_was_set=1
      shift 2
      ;;
    build|--ensure|--source-info|--verify-media|--verify-mesa|--verify-tls)
      [ "$mode_was_set" -eq 0 ] || {
        echo "A build mode may be specified only once." >&2
        runtime_build_usage
      }
      MODE="$1"
      mode_was_set=1
      shift
      ;;
    *) runtime_build_usage ;;
  esac
done

switchyard_load_runtime_profile "$requested_runtime_profile" || exit $?
switchyard_require_runtime_profile_enabled || exit $?

BUILD_PROFILE="$SWITCHYARD_RUNTIME_PROFILE_BUILD_PROFILE"
PE_ARCHS=("${SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}")
INSTALLED_PE_ARCHS=("${SWITCHYARD_RUNTIME_PROFILE_INSTALLED_PE_ARCHS[@]}")
WINE_UNIX_ARCH="$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH"
HOST_MACHO_ARCH="$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH"
WINE_BUILD_TRIPLET="$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET"
WINE_HOST_TRIPLET="$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET"
PROFILE_ARCH_COMMAND=("${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}")
HOST_DEPENDENCY_ARCH="$SWITCHYARD_RUNTIME_PROFILE_HOST_DEPENDENCY_ARCH"
GSTREAMER_MACHO_ARCHS=("${SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_MACHO_ARCHS[@]}")
NATIVE_CPU_PROVIDER_ENABLED=0
if [ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_UNICORN" = "true" ]; then
  NATIVE_CPU_PROVIDER_ENABLED=1
fi
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  for native_packaging_library in \
      "$ROOT_DIR/switchyard/lib/native_arm64_packaging.sh" \
      "$ROOT_DIR/switchyard/lib/native_cpu_provider.sh" \
      "$ROOT_DIR/switchyard/lib/dxmt_artifact.sh" \
      "$ROOT_DIR/switchyard/lib/macho_signing.sh"; do
    [ -f "$native_packaging_library" ] && [ ! -L "$native_packaging_library" ] || {
      echo "Native ARM64 packaging policy is missing or unsafe: $native_packaging_library" >&2
      exit 1
    }
    # shellcheck disable=SC1090 # Paths are fixed repository policy libraries.
    source "$native_packaging_library"
  done
fi
NATIVE_ARM64_HOST_PROBE=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  NATIVE_ARM64_HOST_PROBE="$ROOT_DIR/switchyard/native_arm64_host_probe.sh"
  [ -f "$NATIVE_ARM64_HOST_PROBE" ] && [ ! -L "$NATIVE_ARM64_HOST_PROBE" ] &&
    [ -x "$NATIVE_ARM64_HOST_PROBE" ] || {
    echo "Native ARM64 strict host probe is missing or unsafe: $NATIVE_ARM64_HOST_PROBE" >&2
    exit 1
  }
fi
WINE_GRAPHICS_FALLBACK_MODULES=("d3d10" "d3d11" "d3d12" "d3d12core" "dcomp" "dwmapi" "dxgi" "wined3d")
WINE_MONO_VERSION="11.2.0"
WINE_MONO_ARCH="x86"
WINE_MONO_SHA256="b4525679e7da30d4658ceb85739cbc55c771791054abbb4b3152fe96ded0b897"
WINE_MONO_FILE="wine-mono-${WINE_MONO_VERSION}-${WINE_MONO_ARCH}.msi"
WINE_MONO_URL="https://dl.winehq.org/wine/wine-mono/${WINE_MONO_VERSION}/${WINE_MONO_FILE}"
WINE_MONO_CACHE_DIR="${WINE_MONO_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Wine/addons/mono}"
VULKAN_LOADER_VERSION="1.4.350.1"
VULKAN_LOADER_REPOSITORY="homebrew/core/vulkan-loader"
VULKAN_HEADERS_VERSION="1.4.350.1"
VULKAN_HEADERS_REPOSITORY="homebrew/core/vulkan-headers"
MOLTENVK_VERSION="1.4.1"
MOLTENVK_REPOSITORY="homebrew/core/molten-vk"
case "$SWITCHYARD_RUNTIME_PROFILE" in
  stable-x86_64-rosetta)
    VULKAN_LOADER_LAYER_SHA256="03185dd14f4a4501875b38cac7b69f11a2dd6921df4deaf7436aed74d62186e0"
    VULKAN_LOADER_MANIFEST_DIGEST="sha256:ecfcd7a2cb9fd52f60b200e9feaa7448057435de86ed504bfa44ac22a7d38149"
    VULKAN_HEADERS_LAYER_SHA256="b482fc6a2e4831ae1b572370791cffb91f44ba08908885ee579d44fdfe1f43d0"
    VULKAN_HEADERS_MANIFEST_DIGEST="sha256:c7f375dee3dc83d989457e74db0636eef966d79deb57ed98dafe8b44e07bc56b"
    MOLTENVK_LAYER_SHA256="9bb2d88ee0ed7cd035f982a59a2e9c5878237c9f4df88117172ccdbc5127f6d9"
    MOLTENVK_MANIFEST_DIGEST="sha256:6facac52c2f0f948cf185cf97f5f941c4d2f55a75e5e19d7e259e807597afd94"
    ;;
  preview-native-arm64-fex)
    VULKAN_LOADER_LAYER_SHA256="29759c4cff4f88360ee973d63bd99ca70131b03cac1256e0642ebb9756c8950f"
    VULKAN_LOADER_MANIFEST_DIGEST="sha256:0ca71aa91842d6f8e621c9999d56f9f95c7dccf132d92805b108c0f918a24e1c"
    VULKAN_HEADERS_LAYER_SHA256="b94439ee3fade8cb511fe296e5d2b75c0ed2a5b9943feb4c94fcd5aac6c07a8b"
    VULKAN_HEADERS_MANIFEST_DIGEST="sha256:482d23c4186ded585cd30ebd518357000fb27d1f621e44a4630f90d6c4e3a540"
    MOLTENVK_LAYER_SHA256="c0b1bda916255edc08d5a884eec4826e2649a890283b03e6f62e4aa9984cc9b8"
    MOLTENVK_MANIFEST_DIGEST="sha256:a1fe928a3d7f92c9cca9795ce24c02a76b92c7bd86fe6c7dacd3e56738a1f11b"
    ;;
  *)
    echo "No closed Vulkan dependency policy exists for $SWITCHYARD_RUNTIME_PROFILE." >&2
    exit 2
    ;;
esac
VULKAN_LOADER_BOTTLE="vulkan-loader--${VULKAN_LOADER_VERSION}.${SWITCHYARD_RUNTIME_PROFILE_VULKAN_LOADER_BOTTLE_TAG}.bottle.tar.gz"
VULKAN_HEADERS_BOTTLE="vulkan-headers--${VULKAN_HEADERS_VERSION}.${SWITCHYARD_RUNTIME_PROFILE_VULKAN_HEADERS_BOTTLE_TAG}.bottle.tar.gz"
MOLTENVK_BOTTLE="molten-vk--${MOLTENVK_VERSION}.${SWITCHYARD_RUNTIME_PROFILE_MOLTENVK_BOTTLE_TAG}.bottle.tar.gz"
VULKAN_CACHE_DIR="${VULKAN_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Vulkan}"
VULKAN_DEPS_PREFIX="${VULKAN_DEPS_PREFIX:-${HOME}/.switchyard/deps/vulkan/${HOST_DEPENDENCY_ARCH}-loader-${VULKAN_LOADER_VERSION}-moltenvk-${MOLTENVK_VERSION}}"
MESA_WINDOWS_VERSION="26.1.1"
MESA_WINDOWS_LLVM_VERSION="22.1.6"
MESA_WINDOWS_ARCHIVE="mesa3d-${MESA_WINDOWS_VERSION}-release-msvc.7z"
MESA_WINDOWS_ARCHIVE_URL="https://github.com/pal1000/mesa-dist-win/releases/download/${MESA_WINDOWS_VERSION}/${MESA_WINDOWS_ARCHIVE}"
MESA_WINDOWS_ARCHIVE_SHA256="d5e90e9ae4d620313b61fbbf8e9a55761454e38b6501c39be6d93449c88780e1"
MESA_WINDOWS_X86_64_OPENGL_SHA256="d2645f47b4dee4f47dcdfc1b2021a70f471655d95a019cfd1fb48415810867ed"
MESA_WINDOWS_X86_64_GALLIUM_SHA256="27f16f9e98119ad529ed915d4f65c3a2e8d84b4f8cbdce2f13cda0637b73e05c"
MESA_WINDOWS_I386_OPENGL_SHA256="da8cd72a4576a3b45507724fec04a4cabcd6d325b542bf441033982383876c6c"
MESA_WINDOWS_I386_GALLIUM_SHA256="fafac63d8644c9ae80edb51770f8abe5953777b4001dcecb40255c7601c55bec"
MESA_WINDOWS_DISTRIBUTOR_REPOSITORY="https://github.com/pal1000/mesa-dist-win"
MESA_WINDOWS_DISTRIBUTOR_REVISION="1e2b696ce9e81e77e17ee6e4787587237ce9d2ed"
MESA_WINDOWS_DISTRIBUTOR_LICENSE_URL="https://raw.githubusercontent.com/pal1000/mesa-dist-win/${MESA_WINDOWS_VERSION}/LICENSE"
MESA_WINDOWS_DISTRIBUTOR_LICENSE_SHA256="9cd0121dc070f0ac65c1fc266bee74bdbbc22cea4210ae72ae0c1f076448d4cc"
MESA_SOURCE_REPOSITORY="https://gitlab.freedesktop.org/mesa/mesa"
MESA_SOURCE_REVISION="97341aa7d7c340c9a4dbec192795dadd733b5846"
MESA_SOURCE_LICENSE_URL="https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-${MESA_WINDOWS_VERSION}/docs/license.rst"
MESA_SOURCE_LICENSE_SHA256="0d1a0472ecc81830e75c20d59b0ea02841e3db21255e0ebad97ab682c54d6615"
MESA_LLVM_LICENSE_URL="https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-${MESA_WINDOWS_LLVM_VERSION}/LICENSE.TXT"
MESA_LLVM_LICENSE_SHA256="8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"
MESA_WINDOWS_CACHE_DIR="${MESA_WINDOWS_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Mesa/windows-${MESA_WINDOWS_VERSION}}"
MESA_WINDOWS_DEPS_PREFIX="${MESA_WINDOWS_DEPS_PREFIX:-${HOME}/.switchyard/deps/mesa/windows-wow64-${MESA_WINDOWS_VERSION}}"
FONT_DEPS_CACHE_DIR="${FONT_DEPS_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Fonts/deps}"
FONT_DEPS_PREFIX="${FONT_DEPS_PREFIX:-${HOME}/.switchyard/deps/fonts/${HOST_DEPENDENCY_ARCH}-freetype-2.14.3-fontconfig-2.18.1}"
FONT_DLOPEN_FREETYPE="@loader_path/../../switchyard-fonts/lib/libfreetype.6.dylib"
FONT_DLOPEN_FONTCONFIG="@loader_path/../../switchyard-fonts/lib/libfontconfig.1.dylib"
FONT_DEPS_NAMES=("freetype" "fontconfig" "libpng" "gettext" "libunistring")
FONT_DEPS_VERSIONS=("2.14.3" "2.18.1" "1.6.58" "1.0" "1.4.2")
FONT_DEPS_REPOSITORIES=("homebrew/core/freetype" "homebrew/core/fontconfig" "homebrew/core/libpng" "homebrew/core/gettext" "homebrew/core/libunistring")
case "$SWITCHYARD_RUNTIME_PROFILE" in
  stable-x86_64-rosetta)
    FONT_DEPS_LAYER_SHA256=(
      "c266877a4676016b189131c87355f3e9be0d5e0edbe3a464b5b6ef039945f199"
      "9550776a54e32d8340966173a5d30d337a9f9984030bbdf7233eed792ad5d69c"
      "c74a40635359b753e614fb0a69a32149179a27f79d3338d5c5b685f66e223967"
      "2cc112cce103be3beb13cc8ba67f521d4e972c4082fd69868d34920d63120c09"
      "fbb3a7908a19f306823dbd51b417705c73f710a9a1fb1e34ba7aa67a3c966094"
    )
    ;;
  preview-native-arm64-fex)
    FONT_DEPS_LAYER_SHA256=(
      "4aeceab2c37d3685dd0de24b737f07c33a1098eaf757eb24d8d8bbe6ed68d02d"
      "f4854307ce84898d35564e9a7027cd200973736c74bc9b783a8b34cc1fffd821"
      "fd6cbd5d7a231b83e359fd96231bb3dd668124ab5c2009697dee906ace98fadd"
      "f9ea4eed738746ea4150a4f83e8dd11ca21ca3de5bb113995c25eec409bb5749"
      "dc4d4b4406a2c7032dd838ae362ecaeba114d8ac9d9daaa18f760d1d71ba3577"
    )
    ;;
esac
FONT_ASSET_SET_VERSION="noto-monthly-release-2026.07.01-cjk-2.004-emoji-static-3.002-aliases-1"
FONT_ASSET_MANIFEST="$ROOT_DIR/switchyard/font-assets.tsv"
FONT_ASSET_DOWNLOAD_CACHE_DIR="${FONT_ASSET_DOWNLOAD_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Fonts/assets/noto-monthly-release-2026.07.01}"
FONT_ASSET_PREFIX="${FONT_ASSET_PREFIX:-${HOME}/.switchyard/deps/fonts/assets-${FONT_ASSET_SET_VERSION}}"
FONT_ALIAS_SCRIPT="$ROOT_DIR/switchyard/make_font_alias.py"
FONT_EMOJI_STATIC_SCRIPT="$ROOT_DIR/switchyard/make_static_font.py"
FONTCONFIG_ASSET_FRAGMENT="$ROOT_DIR/switchyard/fontconfig/50-switchyard-font-assets.conf"
PE_MODULE_RENAME_SCRIPT="$ROOT_DIR/switchyard/rename_pe_module.py"
FONT_ALIAS_SOURCE="NotoSansCJK-Regular.ttc"
FONT_ALIAS_FILE="ArialUnicodeMS.otf"
FONT_ALIAS_FAMILY="Arial Unicode MS"
FONT_ALIAS_POSTSCRIPT="ArialUnicodeMS"
FONT_ALIAS_FACE_INDEX=1
FONT_ALIAS_SHA256="ccdd3bd646d95b31513e10ad9c975d878c0ef8b25ff2d92f2e635b50218b128e"
FONT_EMOJI_SOURCE="NotoEmoji-VariableFont_wght.ttf"
FONT_EMOJI_FILE="NotoEmoji-Static.ttf"
FONT_EMOJI_FAMILY="Noto Emoji"
FONT_EMOJI_SHA256="65d8794d403b609345baaf7a656608990a836b27af5650f6cc921088b0b026d6"
TLS_PACKAGE_MANIFEST="$ROOT_DIR/switchyard/$SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_MANIFEST_BASENAME"
TLS_SOURCE_MANIFEST="$ROOT_DIR/switchyard/tls-source-deps.tsv"
TLS_PACKAGE_SUBDIR="$SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_SUBDIR"
TLS_PACKAGE_BASE_URL="https://conda.anaconda.org/conda-forge/$TLS_PACKAGE_SUBDIR"
TLS_RUNTIME_LAYOUT_VERSION="5"
TLS_MIN_MACOS_VERSION="14.0"
TLS_PACKAGE_CACHE_DIR="${TLS_PACKAGE_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/TLS/packages}"
TLS_DEPS_CACHE_DIR="${TLS_DEPS_CACHE_DIR:-${HOME}/.switchyard/deps/tls}"
GSTREAMER_VERSION="1.28.5"
GSTREAMER_RUNTIME_LAYOUT_VERSION="4"
GSTREAMER_PACKAGE_BASE_URL="https://gstreamer.freedesktop.org/data/pkg/osx/${GSTREAMER_VERSION}"
GSTREAMER_RUNTIME_PACKAGE="gstreamer-1.0-${GSTREAMER_VERSION}-universal.pkg"
GSTREAMER_RUNTIME_PACKAGE_SHA256="0a8fc7a1cf8d7bac833ca0ebe2fd196a199c2465e810cd5b1e4b4f720c258f43"
GSTREAMER_DEVEL_PACKAGE="gstreamer-1.0-devel-${GSTREAMER_VERSION}-universal.pkg"
GSTREAMER_DEVEL_PACKAGE_SHA256="6f7b55e8fb86dcc615c9cae46b79b7785851e5c77f79a938648a81dfa2603729"
GSTREAMER_PACKAGE_CACHE_DIR="${GSTREAMER_PACKAGE_CACHE_DIR:-${HOME}/Library/Caches/Switchyard/Media/GStreamer}"
GSTREAMER_DEPS_PREFIX="${GSTREAMER_DEPS_PREFIX:-${HOME}/.switchyard/deps/media/gstreamer-${GSTREAMER_VERSION}-universal-curated-v${GSTREAMER_RUNTIME_LAYOUT_VERSION}}"
UNICORN_RUNTIME_PREFIX=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  UNICORN_RUNTIME_PREFIX="${SWITCHYARD_UNICORN_RUNTIME_PREFIX:-${HOME}/.switchyard/deps/cpu-provider/unicorn-${SWITCHYARD_UNICORN_VERSION}-${SWITCHYARD_UNICORN_SOURCE_REVISION:0:12}-build${SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION}-arm64-macos-${SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS}}"
fi
DXMT_ARCHIVE=""
DXMT_SOURCE_DIR=""
DXMT_WOW64_COMPANION_ABI_SCHEMA_SHA256=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  DXMT_ARCHIVE="${SWITCHYARD_DXMT_ARCHIVE:-${HOME}/Library/Caches/Switchyard/DXMT/${SWITCHYARD_DXMT_ARTIFACT_NAME}}"
  DXMT_SOURCE_DIR="${SWITCHYARD_DXMT_SOURCE_DIR:-${HOME}/.switchyard/deps/dxmt/${SWITCHYARD_DXMT_SOURCE_REVISION}/source}"
fi
UNICORN_PACKAGE_ROOT_RELATIVE="lib/switchyard-unicorn"
UNICORN_DYLIB_RELATIVE="$UNICORN_PACKAGE_ROOT_RELATIVE/lib/libunicorn.2.dylib"
UNICORN_SOURCE_PATCH_RELATIVE="$UNICORN_PACKAGE_ROOT_RELATIVE/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"
UNICORN_RUNTIME_RPATH='@loader_path/../../switchyard-unicorn/lib'
UNICORN_PROVIDER_UNIXLIBS=(
  "lib/wine/aarch64-unix/xtajit.so"
  "lib/wine/aarch64-unix/xtajit64.so"
)
UNICORN_PROVIDER_PE_LIBS=(
  "lib/wine/aarch64-windows/xtajit.dll"
  "lib/wine/aarch64-windows/xtajit64.dll"
)
UNICORN_PROVIDER_GUEST_ARCHS=("i386" "x86_64")
GSTREAMER_RUNTIME_COMPONENTS=(
  "base-system-1.0-${GSTREAMER_VERSION}-universal.pkg"
  "base-crypto-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-core-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-playback-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-codecs-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-codecs-restricted-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-effects-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-libav-${GSTREAMER_VERSION}-universal.pkg"
)
GSTREAMER_DEVEL_COMPONENTS=(
  "base-system-1.0-devel-${GSTREAMER_VERSION}-universal.pkg"
  "base-crypto-devel-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-core-devel-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-playback-devel-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-codecs-devel-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-codecs-restricted-devel-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-effects-devel-${GSTREAMER_VERSION}-universal.pkg"
  "gstreamer-1.0-libav-devel-${GSTREAMER_VERSION}-universal.pkg"
)
GSTREAMER_PLUGIN_FILES=(
  "libgstapp.dylib"
  "libgstasf.dylib"
  "libgstaudioconvert.dylib"
  "libgstaudiorate.dylib"
  "libgstaudioresample.dylib"
  "libgstcoreelements.dylib"
  "libgstdeinterlace.dylib"
  "libgstlibav.dylib"
  "libgstplayback.dylib"
  "libgsttypefindfunctions.dylib"
  "libgstvideoconvertscale.dylib"
  "libgstvideofilter.dylib"
  "libgstvideoparsersbad.dylib"
  "libgstvideorate.dylib"
  "libgstvolume.dylib"
)
USER_SET_WINE_BUILD_DIR="${WINE_BUILD_DIR+x}"
WINE_BUILD_DIR="${WINE_BUILD_DIR:-}"
USER_SET_WINE_INSTALL_PREFIX="${WINE_INSTALL_PREFIX+x}"
NATIVE_LLVM_BIN=""
NATIVE_MINGW_CLANG=""
NATIVE_HOST_CLANG=""
NATIVE_HOST_CLANGXX=""
NATIVE_COMPILER_POLICY_IDENTITY=""
NATIVE_MACOS_DEPLOYMENT_FLAG=""
NATIVE_MACOS_SDKROOT=""
NATIVE_MACOS_SDK_VERSION=""
NATIVE_MACOS_SDK_BUILD_VERSION=""
NATIVE_MACOS_SDK_SETTINGS_SHA256=""
NATIVE_MACOS_SDK_FLAG=""
NATIVE_MACOS_SDK_IDENTITY=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  NATIVE_MACOS_DEPLOYMENT_FLAG="-mmacosx-version-min=$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
fi
DISABLE_GPTK_OVERLAY="${SWITCHYARD_DISABLE_GPTK_OVERLAY:-0}"
case "$DISABLE_GPTK_OVERLAY" in
  0) GPTK_PATH="${GPTK_PATH:-$(defaults read dev.switchyard.Switchyard gptkPath 2>/dev/null || true)}" ;;
  1) GPTK_PATH="" ;;
  *)
    echo "SWITCHYARD_DISABLE_GPTK_OVERLAY must be 0 or 1." >&2
    exit 2
    ;;
esac
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  # The x86_64 GPTK host overlay is a Rosetta runtime dependency.  A native
  # profile must never inherit it from preferences or the caller's environment.
  DISABLE_GPTK_OVERLAY=1
  GPTK_PATH=""
fi
TLS_DLOPEN_NAME="@loader_path/../../switchyard-tls/lib/libgnutls.dylib"
DEFAULT_JOBS="$(( $(/usr/sbin/sysctl -n hw.ncpu) - 1 ))"
if [ "$DEFAULT_JOBS" -lt 1 ]; then
  DEFAULT_JOBS=1
fi
JOBS="${JOBS:-$DEFAULT_JOBS}"
if [ "$JOBS" -lt 1 ]; then
  JOBS=1
fi
RECONFIGURE="${RECONFIGURE:-0}"
INSTALL_STAGE_ROOT=""
SWAP_HELPER_DIR=""
NATIVE_ENTITLEMENTS_SNAPSHOT_FD=""
FONT_RUNTIME_PREPARED_ROOT=""

remove_prepared_font_runtime() {
  local prepared_root="$1"

  case "$prepared_root" in
    /private/tmp/switchyard-font-runtime.??????)
      [ -d "$prepared_root" ] && [ ! -L "$prepared_root" ] || return 1
      rm -rf -- "$prepared_root"
      ;;
    *)
      echo "Refusing to remove unexpected prepared font runtime: $prepared_root" >&2
      return 1
      ;;
  esac
}

cleanup_temporary_paths() {
  if [ -n "$NATIVE_ENTITLEMENTS_SNAPSHOT_FD" ] &&
     [ -e "/dev/fd/$NATIVE_ENTITLEMENTS_SNAPSHOT_FD" ]; then
    close_validated_entitlements_snapshot "$NATIVE_ENTITLEMENTS_SNAPSHOT_FD" || true
  fi
  NATIVE_ENTITLEMENTS_SNAPSHOT_FD=""
  if [ -n "$INSTALL_STAGE_ROOT" ]; then
    rm -rf "$INSTALL_STAGE_ROOT"
  fi
  if [ -n "$SWAP_HELPER_DIR" ]; then
    rm -rf "$SWAP_HELPER_DIR"
  fi
  if [ -n "$FONT_RUNTIME_PREPARED_ROOT" ]; then
    remove_prepared_font_runtime "$FONT_RUNTIME_PREPARED_ROOT" || true
  fi
  FONT_RUNTIME_PREPARED_ROOT=""
}

cleanup_temporary_paths_on_signal() {
  local signal_status="$1"

  trap - HUP INT TERM
  exit "$signal_status"
}

trap cleanup_temporary_paths EXIT
trap 'cleanup_temporary_paths_on_signal 129' HUP
trap 'cleanup_temporary_paths_on_signal 130' INT
trap 'cleanup_temporary_paths_on_signal 143' TERM

macos_no_huge_supported() {
  local compiler="clang"
  local temporary_dir

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    compiler="$NATIVE_HOST_CLANG"
  fi
  temporary_dir="$(mktemp -d)"
  cat >"$temporary_dir/conftest.c" <<'EOF'
int main(void) { return 0; }
EOF
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    if "${PROFILE_ARCH_COMMAND[@]}" "$compiler" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
        -arch "$HOST_MACHO_ARCH" "$NATIVE_MACOS_DEPLOYMENT_FLAG" \
        "$NATIVE_MACOS_SDK_FLAG" \
        "$temporary_dir/conftest.c" -o "$temporary_dir/conftest" -Wl,-no_huge >/dev/null 2>&1; then
      rm -rf "$temporary_dir"
      return 0
    fi
  else
    if "${PROFILE_ARCH_COMMAND[@]}" "$compiler" -arch "$HOST_MACHO_ARCH" \
        "$temporary_dir/conftest.c" -o "$temporary_dir/conftest" -Wl,-no_huge >/dev/null 2>&1; then
      rm -rf "$temporary_dir"
      return 0
    fi
  fi
  rm -rf "$temporary_dir"
  return 1
}

MACOS_NO_HUGE_SUPPORTED=0
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 0 ] && macos_no_huge_supported; then
  MACOS_NO_HUGE_SUPPORTED=1
fi

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1" >&2
    return 1
  fi
}

native_configured_compiler_policy_is_exact() {
  switchyard_native_configured_compiler_policy_is_exact \
    "$1" "$NATIVE_HOST_CLANG" "$NATIVE_HOST_CLANGXX" \
    "$HOST_MACHO_ARCH" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" &&
    switchyard_native_configured_host_target_policy_is_exact \
      "$1" "$NATIVE_MACOS_DEPLOYMENT_FLAG" "$NATIVE_MACOS_SDK_FLAG" \
      "$NATIVE_HOST_CLANG" "$NATIVE_HOST_CLANGXX"
}

validate_native_host_toolchain_policy() {
  switchyard_validate_qualified_native_llvm_compilers &&
    switchyard_validate_qualified_native_macos_sdk
}

reject_ambient_native_compiler_policy() {
  local variable
  local value

  for variable in CFLAGS CXXFLAGS OBJCFLAGS OBJCXXFLAGS CPPFLAGS LDFLAGS \
      MACOSX_DEPLOYMENT_TARGET IPHONEOS_DEPLOYMENT_TARGET \
      TVOS_DEPLOYMENT_TARGET WATCHOS_DEPLOYMENT_TARGET \
      DRIVERKIT_DEPLOYMENT_TARGET BRIDGEOS_DEPLOYMENT_TARGET \
      XROS_DEPLOYMENT_TARGET SDKROOT DEVELOPER_DIR TOOLCHAINS \
      CCC_OVERRIDE_OPTIONS ARCHFLAGS RC_ARCHS CLANG_CONFIG_PATH \
      CLANG_CONFIG_FILE CLANG_CONFIG_FILE_SYSTEM_DIR \
      CLANG_CONFIG_FILE_USER_DIR MAKEFLAGS MAKEFILES MFLAGS GNUMAKEFLAGS \
      MAKEOVERRIDES; do
    value="${!variable-}"
    if [ -n "$value" ]; then
      echo "Native Wine build rejects ambient $variable; the profile owns its compiler policy." >&2
      return 1
    fi
    unset "$variable"
  done

  # Autoconf also consults historical lowercase compiler variables.  They are
  # intentionally ignored rather than becoming part of native build policy.
  unset cc cxx cpp objc objcxx
}

switchyard_require_native_arm64_ensure_host() {
  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] && [ "$MODE" = "--ensure" ] || return 0
  /bin/bash "$NATIVE_ARM64_HOST_PROBE" --strict \
    --minimum-macos "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
    --kuser-model "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL"
}

switchyard_sign_preview_native_runtime_entries() {
  local entitlements entry result=0
  local entries=(
    "lib/wine/aarch64-unix/wine"
    "bin/wine.switchyard-real"
  )

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || return 0
  [ "$SWITCHYARD_RUNTIME_PROFILE" = preview-native-arm64-fex ] || {
    echo "Native engineering signing is restricted to preview-native-arm64-fex." >&2
    return 1
  }
  [ -n "$INSTALL_STAGE_ROOT" ] && [ -d "$INSTALL_STAGE_ROOT" ] &&
    [ ! -L "$INSTALL_STAGE_ROOT" ] || {
    echo "Native engineering signing requires the private runtime staging root." >&2
    return 1
  }
  entitlements="$(switchyard_runtime_profile_entitlements_path "$ROOT_DIR")" || return 1
  create_validated_entitlements_snapshot \
    "$SWITCHYARD_RUNTIME_PROFILE" "$entitlements" "$INSTALL_STAGE_ROOT" \
    NATIVE_ENTITLEMENTS_SNAPSHOT_FD || return 1

  for entry in "${entries[@]}"; do
    entry="$WINE_INSTALL_PREFIX/$entry"
    if [ ! -f "$entry" ] || [ -L "$entry" ] || [ ! -x "$entry" ]; then
      echo "Native runtime process entry is missing or unsafe: $entry" >&2
      result=1
      break
    fi
    if ! sign_engineering_macho_atomically \
        /usr/bin/codesign "$SWITCHYARD_RUNTIME_PROFILE" "$entry" \
        "$NATIVE_ENTITLEMENTS_SNAPSHOT_FD"; then
      result=1
      break
    fi
  done

  if ! close_validated_entitlements_snapshot "$NATIVE_ENTITLEMENTS_SNAPSHOT_FD"; then
    result=1
  fi
  NATIVE_ENTITLEMENTS_SNAPSHOT_FD=""
  [ "$result" -eq 0 ]
}

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  reject_ambient_native_compiler_policy || exit $?
fi

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  native_homebrew="/opt/homebrew/bin/brew"
  [ -f "$native_homebrew" ] && [ ! -L "$native_homebrew" ] &&
    [ -x "$native_homebrew" ] || {
    echo "Native ARM64 runtime requires physical Apple Silicon Homebrew." >&2
    exit 1
  }
  BREW_PREFIX="$("$native_homebrew" --prefix)"
else
  require_command brew
  BREW_PREFIX="$(brew --prefix)"
fi
export PATH="${BREW_PREFIX}/opt/bison/bin:${BREW_PREFIX}/opt/flex/bin:${BREW_PREFIX}/opt/pkgconf/bin:${BREW_PREFIX}/bin:${PATH}"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  native_llvm_prefix="$("$native_homebrew" --prefix llvm)"
  [ -d "$native_llvm_prefix/bin" ] || {
    echo "Native ARM64 runtime requires the Homebrew LLVM formula." >&2
    exit 1
  }
  NATIVE_LLVM_BIN="$(cd "$native_llvm_prefix/bin" && /bin/pwd -P)"
  [ "$NATIVE_LLVM_BIN" = \
    "/opt/homebrew/Cellar/llvm/$SWITCHYARD_NATIVE_LLVM_VERSION/bin" ] || {
    echo "Native ARM64 runtime requires the physical pinned Homebrew LLVM Cellar bin." >&2
    exit 1
  }
  switchyard_qualify_native_llvm_compilers "$NATIVE_LLVM_BIN" || exit $?
  switchyard_qualify_native_macos_sdk \
    "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" || exit $?
  NATIVE_MINGW_CLANG="$SWITCHYARD_QUALIFIED_NATIVE_CLANG"
  NATIVE_HOST_CLANG="$SWITCHYARD_QUALIFIED_NATIVE_CLANG"
  NATIVE_HOST_CLANGXX="$SWITCHYARD_QUALIFIED_NATIVE_CLANGXX"
  NATIVE_MACOS_SDKROOT="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT"
  NATIVE_MACOS_SDK_VERSION="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_VERSION"
  NATIVE_MACOS_SDK_BUILD_VERSION="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_BUILD_VERSION"
  NATIVE_MACOS_SDK_SETTINGS_SHA256="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_SETTINGS_SHA256"
  NATIVE_MACOS_SDK_FLAG="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG"
  NATIVE_MACOS_SDK_IDENTITY="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY"
  NATIVE_COMPILER_POLICY_IDENTITY="$(
    /usr/bin/printf '%s\0%s\0' \
      "$SWITCHYARD_QUALIFIED_NATIVE_COMPILER_IDENTITY" \
      "$NATIVE_MACOS_SDK_IDENTITY" |
      /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
  )"
  export SDKROOT="$NATIVE_MACOS_SDKROOT"
  export MACOSX_DEPLOYMENT_TARGET="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
  if macos_no_huge_supported; then
    MACOS_NO_HUGE_SUPPORTED=1
  fi
fi

require_command bison
require_command flex
require_command pkg-config
require_command i686-w64-mingw32-gcc
require_command x86_64-w64-mingw32-gcc
require_command shasum
require_command perl
require_command curl
require_command tar
require_command bsdtar
require_command unzip
require_command zstd
require_command install_name_tool
require_command pkgutil
require_command python3
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  require_command file
  require_command lipo
  require_command otool
  require_command vtool
  require_command xar
fi

sha256_file() {
  shasum -a 256 "$1" | awk '{print $1}'
}

short_sha256_stream() {
  shasum -a 256 | awk '{print substr($1, 1, 12)}'
}

capture_required_output() {
  [ "$#" -ge 2 ] || {
    echo "capture_required_output requires an output variable and command" >&2
    return 2
  }

  local output_variable="$1"
  local captured_output
  local command_status

  shift
  case "$output_variable" in
    ''|[0-9]*|*[!a-zA-Z0-9_]*)
      echo "invalid captured-output variable name: $output_variable" >&2
      return 2
      ;;
  esac

  # macOS Bash 3.2 clears errexit in command-substitution subshells. Restore it
  # at this boundary so nested staging failures cannot be hidden by later work.
  if captured_output="$(
    set -e
    "$@"
  )"; then
    printf -v "$output_variable" '%s' "$captured_output"
  else
    command_status=$?
    return "$command_status"
  fi
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

write_runtime_content_tree_digest() {
  local root="$1"
  python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" write "$root"
}

runtime_content_tree_digest() {
  local root="$1"
  python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" digest "$root"
}

runtime_content_tree_is_verified() {
  local root="$1"
  python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" verify "$root"
}

content_tree_is_verified() {
  local root="$1"
  local marker="$root/.switchyard-content-sha256"
  local expected

  [ -f "$marker" ] || return 1
  expected="$(tr -d '[:space:]' < "$marker")"
  [ -n "$expected" ] && [ "$(content_tree_digest "$root")" = "$expected" ]
}

validate_archive_members() {
  local archive="$1"
  local kind="$2"

  /usr/bin/python3 - "$archive" "$kind" <<'PY'
import pathlib
import stat
import sys
import tarfile
import zipfile

archive, kind = sys.argv[1:]

def validate_name(name):
    path = pathlib.PurePosixPath(name)
    if (not name or path.is_absolute() or "\\" in name or
            any(part in ("", ".", "..") for part in path.parts)):
        raise ValueError("unsafe archive path: " + name)
    return path

def validate_link(path, target, symbolic):
    target_path = pathlib.PurePosixPath(target)
    if not target or target_path.is_absolute() or "\\" in target:
        raise ValueError("unsafe archive link: " + str(path))
    combined = path.parent.joinpath(target_path) if symbolic else target_path
    depth = 0
    for part in combined.parts:
        if part in ("", "."):
            continue
        if part == "..":
            depth -= 1
        else:
            depth += 1
        if depth < 0:
            raise ValueError("archive link escapes its root: " + str(path))

count = 0
total = 0
if kind == "tar":
    with tarfile.open(archive, "r:*") as stream:
        for member in stream:
            count += 1
            total += member.size
            if member.size < 0:
                raise ValueError("archive contains a negative entry size")
            if count > 300000 or total > 4 * 1024 * 1024 * 1024:
                raise ValueError("archive exceeds its extraction resource bounds")
            path = validate_name(member.name)
            if member.issym() or member.islnk():
                validate_link(path, member.linkname, member.issym())
            elif not (member.isfile() or member.isdir()):
                raise ValueError("unsupported archive entry: " + member.name)
elif kind == "zip":
    with zipfile.ZipFile(archive) as stream:
        for member in stream.infolist():
            count += 1
            total += member.file_size
            if count > 300000 or total > 4 * 1024 * 1024 * 1024:
                raise ValueError("archive exceeds its extraction resource bounds")
            validate_name(member.filename)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ValueError("symbolic links are not accepted in zip inputs")
else:
    raise ValueError("unknown archive kind")
PY
}

validate_xar_members() {
  local archive="$1"

  /usr/bin/xar -tf "$archive" | /usr/bin/python3 -c '
import pathlib
import sys

count = 0
for raw in sys.stdin:
    name = raw.rstrip("\n")
    count += 1
    path = pathlib.PurePosixPath(name)
    if (not name or path.is_absolute() or "\\" in name or
            any(part in ("", ".", "..") for part in path.parts)):
        raise SystemExit("unsafe xar member: " + name)
if count == 0 or count > 300000:
    raise SystemExit("xar member count is outside its bound")
'
}

validate_extracted_tree_links() {
  local root="$1"

  /usr/bin/python3 - "$root" <<'PY'
import os
import stat
import sys

root = os.path.realpath(sys.argv[1])
info = os.lstat(root)
if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
    raise SystemExit("extracted root is not a real directory")
for directory, directories, files in os.walk(root, followlinks=False):
    for name in directories + files:
        path = os.path.join(directory, name)
        info = os.lstat(path)
        if stat.S_ISLNK(info.st_mode):
            resolved = os.path.realpath(path)
            if os.path.commonpath((root, resolved)) != root:
                raise SystemExit("extracted link escapes its root: " + path)
        elif not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
            raise SystemExit("extracted tree contains an unsupported entry: " + path)
PY
}

verify_host_macho_tree_arches() {
  local root="$1"
  local label="$2"
  local candidate description actual_arches actual_count expected_arch
  local expected_count macho_count=0
  shift 2

  [ -d "$root" ] && [ ! -L "$root" ] || {
    echo "$label root is missing or unsafe: $root" >&2
    return 1
  }
  expected_count="$#"
  [ "$expected_count" -gt 0 ] || return 1
  while IFS= read -r -d '' candidate; do
    description="$(file -b "$candidate")" || return 1
    case "$candidate:$description" in
      *.dylib:*Mach-O*|*.so:*Mach-O*|*.bundle:*Mach-O*|*.a:*archive*|*:*Mach-O*) ;;
      *.dylib:*|*.so:*|*.bundle:*|*.a:*)
        echo "$label contains a non-Mach-O library: $candidate ($description)" >&2
        return 1
        ;;
      *) continue ;;
    esac
    actual_arches="$(lipo "$candidate" -archs)" || return 1
    lipo "$candidate" -verify_arch "$@" >/dev/null 2>&1 || {
      echo "$label file lacks required architecture(s) $*: $candidate ($actual_arches)" >&2
      return 1
    }
    actual_count="$(/usr/bin/awk '{ print NF }' <<<"$actual_arches")"
    [ "$actual_count" -eq "$expected_count" ] || {
      echo "$label file has unexpected architecture(s): $candidate ($actual_arches; expected $*)" >&2
      return 1
    }
    for expected_arch in "$@"; do
      case " $actual_arches " in
        *" $expected_arch "*) ;;
        *) return 1 ;;
      esac
    done
    macho_count=$((macho_count + 1))
  done < <(find "$root" -type f -print0)
  [ "$macho_count" -gt 0 ] || {
    echo "$label does not contain a Mach-O file." >&2
    return 1
  }
}

adhoc_sign_host_macho_tree() {
  local root="$1"
  local label="$2"
  local candidate description
  local -a candidates=()
  local macho_count=0

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || {
    echo "$label signing is restricted to the native runtime profile." >&2
    return 1
  }
  [ -d "$root" ] && [ ! -L "$root" ] || {
    echo "$label root is missing or unsafe: $root" >&2
    return 1
  }

  # Snapshot the target paths before signing. sign_macho_atomically() creates a
  # private sibling directory, so a concurrently walking find must not discover
  # the signer's own temporary copy as another dependency target.
  while IFS= read -r -d '' candidate; do
    candidates+=("$candidate")
  done < <(find "$root" -type f -print0)

  for candidate in "${candidates[@]}"; do
    description="$(file -b "$candidate")" || {
      echo "Could not identify staged $label file: $candidate" >&2
      return 1
    }
    case "$description" in
      *Mach-O*) ;;
      *) continue ;;
    esac
    sign_macho_atomically /usr/bin/codesign "$candidate" \
      --force --sign - || {
      echo "Could not ad-hoc sign staged $label Mach-O file: $candidate" >&2
      return 1
    }
    macho_count=$((macho_count + 1))
  done

  [ "$macho_count" -gt 0 ] || {
    echo "$label does not contain a staged Mach-O file to sign." >&2
    return 1
  }
}

verify_host_macho_tree_signatures() {
  local root="$1"
  local label="$2"
  local candidate description
  local macho_count=0

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || {
    echo "$label signature verification is restricted to the native runtime profile." >&2
    return 1
  }
  [ -d "$root" ] && [ ! -L "$root" ] || {
    echo "$label root is missing or unsafe: $root" >&2
    return 1
  }

  while IFS= read -r -d '' candidate; do
    description="$(file -b "$candidate")" || {
      echo "Could not identify staged $label file: $candidate" >&2
      return 1
    }
    case "$description" in
      *Mach-O*) ;;
      *) continue ;;
    esac
    /usr/bin/codesign --verify --strict --verbose=2 \
      "$candidate" >/dev/null 2>&1 || {
      echo "Staged $label Mach-O file has an invalid code signature: $candidate" >&2
      return 1
    }
    macho_count=$((macho_count + 1))
  done < <(find "$root" -type f -print0)

  [ "$macho_count" -gt 0 ] || {
    echo "$label does not contain a staged Mach-O file to verify." >&2
    return 1
  }
}

adhoc_sign_and_verify_host_macho_tree() {
  local root="$1"
  local label="$2"

  adhoc_sign_host_macho_tree "$root" "$label" || return 1
  verify_host_macho_tree_signatures "$root" "$label"
}

adhoc_sign_and_verify_host_macho_file() {
  local candidate="$1"
  local label="$2"
  local description

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || {
    echo "$label signing is restricted to the native runtime profile." >&2
    return 1
  }
  [ -f "$candidate" ] && [ ! -L "$candidate" ] || {
    echo "$label Mach-O file is missing or unsafe: $candidate" >&2
    return 1
  }
  description="$(file -b "$candidate")" || return 1
  case "$description" in
    *Mach-O*) ;;
    *)
      echo "$label is not a Mach-O file: $candidate ($description)" >&2
      return 1
      ;;
  esac
  sign_macho_atomically /usr/bin/codesign "$candidate" \
    --force --sign - || {
    echo "Could not ad-hoc sign $label: $candidate" >&2
    return 1
  }
  /usr/bin/codesign --verify --strict --verbose=2 \
    "$candidate" >/dev/null 2>&1 || {
    echo "$label failed strict signature verification: $candidate" >&2
    return 1
  }
}

verify_native_macho_tree_macos_compatibility() {
  local root="$1"
  local label="$2"
  local maximum="$3"
  local candidate description metadata

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || return 0
  while IFS= read -r -d '' candidate; do
    description="$(file -b "$candidate")" || return 1
    case "$description" in *Mach-O*) ;; *) continue ;; esac
    metadata="$(vtool -arch arm64 -show-build "$candidate" 2>/dev/null)" || {
      echo "$label has unreadable macOS metadata: $candidate" >&2
      return 1
    }
    SWITCHYARD_MACHO_BUILD_METADATA="$metadata" /usr/bin/python3 -I - \
        "$maximum" "$candidate" <<'PY' || return 1
import os
import re
import sys

maximum, path = sys.argv[1:]
lines = os.environ["SWITCHYARD_MACHO_BUILD_METADATA"].splitlines()
if sum(line.split()[:2] == ["cmd", "LC_BUILD_VERSION"] for line in lines) != 1:
    raise SystemExit(path + ": expected one LC_BUILD_VERSION")
if any(line.split()[:2] == ["cmd", "LC_VERSION_MIN_MACOSX"] for line in lines):
    raise SystemExit(path + ": legacy macOS deployment command is not accepted")

fields = {}
for line in lines:
    parts = line.split()
    if len(parts) == 2 and parts[0] in ("platform", "minos", "sdk"):
        if parts[0] in fields:
            raise SystemExit(path + ": duplicate build metadata field " + parts[0])
        fields[parts[0]] = parts[1]
if fields.get("platform") != "MACOS":
    raise SystemExit(path + ": Mach-O platform is not macOS")

def version(value):
    if re.fullmatch(r"[0-9]+(?:[.][0-9]+){0,2}", value or "") is None:
        raise SystemExit(path + ": malformed macOS version")
    parts = [int(item) for item in value.split(".")]
    return tuple((parts + [0, 0])[:3])

if version(fields.get("minos")) > version(maximum):
    raise SystemExit(path + ": minimum macOS exceeds the runtime profile")
version(fields.get("sdk"))
PY
  done < <(find "$root" -type f -print0)
}

verify_runtime_relative_macho_tree() {
  local root="$1"
  local label="$2"
  local candidate description dependencies dependency rpaths rpath

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || return 0
  while IFS= read -r -d '' candidate; do
    description="$(file -b "$candidate")" || return 1
    case "$description" in *Mach-O*) ;; *) continue ;; esac
    # This verifier runs inside dependency-staging command substitutions.
    # macOS Bash 3.2 retains nested process-substitution descriptors until the
    # function returns, so snapshot each bounded tool result before iterating.
    dependencies="$(otool -L "$candidate" |
      /usr/bin/awk '$0 ~ /^\t/ { print $1 }')" || return 1
    while IFS= read -r dependency; do
      [ -n "$dependency" ] || continue
      case "$dependency" in
        *'/../'*|*'/./'*|*/..|*/.)
          echo "$label contains a traversing Mach-O dependency: $candidate -> $dependency" >&2
          return 1
          ;;
      esac
      case "$dependency" in
        @rpath/*|@loader_path/*|@executable_path/*|/usr/lib/*|/System/Library/*) ;;
        *)
          echo "$label retains a non-runtime dependency: $candidate -> $dependency" >&2
          return 1
          ;;
      esac
    done <<<"$dependencies"
    rpaths="$(otool -l "$candidate" |
      /usr/bin/awk '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')" ||
      return 1
    while IFS= read -r rpath; do
      [ -n "$rpath" ] || continue
      case "$rpath" in
        @loader_path|@loader_path/*|@executable_path|@executable_path/*) ;;
        *)
          echo "$label retains a non-runtime rpath: $candidate -> $rpath" >&2
          return 1
          ;;
      esac
    done <<<"$rpaths"
  done < <(find "$root" -type f -print0)
  return 0
}

vulkan_deps_match_profile_architecture() {
  local prefix="$1"

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    verify_host_macho_tree_arches "$prefix" \
      "Vulkan runtime" "$HOST_DEPENDENCY_ARCH" >/dev/null 2>&1 &&
      verify_native_macho_tree_macos_compatibility "$prefix" \
        "Vulkan runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
        >/dev/null 2>&1
  else
    file "$prefix/lib/libvulkan.1.4.350.dylib" | grep -q "x86_64" &&
      file "$prefix/lib/libMoltenVK.dylib" | grep -q "x86_64"
  fi
}

font_deps_match_profile_architecture() {
  local prefix="$1"

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    verify_host_macho_tree_arches "$prefix" \
      "font runtime" "$HOST_DEPENDENCY_ARCH" >/dev/null 2>&1 &&
      verify_native_macho_tree_macos_compatibility "$prefix" \
        "font runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
        >/dev/null 2>&1
  else
    file "$prefix/lib/libfreetype.6.dylib" | grep -q "x86_64" &&
      file "$prefix/lib/libfontconfig.1.dylib" | grep -q "x86_64"
  fi
}

tls_deps_match_profile_architecture() {
  local prefix="$1"

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    verify_host_macho_tree_arches "$prefix" \
      "TLS runtime" "$HOST_DEPENDENCY_ARCH" >/dev/null 2>&1 &&
      verify_native_macho_tree_macos_compatibility "$prefix" \
        "TLS runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
        >/dev/null 2>&1
  else
    file "$prefix/lib/libgnutls.30.dylib" | grep -q "x86_64"
  fi
}

gstreamer_deps_match_profile_architecture() {
  local prefix="$1"
  local plugin

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    verify_host_macho_tree_arches "$prefix" \
      "GStreamer runtime" "${GSTREAMER_MACHO_ARCHS[@]}" >/dev/null 2>&1 &&
      verify_native_macho_tree_macos_compatibility "$prefix" \
        "GStreamer runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
        >/dev/null 2>&1
    return
  fi

  file "$prefix/lib/libgstreamer-1.0.0.dylib" | grep "x86_64" >/dev/null || return 1
  for plugin in "${GSTREAMER_PLUGIN_FILES[@]}"; do
    file "$prefix/lib/gstreamer-1.0/$plugin" | grep "x86_64" >/dev/null || return 1
  done
}

SWITCHYARD_MANAGED_RUNTIME_ROOT="${HOME}/.switchyard/runtimes"
source "$ROOT_DIR/switchyard/lib/directory_safety.sh"
source "$ROOT_DIR/switchyard/lib/source_state.sh"

source_tree_digest() {
  {
    git -C "$WINE_DIR" diff --binary HEAD --
    git -C "$WINE_DIR" ls-files --others --exclude-standard | LC_ALL=C sort | while IFS= read -r path; do
      printf 'untracked %s\n' "$path"
      sha256_file "$WINE_DIR/$path"
    done
  } | short_sha256_stream
}

assert_source_state_unchanged() {
  local phase="$1"
  local current_fingerprint
  local current_revision

  if ! current_revision="$(git -C "$WINE_DIR" rev-parse HEAD)" ||
     ! current_fingerprint="$(switchyard_source_state_fingerprint "$WINE_DIR")"; then
    echo "failed to verify the Wine source state after $phase" >&2
    return 1
  fi
  if [ "$current_revision" != "$wine_revision" ] ||
     [ "$current_fingerprint" != "$build_source_fingerprint" ]; then
    echo "Wine source changed after runtime build metadata was captured ($phase)." >&2
    echo "Refusing to publish a runtime assembled from multiple source states." >&2
    return 1
  fi
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
  local emoji_source_asset=""
  local font_count=0
  local license_count=0

  if [ ! -f "$FONT_ASSET_MANIFEST" ]; then
    echo "missing font asset manifest: $FONT_ASSET_MANIFEST" >&2
    exit 1
  fi

  if content_tree_is_verified "$FONT_ASSET_PREFIX" &&
     cmp -s "$FONT_ASSET_MANIFEST" \
       "$FONT_ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv" &&
     [ -f "$FONT_ASSET_PREFIX/lib/switchyard-fonts/share/doc/switchyard-font-assets/generated-fonts.tsv" ] &&
     [ -f "$FONT_ASSET_PREFIX/share/wine/fonts/$FONT_EMOJI_FILE" ] &&
     [ "$(sha256_file "$FONT_ASSET_PREFIX/share/wine/fonts/$FONT_EMOJI_FILE")" = "$FONT_EMOJI_SHA256" ] &&
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
      font|font-source|license) ;;
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

    capture_required_output asset \
      download_font_asset "$name" "$expected_hash" "$url" || return $?
    if [ "$kind" = "font" ] || [ "$kind" = "font-source" ]; then
      case "$name" in
        *.ttf|*.ttc|*.otf) ;;
        *)
          echo "unsupported font file extension: $name" >&2
          exit 1
          ;;
      esac
      if [ "$kind" = "font" ]; then
        install -m 0644 "$asset" "$temporary_prefix/share/wine/fonts/$name"
        font_count=$((font_count + 1))
      elif [ "$name" = "$FONT_EMOJI_SOURCE" ]; then
        emoji_source_asset="$asset"
      else
        echo "unsupported generated font source: $name" >&2
        exit 1
      fi
    else
      install -m 0644 "$asset" \
        "$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/$name"
      license_count=$((license_count + 1))
    fi
  done < "$FONT_ASSET_MANIFEST"

  if [ "$font_count" -lt 1 ] || [ "$license_count" -lt 1 ] || [ -z "$emoji_source_asset" ]; then
    echo "font asset manifest did not provide fonts, the emoji source, and license notices" >&2
    exit 1
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to generate bundled font derivatives" >&2
    exit 1
  fi
  if [ ! -f "$FONT_ALIAS_SCRIPT" ] || [ ! -f "$FONT_EMOJI_STATIC_SCRIPT" ]; then
    echo "missing bundled font generator" >&2
    exit 1
  fi
  python3 "$FONT_EMOJI_STATIC_SCRIPT" \
    "$emoji_source_asset" \
    "$temporary_prefix/share/wine/fonts/$FONT_EMOJI_FILE"
  if [ "$(sha256_file "$temporary_prefix/share/wine/fonts/$FONT_EMOJI_FILE")" != "$FONT_EMOJI_SHA256" ]; then
    echo "generated static emoji font has an unexpected sha256" >&2
    exit 1
  fi
  font_count=$((font_count + 1))
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
  printf 'file\tfamily\tsource\tsha256\n%s\t%s\t%s\t%s\n' \
    "$FONT_EMOJI_FILE" "$FONT_EMOJI_FAMILY" "$FONT_EMOJI_SOURCE" "$FONT_EMOJI_SHA256" \
    >"$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/generated-fonts.tsv"
  cat >"$temporary_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/README.txt" <<EOF
Switchyard Wine redistributable font set $FONT_ASSET_SET_VERSION

The Noto font binaries are installed under share/wine/fonts so every Wine
prefix has deterministic multilingual and emoji fallback. $FONT_EMOJI_FILE is
a default-instance static outline derivative of $FONT_EMOJI_SOURCE: only its
variable-font tables are removed. It retains the upstream Noto Emoji family
name and is licensed under the OFL. ArialUnicodeMS.otf is an OFL-licensed
compatibility alias generated from the Korean face of NotoSansCJK-Regular.ttc;
it is not a Microsoft font. Preserve the included license notices, manifest,
and generated-font metadata when distributing the runtime.
EOF

  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$FONT_ASSET_PREFIX" cache
  printf '%s\n' "$FONT_ASSET_PREFIX"
}

download_mesa_windows_asset() {
  local name="$1"
  local expected_hash="$2"
  local url="$3"
  local cached_file="$MESA_WINDOWS_CACHE_DIR/$name"
  local temporary_file="${cached_file}.tmp.$$"
  local actual_hash

  mkdir -p "$MESA_WINDOWS_CACHE_DIR"
  if [ -f "$cached_file" ]; then
    actual_hash="$(sha256_file "$cached_file")"
    if [ "$actual_hash" = "$expected_hash" ]; then
      printf '%s\n' "$cached_file"
      return 0
    fi
    echo "cached Mesa Windows asset $name has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  rm -f "$temporary_file"
  echo "downloading Mesa Windows asset $name" >&2
  curl -fL --retry 3 --connect-timeout 20 --proto '=https' --tlsv1.2 \
    -o "$temporary_file" "$url"
  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$expected_hash" ]; then
    rm -f "$temporary_file"
    echo "Mesa Windows asset $name sha256 mismatch: expected $expected_hash, got $actual_hash" >&2
    exit 1
  fi
  mv "$temporary_file" "$cached_file"
  printf '%s\n' "$cached_file"
}

stage_mesa_windows_opengl() {
  local x86_64_opengl_dll="$MESA_WINDOWS_DEPS_PREFIX/x86_64-windows/opengl32.dll"
  local x86_64_gallium_dll="$MESA_WINDOWS_DEPS_PREFIX/x86_64-windows/libgallium_wgl.dll"
  local i386_opengl_dll="$MESA_WINDOWS_DEPS_PREFIX/i386-windows/opengl32.dll"
  local i386_gallium_dll="$MESA_WINDOWS_DEPS_PREFIX/i386-windows/libgallium_wgl.dll"
  local notice_root="$MESA_WINDOWS_DEPS_PREFIX/share/doc/switchyard-mesa"
  local archive
  local mesa_license
  local llvm_license
  local distributor_license
  local staging_dir
  local temporary_prefix

  if content_tree_is_verified "$MESA_WINDOWS_DEPS_PREFIX" &&
     [ -f "$x86_64_opengl_dll" ] &&
     [ "$(sha256_file "$x86_64_opengl_dll")" = "$MESA_WINDOWS_X86_64_OPENGL_SHA256" ] &&
     [ -f "$x86_64_gallium_dll" ] &&
     [ "$(sha256_file "$x86_64_gallium_dll")" = "$MESA_WINDOWS_X86_64_GALLIUM_SHA256" ] &&
     [ -f "$i386_opengl_dll" ] &&
     [ "$(sha256_file "$i386_opengl_dll")" = "$MESA_WINDOWS_I386_OPENGL_SHA256" ] &&
     [ -f "$i386_gallium_dll" ] &&
     [ "$(sha256_file "$i386_gallium_dll")" = "$MESA_WINDOWS_I386_GALLIUM_SHA256" ] &&
     [ -f "$notice_root/MESA-LICENSE.rst" ] &&
     [ "$(sha256_file "$notice_root/MESA-LICENSE.rst")" = "$MESA_SOURCE_LICENSE_SHA256" ] &&
     [ -f "$notice_root/LLVM-LICENSE.txt" ] &&
     [ "$(sha256_file "$notice_root/LLVM-LICENSE.txt")" = "$MESA_LLVM_LICENSE_SHA256" ] &&
     [ -f "$notice_root/DISTRIBUTOR-LICENSE.txt" ] &&
     [ "$(sha256_file "$notice_root/DISTRIBUTOR-LICENSE.txt")" = "$MESA_WINDOWS_DISTRIBUTOR_LICENSE_SHA256" ]; then
    printf '%s\n' "$MESA_WINDOWS_DEPS_PREFIX"
    return 0
  fi

  capture_required_output archive download_mesa_windows_asset \
    "$MESA_WINDOWS_ARCHIVE" "$MESA_WINDOWS_ARCHIVE_SHA256" \
    "$MESA_WINDOWS_ARCHIVE_URL" || return $?
  capture_required_output mesa_license download_mesa_windows_asset \
    "MESA-LICENSE.rst" "$MESA_SOURCE_LICENSE_SHA256" \
    "$MESA_SOURCE_LICENSE_URL" || return $?
  capture_required_output llvm_license download_mesa_windows_asset \
    "LLVM-LICENSE.txt" "$MESA_LLVM_LICENSE_SHA256" \
    "$MESA_LLVM_LICENSE_URL" || return $?
  capture_required_output distributor_license download_mesa_windows_asset \
    "DISTRIBUTOR-LICENSE.txt" "$MESA_WINDOWS_DISTRIBUTOR_LICENSE_SHA256" \
    "$MESA_WINDOWS_DISTRIBUTOR_LICENSE_URL" || return $?

  staging_dir="$(mktemp -d)"
  temporary_prefix="${MESA_WINDOWS_DEPS_PREFIX}.tmp.$$"
  rm -rf "$temporary_prefix"
  mkdir -p "$temporary_prefix/x86_64-windows" "$temporary_prefix/i386-windows" \
    "$temporary_prefix/share/doc/switchyard-mesa"

  bsdtar -xf "$archive" -C "$staging_dir" \
    x64/opengl32.dll x64/libgallium_wgl.dll \
    x86/opengl32.dll x86/libgallium_wgl.dll readme.txt
  install -m 0644 "$staging_dir/x64/opengl32.dll" \
    "$temporary_prefix/x86_64-windows/opengl32.dll"
  install -m 0644 "$staging_dir/x64/libgallium_wgl.dll" \
    "$temporary_prefix/x86_64-windows/libgallium_wgl.dll"
  install -m 0644 "$staging_dir/x86/opengl32.dll" \
    "$temporary_prefix/i386-windows/opengl32.dll"
  install -m 0644 "$staging_dir/x86/libgallium_wgl.dll" \
    "$temporary_prefix/i386-windows/libgallium_wgl.dll"
  install -m 0644 "$staging_dir/readme.txt" \
    "$temporary_prefix/share/doc/switchyard-mesa/DISTRIBUTOR-README.txt"
  install -m 0644 "$mesa_license" \
    "$temporary_prefix/share/doc/switchyard-mesa/MESA-LICENSE.rst"
  install -m 0644 "$llvm_license" \
    "$temporary_prefix/share/doc/switchyard-mesa/LLVM-LICENSE.txt"
  install -m 0644 "$distributor_license" \
    "$temporary_prefix/share/doc/switchyard-mesa/DISTRIBUTOR-LICENSE.txt"

  if ! file "$temporary_prefix/x86_64-windows/opengl32.dll" |
       grep -q 'PE32+.*x86-64'; then
    echo "Mesa opengl32.dll is not an x86_64 PE image." >&2
    exit 1
  fi
  if ! file "$temporary_prefix/x86_64-windows/libgallium_wgl.dll" |
       grep -q 'PE32+.*x86-64'; then
    echo "Mesa libgallium_wgl.dll is not an x86_64 PE image." >&2
    exit 1
  fi
  if ! file "$temporary_prefix/i386-windows/opengl32.dll" |
       grep -q 'PE32 executable.*Intel 80386'; then
    echo "Mesa opengl32.dll is not an i386 PE image." >&2
    exit 1
  fi
  if ! file "$temporary_prefix/i386-windows/libgallium_wgl.dll" |
       grep -q 'PE32 executable.*Intel 80386'; then
    echo "Mesa libgallium_wgl.dll is not an i386 PE image." >&2
    exit 1
  fi
  if [ "$(sha256_file "$temporary_prefix/x86_64-windows/opengl32.dll")" != "$MESA_WINDOWS_X86_64_OPENGL_SHA256" ] ||
     [ "$(sha256_file "$temporary_prefix/x86_64-windows/libgallium_wgl.dll")" != "$MESA_WINDOWS_X86_64_GALLIUM_SHA256" ] ||
     [ "$(sha256_file "$temporary_prefix/i386-windows/opengl32.dll")" != "$MESA_WINDOWS_I386_OPENGL_SHA256" ] ||
     [ "$(sha256_file "$temporary_prefix/i386-windows/libgallium_wgl.dll")" != "$MESA_WINDOWS_I386_GALLIUM_SHA256" ]; then
    echo "Mesa Windows OpenGL files do not match their pinned hashes." >&2
    exit 1
  fi

  cat >"$temporary_prefix/share/doc/switchyard-mesa/README.txt" <<EOF
Switchyard Wine Mesa Windows OpenGL fallback

Version: $MESA_WINDOWS_VERSION
Architectures: i386 and x86_64 Windows PE
Driver: llvmpipe (software rendering)
Binary package: $MESA_WINDOWS_ARCHIVE_URL
Binary package SHA-256: $MESA_WINDOWS_ARCHIVE_SHA256
Distributor repository: $MESA_WINDOWS_DISTRIBUTOR_REPOSITORY
Distributor revision: $MESA_WINDOWS_DISTRIBUTOR_REVISION
Mesa source repository: $MESA_SOURCE_REPOSITORY
Mesa source revision: $MESA_SOURCE_REVISION
LLVM version: $MESA_WINDOWS_LLVM_VERSION

Set WINE_OPENGL_DRIVER=llvmpipe in the host environment to select these
DLLs for the complete Windows process tree. The runtime does not inspect or
match executable names. Without that setting, Wine's built-in OpenGL driver
remains selected. GALLIUM_DRIVER and MESA_LOADER_DRIVER_OVERRIDE are pinned to
llvmpipe in this mode for correctness; software rendering can be slower than
hardware graphics.

Preserve this file, the package readme, and all included license notices when
distributing the runtime.
EOF

  {
    printf '{\n'
    printf '  "version": %s,\n' "$(json_string "$MESA_WINDOWS_VERSION")"
    printf '  "architectures": ["i386-windows", "x86_64-windows"],\n'
    printf '  "driver": "llvmpipe",\n'
    printf '  "archive": %s,\n' "$(json_string "$MESA_WINDOWS_ARCHIVE")"
    printf '  "archiveUrl": %s,\n' "$(json_string "$MESA_WINDOWS_ARCHIVE_URL")"
    printf '  "archiveSha256": %s,\n' "$(json_string "$MESA_WINDOWS_ARCHIVE_SHA256")"
    printf '  "distributorRepository": %s,\n' \
      "$(json_string "$MESA_WINDOWS_DISTRIBUTOR_REPOSITORY")"
    printf '  "distributorRevision": %s,\n' \
      "$(json_string "$MESA_WINDOWS_DISTRIBUTOR_REVISION")"
    printf '  "mesaSourceRepository": %s,\n' "$(json_string "$MESA_SOURCE_REPOSITORY")"
    printf '  "mesaSourceRevision": %s,\n' "$(json_string "$MESA_SOURCE_REVISION")"
    printf '  "llvmVersion": %s,\n' "$(json_string "$MESA_WINDOWS_LLVM_VERSION")"
    printf '  "files": {\n'
    printf '    "x86_64-windows/opengl32.dll": %s,\n' "$(json_string "$MESA_WINDOWS_X86_64_OPENGL_SHA256")"
    printf '    "x86_64-windows/libgallium_wgl.dll": %s,\n' "$(json_string "$MESA_WINDOWS_X86_64_GALLIUM_SHA256")"
    printf '    "i386-windows/opengl32.dll": %s,\n' "$(json_string "$MESA_WINDOWS_I386_OPENGL_SHA256")"
    printf '    "i386-windows/libgallium_wgl.dll": %s\n' "$(json_string "$MESA_WINDOWS_I386_GALLIUM_SHA256")"
    printf '  }\n'
    printf '}\n'
  } >"$temporary_prefix/switchyard-mesa-runtime.json"

  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$MESA_WINDOWS_DEPS_PREFIX" cache
  rm -rf "$staging_dir"
  printf '%s\n' "$MESA_WINDOWS_DEPS_PREFIX"
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
     vulkan_deps_match_profile_architecture "$VULKAN_DEPS_PREFIX" &&
     { [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 0 ] ||
       verify_host_macho_tree_signatures "$VULKAN_DEPS_PREFIX" \
         "Vulkan runtime" >/dev/null 2>&1; }; then
    printf '%s\n' "$VULKAN_DEPS_PREFIX"
    return 0
  fi

  capture_required_output loader_archive download_homebrew_oci_blob \
    "$VULKAN_LOADER_REPOSITORY" "sha256:$VULKAN_LOADER_LAYER_SHA256" \
    "$VULKAN_LOADER_BOTTLE" || return $?
  capture_required_output headers_archive download_homebrew_oci_blob \
    "$VULKAN_HEADERS_REPOSITORY" "sha256:$VULKAN_HEADERS_LAYER_SHA256" \
    "$VULKAN_HEADERS_BOTTLE" || return $?
  capture_required_output moltenvk_archive download_homebrew_oci_blob \
    "$MOLTENVK_REPOSITORY" "sha256:$MOLTENVK_LAYER_SHA256" \
    "$MOLTENVK_BOTTLE" || return $?
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_archive_members "$loader_archive" tar
    validate_archive_members "$headers_archive" tar
    validate_archive_members "$moltenvk_archive" tar
  fi

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
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_extracted_tree_links "$staging_dir"
  fi

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
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    vulkan_deps_match_profile_architecture "$temporary_prefix"
    verify_runtime_relative_macho_tree "$temporary_prefix" "Vulkan runtime"
    adhoc_sign_and_verify_host_macho_tree "$temporary_prefix" "Vulkan runtime"
  fi

  {
    printf 'prefix=%s\n' "$VULKAN_DEPS_PREFIX"
    printf 'exec_prefix=${prefix}\n'
    printf 'libdir=${exec_prefix}/lib\n'
    printf 'includedir=${prefix}/include\n\n'
    printf 'Name: Vulkan-Loader\n'
    printf 'Description: Switchyard staged %s Vulkan loader\n' "$HOST_DEPENDENCY_ARCH"
    printf 'Version: 1.4.350\n'
    printf 'Libs: -L${libdir} -lvulkan\n'
    printf 'Cflags: -I${includedir}\n'
  } >"$temporary_prefix/lib/pkgconfig/vulkan.pc"

  {
    printf '{\n'
    printf '  "architecture": %s,\n' "$(json_string "$HOST_DEPENDENCY_ARCH")"
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
     font_deps_match_profile_architecture "$FONT_DEPS_PREFIX" &&
     { [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 0 ] ||
       verify_host_macho_tree_signatures "$FONT_DEPS_PREFIX" \
         "font runtime" >/dev/null 2>&1; }; then
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
    bottle="${name}--${version}.${SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG}.bottle.tar.gz"
    capture_required_output archive download_homebrew_oci_blob \
      "$repository" "sha256:$sha" "$bottle" "$FONT_DEPS_CACHE_DIR" || return $?
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      validate_archive_members "$archive" tar
    fi

    tar -xzf "$archive" -C "$staging_dir"
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      validate_extracted_tree_links "$staging_dir"
    fi
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
Description: Switchyard staged $HOST_DEPENDENCY_ARCH FreeType
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
Description: Switchyard staged $HOST_DEPENDENCY_ARCH fontconfig
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

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    cat >"$temporary_prefix/share/doc/switchyard-font-deps/README.txt" <<EOF
This directory contains user-local $HOST_DEPENDENCY_ARCH FreeType/fontconfig
runtime dependencies staged from pinned Homebrew
$SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG bottles for the Switchyard Wine build.

The staged files keep Wine's GDI font backend independent of a mutable host
Homebrew prefix. Do not commit these binaries to the Switchyard repository.
Preserve upstream license notices when distributing a runtime built with these
libraries.
EOF
  else
    cat >"$temporary_prefix/share/doc/switchyard-font-deps/README.txt" <<'EOF'
This directory contains user-local x86_64 FreeType/fontconfig runtime
dependencies staged from Homebrew sonoma bottles for the Switchyard Wine build.

The staged files let Wine's GDI font backend run under Rosetta without linking
against the host arm64 Homebrew prefix. Do not commit these binaries to the
Switchyard repository. Preserve upstream license notices when distributing a
runtime built with these libraries.
EOF
  fi

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    font_deps_match_profile_architecture "$temporary_prefix"
    adhoc_sign_and_verify_host_macho_tree "$temporary_prefix" "font runtime"
  fi

  test_source="$temporary_prefix/freetype-link-test.c"
  test_binary="$temporary_prefix/freetype-link-test"
  cat >"$test_source" <<'EOF'
#include <ft2build.h>
#include FT_FREETYPE_H
int main(void) { FT_Library lib; return FT_Init_FreeType(&lib); }
EOF
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_native_host_toolchain_policy || return 1
    "${PROFILE_ARCH_COMMAND[@]}" "$NATIVE_HOST_CLANG" \
      "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" -arch "$HOST_MACHO_ARCH" \
      "$NATIVE_MACOS_DEPLOYMENT_FLAG" "$NATIVE_MACOS_SDK_FLAG" \
      -I"$temporary_prefix/include/freetype2" \
      -L"$temporary_prefix/lib" \
      -Wl,-rpath,"$temporary_prefix/lib" \
      "$test_source" -lfreetype -o "$test_binary"
  else
    "${PROFILE_ARCH_COMMAND[@]}" clang -arch "$HOST_MACHO_ARCH" \
      -mmacosx-version-min="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
      -I"$temporary_prefix/include/freetype2" \
      -L"$temporary_prefix/lib" \
      -Wl,-rpath,"$temporary_prefix/lib" \
      "$test_source" -lfreetype -o "$test_binary"
  fi
  env DYLD_LIBRARY_PATH="$temporary_prefix/lib" "$test_binary"
  rm -f "$test_source" "$test_binary"

  test_source="$temporary_prefix/fontconfig-link-test.c"
  test_binary="$temporary_prefix/fontconfig-link-test"
  cat >"$test_source" <<'EOF'
#include <fontconfig/fontconfig.h>
int main(void) { return FcInit() ? 0 : 1; }
EOF
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_native_host_toolchain_policy || return 1
    "${PROFILE_ARCH_COMMAND[@]}" "$NATIVE_HOST_CLANG" \
      "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" -arch "$HOST_MACHO_ARCH" \
      "$NATIVE_MACOS_DEPLOYMENT_FLAG" "$NATIVE_MACOS_SDK_FLAG" \
      -I"$temporary_prefix/include" \
      -I"$temporary_prefix/include/freetype2" \
      -L"$temporary_prefix/lib" \
      -Wl,-rpath,"$temporary_prefix/lib" \
      "$test_source" -lfontconfig -lfreetype -lintl -lunistring -lpng16 \
      -o "$test_binary"
  else
    "${PROFILE_ARCH_COMMAND[@]}" clang -arch "$HOST_MACHO_ARCH" \
      -mmacosx-version-min="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
      -I"$temporary_prefix/include" \
      -I"$temporary_prefix/include/freetype2" \
      -L"$temporary_prefix/lib" \
      -Wl,-rpath,"$temporary_prefix/lib" \
      "$test_source" -lfontconfig -lfreetype -lintl -lunistring -lpng16 \
      -o "$test_binary"
  fi
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

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    adhoc_sign_and_verify_host_macho_tree "$runtime_font_root" \
      "installed font runtime"
  fi
}

prepare_font_runtime_for_install() {
  (
  local prepared_root_cleanup_target=""
  local prepared_root_status

  # This function normally runs inside a command substitution, whose Bash 3.2
  # subshell does not inherit the caller's traps.  Own an additional isolated
  # subshell so a signal or any nonzero return before publication removes the
  # private tree even before the parent can record its path.
  trap '
    prepared_root_status=$?
    trap - EXIT
    if [ "$prepared_root_status" -ne 0 ] &&
       [ -n "$prepared_root_cleanup_target" ]; then
      remove_prepared_font_runtime "$prepared_root_cleanup_target" || true
    fi
    exit "$prepared_root_status"
  ' EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM

  [ "$#" -eq 2 ] || {
    echo "prepare_font_runtime_for_install requires dependency and asset roots" >&2
    return 2
  }

  local source_prefix="$1"
  local font_assets_prefix="$2"
  local created_root
  local runtime_font_root

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || {
    echo "Prepared font runtime closure is restricted to the native profile." >&2
    return 1
  }
  [ -d "$source_prefix" ] && [ ! -L "$source_prefix" ] || {
    echo "Font dependency root is missing or unsafe: $source_prefix" >&2
    return 1
  }
  [ -d "$font_assets_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets" ] &&
    [ ! -L "$font_assets_prefix" ] || {
    echo "Font asset documentation root is missing or unsafe: $font_assets_prefix" >&2
    return 1
  }
  validate_extracted_tree_links "$source_prefix" || {
    echo "Font dependency root contains an unsafe link or file type: $source_prefix" >&2
    return 1
  }
  validate_extracted_tree_links "$font_assets_prefix" || {
    echo "Font asset root contains an unsafe link or file type: $font_assets_prefix" >&2
    return 1
  }
  created_root="$(
    /usr/bin/mktemp -d /private/tmp/switchyard-font-runtime.XXXXXX
  )" || return 1
  prepared_root_cleanup_target="$created_root"
  runtime_font_root="$(cd "$created_root" && /bin/pwd -P)" || return 1
  prepared_root_cleanup_target="$runtime_font_root"
  chmod 0700 "$runtime_font_root" || return 1

  if ! (
    set -e
    ditto "$source_prefix" "$runtime_font_root"
    relocate_font_deps_for_runtime "$runtime_font_root" "$source_prefix"
    if [ -f "$runtime_font_root/etc/fonts/fonts.conf" ]; then
      SWITCHYARD_FONT_SOURCE_PREFIX="$source_prefix" \
        perl -0pi -e '
          s#\n\s*<cachedir>\Q$ENV{SWITCHYARD_FONT_SOURCE_PREFIX}\E/var/cache/fontconfig</cachedir>##g;
          s#\n\s*<cachedir>(?:/usr/local|/opt/homebrew|\@\@HOMEBREW_PREFIX\@\@)/var/cache/fontconfig</cachedir>##g
        ' "$runtime_font_root/etc/fonts/fonts.conf"
    fi
    [ -f "$FONTCONFIG_ASSET_FRAGMENT" ] && [ ! -L "$FONTCONFIG_ASSET_FRAGMENT" ] || {
      echo "missing or unsafe Fontconfig asset fragment: $FONTCONFIG_ASSET_FRAGMENT" >&2
      exit 1
    }
    mkdir -p "$runtime_font_root/etc/fonts/conf.d" \
      "$runtime_font_root/share/doc/switchyard-font-assets"
    install -m 0644 "$FONTCONFIG_ASSET_FRAGMENT" \
      "$runtime_font_root/etc/fonts/conf.d/50-switchyard-font-assets.conf"
    ditto "$font_assets_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets" \
      "$runtime_font_root/share/doc/switchyard-font-assets"
    chmod 0755 "$runtime_font_root"
    validate_extracted_tree_links "$runtime_font_root"
    font_deps_match_profile_architecture "$runtime_font_root"
    verify_runtime_relative_macho_tree "$runtime_font_root" \
      "prepared font runtime"
    verify_host_macho_tree_signatures "$runtime_font_root" \
      "prepared font runtime"
    write_content_tree_digest "$runtime_font_root"
    content_tree_is_verified "$runtime_font_root"
  ); then
    return 1
  fi

  printf '%s\n' "$runtime_font_root"
  )
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

download_tls_source() {
  local source_name="$1"
  local filename="$2"
  local url="$3"
  local expected_hash="$4"
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
    echo "cached $source_name source has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  temporary_file="${cached_file}.tmp.$$"
  rm -f "$temporary_file"
  curl -fL --retry 3 --retry-delay 1 --proto '=https' --tlsv1.2 \
    -o "$temporary_file" "$url"
  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$expected_hash" ]; then
    rm -f "$temporary_file"
    echo "$source_name source has unexpected sha256 $actual_hash; expected $expected_hash." >&2
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
  local uncompressed
  local uncompressed_size

  mkdir -p "$destination"
  case "$archive" in
    *.conda)
      container="$(mktemp -d)"
      if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
        validate_archive_members "$archive" zip
      fi
      unzip -q "$archive" -d "$container"
      if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
        validate_extracted_tree_links "$container"
      fi
      for payload in "$container"/pkg-*.tar.zst "$container"/info-*.tar.zst; do
        [ -f "$payload" ] || continue
        if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
          uncompressed_size="$(zstd -lv "$payload" 2>&1 | /usr/bin/sed -nE \
            's/^Decompressed Size:.*\(([0-9]+) B\).*/\1/p')"
          case "$uncompressed_size" in
            ''|*[!0-9]*)
              echo "TLS package payload has no bounded Zstandard content size: $payload" >&2
              exit 1
              ;;
          esac
          [ "$uncompressed_size" -le $((2 * 1024 * 1024 * 1024)) ] || {
            echo "TLS package payload exceeds its extraction size bound: $payload" >&2
            exit 1
          }
          uncompressed="$container/$(basename "${payload%.zst}").validated"
          zstd -dc "$payload" >"$uncompressed"
          validate_archive_members "$uncompressed" tar
          tar -xf "$uncompressed" -C "$destination"
          rm -f "$uncompressed"
        else
          zstd -dc "$payload" | tar -xf - -C "$destination"
        fi
      done
      rm -rf "$container"
      ;;
    *.tar.bz2)
      if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
        validate_archive_members "$archive" tar
      fi
      tar -xjf "$archive" -C "$destination"
      ;;
    *)
      echo "unsupported TLS package archive: $archive" >&2
      exit 1
      ;;
  esac
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_extracted_tree_links "$destination"
  fi
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
  local source_name
  local source_version
  local source_filename
  local source_url
  local source_hash
  local source_archive
  local source_work
  local source_root
  local source_build
  local source_prefix
  local source_log
  local closure_file
  local previous_file
  local source_library
  local library
  local dependency
  local dependency_name
  local test_source
  local test_binary
  local source_build_triplet="arm-apple-darwin"
  local source_host_triplet="x86_64-apple-darwin"

  if [ ! -f "$TLS_PACKAGE_MANIFEST" ]; then
    echo "missing pinned TLS package manifest at $TLS_PACKAGE_MANIFEST" >&2
    exit 1
  fi
  if [ ! -f "$TLS_SOURCE_MANIFEST" ]; then
    echo "missing pinned TLS source manifest at $TLS_SOURCE_MANIFEST" >&2
    exit 1
  fi
  manifest_digest="$({ /bin/cat "$TLS_PACKAGE_MANIFEST" "$TLS_SOURCE_MANIFEST"; /usr/bin/printf '%s\n' "$TLS_RUNTIME_LAYOUT_VERSION"; } | short_sha256_stream)"
  if [ "$HOST_DEPENDENCY_ARCH" = "arm64" ]; then
    source_host_triplet="arm-apple-darwin"
  fi
  tls_deps_prefix="$TLS_DEPS_CACHE_DIR/${HOST_DEPENDENCY_ARCH}-gnutls-${manifest_digest}"
  if content_tree_is_verified "$tls_deps_prefix" &&
     [ -f "$tls_deps_prefix/lib/libgnutls.30.dylib" ] &&
     [ -f "$tls_deps_prefix/lib/pkgconfig/gnutls.pc" ] &&
     tls_deps_match_profile_architecture "$tls_deps_prefix" &&
     { [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 0 ] ||
       verify_host_macho_tree_signatures "$tls_deps_prefix" \
         "TLS runtime" >/dev/null 2>&1; }; then
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

    capture_required_output package_archive download_tls_package \
      "$package_name" "$package_filename" "$package_hash" || return $?
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

  while IFS=$'\t' read -r source_name source_version source_filename source_url source_hash extra; do
    case "$source_name" in
      ''|'#'*) continue ;;
    esac
    if [ -n "${extra:-}" ] || [ -z "$source_hash" ]; then
      echo "invalid TLS source manifest row for $source_name" >&2
      exit 1
    fi
    if [ "$source_name" != "libunistring" ]; then
      echo "unsupported TLS source build $source_name" >&2
      exit 1
    fi

    capture_required_output source_archive download_tls_source \
      "$source_name" "$source_filename" "$source_url" "$source_hash" || return $?
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      validate_archive_members "$source_archive" tar
    fi
    source_work="${temporary_prefix}.${source_name}-source"
    source_root="$source_work/$source_name-$source_version"
    source_build="$source_work/build"
    source_prefix="$source_work/prefix"
    source_log="$source_work/build.log"
    rm -rf "$source_work"
    mkdir -p "$source_work" "$source_build" "$source_prefix"
    tar -xzf "$source_archive" -C "$source_work"
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      validate_extracted_tree_links "$source_work"
    fi
    [ -x "$source_root/configure" ] || {
      echo "$source_name source archive has no configure script" >&2
      exit 1
    }

    tls_source_cc="clang -arch $HOST_DEPENDENCY_ARCH"
    tls_source_cxx="clang++ -arch $HOST_DEPENDENCY_ARCH"
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      validate_native_host_toolchain_policy || return 1
      tls_source_cc="$NATIVE_HOST_CLANG $SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG -arch $HOST_DEPENDENCY_ARCH"
      tls_source_cxx="$NATIVE_HOST_CLANGXX $SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG -arch $HOST_DEPENDENCY_ARCH"
    fi
    echo "building source-pinned $HOST_DEPENDENCY_ARCH $source_name $source_version" >&2
    if ! (
      cd "$source_build"
      env \
        CC="$tls_source_cc" \
        CXX="$tls_source_cxx" \
        CFLAGS="-O2 -mmacosx-version-min=$TLS_MIN_MACOS_VERSION ${NATIVE_MACOS_SDK_FLAG:-}" \
        CXXFLAGS="-O2 -mmacosx-version-min=$TLS_MIN_MACOS_VERSION ${NATIVE_MACOS_SDK_FLAG:-}" \
        LDFLAGS="-mmacosx-version-min=$TLS_MIN_MACOS_VERSION ${NATIVE_MACOS_SDK_FLAG:-}" \
        MACOSX_DEPLOYMENT_TARGET="$TLS_MIN_MACOS_VERSION" \
        "$source_root/configure" \
          --build="$source_build_triplet" \
          --host="$source_host_triplet" \
          --prefix="$source_prefix" \
          --disable-static \
          --enable-shared
      make -j"$JOBS"
      make install
    ) >"$source_log" 2>&1; then
      /bin/cat "$source_log" >&2
      exit 1
    fi
    if [ ! -f "$source_prefix/lib/libunistring.2.dylib" ] ||
       { [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] &&
         ! verify_host_macho_tree_arches "$source_prefix" \
           "$source_name source build" "$HOST_DEPENDENCY_ARCH" >/dev/null 2>&1; } ||
       { [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 0 ] &&
         ! file "$source_prefix/lib/libunistring.2.dylib" | grep -q 'x86_64'; }; then
      echo "$source_name source build did not produce the expected $HOST_DEPENDENCY_ARCH library" >&2
      exit 1
    fi
    install -m 0644 "$source_prefix/lib/libunistring.2.dylib" \
      "$package_pool/lib/libunistring.2.dylib"
    install_name_tool -id '@rpath/libunistring.2.dylib' \
      "$package_pool/lib/libunistring.2.dylib"

    package_notice_root="$temporary_prefix/share/doc/switchyard-tls/packages/$source_name"
    mkdir -p "$package_notice_root/licenses"
    install -m 0644 "$source_root/COPYING.LIB" "$package_notice_root/licenses/COPYING.LIB"
    if [ -f "$source_root/COPYING" ]; then
      install -m 0644 "$source_root/COPYING" "$package_notice_root/licenses/COPYING"
    fi
    cat >"$package_notice_root/source.txt" <<EOF
Name: $source_name
Version: $source_version
Source: $source_url
SHA-256: $source_hash
Build: $HOST_DEPENDENCY_ARCH macOS, minimum version $TLS_MIN_MACOS_VERSION
EOF
    printf '%s\t%s\t%s\t%s\t%s\n' \
      "$source_name" "$source_version" "source-xcode" "$source_filename" "$source_hash" \
      >> "$temporary_prefix/share/doc/switchyard-tls/manifest.tsv"
    rm -rf "$source_work"
  done < "$TLS_SOURCE_MANIFEST"

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
Description: Pinned redistributable $HOST_DEPENDENCY_ARCH GnuTLS runtime for Switchyard Wine
Version: 3.8.13
Libs: -L\${libdir} -lgnutls
Cflags: -I\${includedir}
EOF

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    cat >"$temporary_prefix/share/doc/switchyard-tls/README.txt" <<EOF
This directory contains the pinned $HOST_DEPENDENCY_ARCH macOS GnuTLS dependency closure used
by Switchyard Wine for schannel support. Binary packages are downloaded from
the conda-forge $TLS_PACKAGE_SUBDIR channel, while the legacy libunistring ABI
is rebuilt from its pinned GNU source for modern macOS compatibility.
Every input is hash-verified before staging. Package metadata, source identity,
and license notices are preserved for redistribution and source tracing.
EOF
  else
    cat >"$temporary_prefix/share/doc/switchyard-tls/README.txt" <<'EOF'
This directory contains the pinned x86_64 macOS GnuTLS dependency closure used
by Switchyard Wine for schannel support. Binary packages are downloaded from
the conda-forge osx-64 channel, while the legacy libunistring ABI is rebuilt
from its pinned GNU source for modern macOS signing and Rosetta compatibility.
Every input is hash-verified before staging. Package metadata, source identity,
and license notices are preserved for redistribution and source tracing.
EOF
  fi
  install -m 0644 "$TLS_PACKAGE_MANIFEST" \
    "$temporary_prefix/share/doc/switchyard-tls/packages.tsv"
  install -m 0644 "$TLS_SOURCE_MANIFEST" \
    "$temporary_prefix/share/doc/switchyard-tls/sources.tsv"

  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    tls_deps_match_profile_architecture "$temporary_prefix"
    verify_runtime_relative_macho_tree "$temporary_prefix" "TLS runtime"
    adhoc_sign_and_verify_host_macho_tree "$temporary_prefix" "TLS runtime"
  fi

  test_source="$temporary_prefix/gnutls-link-test.c"
  test_binary="$temporary_prefix/gnutls-link-test"
  cat >"$test_source" <<'EOF'
#include <gnutls/gnutls.h>
int main(void) { return gnutls_check_version("3.0") ? 0 : 1; }
EOF
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_native_host_toolchain_policy || return 1
    "${PROFILE_ARCH_COMMAND[@]}" "$NATIVE_HOST_CLANG" \
      "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" -arch "$HOST_MACHO_ARCH" \
      "$NATIVE_MACOS_DEPLOYMENT_FLAG" "$NATIVE_MACOS_SDK_FLAG" \
      -I"$temporary_prefix/include" \
      -L"$temporary_prefix/lib" \
      -Wl,-rpath,"$temporary_prefix/lib" \
      "$test_source" -lgnutls -o "$test_binary"
  else
    "${PROFILE_ARCH_COMMAND[@]}" clang -arch "$HOST_MACHO_ARCH" \
      -mmacosx-version-min="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
      -I"$temporary_prefix/include" \
      -L"$temporary_prefix/lib" \
      -Wl,-rpath,"$temporary_prefix/lib" \
      "$test_source" -lgnutls -o "$test_binary"
  fi
  env DYLD_LIBRARY_PATH="$temporary_prefix/lib" "$test_binary"
  rm -f "$test_source" "$test_binary"

  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$tls_deps_prefix" cache
  printf '%s\n' "$tls_deps_prefix"
}

download_gstreamer_package() {
  local filename="$1"
  local expected_hash="$2"
  local cached_file="$GSTREAMER_PACKAGE_CACHE_DIR/$filename"
  local actual_hash
  local temporary_file

  mkdir -p "$GSTREAMER_PACKAGE_CACHE_DIR"
  if [ -f "$cached_file" ]; then
    actual_hash="$(sha256_file "$cached_file")"
    if [ "$actual_hash" = "$expected_hash" ]; then
      printf '%s\n' "$cached_file"
      return 0
    fi
    echo "cached GStreamer package $filename has unexpected sha256 $actual_hash; downloading again." >&2
    rm -f "$cached_file"
  fi

  temporary_file="${cached_file}.tmp.$$"
  echo "downloading GStreamer $GSTREAMER_VERSION package $filename" >&2
  curl -fL --retry 3 --retry-delay 1 --connect-timeout 20 \
    --proto '=https' --tlsv1.2 \
    -o "$temporary_file" "$GSTREAMER_PACKAGE_BASE_URL/$filename"
  actual_hash="$(sha256_file "$temporary_file")"
  if [ "$actual_hash" != "$expected_hash" ]; then
    rm -f "$temporary_file"
    echo "GStreamer package sha256 mismatch for $filename: expected $expected_hash, got $actual_hash" >&2
    exit 1
  fi
  mv "$temporary_file" "$cached_file"
  printf '%s\n' "$cached_file"
}

gstreamer_plugin_is_selected() {
  local candidate="$1"
  local selected

  for selected in "${GSTREAMER_PLUGIN_FILES[@]}"; do
    if [ "$candidate" = "$selected" ]; then
      return 0
    fi
  done
  return 1
}

verify_gstreamer_runtime() {
  local prefix="$1"
  local smoke_asset="${2:-}"
  local registry_dir
  local registry_path
  local feature

  registry_dir="$(mktemp -d)"
  registry_path="$registry_dir/registry-${SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH}.bin"
  for feature in \
    asfdemux avdec_wmv3 avdec_wmapro audioconvert audioresample \
    decodebin deinterlace videoconvert videoflip; do
    env -i \
      HOME="$HOME" \
      PATH="/usr/bin:/bin" \
      TMPDIR="${TMPDIR:-/tmp}" \
      GST_PLUGIN_SYSTEM_PATH_1_0="$prefix/lib/gstreamer-1.0" \
      GST_PLUGIN_PATH_1_0= \
      GST_PLUGIN_SCANNER_1_0="$prefix/libexec/gstreamer-1.0/gst-plugin-scanner" \
      GST_REGISTRY_1_0="$registry_path" \
      GST_REGISTRY_FORK=no \
      "${PROFILE_ARCH_COMMAND[@]}" "$prefix/bin/gst-inspect-1.0" "$feature" >/dev/null
  done

  if [ -n "$smoke_asset" ]; then
    if [ ! -f "$smoke_asset" ]; then
      echo "GStreamer media smoke asset does not exist: $smoke_asset" >&2
      rm -rf "$registry_dir"
      return 1
    fi
    echo "decoding the complete WMV3 video stream from $smoke_asset" >&2
    env -i \
      HOME="$HOME" \
      PATH="/usr/bin:/bin" \
      TMPDIR="${TMPDIR:-/tmp}" \
      GST_PLUGIN_SYSTEM_PATH_1_0="$prefix/lib/gstreamer-1.0" \
      GST_PLUGIN_PATH_1_0= \
      GST_PLUGIN_SCANNER_1_0="$prefix/libexec/gstreamer-1.0/gst-plugin-scanner" \
      GST_REGISTRY_1_0="$registry_path" \
      GST_REGISTRY_FORK=no \
      "${PROFILE_ARCH_COMMAND[@]}" "$prefix/bin/gst-launch-1.0" -q \
        filesrc location="$smoke_asset" ! asfdemux name=demux \
        demux.video_0 ! queue ! avdec_wmv3 ! videoconvert ! fakesink sync=false
  fi
  rm -rf "$registry_dir"
}

gstreamer_deps_are_complete() {
  local prefix="$1"
  local plugin

  content_tree_is_verified "$prefix" || return 1
  [ -x "$prefix/bin/gst-inspect-1.0" ] || return 1
  [ -x "$prefix/bin/gst-launch-1.0" ] || return 1
  [ -x "$prefix/libexec/gstreamer-1.0/gst-plugin-scanner" ] || return 1
  [ -f "$prefix/include/gstreamer-1.0/gst/gst.h" ] || return 1
  [ -f "$prefix/lib/pkgconfig/gstreamer-1.0.pc" ] || return 1
  grep -F 'prefix=${pcfiledir}/../..' \
    "$prefix/lib/pkgconfig/gstreamer-1.0.pc" >/dev/null || return 1
  if grep -R -I -q -E '^prefix=/Users/' "$prefix/lib/pkgconfig" 2>/dev/null; then
    return 1
  fi
  if grep -R -I -q -E '/Users/[^/]+' "$prefix" 2>/dev/null; then
    return 1
  fi
  [ -f "$prefix/share/doc/switchyard-gstreamer/INSTALLER-LICENSE.txt" ] || return 1
  [ -f "$prefix/share/doc/switchyard-gstreamer/packages.tsv" ] || return 1
  [ "$(tr -d '[:space:]' < "$prefix/share/doc/switchyard-gstreamer/VERSION")" = "$GSTREAMER_VERSION" ] || return 1
  gstreamer_deps_match_profile_architecture "$prefix" || return 1
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    verify_host_macho_tree_signatures "$prefix" \
      "GStreamer runtime" >/dev/null 2>&1 || return 1
  fi
  for plugin in "${GSTREAMER_PLUGIN_FILES[@]}"; do
    [ -f "$prefix/lib/gstreamer-1.0/$plugin" ] || return 1
  done
}

stage_gstreamer_deps() {
  local runtime_package
  local devel_package
  local extraction_root
  local runtime_expanded
  local devel_expanded
  local temporary_prefix
  local component
  local payload
  local plugin_path
  local plugin_name
  local devel_base
  local devel_core

  if gstreamer_deps_are_complete "$GSTREAMER_DEPS_PREFIX"; then
    printf '%s\n' "$GSTREAMER_DEPS_PREFIX"
    return 0
  fi

  capture_required_output runtime_package download_gstreamer_package \
    "$GSTREAMER_RUNTIME_PACKAGE" "$GSTREAMER_RUNTIME_PACKAGE_SHA256" || return $?
  capture_required_output devel_package download_gstreamer_package \
    "$GSTREAMER_DEVEL_PACKAGE" "$GSTREAMER_DEVEL_PACKAGE_SHA256" || return $?
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_xar_members "$runtime_package"
    validate_xar_members "$devel_package"
  fi
  extraction_root="$(mktemp -d)"
  runtime_expanded="$extraction_root/runtime"
  devel_expanded="$extraction_root/devel"
  echo "extracting pinned GStreamer runtime and development packages" >&2
  pkgutil --expand-full "$runtime_package" "$runtime_expanded"
  pkgutil --expand-full "$devel_package" "$devel_expanded"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_extracted_tree_links "$runtime_expanded"
    validate_extracted_tree_links "$devel_expanded"
  fi

  mkdir -p "$(dirname "$GSTREAMER_DEPS_PREFIX")"
  temporary_prefix="${GSTREAMER_DEPS_PREFIX}.tmp.$$"
  rm -rf "$temporary_prefix"
  mkdir -p "$temporary_prefix"

  for component in "${GSTREAMER_RUNTIME_COMPONENTS[@]}"; do
    payload="$runtime_expanded/$component/Payload"
    if [ ! -d "$payload" ]; then
      echo "GStreamer runtime package is missing component $component." >&2
      exit 1
    fi
    ditto "$payload" "$temporary_prefix"
  done
  # Upstream developer helpers contain Cerbero builder paths and are not used
  # by Wine. Fontconfig is supplied by Switchyard's separate font runtime.
  rm -rf "$temporary_prefix/etc/fonts"
  rm -f "$temporary_prefix/share/gstreamer/gst-env"

  for plugin_path in "$temporary_prefix"/lib/gstreamer-1.0/*; do
    [ -e "$plugin_path" ] || continue
    plugin_name="$(basename "$plugin_path")"
    if ! gstreamer_plugin_is_selected "$plugin_name"; then
      rm -rf "$plugin_path"
    fi
  done

  devel_base="$devel_expanded/base-system-1.0-devel-${GSTREAMER_VERSION}-universal.pkg/Payload"
  devel_core="$devel_expanded/gstreamer-1.0-core-devel-${GSTREAMER_VERSION}-universal.pkg/Payload"
  for payload in "$devel_base" "$devel_core"; do
    if [ ! -d "$payload/include" ] || [ ! -d "$payload/lib/pkgconfig" ]; then
      echo "GStreamer development package is missing headers or pkg-config metadata under $payload." >&2
      exit 1
    fi
    ditto "$payload/include" "$temporary_prefix/include"
    ditto "$payload/lib/pkgconfig" "$temporary_prefix/lib/pkgconfig"
  done
  ditto "$devel_base/lib/glib-2.0/include" "$temporary_prefix/lib/glib-2.0/include"
  ditto "$devel_core/lib/gstreamer-1.0/include" "$temporary_prefix/lib/gstreamer-1.0/include"
  for payload in "$temporary_prefix"/lib/pkgconfig/*.pc; do
    [ -f "$payload" ] || continue
    perl -0pi -e 's#^prefix=.*$#prefix=\${pcfiledir}/../..#m' "$payload"
  done

  mkdir -p "$temporary_prefix/share/licenses" \
    "$temporary_prefix/share/doc/switchyard-gstreamer"
  for component in "${GSTREAMER_DEVEL_COMPONENTS[@]}"; do
    payload="$devel_expanded/$component/Payload"
    if [ ! -d "$payload" ]; then
      echo "GStreamer development package is missing component $component." >&2
      exit 1
    fi
    if [ -d "$payload/share/licenses" ]; then
      ditto "$payload/share/licenses" "$temporary_prefix/share/licenses"
    fi
  done
  install -m 0644 "$runtime_expanded/Resources/license.txt" \
    "$temporary_prefix/share/doc/switchyard-gstreamer/INSTALLER-LICENSE.txt"
  printf '%s\n' "$GSTREAMER_VERSION" \
    > "$temporary_prefix/share/doc/switchyard-gstreamer/VERSION"
  {
    printf 'kind\tfile\tsha256\turl\n'
    printf 'runtime\t%s\t%s\t%s/%s\n' \
      "$GSTREAMER_RUNTIME_PACKAGE" "$GSTREAMER_RUNTIME_PACKAGE_SHA256" \
      "$GSTREAMER_PACKAGE_BASE_URL" "$GSTREAMER_RUNTIME_PACKAGE"
    printf 'development\t%s\t%s\t%s/%s\n' \
      "$GSTREAMER_DEVEL_PACKAGE" "$GSTREAMER_DEVEL_PACKAGE_SHA256" \
      "$GSTREAMER_PACKAGE_BASE_URL" "$GSTREAMER_DEVEL_PACKAGE"
  } > "$temporary_prefix/share/doc/switchyard-gstreamer/packages.tsv"
  {
    printf 'Switchyard GStreamer media runtime\n\n'
    printf 'Version: %s\n' "$GSTREAMER_VERSION"
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      printf 'Architecture: universal x86_64/arm64 package; native Wine consumes the arm64 slices\n'
    else
      printf 'Architecture: universal package; Wine consumes the x86_64 slices under Rosetta\n'
    fi
    printf 'Selected plugins:\n'
    printf '  %s\n' "${GSTREAMER_PLUGIN_FILES[@]}"
    printf '\nThe runtime is restricted to the plugins needed by Wine Media Foundation.\n'
    printf 'It provides ASF demuxing and libav WMV3/WMA Pro decoding without using a\n'
    printf 'host GStreamer installation. Preserve packages.tsv, INSTALLER-LICENSE.txt,\n'
    printf 'and share/licenses when redistributing the runtime.\n'
  } > "$temporary_prefix/share/doc/switchyard-gstreamer/README.txt"

  chmod -R u+rwX "$temporary_prefix"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_extracted_tree_links "$temporary_prefix"
    gstreamer_deps_match_profile_architecture "$temporary_prefix"
    verify_runtime_relative_macho_tree "$temporary_prefix" "GStreamer runtime"
    adhoc_sign_and_verify_host_macho_tree "$temporary_prefix" \
      "GStreamer runtime"
  fi
  verify_gstreamer_runtime "$temporary_prefix"
  write_content_tree_digest "$temporary_prefix"
  atomic_replace_directory "$temporary_prefix" "$GSTREAMER_DEPS_PREFIX" cache
  rm -rf "$extraction_root"
  printf '%s\n' "$GSTREAMER_DEPS_PREFIX"
}

relocate_winegstreamer_for_runtime() {
  local runtime_root="$1"
  local build_prefix="$2"
  local runtime_rpath="@loader_path/../../switchyard-gstreamer/lib"
  local module
  local module_count=0
  local rpath

  while IFS= read -r -d '' module; do
    module_count=$((module_count + 1))
    while IFS= read -r rpath; do
      case "$rpath" in
        "$build_prefix"/*)
          install_name_tool -delete_rpath "$rpath" "$module"
          ;;
      esac
    done < <(otool -l "$module" |
      awk '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')
    if ! otool -l "$module" | grep -F "$runtime_rpath" >/dev/null 2>&1; then
      install_name_tool -add_rpath "$runtime_rpath" "$module"
    fi
    if otool -l "$module" | grep -F "$build_prefix" >/dev/null 2>&1; then
      echo "Wine GStreamer backend retains a build-cache rpath: $module" >&2
      return 1
    fi
    if ! otool -L "$module" | grep -F '@rpath/libgstreamer-1.0.0.dylib' >/dev/null 2>&1; then
      echo "Wine GStreamer backend does not link the staged GStreamer runtime: $module" >&2
      return 1
    fi
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      adhoc_sign_and_verify_host_macho_file "$module" \
        "relocated Wine GStreamer backend" || return 1
    fi
  done < <(find "$runtime_root/lib/wine" -type f -path '*-unix/winegstreamer.so' -print0)

  if [ "$module_count" -eq 0 ]; then
    echo "Wine was built without its Unix GStreamer backend." >&2
    return 1
  fi
}

verify_exact_arm64_macos_metadata() {
  local candidate="$1"
  local label="$2"
  local expected_version="$3"
  local metadata

  metadata="$(vtool -arch arm64 -show-build "$candidate" 2>/dev/null)" || {
    echo "$label has unreadable macOS build metadata: $candidate" >&2
    return 1
  }
  SWITCHYARD_MACHO_BUILD_METADATA="$metadata" /usr/bin/python3 -I - \
      "$expected_version" "$candidate" <<'PY'
import os
import re
import sys

expected, path = sys.argv[1:]
lines = os.environ["SWITCHYARD_MACHO_BUILD_METADATA"].splitlines()
if sum(line.split()[:2] == ["cmd", "LC_BUILD_VERSION"] for line in lines) != 1:
    raise SystemExit(path + ": expected one LC_BUILD_VERSION")
if any(line.split()[:2] == ["cmd", "LC_VERSION_MIN_MACOSX"] for line in lines):
    raise SystemExit(path + ": legacy deployment metadata is not accepted")
fields = {}
for line in lines:
    parts = line.split()
    if len(parts) == 2 and parts[0] in ("platform", "minos", "sdk"):
        if parts[0] in fields:
            raise SystemExit(path + ": duplicate build metadata field " + parts[0])
        fields[parts[0]] = parts[1]

def version(value):
    if re.fullmatch(r"[0-9]+(?:[.][0-9]+){0,2}", value or "") is None:
        raise SystemExit(path + ": malformed macOS version")
    pieces = [int(piece) for piece in value.split(".")]
    return tuple((pieces + [0, 0])[:3])

if fields.get("platform") != "MACOS":
    raise SystemExit(path + ": Mach-O platform is not macOS")
if version(fields.get("minos")) != version(expected):
    raise SystemExit(path + ": minimum macOS does not match " + expected)
if version(fields.get("sdk")) != version(expected):
    raise SystemExit(path + ": SDK does not match " + expected)
PY
}

validate_staged_unicorn_runtime() {
  local runtime_root="$1"
  local package_root="$runtime_root/$UNICORN_PACKAGE_ROOT_RELATIVE"
  local metadata="$package_root/switchyard-unicorn-runtime.json"
  local dylib="$runtime_root/$UNICORN_DYLIB_RELATIVE"
  local dylib_link="$package_root/lib/libunicorn.dylib"
  local source_archive="$package_root/share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz"
  local source_patch="$runtime_root/$UNICORN_SOURCE_PATCH_RELATIVE"
  local notice_root="$package_root/share/doc/switchyard-unicorn"
  local actual dependency notice

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || return 1
  [ -d "$package_root" ] && [ ! -L "$package_root" ] || return 1
  validate_extracted_tree_links "$package_root" || return 1
  runtime_content_tree_is_verified "$package_root" || return 1
  [ "$(runtime_content_tree_digest "$package_root")" = \
    "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] || return 1
  [ -f "$metadata" ] && [ ! -L "$metadata" ] || return 1
  [ -f "$dylib" ] && [ ! -L "$dylib" ] || return 1
  [ -L "$dylib_link" ] && [ "$(readlink "$dylib_link")" = "libunicorn.2.dylib" ] || return 1
  [ -f "$source_archive" ] && [ ! -L "$source_archive" ] || return 1
  [ -f "$source_patch" ] && [ ! -L "$source_patch" ] || return 1
  [ -z "$(find "$package_root" -type f \( -name '*.a' -o -name '*.o' \) -print -quit)" ] || return 1
  [ ! -e "$package_root/include" ] && [ ! -L "$package_root/include" ] || return 1
  [ ! -e "$package_root/lib/pkgconfig" ] && [ ! -L "$package_root/lib/pkgconfig" ] || return 1
  /usr/bin/python3 -I - "$package_root" "$SWITCHYARD_UNICORN_SOURCE_REVISION" \
      "$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" <<'PY' || return 1
import os
import stat
import sys

root, revision, patch_basename = sys.argv[1:]
allowed = {
    ".switchyard-content-sha256",
    "lib/libunicorn.2.dylib",
    "lib/libunicorn.dylib",
    "share/doc/switchyard-unicorn/README.txt",
    "share/doc/switchyard-unicorn/CORRESPONDING-SOURCE.txt",
    "share/doc/switchyard-unicorn/COPYING",
    "share/doc/switchyard-unicorn/COPYING.LGPL2",
    "share/doc/switchyard-unicorn/COPYING_GLIB",
    "share/doc/switchyard-unicorn/QEMU-COPYING",
    "share/doc/switchyard-unicorn/QEMU-COPYING.LIB",
    "share/doc/switchyard-unicorn/QEMU-LICENSE",
    "share/src/switchyard-unicorn/unicorn-" + revision + ".tar.gz",
    "share/src/switchyard-unicorn/" + patch_basename,
    "switchyard-unicorn-runtime.json",
}
seen = set()
for directory, directories, files in os.walk(root, followlinks=False):
    for name in files + [name for name in directories if os.path.islink(os.path.join(directory, name))]:
        path = os.path.join(directory, name)
        relative = os.path.relpath(path, root).replace(os.sep, "/")
        info = os.lstat(path)
        if not (stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode)):
            raise SystemExit("unsupported Unicorn payload entry: " + relative)
        if relative not in allowed:
            raise SystemExit("unexpected Unicorn payload entry: " + relative)
        seen.add(relative)
if seen != allowed:
    raise SystemExit("Unicorn payload file set is incomplete")
PY
  /usr/bin/python3 -I - "$metadata" "$SWITCHYARD_UNICORN_VERSION" \
      "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" "$SWITCHYARD_UNICORN_SOURCE_REVISION" \
      "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" \
      "$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" \
      "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" \
      "$SWITCHYARD_UNICORN_LIBRARY_SHA256" \
      "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" \
      "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" <<'PY' || return 1
import json
import os
import sys

(
    metadata,
    version,
    repository,
    revision,
    archive_sha,
    patch_basename,
    patch_sha,
    library_sha,
    contract,
    minimum,
) = sys.argv[1:]

def no_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate Unicorn manifest field")
        value[key] = item
    return value

if os.path.getsize(metadata) > 1024 * 1024:
    raise ValueError("Unicorn manifest exceeds its size bound")
with open(metadata, "r", encoding="utf-8") as stream:
    value = json.load(stream, object_pairs_hook=no_duplicates)
expected = {
    "version": version,
    "sourceRepository": repository,
    "sourceRevision": revision,
    "buildContractVersion": int(contract),
    "enabledArchitectures": ["x86"],
    "hostArchitecture": "arm64",
    "minimumMacOS": minimum,
    "library": "lib/libunicorn.2.dylib",
    "sourceArchive": "share/src/switchyard-unicorn/unicorn-" + revision + ".tar.gz",
    "sourceArchiveSha256": archive_sha,
    "sourcePatch": {
        "path": "share/src/switchyard-unicorn/" + patch_basename,
        "sha256": patch_sha,
    },
    "license": "GPL-2.0-only with separately licensed GLib/QEMU components; preserve all included notices and corresponding source",
}
if type(value) is not dict or set(value) != set(expected) | {"librarySha256"}:
    raise ValueError("Unicorn manifest has an unexpected field set")
for key, wanted in expected.items():
    if type(value.get(key)) is not type(wanted) or value.get(key) != wanted:
        raise ValueError("Unicorn manifest field is invalid: " + key)
if value.get("librarySha256") != library_sha:
    raise ValueError("Unicorn library digest is invalid")
PY
  actual="$(/usr/bin/plutil -extract librarySha256 raw -o - "$metadata" 2>/dev/null || true)"
  [ "$actual" = "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ] || return 1
  [ "$(sha256_file "$dylib")" = "$SWITCHYARD_UNICORN_LIBRARY_SHA256" ] || return 1
  [ "$(sha256_file "$source_archive")" = "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" ] || return 1
  [ "$(sha256_file "$source_patch")" = "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] || return 1
  /usr/bin/cmp -s "$source_patch" \
    "$ROOT_DIR/switchyard/patches/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" || return 1
  validate_archive_members "$source_archive" tar || return 1
  [ "$(/usr/bin/gzip -dc "$source_archive" | git get-tar-commit-id 2>/dev/null || true)" = \
    "$SWITCHYARD_UNICORN_SOURCE_REVISION" ] || return 1
  [ "$(lipo -archs "$dylib")" = "arm64" ] || return 1
  /usr/bin/codesign --verify --strict --verbose=2 \
    "$dylib" >/dev/null 2>&1 || return 1
  [ "$(otool -D "$dylib" | /usr/bin/tail -n 1)" = "@rpath/libunicorn.2.dylib" ] || return 1
  verify_exact_arm64_macos_metadata "$dylib" "Unicorn runtime" \
    "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" || return 1
  while IFS= read -r dependency; do
    case "$dependency" in
      @rpath/libunicorn.2.dylib|/usr/lib/*|/System/Library/*) ;;
      *) return 1 ;;
    esac
  done < <(otool -L "$dylib" | /usr/bin/awk 'NR > 1 { print $1 }')
  [ -z "$(otool -l "$dylib" | /usr/bin/awk \
    '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')" ] || return 1
  for notice in README.txt CORRESPONDING-SOURCE.txt COPYING COPYING.LGPL2 COPYING_GLIB \
      QEMU-COPYING QEMU-COPYING.LIB QEMU-LICENSE; do
    [ -s "$notice_root/$notice" ] && [ ! -L "$notice_root/$notice" ] || return 1
  done
}

validate_staged_unicorn_providers() {
  local runtime_root="$1"
  local index relative unix_library pe_library pe_description dependency rpath
  local unicorn_dependency_count ntdll_dependency_count
  local loader_rpath_count runtime_rpath_count

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || return 1
  [ "${#UNICORN_PROVIDER_UNIXLIBS[@]}" -eq "${#UNICORN_PROVIDER_PE_LIBS[@]}" ] &&
    [ "${#UNICORN_PROVIDER_UNIXLIBS[@]}" -eq "${#UNICORN_PROVIDER_GUEST_ARCHS[@]}" ] || return 1
  for index in "${!UNICORN_PROVIDER_UNIXLIBS[@]}"; do
    relative="${UNICORN_PROVIDER_UNIXLIBS[$index]}"
    unix_library="$runtime_root/$relative"
    pe_library="$runtime_root/${UNICORN_PROVIDER_PE_LIBS[$index]}"
    [ -f "$unix_library" ] && [ ! -L "$unix_library" ] || return 1
    [ -f "$pe_library" ] && [ ! -L "$pe_library" ] || return 1
    pe_description="$(file -b "$pe_library")" || return 1
    case "${UNICORN_PROVIDER_GUEST_ARCHS[$index]}:$pe_description" in
      i386:*PE32+*Aarch64*|x86_64:*PE32+*x86-64*) ;;
      *) return 1 ;;
    esac
    [ "$(lipo -archs "$unix_library")" = "arm64" ] || return 1
    /usr/bin/codesign --verify --strict --verbose=2 \
      "$unix_library" >/dev/null 2>&1 || return 1
    verify_exact_arm64_macos_metadata "$unix_library" \
      "native Unicorn provider" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" || return 1
    [ "$(otool -D "$unix_library" | /usr/bin/tail -n 1)" = \
      "@rpath/$(basename "$unix_library")" ] || return 1
    unicorn_dependency_count=0
    ntdll_dependency_count=0
    while IFS= read -r dependency; do
      case "$dependency" in
        @rpath/libunicorn.2.dylib)
          unicorn_dependency_count=$((unicorn_dependency_count + 1))
          ;;
        @rpath/ntdll.so)
          ntdll_dependency_count=$((ntdll_dependency_count + 1))
          ;;
        "@rpath/$(basename "$unix_library")"|/usr/lib/*|/System/Library/*)
          ;;
        *) return 1 ;;
      esac
    done < <(otool -L "$unix_library" | /usr/bin/awk 'NR > 1 { print $1 }')
    [ "$unicorn_dependency_count" -eq 1 ] || return 1
    [ "$ntdll_dependency_count" -eq 1 ] || return 1
    loader_rpath_count=0
    runtime_rpath_count=0
    while IFS= read -r rpath; do
      case "$rpath" in
        "$UNICORN_RUNTIME_RPATH")
          runtime_rpath_count=$((runtime_rpath_count + 1))
          ;;
        @loader_path/)
          loader_rpath_count=$((loader_rpath_count + 1))
          ;;
        *) return 1 ;;
      esac
    done < <(otool -l "$unix_library" | /usr/bin/awk \
      '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')
    [ "$loader_rpath_count" -eq 1 ] || return 1
    [ "$runtime_rpath_count" -eq 1 ] || return 1
  done
}

stage_unicorn_runtime() {
  local runtime_root="$1"
  local provider_prefix="$2"
  local package_root="$runtime_root/$UNICORN_PACKAGE_ROOT_RELATIVE"
  local source_notice_root="$provider_prefix/share/doc/switchyard-unicorn"
  local source_archive="$provider_prefix/share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz"
  local source_patch="$provider_prefix/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"
  local destination_notice_root="$package_root/share/doc/switchyard-unicorn"
  local destination_source_root="$package_root/share/src/switchyard-unicorn"
  local relative unix_library dependency rpath
  local unicorn_dependency_count loader_rpath_count runtime_rpath_count notice

  [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] || return 1
  runtime_content_tree_is_verified "$provider_prefix" || {
    echo "Pinned Unicorn development cache failed content verification." >&2
    return 1
  }
  [ ! -e "$package_root" ] && [ ! -L "$package_root" ] || {
    echo "Wine install unexpectedly contains the Unicorn runtime path." >&2
    return 1
  }
  [ -f "$provider_prefix/lib/libunicorn.2.dylib" ] && \
    [ ! -L "$provider_prefix/lib/libunicorn.2.dylib" ] || return 1
  [ -f "$provider_prefix/switchyard-unicorn-runtime.json" ] && \
    [ ! -L "$provider_prefix/switchyard-unicorn-runtime.json" ] || return 1
  [ -f "$source_archive" ] && [ ! -L "$source_archive" ] || return 1
  [ -f "$source_patch" ] && [ ! -L "$source_patch" ] || return 1
  [ "$(sha256_file "$source_patch")" = "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] || return 1

  mkdir -p "$package_root/lib" "$destination_notice_root" "$destination_source_root"
  chmod 0755 "$package_root" "$package_root/lib" "$package_root/share" \
    "$package_root/share/doc" "$destination_notice_root" "$package_root/share/src" \
    "$destination_source_root"
  install -m 0755 "$provider_prefix/lib/libunicorn.2.dylib" \
    "$runtime_root/$UNICORN_DYLIB_RELATIVE"
  ln -s libunicorn.2.dylib "$package_root/lib/libunicorn.dylib"
  install -m 0644 "$provider_prefix/switchyard-unicorn-runtime.json" \
    "$package_root/switchyard-unicorn-runtime.json"
  for notice in README.txt CORRESPONDING-SOURCE.txt COPYING COPYING.LGPL2 COPYING_GLIB \
      QEMU-COPYING QEMU-COPYING.LIB QEMU-LICENSE; do
    [ -f "$source_notice_root/$notice" ] && [ ! -L "$source_notice_root/$notice" ] || {
      echo "Pinned Unicorn development cache is missing $notice." >&2
      return 1
    }
    install -m 0644 "$source_notice_root/$notice" "$destination_notice_root/$notice"
  done
  install -m 0644 "$source_archive" "$destination_source_root/$(basename "$source_archive")"
  install -m 0644 "$source_patch" "$destination_source_root/$(basename "$source_patch")"
  write_runtime_content_tree_digest "$package_root" >/dev/null

  for relative in "${UNICORN_PROVIDER_UNIXLIBS[@]}"; do
    unix_library="$runtime_root/$relative"
    [ -f "$unix_library" ] && [ ! -L "$unix_library" ] || {
      echo "Wine install is missing native Unicorn provider $relative." >&2
      return 1
    }
    unicorn_dependency_count=0
    while IFS= read -r dependency; do
      case "$dependency" in
        */libunicorn.2.dylib|*/libunicorn.dylib)
          unicorn_dependency_count=$((unicorn_dependency_count + 1))
          if [ "$dependency" != "@rpath/libunicorn.2.dylib" ]; then
            install_name_tool -change "$dependency" "@rpath/libunicorn.2.dylib" "$unix_library"
          fi
          ;;
      esac
    done < <(otool -L "$unix_library" | /usr/bin/awk 'NR > 1 { print $1 }')
    [ "$unicorn_dependency_count" -eq 1 ] || {
      echo "Native provider has an ambiguous Unicorn dependency: $relative" >&2
      return 1
    }
    loader_rpath_count=0
    runtime_rpath_count=0
    while IFS= read -r rpath; do
      case "$rpath" in
        "$UNICORN_RUNTIME_RPATH")
          runtime_rpath_count=$((runtime_rpath_count + 1))
          ;;
        @loader_path/)
          loader_rpath_count=$((loader_rpath_count + 1))
          ;;
        @loader_path)
          install_name_tool -delete_rpath "$rpath" "$unix_library"
          ;;
        *)
          install_name_tool -delete_rpath "$rpath" "$unix_library"
          ;;
      esac
    done < <(otool -l "$unix_library" | /usr/bin/awk \
      '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')
    if [ "$loader_rpath_count" -eq 0 ]; then
      install_name_tool -add_rpath '@loader_path/' "$unix_library"
    elif [ "$loader_rpath_count" -ne 1 ]; then
      echo "Native provider has duplicate ntdll loader rpaths: $relative" >&2
      return 1
    fi
    if [ "$runtime_rpath_count" -eq 0 ]; then
      install_name_tool -add_rpath "$UNICORN_RUNTIME_RPATH" "$unix_library"
    elif [ "$runtime_rpath_count" -ne 1 ]; then
      echo "Native provider has duplicate Unicorn runtime rpaths: $relative" >&2
      return 1
    fi
  done

  for relative in "${UNICORN_PROVIDER_UNIXLIBS[@]}"; do
    adhoc_sign_and_verify_host_macho_file "$runtime_root/$relative" \
      "relocated native Unicorn provider" || return 1
  done

  validate_staged_unicorn_runtime "$runtime_root" || {
    echo "Staged Unicorn runtime payload failed validation." >&2
    return 1
  }
  validate_staged_unicorn_providers "$runtime_root" || {
    echo "Staged native Unicorn providers failed validation." >&2
    return 1
  }
}

if [ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA" = "true" ] &&
   ! "${PROFILE_ARCH_COMMAND[@]}" /usr/bin/true >/dev/null 2>&1; then
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
build_source_fingerprint="$(switchyard_source_state_fingerprint "$WINE_DIR")"
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
  WINE_BUILD_DIR="${HOME}/Library/Caches/Switchyard/Wine/${SWITCHYARD_RUNTIME_PROFILE_BUILD_CACHE_BASENAME}-${source_identity}"
fi
assert_source_state_unchanged "source metadata capture"
if [ "$MODE" = "--source-info" ]; then
  echo "sourceRepository=$SOURCE_REPOSITORY"
  echo "sourceRevision=$wine_revision"
  echo "upstreamWineRevision=$upstream_wine_revision"
  echo "sourceDirty=$source_dirty"
  echo "sourceTreeDigest=$source_digest"
  echo "patchsetID=$patchset_id"
  echo "gptkOverlayDisabled=$DISABLE_GPTK_OVERLAY"
  echo "runtimeProfile=$SWITCHYARD_RUNTIME_PROFILE"
  echo "hostMachOArchitecture=$HOST_MACHO_ARCH"
  echo "wineUnixArchitecture=$WINE_UNIX_ARCH"
  echo "peArchitectures=$SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV"
  echo "wineBuildTriplet=$WINE_BUILD_TRIPLET"
  echo "wineHostTriplet=$WINE_HOST_TRIPLET"
  echo "requiresRosetta=$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA"
  echo "minimumMacOS=$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
  echo "gstreamerRegistryArchitecture=$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    echo "kuserSharedDataModel=$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL"
  fi
  exit 0
fi

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  DXMT_WOW64_COMPANION_ABI_SCHEMA_SHA256="$(
    switchyard_validate_dxmt_wow64_companion_source "$ROOT_DIR"
  )" || exit $?
fi

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] &&
   [ -n "$USER_SET_WINE_INSTALL_PREFIX" ]; then
  WINE_INSTALL_PREFIX="$(
    switchyard_canonical_runtime_install_prefix "$WINE_INSTALL_PREFIX"
  )" || exit $?
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
    "$WINE_INSTALL_PREFIX" || exit $?
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

wine_mono_path=""
gstreamer_deps_prefix=""
vulkan_deps_prefix=""
mesa_windows_prefix=""
font_deps_prefix=""
font_assets_prefix=""
tls_deps_prefix=""

if [ "$MODE" = "--verify-tls" ]; then
  capture_required_output tls_deps_prefix stage_tls_deps || exit $?
  echo "verified pinned $HOST_DEPENDENCY_ARCH TLS runtime at $tls_deps_prefix"
  echo "tlsRuntimeDigest=$(content_tree_digest "$tls_deps_prefix")"
  exit 0
fi

if [ "$MODE" = "--verify-media" ]; then
  capture_required_output gstreamer_deps_prefix stage_gstreamer_deps || exit $?
  verify_gstreamer_runtime "$gstreamer_deps_prefix" "${SWITCHYARD_MEDIA_SMOKE_ASSET:-}"
  echo "verified pinned $SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION GStreamer media runtime at $gstreamer_deps_prefix"
  echo "gstreamerRuntimeDigest=$(content_tree_digest "$gstreamer_deps_prefix")"
  exit 0
fi

if [ "$MODE" = "--verify-mesa" ]; then
  capture_required_output mesa_windows_prefix stage_mesa_windows_opengl || exit $?
  echo "verified pinned i386/x86_64 Mesa Windows OpenGL runtime at $mesa_windows_prefix"
  echo "mesaRuntimeDigest=$(content_tree_digest "$mesa_windows_prefix")"
  exit 0
fi

capture_required_output wine_mono_path download_wine_mono || exit $?
wine_mono_digest="$(sha256_file "$wine_mono_path")"
capture_required_output gstreamer_deps_prefix stage_gstreamer_deps || exit $?
gstreamer_deps_digest="$(content_tree_digest "$gstreamer_deps_prefix")"
capture_required_output vulkan_deps_prefix stage_vulkan_deps || exit $?
vulkan_deps_digest="$(content_tree_digest "$vulkan_deps_prefix")"
capture_required_output mesa_windows_prefix stage_mesa_windows_opengl || exit $?
mesa_windows_digest="$(content_tree_digest "$mesa_windows_prefix")"
capture_required_output font_deps_prefix stage_font_deps || exit $?
capture_required_output font_assets_prefix stage_font_assets || exit $?
font_assets_digest="$(content_tree_digest "$font_assets_prefix")"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  capture_required_output FONT_RUNTIME_PREPARED_ROOT \
    prepare_font_runtime_for_install \
    "$font_deps_prefix" "$font_assets_prefix" || exit $?
  font_deps_digest="$(content_tree_digest "$FONT_RUNTIME_PREPARED_ROOT")"
else
  font_deps_digest="$(content_tree_digest "$font_deps_prefix")"
fi
font_asset_count="$(awk -F '\t' '$1 == "font" { count++ } END { print count + 2 }' "$FONT_ASSET_MANIFEST")"
capture_required_output tls_deps_prefix stage_tls_deps || exit $?
if [ -n "$tls_deps_prefix" ]; then
  tls_deps_digest="$(content_tree_digest "$tls_deps_prefix")"
  tls_dlopen_digest="$(printf '%s' "$TLS_DLOPEN_NAME" | short_sha256_stream)"
  tls_dlopen_closure_digest="$(
    printf '%s' "$TLS_DLOPEN_NAME" | shasum -a 256 | awk '{print $1}'
  )"
else
  tls_deps_digest="none"
  tls_dlopen_digest="none"
  tls_dlopen_closure_digest="none"
fi

unicorn_runtime_prefix=""
unicorn_runtime_digest=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  echo "building or validating pinned Unicorn $SWITCHYARD_UNICORN_VERSION for native ARM64"
  "$ROOT_DIR/switchyard/build_unicorn_runtime.sh" \
    --output "$UNICORN_RUNTIME_PREFIX" \
    --minimum-macos "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" >/dev/null
  unicorn_runtime_prefix="$UNICORN_RUNTIME_PREFIX"
  runtime_content_tree_is_verified "$unicorn_runtime_prefix" || {
    echo "Pinned Unicorn development cache failed content verification." >&2
    exit 1
  }
  unicorn_runtime_digest="$(runtime_content_tree_digest "$unicorn_runtime_prefix")"
  [ "$unicorn_runtime_digest" = "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" ] || {
    echo "Pinned Unicorn development cache has an unexpected closed-policy digest." >&2
    exit 1
  }
fi

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  gstreamer_closure_digest="$(runtime_content_tree_digest "$gstreamer_deps_prefix")"
  vulkan_closure_digest="$(runtime_content_tree_digest "$vulkan_deps_prefix")"
  mesa_closure_digest="$(runtime_content_tree_digest "$mesa_windows_prefix")"
  font_closure_digest="$(runtime_content_tree_digest "$FONT_RUNTIME_PREPARED_ROOT")"
  font_assets_closure_digest="$(runtime_content_tree_digest "$font_assets_prefix")"
  if [ -n "$tls_deps_prefix" ]; then
    tls_closure_digest="$(runtime_content_tree_digest "$tls_deps_prefix")"
  else
    tls_closure_digest="none"
  fi
  runtime_closure_digest="$(
    switchyard_native_runtime_closure_digest \
      "$source_identity" "$wine_revision" "$source_dirty" "$build_source_fingerprint" \
      "$gptk_redist_digest" "$wine_mono_digest" "$gstreamer_closure_digest" \
      "$vulkan_closure_digest" "$mesa_closure_digest" "$font_closure_digest" \
      "$font_assets_closure_digest" "$tls_closure_digest" "$TLS_DLOPEN_NAME" \
      "$tls_dlopen_closure_digest" "$unicorn_runtime_digest" \
      "$SWITCHYARD_DXMT_ARTIFACT_SHA256" "$SWITCHYARD_DXMT_SOURCE_PATCH_SHA256" \
      "$SWITCHYARD_DXMT_WINEMETAL_ORIGINAL_SHA256" \
      "$DXMT_WOW64_COMPANION_ABI_SCHEMA_SHA256" "$NATIVE_COMPILER_POLICY_IDENTITY"
  )" || exit $?
  runtime_id="$(
    switchyard_native_runtime_id_from_closure_digest \
      "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX" "$wine_revision" "$source_dirty" \
      "$runtime_closure_digest"
  )" || exit $?
else
  runtime_id="${SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX}${source_identity}-${gptk_redist_digest}-${wine_mono_digest:0:12}-${gstreamer_deps_digest}-${vulkan_deps_digest}-${mesa_windows_digest}-${font_deps_digest}-${font_assets_digest}-${tls_deps_digest}-${tls_dlopen_digest}"
fi
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
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  WINE_INSTALL_PREFIX="$(
    switchyard_canonical_runtime_install_prefix "$WINE_INSTALL_PREFIX"
  )" || exit $?
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
    "$WINE_INSTALL_PREFIX" || exit $?
fi
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
  local manifest_gstreamer_digest
  local manifest_mesa_digest
  local manifest_unicorn_digest
  local manifest_unicorn_payload_digest
  local manifest_unicorn_library_sha
  local manifest_provider_sha
  local manifest_native_value
  local index
  local kind
  local name
  local expected_hash
  local url
  local extra
  local asset_path
  local plugin

  [ -f "$manifest" ] || return 1
  runtime_content_tree_is_verified "$prefix" || return 1
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    switchyard_validate_runtime_manifest_profile \
      "$manifest" "$SWITCHYARD_RUNTIME_PROFILE" "$prefix" >/dev/null 2>&1 || return 1
  else
    switchyard_validate_runtime_manifest_profile \
      "$manifest" "$SWITCHYARD_RUNTIME_PROFILE" >/dev/null 2>&1 || return 1
  fi
  manifest_id="$(/usr/bin/plutil -extract id raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_id" = "$runtime_id" ] || return 1
  manifest_install_prefix="$(/usr/bin/plutil -extract installPrefix raw -o - "$manifest" 2>/dev/null || true)"
  manifest_executable="$(/usr/bin/plutil -extract executable raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_install_prefix" = "$FINAL_WINE_INSTALL_PREFIX" ] || return 1
  [ "$manifest_executable" = "$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine" ] || return 1
  [ -x "$prefix/bin/switchyard-wine" ] || return 1
  [ "$(readlink "$prefix/bin/switchyard-wine" 2>/dev/null || true)" = "wine" ] || return 1
  [ -f "$prefix/share/switchyard/metal_hud_safety.sh" ] || return 1
  [ -f "$prefix/share/switchyard/gpu_capability_policy.sh" ] || return 1
  [ -x "$prefix/libexec/switchyard-host-gpu-info" ] || return 1
  [ -x "$prefix/lib/wine/$WINE_UNIX_ARCH-unix/wine" ] || return 1
  [ -f "$prefix/lib/wine/i386-windows/ntdll.dll" ] || return 1
  [ -f "$prefix/lib/wine/x86_64-windows/ntdll.dll" ] || return 1
  expected_wine_sha="$(/usr/bin/plutil -extract integrity.wineUnixSha256 raw -o - "$manifest" 2>/dev/null || true)"
  expected_i386_ntdll_sha="$(/usr/bin/plutil -extract integrity.i386NtdllSha256 raw -o - "$manifest" 2>/dev/null || true)"
  expected_x86_64_ntdll_sha="$(/usr/bin/plutil -extract integrity.x86_64NtdllSha256 raw -o - "$manifest" 2>/dev/null || true)"
  [ -n "$expected_wine_sha" ] &&
    [ "$(sha256_file "$prefix/lib/wine/$WINE_UNIX_ARCH-unix/wine")" = "$expected_wine_sha" ] || return 1
  [ -n "$expected_i386_ntdll_sha" ] &&
    [ "$(sha256_file "$prefix/lib/wine/i386-windows/ntdll.dll")" = "$expected_i386_ntdll_sha" ] || return 1
  [ -n "$expected_x86_64_ntdll_sha" ] &&
    [ "$(sha256_file "$prefix/lib/wine/x86_64-windows/ntdll.dll")" = "$expected_x86_64_ntdll_sha" ] || return 1
  manifest_gstreamer_digest="$(/usr/bin/plutil -extract gstreamerRuntime.digest raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_gstreamer_digest" = "$gstreamer_deps_digest" ] || return 1
  content_tree_is_verified "$prefix/lib/switchyard-gstreamer" || return 1
  [ "$(content_tree_digest "$prefix/lib/switchyard-gstreamer")" = "$gstreamer_deps_digest" ] || return 1
  [ -x "$prefix/lib/switchyard-gstreamer/libexec/gstreamer-1.0/gst-plugin-scanner" ] || return 1
  for plugin in "${GSTREAMER_PLUGIN_FILES[@]}"; do
    [ -f "$prefix/lib/switchyard-gstreamer/lib/gstreamer-1.0/$plugin" ] || return 1
  done
  [ -f "$prefix/lib/wine/$WINE_UNIX_ARCH-unix/winegstreamer.so" ] || return 1
  otool -L "$prefix/lib/wine/$WINE_UNIX_ARCH-unix/winegstreamer.so" |
    grep -F '@rpath/libgstreamer-1.0.0.dylib' >/dev/null || return 1
  otool -l "$prefix/lib/wine/$WINE_UNIX_ARCH-unix/winegstreamer.so" |
    grep -F '@loader_path/../../switchyard-gstreamer/lib' >/dev/null || return 1
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    vulkan_deps_match_profile_architecture \
      "$prefix/lib/switchyard-vulkan" || return 1
    font_deps_match_profile_architecture \
      "$prefix/lib/switchyard-fonts" || return 1
    content_tree_is_verified \
      "$prefix/lib/switchyard-fonts" || return 1
    [ "$(content_tree_digest "$prefix/lib/switchyard-fonts")" = \
      "$font_deps_digest" ] || return 1
    tls_deps_match_profile_architecture \
      "$prefix/lib/switchyard-tls" || return 1
    gstreamer_deps_match_profile_architecture \
      "$prefix/lib/switchyard-gstreamer" || return 1
    verify_runtime_relative_macho_tree \
      "$prefix/lib/switchyard-vulkan" "Vulkan runtime" >/dev/null 2>&1 || return 1
    verify_runtime_relative_macho_tree \
      "$prefix/lib/switchyard-fonts" "font runtime" >/dev/null 2>&1 || return 1
    verify_runtime_relative_macho_tree \
      "$prefix/lib/switchyard-tls" "TLS runtime" >/dev/null 2>&1 || return 1
    verify_runtime_relative_macho_tree \
      "$prefix/lib/switchyard-gstreamer" "GStreamer runtime" >/dev/null 2>&1 || return 1
    verify_host_macho_tree_signatures \
      "$prefix/lib/switchyard-vulkan" "Vulkan runtime" >/dev/null 2>&1 || return 1
    verify_host_macho_tree_signatures \
      "$prefix/lib/switchyard-fonts" "font runtime" >/dev/null 2>&1 || return 1
    verify_host_macho_tree_signatures \
      "$prefix/lib/switchyard-tls" "TLS runtime" >/dev/null 2>&1 || return 1
    verify_host_macho_tree_signatures \
      "$prefix/lib/switchyard-gstreamer" "GStreamer runtime" >/dev/null 2>&1 || return 1
    /usr/bin/codesign --verify --strict --verbose=2 \
      "$prefix/lib/wine/$WINE_UNIX_ARCH-unix/winegstreamer.so" \
      >/dev/null 2>&1 || return 1

    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.architecture raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = \
      "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.digest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$gstreamer_deps_digest" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.runtimePackage raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$GSTREAMER_RUNTIME_PACKAGE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.runtimePackageUrl raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = \
      "$GSTREAMER_PACKAGE_BASE_URL/$GSTREAMER_RUNTIME_PACKAGE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.runtimePackageSha256 raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$GSTREAMER_RUNTIME_PACKAGE_SHA256" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.developmentPackage raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$GSTREAMER_DEVEL_PACKAGE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.developmentPackageUrl raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = \
      "$GSTREAMER_PACKAGE_BASE_URL/$GSTREAMER_DEVEL_PACKAGE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract gstreamerRuntime.developmentPackageSha256 raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$GSTREAMER_DEVEL_PACKAGE_SHA256" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract fontRuntime.architecture raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$HOST_MACHO_ARCH" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract fontRuntime.digest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$font_deps_digest" ] || return 1
    for index in "${!FONT_DEPS_NAMES[@]}"; do
      manifest_native_value="$(/usr/bin/plutil -extract "fontRuntime.formulae.$index.name" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_native_value" = "${FONT_DEPS_NAMES[$index]}" ] || return 1
      manifest_native_value="$(/usr/bin/plutil -extract "fontRuntime.formulae.$index.version" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_native_value" = "${FONT_DEPS_VERSIONS[$index]}" ] || return 1
      manifest_native_value="$(/usr/bin/plutil -extract "fontRuntime.formulae.$index.repository" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_native_value" = "${FONT_DEPS_REPOSITORIES[$index]}" ] || return 1
      manifest_native_value="$(/usr/bin/plutil -extract "fontRuntime.formulae.$index.bottleTag" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_native_value" = \
        "$SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG" ] || return 1
      manifest_native_value="$(/usr/bin/plutil -extract "fontRuntime.formulae.$index.layerSha256" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_native_value" = "${FONT_DEPS_LAYER_SHA256[$index]}" ] || return 1
    done
    manifest_native_value="$(/usr/bin/plutil -extract tlsRuntime.architecture raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$HOST_MACHO_ARCH" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract tlsRuntime.digest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$tls_deps_digest" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract tlsRuntime.packageSubdir raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$TLS_PACKAGE_SUBDIR" ] || return 1
    cmp -s "$TLS_PACKAGE_MANIFEST" \
      "$prefix/lib/switchyard-tls/share/doc/switchyard-tls/packages.tsv" || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.architecture raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$HOST_MACHO_ARCH" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.digest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$vulkan_deps_digest" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanLoader.version raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_LOADER_VERSION" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanLoader.repository raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_LOADER_REPOSITORY" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanLoader.bottle raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_LOADER_BOTTLE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanLoader.manifestDigest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_LOADER_MANIFEST_DIGEST" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanLoader.layerSha256 raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_LOADER_LAYER_SHA256" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanHeaders.version raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_HEADERS_VERSION" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanHeaders.repository raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_HEADERS_REPOSITORY" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanHeaders.bottle raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_HEADERS_BOTTLE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanHeaders.manifestDigest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_HEADERS_MANIFEST_DIGEST" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.vulkanHeaders.layerSha256 raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$VULKAN_HEADERS_LAYER_SHA256" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.moltenVK.version raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$MOLTENVK_VERSION" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.moltenVK.repository raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$MOLTENVK_REPOSITORY" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.moltenVK.bottle raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$MOLTENVK_BOTTLE" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.moltenVK.manifestDigest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$MOLTENVK_MANIFEST_DIGEST" ] || return 1
    manifest_native_value="$(/usr/bin/plutil -extract vulkanRuntime.moltenVK.layerSha256 raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_native_value" = "$MOLTENVK_LAYER_SHA256" ] || return 1

    validate_staged_unicorn_runtime "$prefix" >/dev/null 2>&1 || return 1
    validate_staged_unicorn_providers "$prefix" >/dev/null 2>&1 || return 1
    manifest_unicorn_digest="$(/usr/bin/plutil -extract cpuProvider.developmentCacheDigest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_unicorn_digest" = "$unicorn_runtime_digest" ] || return 1
    manifest_unicorn_payload_digest="$(/usr/bin/plutil -extract cpuProvider.runtimePayloadDigest raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_unicorn_payload_digest" = \
      "$(runtime_content_tree_digest "$prefix/$UNICORN_PACKAGE_ROOT_RELATIVE")" ] || return 1
    manifest_unicorn_library_sha="$(/usr/bin/plutil -extract cpuProvider.librarySha256 raw -o - "$manifest" 2>/dev/null || true)"
    [ "$manifest_unicorn_library_sha" = \
      "$(sha256_file "$prefix/$UNICORN_DYLIB_RELATIVE")" ] || return 1
    for index in "${!UNICORN_PROVIDER_UNIXLIBS[@]}"; do
      manifest_provider_sha="$(/usr/bin/plutil -extract "cpuProvider.components.$index.unixLibrarySha256" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_provider_sha" = \
        "$(sha256_file "$prefix/${UNICORN_PROVIDER_UNIXLIBS[$index]}")" ] || return 1
      manifest_provider_sha="$(/usr/bin/plutil -extract "cpuProvider.components.$index.peLibrarySha256" raw -o - "$manifest" 2>/dev/null || true)"
      [ "$manifest_provider_sha" = \
        "$(sha256_file "$prefix/${UNICORN_PROVIDER_PE_LIBS[$index]}")" ] || return 1
    done
    switchyard_validate_native_arm64_runtime_packaging \
      "$prefix" "$manifest" "$ROOT_DIR" >/dev/null 2>&1 || return 1
  fi
  manifest_mesa_digest="$(/usr/bin/plutil -extract mesaOpenGL.digest raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_mesa_digest" = "$mesa_windows_digest" ] || return 1
  content_tree_is_verified "$prefix/lib/switchyard-mesa" || return 1
  [ "$(content_tree_digest "$prefix/lib/switchyard-mesa")" = "$mesa_windows_digest" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/x86_64-windows/opengl32.dll" ] || return 1
  [ "$(sha256_file "$prefix/lib/switchyard-mesa/x86_64-windows/opengl32.dll")" = "$MESA_WINDOWS_X86_64_OPENGL_SHA256" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/x86_64-windows/libgallium_wgl.dll" ] || return 1
  [ "$(sha256_file "$prefix/lib/switchyard-mesa/x86_64-windows/libgallium_wgl.dll")" = "$MESA_WINDOWS_X86_64_GALLIUM_SHA256" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/i386-windows/opengl32.dll" ] || return 1
  [ "$(sha256_file "$prefix/lib/switchyard-mesa/i386-windows/opengl32.dll")" = "$MESA_WINDOWS_I386_OPENGL_SHA256" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/i386-windows/libgallium_wgl.dll" ] || return 1
  [ "$(sha256_file "$prefix/lib/switchyard-mesa/i386-windows/libgallium_wgl.dll")" = "$MESA_WINDOWS_I386_GALLIUM_SHA256" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/share/doc/switchyard-mesa/MESA-LICENSE.rst" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/share/doc/switchyard-mesa/LLVM-LICENSE.txt" ] || return 1
  [ -f "$prefix/lib/switchyard-mesa/share/doc/switchyard-mesa/DISTRIBUTOR-LICENSE.txt" ] || return 1
  manifest_font_assets_digest="$(/usr/bin/plutil -extract fontAssets.digest raw -o - "$manifest" 2>/dev/null || true)"
  [ "$manifest_font_assets_digest" = "$font_assets_digest" ] || return 1
  cmp -s "$FONT_ASSET_MANIFEST" \
    "$prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv" || return 1
  [ -f "$FONTCONFIG_ASSET_FRAGMENT" ] || return 1
  cmp -s "$FONTCONFIG_ASSET_FRAGMENT" \
    "$prefix/lib/switchyard-fonts/etc/fonts/conf.d/50-switchyard-font-assets.conf" || return 1
  [ -f "$prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/generated-fonts.tsv" ] || return 1
  while IFS=$'\t' read -r kind name expected_hash url extra; do
    case "$kind" in
      ''|'#'*) continue ;;
      font) asset_path="$prefix/share/wine/fonts/$name" ;;
      font-source) continue ;;
      license) asset_path="$prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets/$name" ;;
      *) return 1 ;;
    esac
    [ -z "${extra:-}" ] || return 1
    [ -f "$asset_path" ] || return 1
    [ "$(sha256_file "$asset_path")" = "$expected_hash" ] || return 1
  done < "$FONT_ASSET_MANIFEST"
  [ -f "$prefix/share/wine/fonts/$FONT_EMOJI_FILE" ] || return 1
  [ "$(sha256_file "$prefix/share/wine/fonts/$FONT_EMOJI_FILE")" = "$FONT_EMOJI_SHA256" ] || return 1
  [ -f "$prefix/share/wine/fonts/$FONT_ALIAS_FILE" ] || return 1
  [ "$(sha256_file "$prefix/share/wine/fonts/$FONT_ALIAS_FILE")" = "$FONT_ALIAS_SHA256" ] || return 1
}

runtime_is_complete() {
  runtime_is_complete_at "$FINAL_WINE_INSTALL_PREFIX"
}

if [ "$MODE" = "--ensure" ] && runtime_is_complete; then
  switchyard_require_native_arm64_ensure_host
  wine_executable="$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine"
  defaults write dev.switchyard.Switchyard winePath "$wine_executable"
  defaults write dev.switchyard.Switchyard 'activeRuntimeSourceRevision.v1' "$wine_revision"
  echo "Switchyard Wine runtime is current ($runtime_id): $wine_executable"
  exit 0
fi

if [ "$MODE" = "--ensure" ]; then
  echo "No complete Switchyard Wine runtime matches $runtime_id; building it now."
fi

mkdir -p "$WINE_BUILD_DIR" "$(dirname "$FINAL_WINE_INSTALL_PREFIX")"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  current_native_runtime_prefix="$(
    switchyard_canonical_runtime_install_prefix "$FINAL_WINE_INSTALL_PREFIX"
  )" || exit $?
  [ "$current_native_runtime_prefix" = "$FINAL_WINE_INSTALL_PREFIX" ] || {
    echo "Native runtime install prefix changed while preparing the build." >&2
    exit 1
  }
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
    "$FINAL_WINE_INSTALL_PREFIX" || exit $?
fi

configured=0
native_compiler_config_identity=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  native_compiler_config_identity="$(
    /usr/bin/printf '%s\0%s\0%s\0%s\0%s\0%s\0%s\0%s\0%s\0%s\0' \
      "$NATIVE_COMPILER_POLICY_IDENTITY" "$NATIVE_HOST_CLANG" \
      "$NATIVE_HOST_CLANGXX" "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
      "$NATIVE_MACOS_DEPLOYMENT_FLAG" "$NATIVE_MACOS_SDKROOT" \
      "$NATIVE_MACOS_SDK_VERSION" "$NATIVE_MACOS_SDK_BUILD_VERSION" \
      "$NATIVE_MACOS_SDK_SETTINGS_SHA256" "$NATIVE_MACOS_SDK_FLAG" |
      /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
  )"
fi
if [ -f "$WINE_BUILD_DIR/config.status" ]; then
  configured=1
fi

if [ "$configured" -eq 1 ] && ! grep -F "prefix = $WINE_INSTALL_PREFIX" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
  RECONFIGURE=1
fi

if [ "$configured" -eq 1 ]; then
  # Only the x86_64 Darwin loader needs either the preloader or the -no_huge
  # reservation.  The aarch64 configure path intentionally uses neither.
  if [ "$WINE_UNIX_ARCH" = "x86_64" ]; then
    if grep -F "#define HAVE_WINE_PRELOADER 1" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
      if [ "$MACOS_NO_HUGE_SUPPORTED" -eq 1 ]; then
        echo "existing Wine build uses the macOS preloader despite -no_huge support; reconfiguring"
        RECONFIGURE=1
      fi
    elif ! grep -F -- "-no_huge" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
      echo "existing Wine build has neither the macOS preloader nor the -no_huge loader reservation; reconfiguring"
      RECONFIGURE=1
    fi
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
  for dependency_prefix in "$font_deps_prefix" "$gstreamer_deps_prefix" "$vulkan_deps_prefix"; do
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
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    if [ ! -f "$WINE_BUILD_DIR/.switchyard-native-compiler-policy" ] ||
       [ -L "$WINE_BUILD_DIR/.switchyard-native-compiler-policy" ] ||
       [ "$(<"$WINE_BUILD_DIR/.switchyard-native-compiler-policy")" != \
         "$native_compiler_config_identity" ]; then
      echo "existing native Wine build does not use the qualified compiler identity; reconfiguring"
      RECONFIGURE=1
    fi
    if ! native_configured_compiler_policy_is_exact "$WINE_BUILD_DIR/Makefile"; then
      echo "existing native Wine build has ambiguous compiler assignments; reconfiguring"
      RECONFIGURE=1
    fi
    if ! grep -F -- "--with-mingw=$NATIVE_MINGW_CLANG" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F -- "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F -- "--with-unicorn" "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "UNICORN_CFLAGS = -I${unicorn_runtime_prefix}/lib/pkgconfig/../../include" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "UNICORN_LIBS = -L${unicorn_runtime_prefix}/lib/pkgconfig/.. -lunicorn" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT64_UNIXLIB = xtajit64.so" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT64_PE_CFLAGS = -DHAVE_UNICORN" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT_UNIXLIB = xtajit.so" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT_PE_CFLAGS = -DHAVE_UNICORN" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "WINEMETAL_WOW64_UNIXLIB = winemetal-wow64.so" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
      echo "existing native Wine build does not use the exact native provider and DXMT companion; reconfiguring"
      RECONFIGURE=1
    fi
  fi
  if ! grep -F -- "-mmacosx-version-min=$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
      "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
    echo "existing Wine build does not target macOS $SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS; reconfiguring"
    RECONFIGURE=1
  fi
fi

if [ "$RECONFIGURE" = "1" ] && [ "$configured" -eq 1 ]; then
  configured=0
fi

if [ "$configured" -eq 0 ]; then
  echo "configuring Switchyard Wine in $WINE_BUILD_DIR"
  profile_configure_options=()
  configure_cppflags="-I${font_deps_prefix}/include -I${font_deps_prefix}/include/freetype2 -I${vulkan_deps_prefix}/include"
  configure_ldflags="-L${font_deps_prefix}/lib -Wl,-rpath,${font_deps_prefix}/lib -L${vulkan_deps_prefix}/lib -Wl,-rpath,${vulkan_deps_prefix}/lib"
  configure_pkg_config_path="${gstreamer_deps_prefix}/lib/pkgconfig:${font_deps_prefix}/lib/pkgconfig:${vulkan_deps_prefix}/lib/pkgconfig"
  configure_deployment_flag="-mmacosx-version-min=$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
  configure_sdk_flag=""
  if [ -n "$tls_deps_prefix" ]; then
    configure_cppflags="-I${tls_deps_prefix}/include ${configure_cppflags}"
    configure_ldflags="-L${tls_deps_prefix}/lib -Wl,-rpath,${tls_deps_prefix}/lib ${configure_ldflags}"
    configure_pkg_config_path="${tls_deps_prefix}/lib/pkgconfig:${configure_pkg_config_path}"
  fi
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    configure_cppflags="-I${unicorn_runtime_prefix}/include ${configure_cppflags}"
    configure_ldflags="-L${unicorn_runtime_prefix}/lib -Wl,-rpath,${unicorn_runtime_prefix}/lib ${configure_ldflags}"
    configure_pkg_config_path="${unicorn_runtime_prefix}/lib/pkgconfig:${configure_pkg_config_path}"
    profile_configure_options+=(
      "--with-mingw=$NATIVE_MINGW_CLANG"
      --with-unicorn
    )
  fi
  configure_cc="clang -arch $HOST_MACHO_ARCH"
  configure_cxx="clang++ -arch $HOST_MACHO_ARCH"
  configure_objc="clang -arch $HOST_MACHO_ARCH"
  configure_cflags="${configure_deployment_flag} ${CFLAGS:-}"
  configure_cxxflags="${configure_deployment_flag} ${CXXFLAGS:-}"
  configure_objcflags="${configure_deployment_flag} ${OBJCFLAGS:-}"
  configure_cppflags_environment="${configure_cppflags} ${CPPFLAGS:-}"
  configure_ldflags_environment="${configure_deployment_flag} ${configure_ldflags} ${LDFLAGS:-}"
  configure_pkg_config_path_environment="${configure_pkg_config_path}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    validate_native_host_toolchain_policy || exit $?
    configure_sdk_flag="$NATIVE_MACOS_SDK_FLAG"
    configure_cc="$NATIVE_HOST_CLANG $SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG -arch $HOST_MACHO_ARCH"
    configure_cxx="$NATIVE_HOST_CLANGXX $SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG -arch $HOST_MACHO_ARCH"
    configure_objc="$NATIVE_HOST_CLANG $SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG -arch $HOST_MACHO_ARCH"
    configure_cflags="-g -O2 $configure_deployment_flag $configure_sdk_flag"
    configure_cxxflags="-g -O2 $configure_deployment_flag $configure_sdk_flag"
    configure_objcflags="-g -O2 $configure_deployment_flag $configure_sdk_flag"
    configure_cppflags_environment="$configure_cppflags"
    configure_ldflags_environment="${configure_deployment_flag} ${configure_sdk_flag} ${configure_ldflags}"
    configure_pkg_config_path_environment="$configure_pkg_config_path"
  fi
  (
    cd "$WINE_BUILD_DIR"
    export ac_cv_lib_soname_freetype="$FONT_DLOPEN_FREETYPE"
    export ac_cv_lib_soname_fontconfig="$FONT_DLOPEN_FONTCONFIG"
    if [ -n "$tls_deps_prefix" ]; then
      export ac_cv_lib_soname_gnutls="$TLS_DLOPEN_NAME"
    fi
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      unset cc cxx cpp objc objcxx
    fi
    SDKROOT="${NATIVE_MACOS_SDKROOT:-${SDKROOT:-}}" \
    MACOSX_DEPLOYMENT_TARGET="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
    CFLAGS="$configure_cflags" \
    CXXFLAGS="$configure_cxxflags" \
    OBJCFLAGS="$configure_objcflags" \
    CPPFLAGS="$configure_cppflags_environment" \
    LDFLAGS="$configure_ldflags_environment" \
    PKG_CONFIG_PATH="$configure_pkg_config_path_environment" \
    "${PROFILE_ARCH_COMMAND[@]}" "$WINE_DIR/configure" \
      --build="$WINE_BUILD_TRIPLET" \
      --host="$WINE_HOST_TRIPLET" \
      CC="$configure_cc" \
      CXX="$configure_cxx" \
      OBJC="$configure_objc" \
      --enable-archs="$SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV" \
      "${profile_configure_options[@]}" \
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
      --with-gstreamer \
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
  if ! grep -F "GSTREAMER_LIBS = -L${gstreamer_deps_prefix}/lib/pkgconfig/../../lib" \
       "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
    echo "Wine configure did not enable the staged GStreamer development runtime." >&2
    echo "Refusing to build a Wine runtime without its Media Foundation backend." >&2
    exit 1
  fi
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    if ! grep -F "UNICORN_CFLAGS = -I${unicorn_runtime_prefix}/lib/pkgconfig/../../include" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "UNICORN_LIBS = -L${unicorn_runtime_prefix}/lib/pkgconfig/.. -lunicorn" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT64_UNIXLIB = xtajit64.so" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT64_PE_CFLAGS = -DHAVE_UNICORN" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT_UNIXLIB = xtajit.so" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "XTAJIT_PE_CFLAGS = -DHAVE_UNICORN" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1 ||
       ! grep -F "WINEMETAL_WOW64_UNIXLIB = winemetal-wow64.so" \
         "$WINE_BUILD_DIR/config.status" >/dev/null 2>&1; then
      echo "Wine configure did not enable the native providers and DXMT WoW64 companion." >&2
      exit 1
    fi
    native_configured_compiler_policy_is_exact "$WINE_BUILD_DIR/Makefile" || {
      echo "Wine configure did not preserve the exact qualified native compilers." >&2
      exit 1
    }
    /usr/bin/printf '%s\n' "$native_compiler_config_identity" > \
      "$WINE_BUILD_DIR/.switchyard-native-compiler-policy"
  fi
fi

assert_source_state_unchanged "dependency preparation"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  validate_native_host_toolchain_policy || exit $?
fi
echo "building Switchyard Wine with $JOBS jobs"
make -C "$WINE_BUILD_DIR" -j"$JOBS"
assert_source_state_unchanged "compilation"

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  current_native_runtime_prefix="$(
    switchyard_canonical_runtime_install_prefix "$FINAL_WINE_INSTALL_PREFIX"
  )" || exit $?
  [ "$current_native_runtime_prefix" = "$FINAL_WINE_INSTALL_PREFIX" ] || {
    echo "Native runtime install prefix changed before runtime staging." >&2
    exit 1
  }
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
    "$FINAL_WINE_INSTALL_PREFIX" || exit $?
fi
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
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  companion="$staged_wine_install_prefix/lib/wine/aarch64-unix/winemetal-wow64.so"
  if [ ! -f "$companion" ] || [ -L "$companion" ] || [ ! -x "$companion" ]; then
    echo "Wine install did not produce the required native ARM64 DXMT WoW64 companion: $companion" >&2
    exit 1
  fi
fi
WINE_INSTALL_PREFIX="$staged_wine_install_prefix"

if ! grep -F "#define HAVE_WINE_PRELOADER 1" "$WINE_BUILD_DIR/include/config.h" >/dev/null 2>&1; then
  echo "removing stale Wine preloader files from no-preloader runtime"
  find "$WINE_INSTALL_PREFIX" \( -type f -o -type l \) \( -name wine-preloader -o -name wine64-preloader \) -exec rm -f {} +
fi

for pe_arch in "${INSTALLED_PE_ARCHS[@]}"; do
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
for pe_arch in "${INSTALLED_PE_ARCHS[@]}"; do
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

  gptk_d3d12_pe="$WINE_INSTALL_PREFIX/lib/wine/x86_64-windows/d3d12.dll"
  gptk_d3d12_unix="$WINE_INSTALL_PREFIX/lib/wine/x86_64-unix/d3d12.so"
  d3d12_metal_pe="$WINE_INSTALL_PREFIX/lib/wine/x86_64-windows/d3dmt.dll"
  d3d12_metal_unix="$WINE_INSTALL_PREFIX/lib/wine/x86_64-unix/d3dmt.so"
  wine_d3d12_pe="$wine_graphics_fallback_root/x86_64-windows/d3d12.dll"
  wine_d3d12core_pe="$wine_graphics_fallback_root/x86_64-windows/d3d12core.dll"

  if [ -f "$gptk_d3d12_pe" ]; then
    if [ ! -e "$gptk_d3d12_unix" ]; then
      echo "GPTK D3D12 overlay is missing its x86_64 Unix library." >&2
      exit 1
    fi
    if [ -e "$d3d12_metal_pe" ] || [ -e "$d3d12_metal_unix" ]; then
      echo "GPTK overlay unexpectedly contains a d3dmt module." >&2
      exit 1
    fi
    if [ ! -f "$wine_d3d12_pe" ] || [ ! -f "$wine_d3d12core_pe" ]; then
      echo "Wine D3D12 Agility proxy modules were not preserved before the GPTK overlay." >&2
      exit 1
    fi

    echo "installing the D3D12 Agility proxy in front of D3DMetal"
    mv "$gptk_d3d12_pe" "$d3d12_metal_pe"
    mv "$gptk_d3d12_unix" "$d3d12_metal_unix"
    python3 "$PE_MODULE_RENAME_SCRIPT" "$d3d12_metal_pe" d3d12.dll d3dmt.dll
    install -m 0644 "$wine_d3d12_pe" "$gptk_d3d12_pe"
    install -m 0644 "$wine_d3d12core_pe" \
      "$WINE_INSTALL_PREFIX/lib/wine/x86_64-windows/d3d12core.dll"
  fi
elif [ "$DISABLE_GPTK_OVERLAY" = "1" ]; then
  echo "GPTK overlay explicitly disabled for this runtime build"
else
  echo "GPTK redist was not found; leaving Wine runtime without GPTK overlay." >&2
fi

if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  echo "normalizing the native ARM64 DXMT WoW64 companion runtime closure"
  switchyard_normalize_native_arm64_dxmt_companion_rpaths \
    "$WINE_INSTALL_PREFIX"
  echo "staging the pinned native ARM64 DXMT artifact closure"
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE_DIR" "$ROOT_DIR" "$WINE_INSTALL_PREFIX"
fi

echo "installing Wine Mono addon $WINE_MONO_FILE"
mkdir -p "$WINE_INSTALL_PREFIX/share/wine/mono"
install -m 0644 "$wine_mono_path" "$WINE_INSTALL_PREFIX/share/wine/mono/$WINE_MONO_FILE"

echo "installing the pinned GStreamer media runtime"
rm -rf "$WINE_INSTALL_PREFIX/lib/switchyard-gstreamer"
mkdir -p "$WINE_INSTALL_PREFIX/lib/switchyard-gstreamer"
ditto "$gstreamer_deps_prefix" "$WINE_INSTALL_PREFIX/lib/switchyard-gstreamer"
relocate_winegstreamer_for_runtime "$WINE_INSTALL_PREFIX" "$gstreamer_deps_prefix"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  verify_host_macho_tree_arches "$WINE_INSTALL_PREFIX/lib/switchyard-gstreamer" \
    "staged GStreamer runtime" "${GSTREAMER_MACHO_ARCHS[@]}"
  verify_native_macho_tree_macos_compatibility \
    "$WINE_INSTALL_PREFIX/lib/switchyard-gstreamer" \
    "staged GStreamer runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
  verify_runtime_relative_macho_tree "$WINE_INSTALL_PREFIX/lib/switchyard-gstreamer" \
    "staged GStreamer runtime"
fi

echo "installing $HOST_DEPENDENCY_ARCH Vulkan loader and MoltenVK runtime"
rm -rf "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan"
mkdir -p "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan"
ditto "$vulkan_deps_prefix" "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  verify_host_macho_tree_arches "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan" \
    "staged Vulkan runtime" "$HOST_DEPENDENCY_ARCH"
  verify_native_macho_tree_macos_compatibility \
    "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan" \
    "staged Vulkan runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
  verify_runtime_relative_macho_tree "$WINE_INSTALL_PREFIX/lib/switchyard-vulkan" \
    "staged Vulkan runtime"
fi

echo "installing i386/x86_64 Mesa Windows OpenGL fallback"
rm -rf "$WINE_INSTALL_PREFIX/lib/switchyard-mesa"
mkdir -p "$WINE_INSTALL_PREFIX/lib/switchyard-mesa"
ditto "$mesa_windows_prefix" "$WINE_INSTALL_PREFIX/lib/switchyard-mesa"

echo "installing $HOST_DEPENDENCY_ARCH FreeType and fontconfig runtime libraries"
runtime_font_root="$WINE_INSTALL_PREFIX/lib/switchyard-fonts"
rm -rf "$runtime_font_root"
mkdir -p "$runtime_font_root"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  ditto "$FONT_RUNTIME_PREPARED_ROOT" "$runtime_font_root"
else
  ditto "$font_deps_prefix" "$runtime_font_root"
  relocate_font_deps_for_runtime "$runtime_font_root" "$font_deps_prefix"
  if [ -f "$runtime_font_root/etc/fonts/fonts.conf" ]; then
    perl -0pi -e "s#\\n\\s*<cachedir>\\Q${font_deps_prefix}/var/cache/fontconfig\\E</cachedir>##g" \
      "$runtime_font_root/etc/fonts/fonts.conf"
  fi
  if [ ! -f "$FONTCONFIG_ASSET_FRAGMENT" ]; then
    echo "missing Fontconfig asset fragment: $FONTCONFIG_ASSET_FRAGMENT" >&2
    exit 1
  fi
  mkdir -p "$runtime_font_root/etc/fonts/conf.d" \
    "$runtime_font_root/share/doc/switchyard-font-assets"
  install -m 0644 "$FONTCONFIG_ASSET_FRAGMENT" \
    "$runtime_font_root/etc/fonts/conf.d/50-switchyard-font-assets.conf"
  ditto "$font_assets_prefix/lib/switchyard-fonts/share/doc/switchyard-font-assets" \
    "$runtime_font_root/share/doc/switchyard-font-assets"
fi
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  content_tree_is_verified "$runtime_font_root" || {
    echo "Staged font runtime content marker does not match the installed bytes." >&2
    exit 1
  }
  installed_font_digest="$(content_tree_digest "$runtime_font_root")" || {
    echo "Could not digest the staged font runtime." >&2
    exit 1
  }
  [ "$installed_font_digest" = "$font_deps_digest" ] || {
    echo "Staged font runtime digest changed after final preparation." >&2
    exit 1
  }
  verify_host_macho_tree_arches "$runtime_font_root" \
    "staged font runtime" "$HOST_DEPENDENCY_ARCH"
  verify_native_macho_tree_macos_compatibility "$runtime_font_root" \
    "staged font runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
  verify_runtime_relative_macho_tree "$runtime_font_root" "staged font runtime"
  verify_host_macho_tree_signatures "$runtime_font_root" "staged font runtime"
  remove_prepared_font_runtime "$FONT_RUNTIME_PREPARED_ROOT"
  FONT_RUNTIME_PREPARED_ROOT=""
fi

echo "installing $font_asset_count redistributable font files"
mkdir -p "$WINE_INSTALL_PREFIX/share/wine/fonts" \
  "$runtime_font_root/share/doc/switchyard-font-assets"
ditto "$font_assets_prefix/share/wine/fonts" "$WINE_INSTALL_PREFIX/share/wine/fonts"

if [ -n "$tls_deps_prefix" ]; then
  echo "installing $HOST_DEPENDENCY_ARCH GnuTLS runtime libraries"
  rm -rf "$WINE_INSTALL_PREFIX/lib/switchyard-tls"
  mkdir -p "$WINE_INSTALL_PREFIX/lib/switchyard-tls"
  ditto "$tls_deps_prefix" "$WINE_INSTALL_PREFIX/lib/switchyard-tls"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    verify_host_macho_tree_arches "$WINE_INSTALL_PREFIX/lib/switchyard-tls" \
      "staged TLS runtime" "$HOST_DEPENDENCY_ARCH"
    verify_native_macho_tree_macos_compatibility \
      "$WINE_INSTALL_PREFIX/lib/switchyard-tls" \
      "staged TLS runtime" "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"
    verify_runtime_relative_macho_tree "$WINE_INSTALL_PREFIX/lib/switchyard-tls" \
      "staged TLS runtime"
  fi
fi

unicorn_payload_digest=""
unicorn_library_sha256=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  echo "installing the pinned Unicorn native CPU-provider runtime closure"
  stage_unicorn_runtime "$WINE_INSTALL_PREFIX" "$unicorn_runtime_prefix"
  unicorn_payload_digest="$(
    runtime_content_tree_digest "$WINE_INSTALL_PREFIX/$UNICORN_PACKAGE_ROOT_RELATIVE"
  )"
  unicorn_library_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/$UNICORN_DYLIB_RELATIVE")"
fi

echo "installing Wine license, source, and replacement notices"
wine_notice_root="$WINE_INSTALL_PREFIX/share/doc/switchyard-wine"
mkdir -p "$wine_notice_root"
install -m 0644 "$ROOT_DIR/LICENSE" "$wine_notice_root/LICENSE"
install -m 0644 "$ROOT_DIR/COPYING.LIB" "$wine_notice_root/COPYING.LIB"
install -m 0644 "$ROOT_DIR/AUTHORS" "$wine_notice_root/AUTHORS"
install -m 0644 "$ROOT_DIR/docs/building.md" "$wine_notice_root/BUILDING.md"
install -m 0644 "$ROOT_DIR/docs/provenance.md" "$wine_notice_root/PROVENANCE.md"
mkdir -p "$WINE_INSTALL_PREFIX/share/switchyard"
install -m 0644 "$ROOT_DIR/switchyard/lib/metal_hud_safety.sh" \
  "$WINE_INSTALL_PREFIX/share/switchyard/metal_hud_safety.sh"
install -m 0644 "$ROOT_DIR/switchyard/lib/gpu_capability_policy.sh" \
  "$WINE_INSTALL_PREFIX/share/switchyard/gpu_capability_policy.sh"
mkdir -p "$WINE_INSTALL_PREFIX/libexec"
echo "building the D3DMetal host GPU identity helper"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  validate_native_host_toolchain_policy || exit $?
  "${PROFILE_ARCH_COMMAND[@]}" "$NATIVE_HOST_CLANG" \
    "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
    -arch "$HOST_MACHO_ARCH" -fobjc-arc -Wall -Wextra -Werror \
    "$NATIVE_MACOS_DEPLOYMENT_FLAG" "$NATIVE_MACOS_SDK_FLAG" \
    "$ROOT_DIR/switchyard/host_gpu_info.m" \
    -framework Foundation -framework IOKit -framework Metal \
    -o "$WINE_INSTALL_PREFIX/libexec/switchyard-host-gpu-info"
else
  "${PROFILE_ARCH_COMMAND[@]}" clang -arch "$HOST_MACHO_ARCH" -fobjc-arc -Wall -Wextra -Werror \
    -mmacosx-version-min="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
    "$ROOT_DIR/switchyard/host_gpu_info.m" \
    -framework Foundation -framework IOKit -framework Metal \
    -o "$WINE_INSTALL_PREFIX/libexec/switchyard-host-gpu-info"
fi
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
gstreamer_root="$runtime_dir/lib/switchyard-gstreamer"
gstreamer_lib="$gstreamer_root/lib"
gstreamer_plugins="$gstreamer_lib/gstreamer-1.0"
gstreamer_scanner="$gstreamer_root/libexec/gstreamer-1.0/gst-plugin-scanner"
gstreamer_version_file="$gstreamer_root/share/doc/switchyard-gstreamer/VERSION"
font_root="$runtime_dir/lib/switchyard-fonts"
font_lib="$font_root/lib"
mesa_gl_root="$runtime_dir/lib/switchyard-mesa"
metal_hud_safety="$runtime_dir/share/switchyard/metal_hud_safety.sh"
gpu_capability_policy="$runtime_dir/share/switchyard/gpu_capability_policy.sh"

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
if [ ! -d "$gstreamer_plugins" ] || [ ! -x "$gstreamer_scanner" ] ||
   [ ! -f "$gstreamer_version_file" ]; then
  echo "Switchyard runtime is missing its GStreamer media backend." >&2
  exit 127
fi
gstreamer_version="$(tr -d '[:space:]' < "$gstreamer_version_file")"
case "$gstreamer_version" in
  *[!0-9.]*|'')
    echo "Switchyard runtime has invalid GStreamer version metadata." >&2
    exit 127
    ;;
esac
gstreamer_registry_dir="${WINEPREFIX:-${HOME}/.wine}/.switchyard/gstreamer-$gstreamer_version"
mkdir -p "$gstreamer_registry_dir"
export GST_PLUGIN_SYSTEM_PATH="$gstreamer_plugins"
export GST_PLUGIN_SYSTEM_PATH_1_0="$gstreamer_plugins"
export GST_PLUGIN_PATH=
export GST_PLUGIN_PATH_1_0=
export GST_PLUGIN_SCANNER="$gstreamer_scanner"
export GST_PLUGIN_SCANNER_1_0="$gstreamer_scanner"
export GST_REGISTRY="$gstreamer_registry_dir/registry-__SWITCHYARD_GSTREAMER_REGISTRY_ARCH__.bin"
export GST_REGISTRY_1_0="$GST_REGISTRY"
# Forked plugin discovery can stall when both sides are translated by Rosetta.
# The curated plugin set is small enough to register safely in the Wine process.
export GST_REGISTRY_FORK=no
if [ -d "$gstreamer_lib/gio/modules" ]; then
  export GIO_EXTRA_MODULES="$gstreamer_lib/gio/modules"
fi
if [ -f "$font_root/etc/fonts/fonts.conf" ]; then
  export FONTCONFIG_FILE="${FONTCONFIG_FILE:-$font_root/etc/fonts/fonts.conf}"
  export FONTCONFIG_PATH="$(prepend_path "$font_root/etc/fonts" "${FONTCONFIG_PATH:-}")"
fi
if [ ! -f "$metal_hud_safety" ]; then
  echo "Switchyard runtime is missing its Metal HUD safety policy." >&2
  exit 127
fi
# shellcheck source=/dev/null
source "$metal_hud_safety"
switchyard_configure_metal_hud
if [ ! -f "$gpu_capability_policy" ]; then
  echo "Switchyard runtime is missing its D3DMetal GPU capability policy." >&2
  exit 127
fi
# shellcheck source=/dev/null
source "$gpu_capability_policy"
switchyard_configure_d3dmetal_gpu "$runtime_dir"
if [ -n "${VK_ICD_FILENAMES:-}" ]; then
  export SWITCHYARD_HOST_VK_ICD_FILENAMES="$VK_ICD_FILENAMES"
fi
export VK_ICD_FILENAMES="$vulkan_icd"
unset SWITCHYARD_OPENGL_DLL_PATH SWITCHYARD_MESA_DLL_NT_PATH
case "${WINE_OPENGL_DRIVER:-wine}" in
  ''|wine)
    :
    ;;
  llvmpipe)
    if [ ! -f "$mesa_gl_root/x86_64-windows/opengl32.dll" ] ||
       [ ! -f "$mesa_gl_root/x86_64-windows/libgallium_wgl.dll" ] ||
       [ ! -f "$mesa_gl_root/i386-windows/opengl32.dll" ] ||
       [ ! -f "$mesa_gl_root/i386-windows/libgallium_wgl.dll" ]; then
      echo "Switchyard runtime is missing the Mesa OpenGL backend under $mesa_gl_root." >&2
      exit 127
    fi
    export GALLIUM_DRIVER="llvmpipe"
    export MESA_LOADER_DRIVER_OVERRIDE="llvmpipe"
    export LIBGL_ALWAYS_SOFTWARE="1"
    ;;
  *)
    echo "Unsupported WINE_OPENGL_DRIVER value: $WINE_OPENGL_DRIVER" >&2
    echo "Supported values are wine and llvmpipe." >&2
    exit 2
    ;;
esac

invoked_name="$(basename "$0")"
invoked_path="$bin_dir/$invoked_name"
if [ "$invoked_name" = "switchyard-wine" ]; then
  invoked_name="wine"
  invoked_path="$bin_dir/wine"
fi

real_executable="$runtime_dir/lib/wine/__SWITCHYARD_WINE_UNIX_ARCH__-unix/wine"
if [ "$invoked_name" = "wine64" ]; then
  real_executable="$runtime_dir/lib/wine/__SWITCHYARD_WINE_UNIX_ARCH__-unix/wine"
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
SWITCHYARD_WRAPPER_GSTREAMER_REGISTRY_ARCH="$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH" \
SWITCHYARD_WRAPPER_WINE_UNIX_ARCH="$WINE_UNIX_ARCH" \
  perl -0pi -e '
    s/__SWITCHYARD_GSTREAMER_REGISTRY_ARCH__/$ENV{SWITCHYARD_WRAPPER_GSTREAMER_REGISTRY_ARCH}/g;
    s/__SWITCHYARD_WINE_UNIX_ARCH__/$ENV{SWITCHYARD_WRAPPER_WINE_UNIX_ARCH}/g;
  ' "$WINE_INSTALL_PREFIX/bin/wine"
chmod 0755 "$WINE_INSTALL_PREFIX/bin/wine"
if [ -x "$WINE_INSTALL_PREFIX/bin/wine64.switchyard-real" ]; then
  cp "$WINE_INSTALL_PREFIX/bin/wine" "$WINE_INSTALL_PREFIX/bin/wine64"
  chmod 0755 "$WINE_INSTALL_PREFIX/bin/wine64"
fi

staged_wine_executable="$WINE_INSTALL_PREFIX/bin/switchyard-wine"
ln -sf wine "$staged_wine_executable"
wine_executable="$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  echo "ad-hoc signing the native ARM64 runtime process entries"
  switchyard_sign_preview_native_runtime_entries
fi
wine_unix_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/lib/wine/$WINE_UNIX_ARCH-unix/wine")"
wine_real_sha256=""
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  wine_real_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/bin/wine.switchyard-real")"
fi
i386_ntdll_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/lib/wine/i386-windows/ntdll.dll")"
x86_64_ntdll_sha256="$(sha256_file "$WINE_INSTALL_PREFIX/lib/wine/x86_64-windows/ntdll.dll")"

assert_source_state_unchanged "runtime assembly"

{
  printf '{\n'
  printf '  "manifestVersion": %s,\n' "$SWITCHYARD_RUNTIME_MANIFEST_VERSION"
  printf '  "id": %s,\n' "$(json_string "$runtime_id")"
  printf '  "runtimeFamily": %s,\n' "$(json_string "$SWITCHYARD_RUNTIME_PROFILE")"
  printf '  "buildProfile": %s,\n' "$(json_string "$BUILD_PROFILE")"
  printf '  "host": {\n'
  printf '    "platform": "macos",\n'
  printf '    "architecture": %s,\n' "$(json_string "$HOST_MACHO_ARCH")"
  printf '    "wineUnixArchitecture": %s,\n' "$(json_string "$WINE_UNIX_ARCH")"
  printf '    "buildTriplet": %s,\n' "$(json_string "$WINE_BUILD_TRIPLET")"
  printf '    "hostTriplet": %s,\n' "$(json_string "$WINE_HOST_TRIPLET")"
  printf '    "architectureCommand": [\n'
  for index in "${!PROFILE_ARCH_COMMAND[@]}"; do
    if [ "$index" -lt "$((${#PROFILE_ARCH_COMMAND[@]} - 1))" ]; then
      printf '      %s,\n' "$(json_string "${PROFILE_ARCH_COMMAND[$index]}")"
    else
      printf '      %s\n' "$(json_string "${PROFILE_ARCH_COMMAND[$index]}")"
    fi
  done
  printf '    ],\n'
  printf '    "requiresRosetta": %s,\n' "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA"
  printf '    "minimumMacOS": %s,\n' "$(json_string "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS")"
  printf '    "gstreamerRegistryArchitecture": %s\n' \
    "$(json_string "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH")"
  printf '  },\n'
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
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    printf '  "runtimeSigning": {\n'
    printf '    "mode": "engineering-adhoc",\n'
    printf '    "processEntryMachOs": [\n'
    printf '      {"path": "lib/wine/aarch64-unix/wine", "sha256": %s},\n' \
      "$(json_string "$wine_unix_sha256")"
    printf '      {"path": "bin/wine.switchyard-real", "sha256": %s}\n' \
      "$(json_string "$wine_real_sha256")"
    printf '    ]\n'
    printf '  },\n'
    printf '  "cpuProvider": {\n'
    printf '    "implementation": "unicorn",\n'
    printf '    "version": %s,\n' "$(json_string "$SWITCHYARD_UNICORN_VERSION")"
    printf '    "sourceRepository": %s,\n' \
      "$(json_string "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY")"
    printf '    "sourceRevision": %s,\n' \
      "$(json_string "$SWITCHYARD_UNICORN_SOURCE_REVISION")"
    printf '    "sourceArchive": %s,\n' \
      "$(json_string "$UNICORN_PACKAGE_ROOT_RELATIVE/share/src/switchyard-unicorn/unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz")"
    printf '    "sourceArchiveSha256": %s,\n' \
      "$(json_string "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256")"
    printf '    "sourcePatch": {\n'
    printf '      "path": %s,\n' "$(json_string "$UNICORN_SOURCE_PATCH_RELATIVE")"
    printf '      "sha256": %s\n' \
      "$(json_string "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256")"
    printf '    },\n'
    printf '    "buildContractVersion": %s,\n' \
      "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION"
    printf '    "hostArchitecture": "arm64",\n'
    printf '    "kuserSharedDataModel": %s,\n' \
      "$(json_string "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL")"
    printf '    "emulatedArchitectures": ["i386", "x86_64"],\n'
    printf '    "developmentCacheDigest": %s,\n' \
      "$(json_string "$unicorn_runtime_digest")"
    printf '    "runtimeRoot": %s,\n' \
      "$(json_string "$UNICORN_PACKAGE_ROOT_RELATIVE")"
    printf '    "runtimePayloadDigest": %s,\n' \
      "$(json_string "$unicorn_payload_digest")"
    printf '    "library": %s,\n' "$(json_string "$UNICORN_DYLIB_RELATIVE")"
    printf '    "librarySha256": %s,\n' "$(json_string "$unicorn_library_sha256")"
    printf '    "providerUnixLibraries": [\n'
    for index in "${!UNICORN_PROVIDER_UNIXLIBS[@]}"; do
      if [ "$index" -lt "$((${#UNICORN_PROVIDER_UNIXLIBS[@]} - 1))" ]; then
        printf '      %s,\n' "$(json_string "${UNICORN_PROVIDER_UNIXLIBS[$index]}")"
      else
        printf '      %s\n' "$(json_string "${UNICORN_PROVIDER_UNIXLIBS[$index]}")"
      fi
    done
    printf '    ],\n'
    printf '    "components": [\n'
    for index in "${!UNICORN_PROVIDER_UNIXLIBS[@]}"; do
      printf '      {\n'
      printf '        "guestArchitecture": %s,\n' \
        "$(json_string "${UNICORN_PROVIDER_GUEST_ARCHS[$index]}")"
      printf '        "unixLibrary": %s,\n' \
        "$(json_string "${UNICORN_PROVIDER_UNIXLIBS[$index]}")"
      printf '        "unixLibrarySha256": %s,\n' \
        "$(json_string "$(sha256_file "$WINE_INSTALL_PREFIX/${UNICORN_PROVIDER_UNIXLIBS[$index]}")")"
      printf '        "peLibrary": %s,\n' \
        "$(json_string "${UNICORN_PROVIDER_PE_LIBS[$index]}")"
      printf '        "peLibrarySha256": %s\n' \
        "$(json_string "$(sha256_file "$WINE_INSTALL_PREFIX/${UNICORN_PROVIDER_PE_LIBS[$index]}")")"
      if [ "$index" -lt "$((${#UNICORN_PROVIDER_UNIXLIBS[@]} - 1))" ]; then
        printf '      },\n'
      else
        printf '      }\n'
      fi
    done
    printf '    ],\n'
    printf '    "runtimeRpath": %s,\n' "$(json_string "$UNICORN_RUNTIME_RPATH")"
    printf '    "manifest": %s\n' \
      "$(json_string "$UNICORN_PACKAGE_ROOT_RELATIVE/switchyard-unicorn-runtime.json")"
    printf '  },\n'
  fi
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
  printf '  "gstreamerRuntime": {\n'
  printf '    "root": "lib/switchyard-gstreamer",\n'
  printf '    "digest": %s,\n' "$(json_string "$gstreamer_deps_digest")"
  printf '    "version": %s,\n' "$(json_string "$GSTREAMER_VERSION")"
  printf '    "architecture": %s,\n' \
    "$(json_string "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_ARCHITECTURE_DESCRIPTION")"
  printf '    "runtimePackage": %s,\n' "$(json_string "$GSTREAMER_RUNTIME_PACKAGE")"
  printf '    "runtimePackageUrl": %s,\n' \
    "$(json_string "$GSTREAMER_PACKAGE_BASE_URL/$GSTREAMER_RUNTIME_PACKAGE")"
  printf '    "runtimePackageSha256": %s,\n' "$(json_string "$GSTREAMER_RUNTIME_PACKAGE_SHA256")"
  printf '    "developmentPackage": %s,\n' "$(json_string "$GSTREAMER_DEVEL_PACKAGE")"
  printf '    "developmentPackageUrl": %s,\n' \
    "$(json_string "$GSTREAMER_PACKAGE_BASE_URL/$GSTREAMER_DEVEL_PACKAGE")"
  printf '    "developmentPackageSha256": %s,\n' "$(json_string "$GSTREAMER_DEVEL_PACKAGE_SHA256")"
  printf '    "pluginScanner": "lib/switchyard-gstreamer/libexec/gstreamer-1.0/gst-plugin-scanner",\n'
  printf '    "plugins": [\n'
  for index in "${!GSTREAMER_PLUGIN_FILES[@]}"; do
    if [ "$index" -lt "$((${#GSTREAMER_PLUGIN_FILES[@]} - 1))" ]; then
      printf '      %s,\n' "$(json_string "${GSTREAMER_PLUGIN_FILES[$index]}")"
    else
      printf '      %s\n' "$(json_string "${GSTREAMER_PLUGIN_FILES[$index]}")"
    fi
  done
  printf '    ],\n'
  printf '    "license": "LGPL and permissive dependency licenses; restricted codecs can be patent-encumbered in some jurisdictions",\n'
  printf '    "documentation": "lib/switchyard-gstreamer/share/doc/switchyard-gstreamer",\n'
  printf '    "licenseDirectory": "lib/switchyard-gstreamer/share/licenses"\n'
  printf '  },\n'
  printf '  "mesaOpenGL": {\n'
  printf '    "root": "lib/switchyard-mesa",\n'
  printf '    "digest": %s,\n' "$(json_string "$mesa_windows_digest")"
  printf '    "version": %s,\n' "$(json_string "$MESA_WINDOWS_VERSION")"
  printf '    "architectures": ["i386-windows", "x86_64-windows"],\n'
  printf '    "driver": "llvmpipe",\n'
  printf '    "selectionEnvironment": "WINE_OPENGL_DRIVER",\n'
  printf '    "selectionValue": "llvmpipe",\n'
  printf '    "archive": %s,\n' "$(json_string "$MESA_WINDOWS_ARCHIVE")"
  printf '    "archiveUrl": %s,\n' "$(json_string "$MESA_WINDOWS_ARCHIVE_URL")"
  printf '    "archiveSha256": %s,\n' "$(json_string "$MESA_WINDOWS_ARCHIVE_SHA256")"
  printf '    "x86_64OpenGL32Sha256": %s,\n' "$(json_string "$MESA_WINDOWS_X86_64_OPENGL_SHA256")"
  printf '    "x86_64GalliumSha256": %s,\n' "$(json_string "$MESA_WINDOWS_X86_64_GALLIUM_SHA256")"
  printf '    "i386OpenGL32Sha256": %s,\n' "$(json_string "$MESA_WINDOWS_I386_OPENGL_SHA256")"
  printf '    "i386GalliumSha256": %s,\n' "$(json_string "$MESA_WINDOWS_I386_GALLIUM_SHA256")"
  printf '    "sourceRepository": %s,\n' "$(json_string "$MESA_SOURCE_REPOSITORY")"
  printf '    "sourceRevision": %s,\n' "$(json_string "$MESA_SOURCE_REVISION")"
  printf '    "documentation": "lib/switchyard-mesa/share/doc/switchyard-mesa"\n'
  printf '  },\n'
  printf '  "fontRuntime": {\n'
  printf '    "root": "lib/switchyard-fonts",\n'
  printf '    "digest": %s,\n' "$(json_string "$font_deps_digest")"
  printf '    "architecture": %s,\n' "$(json_string "$HOST_MACHO_ARCH")"
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
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      printf '        "bottleTag": %s,\n' \
        "$(json_string "$SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG")"
    fi
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
  printf '    "emojiFallback": {"file": %s, "family": %s, "source": %s, "sha256": %s},\n' \
    "$(json_string "$FONT_EMOJI_FILE")" "$(json_string "$FONT_EMOJI_FAMILY")" \
    "$(json_string "$FONT_EMOJI_SOURCE")" "$(json_string "$FONT_EMOJI_SHA256")"
  printf '    "manifest": "lib/switchyard-fonts/share/doc/switchyard-font-assets/manifest.tsv"\n'
  printf '  },\n'
  if [ -n "$tls_deps_prefix" ]; then
    printf '  "tlsRuntime": {\n'
    printf '    "root": "lib/switchyard-tls",\n'
    printf '    "digest": %s,\n' "$(json_string "$tls_deps_digest")"
    printf '    "architecture": %s,\n' "$(json_string "$HOST_MACHO_ARCH")"
    if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
      printf '    "packageSubdir": %s,\n' "$(json_string "$TLS_PACKAGE_SUBDIR")"
    fi
    printf '    "dlopenName": %s,\n' "$(json_string "$TLS_DLOPEN_NAME")"
    printf '    "license": "redistributable conda-forge and source-built GnuTLS dependency closure with notices",\n'
    printf '    "packageManifest": "lib/switchyard-tls/share/doc/switchyard-tls/packages.tsv",\n'
    printf '    "sourceNote": "lib/switchyard-tls/share/doc/switchyard-tls/README.txt"\n'
    printf '  },\n'
  else
    printf '  "tlsRuntime": null,\n'
  fi
  printf '  "vulkanRuntime": {\n'
  printf '    "root": "lib/switchyard-vulkan",\n'
  printf '    "digest": %s,\n' "$(json_string "$vulkan_deps_digest")"
  printf '    "architecture": %s,\n' "$(json_string "$HOST_MACHO_ARCH")"
  printf '    "license": "Apache-2.0",\n'
  printf '    "icdFile": "lib/switchyard-vulkan/etc/vulkan/icd.d/MoltenVK_icd.json",\n'
  printf '    "vulkanLoader": {\n'
  printf '      "version": %s,\n' "$(json_string "$VULKAN_LOADER_VERSION")"
  printf '      "repository": %s,\n' "$(json_string "$VULKAN_LOADER_REPOSITORY")"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    printf '      "bottle": %s,\n' "$(json_string "$VULKAN_LOADER_BOTTLE")"
  fi
  printf '      "manifestDigest": %s,\n' "$(json_string "$VULKAN_LOADER_MANIFEST_DIGEST")"
  printf '      "layerSha256": %s\n' "$(json_string "$VULKAN_LOADER_LAYER_SHA256")"
  printf '    },\n'
  printf '    "vulkanHeaders": {\n'
  printf '      "version": %s,\n' "$(json_string "$VULKAN_HEADERS_VERSION")"
  printf '      "repository": %s,\n' "$(json_string "$VULKAN_HEADERS_REPOSITORY")"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    printf '      "bottle": %s,\n' "$(json_string "$VULKAN_HEADERS_BOTTLE")"
  fi
  printf '      "manifestDigest": %s,\n' "$(json_string "$VULKAN_HEADERS_MANIFEST_DIGEST")"
  printf '      "layerSha256": %s\n' "$(json_string "$VULKAN_HEADERS_LAYER_SHA256")"
  printf '    },\n'
  printf '    "moltenVK": {\n'
  printf '      "version": %s,\n' "$(json_string "$MOLTENVK_VERSION")"
  printf '      "repository": %s,\n' "$(json_string "$MOLTENVK_REPOSITORY")"
  if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
    printf '      "bottle": %s,\n' "$(json_string "$MOLTENVK_BOTTLE")"
  fi
  printf '      "manifestDigest": %s,\n' "$(json_string "$MOLTENVK_MANIFEST_DIGEST")"
  printf '      "layerSha256": %s\n' "$(json_string "$MOLTENVK_LAYER_SHA256")"
  printf '    }\n'
  printf '  }\n'
  printf '}\n'
} >"$WINE_INSTALL_PREFIX/switchyard-runtime.json"
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  switchyard_finalize_native_arm64_runtime_manifest \
    "$WINE_INSTALL_PREFIX" "$WINE_INSTALL_PREFIX/switchyard-runtime.json"
  switchyard_refresh_native_arm64_signed_runtime_manifest \
    "$WINE_INSTALL_PREFIX" "$WINE_INSTALL_PREFIX/switchyard-runtime.json"
fi
profile_validation_arguments=(
  "$WINE_INSTALL_PREFIX/switchyard-runtime.json"
  "$SWITCHYARD_RUNTIME_PROFILE"
)
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  profile_validation_arguments+=("$WINE_INSTALL_PREFIX")
fi
if ! switchyard_validate_runtime_manifest_profile "${profile_validation_arguments[@]}"; then
  echo "Refusing to publish a runtime with inconsistent profile metadata." >&2
  exit 1
fi
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] &&
   ! switchyard_validate_native_arm64_runtime_packaging \
     "$WINE_INSTALL_PREFIX" "$WINE_INSTALL_PREFIX/switchyard-runtime.json" "$ROOT_DIR"; then
  echo "Refusing to publish a native runtime with an invalid packaging closure." >&2
  exit 1
fi
runtime_content_sha256="$(write_runtime_content_tree_digest "$WINE_INSTALL_PREFIX")"

if ! runtime_is_complete_at "$WINE_INSTALL_PREFIX"; then
  echo "Refusing to publish an incomplete or internally inconsistent Wine runtime." >&2
  exit 1
fi

switchyard_require_native_arm64_ensure_host
if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  current_native_runtime_prefix="$(
    switchyard_canonical_runtime_install_prefix "$FINAL_WINE_INSTALL_PREFIX"
  )" || exit $?
  [ "$current_native_runtime_prefix" = "$FINAL_WINE_INSTALL_PREFIX" ] || {
    echo "Native runtime install prefix changed before publication." >&2
    exit 1
  }
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
    "$FINAL_WINE_INSTALL_PREFIX" || exit $?
fi
atomic_replace_directory "$WINE_INSTALL_PREFIX" "$FINAL_WINE_INSTALL_PREFIX" runtime \
  "$SWITCHYARD_RUNTIME_PROFILE"
WINE_INSTALL_PREFIX="$FINAL_WINE_INSTALL_PREFIX"
rm -rf "$INSTALL_STAGE_ROOT"
INSTALL_STAGE_ROOT=""

if [ "$MODE" = "--ensure" ]; then
  defaults write dev.switchyard.Switchyard winePath "$wine_executable"
  defaults write dev.switchyard.Switchyard 'activeRuntimeSourceRevision.v1' "$wine_revision"
  echo "configured Switchyard winePath=$wine_executable"
else
  echo "built Switchyard Wine runtime at $FINAL_WINE_INSTALL_PREFIX"
fi
echo "runtime content sha256: $runtime_content_sha256"
