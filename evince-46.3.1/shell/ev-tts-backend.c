/* ev-tts-backend.c — multi-provider TTS backend (MiniMax / OpenAI-compatible / Google). */

#include "ev-tts-backend.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

struct _EvTtsBackend {
        GObject       parent_instance;

        SoupSession  *session;

        EvTtsProvider provider;
        char    *base_url;   /* OpenAI-compatible base, e.g. https://openrouter.ai/api/v1 */
        char    *host;       /* MiniMax host, e.g. api.minimax.io */
        char    *group_id;
        char    *api_key;
        char    *voice_id;
        char    *model;
        double   speed;
        double   vol;
        int      pitch;
};

enum { SIGNAL_LOG, N_SIGNALS };
static guint backend_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (EvTtsBackend, ev_tts_backend, G_TYPE_OBJECT)

EvTtsProvider
ev_tts_provider_from_string (const char *s)
{
        if (g_strcmp0 (s, "openai") == 0)  return EV_TTS_PROVIDER_OPENAI;
        if (g_strcmp0 (s, "google") == 0)  return EV_TTS_PROVIDER_GOOGLE;
        return EV_TTS_PROVIDER_MINIMAX;
}

const char *
ev_tts_provider_to_string (EvTtsProvider p)
{
        switch (p) {
        case EV_TTS_PROVIDER_OPENAI: return "openai";
        case EV_TTS_PROVIDER_GOOGLE: return "google";
        default:                     return "minimax";
        }
}

G_GNUC_PRINTF (2, 3) static void
emit_log (EvTtsBackend *self, const char *fmt, ...)
{
        va_list ap;
        char *msg;

        va_start (ap, fmt);
        msg = g_strdup_vprintf (fmt, ap);
        va_end (ap);
        g_signal_emit (self, backend_signals[SIGNAL_LOG], 0, msg);
        g_free (msg);
}

/* show only enough of a secret to recognise it */
static char *
mask_key (const char *key)
{
        gsize n = key ? strlen (key) : 0;
        if (n == 0)
                return g_strdup ("(none)");
        if (n <= 12)
                return g_strdup ("***");
        return g_strdup_printf ("%.8s…%.4s", key, key + n - 4);
}

static void
ev_tts_backend_finalize (GObject *object)
{
        EvTtsBackend *self = EV_TTS_BACKEND (object);

        g_clear_object (&self->session);
        g_clear_pointer (&self->base_url, g_free);
        g_clear_pointer (&self->host, g_free);
        g_clear_pointer (&self->group_id, g_free);
        g_clear_pointer (&self->api_key, g_free);
        g_clear_pointer (&self->voice_id, g_free);
        g_clear_pointer (&self->model, g_free);

        G_OBJECT_CLASS (ev_tts_backend_parent_class)->finalize (object);
}

