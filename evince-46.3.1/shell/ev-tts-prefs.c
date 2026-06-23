/* ev-tts-prefs.c */

#include "ev-tts-prefs.h"
#include "ev-tts-backend.h"
#include "ev-keyring.h"

#define TTS_KEYRING_URI "minimax-tts://default"
#define TTS_SCHEMA      "org.gnome.Evince"

static const struct { const char *host; const char *label; } regions[] = {
        { "api.minimax.io",    "Global (api.minimax.io)" },
        { "api-uw.minimax.io", "US West (api-uw.minimax.io)" },
        { "api.minimaxi.chat", "Mainland China (api.minimaxi.chat)" },
};

/* Presets. OpenAI / OpenRouter / Local all use the same "openai" backend,
 * differing only by base URL. */
static const struct {
        const char *id;
        const char *label;
        const char *provider;   /* backend id: minimax / openai / google */
        const char *base;       /* default base URL (openai-family) */
} providers[] = {
        { "minimax",    "MiniMax (custom cloned voice)", "minimax", ""                              },
        { "openai",     "OpenAI",                        "openai",  "https://api.openai.com/v1"     },
        { "openrouter", "OpenRouter",                    "openai",  "https://openrouter.ai/api/v1"  },
        { "local",      "Local (OpenAI-compatible)",     "openai",  "http://localhost:8880/v1"      },
        { "google",     "Google Cloud TTS",              "google",  ""                              },
};

typedef struct {
        EvTtsController *controller;
        GSettings       *settings;
        GtkWidget       *provider;   /* GtkComboBoxText (preset id) */
        GtkWidget       *base_url;
        GtkWidget       *api_key;
        GtkWidget       *region;
        GtkWidget       *group_id;
        GtkWidget       *voice_id;   /* GtkComboBoxText (with entry) */
        GtkWidget       *model;
        GtkWidget       *speed;
        GtkWidget       *vol;
        GtkWidget       *pitch;
        GtkWidget       *status;
} Prefs;

/* Index into providers[] for the active preset (0 if none). */
static int
current_provider (Prefs *p)
{
        const char *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (p->provider));
        for (guint i = 0; i < G_N_ELEMENTS (providers); i++)
                if (g_strcmp0 (id, providers[i].id) == 0)
                        return i;
        return 0;
}

static GtkWidget *
add_row (GtkGrid *grid, int row, const char *label, GtkWidget *w)
{
        GtkWidget *l = gtk_label_new (label);
        gtk_widget_set_halign (l, GTK_ALIGN_END);
        gtk_widget_set_hexpand (w, TRUE);
        gtk_grid_attach (grid, l, 0, row, 1, 1);
        gtk_grid_attach (grid, w, 1, row, 1, 1);
        return w;
}

static GtkEntry *
voice_entry (Prefs *p)
{
        return GTK_ENTRY (gtk_bin_get_child (GTK_BIN (p->voice_id)));
}

static int
current_region (Prefs *p)
{
        int idx = gtk_combo_box_get_active (GTK_COMBO_BOX (p->region));
        return idx < 0 ? 0 : idx;
}

static void
on_fetch_voices (GtkButton *button, gpointer user_data)
{
        Prefs *p = user_data;
        int    pi = current_provider (p);
        EvTtsProvider provider = ev_tts_provider_from_string (providers[pi].provider);
        const char *key = gtk_entry_get_text (GTK_ENTRY (p->api_key));
        const char *endpoint = (provider == EV_TTS_PROVIDER_MINIMAX)
                                       ? regions[current_region (p)].host
                                       : gtk_entry_get_text (GTK_ENTRY (p->base_url));
        g_autofree char *keep = g_strdup (gtk_entry_get_text (voice_entry (p)));
        GError *error = NULL;
        char  **voices;
        guint   n = 0;

        gtk_label_set_text (GTK_LABEL (p->status), "Fetching voices…");

        voices = ev_tts_backend_list_voices (provider, endpoint, key, &error);
        if (!voices) {
                gtk_label_set_text (GTK_LABEL (p->status),
                                    error ? error->message : "Failed to fetch voices.");
                g_clear_error (&error);
                return;
        }

        gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (p->voice_id));
        for (char **v = voices; *v; v++, n++)
                gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (p->voice_id), *v);
        gtk_entry_set_text (voice_entry (p), keep);   /* keep prior selection */
        g_strfreev (voices);

        if (n == 0)
                gtk_label_set_text (GTK_LABEL (p->status), "No voices found.");
        else {
                g_autofree char *msg =
                        g_strdup_printf ("Found %u voice%s.", n, n == 1 ? "" : "s");
                gtk_label_set_text (GTK_LABEL (p->status), msg);
        }
}

