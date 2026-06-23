/* ev-tts-controller.c */

#include "ev-tts-controller.h"
#include "ev-tts-minimax.h"
#include "ev-keyring.h"

#include "ev-document.h"
#include "ev-document-text.h"
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define TTS_KEYRING_URI "minimax-tts://default"
#define TTS_SCHEMA      "org.gnome.Evince"
/* Hard split very long sentences so a single request stays small and the
 * highlight stays legible. */
#define TTS_MAX_SENTENCE_CHARS 400
/* Pre-synthesize the current page ± this many pages and cache the audio. */
#define CACHE_RADIUS 2
/* Pre-generate a page's audio after dwelling on it this long (idle). */
#define DWELL_MS 5000

typedef struct {
        char        *text;
        EvRectangle *rects;     /* merged line rectangles (doc coords) */
        guint        n_rects;
        GBytes      *audio;     /* cached MP3, NULL until synthesized */
        char        *tmpfile;   /* separate cache file, written when audio arrives */
        gboolean     requested;
} Sentence;

typedef struct {
        GPtrArray   *sentences; /* of Sentence* (may be empty for text-less pages) */
} PageCache;

struct _EvTtsController {
        GObject          parent_instance;

        EvView          *view;          /* not owned */
        EvDocumentModel *model;         /* owned ref */
        EvTtsMiniMax    *backend;
        GSettings       *settings;

        GstElement      *playbin;
        guint            bus_watch_id;

        GHashTable      *pages;         /* gint page -> PageCache* */
        gint             cur;           /* current sentence on self->page */
        gint             page;          /* page currently being read */

        GCancellable    *cancellable;   /* persistent; bumped on invalidate */
        guint            gen;           /* generation; stale synth results dropped */
        guint            dwell_id;      /* idle pre-gen timer */

        gboolean         active;
        gboolean         paused;
        double           volume;        /* playback volume 0..1 */
};

/* Per-request context so each synth result lands on the right page+sentence. */
typedef struct {
        EvTtsController *self;          /* ref'd */
        gint             page;
        gint             idx;
        guint            gen;
} SynthCtx;

enum {
        PROP_0,
        PROP_ACTIVE,
        PROP_PAUSED,
        N_PROPS
};

enum {
        SIGNAL_STATUS,
        SIGNAL_LOG,
        SIGNAL_VOICES_CHANGED,
        N_SIGNALS
};

static GParamSpec *props[N_PROPS];
static guint       signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (EvTtsController, ev_tts_controller, G_TYPE_OBJECT)

/* --- forward decls --- */
static void     ev_tts_controller_advance       (EvTtsController *self);
static void     ev_tts_controller_ensure_playing (EvTtsController *self);
static void     request_page                    (EvTtsController *self, gint page);
static void     request_sentence_ctx            (EvTtsController *self, gint page, gint idx);
static void     ensure_window                   (EvTtsController *self, gint center);
static gboolean play_current                    (EvTtsController *self);

/* --- Sentence / PageCache helpers --- */

static void
sentence_free (Sentence *s)
{
        if (!s)
                return;
        g_free (s->text);
        g_free (s->rects);
        if (s->audio)
                g_bytes_unref (s->audio);
        if (s->tmpfile) {
                g_unlink (s->tmpfile);
                g_free (s->tmpfile);
        }
        g_free (s);
}

static void
page_cache_free (PageCache *pc)
{
        if (!pc)
                return;
        if (pc->sentences)
                g_ptr_array_unref (pc->sentences);
        g_free (pc);
}

static void
emit_status (EvTtsController *self, const char *fmt, ...)
{
        va_list ap;
        char *msg;

        va_start (ap, fmt);
        msg = g_strdup_vprintf (fmt, ap);
        va_end (ap);

        g_signal_emit (self, signals[SIGNAL_STATUS], 0, msg);
        g_free (msg);
}

