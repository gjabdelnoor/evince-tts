/* ev-tts-debug.h — small scrolling console that logs TTS API calls (curl form). */
#pragma once

#include <gtk/gtk.h>
#include "ev-tts-controller.h"

G_BEGIN_DECLS

/* Returns a GtkWindow (~1/16 of the screen) that appends every controller
 * "log" line. Caller shows/hides/destroys it. */
GtkWidget *ev_tts_debug_window_new (GtkWindow       *parent,
                                    EvTtsController *controller);

G_END_DECLS
