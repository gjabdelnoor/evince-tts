#!/usr/bin/env bash
# Build a self-contained AppImage. Best-effort: downloads linuxdeploy + the GTK
# plugin + appimagetool on first run (needs network). Bundles GTK3, poppler,
# gstreamer and friends so it runs on most x86_64 distros.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$HERE/evince-46.3.1"
TOOLS="$HERE/dist/tools"
APPDIR="$HERE/dist/AppDir"
mkdir -p "$TOOLS" "$HERE/dist"

fetch() {  # url dest
        [ -f "$2" ] || { echo "fetching $(basename "$2")"; wget -q -O "$2" "$1"; chmod +x "$2"; }
}
B=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous
G=https://github.com/linuxdeploy/linuxdeploy-plugin-gtk/releases/download/continuous
A=https://github.com/AppImage/AppImageKit/releases/download/continuous
fetch "$B/linuxdeploy-x86_64.AppImage"            "$TOOLS/linuxdeploy.AppImage"
fetch "$G/linuxdeploy-plugin-gtk.sh"              "$TOOLS/linuxdeploy-plugin-gtk.sh"
fetch "$A/appimagetool-x86_64.AppImage"           "$TOOLS/appimagetool.AppImage"

# Fresh /usr-prefixed build staged into the AppDir.
rm -rf "$APPDIR" "$SRC/build-appimage"
meson setup "$SRC/build-appimage" "$SRC" --prefix=/usr -Dgtk_doc=false >/dev/null
ninja -C "$SRC/build-appimage" >/dev/null
DESTDIR="$APPDIR" ninja -C "$SRC/build-appimage" install >/dev/null

export PATH="$TOOLS:$PATH"
export OUTPUT="$HERE/dist/Evince-TTS-x86_64.AppImage"
export DEPLOY_GTK_VERSION=3

"$TOOLS/linuxdeploy.AppImage" --appdir "$APPDIR" \
        --plugin gtk \
        --desktop-file "$APPDIR/usr/share/applications/org.gnome.Evince.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/org.gnome.Evince.svg" \
        --output appimage

echo "built: $OUTPUT"