/* Merge the per-character rectangles areas[c0..c1) into line-level rectangles. */
static EvRectangle *
merge_line_rects (const EvRectangle *areas,
                  guint              n_areas,
                  guint              c0,
                  guint              c1,
                  guint             *n_out)
{
        GArray *out = g_array_new (FALSE, FALSE, sizeof (EvRectangle));
        gboolean have_cur = FALSE;
        EvRectangle cur = { 0 };

        if (c1 > n_areas)
                c1 = n_areas;

        for (guint i = c0; i < c1; i++) {
                EvRectangle r = areas[i];

                if (r.x2 <= r.x1 || r.y2 <= r.y1)
                        continue;

                if (!have_cur) {
                        cur = r;
                        have_cur = TRUE;
                        continue;
                }

                if (r.y1 < cur.y2 && r.y2 > cur.y1) {
                        cur.x1 = MIN (cur.x1, r.x1);
                        cur.y1 = MIN (cur.y1, r.y1);
                        cur.x2 = MAX (cur.x2, r.x2);
                        cur.y2 = MAX (cur.y2, r.y2);
                } else {
                        g_array_append_val (out, cur);
                        cur = r;
                }
        }
        if (have_cur)
                g_array_append_val (out, cur);

        *n_out = out->len;
        return (EvRectangle *) g_array_free (out, out->len == 0);
}

static gboolean
is_sentence_terminator (gunichar c)
{
        return c == '.' || c == '!' || c == '?' || c == '\n' ||
               c == 0x3002 /* 。 */ || c == 0xFF01 /* ！ */ || c == 0xFF1F /* ？ */;
}

/* Extract sentences (text + highlight rects) for a page. Never returns NULL. */
static GPtrArray *
extract_sentences (EvTtsController *self, gint page)
{
        GPtrArray   *out = g_ptr_array_new_with_free_func ((GDestroyNotify) sentence_free);
        EvDocument  *doc = ev_document_model_get_document (self->model);
        EvPage      *ev_page;
        EvRectangle *areas = NULL;
        guint        n_areas = 0;
        char        *text;
        const char  *p;
        guint        char_start = 0, char_idx = 0;
        GString     *buf;

        if (!doc || !EV_IS_DOCUMENT_TEXT (doc))
                return out;

        ev_page = ev_document_get_page (doc, page);
        if (!ev_page)
                return out;

        text = ev_document_text_get_text (EV_DOCUMENT_TEXT (doc), ev_page);
        ev_document_text_get_text_layout (EV_DOCUMENT_TEXT (doc), ev_page,
                                          &areas, &n_areas);
        g_object_unref (ev_page);

        if (!text || !*text) {
                g_free (text);
                g_free (areas);
                return out;
        }

        buf = g_string_new (NULL);
        for (p = text; *p; p = g_utf8_next_char (p)) {
                gunichar c = g_utf8_get_char (p);
                guint sentence_len = char_idx - char_start + 1;
                gboolean force = sentence_len >= TTS_MAX_SENTENCE_CHARS && c == ' ';

                g_string_append_unichar (buf, c);
                char_idx++;

                if (is_sentence_terminator (c) || force) {
                        char *t = g_strstrip (g_strdup (buf->str));
                        if (t && *t) {
                                Sentence *s = g_new0 (Sentence, 1);
                                s->text = t;
                                if (areas)
                                        s->rects = merge_line_rects (areas, n_areas,
                                                                     char_start, char_idx,
                                                                     &s->n_rects);
                                g_ptr_array_add (out, s);
                        } else {
                                g_free (t);
                        }
                        g_string_truncate (buf, 0);
                        char_start = char_idx;
                }
        }
        if (buf->len > 0) {
                char *t = g_strstrip (g_strdup (buf->str));
                if (t && *t) {
                        Sentence *s = g_new0 (Sentence, 1);
                        s->text = t;
                        if (areas)
                                s->rects = merge_line_rects (areas, n_areas,
                                                             char_start, char_idx,
                                                             &s->n_rects);
                        g_ptr_array_add (out, s);
                } else {
                        g_free (t);
                }
        }

        g_string_free (buf, TRUE);
        g_free (text);
        g_free (areas);
        return out;
}