static void
ev_tts_backend_class_init (EvTtsBackendClass *klass)
{
        G_OBJECT_CLASS (klass)->finalize = ev_tts_backend_finalize;

        backend_signals[SIGNAL_LOG] =
                g_signal_new ("log", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                              0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
ev_tts_backend_init (EvTtsBackend *self)
{
        self->session  = soup_session_new ();
        self->provider = EV_TTS_PROVIDER_MINIMAX;
        self->host     = g_strdup ("api.minimax.io");
        self->base_url = g_strdup ("");
        self->model    = g_strdup ("speech-2.8-hd");
        self->speed    = 1.0;
        self->vol      = 1.0;
        self->pitch    = 0;
}

EvTtsBackend *
ev_tts_backend_new (void)
{
        return g_object_new (EV_TYPE_TTS_BACKEND, NULL);
}

static char *
dup_stripped (const char *s)
{
        return g_strstrip (g_strdup (s ? s : ""));
}

void
ev_tts_backend_configure (EvTtsBackend *self,
                          EvTtsProvider provider,
                          const char   *base_url,
                          const char   *host,
                          const char   *group_id,
                          const char   *api_key,
                          const char   *voice_id,
                          const char   *model,
                          double        speed,
                          double        vol,
                          int           pitch)
{
        g_return_if_fail (EV_IS_TTS_BACKEND (self));

        self->provider = provider;
        g_free (self->base_url);
        self->base_url = dup_stripped (base_url);
        /* trim a trailing slash so we can append "/audio/speech" cleanly */
        if (self->base_url[0]) {
                gsize n = strlen (self->base_url);
                if (self->base_url[n - 1] == '/')
                        self->base_url[n - 1] = '\0';
        }
        if (host && *host) {
                g_free (self->host);
                self->host = dup_stripped (host);
        }
        g_free (self->group_id);
        self->group_id = dup_stripped (group_id);
        g_free (self->api_key);
        self->api_key = dup_stripped (api_key);
        g_free (self->voice_id);
        self->voice_id = dup_stripped (voice_id);
        if (model && *model) {
                g_free (self->model);
                self->model = dup_stripped (model);
        }
        self->speed = speed > 0 ? speed : 1.0;
        self->vol   = vol   > 0 ? vol   : 1.0;
        self->pitch = pitch;
}

gboolean
ev_tts_backend_is_configured (EvTtsBackend *self)
{
        g_return_val_if_fail (EV_IS_TTS_BACKEND (self), FALSE);

        switch (self->provider) {
        case EV_TTS_PROVIDER_OPENAI:
                /* key may be empty for local servers */
                return self->base_url && *self->base_url &&
                       self->model && *self->model &&
                       self->voice_id && *self->voice_id;
        case EV_TTS_PROVIDER_GOOGLE:
                return self->api_key && *self->api_key &&
                       self->voice_id && *self->voice_id;
        default: /* MiniMax — GroupId optional on api.minimax.io */
                return self->host && *self->host &&
                       self->api_key && *self->api_key &&
                       self->voice_id && *self->voice_id;
        }
}

/* ---- hex / base64 decode ---- */

static GBytes *
hex_decode (const char *hex, gsize hex_len)
{
        guint8 *out;
        gsize   out_len;

        if (hex_len % 2 != 0)
                return NULL;
        out_len = hex_len / 2;
        out = g_malloc (out_len ? out_len : 1);
        for (gsize i = 0; i < out_len; i++) {
                int hi = g_ascii_xdigit_value (hex[2 * i]);
                int lo = g_ascii_xdigit_value (hex[2 * i + 1]);
                if (hi < 0 || lo < 0) {
                        g_free (out);
                        return NULL;
                }
                out[i] = (hi << 4) | lo;
        }
        return g_bytes_new_take (out, out_len);
}

/* =====================  request builders  ===================== */

/* ----- MiniMax ----- */

static char *
build_minimax_body (EvTtsBackend *self, const char *text, gsize *len_out)
{
        g_autoptr (JsonBuilder) b = json_builder_new ();
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "model");
        json_builder_add_string_value (b, self->model);
        json_builder_set_member_name (b, "text");
        json_builder_add_string_value (b, text);
        json_builder_set_member_name (b, "stream");
        json_builder_add_boolean_value (b, FALSE);
        json_builder_set_member_name (b, "voice_setting");
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "voice_id");
        json_builder_add_string_value (b, self->voice_id);
        json_builder_set_member_name (b, "speed");
        json_builder_add_double_value (b, self->speed);
        json_builder_set_member_name (b, "vol");
        json_builder_add_double_value (b, self->vol);
        json_builder_set_member_name (b, "pitch");
        json_builder_add_int_value (b, self->pitch);
        json_builder_end_object (b);
        json_builder_set_member_name (b, "audio_setting");
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "format");
        json_builder_add_string_value (b, "mp3");
        json_builder_set_member_name (b, "sample_rate");
        json_builder_add_int_value (b, 32000);
        json_builder_set_member_name (b, "channel");
        json_builder_add_int_value (b, 1);
        json_builder_end_object (b);
        json_builder_end_object (b);

        g_autoptr (JsonGenerator) gen = json_generator_new ();
        g_autoptr (JsonNode) root = json_builder_get_root (b);
        json_generator_set_root (gen, root);
        return json_generator_to_data (gen, len_out);
}

