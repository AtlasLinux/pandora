#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 500
#include "profile_manager.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <ftw.h>
#include <time.h>
#include <sys/types.h>

static const char *PANDORA_ENV = "PANDORA_HOME";
static const char *DEFAULT_PANDORA = ".pandora";

const char *profile_get_pandora_root(char *buf, size_t buflen) {
    const char *env = getenv(PANDORA_ENV);
    if (env && env[0]) {
        strncpy(buf, env, buflen);
        buf[buflen - 1] = '\0';
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home) return NULL;
    if ((size_t)snprintf(buf, buflen, "%s/pandora", home) >= buflen) return NULL;
    return buf;
}

static int ensure_dir(const char *path) {
    if (!path) return -1;
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

/* mkdir -p with heap temp copy */
static int make_parents_dir(const char *path) {
    if (!path || path[0] == '\0') return -1;
    char *tmp = strdup(path);
    if (!tmp) return -1;
    size_t len = strlen(tmp);
    if (len == 0) { free(tmp); return -1; }
    /* strip trailing slashes except root of the given path */
    while (len > 1 && tmp[len-1] == '/') { tmp[len-1] = '\0'; len--; }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

/* remove tree using nftw, return 0 on success, -1 on failure */
static int rm_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    int rc = 0;
    if (typeflag == FTW_DP || typeflag == FTW_D) rc = rmdir(fpath);
    else rc = unlink(fpath);
    if (rc != 0) return -1;
    return 0;
}
static int remove_tree_and_dir(const char *path) {
    if (!path) return -1;
    /* ignore return for nftw children removal but propagate any error */
    if (nftw(path, rm_cb, 64, FTW_DEPTH | FTW_PHYS) != 0) {
        /* attempt best-effort continued cleanup */
    }
    /* finally remove the directory itself if it still exists */
    if (rmdir(path) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    return 0;
}

/* normalize_relpath: strictly rejects absolute, empty, ., .., or empty components.
 * returns 0 on success and *out is a malloc'ed normalized string (caller must free).
 */
static int normalize_relpath(char **out, const char *src) {
    if (!src || src[0] == '\0') return -1;
    if (src[0] == '/') return -1;
    char *tmp = strdup(src);
    if (!tmp) return -1;

    char buf[PATH_MAX];
    size_t off = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(tmp, "/", &saveptr);
    int first = 1;
    while (tok) {
        if (tok[0] == '\0' || strcmp(tok, ".") == 0 || strcmp(tok, "..") == 0) { free(tmp); return -1; }
        size_t need = off + (first ? 0 : 1) + strlen(tok) + 1;
        if (need > sizeof(buf)) { free(tmp); return -1; }
        if (!first) buf[off++] = '/';
        strcpy(buf + off, tok);
        off += strlen(tok);
        first = 0;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    free(tmp);
    if (off == 0) return -1;
    buf[off] = '\0';
    *out = strdup(buf);
    return *out ? 0 : -1;
}

int profile_assemble_tmp(const ProfileEntry *entries, size_t n_entries, char **tmp_profile_path_out) {
    if (!entries || n_entries == 0 || !tmp_profile_path_out) return PROFILE_INVALID_INPUT;
    *tmp_profile_path_out = NULL;

    char root[PATH_MAX];
    if (!profile_get_pandora_root(root, sizeof(root))) return PROFILE_INTERNAL_ERROR;

    /* ensure profiles/ exists */
    char profiles_dir[PATH_MAX];
    if ((size_t)snprintf(profiles_dir, sizeof(profiles_dir), "%s/profiles", root) >= sizeof(profiles_dir)) return PROFILE_INTERNAL_ERROR;
    if (ensure_dir(profiles_dir) != 0 && errno != EEXIST) return PROFILE_INTERNAL_ERROR;

    /* create unique temp dir under profiles */
    char tmp_template[PATH_MAX];
    if ((size_t)snprintf(tmp_template, sizeof(tmp_template), "%s/.tmp-profile-XXXXXX", profiles_dir) >= sizeof(tmp_template)) return PROFILE_INTERNAL_ERROR;

    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) return PROFILE_INTERNAL_ERROR;

    /* process entries */
    for (size_t i = 0; i < n_entries; ++i) {
        const ProfileEntry *e = &entries[i];
        if (!e->relpath || !e->target_path) {
            remove_tree_and_dir(tmp_dir);
            return PROFILE_MISSING_TARGET;
        }

        char *nrel = NULL;
        if (normalize_relpath(&nrel, e->relpath) != 0) {
            fprintf(stderr, "Invalid relpath: %s\n", e->relpath);
            remove_tree_and_dir(tmp_dir);
            return PROFILE_INVALID_INPUT;
        }

        /* check target exists */
        struct stat st;
        if (stat(e->target_path, &st) != 0) {
            fprintf(stderr, "Missing target path: %s (errno=%d)\n", e->target_path, errno);
            free(nrel);
            remove_tree_and_dir(tmp_dir);
            return PROFILE_MISSING_TARGET;
        }

        /* conflict detection */
        for (size_t j = 0; j < i; ++j) {
            char *prevnorm = NULL;
            if (normalize_relpath(&prevnorm, entries[j].relpath) == 0) {
                if (strcmp(prevnorm, nrel) == 0) {
                    fprintf(stderr, "Conflict on path %s between %s@%s and %s@%s\n",
                            nrel,
                            entries[j].pkg_name ? entries[j].pkg_name : "(unknown)", entries[j].pkg_version ? entries[j].pkg_version : "(unknown)",
                            e->pkg_name ? e->pkg_name : "(unknown)", e->pkg_version ? e->pkg_version : "(unknown)");
                    free(prevnorm);
                    free(nrel);
                    remove_tree_and_dir(tmp_dir);
                    return PROFILE_CONFLICT;
                }
                free(prevnorm);
            }
        }

        /* build link target path (heap allocate exact size) */
        size_t need = strlen(tmp_dir) + 1 + strlen(nrel) + 1;
        char *link_target = malloc(need);
        if (!link_target) { free(nrel); remove_tree_and_dir(tmp_dir); return PROFILE_INTERNAL_ERROR; }
        snprintf(link_target, need, "%s/%s", tmp_dir, nrel);

        /* create parent dirs */
        char *parent = strdup(link_target);
        if (!parent) { free(nrel); free(link_target); remove_tree_and_dir(tmp_dir); return PROFILE_INTERNAL_ERROR; }
        char *last = strrchr(parent, '/');
        if (!last) { free(parent); free(nrel); free(link_target); remove_tree_and_dir(tmp_dir); return PROFILE_INTERNAL_ERROR; }
        *last = '\0';
        if (make_parents_dir(parent) != 0) { fprintf(stderr, "Failed to create parents for %s\n", parent); free(parent); free(nrel); free(link_target); remove_tree_and_dir(tmp_dir); return PROFILE_INTERNAL_ERROR; }
        free(parent);

        /* remove existing node only if it's a symlink or file; error on dir */
        struct stat lst;
        if (lstat(link_target, &lst) == 0) {
            if (S_ISDIR(lst.st_mode)) {
                fprintf(stderr, "Conflict: existing directory at %s\n", link_target);
                free(nrel); free(link_target); remove_tree_and_dir(tmp_dir); return PROFILE_CONFLICT;
            } else {
                if (unlink(link_target) != 0) {
                    fprintf(stderr, "Failed to unlink existing %s: %s\n", link_target, strerror(errno));
                    free(nrel); free(link_target); remove_tree_and_dir(tmp_dir); return PROFILE_INTERNAL_ERROR;
                }
            }
        }

        if (symlink(e->target_path, link_target) != 0) {
            fprintf(stderr, "Failed to create symlink %s -> %s: %s\n", link_target, e->target_path, strerror(errno));
            free(nrel); free(link_target); remove_tree_and_dir(tmp_dir); return PROFILE_INTERNAL_ERROR;
        }

        free(nrel);
        free(link_target);
    }

    /* success: return strdup of tmp_dir (caller owns and may free if not activating) */
    *tmp_profile_path_out = strdup(tmp_dir);
    if (!*tmp_profile_path_out) {
        remove_tree_and_dir(tmp_dir);
        return PROFILE_INTERNAL_ERROR;
    }

    return PROFILE_OK;
}

int profile_atomic_activate(const char *tmp_profile_path, const char *profile_name) {
    if (!tmp_profile_path || !profile_name) return -1;
    char root[PATH_MAX];
    if (!profile_get_pandora_root(root, sizeof(root))) return -1;

    char profiles_dir[PATH_MAX];
    if ((size_t)snprintf(profiles_dir, sizeof(profiles_dir), "%s/profiles", root) >= sizeof(profiles_dir)) return -1;
    if (ensure_dir(profiles_dir) != 0 && errno != EEXIST) return -1;

    /* Compose unique final profile name */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char final_profile[PATH_MAX];
    if ((size_t)snprintf(final_profile, sizeof(final_profile), "%s/%s-%ld-%ld", profiles_dir, profile_name, (long)getpid(), (long)ts.tv_nsec) >= sizeof(final_profile)) return -1;

    if (rename(tmp_profile_path, final_profile) != 0) {
        return -1;
    }

    /* create vir-new symlink pointing to final_profile */
    char vir_new[PATH_MAX];
    if ((size_t)snprintf(vir_new, sizeof(vir_new), "%s/vir-new", root) >= sizeof(vir_new)) return -1;
    unlink(vir_new);
    if (symlink(final_profile, vir_new) != 0) {
        /* best-effort: try to move profile back (ignore result) */
        return -1;
    }

    /* atomically swap vir-new -> vir */
    char vir_path[PATH_MAX];
    if ((size_t)snprintf(vir_path, sizeof(vir_path), "%s/vir", root) >= sizeof(vir_path)) return -1;
    if (rename(vir_new, vir_path) != 0) {
        return -1;
    }

    /* write txn log */
    char txn_dir[PATH_MAX];
    if ((size_t)snprintf(txn_dir, sizeof(txn_dir), "%s/tmp", root) >= sizeof(txn_dir)) return 0;
    ensure_dir(txn_dir);
    char txn_path[PATH_MAX];
    if ((size_t)snprintf(txn_path, sizeof(txn_path), "%s/txn-%ld-%ld.log", txn_dir, (long)getpid(), (long)ts.tv_nsec) < sizeof(txn_path)) {
        FILE *f = fopen(txn_path, "w");
        if (f) {
            fprintf(f, "activated=%s\n", final_profile);
            fclose(f);
        }
    }
    return 0;
}
