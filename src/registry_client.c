// registry_client.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "registry_client.h"
#include "downloader.h" /* downloader_stream_to_temp_with_sha256 */
#include "acl.h"
#include <unistd.h>

/* Simple client that caches index_url, raw text and parsed AclBlock */
struct RegistryClient {
    char *index_url;    /* malloc'd */
    char *index_text;   /* malloc'd raw index.acl (optional) */
    AclBlock *index_acl;/* parsed index cached (owned) */
    int curl_initialized;
};

/* create/destroy client */
RegistryClient *registry_client_create(void)
{
    RegistryClient *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->index_url = NULL;
    c->index_text = NULL;
    c->index_acl = NULL;
    c->curl_initialized = 0;
    return c;
}

void registry_client_destroy(RegistryClient *c)
{
    if (!c) return;
    free(c->index_url);
    free(c->index_text);
    if (c->index_acl) acl_free(c->index_acl);
    free(c);
}

/* set the index URL */
int registry_client_set_index(RegistryClient *c, const char *index_url)
{
    if (!c || !index_url) return -1;
    char *s = strdup(index_url);
    if (!s) return -1;
    free(c->index_url);
    c->index_url = s;
    return 0;
}

/* Helper: read a file path produced by downloader into a malloc'd string; caller frees */
static char *read_file_to_string(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

 /* Fetch index URL, cache text and parsed AclBlock, return parsed block (owned by client) */
AclBlock *registry_client_fetch_index(RegistryClient *c)
{
    if (!c || !c->index_url) return NULL;

    char *txt = NULL;
    char *tmp_path = NULL;
    char *sha = NULL;

    /* If index_url looks like http(s), use downloader; otherwise treat as local file path */
    if (strncmp(c->index_url, "http://", 7) == 0 || strncmp(c->index_url, "https://", 8) == 0) {
        int rc = downloader_stream_to_temp_with_sha256(c->index_url, &tmp_path, &sha, NULL, NULL);
        free(sha);
        if (rc != 0 || !tmp_path) {
            free(tmp_path);
            return NULL;
        }
        txt = read_file_to_string(tmp_path);
        unlink(tmp_path);
        free(tmp_path);
        if (!txt) return NULL;
    } else {
        /* local file path */
        txt = read_file_to_string(c->index_url);
        if (!txt) return NULL;
    }

    /* free any previous cached index */
    free(c->index_text);
    if (c->index_acl) { acl_free(c->index_acl); c->index_acl = NULL; }
    c->index_text = txt;

    AclBlock *root = acl_parse_string(txt);
    if (!root) return NULL;
    if (!acl_resolve_all(root)) { acl_free(root); return NULL; }

    c->index_acl = root;
    return root;
}

/* Fetch manifest.acl by URL and parse it, return parsed block (caller owns) */
AclBlock *registry_client_fetch_manifest(RegistryClient *c, const char *manifest_url)
{
    (void)c;
    if (!manifest_url) return NULL;

    char *tmp_path = NULL;
    char *sha = NULL;
    int rc = downloader_stream_to_temp_with_sha256(manifest_url, &tmp_path, &sha, NULL, NULL);
    free(sha);
    if (rc != 0 || !tmp_path) { free(tmp_path); return NULL; }

    char *txt = read_file_to_string(tmp_path);
    unlink(tmp_path);
    free(tmp_path);
    if (!txt) return NULL;

    AclBlock *b = acl_parse_string(txt);
    free(txt);
    if (!b) return NULL;
    if (!acl_resolve_all(b)) { acl_free(b); return NULL; }
    return b;
}

/* Helper: call acl_get_string with a formatted path; returns strdup'd value or NULL.
   acl_get_string is expected to allocate *out and return non-zero on success; adapt if your API differs. */
static char *acl_get_string_dup_fmt(AclBlock *root, const char *fmt, ...)
{
    if (!root || !fmt) return NULL;
    char path[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(path)) return NULL;

    char *tmp = NULL;
    if (!acl_get_string(root, path, &tmp)) return NULL;
    char *out = strdup(tmp);
    free(tmp);
    return out;
}

/* Public finders: first check nested Registry.Package then Package. Return malloc'd string or NULL. */

char *registry_client_find_manifest_url(AclBlock *index, const char *pkg_name, const char *version)
{
    if (!index || !pkg_name || !version) return NULL;

    /* Try Registry.Package["name"].Version["ver"].manifest_url */
    char *res = acl_get_string_dup_fmt(index, "Registry.Package[\"%s\"].Version[\"%s\"].manifest_url", pkg_name, version);
    if (res) return res;

    /* Try non-nested Package */
    res = acl_get_string_dup_fmt(index, "Package[\"%s\"].Version[\"%s\"].manifest_url", pkg_name, version);
    if (res) return res;

    /* Fallback per-package per-version top key */
    res = acl_get_string_dup_fmt(index, "Registry.Package[\"%s\"].manifest_url_%s", pkg_name, version);
    if (res) return res;
    res = acl_get_string_dup_fmt(index, "Package[\"%s\"].manifest_url_%s", pkg_name, version);
    if (res) return res;

    return NULL;
}

char *registry_client_find_pkg_url(AclBlock *index, const char *pkg_name, const char *version)
{
    if (!index || !pkg_name || !version) return NULL;

    char *res = acl_get_string_dup_fmt(index, "Registry.Package[\"%s\"].Version[\"%s\"].pkg_url", pkg_name, version);
    if (res) return res;

    res = acl_get_string_dup_fmt(index, "Package[\"%s\"].Version[\"%s\"].pkg_url", pkg_name, version);
    if (res) return res;

    res = acl_get_string_dup_fmt(index, "Registry.Package[\"%s\"].pkg_url_%s", pkg_name, version);
    if (res) return res;
    res = acl_get_string_dup_fmt(index, "Package[\"%s\"].pkg_url_%s", pkg_name, version);
    if (res) return res;

    /* Try pkg_base_url fallback (nested then non-nested) */
    char *base = acl_get_string_dup_fmt(index, "Registry.Package[\"%s\"].pkg_base_url", pkg_name);
    if (base) {
        size_t need = strlen(base) + 1 + strlen(version) + 1 + strlen(pkg_name) + strlen(version) + 8;
        char *constructed = malloc(need);
        if (constructed) {
            snprintf(constructed, need, "%s/%s/%s-%s.pkg", base, version, pkg_name, version);
            free(base);
            return constructed;
        }
        free(base);
    }
    base = acl_get_string_dup_fmt(index, "Package[\"%s\"].pkg_base_url", pkg_name);
    if (base) {
        size_t need = strlen(base) + 1 + strlen(version) + 1 + strlen(pkg_name) + strlen(version) + 8;
        char *constructed = malloc(need);
        if (constructed) {
            snprintf(constructed, need, "%s/%s/%s-%s.pkg", base, version, pkg_name, version);
            free(base);
            return constructed;
        }
        free(base);
    }

    return NULL;
}
