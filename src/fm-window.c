/* fm-window.c */
#include "fm-window.h"
#include "fm-file-item.h"
#include "../bridge/fm-bridge-api.h"
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>
#include <unistd.h>
#include <string.h>

struct _FmWindow {
  AdwApplicationWindow parent;

  AdwHeaderBar  *header;
  GtkWidget     *column_view;
  GtkWidget     *scrolled;
  GtkWidget     *status;
  GListStore    *store;
  GtkMultiSelection *selection;

  FmBridge      *bridge;
  FmArchive     *archive;
  char          *archive_path;

  /* virtual directory navigation */
  char          *current_dir;  /* e.g. "" or "subdir/" */

  /* nested archive stack */
  GPtrArray     *archive_stack;  /* elements: FmArchiveFrame* */

  /* drag-out temp dir */
  char          *drag_temp_dir;
  guint          drag_cleanup_id;

  /* search */
  GtkWidget     *search_bar;
  GtkWidget     *search_entry;
  GtkFilterListModel *filter_model;
  GtkCustomFilter    *filter;
};

G_DEFINE_TYPE(FmWindow, fm_window, ADW_TYPE_APPLICATION_WINDOW)

/* generic modal dialog response tracker */
typedef struct { gboolean done; int response; } ModalDlgData;
static void on_modal_response(GtkDialog *d, int r, gpointer ud) {
  ModalDlgData *data = ud; data->response = r; data->done = TRUE; (void)d;
}

/* ---- helpers ---- */

/* Safe recursive directory removal — no shell involved */
static void
remove_dir_recursive(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree char *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
          !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_dir_recursive(child);
      else
        g_unlink(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

/* sort: directories first, then alphabetical by name */
static int
item_compare(gconstpointer a, gconstpointer b, gpointer ud)
{
  (void)ud;
  FmFileItem *ia = FM_FILE_ITEM((gpointer)a);
  FmFileItem *ib = FM_FILE_ITEM((gpointer)b);
  gboolean da = fm_file_item_is_dir(ia);
  gboolean db = fm_file_item_is_dir(ib);
  if (da != db) return db - da;  /* dirs first */
  return g_utf8_collate(fm_file_item_get_name(ia), fm_file_item_get_name(ib));
}

static void
populate_store(FmWindow *self)
{
  g_list_store_remove_all(self->store);
  if (!self->archive) return;

  uint32_t n = fm_archive_get_count(self->archive);
  const char *dir = self->current_dir;

  /* collect unique immediate children of current_dir */
  GHashTable *seen_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (uint32_t i = 0; i < n; i++) {
    char *path = fm_archive_get_path(self->archive, i);
    if (!path) continue;

    /* normalize separators */
    for (char *p = path; *p; p++)
      if (*p == '\\') *p = '/';

    size_t dlen = strlen(dir);
    if (strncmp(path, dir, dlen) != 0) { free(path); continue; }

    const char *rest = path + dlen;
    if (*rest == '\0') { free(path); continue; }

    /* find next separator */
    const char *sep = strchr(rest, '/');

    if (sep == NULL) {
      /* direct child — could be a file or an explicit dir entry */
      gboolean is_dir = fm_archive_is_dir(self->archive, i);
      if (is_dir) {
        if (g_hash_table_contains(seen_dirs, rest)) { free(path); continue; }
        g_hash_table_add(seen_dirs, g_strdup(rest));
      }
      FmFileItem *item = fm_file_item_new(
          rest,
          fm_archive_get_size(self->archive, i),
          is_dir,
          fm_archive_get_mtime(self->archive, i), i);
      g_list_store_insert_sorted(self->store, item, item_compare, NULL);
      g_object_unref(item);
    } else {
      /* directory child — deduplicate */
      g_autofree char *dirname = g_strndup(rest, (gsize)(sep - rest));
      if (!g_hash_table_contains(seen_dirs, dirname)) {
        g_hash_table_add(seen_dirs, g_strdup(dirname));
        FmFileItem *item = fm_file_item_new(dirname, 0, TRUE, 0, UINT32_MAX);
        g_list_store_insert_sorted(self->store, item, item_compare, NULL);
        g_object_unref(item);
      }
    }
    free(path);
  }
  g_hash_table_destroy(seen_dirs);

  /* update status */
  uint32_t count = g_list_model_get_n_items(G_LIST_MODEL(self->store));
  g_autofree char *status_text = g_strdup_printf(
      "%s — %u items — %s",
      self->archive_path ? self->archive_path : "",
      count,
      fm_archive_get_format(self->archive));
  gtk_label_set_text(GTK_LABEL(self->status), status_text);
}

static void
navigate_to(FmWindow *self, const char *dir)
{
  g_free(self->current_dir);
  self->current_dir = g_strdup(dir);
  populate_store(self);

  /* update title */
  if (dir[0] == '\0')
    adw_header_bar_set_title_widget(self->header, NULL);
  else {
    g_autofree char *title = g_strdup_printf("/ %s", dir);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
    adw_header_bar_set_title_widget(self->header, label);
  }
}

/* ---- column factories ---- */

static void
setup_label(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f; (void)ud;
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_list_item_set_child(li, label);
}

static void
setup_name(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f; (void)ud;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_list_item_set_child(li, box);
}

static void
bind_name(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f; (void)ud;
  FmFileItem *item = gtk_list_item_get_item(li);
  GtkWidget *box = gtk_list_item_get_child(li);
  GtkWidget *icon = gtk_widget_get_first_child(box);
  GtkWidget *label = gtk_widget_get_next_sibling(icon);
  const char *name = fm_file_item_get_name(item);

  if (fm_file_item_is_dir(item)) {
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), "folder-symbolic");
    g_autofree char *markup = g_markup_printf_escaped("<b>%s</b>", name);
    gtk_label_set_markup(GTK_LABEL(label), markup);
  } else {
    /* guess icon from content type */
    g_autofree char *content_type = g_content_type_guess(name, NULL, 0, NULL);
    g_autoptr(GIcon) gicon = g_content_type_get_symbolic_icon(content_type);
    gtk_image_set_from_gicon(GTK_IMAGE(icon), gicon);
    gtk_label_set_text(GTK_LABEL(label), name);
  }
}

