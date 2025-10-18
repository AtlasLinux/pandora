#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "fs/fs.h"
#include "util/err.h"
#include "util/path.h"

error_t fs_init(void) {
    struct stat st;
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME environment variable not set\n");
        exit(1);
    }

    const char *target_dirs[] = {
        "/pandora/store",
        "/pandora/vir/bin",
        "/pandora/vir/lib",
        "/pandora/profiles/default",
        "/pandora/manifests",
        "/pandora/cache",
        "/pandora/tmp",
        NULL
    };

    for (int i = 0; target_dirs[i] != NULL; ++i) {
        char *target_dir = make_path(home, target_dirs[i]);
        if (!target_dir) {
            perror("malloc");
            exit(1);
        }

        if (ensure_dir(target_dir, 0755) == ERR_FAILED) {
            fprintf(stderr, "Failed to create dir %s\n", target_dir);
            exit(1);
        }
        free(target_dir);
    }

    const char *target_links[] = {
        "/pandora/vir/bin", "/bin",
        "/pandora/vir/lib", "/lib",
        NULL
    };

    for (int i = 0; target_links[i] != NULL; i += 2) {
        const char *from_tpl = target_links[i];     /* target the symlink will point to */
        const char *to_tpl   = target_links[i+1];   /* path of the symlink itself */

        char *full_from = make_path(home, from_tpl);
        char *full_to   = make_path(home, to_tpl);
        if (!full_from || !full_to) {
            perror("malloc");
            free(full_from); free(full_to);
            exit(1);
        }

        printf("Linking %s to %s\n", full_from, full_to);
        if (symlink(full_from, full_to) == -1) {
            if (errno == EEXIST) {
                /* optionally handle existing link/file: skip or replace */
                fprintf(stderr, "Link already exists: %s\n", full_to);
            } else {
                fprintf(stderr, "symlink(%s, %s) failed: %s\n", full_from, full_to, strerror(errno));
                free(full_from); free(full_to);
                exit(1);
            }
        }

        free(full_from);
        free(full_to);
    }

    return ERR_OK;
}