/* Get the cache entry for a page, extracting sentences on first use. */
static PageCache *
page_get (EvTtsController *self, gint page, gboolean build)
{
        PageCache *pc;
        EvDocument *doc = ev_document_model_get_document (self->model);
        gint n_pages = doc ? ev_document_get_n_pages (doc) : 0;

        if (page < 0 || page >= n_pages)
                return NULL;

        pc = g_hash_table_lookup (self->pages, GINT_TO_POINTER (page));
        if (pc || !build)
                return pc;

        pc = g_new0 (PageCache, 1);
        pc->sentences = extract_sentences (self, page);
        g_hash_table_insert (self->pages, GINT_TO_POINTER (page), pc);
        return pc;
}

static Sentence *
sentence_at (EvTtsController *self, gint page, gint idx)
{
        PageCache *pc = g_hash_table_lookup (self->pages, GINT_TO_POINTER (page));
        if (!pc || idx < 0 || idx >= (gint) pc->sentences->len)
                return NULL;
        return g_ptr_array_index (pc->sentences, idx);
}

static gboolean
ensure_tmpfile (EvTtsController *self, Sentence *s)
{
        GError *error = NULL;
        int     fd;
        gsize   len;
        const char *data;

        if (s->tmpfile)
                return TRUE;
        if (!s->audio)
                return FALSE;

        data = g_bytes_get_data (s->audio, &len);
        fd = g_file_open_tmp ("evince-tts-XXXXXX.mp3", &s->tmpfile, &error);
        if (fd < 0) {
                emit_status (self, "TTS: %s", error->message);
                g_clear_error (&error);
                return FALSE;
        }
        if (write (fd, data, len) != (gssize) len) {
                close (fd);
                emit_status (self, "TTS: failed to buffer audio");
                return FALSE;
        }
        close (fd);
        return TRUE;
}

/* --- playback --- */

static void
highlight_current (EvTtsController *self)
{
        Sentence *s = sentence_at (self, self->page, self->cur);
        if (!self->view)
                return;
        if (s && s->n_rects > 0)
                ev_view_set_tts_highlight (self->view, self->page, s->rects, s->n_rects);
        else
                ev_view_set_tts_highlight (self->view, self->page, NULL, 0);
}

/* Play the current sentence if its audio is ready; otherwise request it and
 * return FALSE (the synth callback will start playback). */
static gboolean
play_current (EvTtsController *self)
{
        Sentence *s = sentence_at (self, self->page, self->cur);
        PageCache *pc = g_hash_table_lookup (self->pages, GINT_TO_POINTER (self->page));
        g_autofree char *uri = NULL;
        guint n = pc ? pc->sentences->len : 0;

        if (!s)
                return FALSE;

        highlight_current (self);

        if (!s->audio) {
                request_page (self, self->page);
                ensure_window (self, self->page);
                emit_status (self, "Synthesizing… page %d sentence %d/%u",
                             self->page + 1, self->cur + 1, n);
                return FALSE;
        }

        if (!ensure_tmpfile (self, s))
                return FALSE;

        uri = g_filename_to_uri (s->tmpfile, NULL, NULL);
        gst_element_set_state (self->playbin, GST_STATE_READY);
        g_object_set (self->playbin, "uri", uri, "volume", self->volume, NULL);
        gst_element_set_state (self->playbin,
                               self->paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);

        emit_status (self, "Reading page %d · sentence %d/%u",
                     self->page + 1, self->cur + 1, n);

        ensure_window (self, self->page);
        return TRUE;
}

static void
on_synth_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
        SynthCtx        *ctx = user_data;
        EvTtsController *self = ctx->self;
        GError          *error = NULL;
        GBytes          *mp3;
        gboolean         stale = (ctx->gen != self->gen);

        mp3 = ev_tts_minimax_synthesize_finish (EV_TTS_MINIMAX (source), res, &error);
        if (!mp3) {
                if (!stale && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        emit_status (self, "TTS error: %s",
                                     error ? error->message : "unknown");
                g_clear_error (&error);
                goto out;
        }

        if (!stale) {
                Sentence *s = sentence_at (self, ctx->page, ctx->idx);
                if (s && !s->audio) {
                        s->audio = g_bytes_ref (mp3);
                        ensure_tmpfile (self, s);
                }
                /* If we're stalled waiting for exactly this sentence, start it. */
                if (self->active && ctx->page == self->page && ctx->idx == self->cur) {
                        GstState st = GST_STATE_NULL;
                        gst_element_get_state (self->playbin, &st, NULL, 0);
                        if (st != GST_STATE_PLAYING && st != GST_STATE_PAUSED)
                                play_current (self);
                }
        }
        g_bytes_unref (mp3);

out:
        g_object_unref (self);
        g_free (ctx);
}

