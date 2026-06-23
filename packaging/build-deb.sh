#!/usr/bin/env bash
# Build a .deb that installs the fork under /usr/local (shadows the system
# evince via PATH; no dpkg file conflicts since no package owns /usr/local).
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$HERE/evince-46.3.1"
STAGE="$HERE/dist/deb"
VER=46.3.1
ARCH=$(dpkg --print-architecture)

if [ ! -f "$SRC/build/build.ninja" ]; then
        echo "Configure + build first:  (cd evince-46.3.1 && meson setup build && ninja -C build)" >&2
        exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE" "$HERE/dist"
meson install -C "$SRC/build" --destdir "$STAGE" --no-rebuild >/dev/null

INSTALLED_KB=$(du -sk "$STAGE" | cut -f1)

install -d "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: evince-tts
Version: $VER
Section: text
Priority: optional
Architecture: $ARCH
Maintainer: gjabdelnoor <noreply@users.noreply.github.com>
Installed-Size: $INSTALLED_KB
Depends: libgtk-3-0, libpoppler-glib8, libhandy-1-0, libsoup-3.0-0, libjson-glib-1.0-0, libsecret-1-0, libgstreamer1.0-0, gstreamer1.0-plugins-good, gstreamer1.0-libav
Description: Evince fork that reads PDFs aloud in a custom MiniMax TTS voice
 Read Aloud with sentence highlighting, a persistent on-disk audio cache,
 HD/Turbo selection, media-key control and a custom cloned voice. Installs
 under /usr/local and shadows the stock Document Viewer.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
glib-compile-schemas /usr/local/share/glib-2.0/schemas 2>/dev/null || true
update-desktop-database /usr/local/share/applications 2>/dev/null || true
ldconfig || true
EOF
chmod 0755 "$STAGE/DEBIAN/postinst"

OUT="$HERE/dist/evince-tts_${VER}_${ARCH}.deb"
fakeroot dpkg-deb --build "$STAGE" "$OUT"
echo "built: $OUT"
