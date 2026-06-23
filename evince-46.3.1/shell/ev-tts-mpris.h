/* ev-tts-mpris.h — expose the TTS player on MPRIS so media keys / headphone
 * play-pause control it. */
#pragma once

#include <gtk/gtk.h>
#include "ev-tts-controller.h"

G_BEGIN_DECLS

#define EV_TYPE_TTS_MPRIS (ev_tts_mpris_get_type ())
G_DECLARE_FINAL_TYPE (EvTtsMpris, ev_tts_mpris, EV, TTS_MPRIS, GObject)

EvTtsMpris *ev_tts_mpris_new (EvTtsController *controller, GtkWindow *window);

G_END_DECLS
