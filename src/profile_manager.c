// profile_manager.c
#define _POSIX_C_SOURCE 200809L
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
#include <libgen.h>

/* Helpers to construct paths under PANDORA root */
static const char *PANDORA_ENV = "PANDORA_HOME";
static const char *DEFAULT_PANDORA = ".pandora";

static const char *get_pandora_root(char *buf, size_t sz) {
    const char *env = getenv(PANDORA_ENV);
    if (env && env[0]) {
        strncpy(buf, env, sz);
        buf[sz-1] = '\0';
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(buf, sz, "%s/pandora", home);
    return buf;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

/* Create parent directories for a given path */
static int ensure_parents(const char *path) {
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp)-1] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return 0;
}

/* Create a symlink, replacing existing file only if it is a symlink pointing to something else */
static int safe_symlink(const char *target, const char *linkpath) {
    /* remove if exists and is symlink or file */
    struct stat st;
    if (lstat(linkpath, &st) == 0) {
        if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) {
            unlink(linkpath);
        } else if (S_ISDIR(st.st_mode)) {
            /* conflict with directory; fail */
            return -1;
        }
    }
    /* ensure parent exists */
    if (ensure_parents(linkpath) != 0) return -1;
    if (symlink(target, linkpath) != 0) return -1;
    return 0;
}

int profile_assemble_tmp(const ProfileEntry *entries, size_t n_entries, char **tmp_profile_path_out) {
    if (!entries || n_entries == 0 || !tmp_profile_path_out) return PROFILE_MISSING_TARGET;

    char root[PATH_MAX];
    if (!get_pandora_root(root, sizeof(root))) return PROFILE_MISSING_TARGET;

    /* create tmp profile dir */
    char tmp_template[PATH_MAX];
    snprintf(tmp_template, sizeof(tmp_template), "%s/profiles/.tmp-profile-XXXXXX", root);
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) return PROFILE_MISSING_TARGET;

    /* conflict map: relpath -> index of first owner */
    /* naive implementation: linear search for conflicts */
    for (size_t i = 0; i < n_entries; ++i) {
        const ProfileEntry *e = &entries[i];
        if (!e->relpath || !e->target_path) {
            /* missing target */
            /* cleanup */
            rmdir(tmp_dir);
            return PROFILE_MISSING_TARGET;
        }
        /* check if target exists */
        struct stat st;
        if (stat(e->target_path, &st) != 0) {
            /* missing target */
            rmdir(tmp_dir);
            return PROFILE_MISSING_TARGET;
        }
        /* conflict detection */
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(entries[j].relpath, e->relpath) == 0) {
                /* conflict found: print diagnostics to stderr */
                fprintf(stderr, "Conflict on path %s between %s@%s and %s@%s\n",
                        e->relpath,
                        entries[j].pkg_name, entries[j].pkg_version,
                        e->pkg_name, e->pkg_version);
                /* cleanup */
                rmdir(tmp_dir);
                return PROFILE_CONFLICT;
            }
        }
        /* construct link target path under tmp_dir: tmp_dir/<relpath> -> target_path */
        char link_target[PATH_MAX];
        snprintf(link_target, sizeof(link_target), "%s/%s", tmp_dir, e->relpath);
        /* ensure parent dirs exist for link path */
        char parent[PATH_MAX];
        strncpy(parent, link_target, sizeof(parent));
        char *p = dirname(parent);
        if (ensure_parents(p) != 0) {
            rmdir(tmp_dir);
            return PROFILE_MISSING_TARGET;
        }
        if (safe_symlink(e->target_path, link_target) != 0) {
            rmdir(tmp_dir);
            return PROFILE_MISSING_TARGET;
        }
    }

    *tmp_profile_path_out = strdup(tmp_dir);
    return PROFILE_OK;
}

/* Atomic activate: rename tmp_profile into profiles/<profile>-new-<pid> and swap vir */
int profile_atomic_activate(const char *tmp_profile_path, const char *profile_name) {
    if (!tmp_profile_path || !profile_name) return -1;
    char root[PATH_MAX];
    if (!get_pandora_root(root, sizeof(root))) return -1;

    /* ensure profiles dir exists */
    char profiles_dir[PATH_MAX];
    snprintf(profiles_dir, sizeof(profiles_dir), "%s/profiles", root);
    ensure_dir(profiles_dir);

    /* Compose new profile path */
    char new_profile[PATH_MAX];
    snprintf(new_profile, sizeof(new_profile), "%s/%s-new-%d", profiles_dir, profile_name, (int)getpid());

    if (rename(tmp_profile_path, new_profile) != 0) {
        return -1;
    }

    /* create vir-new symlink pointing to new_profile */
    char vir_new[PATH_MAX];
    snprintf(vir_new, sizeof(vir_new), "%s/vir-new", root);
    /* remove existing vir-new if any */
    unlink(vir_new);
    if (symlink(new_profile, vir_new) != 0) {
        /* try to restore by renaming profile back? leave for caller to handle */
        return -1;
    }

    /* atomically rename vir-new to vir (overwrite) */
    char vir_path[PATH_MAX];
    snprintf(vir_path, sizeof(vir_path), "%s/vir", root);
    if (rename(vir_new, vir_path) != 0) {
        /* restore state not attempted here */
        return -1;
    }

    /* Optionally write a txn file for recovery */
    char txn_path[PATH_MAX];
    snprintf(txn_path, sizeof(txn_path), "%s/tmp/txn-%d.log", root, (int)getpid());
    FILE *txn = fopen(txn_path, "w");
    if (txn) {
        fprintf(txn, "activated=%s\n", new_profile);
        fclose(txn);
    }

    return 0;
}