static void
request_sentence_ctx (EvTtsController *self, gint page, gint idx)
{
        Sentence *s = sentence_at (self, page, idx);
        SynthCtx *ctx;

        if (!s || s->requested || s->audio)
                return;

        s->requested = TRUE;
        ctx = g_new0 (SynthCtx, 1);
        ctx->self = g_object_ref (self);
        ctx->page = page;
        ctx->idx  = idx;
        ctx->gen  = self->gen;
        ev_tts_minimax_synthesize_async (self->backend, s->text, self->cancellable,
                                         on_synth_ready, ctx);
}

static void
request_page (EvTtsController *self, gint page)
{
        PageCache *pc = page_get (self, page, TRUE);
        if (!pc)
                return;
        for (guint i = 0; i < pc->sentences->len; i++)
                request_sentence_ctx (self, page, i);
}

/* Keep [center-R, center+R] built+synthesized; evict everything else. */
static void
ensure_window (EvTtsController *self, gint center)
{
        EvDocument *doc = ev_document_model_get_document (self->model);
        gint n_pages = doc ? ev_document_get_n_pages (doc) : 0;
        gint lo, hi;
        GHashTableIter iter;
        gpointer key, val;
        GList *evict = NULL;

        if (n_pages <= 0)
                return;

        lo = MAX (0, center - CACHE_RADIUS);
        hi = MIN (n_pages - 1, center + CACHE_RADIUS);

        g_hash_table_iter_init (&iter, self->pages);
        while (g_hash_table_iter_next (&iter, &key, &val)) {
                gint page = GPOINTER_TO_INT (key);
                if (page < lo || page > hi)
                        evict = g_list_prepend (evict, key);
        }
        for (GList *l = evict; l; l = l->next)
                g_hash_table_remove (self->pages, l->data);
        g_list_free (evict);

        for (gint p = lo; p <= hi; p++)
                request_page (self, p);
}

/* Move to the next page that has text, or stop if at the end. */
static void
start_next_page (EvTtsController *self)
{
        EvDocument *doc = ev_document_model_get_document (self->model);
        gint n_pages = doc ? ev_document_get_n_pages (doc) : 0;

        for (gint page = self->page + 1; page < n_pages; page++) {
                PageCache *pc = page_get (self, page, TRUE);
                if (pc && pc->sentences->len > 0) {
                        self->page = page;
                        self->cur = 0;
                        ev_document_model_set_page (self->model, page);
                        ensure_window (self, page);
                        ev_tts_controller_ensure_playing (self);
                        return;
                }
        }
        emit_status (self, "Finished reading.");
        ev_tts_controller_stop (self);
}

static void
ev_tts_controller_ensure_playing (EvTtsController *self)
{
        PageCache *pc = page_get (self, self->page, TRUE);

        if (!pc || pc->sentences->len == 0 || self->cur >= (gint) pc->sentences->len) {
                start_next_page (self);
                return;
        }
        play_current (self);
}

static void
ev_tts_controller_advance (EvTtsController *self)
{
        PageCache *pc = g_hash_table_lookup (self->pages, GINT_TO_POINTER (self->page));

        self->cur++;
        if (pc && self->cur < (gint) pc->sentences->len)
                ev_tts_controller_ensure_playing (self);
        else
                start_next_page (self);
}

static gboolean
bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data)
{
        EvTtsController *self = EV_TTS_CONTROLLER (user_data);

        switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_EOS:
                if (self->active)
                        ev_tts_controller_advance (self);
                break;
        case GST_MESSAGE_ERROR: {
                GError *err = NULL;
                gst_message_parse_error (msg, &err, NULL);
                emit_status (self, "Audio error: %s", err ? err->message : "unknown");
                g_clear_error (&err);
                ev_tts_controller_stop (self);
                break;
        }
        default:
                break;
        }
        return G_SOURCE_CONTINUE;
}

/* --- idle pre-generation --- */

