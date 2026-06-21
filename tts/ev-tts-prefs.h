/* ev-tts-prefs.h — MiniMax TTS settings dialog. */
#pragma once

#include <gtk/gtk.h>
#include "ev-tts-controller.h"

G_BEGIN_DECLS

/* Modal-ish settings dialog. Writes non-secret config to GSettings and the API
 * key to the keyring, then asks the controller to reload its config. */
void ev_tts_show_preferences (GtkWindow       *parent,
                              EvTtsController *controller);

G_END_DECLS
