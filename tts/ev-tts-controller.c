/* ev-tts-controller.c */

#include "ev-tts-controller.h"
#include "ev-tts-minimax.h"
#include "ev-keyring.h"

#include <ev-document-text.h>
#include <gst/gst.h>
#include <string.h>

#define TTS_KEYRING_URI "minimax-tts://default"
#define TTS_SCHEMA      "org.gnome.Evince"
/* Hard split very long sentences so a single request stays small and the
 * highlight stays legible. */
#define TTS_MAX_SENTENCE_CHARS 400

typedef struct {
        char        *text;
        EvRectangle *rects;     /* merged line rectangles (doc coords) */
        guint        n_rects;
        GBytes      *audio;     /* cached MP3, NULL until synthesized */
        char        *tmpfile;   /* written lazily for playback */
        gboolean     requested;
} Sentence;

struct _EvTtsController {
        GObject          parent_instance;

        EvView          *view;          /* not owned */
        EvDocumentModel *model;         /* owned ref */
        EvTtsMiniMax    *backend;
        GSettings       *settings;

        GstElement      *playbin;
        guint            bus_watch_id;

        GPtrArray       *sentences;     /* of Sentence* */
        gint             cur;           /* index into sentences */
        gint             page;          /* page currently being read */

        GCancellable    *cancellable;

        gboolean         active;
        gboolean         paused;
};

enum {
        PROP_0,
        PROP_ACTIVE,
        PROP_PAUSED,
        N_PROPS
};

enum {
        SIGNAL_STATUS,
        N_SIGNALS
};

static GParamSpec *props[N_PROPS];
static guint       signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (EvTtsController, ev_tts_controller, G_TYPE_OBJECT)

/* --- forward decls --- */
static void     ev_tts_controller_advance      (EvTtsController *self);
static void     ev_tts_controller_ensure_playing (EvTtsController *self);
static void     request_sentence               (EvTtsController *self, gint idx);

/* --- Sentence helpers --- */

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

                /* Skip degenerate rects (whitespace / control chars). */
                if (r.x2 <= r.x1 || r.y2 <= r.y1)
                        continue;

                if (!have_cur) {
                        cur = r;
                        have_cur = TRUE;
                        continue;
                }

                /* Same line if there is vertical overlap. */
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

/* Build self->sentences from the page text + layout. Returns TRUE if the page
 * has any readable text. */
static gboolean
build_sentences_for_page (EvTtsController *self, gint page)
{
        EvDocument  *doc = ev_document_model_get_document (self->model);
        EvPage      *ev_page;
        EvRectangle *areas = NULL;
        guint        n_areas = 0;
        char        *text;
        const char  *p;
        guint        char_start = 0, char_idx = 0;
        GString     *buf;

        g_clear_pointer (&self->sentences, g_ptr_array_unref);
        self->sentences = g_ptr_array_new_with_free_func ((GDestroyNotify) sentence_free);

        if (!doc || !EV_IS_DOCUMENT_TEXT (doc))
                return FALSE;

        ev_page = ev_document_get_page (doc, page);
        if (!ev_page)
                return FALSE;

        text = ev_document_text_get_text (EV_DOCUMENT_TEXT (doc), ev_page);
        ev_document_text_get_text_layout (EV_DOCUMENT_TEXT (doc), ev_page,
                                          &areas, &n_areas);
        g_object_unref (ev_page);

        if (!text || !*text) {
                g_free (text);
                g_free (areas);
                return FALSE;
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
                                g_ptr_array_add (self->sentences, s);
                        } else {
                                g_free (t);
                        }
                        g_string_truncate (buf, 0);
                        char_start = char_idx;
                }
        }
        /* trailing text without terminator */
        if (buf->len > 0) {
                char *t = g_strstrip (g_strdup (buf->str));
                if (t && *t) {
                        Sentence *s = g_new0 (Sentence, 1);
                        s->text = t;
                        if (areas)
                                s->rects = merge_line_rects (areas, n_areas,
                                                             char_start, char_idx,
                                                             &s->n_rects);
                        g_ptr_array_add (self->sentences, s);
                } else {
                        g_free (t);
                }
        }

        g_string_free (buf, TRUE);
        g_free (text);
        g_free (areas);

        return self->sentences->len > 0;
}

/* --- playback --- */

static Sentence *
cur_sentence (EvTtsController *self)
{
        if (!self->sentences || self->cur < 0 ||
            self->cur >= (gint) self->sentences->len)
                return NULL;
        return g_ptr_array_index (self->sentences, self->cur);
}

static void
highlight_current (EvTtsController *self)
{
        Sentence *s = cur_sentence (self);
        if (!self->view)
                return;
        if (s && s->n_rects > 0)
                ev_view_set_tts_highlight (self->view, self->page, s->rects, s->n_rects);
        else
                ev_view_set_tts_highlight (self->view, self->page, NULL, 0);
}

static gboolean
play_sentence (EvTtsController *self, Sentence *s)
{
        g_autofree char *uri = NULL;
        GError *error = NULL;

        if (!s->audio)
                return FALSE;

        if (!s->tmpfile) {
                int   fd;
                gsize len;
                const char *data = g_bytes_get_data (s->audio, &len);

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
        }

        uri = g_filename_to_uri (s->tmpfile, NULL, NULL);
        gst_element_set_state (self->playbin, GST_STATE_READY);
        g_object_set (self->playbin, "uri", uri, NULL);
        gst_element_set_state (self->playbin,
                               self->paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);

        highlight_current (self);
        emit_status (self, "Reading page %d · sentence %d/%u",
                     self->page + 1, self->cur + 1, self->sentences->len);

        /* Prefetch the next sentence. */
        request_sentence (self, self->cur + 1);
        return TRUE;
}

