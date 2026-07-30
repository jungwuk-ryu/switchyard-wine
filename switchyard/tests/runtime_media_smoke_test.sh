#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <runtime> [asf-wmv3-media]" >&2
  exit 2
fi

RUNTIME="${1%/}"
MEDIA_ASSET="${2:-}"
GSTREAMER_ROOT="$RUNTIME/lib/switchyard-gstreamer"
PLUGIN_DIR="$GSTREAMER_ROOT/lib/gstreamer-1.0"
PLUGIN_SCANNER="$GSTREAMER_ROOT/libexec/gstreamer-1.0/gst-plugin-scanner"
GST_INSPECT="$GSTREAMER_ROOT/bin/gst-inspect-1.0"
GST_LAUNCH="$GSTREAMER_ROOT/bin/gst-launch-1.0"
WINEGSTREAMER="$RUNTIME/lib/wine/x86_64-unix/winegstreamer.so"
WINE_WRAPPER="$RUNTIME/bin/switchyard-wine"

[ -d "$RUNTIME" ] || {
  echo "runtime does not exist: $RUNTIME" >&2
  exit 1
}
[ -x "$GST_INSPECT" ] || {
  echo "runtime is missing gst-inspect-1.0" >&2
  exit 1
}
[ -x "$GST_LAUNCH" ] || {
  echo "runtime is missing gst-launch-1.0" >&2
  exit 1
}
[ -x "$PLUGIN_SCANNER" ] || {
  echo "runtime is missing the GStreamer plugin scanner" >&2
  exit 1
}
[ -f "$WINEGSTREAMER" ] || {
  echo "runtime is missing Wine's x86_64 GStreamer backend" >&2
  exit 1
}
[ -x "$WINE_WRAPPER" ] || {
  echo "runtime is missing the Switchyard Wine wrapper" >&2
  exit 1
}

if ! /usr/bin/otool -L "$WINEGSTREAMER" |
     /usr/bin/grep -F '@rpath/libgstreamer-1.0.0.dylib' >/dev/null; then
  echo "Wine's media backend is not linked to GStreamer" >&2
  exit 1
fi
if ! /usr/bin/otool -l "$WINEGSTREAMER" |
     /usr/bin/grep -F '@loader_path/../../switchyard-gstreamer/lib' >/dev/null; then
  echo "Wine's media backend has no runtime-relative GStreamer search path" >&2
  exit 1
fi
if ! /usr/bin/grep -F \
     'export GST_REGISTRY="$gstreamer_registry_dir/registry-x86_64.bin"' \
     "$WINE_WRAPPER" >/dev/null ||
   ! /usr/bin/grep -F 'export GST_REGISTRY_1_0="$GST_REGISTRY"' \
     "$WINE_WRAPPER" >/dev/null; then
  echo "Wine wrapper does not isolate its GStreamer registry from the host" >&2
  exit 1
fi

WORK="$(/usr/bin/mktemp -d /tmp/switchyard-runtime-media.XXXXXX)"
cleanup() {
  /bin/rm -rf "$WORK"
}
trap cleanup EXIT

run_gstreamer() {
  /usr/bin/env -i \
    HOME="$HOME" \
    PATH="/usr/bin:/bin" \
    TMPDIR="${TMPDIR:-/tmp}" \
    GST_PLUGIN_SYSTEM_PATH_1_0="$PLUGIN_DIR" \
    GST_PLUGIN_PATH_1_0= \
    GST_PLUGIN_SCANNER_1_0="$PLUGIN_SCANNER" \
    GST_REGISTRY_1_0="$WORK/registry-x86_64.bin" \
    GST_REGISTRY_FORK=no \
    /usr/bin/arch -x86_64 "$@"
}

for feature in \
  asfdemux avdec_wmv3 avdec_wmapro audioconvert audioresample \
  decodebin deinterlace videoconvert videoflip; do
  run_gstreamer "$GST_INSPECT" "$feature" >/dev/null
done

if [ -n "$MEDIA_ASSET" ]; then
  [ -f "$MEDIA_ASSET" ] || {
    echo "media asset does not exist: $MEDIA_ASSET" >&2
    exit 1
  }
  run_gstreamer "$GST_LAUNCH" -q \
    filesrc location="$MEDIA_ASSET" ! asfdemux name=demux \
    demux.video_0 ! queue ! avdec_wmv3 ! videoconvert ! fakesink sync=false
  echo "verified Wine GStreamer linkage, ASF/WMV3/WMA Pro plugins, and full WMV3 media decoding"
else
  echo "verified Wine GStreamer linkage and ASF/WMV3/WMA Pro plugin availability; no media asset was decoded"
fi
