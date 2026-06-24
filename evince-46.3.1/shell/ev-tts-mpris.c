/* ev-tts-mpris.c
 *
 * Minimal MPRIS2 (org.mpris.MediaPlayer2 + .Player) implementation so the
 * desktop routes media keys (F5/play-pause) and Bluetooth headphone
 * play/pause to the Read-Aloud player. */

#include "ev-tts-mpris.h"

#include <unistd.h>

static const char *MPRIS_XML =
"<node>"
"  <interface name='org.mpris.MediaPlayer2'>"
"    <method name='Raise'/>"
"    <method name='Quit'/>"
"    <property name='CanQuit' type='b' access='read'/>"
"    <property name='CanRaise' type='b' access='read'/>"
"    <property name='HasTrackList' type='b' access='read'/>"
"    <property name='Identity' type='s' access='read'/>"
"    <property name='DesktopEntry' type='s' access='read'/>"
"    <property name='SupportedUriSchemes' type='as' access='read'/>"
"    <property name='SupportedMimeTypes' type='as' access='read'/>"
"  </interface>"
"  <interface name='org.mpris.MediaPlayer2.Player'>"
"    <method name='Next'/>"
"    <method name='Previous'/>"
"    <method name='Pause'/>"
"    <method name='PlayPause'/>"
"    <method name='Stop'/>"
"    <method name='Play'/>"
"    <method name='Seek'><arg name='Offset' type='x' direction='in'/></method>"
"    <method name='SetPosition'>"
"      <arg name='TrackId' type='o' direction='in'/>"
"      <arg name='Position' type='x' direction='in'/>"
"    </method>"
"    <property name='PlaybackStatus' type='s' access='read'/>"
"    <property name='Rate' type='d' access='read'/>"
"    <property name='Metadata' type='a{sv}' access='read'/>"
"    <property name='Volume' type='d' access='readwrite'/>"
"    <property name='Position' type='x' access='read'/>"
"    <property name='MinimumRate' type='d' access='read'/>"
"    <property name='MaximumRate' type='d' access='read'/>"
"    <property name='CanGoNext' type='b' access='read'/>"
"    <property name='CanGoPrevious' type='b' access='read'/>"
"    <property name='CanPlay' type='b' access='read'/>"
"    <property name='CanPause' type='b' access='read'/>"
"    <property name='CanSeek' type='b' access='read'/>"
"    <property name='CanControl' type='b' access='read'/>"
"  </interface>"
"</node>";

struct _EvTtsMpris {
        GObject          parent_instance;

        EvTtsController *controller;    /* not owned */
        GtkWindow       *window;        /* not owned */

        guint            owner_id;
        GDBusConnection *connection;    /* not owned (from bus) */
        GDBusNodeInfo   *node;
        guint            root_reg;
        guint            player_reg;
};

G_DEFINE_FINAL_TYPE (EvTtsMpris, ev_tts_mpris, G_TYPE_OBJECT)

static const char *
playback_status (EvTtsMpris *self)
{
        if (!ev_tts_controller_is_active (self->controller))
                return "Stopped";
        return ev_tts_controller_get_paused (self->controller) ? "Paused" : "Playing";
}

/* --- method dispatch --- */