static gboolean
dwell_fire (gpointer data)
{
        EvTtsController *self = data;

        self->dwell_id = 0;
        if (!self->active) {
                ev_tts_controller_reload_config (self);
                if (ev_tts_minimax_is_configured (self->backend))
                        ensure_window (self, ev_document_model_get_page (self->model));
        }
        return G_SOURCE_REMOVE;
}

static void
on_model_page_changed (EvDocumentModel *model, gint old, gint new_page,
                       EvTtsController *self)
{
        if (self->active)
                return;   /* active playback drives its own prefetch */
        if (self->dwell_id)
                g_source_remove (self->dwell_id);
        self->dwell_id = g_timeout_add (DWELL_MS, dwell_fire, self);
}

static void
on_backend_log (EvTtsMiniMax *backend, const char *line, EvTtsController *self)
{
        g_signal_emit (self, signals[SIGNAL_LOG], 0, line);
}

/* --- public API --- */

void
ev_tts_controller_reload_config (EvTtsController *self)
{
        g_autofree char *host    = g_settings_get_string (self->settings, "tts-host");
        g_autofree char *group   = g_settings_get_string (self->settings, "tts-group-id");
        g_autofree char *voice   = g_settings_get_string (self->settings, "tts-voice-id");
        g_autofree char *model   = g_settings_get_string (self->settings, "tts-model");
        g_autofree char *api_key = ev_keyring_lookup_password (TTS_KEYRING_URI);
        double speed = g_settings_get_double (self->settings, "tts-speed");
        double vol   = g_settings_get_double (self->settings, "tts-vol");
        int    pitch = g_settings_get_int (self->settings, "tts-pitch");

        ev_tts_minimax_configure (self->backend, host, group, api_key, voice,
                                  model, speed, vol, pitch);
}

/* Drop all cached audio (e.g. voice/speed changed) and re-warm. */
void
ev_tts_controller_invalidate_cache (EvTtsController *self)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));

        self->gen++;        /* stale in-flight results will be ignored */
        if (self->cancellable) {
                g_cancellable_cancel (self->cancellable);
                g_clear_object (&self->cancellable);
        }
        self->cancellable = g_cancellable_new ();
        g_hash_table_remove_all (self->pages);

        if (self->active) {
                if (self->playbin)
                        gst_element_set_state (self->playbin, GST_STATE_NULL);
                ensure_window (self, self->page);
                ev_tts_controller_ensure_playing (self);
        } else if (ev_tts_minimax_is_configured (self->backend)) {
                ensure_window (self, ev_document_model_get_page (self->model));
        }
}

void
ev_tts_controller_set_voice (EvTtsController *self, const char *voice)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        g_settings_set_string (self->settings, "tts-voice-id", voice ? voice : "");
        ev_tts_controller_reload_config (self);
        ev_tts_controller_invalidate_cache (self);
}

void
ev_tts_controller_set_speed (EvTtsController *self, double speed)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        g_settings_set_double (self->settings, "tts-speed", speed);
        ev_tts_controller_reload_config (self);
        ev_tts_controller_invalidate_cache (self);
}

char *
ev_tts_controller_dup_voice (EvTtsController *self)
{
        g_return_val_if_fail (EV_IS_TTS_CONTROLLER (self), NULL);
        return g_settings_get_string (self->settings, "tts-voice-id");
}

double
ev_tts_controller_get_speed (EvTtsController *self)
{
        g_return_val_if_fail (EV_IS_TTS_CONTROLLER (self), 1.0);
        return g_settings_get_double (self->settings, "tts-speed");
}

static void
on_voices_ready (GObject *source, GAsyncResult *res, gpointer data)
{
        EvTtsController *self = data;
        GError *error = NULL;
        char  **voices = ev_tts_minimax_list_cloned_voices_finish (res, &error);

        if (voices) {
                g_signal_emit (self, signals[SIGNAL_VOICES_CHANGED], 0, voices);
                g_strfreev (voices);
        } else {
                g_clear_error (&error);
        }
        g_object_unref (self);
}

