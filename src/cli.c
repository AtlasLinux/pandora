// cli.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "downloader.h"
#include "store_manager.h"
#include "profile_manager.h"
#include "registry_client.h"
#include "acl.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s install <name@version> --index <index_url> [--no-activate] [--profile <name>] [-y]\n",
        prog);
}

/* Helper: split "name@version" into name and version; returns 0 on ok. */
static int split_name_ver(const char *s, char **out_name, char **out_ver) {
    if (!s || !out_name || !out_ver) return -1;
    char *at = strchr(s, '@');
    if (!at) return -1;
    size_t nlen = (size_t)(at - s);
    *out_name = malloc(nlen + 1);
    if (!*out_name) return -1;
    memcpy(*out_name, s, nlen);
    (*out_name)[nlen] = '\0';
    *out_ver = strdup(at + 1);
    if (!*out_ver) { free(*out_name); return -1; }
    return 0;
}

/* Prompt yes/no; returns 1 for yes, 0 for no */
static int prompt_yesno(const char *msg, int assume_yes) {
    if (assume_yes) return 1;
    fprintf(stderr, "%s [y/N]: ", msg);
    fflush(stderr);
    char buf[8];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    if (buf[0] == 'y' || buf[0] == 'Y') return 1;
    return 0;
}

/* Minimal helper to extract manifest fields using ACL: manifest must contain NAME, VERSION, SHA256 */
static int manifest_get_sha256_and_names(AclBlock *manifest, char **out_name, char **out_ver, char **out_sha256) {
    if (!manifest || !out_name || !out_ver || !out_sha256) return -1;
    char *v = NULL;
    if (!acl_get_string(manifest, "Manifest.name", &v)) return -1;
    *out_name = strdup(v);
    free(v);
    v = NULL;
    if (!acl_get_string(manifest, "Manifest.version", &v)) { free(*out_name); return -1; }
    *out_ver = strdup(v);
    free(v);
    v = NULL;
    if (!acl_get_string(manifest, "Manifest.sha256", &v)) { free(*out_name); free(*out_ver); return -1; }
    *out_sha256 = strdup(v);
    free(v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }

    if (strcmp(argv[1], "install") != 0) { usage(argv[0]); return 2; }

    /* parse args */
    const char *pkg_spec = NULL;
    const char *index_url = "https://atlaslinux.github.io/pandora/index.acl";
    const char *profile = "default";
    int no_activate = 0;
    int assume_yes = 0;

    for (int i = 2; i < argc; ++i) {
        if (!pkg_spec && argv[i][0] != '-') {
            pkg_spec = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--index") == 0 && i+1 < argc) {
            index_url = argv[++i]; continue;
        }
        if (strcmp(argv[i], "--no-activate") == 0) { no_activate = 1; continue; }
        if (strcmp(argv[i], "--profile") == 0 && i+1 < argc) { profile = argv[++i]; continue; }
        if (strcmp(argv[i], "-y") == 0) { assume_yes = 1; continue; }
        fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }
    if (!pkg_spec) { usage(argv[0]); return 2; }

    /* parse name@version */
    char *pkg_name = NULL, *pkg_ver = NULL;
    if (split_name_ver(pkg_spec, &pkg_name, &pkg_ver) != 0) {
        fprintf(stderr, "Invalid package spec; use name@version\n");
        return 2;
    }

    /* Create registry client and fetch index */
    RegistryClient *rc = registry_client_create();
    if (!rc) { fprintf(stderr, "Failed to create registry client\n"); free(pkg_name); free(pkg_ver); return 1; }
    if (registry_client_set_index(rc, index_url) != 0) {
        fprintf(stderr, "Failed to set index URL\n"); registry_client_destroy(rc); free(pkg_name); free(pkg_ver); return 1;
    }
    AclBlock *index = registry_client_fetch_index(rc);
    if (!index) { fprintf(stderr, "Failed to fetch index\n"); registry_client_destroy(rc); free(pkg_name); free(pkg_ver); return 1; }

    /* Find manifest URL and pkg URL */
    char *manifest_url = registry_client_find_manifest_url(index, pkg_name, pkg_ver);
    char *pkg_url = registry_client_find_pkg_url(index, pkg_name, pkg_ver);
    if (!manifest_url || !pkg_url) {
        fprintf(stderr, "Package %s@%s not found in index\n", pkg_name, pkg_ver);
        free(manifest_url);
        free(pkg_url);
        acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }

    /* Fetch manifest */
    AclBlock *manifest = registry_client_fetch_manifest(rc, manifest_url);
    if (!manifest) {
        fprintf(stderr, "Failed to fetch manifest at %s\n", manifest_url);
        free(manifest_url); free(pkg_url); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }

    /* Extract SHA256 from manifest */
    char *mname = NULL, *mver = NULL, *expected_sha = NULL;
    if (manifest_get_sha256_and_names(manifest, &mname, &mver, &expected_sha) != 0) {
        fprintf(stderr, "Malformed manifest: missing required fields\n");
        free(manifest_url); free(pkg_url); acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }

    /* Confirm manifest matches requested package */
    if (strcmp(mname, pkg_name) != 0 || strcmp(mver, pkg_ver) != 0) {
        fprintf(stderr, "Manifest mismatch (index vs manifest)\n");
        free(mname); free(mver); free(expected_sha); free(manifest_url); free(pkg_url);
        acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }

    /* Download package into tempfile while computing sha256 */
    char *tmp_path = NULL;
    char *computed_sha = NULL;
    fprintf(stderr, "Downloading %s ...\n", pkg_url);
    int dlrc = downloader_stream_to_temp_with_sha256(pkg_url, &tmp_path, &computed_sha, NULL, NULL);
    if (dlrc != 0) {
        fprintf(stderr, "Download failed (code %d)\n", dlrc);
        free(mname); free(mver); free(expected_sha); free(manifest_url); free(pkg_url);
        acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }
    if (!tmp_path || !computed_sha) {
        fprintf(stderr, "Download did not produce expected outputs\n");
        free(tmp_path); free(computed_sha);
        free(mname); free(mver); free(expected_sha); free(manifest_url); free(pkg_url);
        acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }

    /* Compare computed SHA with expected */
    if (strcmp(computed_sha, expected_sha) != 0) {
        fprintf(stderr, "SHA256 mismatch!\n  expected: %s\n  computed: %s\n", expected_sha, computed_sha);
        unlink(tmp_path); free(tmp_path); free(computed_sha);
        free(mname); free(mver); free(expected_sha); free(manifest_url); free(pkg_url);
        acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }
    fprintf(stderr, "SHA256 verified: %s\n", computed_sha);

    /* Import package into store atomically */
    char *store_path = NULL;
    if (store_import_pkg_atomic(tmp_path, pkg_name, pkg_ver, expected_sha, &store_path) != 0) {
        fprintf(stderr, "Failed to import package into store\n");
        unlink(tmp_path); free(tmp_path); free(computed_sha);
        free(mname); free(mver); free(expected_sha); free(manifest_url); free(pkg_url);
        acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }
    if (!store_path) {
        fprintf(stderr, "Store import did not return store path\n");
        unlink(tmp_path); free(tmp_path); free(computed_sha);
        free(mname); free(mver); free(expected_sha); free(manifest_url); free(pkg_url);
        acl_free(manifest); acl_free(index); registry_client_destroy(rc);
        free(pkg_name); free(pkg_ver);
        return 1;
    }
    fprintf(stderr, "Imported into store: %s\n", store_path);

    /* Cleanup temporary package file */
    unlink(tmp_path);
    free(tmp_path);
    free(computed_sha);

    /* Optionally assemble and activate profile */
    if (!no_activate) {
        char promptbuf[256];
        snprintf(promptbuf, sizeof(promptbuf), "Activate %s@%s into profile '%s' now?", pkg_name, pkg_ver, profile);
        if (!prompt_yesno(promptbuf, assume_yes)) {
            fprintf(stderr, "Skipping activation. Use 'pandora activate %s@%s --profile %s' later.\n", pkg_name, pkg_ver, profile);
        } else {
            /* Build a single-entry profile plan pointing relpath "<pkg_name>" to store_path/files */
            ProfileEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.relpath = strdup(pkg_name);
            if (!entry.relpath) {
                fprintf(stderr, "Out of memory\n");
                free(store_path); free(mname); free(mver); free(expected_sha);
                free(manifest_url); free(pkg_url); acl_free(manifest); acl_free(index); registry_client_destroy(rc);
                free(pkg_name); free(pkg_ver);
                return 1;
            }

            size_t tlen = strlen(store_path) + strlen("/files") + 1;
            char *target = malloc(tlen + 1);
            if (!target) {
                fprintf(stderr, "Out of memory\n");
                free((void*)entry.relpath);
                free(store_path); free(mname); free(mver); free(expected_sha);
                free(manifest_url); free(pkg_url); acl_free(manifest); acl_free(index); registry_client_destroy(rc);
                free(pkg_name); free(pkg_ver);
                return 1;
            }
            snprintf(target, tlen + 1, "%s/files", store_path);
            entry.target_path = target;
            entry.pkg_name = pkg_name;
            entry.pkg_version = pkg_ver;

            char *tmp_profile_path = NULL;
            int asm_rc = profile_assemble_tmp(&entry, 1, &tmp_profile_path);
            free((void*)entry.target_path);
            free((void*)entry.relpath);

            if (asm_rc != PROFILE_OK) {
                fprintf(stderr, "Failed to assemble profile (code %d)\n", asm_rc);
                free(store_path); free(mname); free(mver); free(expected_sha);
                free(manifest_url); free(pkg_url); acl_free(manifest); acl_free(index); registry_client_destroy(rc);
                free(pkg_name); free(pkg_ver);
                return 1;
            }

            if (profile_atomic_activate(tmp_profile_path, profile) != 0) {
                fprintf(stderr, "Failed to activate profile\n");
                /* keep tmp_profile_path for inspection */
            } else {
                fprintf(stderr, "Activated %s@%s into profile %s\n", pkg_name, pkg_ver, profile);
            }
            free(tmp_profile_path);
        }
    } else {
        fprintf(stderr, "Installed %s@%s but did not activate (--no-activate)\n", pkg_name, pkg_ver);
    }

    /* Cleanup and exit */
    free(store_path);
    free(mname); free(mver); free(expected_sha);
    free(manifest_url); free(pkg_url);
    acl_free(manifest);
    acl_free(index);
    registry_client_destroy(rc);
    free(pkg_name); free(pkg_ver);
    return 0;
}