/* When the provider preset changes, prime the base-URL field with its default. */
static void
on_provider_changed (GtkComboBox *combo, gpointer user_data)
{
        Prefs *p = user_data;
        int    pi = current_provider (p);
        gtk_entry_set_text (GTK_ENTRY (p->base_url), providers[pi].base);
}

static void
on_response (GtkDialog *dialog, int response, gpointer user_data)
{
        Prefs *p = user_data;

        if (response == GTK_RESPONSE_OK) {
                const char *key = gtk_entry_get_text (GTK_ENTRY (p->api_key));
                int pi = current_provider (p);

                g_settings_set_string (p->settings, "tts-provider", providers[pi].provider);
                g_settings_set_string (p->settings, "tts-base-url",
                                       gtk_entry_get_text (GTK_ENTRY (p->base_url)));
                g_settings_set_string (p->settings, "tts-host",
                                       regions[current_region (p)].host);
                g_settings_set_string (p->settings, "tts-group-id",
                                       gtk_entry_get_text (GTK_ENTRY (p->group_id)));
                g_settings_set_string (p->settings, "tts-voice-id",
                                       gtk_entry_get_text (voice_entry (p)));
                g_settings_set_string (p->settings, "tts-model",
                                       gtk_entry_get_text (GTK_ENTRY (p->model)));
                g_settings_set_double (p->settings, "tts-speed",
                                       gtk_spin_button_get_value (GTK_SPIN_BUTTON (p->speed)));
                g_settings_set_double (p->settings, "tts-vol",
                                       gtk_spin_button_get_value (GTK_SPIN_BUTTON (p->vol)));
                g_settings_set_int (p->settings, "tts-pitch",
                                    gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (p->pitch)));

                if (key && *key) {
                        g_autofree char *clean = g_strstrip (g_strdup (key));
                        ev_keyring_save_password (TTS_KEYRING_URI, clean,
                                                  G_PASSWORD_SAVE_PERMANENTLY);
                }

                if (p->controller) {
                        ev_tts_controller_reload_config (p->controller);
                        ev_tts_controller_invalidate_cache (p->controller);
                }
        }

        g_object_unref (p->settings);
        g_free (p);
        gtk_widget_destroy (GTK_WIDGET (dialog));
}

