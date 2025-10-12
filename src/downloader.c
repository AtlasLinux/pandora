#define _POSIX_C_SOURCE 200809L
#include "curl.h"            /* your simplified libcurl header */
#include "downloader.h"      /* forward declarations from earlier messages */
#include "sha256.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

/* Create tempfile and return its path (malloc'd) or NULL on error.
   Template is created under /tmp; adapt to PANDORA tmp path if desired. */
static char *make_tempfile_path(int *out_fd) {
    char tmpl[] = "/tmp/pandora_dl_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
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

    /* Use default write function provided by simplified curl.h implementation.
       Provide the FILE* as WRITEDATA so curl will fwrite into it. */
    curl_easy_setopt(easy, CURLOPT_URL, (void*)url);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void*)f);
    curl_easy_setopt(easy, CURLOPT_VERBOSE, (void*)(intptr_t)0);

    int rc = curl_easy_perform(easy);
    curl_easy_cleanup(easy);
    curl_global_cleanup();

    if (rc != CURLE_OK) {
        fclose(f); unlink(tmp_path); free(tmp_path);
        return rc;
    }

    fflush(f);
#if defined(_POSIX_VERSION)
    fsync(fileno(f));
#endif
    fclose(f);

    /* Compute SHA-256 using the exact same method as the standalone sha256 CLI above:
       stat the file, allocate a buffer of file size, read the entire file, run one-shot sha256(). */
    struct stat st;
    if (stat(tmp_path, &st) != 0) {
        unlink(tmp_path); free(tmp_path); return CURLE_RECV_ERROR;
    }

    size_t filesize = (size_t)st.st_size;
    uint8_t *data = NULL;

    if (filesize > 0) {
        FILE *r = fopen(tmp_path, "rb");
        if (!r) { unlink(tmp_path); free(tmp_path); return CURLE_RECV_ERROR; }

        data = malloc(filesize);
        if (!data) { fclose(r); unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }

        size_t read_total = 0;
        while (read_total < filesize) {
            size_t n = fread(data + read_total, 1, filesize - read_total, r);
            if (n == 0) {
                if (ferror(r)) {
                    fclose(r);
                    free(data);
                    unlink(tmp_path);
                    free(tmp_path);
                    return CURLE_RECV_ERROR;
                }
                break;
            }
            read_total += n;
        }
        fclose(r);
        if (read_total != filesize) {
            free(data);
            unlink(tmp_path);
            free(tmp_path);
            return CURLE_RECV_ERROR;
        }
    }

    uint8_t digest[32];
    sha256(data, filesize, digest);

    if (data) free(data);

    char *hex = malloc(65);
    if (!hex) { unlink(tmp_path); free(tmp_path); return CURLE_OTHER_ERROR; }
    sha256_to_hex(digest, hex);

    *out_temp_path = tmp_path;
    *out_sha256_hex = hex;

    return CURLE_OK;
}
