/* test-bridge.c — test the fm-bridge C API */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bridge/fm-bridge-api.h"

static void progress(uint64_t completed, uint64_t total, void *ud)
{
    (void)ud;
    if (total > 0)
        printf("\r  extracting: %3u%%", (unsigned)(completed * 100 / total));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <archive> [extract_dir]\n", argv[0]);
        return 1;
    }

    /* auto-discover 7z.so */
    FmBridge *bridge = fm_bridge_new_auto();
    if (!bridge) {
        fprintf(stderr, "error: cannot load 7z.so\n");
        return 1;
    }

    FmArchive *ar = fm_archive_open(bridge, argv[1]);
    if (!ar) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        fm_bridge_free(bridge);
        return 1;
    }

    printf("Format: %s\n", fm_archive_get_format(ar));
    printf("Items:  %u\n\n", fm_archive_get_count(ar));

    uint32_t n = fm_archive_get_count(ar);
    printf("%-12s %-5s %-20s %s\n", "Size", "Dir", "Modified", "Path");
    printf("%-12s %-5s %-20s %s\n", "----", "---", "--------", "----");

    for (uint32_t i = 0; i < n; i++) {
        char *path = fm_archive_get_path(ar, i);
        uint64_t size = fm_archive_get_size(ar, i);
        bool is_dir = fm_archive_is_dir(ar, i);
        int64_t mtime = fm_archive_get_mtime(ar, i);

        char timebuf[32] = "";
        if (mtime > 0) {
            time_t t = (time_t)mtime;
            struct tm *tm = localtime(&t);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
        }

        printf("%-12lu %-5s %-20s %s\n",
               (unsigned long)size,
               is_dir ? "yes" : "no",
               timebuf,
               path ? path : "(null)");
        free(path);
    }

    /* extract if requested */
    if (argc >= 3) {
        printf("\nExtracting to: %s\n", argv[2]);
        int rc = fm_archive_extract(ar, NULL, 0, argv[2], progress, NULL);
        printf("\n%s (files: %d)\n", rc >= 0 ? "OK" : "FAILED", rc);
    }

    fm_archive_close(ar);
    fm_bridge_free(bridge);
    return 0;
}
