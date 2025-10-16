#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "net/download.h"
#include "util/err.h"
#include "core/sha256.h"
#include "core/acl.h"
#include "core/curl.h"
#include "util/sha256.h"
#include "util/path.h"

#define PATH_MAX_LEN 1024
#define SMALL_PATH_LEN 512
#define SHA256_HEX_LEN 65
#define SHA256_BIN_LEN 32

/* Download url -> out_path. Returns 0 on success, ERR_FAILED on curl failure, -1 on other */
static int download_to_file(const char *url, const char *out_path) {
    FILE *out = fopen(out_path, "wb");
    if (!out) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(out);
        return -1;
    }

    int rc = 0;
    if (curl_easy_setopt(curl, CURLOPT_URL, (void*)url) != CURLE_OK
     || curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)out) != CURLE_OK
     || curl_easy_setopt(curl, CURLOPT_VERBOSE, (void*)(intptr_t)0) != CURLE_OK) {
        rc = -1;
        goto done;
    }

    int res = curl_easy_perform(curl);
    if (res != CURLE_OK) rc = ERR_FAILED;

done:
    curl_easy_cleanup(curl);
    fclose(out);
    return rc;
}

/* Retrieve a string value from ACL; returns 0 on success and sets *out to a newly allocated copy.
   Caller must free *out. Returns -1 on missing/other error. */
static int acl_get_string_dup(AclBlock *root, const char *key, char **out) {
    char *tmp = NULL;
    if (acl_get_string(root, key, &tmp) && !tmp) return -1;
    if (!tmp) return -1;
    *out = strdup(tmp);
    return *out ? 0 : -1;
}