static void
bind_size(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f; (void)ud;
  FmFileItem *item = gtk_list_item_get_item(li);
  GtkWidget *label = gtk_list_item_get_child(li);
  g_autofree char *s = fm_file_item_get_size_str(item);
  gtk_label_set_text(GTK_LABEL(label), s);
  gtk_label_set_xalign(GTK_LABEL(label), 1.0);
}

static void
bind_mtime(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f; (void)ud;
  FmFileItem *item = gtk_list_item_get_item(li);
  GtkWidget *label = gtk_list_item_get_child(li);
  g_autofree char *s = fm_file_item_get_mtime_str(item);
  gtk_label_set_text(GTK_LABEL(label), s);
}

/* ---- nested archive support ---- */

typedef struct {
  FmArchive *archive;
  char      *archive_path;
  char      *current_dir;
  char      *temp_dir;  /* temp dir holding the extracted nested archive */
} FmArchiveFrame;

static void
archive_frame_free(gpointer p)
{
  FmArchiveFrame *f = p;
  if (f->archive) fm_archive_close(f->archive);
  g_free(f->archive_path);
  g_free(f->current_dir);
  if (f->temp_dir) {
    remove_dir_recursive(f->temp_dir);
    g_free(f->temp_dir);
  }
  g_free(f);
}

