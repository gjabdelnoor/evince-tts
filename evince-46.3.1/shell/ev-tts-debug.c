/* ev-tts-debug.c */

#include "ev-tts-debug.h"

#define MAX_LINES 600

static void
append_line (GtkTextView *tv, const char *line)
{
        GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
        GtkTextIter end;
        int n;

        gtk_text_buffer_get_end_iter (buf, &end);
        if (gtk_text_buffer_get_char_count (buf) > 0)
                gtk_text_buffer_insert (buf, &end, "\n", 1);
        gtk_text_buffer_insert (buf, &end, line, -1);

        /* Cap scrollback. */
        n = gtk_text_buffer_get_line_count (buf);
        if (n > MAX_LINES) {
                GtkTextIter start, cut;
                gtk_text_buffer_get_start_iter (buf, &start);
                gtk_text_buffer_get_iter_at_line (buf, &cut, n - MAX_LINES);
                gtk_text_buffer_delete (buf, &start, &cut);
        }

        /* Auto-scroll to the bottom. */
        gtk_text_buffer_get_end_iter (buf, &end);
        GtkTextMark *mark = gtk_text_buffer_get_insert (buf);
        gtk_text_buffer_move_mark (buf, mark, &end);
        gtk_text_view_scroll_mark_onscreen (tv, mark);
}

static void
on_log (EvTtsController *controller, const char *line, gpointer window)
{
        GtkTextView *tv = g_object_get_data (G_OBJECT (window), "textview");
        if (tv)
                append_line (tv, line);
}

GtkWidget *
ev_tts_debug_window_new (GtkWindow *parent, EvTtsController *controller)
{
        GtkWidget    *win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
        GtkWidget    *sw  = gtk_scrolled_window_new (NULL, NULL);
        GtkWidget    *tv  = gtk_text_view_new ();
        GdkRectangle  geo = { 0, 0, 1280, 800 };
        GdkMonitor   *mon;

        gtk_window_set_title (GTK_WINDOW (win), "TTS Debug — API calls");
        gtk_window_set_transient_for (GTK_WINDOW (win), parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (win), TRUE);

        /* ~1/16 of the screen area (quarter width × quarter height). */
        mon = gdk_display_get_primary_monitor (gtk_widget_get_display (GTK_WIDGET (parent)));
        if (mon)
                gdk_monitor_get_geometry (mon, &geo);
        gtk_window_set_default_size (GTK_WINDOW (win), geo.width / 4, geo.height / 4);

        gtk_text_view_set_editable (GTK_TEXT_VIEW (tv), FALSE);
        gtk_text_view_set_monospace (GTK_TEXT_VIEW (tv), TRUE);
        gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (tv), FALSE);
        gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (tv), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin (GTK_TEXT_VIEW (tv), 6);
        gtk_text_view_set_right_margin (GTK_TEXT_VIEW (tv), 6);

        gtk_container_add (GTK_CONTAINER (sw), tv);
        gtk_container_add (GTK_CONTAINER (win), sw);

        g_object_set_data (G_OBJECT (win), "textview", tv);
        /* Auto-disconnects when the window is destroyed. */
        g_signal_connect_object (controller, "log", G_CALLBACK (on_log), win, 0);

        return win;
}
