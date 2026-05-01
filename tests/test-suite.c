/* test-suite.c — automated tests for the Unveiler bridge API */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "bridge/fm-bridge-api.h"

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) \
  static void test_##name(FmBridge *bridge, const char *archdir, const char *tmpdir)

#define RUN(name) do { \
    printf("  %-40s ", #name); \
    fflush(stdout); \
    test_##name(bridge, archdir, tmpdir); \
    printf("PASS\n"); \
    g_pass++; \
  } while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
      printf("FAIL\n    assertion failed: %s\n    at %s:%d\n", \
             #cond, __FILE__, __LINE__); \
      g_fail++; \
      return; \
    } \
  } while(0)

/* ---- helpers ---- */

static void rmrf(const char *path)
{
  DIR *d = opendir(path);
  if (!d) { unlink(path); return; }
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
      rmrf(child);
    else
      unlink(child);
  }
  closedir(d);
  rmdir(path);
}

static int file_exists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static int is_regular_file(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int count_files_recursive(const char *path)
{
  int count = 0;
  DIR *d = opendir(path);
  if (!d) return 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    struct stat st;
    if (lstat(child, &st) == 0) {
      if (S_ISREG(st.st_mode))
        count++;
      else if (S_ISDIR(st.st_mode))
        count += count_files_recursive(child);
    }
  }
  closedir(d);
  return count;
}

static char *make_extract_dir(const char *tmpdir, const char *name)
{
  char *path = malloc(strlen(tmpdir) + strlen(name) + 2);
  sprintf(path, "%s/%s", tmpdir, name);
  rmrf(path);
  mkdir(path, 0755);
  return path;
}

static char *archive_path(const char *archdir, const char *name)
{
  char *path = malloc(strlen(archdir) + strlen(name) + 2);
  sprintf(path, "%s/%s", archdir, name);
  return path;
}

/* ---- tests ---- */

TEST(bridge_loads)
{
  (void)archdir; (void)tmpdir;
  ASSERT(bridge != NULL);
}

TEST(open_normal_zip)
{
  (void)tmpdir;
  char *p = archive_path(archdir, "normal.zip");
  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);
  ASSERT(fm_archive_get_count(ar) > 0);
  ASSERT(strcmp(fm_archive_get_format(ar), "zip") == 0);
  fm_archive_close(ar);
  free(p);
}

TEST(enumerate_items)
{
  (void)tmpdir;
  char *p = archive_path(archdir, "normal.zip");
  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  uint32_t n = fm_archive_get_count(ar);
  int found_hello = 0, found_nested = 0;
  for (uint32_t i = 0; i < n; i++) {
    char *path = fm_archive_get_path(ar, i);
    if (path) {
      if (strcmp(path, "hello.txt") == 0) {
        found_hello = 1;
        ASSERT(fm_archive_get_size(ar, i) == 12);
        ASSERT(!fm_archive_is_dir(ar, i));
      }
      if (strcmp(path, "subdir/nested.txt") == 0)
        found_nested = 1;
      free(path);
    }
  }
  ASSERT(found_hello);
  ASSERT(found_nested);

  fm_archive_close(ar);
  free(p);
}

TEST(extract_normal)
{
  char *p = archive_path(archdir, "normal.zip");
  char *dest = make_extract_dir(tmpdir, "extract_normal");

  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  FmExtractResult r = fm_archive_extract_ex(ar, NULL, 0, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.extracted >= 3);  /* hello.txt, nested.txt, deep/file.txt */
  ASSERT(r.errors == 0);

  /* verify files on disk */
  char buf[4096];
  snprintf(buf, sizeof(buf), "%s/hello.txt", dest);
  ASSERT(is_regular_file(buf));
  snprintf(buf, sizeof(buf), "%s/subdir/nested.txt", dest);
  ASSERT(is_regular_file(buf));
  snprintf(buf, sizeof(buf), "%s/subdir/deep/file.txt", dest);
  ASSERT(is_regular_file(buf));

  fm_archive_close(ar);
  rmrf(dest);
  free(dest);
  free(p);
}

TEST(path_traversal_blocked)
{
  char *p = archive_path(archdir, "traversal.zip");
  char *dest = make_extract_dir(tmpdir, "extract_traversal");

  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  FmExtractResult r = fm_archive_extract_ex(ar, NULL, 0, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.extracted == 1);  /* only safe.txt */
  ASSERT(r.skipped >= 4);    /* the 4 malicious entries */

  /* safe.txt should exist */
  char buf[4096];
  snprintf(buf, sizeof(buf), "%s/safe.txt", dest);
  ASSERT(is_regular_file(buf));

  /* malicious paths must NOT exist anywhere */
  snprintf(buf, sizeof(buf), "%s/../../etc/evil.txt", dest);
  ASSERT(!file_exists(buf));

  /* nothing should have escaped the dest dir */
  ASSERT(count_files_recursive(dest) == 1);

  fm_archive_close(ar);
  rmrf(dest);
  free(dest);
  free(p);
}