static void
root_method (GDBusConnection *conn, const char *sender, const char *path,
             const char *iface, const char *method, GVariant *params,
             GDBusMethodInvocation *invocation, gpointer user_data)
{
        EvTtsMpris *self = user_data;

        if (g_strcmp0 (method, "Raise") == 0 && self->window)
                gtk_window_present (self->window);
        else if (g_strcmp0 (method, "Quit") == 0 && self->window)
                gtk_widget_destroy (GTK_WIDGET (self->window));

        g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
player_method (GDBusConnection *conn, const char *sender, const char *path,
               const char *iface, const char *method, GVariant *params,
               GDBusMethodInvocation *invocation, gpointer user_data)
{
        EvTtsMpris *self = user_data;
        EvTtsController *c = self->controller;

        if (g_strcmp0 (method, "PlayPause") == 0) {
                ev_tts_controller_play_pause (c);
        } else if (g_strcmp0 (method, "Play") == 0) {
                if (!ev_tts_controller_is_active (c))
                        ev_tts_controller_start (c);
                else
                        ev_tts_controller_set_paused (c, FALSE);
        } else if (g_strcmp0 (method, "Pause") == 0) {
                ev_tts_controller_set_paused (c, TRUE);
        } else if (g_strcmp0 (method, "Stop") == 0) {
                ev_tts_controller_stop (c);
        } else if (g_strcmp0 (method, "Next") == 0) {
                ev_tts_controller_next_page (c);
        } else if (g_strcmp0 (method, "Previous") == 0) {
                ev_tts_controller_prev_page (c);
        }
        /* Seek / SetPosition: accepted but no-op (per-sentence clips). */

        g_dbus_method_invocation_return_value (invocation, NULL);
}

/* --- property reads --- */

static GVariant *
root_get_property (GDBusConnection *conn, const char *sender, const char *path,
                   const char *iface, const char *prop, GError **error,
                   gpointer user_data)
{
        if (g_strcmp0 (prop, "CanQuit") == 0)   return g_variant_new_boolean (TRUE);
        if (g_strcmp0 (prop, "CanRaise") == 0)  return g_variant_new_boolean (TRUE);
        if (g_strcmp0 (prop, "HasTrackList") == 0) return g_variant_new_boolean (FALSE);
        if (g_strcmp0 (prop, "Identity") == 0)  return g_variant_new_string ("Document Viewer (Read Aloud)");
        if (g_strcmp0 (prop, "DesktopEntry") == 0) return g_variant_new_string ("org.gnome.Evince");
        if (g_strcmp0 (prop, "SupportedUriSchemes") == 0 ||
            g_strcmp0 (prop, "SupportedMimeTypes") == 0) {
                GVariantBuilder b;
                g_variant_builder_init (&b, G_VARIANT_TYPE ("as"));
                return g_variant_builder_end (&b);
        }
        return NULL;
}

static GVariant *
player_get_property (GDBusConnection *conn, const char *sender, const char *path,
                     const char *iface, const char *prop, GError **error,
                     gpointer user_data)
{
        EvTtsMpris *self = user_data;

        if (g_strcmp0 (prop, "PlaybackStatus") == 0)
                return g_variant_new_string (playback_status (self));
        if (g_strcmp0 (prop, "Rate") == 0 ||
            g_strcmp0 (prop, "MinimumRate") == 0 ||
            g_strcmp0 (prop, "MaximumRate") == 0)
                return g_variant_new_double (1.0);
        if (g_strcmp0 (prop, "Volume") == 0)
                return g_variant_new_double (ev_tts_controller_get_volume (self->controller));
        if (g_strcmp0 (prop, "Position") == 0) {
                gint64 pos = 0, dur = 0;
                ev_tts_controller_get_progress (self->controller, &pos, &dur);
                return g_variant_new_int64 (pos / 1000);   /* ns -> us */
        }
        if (g_strcmp0 (prop, "Metadata") == 0) {
                GVariantBuilder b;
                g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
                g_variant_builder_add (&b, "{sv}", "mpris:trackid",
                                       g_variant_new_object_path ("/org/gnome/Evince/tts/track"));
                return g_variant_builder_end (&b);
        }
        if (g_strcmp0 (prop, "CanControl") == 0 ||
            g_strcmp0 (prop, "CanPlay") == 0 ||
            g_strcmp0 (prop, "CanPause") == 0 ||
            g_strcmp0 (prop, "CanGoNext") == 0 ||
            g_strcmp0 (prop, "CanGoPrevious") == 0)
                return g_variant_new_boolean (TRUE);
        if (g_strcmp0 (prop, "CanSeek") == 0)
                return g_variant_new_boolean (FALSE);
        return NULL;
}

static gboolean
player_set_property (GDBusConnection *conn, const char *sender, const char *path,
                     const char *iface, const char *prop, GVariant *value,
                     GError **error, gpointer user_data)
{
        EvTtsMpris *self = user_data;
        if (g_strcmp0 (prop, "Volume") == 0)
                ev_tts_controller_set_volume (self->controller, g_variant_get_double (value));
        return TRUE;
}

static const GDBusInterfaceVTable root_vtable = {
        root_method, root_get_property, NULL, { 0 }
};
static const GDBusInterfaceVTable player_vtable = {
        player_method, player_get_property, player_set_property, { 0 }
};

/* Notify peers (the media-keys daemon) when play state changes. */
static void
emit_player_props_changed (EvTtsMpris *self)
{
        GVariantBuilder props;
        GError *error = NULL;

        if (!self->connection)
                return;

        g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&props, "{sv}", "PlaybackStatus",
                               g_variant_new_string (playback_status (self)));

        g_dbus_connection_emit_signal (
                self->connection, NULL, "/org/mpris/MediaPlayer2",
                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                g_variant_new ("(sa{sv}as)", "org.mpris.MediaPlayer2.Player",
                               &props, NULL),
                &error);
        g_clear_error (&error);
}