static gboolean
file_looks_like_archive(const char *name)
{
  static const char *exts[] = {
    ".7z",".zip",".rar",".tar",".gz",".tgz",".bz2",".tbz2",".xz",".txz",
    ".zst",".lzma",".cab",".iso",".wim",".rpm",".deb",".cpio",".arj",
    ".lzh",".lha",".dmg",".tar.gz",".tar.bz2",".tar.xz",".tar.zst",NULL
  };
  for (int i = 0; exts[i]; i++) {
    size_t elen = strlen(exts[i]);
    size_t nlen = strlen(name);
    if (nlen > elen && strcasecmp(name + nlen - elen, exts[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

/* ---- signals ---- */

static void
on_activate(GtkColumnView *cv, guint position, gpointer ud)
{
  FmWindow *self = FM_WINDOW(ud);
  (void)cv;
  FmFileItem *item = g_list_model_get_item(G_LIST_MODEL(self->store), position);
  if (!item) return;

  if (fm_file_item_is_dir(item)) {
    g_autofree char *new_dir = g_strdup_printf(
        "%s%s/", self->current_dir, fm_file_item_get_name(item));
    navigate_to(self, new_dir);
  } else if (self->archive) {
    /* extract to temp */
    uint32_t idx = fm_file_item_get_index(item);
    if (idx != UINT32_MAX) {
      char tmpl[] = "/tmp/unveiler-XXXXXX";
      char *tmpdir = mkdtemp(tmpl);
      if (tmpdir) {
        tmpdir = g_strdup(tmpdir);
        const char *name = fm_file_item_get_name(item);
        int rc = fm_archive_extract(self->archive, &idx, 1, tmpdir, NULL, NULL);
        if (rc > 0) {
          g_autofree char *filepath = g_build_filename(tmpdir, name, NULL);

          /* try opening as nested archive if it looks like one */
          if (file_looks_like_archive(name)) {
            FmArchive *nested = fm_archive_open(self->bridge, filepath);
            if (nested) {
              /* push current state onto stack */
              FmArchiveFrame *frame = g_new0(FmArchiveFrame, 1);
              frame->archive = self->archive;
              frame->archive_path = g_strdup(self->archive_path);
              frame->current_dir = g_strdup(self->current_dir);
              frame->temp_dir = tmpdir;
              tmpdir = NULL;  /* ownership transferred */
              g_ptr_array_add(self->archive_stack, frame);

              self->archive = nested;
              g_free(self->archive_path);
              self->archive_path = g_strdup(filepath);
              g_autofree char *title = g_strdup_printf("%s — %s",
                  g_path_get_basename(self->archive_path),
                  fm_archive_get_format(nested));
              gtk_window_set_title(GTK_WINDOW(self), title);
              navigate_to(self, "");
              g_object_unref(item);
              return;
            }
          }

          /* not an archive — open with default app */
          GFile *file = g_file_new_for_path(filepath);
          GtkFileLauncher *launcher = gtk_file_launcher_new(file);
          gtk_file_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
          g_object_unref(launcher);
          g_object_unref(file);
          gtk_label_set_text(GTK_LABEL(self->status), "Opened file");
        }
        g_free(tmpdir);
      }
    }
  }
  g_object_unref(item);
}

static void
on_open_response(GObject *source, GAsyncResult *res, gpointer ud)
{
  FmWindow *self = FM_WINDOW(ud);
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  g_autoptr(GFile) file = gtk_file_dialog_open_finish(dlg, res, NULL);
  if (file)
    fm_window_open_archive(self, file);
}

static void
action_open(GtkWidget *widget, const char *name, GVariant *param)
{
  (void)name; (void)param;
  FmWindow *self = FM_WINDOW(widget);

  GtkFileFilter *archives = gtk_file_filter_new();
  gtk_file_filter_set_name(archives, "Archives");
  static const char *patterns[] = {
    "*.7z","*.zip","*.rar","*.tar","*.tar.gz","*.tgz","*.tar.bz2","*.tbz2",
    "*.tar.xz","*.txz","*.tar.zst","*.gz","*.bz2","*.xz","*.zst","*.lzma",
    "*.cab","*.iso","*.wim","*.rpm","*.deb","*.cpio","*.arj","*.lzh","*.lha",
    "*.dmg","*.hfs","*.fat","*.ntfs","*.squashfs","*.cramfs",NULL
  };
  for (int i = 0; patterns[i]; i++)
    gtk_file_filter_add_pattern(archives, patterns[i]);

  GtkFileFilter *all = gtk_file_filter_new();
  gtk_file_filter_set_name(all, "All Files");
  gtk_file_filter_add_pattern(all, "*");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, archives);
  g_list_store_append(filters, all);
  g_object_unref(archives);
  g_object_unref(all);

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Open Archive");
  gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
  gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_open_response, self);
  g_object_unref(dlg);
  g_object_unref(filters);
}

static void
action_up(GtkWidget *widget, const char *name, GVariant *param)
{
  (void)name; (void)param;
  FmWindow *self = FM_WINDOW(widget);

  if (!self->current_dir || self->current_dir[0] == '\0') {
    /* at root — pop nested archive stack if possible */
    if (self->archive_stack && self->archive_stack->len > 0) {
      FmArchiveFrame *frame = g_ptr_array_steal_index(
          self->archive_stack, self->archive_stack->len - 1);
      fm_archive_close(self->archive);
      self->archive = frame->archive;
      g_free(self->archive_path);
      self->archive_path = frame->archive_path;
      g_free(self->current_dir);
      self->current_dir = frame->current_dir;
      /* clean up temp dir */
      if (frame->temp_dir) {
        remove_dir_recursive(frame->temp_dir);
        g_free(frame->temp_dir);
      }
      g_free(frame);  /* don't use archive_frame_free — we took ownership */
      g_autofree char *basename = g_path_get_basename(self->archive_path);
      gtk_window_set_title(GTK_WINDOW(self), basename);
      populate_store(self);
    }
    return;
  }

  g_autofree char *dir = g_strdup(self->current_dir);
  /* remove trailing slash */
  size_t len = strlen(dir);
  if (len > 0 && dir[len-1] == '/') dir[len-1] = '\0';
  /* find previous slash */
  char *sep = strrchr(dir, '/');
  if (sep) { sep[1] = '\0'; navigate_to(self, dir); }
  else navigate_to(self, "");
}

/* ---- thread-safe callback wrappers ---- */
static char *password_cb(const char *archive_path, void *user_data);
static bool overwrite_cb(const char *file_path, void *user_data);

/* These marshal password/overwrite dialogs to the main thread when
 * extraction runs in a background thread. */

typedef struct {
  GMutex    mutex;
  GCond     cond;
  gboolean  done;
  /* password */
  const char *archive_path;
  void       *user_data;
  char       *pw_result;
  /* overwrite */
  const char *file_path;
  gboolean    ow_result;
} CallbackReq;

static gboolean
pw_idle_cb(gpointer ud)
{
  CallbackReq *req = ud;
  req->pw_result = password_cb(req->archive_path, req->user_data);
  g_mutex_lock(&req->mutex);
  req->done = TRUE;
  g_cond_signal(&req->cond);
  g_mutex_unlock(&req->mutex);
  return G_SOURCE_REMOVE;
}

static char *
threadsafe_password_cb(const char *archive_path, void *user_data)
{
  if (g_main_context_is_owner(g_main_context_default()))
    return password_cb(archive_path, user_data);

  CallbackReq req = { .archive_path = archive_path, .user_data = user_data, .done = FALSE };
  g_mutex_init(&req.mutex);
  g_cond_init(&req.cond);
  g_idle_add(pw_idle_cb, &req);
  g_mutex_lock(&req.mutex);
  while (!req.done) g_cond_wait(&req.cond, &req.mutex);
  g_mutex_unlock(&req.mutex);
  g_mutex_clear(&req.mutex);
  g_cond_clear(&req.cond);
  return req.pw_result;
}

static gboolean
ow_idle_cb(gpointer ud)
{
  CallbackReq *req = ud;
  req->ow_result = overwrite_cb(req->file_path, req->user_data);
  g_mutex_lock(&req->mutex);
  req->done = TRUE;
  g_cond_signal(&req->cond);
  g_mutex_unlock(&req->mutex);
  return G_SOURCE_REMOVE;
}

static bool
threadsafe_overwrite_cb(const char *file_path, void *user_data)
{
  if (g_main_context_is_owner(g_main_context_default()))
    return overwrite_cb(file_path, user_data);

  CallbackReq req = { .file_path = file_path, .user_data = user_data, .done = FALSE };
  g_mutex_init(&req.mutex);
  g_cond_init(&req.cond);
  g_idle_add(ow_idle_cb, &req);
  g_mutex_lock(&req.mutex);
  while (!req.done) g_cond_wait(&req.cond, &req.mutex);
  g_mutex_unlock(&req.mutex);
  g_mutex_clear(&req.mutex);
  g_cond_clear(&req.cond);
  return req.ow_result;
}

/* ---- threaded extraction with progress ---- */

typedef struct {
  FmArchive      *archive;
  GArray         *indices;
  char           *dest;
  FmExtractResult result;
  volatile uint64_t completed;
  volatile uint64_t total;
  volatile gboolean done;
} ExtractJob;

static void
extract_progress_cb(uint64_t completed, uint64_t total, void *ud)
{
  ExtractJob *job = ud;
  job->completed = completed;
  job->total = total;
}

static gpointer
extract_thread_func(gpointer ud)
{
  ExtractJob *job = ud;
  if (job->indices->len > 0)
    job->result = fm_archive_extract_ex(job->archive,
        (const uint32_t *)job->indices->data, job->indices->len,
        job->dest, extract_progress_cb, job);
  else
    job->result = fm_archive_extract_ex(job->archive,
        NULL, 0, job->dest, extract_progress_cb, job);
  job->done = TRUE;
  return NULL;
}

/* ---- extract folder response ---- */

static void
on_extract_folder_response(GObject *source, GAsyncResult *res, gpointer ud)
{
  FmWindow *self = FM_WINDOW(ud);
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(dlg, res, NULL);
  if (!folder || !self->archive) return;

  g_autofree char *dest = g_file_get_path(folder);

  /* collect selected archive indices */
  GtkBitset *bitset = gtk_selection_model_get_selection(
      GTK_SELECTION_MODEL(self->selection));
  guint64 n_selected = gtk_bitset_get_size(bitset);

  GArray *indices = g_array_new(FALSE, FALSE, sizeof(uint32_t));

  if (n_selected == 0) {
    /* nothing selected — extract everything in current dir view */
    uint32_t n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    for (uint32_t i = 0; i < n; i++) {
      FmFileItem *item = g_list_model_get_item(G_LIST_MODEL(self->store), i);
      uint32_t idx = fm_file_item_get_index(item);
      if (idx != UINT32_MAX)
        g_array_append_val(indices, idx);
      /* for directories, also collect all children from the archive */
      if (fm_file_item_is_dir(item)) {
        const char *dir_name = fm_file_item_get_name(item);
        g_autofree char *prefix = g_strdup_printf("%s%s/",
            self->current_dir, dir_name);
        uint32_t total = fm_archive_get_count(self->archive);
        for (uint32_t j = 0; j < total; j++) {
          char *p = fm_archive_get_path(self->archive, j);
          if (p) {
            for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
            if (g_str_has_prefix(p, prefix))
              g_array_append_val(indices, j);
            free(p);
          }
        }
      }
      g_object_unref(item);
    }
  } else {
    /* extract selected items */
    GtkBitsetIter iter;
    guint pos;
    if (gtk_bitset_iter_init_first(&iter, bitset, &pos)) {
      do {
        FmFileItem *item = g_list_model_get_item(G_LIST_MODEL(self->store), pos);
        if (!item) continue;
        uint32_t idx = fm_file_item_get_index(item);
        if (idx != UINT32_MAX)
          g_array_append_val(indices, idx);
        /* include children of selected directories */
        if (fm_file_item_is_dir(item)) {
          const char *dir_name = fm_file_item_get_name(item);
          g_autofree char *prefix = g_strdup_printf("%s%s/",
              self->current_dir, dir_name);
          uint32_t total = fm_archive_get_count(self->archive);
          for (uint32_t j = 0; j < total; j++) {
            char *p = fm_archive_get_path(self->archive, j);
            if (p) {
              for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
              if (g_str_has_prefix(p, prefix))
                g_array_append_val(indices, j);
              free(p);
            }
          }
        }
        g_object_unref(item);
      } while (gtk_bitset_iter_next(&iter, &pos));
    }
  }
  gtk_bitset_unref(bitset);

  /* check if destination has existing files and ask overwrite policy upfront */
  {
    gboolean dest_has_files = FALSE;
    GDir *ddir = g_dir_open(dest, 0, NULL);
    if (ddir) {
      if (g_dir_read_name(ddir)) dest_has_files = TRUE;
      g_dir_close(ddir);
    }

    if (dest_has_files) {
      GtkWidget *odlg = gtk_dialog_new_with_buttons(
          "Files Exist", GTK_WINDOW(self),
          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
          "_Cancel", GTK_RESPONSE_CANCEL,
          "_Skip Existing", GTK_RESPONSE_NO,
          "_Overwrite", GTK_RESPONSE_YES, NULL);
      GtkWidget *oc = gtk_dialog_get_content_area(GTK_DIALOG(odlg));
      gtk_widget_set_margin_start(oc, 12);
      gtk_widget_set_margin_end(oc, 12);
      gtk_widget_set_margin_top(oc, 8);
      gtk_widget_set_margin_bottom(oc, 8);
      gtk_box_append(GTK_BOX(oc),
          gtk_label_new("Destination folder already contains files."));

      ModalDlgData odata = { FALSE, GTK_RESPONSE_CANCEL };
      g_signal_connect(odlg, "response", G_CALLBACK(on_modal_response), &odata);
      gtk_window_present(GTK_WINDOW(odlg));
      while (!odata.done) g_main_context_iteration(NULL, TRUE);
      gtk_window_destroy(GTK_WINDOW(odlg));

      if (odata.response == GTK_RESPONSE_CANCEL) {
        g_array_free(indices, TRUE);
        return;
      }
      fm_bridge_set_overwrite_policy(self->bridge,
          odata.response == GTK_RESPONSE_YES ? 1 : 2);
    } else {
      fm_bridge_set_overwrite_policy(self->bridge, 1);
    }
  }

  /* show progress dialog and extract in background thread */
  ExtractJob *job = g_new0(ExtractJob, 1);
  job->archive = self->archive;
  job->indices = indices;
  job->dest = g_strdup(dest);
  job->done = FALSE;

  GtkWidget *pdlg = gtk_dialog_new_with_buttons(
      "Extracting…", GTK_WINDOW(self),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      NULL);
  gtk_window_set_default_size(GTK_WINDOW(pdlg), 350, -1);
  gtk_window_set_deletable(GTK_WINDOW(pdlg), FALSE);

  GtkWidget *pcontent = gtk_dialog_get_content_area(GTK_DIALOG(pdlg));
  gtk_box_set_spacing(GTK_BOX(pcontent), 8);
  gtk_widget_set_margin_start(pcontent, 16);
  gtk_widget_set_margin_end(pcontent, 16);
  gtk_widget_set_margin_top(pcontent, 12);
  gtk_widget_set_margin_bottom(pcontent, 12);

  GtkWidget *plabel = gtk_label_new("Extracting files…");
  gtk_box_append(GTK_BOX(pcontent), plabel);

  GtkWidget *pbar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pbar), TRUE);
  gtk_box_append(GTK_BOX(pcontent), pbar);

  gtk_window_present(GTK_WINDOW(pdlg));

  g_thread_unref(g_thread_new("extract", extract_thread_func, job));

  while (!job->done) {
    g_main_context_iteration(NULL, FALSE);
    if (job->total > 0) {
      double frac = (double)job->completed / (double)job->total;
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), frac);
      g_autofree char *ptext = g_strdup_printf("%.0f%%", frac * 100);
      gtk_progress_bar_set_text(GTK_PROGRESS_BAR(pbar), ptext);
    } else {
      gtk_progress_bar_pulse(GTK_PROGRESS_BAR(pbar));
    }
    g_usleep(30000); /* ~30fps update */
  }

  gtk_window_destroy(GTK_WINDOW(pdlg));

  FmExtractResult r = job->result;
  g_array_free(indices, TRUE);
  g_free(job->dest);
  g_free(job);

  g_autofree char *msg = NULL;
  if (!r.ok)
    msg = g_strdup_printf("Extraction failed: %s", r.error_msg);
  else if (r.extracted == 0 && r.skipped == 0)
    msg = g_strdup("No files extracted");
  else if (r.errors > 0 || r.skipped > 0)
    msg = g_strdup_printf("Extracted %d file%s to %s (%d skipped, %d failed)",
        r.extracted, r.extracted == 1 ? "" : "s", dest, r.skipped, r.errors);
  else
    msg = g_strdup_printf("Extracted %d file%s to %s",
        r.extracted, r.extracted == 1 ? "" : "s", dest);
  gtk_label_set_text(GTK_LABEL(self->status), msg);
}

