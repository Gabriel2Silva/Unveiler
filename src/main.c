/* main.c */
#include <adwaita.h>
#include "fm-window.h"

static void
on_activate(GtkApplication *app, gpointer ud)
{
  (void)ud;
  FmWindow *win = fm_window_new(app);
  gtk_window_present(GTK_WINDOW(win));
}

static void
on_open(GApplication *app, GFile **files, int n, const char *hint, gpointer ud)
{
  (void)hint; (void)ud;
  FmWindow *win = fm_window_new(GTK_APPLICATION(app));
  gtk_window_present(GTK_WINDOW(win));
  if (n > 0)
    fm_window_open_archive(win, files[0]);
}

int main(int argc, char *argv[])
{
  AdwApplication *app = adw_application_new("org.unveiler.App",
                                            G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(app, "open",     G_CALLBACK(on_open),     NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
