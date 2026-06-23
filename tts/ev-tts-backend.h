/* ev-tts-backend.h
 *
 * Multi-provider text-to-speech backend for Evince. Performs an async HTTPS
 * POST to the configured provider and returns synthesized audio as MP3 bytes.
 *
 * Providers:
 *   - MINIMAX: native MiniMax T2A V2 (data.audio is a hex string).
 *   - OPENAI:  OpenAI-compatible /audio/speech (raw audio bytes). Covers
 *              OpenAI, OpenRouter, and local servers (Kokoro, openedai-speech,
 *              LocalAI, …) — distinguished only by base URL.
 *   - GOOGLE:  Google Cloud Text-to-Speech (audioContent is base64).
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
        EV_TTS_PROVIDER_MINIMAX = 0,
        EV_TTS_PROVIDER_OPENAI,     /* OpenAI-compatible: OpenAI / OpenRouter / local */
        EV_TTS_PROVIDER_GOOGLE,
} EvTtsProvider;

EvTtsProvider  ev_tts_provider_from_string (const char *s);
const char    *ev_tts_provider_to_string   (EvTtsProvider p);

#define EV_TYPE_TTS_BACKEND (ev_tts_backend_get_type ())
G_DECLARE_FINAL_TYPE (EvTtsBackend, ev_tts_backend, EV, TTS_BACKEND, GObject)

EvTtsBackend *ev_tts_backend_new (void);

/* Apply provider + credentials/voice parameters. Values are copied.
 *   base_url  full base for the OpenAI-compatible provider, e.g.
 *             "https://openrouter.ai/api/v1" or "http://localhost:8880/v1".
 *   host      bare MiniMax host, e.g. "api.minimax.io".
 *   group_id  MiniMax GroupId (optional).
 */
void ev_tts_backend_configure (EvTtsBackend *self,
                               EvTtsProvider provider,
                               const char   *base_url,
                               const char   *host,
                               const char   *group_id,
                               const char   *api_key,
                               const char   *voice_id,
                               const char   *model,
                               double        speed,
                               double        vol,
                               int           pitch);

/* TRUE once the provider has the minimum config it needs to synthesize. */
gboolean ev_tts_backend_is_configured (EvTtsBackend *self);

/* List selectable voices for a provider. For MiniMax this fetches the
 * account's cloned voice ids; for OpenAI/Google it returns a known set.
 * endpoint = MiniMax host or the OpenAI-compatible base URL. */
char **ev_tts_backend_list_voices (EvTtsProvider provider,
                                   const char   *endpoint,
                                   const char   *api_key,
                                   GError      **error);
void   ev_tts_backend_list_voices_async  (EvTtsProvider        provider,
                                           const char          *endpoint,
                                           const char          *api_key,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
char **ev_tts_backend_list_voices_finish (GAsyncResult *result, GError **error);

/* Synthesize one chunk of text. Completes with a GBytes of MP3, or NULL+error.
 * Emits "log" (gchar*) — one curl-form line per API call. */
void    ev_tts_backend_synthesize_async  (EvTtsBackend        *self,
                                           const char          *text,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
GBytes *ev_tts_backend_synthesize_finish (EvTtsBackend  *self,
                                           GAsyncResult  *result,
                                           GError       **error);

G_END_DECLS