static void
action_extract(GtkWidget *widget, const char *name, GVariant *param)
{
  (void)name; (void)param;
  FmWindow *self = FM_WINDOW(widget);
  if (!self->archive) return;

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Extract to…");
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL,
      on_extract_folder_response, self);
  g_object_unref(dlg);
}

static void
action_select_all(GtkWidget *widget, const char *name, GVariant *param)
{
  (void)name; (void)param;
  FmWindow *self = FM_WINDOW(widget);
  uint32_t n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
  if (n > 0)
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(self->selection));
}

static gboolean
search_filter_func(gpointer item, gpointer ud)
{
  FmWindow *self = FM_WINDOW(ud);
  const char *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
  if (!query || !*query) return TRUE;
  FmFileItem *fi = FM_FILE_ITEM(item);
  const char *name = fm_file_item_get_name(fi);
  /* case-insensitive substring match */
  g_autofree char *name_down = g_utf8_strdown(name, -1);
  g_autofree char *query_down = g_utf8_strdown(query, -1);
  return strstr(name_down, query_down) != NULL;
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer ud)
{
  (void)entry;
  FmWindow *self = FM_WINDOW(ud);
  const char *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));

  if (!query || !*query) {
    /* search cleared — restore normal directory view */
    populate_store(self);
    return;
  }

  /* populate store with ALL matching items from the entire archive */
  g_list_store_remove_all(self->store);
  if (!self->archive) return;

  g_autofree char *query_down = g_utf8_strdown(query, -1);
  uint32_t n = fm_archive_get_count(self->archive);

  for (uint32_t i = 0; i < n; i++) {
    if (fm_archive_is_dir(self->archive, i)) continue;
    char *path = fm_archive_get_path(self->archive, i);
    if (!path) continue;
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';

    /* match against the filename part */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    g_autofree char *name_down = g_utf8_strdown(name, -1);

    if (strstr(name_down, query_down)) {
      /* show full path so user knows where the file is */
      FmFileItem *item = fm_file_item_new(
          path,
          fm_archive_get_size(self->archive, i),
          FALSE,
          fm_archive_get_mtime(self->archive, i), i);
      g_list_store_insert_sorted(self->store, item, item_compare, NULL);
      g_object_unref(item);
    }
    free(path);
  }
}

