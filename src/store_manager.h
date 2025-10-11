#ifndef STORE_MANAGER_H
#define STORE_MANAGER_H

#include <stddef.h>

/* Import a verified .pkg file into the store atomically.
   - pkg_path: path to local .pkg file (already downloaded)
   - pkg_name, pkg_version: name and exact version (UTF-8)
   - expected_sha256: hex digest from manifest (verify again for safety)
   - out_store_path: *optional* malloc-ed string with final store path (free with free())
   Returns 0 on success, nonzero on error. */
int store_import_pkg_atomic(const char *pkg_path,
                            const char *pkg_name,
                            const char *pkg_version,
                            const char *expected_sha256,
                            char **out_store_path);

/* Verify unpack safety (path traversal, symlink loops). Returns 0 if safe, nonzero if unsafe. */
int store_validate_unpacked_tree(const char *unpack_path);

/* Remove unreferenced store/<pkg>/<ver> directory (requires explicit confirmation). */
int store_remove_version(const char *pkg_name, const char *pkg_version);

#endif
