# Evince TTS — Read PDFs aloud with cloud or local voices

A fork of Ubuntu's stock **Document Viewer (GNOME Evince 46.3.1)** that adds a
**Read Aloud** feature: it pulls each page's text layer (via Evince's existing
Poppler extraction), synthesizes it sentence-by-sentence through your chosen TTS
provider, plays it with GStreamer, and highlights the sentence being spoken —
auto-advancing through the document.

> This is a community fork for personal/educational use. Evince is GPLv2+;
> these additions inherit that license. Not affiliated with GNOME, MiniMax,
> OpenAI, OpenRouter or Google.

## Providers

Pick a provider in **☰ → TTS Settings**:

| Provider | What it is | Config |
|----------|-----------|--------|
| **MiniMax** | Speech 2.8 (`speech-2.8-hd` / `speech-2.8-turbo`) in your **custom cloned voice** | API key + region; *Fetch voices* lists your clones |
| **OpenAI** | `gpt-4o-mini-tts` / `tts-1` / `tts-1-hd` | API key (base `https://api.openai.com/v1`) |
| **OpenRouter** | OpenRouter's `/audio/speech` aggregator | API key (base `https://openrouter.ai/api/v1`) |
| **Local** | any **OpenAI-compatible** server (Kokoro-FastAPI, openedai-speech, LocalAI…) | base URL e.g. `http://localhost:8880/v1`; key usually blank |
| **Google** | Google Cloud Text-to-Speech | API key; voice like `en-US-Neural2-C` |

OpenAI, OpenRouter and Local share one backend (the OpenAI-compatible
`/audio/speech` endpoint) and differ only by base URL. The audio cache is keyed
by provider + voice + model + speed + pitch, so switching providers never
re-synthesizes work you've already paid for.

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
- **Batch pre-cache** — `EV_TTS_PRECACHE="50" evince book.pdf` synthesizes and
  caches the first 50 pages (or `"10-60"` for a range), sequentially and
  throttled, then quits. Uses the exact same cache keys as playback, so the
  reader reuses every clip. Resumable: already-cached sentences are skipped.
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
| `ev-tts-backend.{c,h}` | multi-provider client (MiniMax hex / OpenAI-compatible raw / Google base64), voice list, `log` signal |
| `ev-tts-controller.{c,h}` | read loop, per-page sliding + on-disk cache, idle pre-gen, voice/speed/model |
| `ev-tts-bar.{c,h}` | header-bar media + voice/speed/model controls |
| `ev-tts-prefs.{c,h}` | settings dialog (key, region, voice, model, speed…) |
| `ev-tts-debug.{c,h}` | API-call console |
| `ev-tts-mpris.{c,h}` | MPRIS2 player for media keys |

Plus `ev_view_set_tts_highlight()` in `libview/ev-view.c` and the wiring in
`shell/ev-window.c`.