static void
action_search(GtkWidget *widget, const char *name, GVariant *param)
{
  (void)name; (void)param;
  FmWindow *self = FM_WINDOW(widget);
  gboolean active = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(self->search_bar));
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar), !active);
  if (!active) {
    gtk_widget_grab_focus(self->search_entry);
  } else {
    /* closing search — restore directory view */
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    populate_store(self);
  }
}

/* ---- lifecycle ---- */

static void
fm_window_dispose(GObject *obj)
{
  FmWindow *self = FM_WINDOW(obj);
  if (self->archive) { fm_archive_close(self->archive); self->archive = NULL; }
  if (self->bridge)  { fm_bridge_free(self->bridge);  self->bridge = NULL; }
  g_clear_pointer(&self->archive_path, g_free);
  g_clear_pointer(&self->current_dir, g_free);
  g_clear_pointer(&self->archive_stack, g_ptr_array_unref);
  if (self->drag_cleanup_id) {
    g_source_remove(self->drag_cleanup_id);
    self->drag_cleanup_id = 0;
  }
  if (self->drag_temp_dir) {
    remove_dir_recursive(self->drag_temp_dir);
    g_clear_pointer(&self->drag_temp_dir, g_free);
  }
  G_OBJECT_CLASS(fm_window_parent_class)->dispose(obj);
}

/* ---- column sorters (dirs always first) ---- */

static int
sort_by_name(gconstpointer a, gconstpointer b, gpointer ud)
{
  (void)ud;
  FmFileItem *ia = FM_FILE_ITEM((gpointer)a);
  FmFileItem *ib = FM_FILE_ITEM((gpointer)b);
  gboolean da = fm_file_item_is_dir(ia), db = fm_file_item_is_dir(ib);
  if (da != db) return db - da;
  return g_utf8_collate(fm_file_item_get_name(ia), fm_file_item_get_name(ib));
}

static int
sort_by_size(gconstpointer a, gconstpointer b, gpointer ud)
{
  (void)ud;
  FmFileItem *ia = FM_FILE_ITEM((gpointer)a);
  FmFileItem *ib = FM_FILE_ITEM((gpointer)b);
  gboolean da = fm_file_item_is_dir(ia), db = fm_file_item_is_dir(ib);
  if (da != db) return db - da;
  uint64_t sa = fm_file_item_get_size(ia), sb = fm_file_item_get_size(ib);
  return (sa > sb) - (sa < sb);
}

static int
sort_by_mtime(gconstpointer a, gconstpointer b, gpointer ud)
{
  (void)ud;
  FmFileItem *ia = FM_FILE_ITEM((gpointer)a);
  FmFileItem *ib = FM_FILE_ITEM((gpointer)b);
  gboolean da = fm_file_item_is_dir(ia), db = fm_file_item_is_dir(ib);
  if (da != db) return db - da;
  int64_t ta = fm_file_item_get_mtime(ia), tb = fm_file_item_get_mtime(ib);
  return (ta > tb) - (ta < tb);
}