static GBytes *
parse_minimax (GBytes *body, GError **error)
{
        gsize size = 0;
        const char *data = g_bytes_get_data (body, &size);
        g_autoptr (JsonParser) parser = json_parser_new ();

        if (!json_parser_load_from_data (parser, data, size, error))
                return NULL;
        JsonNode *root = json_parser_get_root (parser);
        if (!JSON_NODE_HOLDS_OBJECT (root)) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Unexpected TTS response (not a JSON object)");
                return NULL;
        }
        JsonObject *obj = json_node_get_object (root);
        if (json_object_has_member (obj, "base_resp")) {
                JsonObject *base = json_object_get_object_member (obj, "base_resp");
                gint64 code = json_object_get_int_member_with_default (base, "status_code", 0);
                if (code != 0) {
                        const char *msg = json_object_get_string_member_with_default (
                                base, "status_msg", "unknown error");
                        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "MiniMax TTS error %" G_GINT64_FORMAT ": %s", code, msg);
                        return NULL;
                }
        }
        if (!json_object_has_member (obj, "data")) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "TTS response missing 'data'");
                return NULL;
        }
        JsonObject *dobj = json_object_get_object_member (obj, "data");
        const char *audio_hex =
                json_object_get_string_member_with_default (dobj, "audio", NULL);
        if (!audio_hex || !*audio_hex) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "TTS response contained no audio");
                return NULL;
        }
        GBytes *mp3 = hex_decode (audio_hex, strlen (audio_hex));
        if (!mp3) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Failed to hex-decode audio payload");
                return NULL;
        }
        return mp3;
}

/* ----- OpenAI-compatible (/audio/speech, raw bytes) ----- */

static char *
build_openai_body (EvTtsBackend *self, const char *text, gsize *len_out)
{
        g_autoptr (JsonBuilder) b = json_builder_new ();
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "model");
        json_builder_add_string_value (b, self->model);
        json_builder_set_member_name (b, "input");
        json_builder_add_string_value (b, text);
        json_builder_set_member_name (b, "voice");
        json_builder_add_string_value (b, self->voice_id);
        json_builder_set_member_name (b, "response_format");
        json_builder_add_string_value (b, "mp3");
        json_builder_set_member_name (b, "speed");
        json_builder_add_double_value (b, self->speed);
        json_builder_end_object (b);

        g_autoptr (JsonGenerator) gen = json_generator_new ();
        g_autoptr (JsonNode) root = json_builder_get_root (b);
        json_generator_set_root (gen, root);
        return json_generator_to_data (gen, len_out);
}

/* The audio is the body itself on success; errors come back as JSON. */
static GBytes *
parse_openai (guint status, GBytes *body, GError **error)
{
        gsize size = 0;
        const char *data = g_bytes_get_data (body, &size);

        if (status >= 200 && status < 300 && size > 0)
                return g_bytes_ref (body);

        /* error: try to pull a JSON message, else show a snippet */
        g_autoptr (JsonParser) parser = json_parser_new ();
        if (json_parser_load_from_data (parser, data, size, NULL)) {
                JsonNode *root = json_parser_get_root (parser);
                if (JSON_NODE_HOLDS_OBJECT (root)) {
                        JsonObject *obj = json_node_get_object (root);
                        if (json_object_has_member (obj, "error")) {
                                JsonNode *en = json_object_get_member (obj, "error");
                                const char *msg = NULL;
                                if (JSON_NODE_HOLDS_OBJECT (en))
                                        msg = json_object_get_string_member_with_default (
                                                json_node_get_object (en), "message", NULL);
                                else if (JSON_NODE_HOLDS_VALUE (en))
                                        msg = json_node_get_string (en);
                                if (msg) {
                                        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                                     "TTS HTTP %u: %s", status, msg);
                                        return NULL;
                                }
                        }
                }
        }
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "TTS HTTP %u: %.200s", status, size ? data : "(empty body)");
        return NULL;
}

/* ----- Google Cloud Text-to-Speech ----- */

/* "en-US-Neural2-C" -> "en-US"; fall back to en-US */
static char *
google_lang_from_voice (const char *voice)
{
        if (voice) {
                const char *first = strchr (voice, '-');
                if (first) {
                        const char *second = strchr (first + 1, '-');
                        if (second)
                                return g_strndup (voice, second - voice);
                }
        }
        return g_strdup ("en-US");
}

