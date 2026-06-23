# Evince TTS — Read PDFs aloud in a custom MiniMax voice

A fork of Ubuntu's stock **Document Viewer (GNOME Evince 46.3.1)** that adds a
**Read Aloud** feature: it pulls each page's text layer (via Evince's existing
Poppler extraction), synthesizes it sentence-by-sentence with the **MiniMax
T2A V2** API in your **custom cloned voice**, plays it with GStreamer, and
highlights the sentence being spoken — auto-advancing through the document.

> This is a community fork for personal/educational use. Evince is GPLv2+;
> these additions inherit that license. Not affiliated with GNOME or MiniMax.

## Features

- **Read Aloud** with sentence highlighting and automatic page advance.
- **Media bar** in the header: voice selector, **HD/Turbo** model toggle, speed
  dial, previous/next page, play-pause, a seek scrubber, and volume.
- **Persistent on-disk cache** — every clip is cached under
  `~/.cache/evince-tts/<doc-sha>/<voice>_<model>_s<speed>_p<pitch>/page-NNNN/`
  with a `page.json` (text + ordering) per page. Reopening a document reuses the
  audio instead of re-hitting the API; nothing is re-synthesized. The folder
  layout is designed so a script can later stitch the clips into an audiobook.
- **Idle pre-generation** — dwell on a page for 5 s and its window is
  synthesized in the background so playback starts instantly.
- **Sliding window** — the current page ±2 are kept warm; pages outside are
  evicted from memory (the on-disk cache stays).
- **Media keys / Bluetooth headphones** via MPRIS: play-pause, and the transport
  keys page the reader (Previous/Rewind → back, Next/FastForward → forward).
- **Debug console** (menu → *TTS Debug Console*) — a small scrolling window that
  logs every API call in curl form.
- Credentials in the GNOME keyring; everything else in GSettings. GroupId is
  optional on `api.minimax.io`.

## Build

```bash
sudo apt build-dep evince
sudo apt install -y meson libsoup-3.0-dev libjson-glib-dev libsecret-1-dev \
                    libgstreamer1.0-dev gstreamer1.0-plugins-good gstreamer1.0-libav
cd evince-46.3.1
meson setup build
ninja -C build
```

## Run (uninstalled)

```bash
./run.sh ~/some.pdf      # wraps `meson devenv` so the build tree works
```

## Install

```bash
sudo ninja -C build install     # installs to /usr/local (shadows apt's evince)
```

Then open any PDF, go to **☰ → TTS Settings**, paste your MiniMax **API key**,
click **Fetch voices**, pick your cloned voice, **Save**, and choose
**☰ → Read Aloud**.

## Packages

`packaging/build-deb.sh` builds a self-contained `.deb` (installs under
`/opt/evince-tts`, no conflict with the system `evince`).
`packaging/build-appimage.sh` builds an AppImage. See `packaging/README.md`.

## Layout of the TTS code (all under `evince-46.3.1/shell/`)

| File | Purpose |
|------|---------|
| `ev-tts-minimax.{c,h}` | MiniMax T2A V2 client (libsoup + json-glib), hex→mp3, voice list, `log` signal |
| `ev-tts-controller.{c,h}` | read loop, per-page sliding + on-disk cache, idle pre-gen, voice/speed/model |
| `ev-tts-bar.{c,h}` | header-bar media + voice/speed/model controls |
| `ev-tts-prefs.{c,h}` | settings dialog (key, region, voice, model, speed…) |
| `ev-tts-debug.{c,h}` | API-call console |
| `ev-tts-mpris.{c,h}` | MPRIS2 player for media keys |

Plus `ev_view_set_tts_highlight()` in `libview/ev-view.c` and the wiring in
`shell/ev-window.c`.
