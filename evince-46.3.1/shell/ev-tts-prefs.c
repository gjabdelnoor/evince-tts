/* ev-tts-prefs.c */

#include "ev-tts-prefs.h"
#include "ev-tts-minimax.h"
#include "ev-keyring.h"

#define TTS_KEYRING_URI "minimax-tts://default"
#define TTS_SCHEMA      "org.gnome.Evince"

static const struct { const char *host; const char *label; } regions[] = {
        { "api.minimax.io",    "Global (api.minimax.io)" },
        { "api-uw.minimax.io", "US West (api-uw.minimax.io)" },
        { "api.minimaxi.chat", "Mainland China (api.minimaxi.chat)" },
};

typedef struct {
        EvTtsController *controller;
        GSettings       *settings;
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
        const char *key = gtk_entry_get_text (GTK_ENTRY (p->api_key));
        const char *host = regions[current_region (p)].host;
        g_autofree char *keep = g_strdup (gtk_entry_get_text (voice_entry (p)));
        GError *error = NULL;
        char  **voices;
        guint   n = 0;

        gtk_label_set_text (GTK_LABEL (p->status), "Fetching voices…");

        voices = ev_tts_minimax_list_cloned_voices (host, key, &error);
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
                gtk_label_set_text (GTK_LABEL (p->status),
                                    "No cloned voices found on this account.");
        else {
                g_autofree char *msg =
                        g_strdup_printf ("Found %u cloned voice%s.", n, n == 1 ? "" : "s");
                gtk_label_set_text (GTK_LABEL (p->status), msg);
        }
}

static void
on_response (GtkDialog *dialog, int response, gpointer user_data)
{
        Prefs *p = user_data;

        if (response == GTK_RESPONSE_OK) {
                const char *key = gtk_entry_get_text (GTK_ENTRY (p->api_key));

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

        host  = g_settings_get_string (p->settings, "tts-host");
        group = g_settings_get_string (p->settings, "tts-group-id");
        voice = g_settings_get_string (p->settings, "tts-voice-id");
        model = g_settings_get_string (p->settings, "tts-model");
        key   = ev_keyring_lookup_password (TTS_KEYRING_URI);

        p->api_key = add_row (GTK_GRID (grid), row++, "API key:", gtk_entry_new ());
        gtk_entry_set_visibility (GTK_ENTRY (p->api_key), FALSE);
        gtk_entry_set_placeholder_text (GTK_ENTRY (p->api_key), "MiniMax API key");
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
        gtk_entry_set_text (GTK_ENTRY (p->model), (model && *model) ? model : "speech-2.6-hd");

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
