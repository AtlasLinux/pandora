#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <stdio.h>
#include <stddef.h>

/* Download progress callback: bytes_received, total_known (0 if unknown), userdata */
typedef void (*download_progress_cb)(size_t, size_t, void*);

/* Stream-download URL into a temporary file path and compute SHA256.
   On success returns 0 and fills out_temp_path (caller frees with free()) and out_sha256 (hex string malloced).
   On failure returns nonzero and out_* set to NULL. */
int downloader_stream_to_temp_with_sha256(const char *url,
                                          char **out_temp_path,
                                          char **out_sha256_hex,
                                          download_progress_cb progress,
                                          void *userdata);

/* Download raw URL to a provided open FILE* (seekable) while computing sha256.
   Returns 0 on success, nonzero on error; out_sha256_hex allocated on success. */
int downloader_write_file_with_sha256(const char *url, FILE *out_fd, char **out_sha256_hex,
                                     download_progress_cb progress, void *userdata);

#endif /* DOWNLOADER_H */
