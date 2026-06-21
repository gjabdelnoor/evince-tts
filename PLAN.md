# Evince TTS Fork — "Read Aloud" with MiniMax Custom Voice

## Context
You want the familiar Ubuntu stock document viewer, but with a built-in audio
player that reads each page aloud in your **MiniMax custom-cloned voice**.
Electron was the first idea, but Evince (Ubuntu's "Document Viewer") is a GTK/C
app and can't be embedded in Electron — so we instead **fork Evince itself** and
add a TTS panel. This is the better fit anyway: Evince already extracts per-page
text via Poppler and already renders highlight overlays for its find feature, so
the per-page text layer and the sentence-highlight playback you asked for map
directly onto existing Evince machinery.

Target: **evince 46.3.1** (the exact version on this Ubuntu 24.04 box).
Stack: C / GTK3 / meson — **not** Electron/JS.

## Decisions (confirmed)
- **Base:** Fork stock Evince 46, add a TTS module. Reuse Evince's Poppler text
  extraction and find/highlight rendering.
- **Playback:** Per-page, **with sentence highlighting** + auto-advance to next page.
- **Credentials:** In-app Settings dialog; API key in **libsecret** (keyring,
  encrypted), non-secret config (GroupId, voice_id, region, model, speed/vol/pitch)
  in **GSettings**. Nothing secret committed to source.

## MiniMax T2A V2 contract (verified)
- `POST https://api.minimax.io/v1/t2a_v2?GroupId=<GroupId>`
  (region-selectable: `api.minimaxi.chat` for CN, `api-uw.minimax.io` for US-West)
- Headers: `Authorization: Bearer <api_key>`, `Content-Type: application/json`
- Body: `{ "model":"speech-2.6-hd", "text":"...", "voice_setting":{ "voice_id":"<custom>",
  "speed":1.0,"vol":1.0,"pitch":0 }, "audio_setting":{ "format":"mp3","sample_rate":32000 } }`
- Response: JSON → `data.audio` is a **hex string**; decode hex → MP3 bytes → play.
  `text` cap 10k chars (sentence-sized requests stay well under).

## Architecture
Synthesize **one clip per sentence**, play sequentially, highlight the active
sentence while its clip plays, prefetch the next. No word-level timestamps needed —
sentence boundaries drive both the highlight and the audio queue.

Per-page reading loop:
1. Get current page from `EvDocumentModel` (`ev_document_model_get_page`).
2. Extract text: `ev_document_text_get_text(EV_DOCUMENT_TEXT(doc), page)` and
   per-char rects: `ev_document_text_get_text_layout(...)`.
3. Split text into sentences, tracking `[start,end)` char offsets. Slice the rect
   array per sentence → bounding highlight rectangles.
4. For each sentence: POST to MiniMax (async, libsoup3), cache the MP3; prefetch
   next 1–2 sentences.
5. Play clip via GStreamer; set the active-sentence highlight on `EvView` → redraw.
6. On clip EOS → next sentence. Page exhausted → `ev_document_model_set_page(+1)`,
   repeat. Pages with no text (scanned) → skip with a brief notification (OCR is
   out of scope).
7. Toolbar controls: Read-aloud toggle, Play/Pause, Stop, next/prev sentence.

## Files

### New
- `shell/ev-tts-controller.{c,h}` — orchestrates the read loop: sentence splitting,
  synthesis queue + cache, GStreamer playback, page advance, highlight dispatch.
  Owns state (idle/playing/paused), bound to the `EvWindow`'s `EvDocumentModel`.
- `shell/ev-tts-minimax.{c,h}` — libsoup-3.0 `SoupSession` async POST; json-glib to
  build the request and parse `data.audio`; hex→bytes decode. Reads creds from
  settings/keyring.
- `shell/ev-tts-settings.{c,h}` — GSettings wrapper + libsecret get/store for the
  API key (`SecretSchema`).
- `shell/ev-tts-prefs.c` — small GtkDialog: API key, GroupId, voice_id, region,
  model, speed/vol/pitch. "Test voice" button.
- `data/org.gnome.Evince.tts.gschema.xml` — non-secret keys.

### Modified
- `meson.build` — add `dependency('libsoup-3.0')` and `dependency('json-glib-1.0')`;
  GStreamer + libsecret are already optional deps — make them required for this build.
- `shell/ev-window.c` — add the Read-aloud header-bar/toolbar control + menu item,
  instantiate `EvTtsController`, open the prefs dialog, wire page-change.
- `libview/ev-view.{c,h}` — add `ev_view_set_tts_highlight(view, page, EvRectangle*,
  n)` (and a clear); draw it by reusing the existing find/selection highlight overlay
  path in `ev_view`'s `draw`/highlight code, in its own color.

## Reused Evince machinery (don't reinvent)
- Text + char rects: `ev_document_text_get_text`, `ev_document_text_get_text_layout`
  (`libdocument/ev-document-text.h`), backed by `poppler_page_get_text*` in
  `backend/pdf/ev-poppler.cc`.
- Page navigation/state: `EvDocumentModel` (`libview/ev-document-model.h`).
- Highlight rendering: existing find-match highlight drawing in `libview/ev-view.c`.
- Async I/O on the GLib main loop: `SoupSession` (no manual threads).
- Secret storage: `libsecret` (already an Evince optional dep).
- Audio: GStreamer `playbin` fed from in-memory MP3 (`GBytes` →
  `giostreamsrc`/`GMemoryInputStream`), or a temp file fallback.

## Build & dependencies
```
sudo apt build-dep evince
sudo apt install meson ninja-build libsoup-3.0-dev libjson-glib-dev \
                 libsecret-1-dev libgstreamer1.0-dev
apt source evince            # fetch the exact 46.3.1-0ubuntu1.1 tree
cd evince-46.3.1
meson setup build && ninja -C build
./build/shell/evince ~/some.pdf
```

## Verification
1. **Synthesis harness first** (de-risk before UI): tiny C/`curl` test that POSTs one
   sentence with your real key/GroupId/voice_id, decodes the hex, writes `out.mp3`,
   confirms it plays in your voice. (Audio plays through a real sink only with your
   say-so per the no-audible-tests rule — otherwise route to a null sink/monitor.)
2. Open a text-based PDF, enter creds in Settings → "Test voice" succeeds.
3. Toggle Read-aloud: hear the page in the custom voice; watch the active sentence
   highlight advance; confirm auto page-flip at page end.
4. Pause/resume/stop behave; switching pages mid-read re-syncs.
5. Scanned/no-text page → graceful skip notification.
6. Confirm the API key is in the keyring (`secret-tool search ...`), not in any file.

## Out of scope (note)
OCR for scanned PDFs; word-level karaoke timing; non-PDF backends (DjVu/PS) — text
extraction works for those too but highlight rects are PDF-tested only.
