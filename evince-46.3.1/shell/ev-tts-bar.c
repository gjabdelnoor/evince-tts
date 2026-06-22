/* ev-tts-bar.c */

#include "ev-tts-bar.h"

struct _EvTtsBar {
        GtkBox           parent_instance;

        EvTtsController *controller;    /* not owned */

        GtkWidget       *prev_btn;
        GtkWidget       *play_btn;
        GtkWidget       *play_img;
        GtkWidget       *next_btn;
        GtkWidget       *seek;          /* GtkScale 0..1 */
        GtkWidget       *volume;        /* GtkVolumeButton */

        guint            timer_id;
        gboolean         updating;      /* guard programmatic seek updates */
};

G_DEFINE_FINAL_TYPE (EvTtsBar, ev_tts_bar, GTK_TYPE_BOX)

static void
update_state (EvTtsBar *self)
{
        gboolean active = ev_tts_controller_is_active (self->controller);
        gboolean paused = ev_tts_controller_get_paused (self->controller);
        const char *icon = (active && !paused)
                ? "media-playback-pause-symbolic"
                : "media-playback-start-symbolic";

        gtk_image_set_from_icon_name (GTK_IMAGE (self->play_img), icon,
                                      GTK_ICON_SIZE_BUTTON);

        /* Page nav + seek only make sense while reading. */
        gtk_widget_set_sensitive (self->prev_btn, active);
        gtk_widget_set_sensitive (self->next_btn, active);
        gtk_widget_set_sensitive (self->seek, active);

        if (!active) {
                self->updating = TRUE;
                gtk_range_set_value (GTK_RANGE (self->seek), 0.0);
                self->updating = FALSE;
        }
}

static gboolean
tick_cb (gpointer user_data)
{
        EvTtsBar *self = user_data;
        gint64 pos = 0, dur = 0;

        if (ev_tts_controller_get_progress (self->controller, &pos, &dur)) {
                self->updating = TRUE;
                gtk_range_set_value (GTK_RANGE (self->seek), (double) pos / (double) dur);
                self->updating = FALSE;
        }
        return G_SOURCE_CONTINUE;
}

static void
on_prev (GtkButton *b, EvTtsBar *self)  { ev_tts_controller_prev_page (self->controller); }
static void
on_next (GtkButton *b, EvTtsBar *self)  { ev_tts_controller_next_page (self->controller); }

static void
on_play (GtkButton *b, EvTtsBar *self)
{
        if (!ev_tts_controller_is_active (self->controller))
                ev_tts_controller_start (self->controller);
        else
                ev_tts_controller_set_paused (self->controller,
                                              !ev_tts_controller_get_paused (self->controller));
        update_state (self);
}

static gboolean
on_seek (GtkRange *range, GtkScrollType scroll, gdouble value, EvTtsBar *self)
{
        if (!self->updating)
                ev_tts_controller_seek_fraction (self->controller, value);
        return FALSE;   /* let the range update its visual value */
}

static void
on_volume (GtkScaleButton *button, gdouble value, EvTtsBar *self)
{
        ev_tts_controller_set_volume (self->controller, value);
}

static void
on_controller_notify (GObject *obj, GParamSpec *pspec, EvTtsBar *self)
{
        update_state (self);
}

static GtkWidget *
icon_button (const char *icon, const char *tooltip)
{
        GtkWidget *b = gtk_button_new_from_icon_name (icon, GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text (b, tooltip);
        gtk_widget_set_focus_on_click (b, FALSE);
        return b;
}

static void
ev_tts_bar_dispose (GObject *object)
{
        EvTtsBar *self = EV_TTS_BAR (object);

        if (self->timer_id) {
                g_source_remove (self->timer_id);
                self->timer_id = 0;
        }
        if (self->controller) {
                g_signal_handlers_disconnect_by_data (self->controller, self);
                self->controller = NULL;
        }
        G_OBJECT_CLASS (ev_tts_bar_parent_class)->dispose (object);
}

static void
ev_tts_bar_class_init (EvTtsBarClass *klass)
{
        G_OBJECT_CLASS (klass)->dispose = ev_tts_bar_dispose;
}

static void
ev_tts_bar_init (EvTtsBar *self)
{
        gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
        gtk_box_set_spacing (GTK_BOX (self), 2);
        gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (self)),
                                     "linked");
}

GtkWidget *
ev_tts_bar_new (EvTtsController *controller)
{
        EvTtsBar *self = g_object_new (EV_TYPE_TTS_BAR, NULL);

        self->controller = controller;

        self->prev_btn = icon_button ("media-skip-backward-symbolic", "Previous page");
        self->play_btn = gtk_button_new ();
        self->play_img = gtk_image_new_from_icon_name ("media-playback-start-symbolic",
                                                       GTK_ICON_SIZE_BUTTON);
        gtk_button_set_image (GTK_BUTTON (self->play_btn), self->play_img);
        gtk_widget_set_tooltip_text (self->play_btn, "Play / Pause");
        gtk_widget_set_focus_on_click (self->play_btn, FALSE);
        self->next_btn = icon_button ("media-skip-forward-symbolic", "Next page");

        self->seek = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
        gtk_scale_set_draw_value (GTK_SCALE (self->seek), FALSE);
        gtk_widget_set_size_request (self->seek, 130, -1);
        gtk_widget_set_valign (self->seek, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (self->seek, "Scroll through the voiceover");

        self->volume = gtk_volume_button_new ();
        gtk_scale_button_set_value (GTK_SCALE_BUTTON (self->volume),
                                    ev_tts_controller_get_volume (controller));

        gtk_box_pack_start (GTK_BOX (self), self->prev_btn, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (self), self->play_btn, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (self), self->next_btn, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (self), self->seek,     FALSE, FALSE, 4);
        gtk_box_pack_start (GTK_BOX (self), self->volume,   FALSE, FALSE, 0);

        g_signal_connect (self->prev_btn, "clicked", G_CALLBACK (on_prev), self);
        g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play), self);
        g_signal_connect (self->next_btn, "clicked", G_CALLBACK (on_next), self);
        g_signal_connect (self->seek, "change-value", G_CALLBACK (on_seek), self);
        g_signal_connect (self->volume, "value-changed", G_CALLBACK (on_volume), self);

        g_signal_connect (controller, "notify::active",
                          G_CALLBACK (on_controller_notify), self);
        g_signal_connect (controller, "notify::paused",
                          G_CALLBACK (on_controller_notify), self);

        self->timer_id = g_timeout_add (250, tick_cb, self);
        update_state (self);

        return GTK_WIDGET (self);
}