static GtkColumnViewColumn *
add_column(GtkColumnView *cv, const char *title, int width,
           void (*setup)(GtkListItemFactory*, GtkListItem*, gpointer),
           void (*bind)(GtkListItemFactory*, GtkListItem*, gpointer))
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup), NULL);
  g_signal_connect(factory, "bind",  G_CALLBACK(bind), NULL);
  GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);
  if (width > 0)
    gtk_column_view_column_set_fixed_width(col, width);
  else
    gtk_column_view_column_set_expand(col, TRUE);
  gtk_column_view_append_column(cv, col);
  return col;
}

/* ---- overwrite confirmation dialog ---- */

typedef struct { gboolean done; int response; } OwDlgData;

static void
on_ow_response(GtkDialog *d, int response, gpointer ud)
{
  OwDlgData *data = ud;
  data->response = response;
  data->done = TRUE;
  (void)d;
}

static bool
overwrite_cb(const char *file_path, void *user_data)
{
  FmWindow *self = FM_WINDOW(user_data);

  g_autofree char *basename = g_path_get_basename(file_path);
  g_autofree char *msg = g_strdup_printf("Overwrite \"%s\"?", basename);

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "File Exists", GTK_WINDOW(self),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Skip", GTK_RESPONSE_NO,
      "_Overwrite", GTK_RESPONSE_YES, NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 8);
  gtk_widget_set_margin_bottom(content, 8);
  gtk_box_append(GTK_BOX(content), gtk_label_new(msg));

  OwDlgData data = { FALSE, GTK_RESPONSE_NO };
  g_signal_connect(dialog, "response", G_CALLBACK(on_ow_response), &data);
  gtk_window_present(GTK_WINDOW(dialog));

  while (!data.done)
    g_main_context_iteration(NULL, TRUE);

  gtk_window_destroy(GTK_WINDOW(dialog));
  return data.response == GTK_RESPONSE_YES;
}

/* ---- password dialog (runs synchronously from bridge callback) ---- */

typedef struct { gboolean done; int response; } PwDlgData;

static void
on_pw_response(GtkDialog *d, int response, gpointer ud)
{
  PwDlgData *data = ud;
  data->response = response;
  data->done = TRUE;
  (void)d;
}

static void
on_pw_entry_activate(GtkWidget *entry, gpointer ud)
{
  (void)entry;
  gtk_dialog_response(GTK_DIALOG(ud), GTK_RESPONSE_OK);
}

static char *
password_cb(const char *archive_path, void *user_data)
{
  FmWindow *self = FM_WINDOW(user_data);

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Password Required", GTK_WINDOW(self),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_OK", GTK_RESPONSE_OK, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_set_spacing(GTK_BOX(content), 8);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 8);
  gtk_widget_set_margin_bottom(content, 8);

  g_autofree char *basename = g_path_get_basename(archive_path);
  g_autofree char *msg = g_strdup_printf("Enter password for:\n%s", basename);
  gtk_box_append(GTK_BOX(content), gtk_label_new(msg));

  GtkWidget *entry = gtk_password_entry_new();
  gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
  gtk_box_append(GTK_BOX(content), entry);

  /* Enter key confirms the dialog */
  g_signal_connect(entry, "activate", G_CALLBACK(on_pw_entry_activate), dialog);

  PwDlgData data = { FALSE, GTK_RESPONSE_CANCEL };
  g_signal_connect(dialog, "response", G_CALLBACK(on_pw_response), &data);
  gtk_window_present(GTK_WINDOW(dialog));

  while (!data.done)
    g_main_context_iteration(NULL, TRUE);

  char *result = NULL;
  if (data.response == GTK_RESPONSE_OK)
    result = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

  gtk_window_destroy(GTK_WINDOW(dialog));
  return result;
}

/* ---- drag-out ---- */

static gboolean
drag_cleanup_cb(gpointer ud)
{
  char *dir = ud;
  remove_dir_recursive(dir);
  g_free(dir);
  return G_SOURCE_REMOVE;
}

static GdkContentProvider *
on_drag_prepare(GtkDragSource *src, double x, double y, gpointer ud)
{
  (void)src; (void)x; (void)y;
  FmWindow *self = FM_WINDOW(ud);
  if (!self->archive) return NULL;

  /* collect selected items */
  GtkBitset *bitset = gtk_selection_model_get_selection(
      GTK_SELECTION_MODEL(self->selection));
  guint64 n_sel = gtk_bitset_get_size(bitset);
  if (n_sel == 0) { gtk_bitset_unref(bitset); return NULL; }

  /* create temp dir */
  char tmpl[] = "/tmp/7zfm-XXXXXX";
  char *tmpdir = mkdtemp(tmpl);
  if (!tmpdir) { gtk_bitset_unref(bitset); return NULL; }
  tmpdir = g_strdup(tmpdir);

  /* build index list */
  GArray *indices = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  GtkBitsetIter iter;
  guint pos;
  if (gtk_bitset_iter_init_first(&iter, bitset, &pos)) {
    do {
      FmFileItem *item = g_list_model_get_item(G_LIST_MODEL(self->store), pos);
      if (!item) continue;
      uint32_t idx = fm_file_item_get_index(item);
      if (idx != UINT32_MAX)
        g_array_append_val(indices, idx);
      /* include children of selected directories */
      if (fm_file_item_is_dir(item)) {
        const char *dname = fm_file_item_get_name(item);
        g_autofree char *prefix = g_strdup_printf("%s%s/", self->current_dir, dname);
        uint32_t total = fm_archive_get_count(self->archive);
        for (uint32_t j = 0; j < total; j++) {
          char *p = fm_archive_get_path(self->archive, j);
          if (p) {
            for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
            if (g_str_has_prefix(p, prefix))
              g_array_append_val(indices, j);
            free(p);
          }
        }
      }
      g_object_unref(item);
    } while (gtk_bitset_iter_next(&iter, &pos));
  }
  gtk_bitset_unref(bitset);

  if (indices->len == 0) {
    g_array_free(indices, TRUE);
    g_free(tmpdir);
    return NULL;
  }

  /* extract to temp dir — blocks briefly */
  int rc = fm_archive_extract(self->archive,
      (const uint32_t *)indices->data, indices->len,
      tmpdir, NULL, NULL);
  g_array_free(indices, TRUE);

  if (rc <= 0) {
    g_free(tmpdir);
    return NULL;
  }

  gtk_label_set_text(GTK_LABEL(self->status),
      g_strdup_printf("Dragging %d file%s…", rc, rc == 1 ? "" : "s"));

  /* clean up previous drag temp */
  if (self->drag_cleanup_id) {
    g_source_remove(self->drag_cleanup_id);
    self->drag_cleanup_id = 0;
  }
  if (self->drag_temp_dir) {
    remove_dir_recursive(self->drag_temp_dir);
    g_clear_pointer(&self->drag_temp_dir, g_free);
  }
  self->drag_temp_dir = tmpdir;

  /* enumerate top-level extracted items */
  GSList *file_list = NULL;
  GDir *dir = g_dir_open(tmpdir, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree char *path = g_build_filename(tmpdir, name, NULL);
      file_list = g_slist_prepend(file_list, g_file_new_for_path(path));
    }
    g_dir_close(dir);
  }

  if (!file_list) return NULL;

  GdkContentProvider *provider =
      gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, file_list);
  g_slist_free_full(file_list, g_object_unref);
  return provider;
}

