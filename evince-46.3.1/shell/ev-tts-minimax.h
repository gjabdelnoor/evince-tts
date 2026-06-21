/* ev-tts-minimax.h
 *
 * MiniMax T2A V2 text-to-speech backend for Evince.
 * Performs an async HTTPS POST to the MiniMax speech endpoint and returns
 * the synthesized audio as MP3 bytes (decoded from the hex string MiniMax
 * returns in data.audio).
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define EV_TYPE_TTS_MINIMAX (ev_tts_minimax_get_type ())
G_DECLARE_FINAL_TYPE (EvTtsMiniMax, ev_tts_minimax, EV, TTS_MINIMAX, GObject)

EvTtsMiniMax *ev_tts_minimax_new (void);

/* Apply credentials/voice parameters. Values are copied. host is the bare
 * API host (e.g. "api.minimax.io"); the path /v1/t2a_v2 is appended. */
void ev_tts_minimax_configure (EvTtsMiniMax *self,
                               const char   *host,
                               const char   *group_id,
                               const char   *api_key,
                               const char   *voice_id,
                               const char   *model,
                               double        speed,
                               double        vol,
                               int           pitch);

/* TRUE once host/group_id/api_key/voice_id are all non-empty. */
gboolean ev_tts_minimax_is_configured (EvTtsMiniMax *self);

/* Synthesize a single chunk of text (keep under MiniMax's 10k char cap).
 * Completes with a GBytes of MP3 audio, or NULL + error. */
void    ev_tts_minimax_synthesize_async  (EvTtsMiniMax        *self,
                                           const char          *text,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
GBytes *ev_tts_minimax_synthesize_finish (EvTtsMiniMax  *self,
                                           GAsyncResult  *result,
                                           GError       **error);

G_END_DECLS
