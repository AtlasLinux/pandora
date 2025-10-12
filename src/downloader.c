#define _POSIX_C_SOURCE 200809L
#include "curl.h"            /* your simplified libcurl header */
#include "downloader.h"      /* forward declarations from earlier messages */
#include "sha256.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

/* Context passed to write callback */
typedef struct {
    FILE *outf;
    sha256_ctx_t sha;
    int error;
} dl_ctx_t;

/* Write callback must return number of bytes handled (size * nmemb) on success */
static size_t dl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    dl_ctx_t *c = (dl_ctx_t*)userdata;
    if (!c || !c->outf) return 0;
    size_t total = size * nmemb;
    if (total == 0) return 0;
    size_t written = fwrite(ptr, 1, total, c->outf);
    if (written != total) {
        c->error = 1;
        return 0; /* indicate error to libcurl */
    }
    sha256_inc_update(&c->sha, ptr, total);
    return total; /* MUST return number of bytes processed */
}

/* Create tempfile and return its path (malloc'd) or NULL on error.
   Template is created under /tmp; adapt to PANDORA tmp path if desired. */
static char *make_tempfile_path(int *out_fd) {
    char tmpl[] = "/tmp/pandora_dl_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    /* Caller takes ownership of fd */
    if (out_fd) *out_fd = fd;
    return strdup(tmpl);
}

int downloader_stream_to_temp_with_sha256(const char *url,
                                          char **out_temp_path,
                                          char **out_sha256_hex,
                                          download_progress_cb progress,
                                          void *userdata)
{
    (void)progress; (void)userdata;
    puts(url);

    if (!url || !out_temp_path || !out_sha256_hex) return CURLE_OTHER_ERROR;

    int fd;
    char *tmp_path = make_tempfile_path(&fd);
    if (!tmp_path) return CURLE_OTHER_ERROR;

    FILE *f = fdopen(fd, "wb+");
    if (!f) { close(fd); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }

    /* initialize curl for this request (you may prefer a single global init elsewhere) */
    if (curl_global_init(0) != 0) {
        fclose(f); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR;
    }

    CURL *easy = curl_easy_init();
    if (!easy) { curl_global_cleanup(); fclose(f); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }

    /* Ask your simple curl to write directly into our FILE* */
    curl_easy_setopt(easy, CURLOPT_URL, (void*)url);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void*)f);
    /* your wrapper expects a pointer-sized flag for verbose; mirror how your test app set it */
    curl_easy_setopt(easy, CURLOPT_VERBOSE, (void*)(intptr_t)1);

    int rc = curl_easy_perform(easy);
    fprintf(stderr, "DEBUG: curl_easy_perfom returned rc=%d\n", rc);
    curl_easy_cleanup(easy);
    curl_global_cleanup();

    if (rc != CURLE_OK) {
        fclose(f); unlink(tmp_path); free(tmp_path);
        return rc;
    }

    /* flush and close file so we can reopen for reading and compute SHA */
    fflush(f);
    fclose(f);

    /* Compute SHA256 by reading the file we just wrote */
    uint8_t digest[32];
    sha256_ctx_t sha;
    sha256_inc_init(&sha);

    FILE *r = fopen(tmp_path, "rb");
    if (!r) { unlink(tmp_path); free(tmp_path); return CURLE_RECV_ERROR; }

    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), r)) > 0) {
        sha256_inc_update(&sha, buf, n);
    }
    if (ferror(r)) {
        fclose(r); unlink(tmp_path); free(tmp_path); return CURLE_RECV_ERROR;
    }
    fclose(r);

    sha256_inc_final(&sha, digest);

    char *hex = malloc(65);
    if (!hex) { unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }
    sha256_to_hex_lower(digest, hex);

    *out_temp_path = tmp_path;    /* malloc'd path */
    *out_sha256_hex = hex;        /* malloc'd hex string */

    return CURLE_OK;
}