error_t fetch_package(const char* name, const char* version) {
    const char *home_env = getenv("HOME");
    if (!home_env) {
        fprintf(stderr, "HOME not set\n");
        return -1;
    }

    char conf_path[SMALL_PATH_LEN];
    if (snprintf(conf_path, sizeof(conf_path), "%s/conf/pandora.conf", home_env) >= (int)sizeof(conf_path)) {
        fprintf(stderr, "conf path too long\n");
        return -1;
    }

    AclBlock *root = acl_parse_file(conf_path);
    if (!root) {
        fprintf(stderr, "Failed to parse config %s\n", conf_path);
        return -1;
    }

    char *mirror_index = NULL;
    if (acl_get_string_dup(root, "Pandora.Mirrors.mirror.index", &mirror_index) != 0) {
        fprintf(stderr, "Missing required mirror index in config %s\n", conf_path);
        acl_free(root);
        return -1;
    }

    /* Ensure directories exist (best-effort) */
    {
        char tmp_dir[SMALL_PATH_LEN];
        char manifests_dir[SMALL_PATH_LEN];
        char pkgs_dir[SMALL_PATH_LEN];

        if (snprintf(tmp_dir, sizeof(tmp_dir), "%s/pandora/tmp", home_env) >= (int)sizeof(tmp_dir)
         || snprintf(manifests_dir, sizeof(manifests_dir), "%s/pandora/manifests", home_env) >= (int)sizeof(manifests_dir)
         || snprintf(pkgs_dir, sizeof(pkgs_dir), "%s/pandora/pkgs", home_env) >= (int)sizeof(pkgs_dir)) {
            fprintf(stderr, "path too long\n");
            free(mirror_index);
            acl_free(root);
            return -1;
        }

        (void)ensure_dir(tmp_dir, 0755);
        (void)ensure_dir(manifests_dir, 0755);
        (void)ensure_dir(pkgs_dir, 0755);
    }

    char index_path[SMALL_PATH_LEN];
    if (snprintf(index_path, sizeof(index_path), "%s/pandora/tmp/index.acl", home_env) >= (int)sizeof(index_path)) {
        fprintf(stderr, "index path too long\n");
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    struct stat st;

    /* Initialize curl once for this operation */
    if (curl_global_init(0) != 0) {
        fprintf(stderr, "curl_global_init failed\n");
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    /* Download index if missing */
    if (stat(index_path, &st) != 0) {
        int dres = download_to_file(mirror_index, index_path);
        if (dres != 0) {
            if (dres == ERR_FAILED) fprintf(stderr, "curl_easy_perform failed when downloading index\n");
            else perror("fopen/download");
            curl_global_cleanup();
            free(mirror_index);
            acl_free(root);
            return ERR_FAILED;
        }
    }

    AclBlock *index_root = acl_parse_file(index_path);
    if (!index_root) {
        fprintf(stderr, "Failed to parse index %s\n", index_path);
        curl_global_cleanup();
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    char manifest_key[SMALL_PATH_LEN];
    if (snprintf(manifest_key, sizeof(manifest_key),
                 "Registry.Package[\"%s\"].Version[\"%s\"].manifest_url", name, version) >= (int)sizeof(manifest_key)) {
        fprintf(stderr, "manifest key too long\n");
        acl_free(index_root);
        curl_global_cleanup();
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    char *manifest_url = NULL;
    if (acl_get_string_dup(index_root, manifest_key, &manifest_url) != 0) {
        fprintf(stderr, "manifest_url not found for %s-%s in index\n", name, version);
        acl_free(index_root);
        curl_global_cleanup();
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    char manifest_path[SMALL_PATH_LEN];
    if (snprintf(manifest_path, sizeof(manifest_path), "%s/pandora/manifests/%s-%s-manifest.acl",
                 home_env, name, version) >= (int)sizeof(manifest_path)) {
        fprintf(stderr, "manifest path too long\n");
        free(manifest_url);
        acl_free(index_root);
        curl_global_cleanup();
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    if (stat(manifest_path, &st) != 0) {
        int dres = download_to_file(manifest_url, manifest_path);
        if (dres != 0) {
            if (dres == ERR_FAILED) fprintf(stderr, "curl_easy_perform failed when downloading manifest\n");
            else perror("fopen/download");
            free(manifest_url);
            acl_free(index_root);
            curl_global_cleanup();
            free(mirror_index);
            acl_free(root);
            return ERR_FAILED;
        }
    }

    AclBlock *manifest_root = acl_parse_file(manifest_path);
    if (!manifest_root) {
        fprintf(stderr, "Failed to parse manifest %s\n", manifest_path);
        free(manifest_url);
        acl_free(index_root);
        curl_global_cleanup();
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    char *pkg_url = NULL;
    char *expected_sha256 = NULL;
    if (acl_get_string_dup(manifest_root, "Manifest.pkg_url", &pkg_url) != 0
     || acl_get_string_dup(manifest_root, "Manifest.sha256", &expected_sha256) != 0) {
        fprintf(stderr, "Missing pkg_url or sha256 in manifest\n");
        if (pkg_url) free(pkg_url);
        if (expected_sha256) free(expected_sha256);
        acl_free(manifest_root);
        acl_free(index_root);
        curl_global_cleanup();
        free(manifest_url);
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    char pkg_path[SMALL_PATH_LEN];
    if (snprintf(pkg_path, sizeof(pkg_path), "%s/pandora/pkgs/%s-%s.pkg", home_env, name, version) >= (int)sizeof(pkg_path)) {
        fprintf(stderr, "package path too long\n");
        free(pkg_url);
        free(expected_sha256);
        acl_free(manifest_root);
        acl_free(index_root);
        curl_global_cleanup();
        free(manifest_url);
        free(mirror_index);
        acl_free(root);
        return -1;
    }

    if (stat(pkg_path, &st) != 0) {
        int dres = download_to_file(pkg_url, pkg_path);
        if (dres != 0) {
            if (dres == ERR_FAILED) fprintf(stderr, "curl_easy_perform failed when downloading package\n");
            else perror("fopen/download");
            free(pkg_url);
            free(expected_sha256);
            acl_free(manifest_root);
            acl_free(index_root);
            curl_global_cleanup();
            free(manifest_url);
            free(mirror_index);
            acl_free(root);
            return ERR_FAILED;
        }
    }

    /* Done with network operations */
    curl_global_cleanup();

    /* Verify SHA-256 */
    char actual_sha256[SHA256_HEX_LEN] = {0};
    if (sha256_file_hex(pkg_path, actual_sha256) != 0) {
        fprintf(stderr, "sha256_file failed\n");
        free(pkg_url);
        free(expected_sha256);
        acl_free(manifest_root);
        acl_free(index_root);
        free(manifest_url);
        free(mirror_index);
        acl_free(root);
        return ERR_FAILED;
    }

    uint8_t expected_bin[SHA256_BIN_LEN];
    uint8_t actual_bin[SHA256_BIN_LEN];
    if (hex_to_bin(expected_sha256, expected_bin, sizeof(expected_bin)) != (int)sizeof(expected_bin)) {
        fprintf(stderr, "invalid expected sha256 hex\n");
        free(pkg_url);
        free(expected_sha256);
        acl_free(manifest_root);
        acl_free(index_root);
        free(manifest_url);
        free(mirror_index);
        acl_free(root);
        return ERR_FAILED;
    }
    if (hex_to_bin(actual_sha256, actual_bin, sizeof(actual_bin)) != (int)sizeof(actual_bin)) {
        fprintf(stderr, "invalid computed sha256 hex\n");
        free(pkg_url);
        free(expected_sha256);
        acl_free(manifest_root);
        acl_free(index_root);
        free(manifest_url);
        free(mirror_index);
        acl_free(root);
        return ERR_FAILED;
    }

    if (!ct_memcmp(expected_bin, actual_bin, sizeof(expected_bin))) {
        fprintf(stderr, "SHA-256 mismatch:\nExpected: %s\nActual:   %s\n", expected_sha256, actual_sha256);
        free(pkg_url);
        free(expected_sha256);
        acl_free(manifest_root);
        acl_free(index_root);
        free(manifest_url);
        free(mirror_index);
        acl_free(root);
        return ERR_FAILED;
    }

    free(pkg_url);
    free(expected_sha256);
    acl_free(manifest_root);
    acl_free(index_root);
    free(manifest_url);
    free(mirror_index);
    acl_free(root);
    return 0;
}
