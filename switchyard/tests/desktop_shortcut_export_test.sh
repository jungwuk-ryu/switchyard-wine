#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-}"
[ -n "$BUILD_DIR" ] || { echo "usage: $0 WINE_BUILD_DIR" >&2; exit 2; }
[ -x "$BUILD_DIR/wine" ] || { echo "Wine build launcher is missing" >&2; exit 1; }
[ -x "$BUILD_DIR/server/wineserver" ] || { echo "Wine build server is missing" >&2; exit 1; }

work="$(/usr/bin/mktemp -d /tmp/switchyard-desktop-export.XXXXXX)"
prefix="$work/prefix"
manifest="$prefix/drive_c/windows/temp/switchyard-desktop-shortcuts-v1.txt"
windows_manifest='C:\windows\temp\switchyard-desktop-shortcuts-v1.txt'
monitor_pid=""
wine_debug="${SWITCHYARD_TEST_WINEDEBUG:--all}"

cleanup() {
  if [ -n "$monitor_pid" ]; then
    kill -TERM "$monitor_pid" >/dev/null 2>&1 || true
    wait "$monitor_pid" >/dev/null 2>&1 || true
  fi
  WINEPREFIX="$prefix" "$BUILD_DIR/server/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$BUILD_DIR/server/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

run_wine() {
  WINEPREFIX="$prefix" \
  WINEDEBUG="$wine_debug" \
  SWITCHYARD_PRIVATE_DESKTOP=1 \
  SWITCHYARD_DESKTOP_SHORTCUTS_FILE="$windows_manifest" \
    "$BUILD_DIR/wine" "$@"
}

decode_hex() {
  /usr/bin/perl -e 'print pack("H*", $ARGV[0])' "$1"
}

run_wine wineboot.exe -u >/dev/null 2>&1
user_dir="$(find "$prefix/drive_c/users" -mindepth 1 -maxdepth 1 -type d ! -name Public | head -n 1)"
[ -n "$user_dir" ] || { echo "Wine user directory was not created" >&2; exit 1; }
desktop="$user_dir/Desktop"
[ -d "$desktop" ] || { echo "private Wine desktop was not created" >&2; exit 1; }
[ ! -L "$desktop" ] || { echo "Wine desktop still points at the host desktop" >&2; exit 1; }

printf '[InternetShortcut]\nURL=https://example.invalid/first\n' >"$desktop/Switchyard Test.url"
run_wine winemenubuilder.exe -a >/dev/null 2>&1
[ -f "$manifest" ] || { echo "desktop shortcut manifest was not created" >&2; exit 1; }
[ "$(sed -n '1p' "$manifest")" = '# switchyard-wine-desktop-shortcuts-v1' ]

IFS=$'\t' read -r kind display_hex source_hex icon_hex < <(sed -n '2p' "$manifest")
[ "$kind" = url ]
[ "$(decode_hex "$display_hex")" = 'Switchyard Test' ]
case "$(decode_hex "$source_hex")" in
  *'\Desktop\Switchyard Test.url') ;;
  *) echo "manifest exported an unexpected shortcut path" >&2; exit 1 ;;
esac
[ -n "$icon_hex" ] || { echo "desktop shortcut icon was not exported" >&2; exit 1; }
icon_windows="$(decode_hex "$icon_hex")"
icon_relative="${icon_windows#C:\\}"
icon_host="$prefix/drive_c/${icon_relative//\\//}"
[ -s "$icon_host" ] || { echo "desktop shortcut icon file is missing" >&2; exit 1; }

cat >"$work/create-shortcut.vbs" <<'VBS'
Set shell = CreateObject("WScript.Shell")
Set shortcut = shell.CreateShortcut(shell.SpecialFolders("Desktop") & "\Switchyard Link.lnk")
shortcut.TargetPath = "C:\windows\notepad.exe"
shortcut.Save
VBS
windows_vbs="Z:${work//\//\\}\\create-shortcut.vbs"
run_wine wscript.exe //nologo "$windows_vbs" >/dev/null 2>&1
link_hex="$(printf 'Switchyard Link' | xxd -p -c 256)"
for _ in {1..100}; do
  if grep -q "^lnk[[:space:]]${link_hex}[[:space:]]" "$manifest"; then
    break
  fi
  sleep 0.05
done
grep -q "^lnk[[:space:]]${link_hex}[[:space:]]" "$manifest"
IFS=$'\t' read -r link_kind link_display_hex link_source_hex link_icon_hex < <(
  awk -F '\t' '$1 == "lnk" { print; exit }' "$manifest"
)
[ "$link_kind" = lnk ]
[ "$(decode_hex "$link_display_hex")" = 'Switchyard Link' ]
case "$(decode_hex "$link_source_hex")" in
  *'\Desktop\Switchyard Link.lnk') ;;
  *) echo "manifest exported an unexpected shell link path" >&2; exit 1 ;;
esac
[ -n "$link_icon_hex" ] || { echo "shell link icon was not exported" >&2; exit 1; }
link_icon_windows="$(decode_hex "$link_icon_hex")"
link_icon_relative="${link_icon_windows#C:\\}"
link_icon_host="$prefix/drive_c/${link_icon_relative//\\//}"
[ -s "$link_icon_host" ] || { echo "shell link icon file is missing" >&2; exit 1; }

run_wine winemenubuilder.exe -m >/dev/null 2>&1 &
monitor_pid=$!
printf '[InternetShortcut]\nURL=https://example.invalid/second\n' >"$desktop/Second Shortcut.url"
second_hex="$(printf 'Second Shortcut' | xxd -p -c 256)"
for _ in {1..100}; do
  if grep -q "^url[[:space:]]${second_hex}[[:space:]]" "$manifest"; then
    break
  fi
  sleep 0.05
done
grep -q "^url[[:space:]]${second_hex}[[:space:]]" "$manifest"

rm "$desktop/Switchyard Test.url"
for _ in {1..100}; do
  if [ "$(wc -l <"$manifest" | tr -d ' ')" = 3 ]; then
    break
  fi
  sleep 0.05
done
[ "$(wc -l <"$manifest" | tr -d ' ')" = 3 ]

echo "desktop shortcut export test passed"
