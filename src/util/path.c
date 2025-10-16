#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

#include "util/err.h"

char *make_path(const char *home, const char *suffix) {
    size_t need = strlen(home) + strlen(suffix) + 1;
    char *buf = malloc(need);
    if (!buf) return NULL;
    /* home already contains trailing path root (no slash added) and suffix starts with '/' */
    snprintf(buf, need, "%s%s", home, suffix);
    return buf;
}

error_t ensure_dir(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return ERR_OK;
    }

    if (mkdir(path, mode) == 0) {
        return ERR_OK;
    }

    if (errno == ENOENT) {

        char tmp[256];
        strncpy(tmp, path, sizeof(tmp)-1);
        tmp[sizeof(tmp)-1] = '\0';

        for (char *p = tmp + 1; *p; ++p) {
            if (*p == '/') {
                *p = '\0';
                if (mkdir(tmp, 0755) == 0) {
                } else if (errno != EEXIST) {
                    return ERR_FAILED;
                }
                *p = '/';
            }
        }

        if (mkdir(path, mode) == 0) {
            return ERR_OK;
        }
    }

    return ERR_FAILED;
}