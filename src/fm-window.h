/* fm-window.h */
#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

#define FM_TYPE_WINDOW (fm_window_get_type())
G_DECLARE_FINAL_TYPE(FmWindow, fm_window, FM, WINDOW, AdwApplicationWindow)

FmWindow *fm_window_new(GtkApplication *app);
void      fm_window_open_archive(FmWindow *self, GFile *file);

G_END_DECLS
