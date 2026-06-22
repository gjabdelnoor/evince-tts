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

/* Re-read credentials/voice parameters from GSettings + keyring. */
void     ev_tts_controller_reload_config (EvTtsController *self);

/* Emitted ("notify::active", "notify::paused") so the UI can update controls.
 * A "status" signal carries a human-readable string for an info bar. */

G_END_DECLS