static char *
build_google_body (EvTtsBackend *self, const char *text, gsize *len_out)
{
        g_autofree char *lang = google_lang_from_voice (self->voice_id);
        g_autoptr (JsonBuilder) b = json_builder_new ();
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "input");
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "text");
        json_builder_add_string_value (b, text);
        json_builder_end_object (b);
        json_builder_set_member_name (b, "voice");
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "languageCode");
        json_builder_add_string_value (b, lang);
        json_builder_set_member_name (b, "name");
        json_builder_add_string_value (b, self->voice_id);
        json_builder_end_object (b);
        json_builder_set_member_name (b, "audioConfig");
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "audioEncoding");
        json_builder_add_string_value (b, "MP3");
        json_builder_set_member_name (b, "speakingRate");
        json_builder_add_double_value (b, self->speed);
        json_builder_set_member_name (b, "pitch");
        json_builder_add_double_value (b, (double) self->pitch);
        json_builder_end_object (b);
        json_builder_end_object (b);

        g_autoptr (JsonGenerator) gen = json_generator_new ();
        g_autoptr (JsonNode) root = json_builder_get_root (b);
        json_generator_set_root (gen, root);
        return json_generator_to_data (gen, len_out);
}

static GBytes *
parse_google (GBytes *body, GError **error)
{
        gsize size = 0;
        const char *data = g_bytes_get_data (body, &size);
        g_autoptr (JsonParser) parser = json_parser_new ();

        if (!json_parser_load_from_data (parser, data, size, error))
                return NULL;
        JsonNode *root = json_parser_get_root (parser);
        if (!JSON_NODE_HOLDS_OBJECT (root)) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Unexpected Google TTS response");
                return NULL;
        }
        JsonObject *obj = json_node_get_object (root);
        if (json_object_has_member (obj, "error")) {
                JsonObject *e = json_object_get_object_member (obj, "error");
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Google TTS: %s",
                             json_object_get_string_member_with_default (e, "message", "error"));
                return NULL;
        }
        const char *b64 =
                json_object_get_string_member_with_default (obj, "audioContent", NULL);
        if (!b64 || !*b64) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Google TTS response contained no audio");
                return NULL;
        }
        gsize out_len = 0;
        guchar *raw = g_base64_decode (b64, &out_len);
        if (!raw || out_len == 0) {
                g_free (raw);
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Failed to base64-decode Google audio");
                return NULL;
        }
        return g_bytes_new_take (raw, out_len);
}

/* =====================  synthesize  ===================== */

typedef struct {
        SoupMessage  *msg;        /* owned, for reading status */
        EvTtsProvider provider;
} SynthData;

static void
synth_data_free (gpointer data)
{
        SynthData *d = data;
        g_clear_object (&d->msg);
        g_free (d);
}

static void
on_soup_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
        SoupSession  *session = SOUP_SESSION (source);
        g_autoptr (GTask) task = G_TASK (user_data);
        EvTtsBackend *self = EV_TTS_BACKEND (g_task_get_source_object (task));
        SynthData    *d = g_task_get_task_data (task);
        guint         status = 0;
        GError       *error = NULL;

        g_autoptr (GBytes) body =
                soup_session_send_and_read_finish (session, result, &error);
        if (!body) {
                emit_log (self, "# \xE2\x86\x90 request failed: %s",
                          error ? error->message : "unknown");
                g_task_return_error (task, error);
                return;
        }
        status = soup_message_get_status (d->msg);

        GBytes *mp3 = NULL;
        switch (d->provider) {
        case EV_TTS_PROVIDER_OPENAI: mp3 = parse_openai (status, body, &error); break;
        case EV_TTS_PROVIDER_GOOGLE: mp3 = parse_google (body, &error);         break;
        default:                     mp3 = parse_minimax (body, &error);        break;
        }

        if (!mp3) {
                emit_log (self, "# \xE2\x86\x90 HTTP %u · %s", status,
                          error ? error->message : "error");
                g_task_return_error (task, error);
                return;
        }
        emit_log (self, "# \xE2\x86\x90 HTTP %u · %zu bytes mp3",
                  status, g_bytes_get_size (mp3));
        g_task_return_pointer (task, mp3, (GDestroyNotify) g_bytes_unref);
}

