/* ev-tts-controller.h
 *
 * Drives "Read aloud": pulls the current page's text layer from the document,
 * splits it into sentences, synthesizes each via MiniMax, plays them in order
 * with GStreamer, highlights the active sentence in the view, and auto-advances
 * to the next page.
 */
#pragma once

#include <gtk/gtk.h>
#include "ev-view.h"
#include "ev-document-model.h"

G_BEGIN_DECLS

#define EV_TYPE_TTS_CONTROLLER (ev_tts_controller_get_type ())
G_DECLARE_FINAL_TYPE (EvTtsController, ev_tts_controller, EV, TTS_CONTROLLER, GObject)

EvTtsController *ev_tts_controller_new (EvView          *view,
                                        EvDocumentModel *model);

/* Begin/stop reading. Starting reads from the model's current page. */
void     ev_tts_controller_start  (EvTtsController *self);
void     ev_tts_controller_stop   (EvTtsController *self);
void     ev_tts_controller_toggle (EvTtsController *self);
gboolean ev_tts_controller_is_active (EvTtsController *self);

/* Pause/resume playback without losing position. */
void     ev_tts_controller_set_paused (EvTtsController *self, gboolean paused);
gboolean ev_tts_controller_get_paused (EvTtsController *self);

/* Media-player controls. */
void     ev_tts_controller_next_page (EvTtsController *self);
void     ev_tts_controller_prev_page (EvTtsController *self);
void     ev_tts_controller_set_volume (EvTtsController *self, double volume); /* 0..1 */
double   ev_tts_controller_get_volume (EvTtsController *self);
gboolean ev_tts_controller_get_progress (EvTtsController *self,
                                         gint64 *pos_ns, gint64 *dur_ns);
void     ev_tts_controller_seek_fraction (EvTtsController *self, double fraction);

/* Voice / speed (persisted to GSettings; invalidates the audio cache). */
void     ev_tts_controller_set_voice (EvTtsController *self, const char *voice);
void     ev_tts_controller_set_speed (EvTtsController *self, double speed);
void     ev_tts_controller_set_model (EvTtsController *self, const char *model);
char    *ev_tts_controller_dup_voice (EvTtsController *self);   /* free with g_free */
char    *ev_tts_controller_dup_model (EvTtsController *self);   /* free with g_free */
char    *ev_tts_controller_dup_provider (EvTtsController *self);/* free with g_free */
double   ev_tts_controller_get_speed (EvTtsController *self);

/* Asynchronously fetch cloned voices; result delivered via "voices-changed". */
void     ev_tts_controller_refresh_voices (EvTtsController *self);

/* Drop all cached audio and re-warm (e.g. after a voice/speed/key change). */
void     ev_tts_controller_invalidate_cache (EvTtsController *self);

/* Re-read credentials/voice parameters from GSettings + keyring. */
void     ev_tts_controller_reload_config (EvTtsController *self);

/* Signals:
 *   "status"  (gchar*)  — human-readable status for the info bar
 *   "log"     (gchar*)  — one API-call line (curl form) for the debug console
 *   "voices-changed" (GStrv) — cloned voice ids from refresh_voices()
 *   notify::active / notify::paused — playback state for the UI */

G_END_DECLS
