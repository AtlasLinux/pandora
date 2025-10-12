#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <stddef.h>

/* Error codes returned by profile_assemble_tmp */
#define PROFILE_OK 0
#define PROFILE_MISSING_TARGET 1
#define PROFILE_CONFLICT 2
#define PROFILE_INVALID_INPUT 3
#define PROFILE_INTERNAL_ERROR 4

typedef struct {
    const char *relpath;      /* relative path inside profile (caller-owned) */
    const char *target_path;  /* path in store to point at (caller-owned) */
    const char *pkg_name;     /* optional, caller-owned (for diagnostics) */
    const char *pkg_version;  /* optional, caller-owned (for diagnostics) */
} ProfileEntry;

/* Assemble a temporary profile directory. entries and the strings inside are
 * owned by the caller and must remain valid for the call duration. The
 * function makes its own copies as required and never frees caller memory.
 *
 * On success: returns PROFILE_OK and *out_tmp_profile_dir is a malloc'ed
 * string containing the path to the created temp profile dir. Caller owns
 * this string. Caller must free() it if not passed to profile_atomic_activate.
 *
 * On error: returns a PROFILE_* error code and *out_tmp_profile_dir is NULL.
 */
int profile_assemble_tmp(const ProfileEntry *entries, size_t n_entries, char **out_tmp_profile_dir);

/* Atomically activate a tmp profile produced by profile_assemble_tmp.
 *
 * tmp_profile_dir must be a path returned by profile_assemble_tmp.
 * On success: returns 0. The directory referred to by tmp_profile_dir has
 * been renamed into the profiles directory and vir now points to the new
 * profile. After success the caller must NOT free tmp_profile_dir (its path
 * has been moved on disk; the string value is still heap memory but the
 * namespace ownership has changed).
 *
 * On error: returns -1 and the tmp_profile_dir has not been consumed; caller
 * retains ownership and should free it when appropriate.
 */
int profile_atomic_activate(const char *tmp_profile_dir, const char *profile_name);

/* Convenience: remove a profile (not implemented here in detail) */
int profile_remove(const char *profile_name);

/* Helper to get pandora root into buf (buflen must be PATH_MAX). Returns buf or NULL */
const char *profile_get_pandora_root(char *buf, size_t buflen);

#endif /* PROFILE_MANAGER_H */