static void
on_drag_end(GtkDragSource *src, GdkDrag *drag, gboolean delete_data, gpointer ud)
{
  (void)src; (void)drag; (void)delete_data;
  FmWindow *self = FM_WINDOW(ud);
  if (self->drag_temp_dir) {
    gtk_label_set_text(GTK_LABEL(self->status), "Drop completed");
    /* delay cleanup so the drop target has time to copy */
    self->drag_cleanup_id = g_timeout_add_seconds(60, drag_cleanup_cb,
        g_strdup(self->drag_temp_dir));
    g_clear_pointer(&self->drag_temp_dir, g_free);
  } else {
    gtk_label_set_text(GTK_LABEL(self->status), "Drop cancelled");
  }
}

static void
fm_window_init(FmWindow *self)
{
  self->current_dir = g_strdup("");
  self->archive_stack = g_ptr_array_new_with_free_func(archive_frame_free);

  /* find 7z.so */
  self->bridge = fm_bridge_new_auto();
  if (!self->bridge)
    g_warning("Could not load 7z.so");
  else {
    fm_bridge_set_password_cb(self->bridge, threadsafe_password_cb, self);
    fm_bridge_set_overwrite_cb(self->bridge, threadsafe_overwrite_cb, self);
  }

  /* actions are installed via class_init */

  /* layout */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

  /* header bar */
  self->header = ADW_HEADER_BAR(adw_header_bar_new());

  GtkWidget *open_btn = gtk_button_new_with_label("Open");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(open_btn), "win.open");
  adw_header_bar_pack_start(self->header, open_btn);

  GtkWidget *up_btn = gtk_button_new_from_icon_name("go-up-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(up_btn), "win.up");
  adw_header_bar_pack_start(self->header, up_btn);

  GtkWidget *extract_btn = gtk_button_new_with_label("Extract");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(extract_btn), "win.extract");
  adw_header_bar_pack_end(self->header, extract_btn);

  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->header));

  /* search bar */
  self->search_bar = gtk_search_bar_new();
  self->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->search_entry, TRUE);
  GtkWidget *search_clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(search_clamp), self->search_entry);
  gtk_search_bar_set_child(GTK_SEARCH_BAR(self->search_bar), search_clamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(self->search_bar),
      GTK_EDITABLE(self->search_entry));
  g_signal_connect(self->search_entry, "search-changed",
      G_CALLBACK(on_search_changed), self);
  gtk_box_append(GTK_BOX(box), self->search_bar);

  /* search button in header */
  GtkWidget *search_btn = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(search_btn), "system-search-symbolic");
  g_object_bind_property(search_btn, "active",
      self->search_bar, "search-mode-enabled",
      G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  adw_header_bar_pack_end(self->header, search_btn);

  /* column view */
  self->store = g_list_store_new(FM_TYPE_FILE_ITEM);

  self->filter = gtk_custom_filter_new(search_filter_func, self, NULL);
  self->filter_model = gtk_filter_list_model_new(
      G_LIST_MODEL(self->store), GTK_FILTER(self->filter));

  GtkSortListModel *sort_model = gtk_sort_list_model_new(
      G_LIST_MODEL(self->filter_model), NULL);
  self->selection = gtk_multi_selection_new(G_LIST_MODEL(sort_model));

  self->column_view = gtk_column_view_new(GTK_SELECTION_MODEL(self->selection));
  gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(self->column_view), TRUE);

  /* connect the column view's sorter to the sort model */
  gtk_sort_list_model_set_sorter(sort_model,
      gtk_column_view_get_sorter(GTK_COLUMN_VIEW(self->column_view)));

  GtkColumnViewColumn *col;
  col = add_column(GTK_COLUMN_VIEW(self->column_view), "Name", -1, setup_name, bind_name);
  gtk_column_view_column_set_sorter(col,
      GTK_SORTER(gtk_custom_sorter_new(sort_by_name, NULL, NULL)));

  col = add_column(GTK_COLUMN_VIEW(self->column_view), "Size", 120, setup_label, bind_size);
  gtk_column_view_column_set_sorter(col,
      GTK_SORTER(gtk_custom_sorter_new(sort_by_size, NULL, NULL)));

  col = add_column(GTK_COLUMN_VIEW(self->column_view), "Modified", 160, setup_label, bind_mtime);
  gtk_column_view_column_set_sorter(col,
      GTK_SORTER(gtk_custom_sorter_new(sort_by_mtime, NULL, NULL)));

  g_signal_connect(self->column_view, "activate", G_CALLBACK(on_activate), self);

  /* drag-out source */
  GtkDragSource *drag_src = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag_src, GDK_ACTION_COPY);
  g_signal_connect(drag_src, "prepare",  G_CALLBACK(on_drag_prepare), self);
  g_signal_connect(drag_src, "drag-end", G_CALLBACK(on_drag_end), self);
  gtk_widget_add_controller(self->column_view, GTK_EVENT_CONTROLLER(drag_src));

  self->scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled), self->column_view);
  gtk_widget_set_vexpand(self->scrolled, TRUE);
  gtk_box_append(GTK_BOX(box), self->scrolled);

  /* status bar */
  self->status = gtk_label_new("No archive opened");
  gtk_label_set_xalign(GTK_LABEL(self->status), 0);
  gtk_widget_set_margin_start(self->status, 8);
  gtk_widget_set_margin_end(self->status, 8);
  gtk_widget_set_margin_top(self->status, 4);
  gtk_widget_set_margin_bottom(self->status, 4);
  gtk_widget_add_css_class(self->status, "dim-label");
  gtk_box_append(GTK_BOX(box), self->status);

  gtk_window_set_default_size(GTK_WINDOW(self), 800, 600);
  gtk_window_set_title(GTK_WINDOW(self), "Unveiler");
}