TEST(symlink_escape_blocked)
{
  char *dest = make_extract_dir(tmpdir, "extract_symlink");
  char *target = make_extract_dir(tmpdir, "symlink_target");

  /* plant a symlink inside dest that points outside */
  char link_path[4096];
  snprintf(link_path, sizeof(link_path), "%s/evil_link", dest);
  int sr = symlink(target, link_path);
  ASSERT(sr == 0);

  /* create a zip with a file that writes through the symlink */
  char zip_path[4096];
  snprintf(zip_path, sizeof(zip_path), "%s/symlink_attack.zip", tmpdir);
  char cmd[8192];
  snprintf(cmd, sizeof(cmd),
    "python3 -c '"
    "import zipfile; zf=zipfile.ZipFile(\"%s\",\"w\"); "
    "zf.writestr(\"evil_link/pwned.txt\",\"hacked\"); "
    "zf.writestr(\"safe.txt\",\"ok\"); "
    "zf.close()'", zip_path);
  ASSERT(system(cmd) == 0);

  FmArchive *ar = fm_archive_open(bridge, zip_path);
  ASSERT(ar != NULL);

  FmExtractResult r = fm_archive_extract_ex(ar, NULL, 0, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.extracted == 1);  /* only safe.txt */

  /* pwned.txt must NOT appear in the symlink target */
  char check[4096];
  snprintf(check, sizeof(check), "%s/pwned.txt", target);
  ASSERT(!file_exists(check));

  /* safe.txt should be in dest */
  snprintf(check, sizeof(check), "%s/safe.txt", dest);
  ASSERT(is_regular_file(check));

  fm_archive_close(ar);
  unlink(zip_path);
  rmrf(dest);
  rmrf(target);
  free(dest);
  free(target);
}

TEST(overwrite_policy_skip)
{
  char *p = archive_path(archdir, "overwrite.zip");
  char *dest = make_extract_dir(tmpdir, "extract_overwrite");

  /* pre-create the file with different content */
  char filepath[4096];
  snprintf(filepath, sizeof(filepath), "%s/existing.txt", dest);
  FILE *f = fopen(filepath, "w");
  ASSERT(f != NULL);
  fprintf(f, "original\n");
  fclose(f);

  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  /* policy 2 = skip existing */
  fm_bridge_set_overwrite_policy(bridge, 2);
  FmExtractResult r = fm_archive_extract_ex(ar, NULL, 0, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.skipped >= 1);

  /* file should still have original content */
  f = fopen(filepath, "r");
  ASSERT(f != NULL);
  char buf[64];
  ASSERT(fgets(buf, sizeof(buf), f) != NULL);
  fclose(f);
  ASSERT(strcmp(buf, "original\n") == 0);

  /* reset policy */
  fm_bridge_set_overwrite_policy(bridge, 1);

  fm_archive_close(ar);
  rmrf(dest);
  free(dest);
  free(p);
}

TEST(overwrite_policy_replace)
{
  char *p = archive_path(archdir, "overwrite.zip");
  char *dest = make_extract_dir(tmpdir, "extract_overwrite2");

  /* pre-create the file */
  char filepath[4096];
  snprintf(filepath, sizeof(filepath), "%s/existing.txt", dest);
  FILE *f = fopen(filepath, "w");
  ASSERT(f != NULL);
  fprintf(f, "original\n");
  fclose(f);

  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  /* policy 1 = overwrite */
  fm_bridge_set_overwrite_policy(bridge, 1);
  FmExtractResult r = fm_archive_extract_ex(ar, NULL, 0, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.extracted >= 1);

  /* file should have new content */
  f = fopen(filepath, "r");
  ASSERT(f != NULL);
  char buf[64];
  ASSERT(fgets(buf, sizeof(buf), f) != NULL);
  fclose(f);
  ASSERT(strcmp(buf, "new content\n") == 0);

  fm_archive_close(ar);
  rmrf(dest);
  free(dest);
  free(p);
}

