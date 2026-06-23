# Packaging

Both scripts expect the project to build first:

```bash
cd evince-46.3.1 && meson setup build && ninja -C build
```

## .deb (Debian/Ubuntu)

```bash
packaging/build-deb.sh        # -> dist/evince-tts_46.3.1_amd64.deb
sudo apt install ./dist/evince-tts_46.3.1_amd64.deb
```

Installs under `/usr/local`, so it shadows the stock `evince` on `PATH` without
fighting the `evince` apt package (nothing owns `/usr/local`). Remove with
`sudo apt remove evince-tts`.

## AppImage (any x86_64 distro)

```bash
packaging/build-appimage.sh   # -> dist/Evince-TTS-x86_64.AppImage
```

First run downloads `linuxdeploy`, `linuxdeploy-plugin-gtk` and `appimagetool`
(needs network). The GTK plugin bundles GTK3 + poppler + gstreamer; the result
is a single self-contained executable.

## Arch / RPM

Not provided yet. A PKGBUILD or `.spec` would wrap the same `meson install`;
contributions welcome.
