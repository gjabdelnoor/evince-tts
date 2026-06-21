# Evince TTS — "Read Aloud" with MiniMax custom voice

A fork of Ubuntu's stock **Document Viewer (Evince 46.3.1)** that adds a
**Read Aloud** feature: it pulls each page's text layer (via Evince's existing
Poppler extraction), synthesizes it sentence-by-sentence with the **MiniMax
T2A V2** API using your **custom cloned voice**, plays it with GStreamer, and
highlights the sentence currently being spoken — auto-advancing through pages.

## What was added (on top of stock evince 46.3.1)

| File | Purpose |
|------|---------|
| `shell/ev-tts-minimax.{c,h}` | Async HTTPS POST to MiniMax `t2a_v2` (libsoup-3.0 + json-glib); hex→MP3 decode |
| `shell/ev-tts-controller.{c,h}` | Read loop: sentence split, synth queue/prefetch, GStreamer playback, page advance, highlight dispatch |
| `shell/ev-tts-prefs.{c,h}` | Settings dialog (API key, region, GroupId, voice id, model, speed/vol/pitch) |
| `libview/ev-view.c` (+ private/header) | `ev_view_set_tts_highlight()` — draws the active-sentence overlay, reusing the find-highlight path |
| `shell/ev-window.c` | `win.read-aloud` toggle + `win.tts-preferences` actions, controller lifecycle, status info-bar |
| `shell/evince-toolbar.ui` | "Read Aloud" + "TTS Settings…" items in the primary menu |
| `data/org.gnome.Evince.gschema.xml` | `tts-*` settings keys |
| `meson.build`, `shell/meson.build` | libsoup-3.0 + json-glib deps, new sources |

The **API key** is stored in the GNOME keyring (via Evince's existing
`ev-keyring`), never in a file. Everything else lives in GSettings.

## Build

```bash
sudo apt build-dep evince
sudo apt install -y meson libsoup-3.0-dev libjson-glib-dev libsecret-1-dev \
                    libgstreamer1.0-dev gstreamer1.0-plugins-good gstreamer1.0-libav
cd evince-46.3.1
meson setup build
ninja -C build
```

## Run

```bash
./run.sh ~/some.pdf        # wraps `meson devenv` so the uninstalled build works
```

Then: primary menu → **TTS Settings…** → enter your MiniMax **API key**,
**GroupId**, **Voice ID**, pick the **region**, Save. Open a text PDF and choose
**Read Aloud** (or toggle it from the menu).

> Note: requires MP3 decoding — `gstreamer1.0-plugins-good`/`-libav`.

## Status / limitations

- Built & launches cleanly against Ubuntu 24.04's evince 46.3.1.
- End-to-end voice playback needs your MiniMax **GroupId** + cloned **voice_id**
  (entered in TTS Settings) — not yet exercised here.
- Scanned/image-only pages (no text layer) are skipped (no OCR).
- Highlight uses per-sentence boundaries; no word-level karaoke timing.
