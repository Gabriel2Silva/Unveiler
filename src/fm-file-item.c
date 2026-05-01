/* fm-file-item.c */
#include "fm-file-item.h"
#include <string.h>
#include <time.h>

struct _FmFileItem {
  GObject   parent;
  char     *path;
  char     *name;     /* points into path or after last separator */
  uint64_t  size;
  gboolean  is_dir;
  int64_t   mtime;
  uint32_t  index;    /* index in the archive's flat item list */
};

G_DEFINE_TYPE(FmFileItem, fm_file_item, G_TYPE_OBJECT)

static void fm_file_item_finalize(GObject *obj)
{
  FmFileItem *self = FM_FILE_ITEM(obj);
  g_free(self->path);
  G_OBJECT_CLASS(fm_file_item_parent_class)->finalize(obj);
}

static void fm_file_item_class_init(FmFileItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = fm_file_item_finalize;
}

static void fm_file_item_init(FmFileItem *self)
{
  (void)self;
}

FmFileItem *fm_file_item_new(const char *path, uint64_t size,
                             gboolean is_dir, int64_t mtime, uint32_t index)
{
  FmFileItem *self = g_object_new(FM_TYPE_FILE_ITEM, NULL);
  self->path  = g_strdup(path ? path : "");
  self->size  = size;
  self->is_dir = is_dir;
  self->mtime = mtime;
  self->index = index;

  /* extract display name from path */
  const char *sep = strrchr(self->path, '/');
  if (!sep) sep = strrchr(self->path, '\\');
  self->name = (char *)(sep ? sep + 1 : self->path);

  return self;
}

const char *fm_file_item_get_name(FmFileItem *self)  { return self->name; }
const char *fm_file_item_get_path(FmFileItem *self)  { return self->path; }
uint64_t    fm_file_item_get_size(FmFileItem *self)  { return self->size; }
gboolean    fm_file_item_is_dir(FmFileItem *self)    { return self->is_dir; }
int64_t     fm_file_item_get_mtime(FmFileItem *self) { return self->mtime; }
uint32_t    fm_file_item_get_index(FmFileItem *self) { return self->index; }

char *fm_file_item_get_size_str(FmFileItem *self)
{
  if (self->is_dir) return g_strdup("");
  return g_format_size(self->size);
}

char *fm_file_item_get_mtime_str(FmFileItem *self)
{
  if (self->mtime <= 0) return g_strdup("");
  time_t t = (time_t)self->mtime;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return g_strdup(buf);
}
