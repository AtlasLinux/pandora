// store_manager.c
#define _POSIX_C_SOURCE 200809L
#include "store_manager.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>

static const char *PANDORA_HOME_ENV = "PANDORA_HOME";
static const char *DEFAULT_PANDORA_SUB = ".pandora"; /* fallback if PANDORA_HOME not set */

/* Hook: provide this in your project to call existing unpacker.
   Should extract pkg_path into dest_dir and return 0 on success. */
extern int unpack_pkg_to_dir(const char *pkg_path, const char *dest_dir);

/* Hook: provide this to validate the unpacked tree (path traversal, etc.) */
extern int store_validate_unpacked_tree(const char *unpack_path);

/* Helper to allocate path strings */
static char *make_path(const char *a, const char *b, const char *c, const char *d) {
    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%s/%s/%s/%s", a, b ? b : "", c ? c : "", d ? d : "");
    if (n < 0 || n >= (int)sizeof(buf)) return NULL;
    return strdup(buf);
}

/* Ensure directory exists (mkdir -p semantics) */
static int ensure_dir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp)-1] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Determine pandora root */
static const char *pandora_root_get(char *buf, size_t bufsz) {
    const char *env = getenv(PANDORA_HOME_ENV);
    if (env && env[0]) {
        strncpy(buf, env, bufsz);
        buf[bufsz-1] = '\0';
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(buf, bufsz, "%s/pandora", home);
    return buf;
}

/* Atomically import .pkg into store/<pkg_name>/<pkg_version>
   Returns 0 on success and sets out_store_path (malloc'd) */
int store_import_pkg_atomic(const char *pkg_path,
                            const char *pkg_name,
                            const char *pkg_version,
                            const char *expected_sha256,
                            char **out_store_path)
{
    if (!pkg_path || !pkg_name || !pkg_version) return -1;
    char root[PATH_MAX];
    if (!pandora_root_get(root, sizeof(root))) return -1;

    /* create base paths */
    char *store_base = make_path(root, "store", NULL, NULL); /* root/store/ */
    if (!store_base) return -1;
    if (ensure_dir_p(store_base, 0755) != 0) { free(store_base); return -1; }

    /* temp import dir: store/.tmp-<pid>-<rand> */
    char tmp_template[PATH_MAX];
    snprintf(tmp_template, sizeof(tmp_template), "%s/.tmp-import-XXXXXX", store_base);
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) { free(store_base); return -1; }

    /* construct dest unpack path: <tmp_dir>/<pkg_name>/<pkg_version>/files */
    if (ensure_dir_p(tmp_dir, 0700) != 0) { rmdir(tmp_dir); free(store_base); return -1; }
    char *unpack_dir = make_path(tmp_dir, pkg_name, pkg_version, "files");
    if (!unpack_dir) { /* cleanup */ rmdir(tmp_dir); free(store_base); return -1; }
    if (ensure_dir_p(unpack_dir, 0755) != 0) { free(unpack_dir); rmdir(tmp_dir); free(store_base); return -1; }

    /* Call into your unpacker to extract pkg to unpack_dir */
    if (unpack_pkg_to_dir(pkg_path, unpack_dir) != 0) {
        /* cleanup */
        free(unpack_dir);
        rmdir(tmp_dir);
        free(store_base);
        return -1;
    }

    /* Validate unpacked tree using provided validator */
    if (store_validate_unpacked_tree(unpack_dir) != 0) {
        /* cleanup: naive - caller should remove recursively; keep minimal here */
        free(unpack_dir);
        rmdir(tmp_dir);
        free(store_base);
        return -1;
    }

    /* prepare final path: store/<pkg_name>/<pkg_version> */
    char *final_dir_parent = make_path(store_base, pkg_name, NULL, NULL); /* store/<pkg> */
    if (!final_dir_parent) { free(unpack_dir); rmdir(tmp_dir); free(store_base); return -1; }
    if (ensure_dir_p(final_dir_parent, 0755) != 0) { free(final_dir_parent); free(unpack_dir); rmdir(tmp_dir); free(store_base); return -1; }

    char *final_dir = make_path(final_dir_parent, pkg_version, NULL, NULL); /* store/<pkg>/<ver> */
    if (!final_dir) { free(final_dir_parent); free(unpack_dir); rmdir(tmp_dir); free(store_base); return -1; }

    /* If final_dir already exists, treat as success (idempotent) or error based on policy.
       Here we fail to avoid overwriting existing published versions. */
    struct stat st;
    if (stat(final_dir, &st) == 0) {
        /* already exists */
        free(final_dir);
        free(final_dir_parent);
        free(unpack_dir);
        /* cleanup temp import tree (best-effort) */
        /* TODO: implement recursive rm - here we just rmdir empty layers */
        rmdir(unpack_dir);
        rmdir(tmp_dir);
        free(store_base);
        return -1;
    }

    /* Now atomically move tmp/<pkg>/<ver> to store/<pkg>/<ver> via rename.
       First ensure the tmp node for pkg/version path exists: tmp_dir/<pkg>/<ver> */
    char tmp_pkg_ver_path[PATH_MAX];
    snprintf(tmp_pkg_ver_path, sizeof(tmp_pkg_ver_path), "%s/%s/%s", tmp_dir, pkg_name, pkg_version);

    /* final_dir_parent is store/<pkg>, rename tmp_pkg_ver_path to final_dir */
    if (rename(tmp_pkg_ver_path, final_dir) != 0) {
        /* rename failed */
        free(final_dir);
        free(final_dir_parent);
        free(unpack_dir);
        rmdir(tmp_dir);
        free(store_base);
        return -1;
    }

    /* cleanup: remove tmp_dir if empty */
    rmdir(tmp_dir);

    /* Set out path */
    if (out_store_path) {
        *out_store_path = strdup(final_dir);
    }

    free(final_dir);
    free(final_dir_parent);
    free(unpack_dir);
    free(store_base);
    return 0;
}

/* stub: store_remove_version */
int store_remove_version(const char *pkg_name, const char *pkg_version) {
    (void)pkg_name; (void)pkg_version;
    /* Implement with careful recursive remove and confirmation in CLI */
    return -1;
}