void
ev_tts_show_preferences (GtkWindow *parent, EvTtsController *controller)
{
        Prefs     *p = g_new0 (Prefs, 1);
        GtkWidget *dialog, *content, *grid, *voice_box, *fetch;
        g_autofree char *prov = NULL, *base = NULL;
        g_autofree char *host = NULL, *group = NULL, *voice = NULL, *model = NULL, *key = NULL;
        int row = 0;

        p->controller = controller;
        p->settings = g_settings_new (TTS_SCHEMA);

        dialog = gtk_dialog_new_with_buttons ("TTS Settings", parent,
                                              GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                              "_Cancel", GTK_RESPONSE_CANCEL,
                                              "_Save", GTK_RESPONSE_OK,
                                              NULL);
        gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
        content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

        grid = gtk_grid_new ();
        gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
        gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
        g_object_set (grid, "margin", 12, NULL);
        gtk_container_add (GTK_CONTAINER (content), grid);

        prov  = g_settings_get_string (p->settings, "tts-provider");
        base  = g_settings_get_string (p->settings, "tts-base-url");
        host  = g_settings_get_string (p->settings, "tts-host");
        group = g_settings_get_string (p->settings, "tts-group-id");
        voice = g_settings_get_string (p->settings, "tts-voice-id");
        model = g_settings_get_string (p->settings, "tts-model");
        key   = ev_keyring_lookup_password (TTS_KEYRING_URI);

        /* Provider preset. Match stored (provider,base) back onto a preset id. */
        p->provider = add_row (GTK_GRID (grid), row++, "Provider:", gtk_combo_box_text_new ());
        for (guint i = 0; i < G_N_ELEMENTS (providers); i++)
                gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (p->provider),
                                           providers[i].id, providers[i].label);
        {
                const char *want = "minimax";
                if (g_strcmp0 (prov, "google") == 0)
                        want = "google";
                else if (g_strcmp0 (prov, "openai") == 0) {
                        want = "openai";
                        for (guint i = 0; i < G_N_ELEMENTS (providers); i++)
                                if (g_strcmp0 (providers[i].provider, "openai") == 0 &&
                                    g_strcmp0 (providers[i].base, base) == 0)
                                        want = providers[i].id;
                }
                gtk_combo_box_set_active_id (GTK_COMBO_BOX (p->provider), want);
        }

        p->base_url = add_row (GTK_GRID (grid), row++, "Base URL:", gtk_entry_new ());
        gtk_entry_set_placeholder_text (GTK_ENTRY (p->base_url),
                                        "https://… /v1  (OpenAI-compatible only)");
        gtk_entry_set_text (GTK_ENTRY (p->base_url), base ? base : "");
        /* Connect after the initial value is set so it isn't clobbered on load. */
        g_signal_connect (p->provider, "changed", G_CALLBACK (on_provider_changed), p);

        p->api_key = add_row (GTK_GRID (grid), row++, "API key:", gtk_entry_new ());
        gtk_entry_set_visibility (GTK_ENTRY (p->api_key), FALSE);
        gtk_entry_set_placeholder_text (GTK_ENTRY (p->api_key),
                                        "API key (blank for most local servers)");
        if (key)
                gtk_entry_set_text (GTK_ENTRY (p->api_key), key);

        p->region = add_row (GTK_GRID (grid), row++, "Region:", gtk_combo_box_text_new ());
        for (guint i = 0; i < G_N_ELEMENTS (regions); i++) {
                gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (p->region),
                                                regions[i].label);
                if (g_strcmp0 (host, regions[i].host) == 0)
                        gtk_combo_box_set_active (GTK_COMBO_BOX (p->region), i);
        }
        if (gtk_combo_box_get_active (GTK_COMBO_BOX (p->region)) < 0)
                gtk_combo_box_set_active (GTK_COMBO_BOX (p->region), 0);

        p->group_id = add_row (GTK_GRID (grid), row++, "GroupId:", gtk_entry_new ());
        gtk_entry_set_placeholder_text (GTK_ENTRY (p->group_id),
                                        "optional (not needed for api.minimax.io)");
        gtk_entry_set_text (GTK_ENTRY (p->group_id), group ? group : "");

        /* Voice ID: editable combo + a button to fetch cloned voices live. */
        voice_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        p->voice_id = gtk_combo_box_text_new_with_entry ();
        gtk_entry_set_placeholder_text (voice_entry (p), "cloned voice id");
        if (voice && *voice) {
                gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (p->voice_id), voice);
                gtk_entry_set_text (voice_entry (p), voice);
        }
        fetch = gtk_button_new_with_label ("Fetch voices");
        g_signal_connect (fetch, "clicked", G_CALLBACK (on_fetch_voices), p);
        gtk_box_pack_start (GTK_BOX (voice_box), p->voice_id, TRUE, TRUE, 0);
        gtk_box_pack_start (GTK_BOX (voice_box), fetch, FALSE, FALSE, 0);
        add_row (GTK_GRID (grid), row++, "Voice ID:", voice_box);

        p->model = add_row (GTK_GRID (grid), row++, "Model:", gtk_entry_new ());
        gtk_entry_set_text (GTK_ENTRY (p->model), (model && *model) ? model : "speech-2.8-hd");

        p->speed = add_row (GTK_GRID (grid), row++, "Speed:",
                            gtk_spin_button_new_with_range (0.5, 2.0, 0.05));
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (p->speed),
                                   g_settings_get_double (p->settings, "tts-speed"));

        p->vol = add_row (GTK_GRID (grid), row++, "Volume:",
                          gtk_spin_button_new_with_range (0.1, 10.0, 0.1));
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (p->vol),
                                   g_settings_get_double (p->settings, "tts-vol"));

        p->pitch = add_row (GTK_GRID (grid), row++, "Pitch:",
                            gtk_spin_button_new_with_range (-12, 12, 1));
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (p->pitch),
                                   g_settings_get_int (p->settings, "tts-pitch"));

        p->status = gtk_label_new (NULL);
        gtk_widget_set_halign (p->status, GTK_ALIGN_START);
        gtk_label_set_xalign (GTK_LABEL (p->status), 0.0);
        gtk_grid_attach (GTK_GRID (grid), p->status, 0, row++, 2, 1);

        g_signal_connect (dialog, "response", G_CALLBACK (on_response), p);
        gtk_widget_show_all (dialog);
}