static void
on_controller_notify (GObject *obj, GParamSpec *pspec, EvTtsMpris *self)
{
        emit_player_props_changed (self);
}

static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
        EvTtsMpris *self = user_data;

        self->connection = connection;
        self->root_reg = g_dbus_connection_register_object (
                connection, "/org/mpris/MediaPlayer2",
                self->node->interfaces[0], &root_vtable, self, NULL, NULL);
        self->player_reg = g_dbus_connection_register_object (
                connection, "/org/mpris/MediaPlayer2",
                self->node->interfaces[1], &player_vtable, self, NULL, NULL);
}

static void
ev_tts_mpris_dispose (GObject *object)
{
        EvTtsMpris *self = EV_TTS_MPRIS (object);

        if (self->controller)
                g_signal_handlers_disconnect_by_data (self->controller, self);
        if (self->connection && self->root_reg)
                g_dbus_connection_unregister_object (self->connection, self->root_reg);
        if (self->connection && self->player_reg)
                g_dbus_connection_unregister_object (self->connection, self->player_reg);
        self->root_reg = self->player_reg = 0;
        if (self->owner_id) {
                g_bus_unown_name (self->owner_id);
                self->owner_id = 0;
        }
        g_clear_pointer (&self->node, g_dbus_node_info_unref);
        self->controller = NULL;
        self->window = NULL;

        G_OBJECT_CLASS (ev_tts_mpris_parent_class)->dispose (object);
}

static void
ev_tts_mpris_class_init (EvTtsMprisClass *klass)
{
        G_OBJECT_CLASS (klass)->dispose = ev_tts_mpris_dispose;
}

static void
ev_tts_mpris_init (EvTtsMpris *self)
{
}

EvTtsMpris *
ev_tts_mpris_new (EvTtsController *controller, GtkWindow *window)
{
        EvTtsMpris *self = g_object_new (EV_TYPE_TTS_MPRIS, NULL);
        g_autofree char *name = NULL;

        self->controller = controller;
        self->window = window;
        self->node = g_dbus_node_info_new_for_xml (MPRIS_XML, NULL);

        /* Unique bus name per process so multiple windows don't clash. */
        name = g_strdup_printf ("org.mpris.MediaPlayer2.evince-tts.instance%d", getpid ());
        self->owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, name,
                                         G_BUS_NAME_OWNER_FLAGS_NONE,
                                         on_bus_acquired, NULL, NULL, self, NULL);

        g_signal_connect (controller, "notify::active",
                          G_CALLBACK (on_controller_notify), self);
        g_signal_connect (controller, "notify::paused",
                          G_CALLBACK (on_controller_notify), self);

        return self;
}