static void
fm_window_class_init(FmWindowClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = fm_window_dispose;

  gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "win.open", NULL,
      (GtkWidgetActionActivateFunc)action_open);
  gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "win.up", NULL,
      (GtkWidgetActionActivateFunc)action_up);
  gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "win.extract", NULL,
      (GtkWidgetActionActivateFunc)action_extract);
  gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "win.select-all", NULL,
      (GtkWidgetActionActivateFunc)action_select_all);
  gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "win.search", NULL,
      (GtkWidgetActionActivateFunc)action_search);

  /* keyboard shortcuts */
  gtk_widget_class_add_binding_action(GTK_WIDGET_CLASS(klass),
      GDK_KEY_BackSpace, 0, "win.up", NULL);
  gtk_widget_class_add_binding_action(GTK_WIDGET_CLASS(klass),
      GDK_KEY_Up, GDK_ALT_MASK, "win.up", NULL);
  gtk_widget_class_add_binding_action(GTK_WIDGET_CLASS(klass),
      GDK_KEY_a, GDK_CONTROL_MASK, "win.select-all", NULL);
  gtk_widget_class_add_binding_action(GTK_WIDGET_CLASS(klass),
      GDK_KEY_o, GDK_CONTROL_MASK, "win.open", NULL);
  gtk_widget_class_add_binding_action(GTK_WIDGET_CLASS(klass),
      GDK_KEY_f, GDK_CONTROL_MASK, "win.search", NULL);
}

FmWindow *fm_window_new(GtkApplication *app)
{
  return g_object_new(FM_TYPE_WINDOW, "application", app, NULL);
}

void fm_window_open_archive(FmWindow *self, GFile *file)
{
  if (!self->bridge) {
    g_warning("No bridge loaded, cannot open archive");
    return;
  }

  if (self->archive) {
    fm_archive_close(self->archive);
    self->archive = NULL;
  }
  g_clear_pointer(&self->archive_path, g_free);

  g_autofree char *path = g_file_get_path(file);
  g_debug("Opening archive: %s", path);
  self->archive = fm_archive_open(self->bridge, path);
  if (!self->archive) {
    g_autofree char *msg = g_strdup_printf("Failed to open: %s", path);
    gtk_label_set_text(GTK_LABEL(self->status), msg);
    g_warning("%s (bridge=%p)", msg, (void*)self->bridge);
    g_list_store_remove_all(self->store);
    return;
  }

  g_debug("Archive opened: format=%s items=%u",
          fm_archive_get_format(self->archive),
          fm_archive_get_count(self->archive));

  self->archive_path = g_strdup(path);
  g_autofree char *basename = g_file_get_basename(file);
  gtk_window_set_title(GTK_WINDOW(self), basename);

  /* auto-dive: if archive has exactly 1 item with no path (gzip/bz2/xz wrapper),
   * extract it and open as nested archive */
  if (fm_archive_get_count(self->archive) == 1) {
    char *inner_path = fm_archive_get_path(self->archive, 0);
    gboolean is_wrapper = (inner_path == NULL);
    free(inner_path);

    if (is_wrapper) {
      /* infer inner filename by stripping outer extension */
      g_autofree char *inner_name = g_strdup(basename);
      char *dot = strrchr(inner_name, '.');
      if (dot) *dot = '\0';  /* "test.tar.gz" → "test.tar" */

      if (file_looks_like_archive(inner_name)) {
        char tmpl[] = "/tmp/unveiler-XXXXXX";
        char *tmpdir = mkdtemp(tmpl);
        if (tmpdir) {
          tmpdir = g_strdup(tmpdir);
          uint32_t idx = 0;
          int rc = fm_archive_extract(self->archive, &idx, 1, tmpdir, NULL, NULL);
          if (rc > 0) {
            /* the extracted file might have a different name — find it */
            GDir *dir = g_dir_open(tmpdir, 0, NULL);
            const char *extracted_name = dir ? g_dir_read_name(dir) : NULL;
            if (extracted_name) {
              g_autofree char *extracted_path = g_build_filename(tmpdir, extracted_name, NULL);
              FmArchive *nested = fm_archive_open(self->bridge, extracted_path);
              if (nested) {
                FmArchiveFrame *frame = g_new0(FmArchiveFrame, 1);
                frame->archive = self->archive;
                frame->archive_path = g_strdup(self->archive_path);
                frame->current_dir = g_strdup("");
                frame->temp_dir = tmpdir;
                tmpdir = NULL;
                g_ptr_array_add(self->archive_stack, frame);

                self->archive = nested;
                g_free(self->archive_path);
                self->archive_path = g_strdup(extracted_path);
                gtk_window_set_title(GTK_WINDOW(self), basename);
              }
            }
            if (dir) g_dir_close(dir);
          }
          g_free(tmpdir);
        }
      }
    }
  }

  navigate_to(self, "");
}