void
ev_tts_backend_synthesize_async (EvTtsBackend        *self,
                                 const char          *text,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
        g_return_if_fail (EV_IS_TTS_BACKEND (self));

        GTask *task = g_task_new (self, cancellable, callback, user_data);

        if (!ev_tts_backend_is_configured (self)) {
                g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                         "TTS provider is not configured (check Settings)");
                g_object_unref (task);
                return;
        }

        g_autofree char *url = NULL;
        g_autofree char *body = NULL;
        gsize body_len = 0;
        gboolean send_bearer = TRUE;

        switch (self->provider) {
        case EV_TTS_PROVIDER_OPENAI:
                url = g_strdup_printf ("%s/audio/speech", self->base_url);
                body = build_openai_body (self, text, &body_len);
                send_bearer = (self->api_key && *self->api_key);
                break;
        case EV_TTS_PROVIDER_GOOGLE:
                url = g_strdup_printf (
                        "https://texttospeech.googleapis.com/v1/text:synthesize?key=%s",
                        self->api_key);
                body = build_google_body (self, text, &body_len);
                send_bearer = FALSE;   /* key is in the query string */
                break;
        default:
                url = (self->group_id && *self->group_id)
                        ? g_strdup_printf ("https://%s/v1/t2a_v2?GroupId=%s",
                                           self->host, self->group_id)
                        : g_strdup_printf ("https://%s/v1/t2a_v2", self->host);
                body = build_minimax_body (self, text, &body_len);
                break;
        }

        g_autoptr (SoupMessage) msg = soup_message_new ("POST", url);
        if (!msg) {
                g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "Invalid TTS endpoint URL: %s", url);
                g_object_unref (task);
                return;
        }

        SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
        if (send_bearer) {
                g_autofree char *bearer = g_strdup_printf ("Bearer %s", self->api_key);
                soup_message_headers_append (headers, "Authorization", bearer);
        }
        g_autoptr (GBytes) body_bytes = g_bytes_new (body, body_len);
        soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);

        SynthData *d = g_new0 (SynthData, 1);
        d->msg = g_object_ref (msg);
        d->provider = self->provider;
        g_task_set_task_data (task, d, synth_data_free);

        {
                g_autofree char *masked = mask_key (self->api_key);
                /* hide the key in Google's query string */
                g_autofree char *log_url =
                        (self->provider == EV_TTS_PROVIDER_GOOGLE)
                                ? g_strdup ("https://texttospeech.googleapis.com/v1/text:synthesize?key=***")
                                : g_strdup (url);
                g_autofree char *short_body =
                        (body_len > 360) ? g_strdup_printf ("%.360s…", body) : g_strdup (body);
                emit_log (self,
                          "curl -X POST '%s' -H 'Authorization: Bearer %s' "
                          "-H 'Content-Type: application/json' -d '%s'",
                          log_url, masked, short_body);
        }

        soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                          cancellable, on_soup_done, task);
}

GBytes *
ev_tts_backend_synthesize_finish (EvTtsBackend  *self,
                                  GAsyncResult  *result,
                                  GError       **error)
{
        g_return_val_if_fail (g_task_is_valid (result, self), NULL);
        return g_task_propagate_pointer (G_TASK (result), error);
}

/* =====================  voice listing  ===================== */

static const char *OPENAI_VOICES[] = {
        "alloy", "ash", "ballad", "coral", "echo", "fable",
        "onyx", "nova", "sage", "shimmer", "verse", NULL
};
static const char *GOOGLE_VOICES[] = {
        "en-US-Neural2-C", "en-US-Neural2-D", "en-US-Neural2-F",
        "en-US-Wavenet-D", "en-US-Studio-O", "en-GB-Neural2-A",
        "en-GB-Neural2-B", NULL
};

static char **
dup_static_voices (const char **list)
{
        GPtrArray *a = g_ptr_array_new ();
        for (guint i = 0; list[i]; i++)
                g_ptr_array_add (a, g_strdup (list[i]));
        g_ptr_array_add (a, NULL);
        return (char **) g_ptr_array_free (a, FALSE);
}

