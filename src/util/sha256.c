#include "core/sha256.h"
#include <stdio.h>
#include <stdlib.h>

int sha256_file_hex(const char *path, char out_hex[65]) {
    if (!path || !out_hex) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Read file into a growing buffer */
    uint8_t *buf = NULL;
    size_t cap = 0, len = 0;
    const size_t CHUNK = 65536;
    for (;;) {
        if (cap - len < CHUNK) {
            size_t newcap = cap ? cap * 2 : CHUNK;
            if (newcap - len < CHUNK) newcap = len + CHUNK;
            uint8_t *nb = (uint8_t*)realloc(buf, newcap);
            if (!nb) { free(buf); fclose(f); return -1; }
            buf = nb;
            cap = newcap;
        }
        size_t r = fread(buf + len, 1, CHUNK, f);
        len += r;
        if (r < CHUNK) {
            if (feof(f)) break;
            if (ferror(f)) { free(buf); fclose(f); return -1; }
        }
    }
    fclose(f);

    uint8_t digest[32];
    sha256(buf, len, digest);
    free(buf);

    sha256_to_hex(digest, out_hex);
    return 0;
}
