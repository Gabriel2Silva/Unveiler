/* fm-file-item.h */
#pragma once
#include <glib-object.h>

G_BEGIN_DECLS

#define FM_TYPE_FILE_ITEM (fm_file_item_get_type())
G_DECLARE_FINAL_TYPE(FmFileItem, fm_file_item, FM, FILE_ITEM, GObject)

FmFileItem *fm_file_item_new       (const char *path, uint64_t size,
                                    gboolean is_dir, int64_t mtime,
                                    uint32_t index);
const char *fm_file_item_get_name  (FmFileItem *self);
const char *fm_file_item_get_path  (FmFileItem *self);
uint64_t    fm_file_item_get_size  (FmFileItem *self);
gboolean    fm_file_item_is_dir    (FmFileItem *self);
int64_t     fm_file_item_get_mtime (FmFileItem *self);
uint32_t    fm_file_item_get_index (FmFileItem *self);
char       *fm_file_item_get_size_str  (FmFileItem *self);
char       *fm_file_item_get_mtime_str (FmFileItem *self);

G_END_DECLS