/* MiniMax: fetch the account's cloned voice ids via /v1/get_voice. */
static char **
minimax_list_voices (const char *host, const char *api_key, GError **error)
{
        g_autoptr (SoupSession) session = NULL;
        g_autoptr (SoupMessage) msg = NULL;
        g_autoptr (GBytes) resp = NULL;
        g_autofree char *url = NULL;
        g_autofree char *bearer = NULL;
        g_autoptr (JsonParser) parser = NULL;
        g_autofree char *clean_host = NULL;
        g_autofree char *clean_key = NULL;
        const char *body_str = "{\"voice_type\":\"all\"}";
        GBytes *req_body;
        JsonObject *root_obj;
        JsonArray  *cloning;
        GPtrArray  *ids;
        gsize size = 0;
        const char *data;

        if (!host || !*host || !api_key || !*api_key) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                     "Enter an API key (and region) first.");
                return NULL;
        }
        clean_host = g_strstrip (g_strdup (host));
        clean_key  = g_strstrip (g_strdup (api_key));

        session = soup_session_new ();
        url = g_strdup_printf ("https://%s/v1/get_voice", clean_host);
        msg = soup_message_new ("POST", url);
        if (!msg) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid URL: %s", url);
                return NULL;
        }
        bearer = g_strdup_printf ("Bearer %s", clean_key);
        soup_message_headers_append (soup_message_get_request_headers (msg),
                                     "Authorization", bearer);
        req_body = g_bytes_new_static (body_str, strlen (body_str));
        soup_message_set_request_body_from_bytes (msg, "application/json", req_body);
        g_bytes_unref (req_body);

        resp = soup_session_send_and_read (session, msg, NULL, error);
        if (!resp)
                return NULL;
        parser = json_parser_new ();
        data = g_bytes_get_data (resp, &size);
        if (!json_parser_load_from_data (parser, data, size, error))
                return NULL;
        root_obj = json_node_get_object (json_parser_get_root (parser));
        if (!root_obj || !json_object_has_member (root_obj, "voice_cloning")) {
                if (root_obj && json_object_has_member (root_obj, "base_resp")) {
                        JsonObject *b = json_object_get_object_member (root_obj, "base_resp");
                        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                                     json_object_get_string_member_with_default (
                                             b, "status_msg", "no cloned voices"));
                } else {
                        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "Unexpected get_voice response");
                }
                return NULL;
        }
        cloning = json_object_get_array_member (root_obj, "voice_cloning");
        ids = g_ptr_array_new ();
        for (guint i = 0; cloning && i < json_array_get_length (cloning); i++) {
                JsonObject *v = json_array_get_object_element (cloning, i);
                const char *id = json_object_get_string_member_with_default (v, "voice_id", NULL);
                if (id && *id)
                        g_ptr_array_add (ids, g_strdup (id));
        }
        g_ptr_array_add (ids, NULL);
        return (char **) g_ptr_array_free (ids, FALSE);
}

char **
ev_tts_backend_list_voices (EvTtsProvider provider,
                            const char   *endpoint,
                            const char   *api_key,
                            GError      **error)
{
        switch (provider) {
        case EV_TTS_PROVIDER_OPENAI: return dup_static_voices (OPENAI_VOICES);
        case EV_TTS_PROVIDER_GOOGLE: return dup_static_voices (GOOGLE_VOICES);
        default:                     return minimax_list_voices (endpoint, api_key, error);
        }
}

typedef struct { EvTtsProvider provider; char *endpoint; char *key; } ListCtx;

static void
list_ctx_free (gpointer data)
{
        ListCtx *c = data;
        g_free (c->endpoint);
        g_free (c->key);
        g_free (c);
}

static void
list_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
        ListCtx *c = task_data;
        GError  *error = NULL;
        char   **voices = ev_tts_backend_list_voices (c->provider, c->endpoint, c->key, &error);
        if (voices)
                g_task_return_pointer (task, voices, (GDestroyNotify) g_strfreev);
        else
                g_task_return_error (task, error);
}

void
ev_tts_backend_list_voices_async (EvTtsProvider        provider,
                                  const char          *endpoint,
                                  const char          *api_key,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
        GTask   *task = g_task_new (NULL, cancellable, callback, user_data);
        ListCtx *c = g_new0 (ListCtx, 1);
        c->provider = provider;
        c->endpoint = g_strdup (endpoint);
        c->key      = g_strdup (api_key);
        g_task_set_task_data (task, c, list_ctx_free);
        g_task_run_in_thread (task, list_thread);
        g_object_unref (task);
}

char **
ev_tts_backend_list_voices_finish (GAsyncResult *result, GError **error)
{
        g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
        return g_task_propagate_pointer (G_TASK (result), error);
}
