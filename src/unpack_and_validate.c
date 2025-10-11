#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <ftw.h>
#include <stdint.h>
#include <libgen.h>
#include <dirent.h>

/* Archiver exposes: void do_unpack(int argc, char **argv);
   We call it in-process and treat completion as success if files appear. */
extern void do_unpack(int argc, char **argv);

/* Ensure parent directories exist (mkdir -p semantics). Return 0 on success. */
static int ensure_parents_exist(const char *path) {
    if (!path) return -1;
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return 0;
}

/* Unpack wrapper: calls archiver's do_unpack with an argv shaped for extraction.
   Returns 0 on success, nonzero on failure. */
int unpack_pkg_to_dir(const char *pkg_path, const char *dest_dir) {
    if (!pkg_path || !dest_dir) return -1;

    if (ensure_parents_exist(dest_dir) != 0) return -1;
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    /* Build argv for do_unpack. Adjust these args if your archiver expects different flags. */
    char *argv[5];
    argv[0] = "pandora-archiver";
    argv[1] = "--extract";
    argv[2] = (char *)pkg_path;
    argv[3] = (char *)dest_dir;
    argv[4] = NULL;

    /* Call the archiver in-process. If the archiver calls exit(), this will exit the process. */
    do_unpack(4, argv);

    /* Minimal sanity checks: dest_dir exists and contains at least one non-dot entry. */
    struct stat st;
    if (stat(dest_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "unpack_pkg_to_dir: destination directory missing after do_unpack\n");
        return -1;
    }

    DIR *d = opendir(dest_dir);
    if (!d) {
        perror("opendir");
        return -1;
    }
    int nonempty = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        nonempty = 1;
        break;
    }
    closedir(d);
    if (!nonempty) {
        fprintf(stderr, "unpack_pkg_to_dir: destination directory is empty after unpack\n");
        return -1;
    }

    return 0;
}

/* --- store_validate_unpacked_tree --- */
/* Conservative validator:
   - rejects any path component equal to ".."
   - rejects symlinks whose target is absolute or contains ".."
   - rejects overly long paths
   Returns 0 if safe, nonzero otherwise.
*/

static int g_validate_err = 0;
static const size_t MAX_PATH_LEN = PATH_MAX;

static int validate_callback(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)ftwbuf;
    if (g_validate_err) return 0;

    if (strlen(fpath) >= MAX_PATH_LEN) {
        fprintf(stderr, "Validation error: path too long: %s\n", fpath);
        g_validate_err = 1;
        return 0;
    }

    /* reject any path component that equals ".." */
    char tmp[PATH_MAX];
    strncpy(tmp, fpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *p = tmp;
    while (*p) {
        if (*p == '/') p++;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            fprintf(stderr, "Validation error: parent-traversal component in path: %s\n", fpath);
            g_validate_err = 1;
            return 0;
        }
    }

    /* if it's a symlink, check its target is not absolute and not containing .. */
    if (typeflag == FTW_SL) {
        char link_target[PATH_MAX];
        ssize_t r = readlink(fpath, link_target, sizeof(link_target) - 1);
        if (r < 0) {
            perror("readlink");
            g_validate_err = 1;
            return 0;
        }
        link_target[r] = '\0';
        if (link_target[0] == '/') {
            fprintf(stderr, "Validation error: symlink with absolute target: %s -> %s\n", fpath, link_target);
            g_validate_err = 1;
            return 0;
        }
        if (strstr(link_target, "..")) {
            fprintf(stderr, "Validation error: symlink target contains '..': %s -> %s\n", fpath, link_target);
            g_validate_err = 1;
            return 0;
        }
    }

    return 0;
}

int store_validate_unpacked_tree(const char *unpack_path) {
    if (!unpack_path) return -1;
    g_validate_err = 0;
    /* Use FTW_PHYS to avoid following symlinks */
    int flags = FTW_PHYS;
    int rc = nftw(unpack_path, validate_callback, 20, flags);
    if (rc == -1) {
        perror("nftw");
        return -1;
    }
    return g_validate_err ? -1 : 0;
}
