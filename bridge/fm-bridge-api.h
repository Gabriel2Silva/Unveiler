/* fm-bridge-api.h — C API for 7-Zip archive browsing via 7z.so */
#ifndef FM_BRIDGE_API_H
#define FM_BRIDGE_API_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FmBridge   FmBridge;
typedef struct FmArchive  FmArchive;

/* Library lifecycle — call once at startup/shutdown */
FmBridge *fm_bridge_new    (const char *lib_path);  /* path to 7z.so */
void      fm_bridge_free   (FmBridge *bridge);

/* Auto-discover 7z.so using standard search order:
 *   1. $UNVEILER_PLUGIN_DIR/7z.so  (env override)
 *   2. <directory of executable>/7z.so
 *   3. /usr/local/lib/unveiler/7z.so
 *   4. /usr/lib/unveiler/7z.so
 *   5. 7z.so  (current directory / LD_LIBRARY_PATH)
 * Returns NULL if not found. */
FmBridge *fm_bridge_new_auto(void);

/* Archive lifecycle */
FmArchive *fm_archive_open  (FmBridge *bridge, const char *path);
void       fm_archive_close (FmArchive *archive);

/* Password callback — return newly allocated string, or NULL to cancel.
 * Set on the bridge before calling fm_archive_open. */
typedef char *(*FmPasswordCb)(const char *archive_path, void *user_data);
void fm_bridge_set_password_cb(FmBridge *bridge, FmPasswordCb cb, void *user_data);

/* Overwrite callback — return true to overwrite, false to skip.
 * Set on the bridge before calling fm_archive_extract. */
typedef bool (*FmOverwriteCb)(const char *file_path, void *user_data);
void fm_bridge_set_overwrite_cb(FmBridge *bridge, FmOverwriteCb cb, void *user_data);

/* Overwrite policy for extraction: 1=overwrite all, 2=skip all */
void fm_bridge_set_overwrite_policy(FmBridge *bridge, int policy);

/* Item enumeration (flat list from IInArchive) */
uint32_t   fm_archive_get_count (FmArchive *archive);
char      *fm_archive_get_path  (FmArchive *archive, uint32_t index); /* caller frees */
uint64_t   fm_archive_get_size  (FmArchive *archive, uint32_t index);
bool       fm_archive_is_dir    (FmArchive *archive, uint32_t index);
int64_t    fm_archive_get_mtime (FmArchive *archive, uint32_t index); /* unix timestamp */

/* Archive format info */
const char *fm_archive_get_format (FmArchive *archive);

/* Extraction — returns number of files extracted, or -1 on error */
typedef void (*FmProgressCb)(uint64_t completed, uint64_t total, void *user_data);

/* Detailed extraction result */
typedef struct {
  int      extracted;    /* number of files successfully extracted */
  int      skipped;      /* entries skipped (overwrite policy, unsafe paths) */
  int      errors;       /* entries that failed to extract (I/O errors etc.) */
  bool     ok;           /* true if the 7z Extract() call itself succeeded */
  char     error_msg[256]; /* human-readable summary, empty if ok && errors==0 */
} FmExtractResult;

/* Extract with detailed result */
FmExtractResult fm_archive_extract_ex(FmArchive       *archive,
                                      const uint32_t  *indices,
                                      uint32_t         count,
                                      const char      *dest_path,
                                      FmProgressCb     cb,
                                      void            *user_data);

/* Simple extract — backward compatible, returns extracted count or -1 */
int fm_archive_extract (FmArchive       *archive,
                        const uint32_t  *indices,    /* NULL = all */
                        uint32_t         count,
                        const char      *dest_path,
                        FmProgressCb     cb,
                        void            *user_data);

#ifdef __cplusplus
}
#endif

#endif /* FM_BRIDGE_API_H */
