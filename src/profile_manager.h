#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <stddef.h>

/* Profile assembly result codes */
#define PROFILE_OK 0
#define PROFILE_CONFLICT 1
#define PROFILE_MISSING_TARGET 2

/* Assembly plan entry (caller supplies arrays) */
typedef struct {
    const char *relpath;      /* e.g., "bin/mytool" */
    const char *target_path;  /* absolute path into store/<pkg>/<ver>/files/... */
    const char *pkg_name;
    const char *pkg_version;
} ProfileEntry;

/* Assemble a temporary profile symlink forest and check conflicts.
   - entries: array of ProfileEntry
   - n_entries: count
   - tmp_profile_path_out: malloced path to tmp profile directory on success (caller frees)
   - If conflict or missing target, function returns PROFILE_CONFLICT or PROFILE_MISSING_TARGET and prints diagnostics.
*/
int profile_assemble_tmp(const ProfileEntry *entries, size_t n_entries, char **tmp_profile_path_out);

/* Atomically activate tmp_profile_path by renaming to profiles/<profile>-new-<uuid>, then swap vir symlink.
   - tmp_profile_path must be within $HOME/pandora/profiles
   Returns 0 on success, nonzero on failure. */
int profile_atomic_activate(const char *tmp_profile_path, const char *profile_name);

#endif