void
ev_tts_controller_refresh_voices (EvTtsController *self)
{
        g_autofree char *host = NULL;
        g_autofree char *key = NULL;

        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        host = g_settings_get_string (self->settings, "tts-host");
        key  = ev_keyring_lookup_password (TTS_KEYRING_URI);
        if (!host || !*host || !key || !*key)
                return;

        ev_tts_minimax_list_cloned_voices_async (host, key, self->cancellable,
                                                 on_voices_ready, g_object_ref (self));
}

void
ev_tts_controller_start (EvTtsController *self)
{
        PageCache *pc;

        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));

        if (self->active)
                return;

        ev_tts_controller_reload_config (self);
        if (!ev_tts_minimax_is_configured (self->backend)) {
                emit_status (self, "Set your MiniMax API key and voice in TTS Settings first.");
                return;
        }
        if (!ev_document_model_get_document (self->model)) {
                emit_status (self, "No document open.");
                return;
        }

        self->active = TRUE;
        self->paused = FALSE;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);

        self->page = ev_document_model_get_page (self->model);
        self->cur = 0;
        ensure_window (self, self->page);

        pc = page_get (self, self->page, TRUE);
        if (!pc || pc->sentences->len == 0)
                start_next_page (self);
        else
                ev_tts_controller_ensure_playing (self);
}

void
ev_tts_controller_stop (EvTtsController *self)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));

        if (self->playbin)
                gst_element_set_state (self->playbin, GST_STATE_NULL);
        if (self->view)
                ev_view_set_tts_highlight (self->view, self->page, NULL, 0);

        self->cur = 0;
        /* Keep the page cache so replay is instant; in-flight synth keeps
         * warming it. */

        if (self->active) {
                self->active = FALSE;
                self->paused = FALSE;
                g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
                g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PAUSED]);
        }
}

void
ev_tts_controller_toggle (EvTtsController *self)
{
        if (ev_tts_controller_is_active (self))
                ev_tts_controller_stop (self);
        else
                ev_tts_controller_start (self);
}

gboolean
ev_tts_controller_is_active (EvTtsController *self)
{
        g_return_val_if_fail (EV_IS_TTS_CONTROLLER (self), FALSE);
        return self->active;
}

void
ev_tts_controller_set_paused (EvTtsController *self, gboolean paused)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        if (!self->active || self->paused == paused)
                return;

        self->paused = paused;
        gst_element_set_state (self->playbin,
                               paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PAUSED]);
}

gboolean
ev_tts_controller_get_paused (EvTtsController *self)
{
        g_return_val_if_fail (EV_IS_TTS_CONTROLLER (self), FALSE);
        return self->paused;
}

void
ev_tts_controller_set_volume (EvTtsController *self, double volume)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        self->volume = CLAMP (volume, 0.0, 1.0);
        if (self->playbin)
                g_object_set (self->playbin, "volume", self->volume, NULL);
}

double
ev_tts_controller_get_volume (EvTtsController *self)
{
        g_return_val_if_fail (EV_IS_TTS_CONTROLLER (self), 1.0);
        return self->volume;
}

/* Jump to a page: if reading, restart the loop there; otherwise just navigate. */
static void
controller_jump_to_page (EvTtsController *self, gint page)
{
        EvDocument *doc = ev_document_model_get_document (self->model);
        gint n_pages = doc ? ev_document_get_n_pages (doc) : 0;

        if (page < 0 || page >= n_pages)
                return;

        if (!self->active) {
                ev_document_model_set_page (self->model, page);
                return;
        }

        if (self->playbin)
                gst_element_set_state (self->playbin, GST_STATE_NULL);
        ev_document_model_set_page (self->model, page);
        self->page = page;
        self->cur = 0;
        ensure_window (self, page);
        ev_tts_controller_ensure_playing (self);
}

void
ev_tts_controller_next_page (EvTtsController *self)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        controller_jump_to_page (self, ev_document_model_get_page (self->model) + 1);
}

void
ev_tts_controller_prev_page (EvTtsController *self)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        controller_jump_to_page (self, ev_document_model_get_page (self->model) - 1);
}

