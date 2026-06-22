/* ev-tts-bar.h — compact media-player control strip for Read Aloud. */
#pragma once

#include <gtk/gtk.h>
#include "ev-tts-controller.h"

G_BEGIN_DECLS

#define EV_TYPE_TTS_BAR (ev_tts_bar_get_type ())
G_DECLARE_FINAL_TYPE (EvTtsBar, ev_tts_bar, EV, TTS_BAR, GtkBox)

/* Controls (and reflects) the given controller. The controller is not owned. */
GtkWidget *ev_tts_bar_new (EvTtsController *controller);

G_END_DECLS
