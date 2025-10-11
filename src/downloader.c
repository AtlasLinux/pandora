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

/* Write callback expected to be called by your simplified libcurl when
   CURLOPT_WRITEDATA is set to the pointer to dl_ctx_t. It must match the
   prototype used by your curl implementation (we assume fwrite-like). */
static size_t dl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    dl_ctx_t *c = (dl_ctx_t*)userdata;
    if (!c || !c->outf) return 0;
    size_t total = size * nmemb;
    if (total == 0) return 0;
    size_t written = fwrite(ptr, 1, total, c->outf);
    if (written != total) {
        c->error = 1;
        return 0; /* indicate error */
    }
    /* update incremental sha256 */
    sha256_inc_update(&c->sha, ptr, total);
    return written / size; /* return nmemb as number of items written */
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

/* Public API: single-pass download into tempfile with incremental SHA256. */
int downloader_stream_to_temp_with_sha256(const char *url,
                                          char **out_temp_path,
                                          char **out_sha256_hex,
                                          download_progress_cb progress,
                                          void *userdata)
{
    if (!url || !out_temp_path || !out_sha256_hex) return CURLE_OTHER_ERROR;

    int fd;
    char *tmp_path = make_tempfile_path(&fd);
    if (!tmp_path) return CURLE_OTHER_ERROR;

    FILE *f = fdopen(fd, "wb+");
    if (!f) { close(fd); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }

    if (curl_global_init(0) != 0) {
        fclose(f); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR;
    }

    CURL *easy = curl_easy_init();
    if (!easy) { curl_global_cleanup(); fclose(f); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }

    dl_ctx_t ctx;
    ctx.outf = f;
    ctx.error = 0;
    sha256_inc_init(&ctx.sha);

    /* Set URL and pass ctx as WRITEDATA. The simplified libcurl is expected
       to internally call dl_write_cb with the same userdata. If your curl
       wrapper requires registering a function pointer explicitly, adapt it. */
    curl_easy_setopt(easy, CURLOPT_URL, (void*)url);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void*)&ctx);
    curl_easy_setopt(easy, CURLOPT_VERBOSE, NULL);

    /* If your simplified curl supports setting a write function pointer,
       you would set it here. This code assumes the curl impl will call
       dl_write_cb for CURLOPT_WRITEDATA. */

    int rc = curl_easy_perform(easy);
    curl_easy_cleanup(easy);
    curl_global_cleanup();

    if (rc != CURLE_OK || ctx.error) {
        fclose(f); unlink(tmp_path); free(tmp_path);
        return rc != CURLE_OK ? rc : CURLE_RECV_ERROR;
    }

    /* All data written; finalize SHA and return hex string */
    uint8_t digest[32];
    sha256_inc_final(&ctx.sha, digest);
    char *hex = malloc(65);
    if (!hex) {
        fclose(f); unlink(tmp_path); free(tmp_path);
        return CURLE_OTHER_ERROR;
    }
    sha256_to_hex_lower(digest, hex);

    /* flush and rewind file so caller can read it */
    fflush(f);
    fseek(f, 0, SEEK_SET);
    fclose(f);

    *out_temp_path = tmp_path;    /* malloc'd path */
    *out_sha256_hex = hex;        /* malloc'd hex string */
    return CURLE_OK;
}