TEST(extract_selective_indices)
{
  char *p = archive_path(archdir, "normal.zip");
  char *dest = make_extract_dir(tmpdir, "extract_selective");

  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  /* find index of hello.txt */
  uint32_t n = fm_archive_get_count(ar);
  uint32_t hello_idx = UINT32_MAX;
  for (uint32_t i = 0; i < n; i++) {
    char *path = fm_archive_get_path(ar, i);
    if (path && strcmp(path, "hello.txt") == 0)
      hello_idx = i;
    free(path);
  }
  ASSERT(hello_idx != UINT32_MAX);

  /* extract only hello.txt */
  FmExtractResult r = fm_archive_extract_ex(ar, &hello_idx, 1, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.extracted == 1);

  char buf[4096];
  snprintf(buf, sizeof(buf), "%s/hello.txt", dest);
  ASSERT(is_regular_file(buf));
  ASSERT(count_files_recursive(dest) == 1);

  fm_archive_close(ar);
  rmrf(dest);
  free(dest);
  free(p);
}

TEST(open_nonexistent_file)
{
  (void)tmpdir; (void)archdir;
  FmArchive *ar = fm_archive_open(bridge, "/nonexistent/fake.zip");
  ASSERT(ar == NULL);
}

TEST(open_invalid_file)
{
  (void)archdir;
  /* create a file that isn't an archive — 7z may still "open" it
     as a raw format, so we just verify it doesn't crash */
  char path[4096];
  snprintf(path, sizeof(path), "%s/not_an_archive.txt", tmpdir);
  FILE *f = fopen(path, "w");
  fprintf(f, "this is not an archive\n");
  fclose(f);

  FmArchive *ar = fm_archive_open(bridge, path);
  /* either NULL (rejected) or valid handle (raw format) — both are fine */
  if (ar) {
    /* if opened, count should be sane */
    ASSERT(fm_archive_get_count(ar) <= 1);
    fm_archive_close(ar);
  }
  unlink(path);
}

TEST(null_args_safe)
{
  (void)archdir; (void)tmpdir;
  /* these should not crash */
  ASSERT(fm_archive_open(bridge, NULL) == NULL);
  ASSERT(fm_archive_open(NULL, "/tmp/x.zip") == NULL);
  ASSERT(fm_archive_get_count(NULL) == 0);
  ASSERT(fm_archive_get_path(NULL, 0) == NULL);
  ASSERT(fm_archive_get_size(NULL, 0) == 0);
  ASSERT(fm_archive_is_dir(NULL, 0) == false);
  ASSERT(fm_archive_get_mtime(NULL, 0) == 0);
  ASSERT(fm_archive_get_format(NULL) == NULL);
  ASSERT(fm_archive_extract(NULL, NULL, 0, "/tmp", NULL, NULL) == -1);
  fm_archive_close(NULL);  /* should not crash */
}

TEST(extract_result_fields)
{
  char *p = archive_path(archdir, "traversal.zip");
  char *dest = make_extract_dir(tmpdir, "extract_result");

  FmArchive *ar = fm_archive_open(bridge, p);
  ASSERT(ar != NULL);

  FmExtractResult r = fm_archive_extract_ex(ar, NULL, 0, dest, NULL, NULL);
  ASSERT(r.ok);
  ASSERT(r.extracted + r.skipped > 0);
  ASSERT(r.errors == 0);
  /* the traversal archive has 5 entries: 1 safe + 4 malicious */
  ASSERT(r.extracted == 1);
  ASSERT(r.skipped == 4);

  fm_archive_close(ar);
  rmrf(dest);
  free(dest);
  free(p);
}

/* ---- main ---- */

int main(int argc, char *argv[])
{
  const char *archdir = (argc > 1) ? argv[1] : "/tmp/unveiler-test-archives";
  const char *tmpdir  = (argc > 2) ? argv[2] : "/tmp/unveiler-test-tmp";

  mkdir(tmpdir, 0755);

  printf("Loading bridge...\n");
  FmBridge *bridge = fm_bridge_new_auto();
  if (!bridge) {
    fprintf(stderr, "FATAL: cannot load 7z.so\n");
    return 1;
  }

  printf("Running tests:\n");
  RUN(bridge_loads);
  RUN(open_normal_zip);
  RUN(enumerate_items);
  RUN(extract_normal);
  RUN(path_traversal_blocked);
  RUN(symlink_escape_blocked);
  RUN(overwrite_policy_skip);
  RUN(overwrite_policy_replace);
  RUN(extract_selective_indices);
  RUN(open_nonexistent_file);
  RUN(open_invalid_file);
  RUN(null_args_safe);
  RUN(extract_result_fields);

  printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);

  fm_bridge_free(bridge);
  rmrf(tmpdir);

  return g_fail > 0 ? 1 : 0;
}