gboolean
ev_tts_controller_get_progress (EvTtsController *self,
                                gint64          *pos_ns,
                                gint64          *dur_ns)
{
        gint64 pos = 0, dur = 0;

        g_return_val_if_fail (EV_IS_TTS_CONTROLLER (self), FALSE);
        if (!self->playbin || !self->active)
                return FALSE;
        if (!gst_element_query_position (self->playbin, GST_FORMAT_TIME, &pos))
                return FALSE;
        if (!gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &dur))
                return FALSE;
        if (dur <= 0)
                return FALSE;
        if (pos_ns) *pos_ns = pos;
        if (dur_ns) *dur_ns = dur;
        return TRUE;
}

void
ev_tts_controller_seek_fraction (EvTtsController *self, double fraction)
{
        gint64 dur = 0;

        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));
        if (!self->playbin || !self->active)
                return;
        if (!gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &dur) || dur <= 0)
                return;

        gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                 (gint64) (CLAMP (fraction, 0.0, 1.0) * dur));
}

/* --- GObject boilerplate --- */

static void
ev_tts_controller_get_property (GObject *object, guint prop_id,
                                GValue *value, GParamSpec *pspec)
{
        EvTtsController *self = EV_TTS_CONTROLLER (object);
        switch (prop_id) {
        case PROP_ACTIVE: g_value_set_boolean (value, self->active); break;
        case PROP_PAUSED: g_value_set_boolean (value, self->paused); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
}

static void
ev_tts_controller_dispose (GObject *object)
{
        EvTtsController *self = EV_TTS_CONTROLLER (object);

        if (self->dwell_id) {
                g_source_remove (self->dwell_id);
                self->dwell_id = 0;
        }
        if (self->model)
                g_signal_handlers_disconnect_by_data (self->model, self);
        if (self->backend)
                g_signal_handlers_disconnect_by_data (self->backend, self);
        if (self->cancellable)
                g_cancellable_cancel (self->cancellable);

        if (self->bus_watch_id) {
                g_source_remove (self->bus_watch_id);
                self->bus_watch_id = 0;
        }
        if (self->playbin) {
                gst_element_set_state (self->playbin, GST_STATE_NULL);
                g_clear_pointer ((GstElement **) &self->playbin,
                                 (GDestroyNotify) gst_object_unref);
        }
        g_clear_pointer (&self->pages, g_hash_table_destroy);
        g_clear_object (&self->cancellable);
        g_clear_object (&self->backend);
        g_clear_object (&self->settings);
        g_clear_object (&self->model);

        G_OBJECT_CLASS (ev_tts_controller_parent_class)->dispose (object);
}

static void
ev_tts_controller_class_init (EvTtsControllerClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = ev_tts_controller_get_property;
        object_class->dispose = ev_tts_controller_dispose;

        props[PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
        props[PROP_PAUSED] = g_param_spec_boolean ("paused", NULL, NULL, FALSE,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
        g_object_class_install_properties (object_class, N_PROPS, props);

        signals[SIGNAL_STATUS] =
                g_signal_new ("status", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                              0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
        signals[SIGNAL_LOG] =
                g_signal_new ("log", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                              0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
        signals[SIGNAL_VOICES_CHANGED] =
                g_signal_new ("voices-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                              0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRV);
}

static void
ev_tts_controller_init (EvTtsController *self)
{
        GstBus *bus;

        if (!gst_is_initialized ())
                gst_init (NULL, NULL);

        self->volume      = 1.0;
        self->cancellable = g_cancellable_new ();
        self->pages       = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                   NULL, (GDestroyNotify) page_cache_free);
        self->backend     = ev_tts_minimax_new ();
        self->settings    = g_settings_new (TTS_SCHEMA);
        self->playbin     = gst_element_factory_make ("playbin", "ev-tts-playbin");

        g_signal_connect (self->backend, "log", G_CALLBACK (on_backend_log), self);

        if (self->playbin) {
                bus = gst_element_get_bus (self->playbin);
                self->bus_watch_id = gst_bus_add_watch (bus, bus_cb, self);
                gst_object_unref (bus);
        }
}

EvTtsController *
ev_tts_controller_new (EvView *view, EvDocumentModel *model)
{
        EvTtsController *self = g_object_new (EV_TYPE_TTS_CONTROLLER, NULL);
        self->view  = view;   /* window outlives controller */
        self->model = g_object_ref (model);
        g_signal_connect (self->model, "page-changed",
                          G_CALLBACK (on_model_page_changed), self);
        return self;
}