static void
on_synth_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
        EvTtsController *self = EV_TTS_CONTROLLER (user_data);
        GError *error = NULL;
        GBytes *mp3;
        gint    idx = -1;

        mp3 = ev_tts_minimax_synthesize_finish (EV_TTS_MINIMAX (source), res, &error);
        if (!mp3) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        emit_status (self, "TTS error: %s",
                                     error ? error->message : "unknown");
                g_clear_error (&error);
                g_object_unref (self);
                return;
        }

        /* Find which sentence this belongs to: the lowest requested-but-unfilled.
         * Requests are issued in order, so match by first NULL audio that was
         * requested. */
        for (guint i = 0; self->sentences && i < self->sentences->len; i++) {
                Sentence *s = g_ptr_array_index (self->sentences, i);
                if (s->requested && !s->audio) {
                        s->audio = g_bytes_ref (mp3);
                        idx = i;
                        break;
                }
        }
        g_bytes_unref (mp3);

        /* If this is the sentence we're waiting to play and nothing is playing. */
        if (self->active && idx == self->cur) {
                GstState st = GST_STATE_NULL;
                gst_element_get_state (self->playbin, &st, NULL, 0);
                if (st != GST_STATE_PLAYING && st != GST_STATE_PAUSED)
                        play_sentence (self, cur_sentence (self));
        }

        g_object_unref (self);
}

static void
request_sentence (EvTtsController *self, gint idx)
{
        Sentence *s;

        if (!self->sentences || idx < 0 || idx >= (gint) self->sentences->len)
                return;
        s = g_ptr_array_index (self->sentences, idx);
        if (s->requested || s->audio)
                return;

        s->requested = TRUE;
        ev_tts_minimax_synthesize_async (self->backend, s->text, self->cancellable,
                                         on_synth_ready, g_object_ref (self));
}

static void
ev_tts_controller_ensure_playing (EvTtsController *self)
{
        Sentence *s = cur_sentence (self);

        if (!s) {
                ev_tts_controller_advance (self);
                return;
        }
        if (s->audio)
                play_sentence (self, s);
        else {
                highlight_current (self);
                request_sentence (self, self->cur);
                emit_status (self, "Synthesizing… page %d sentence %d/%u",
                             self->page + 1, self->cur + 1, self->sentences->len);
        }
}

/* Move to the next page that has text, or stop if at the end. */
static void
start_next_page (EvTtsController *self)
{
        EvDocument *doc = ev_document_model_get_document (self->model);
        gint n_pages = doc ? ev_document_get_n_pages (doc) : 0;
        gint page = self->page + 1;

        for (; page < n_pages; page++) {
                if (build_sentences_for_page (self, page)) {
                        self->page = page;
                        self->cur = 0;
                        ev_document_model_set_page (self->model, page);
                        ev_tts_controller_ensure_playing (self);
                        return;
                }
        }
        emit_status (self, "Finished reading.");
        ev_tts_controller_stop (self);
}

static void
ev_tts_controller_advance (EvTtsController *self)
{
        self->cur++;
        if (self->sentences && self->cur < (gint) self->sentences->len)
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

void
ev_tts_controller_start (EvTtsController *self)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));

        if (self->active)
                return;

        ev_tts_controller_reload_config (self);
        if (!ev_tts_minimax_is_configured (self->backend)) {
                emit_status (self, "Set your MiniMax API key, GroupId and voice in "
                                   "TTS Settings first.");
                return;
        }
        if (!ev_document_model_get_document (self->model)) {
                emit_status (self, "No document open.");
                return;
        }

        self->cancellable = g_cancellable_new ();
        self->active = TRUE;
        self->paused = FALSE;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);

        self->page = ev_document_model_get_page (self->model);
        if (build_sentences_for_page (self, self->page)) {
                self->cur = 0;
                ev_tts_controller_ensure_playing (self);
        } else {
                /* Current page has no text; search forward. */
                self->cur = 0;
                start_next_page (self);
        }
}

void
ev_tts_controller_stop (EvTtsController *self)
{
        g_return_if_fail (EV_IS_TTS_CONTROLLER (self));

        if (self->cancellable) {
                g_cancellable_cancel (self->cancellable);
                g_clear_object (&self->cancellable);
        }
        if (self->playbin)
                gst_element_set_state (self->playbin, GST_STATE_NULL);
        if (self->view)
                ev_view_set_tts_highlight (self->view, self->page, NULL, 0);

        g_clear_pointer (&self->sentences, g_ptr_array_unref);
        self->cur = 0;

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
        g_clear_pointer (&self->sentences, g_ptr_array_unref);
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
                              0, NULL, NULL, NULL,
                              G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
ev_tts_controller_init (EvTtsController *self)
{
        GstBus *bus;

        if (!gst_is_initialized ())
                gst_init (NULL, NULL);

        self->backend  = ev_tts_minimax_new ();
        self->settings = g_settings_new (TTS_SCHEMA);
        self->playbin  = gst_element_factory_make ("playbin", "ev-tts-playbin");

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
        return self;
}
