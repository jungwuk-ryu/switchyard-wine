#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONFIGURE_SCRIPT="$ROOT_DIR/configure"
TEST_ROOT="$(mktemp -d)"

cleanup() {
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

fail() {
  echo "$1" >&2
  exit 1
}

if [ "$(uname -s)" != "Darwin" ]; then
  echo "skipping Darwin loader linker flag test on non-Darwin host"
  exit 0
fi

if [ -x /opt/homebrew/opt/bison/bin/bison ]; then
  BISON_PATH="/opt/homebrew/opt/bison/bin:$PATH"
else
  BISON_PATH="$PATH"
fi

if ! env PATH="$BISON_PATH" bison --version | awk 'NR == 1 { split($NF, version, "."); exit(version[1] >= 3 ? 0 : 1) }'; then
  fail "bison 3.0 or newer is required for configure-time loader flag tests"
fi

CONFIGURE_OPTIONS=(
  --disable-tests
  --without-alsa
  --without-capi
  --without-cups
  --without-dbus
  --without-fontconfig
  --without-freetype
  --without-gphoto
  --without-gssapi
  --without-gstreamer
  --without-inotify
  --without-krb5
  --without-netapi
  --without-opencl
  --without-oss
  --without-pcap
  --without-pcsclite
  --without-pulse
  --without-sane
  --without-sdl
  --without-udev
  --without-usb
  --without-v4l2
  --without-vulkan
  --without-x
  --without-xcomposite
  --without-xcursor
  --without-xfixes
  --without-xinerama
  --without-xinput
  --without-xinput2
  --without-xrandr
  --without-xrender
  --without-xshape
  --without-xshm
  --without-xxf86vm
)

configure_loader_flags() {
  local label="$1"
  local build_triplet="$2"
  local host_triplet="$3"
  local pe_archs="$4"
  local build_dir="$TEST_ROOT/$label"
  local compiler_arch
  local -a command_prefix=()
  local -a command
  local -a environment
  shift 4
  if [ "$#" -gt 0 ]; then
    command_prefix=("$@")
  fi

  case "$host_triplet" in
    aarch64-*) compiler_arch=arm64 ;;
    x86_64-*) compiler_arch=x86_64 ;;
    *) fail "$label test does not know the compiler architecture for $host_triplet" ;;
  esac
  environment=(
    "PATH=$BISON_PATH"
    "CC=clang -arch $compiler_arch"
    "CXX=clang++ -arch $compiler_arch"
    "OBJC=clang -arch $compiler_arch"
  )

  mkdir -p "$build_dir"
  if [ "${#command_prefix[@]}" -gt 0 ]; then
    command=("${command_prefix[@]}" env "${environment[@]}" "$CONFIGURE_SCRIPT")
  else
    command=(env "${environment[@]}" "$CONFIGURE_SCRIPT")
  fi
  command+=(
    --build="$build_triplet"
    --host="$host_triplet"
    --enable-archs="$pe_archs"
    "${CONFIGURE_OPTIONS[@]}")
  if ! (cd "$build_dir" && "${command[@]}" >configure.out 2>configure.err); then
    cat "$build_dir/configure.err" >&2
    fail "$label configure failed"
  fi

  awk -F ' = ' '$1 == "WINELOADER_LDFLAGS" { print $2; found = 1 } END { exit found ? 0 : 1 }' \
    "$build_dir/Makefile" || fail "$label configure did not emit WINELOADER_LDFLAGS"
}

aarch64_loader_flags="$(configure_loader_flags \
  aarch64 aarch64-apple-darwin aarch64-apple-darwin aarch64)"

case "$aarch64_loader_flags" in
  *-pagezero_size,0x1000*|*-segalign,0x1000*)
    fail "aarch64 Wine loader still uses low Intel pagezero linker flags: $aarch64_loader_flags"
    ;;
esac
case "$aarch64_loader_flags" in
  *-sectcreate,__TEXT,__info_plist,loader/wine_info.plist*) ;;
  *) fail "aarch64 Wine loader lost the embedded info plist linker flag: $aarch64_loader_flags" ;;
esac

cat >"$TEST_ROOT/main.c" <<'EOF'
int main(void) { return 7; }
EOF
wine_info_plist="$TEST_ROOT/wine_info.plist"
cat >"$wine_info_plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict/></plist>
EOF
safe_aarch64_loader_flags="${aarch64_loader_flags//loader\/wine_info.plist/$wine_info_plist}"
# shellcheck disable=SC2206 # Configure emits trusted linker flags from this source tree.
safe_aarch64_loader_flag_args=($safe_aarch64_loader_flags)
clang -arch arm64 "$TEST_ROOT/main.c" -o "$TEST_ROOT/aarch64-loader-probe" \
  "${safe_aarch64_loader_flag_args[@]}"
set +e
"$TEST_ROOT/aarch64-loader-probe"
probe_status=$?
set -e
[ "$probe_status" -eq 7 ] || {
  fail "aarch64 Mach-O linked with Wine loader flags did not enter main; exit status $probe_status"
}

if arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
  x86_64_loader_flags="$(configure_loader_flags \
    x86_64 x86_64-apple-darwin x86_64-apple-darwin i386,x86_64 arch -x86_64)"
  case "$x86_64_loader_flags" in
    *-pagezero_size,0x1000* ) ;;
    *) fail "x86_64 Wine loader lost its low pagezero linker flag: $x86_64_loader_flags" ;;
  esac
  case "$x86_64_loader_flags" in
    *-segalign,0x1000* ) ;;
    *) fail "x86_64 Wine loader lost its 4 KiB segment alignment flag: $x86_64_loader_flags" ;;
  esac
  case "$x86_64_loader_flags" in
    *-sectcreate,__TEXT,__info_plist,loader/wine_info.plist*) ;;
    *) fail "x86_64 Wine loader lost the embedded info plist linker flag: $x86_64_loader_flags" ;;
  esac
else
  echo "skipping x86_64 configure comparison because Rosetta is unavailable"
fi

echo "Darwin loader linker flag tests passed"
