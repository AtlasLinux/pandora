#ifndef SHA256_INC_H
#define SHA256_INC_H
#include <stddef.h>
#include <stdint.h>

/* Incremental SHA-256 API */
typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t buflen;
    uint64_t bitlen;
} sha256_ctx_t;

void sha256_inc_init(sha256_ctx_t *ctx);
void sha256_inc_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_inc_final(sha256_ctx_t *ctx, uint8_t digest[32]);

/* Hex helper */
void sha256_to_hex_lower(const uint8_t digest[32], char out_hex[65]);

#endif /* SHA256_INC_H */
