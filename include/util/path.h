#ifndef UTIL_PATH_H
#define UTIL_PATH_H

char *make_path(const char *home, const char *suffix);
error_t ensure_dir(const char *path, mode_t mode);

#endif