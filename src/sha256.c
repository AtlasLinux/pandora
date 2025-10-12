#define _POSIX_C_SOURCE 200809L
#include "sha256.h"
#include <string.h>
#include <stdint.h>

/* Constants and helper macros for SHA-256 */
static const uint32_t K[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x,n) ((x) >> (n))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x,2)  ^ ROTR(x,13) ^ ROTR(x,22))
#define BSIG1(x) (ROTR(x,6)  ^ ROTR(x,11) ^ ROTR(x,25))
#define SSIG0(x) (ROTR(x,7)  ^ ROTR(x,18) ^ SHR(x,3))
#define SSIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ SHR(x,10))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
        w[t] = (uint32_t)block[t*4] << 24 |
               (uint32_t)block[t*4+1] << 16 |
               (uint32_t)block[t*4+2] << 8 |
               (uint32_t)block[t*4+3];
    }
    for (int t = 16; t < 64; ++t) {
        w[t] = SSIG1(w[t-2]) + w[t-7] + SSIG0(w[t-15]) + w[t-16];
    }
    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];
    uint32_t f = ctx->h[5];
    uint32_t g = ctx->h[6];
    uint32_t h = ctx->h[7];
    for (int t = 0; t < 64; ++t) {
        uint32_t T1 = h + BSIG1(e) + CH(e,f,g) + K[t] + w[t];
        uint32_t T2 = BSIG0(a) + MAJ(a,b,c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void sha256_inc_init(sha256_ctx_t *ctx) {
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->buflen = 0;
    ctx->bitlen = 0;
    memset(ctx->buf, 0, sizeof(ctx->buf));
}

void sha256_inc_update(sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    ctx->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha256_inc_final(sha256_ctx_t *ctx, uint8_t digest[32]) {
    /* append 0x80, pad with zeros, append 64-bit big-endian bitlen */
    uint8_t pad[128];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    size_t padlen;
    if (ctx->buflen < 56) {
        padlen = 56 - ctx->buflen;
    } else {
        padlen = 64 + 56 - ctx->buflen;
    }
    sha256_inc_update(ctx, pad, padlen);
    /* append 64-bit big-endian length */
    uint8_t lenbuf[8];
    uint64_t bitlen_be = ctx->bitlen;
    lenbuf[0] = (uint8_t)(bitlen_be >> 56);
    lenbuf[1] = (uint8_t)(bitlen_be >> 48);
    lenbuf[2] = (uint8_t)(bitlen_be >> 40);
    lenbuf[3] = (uint8_t)(bitlen_be >> 32);
    lenbuf[4] = (uint8_t)(bitlen_be >> 24);
    lenbuf[5] = (uint8_t)(bitlen_be >> 16);
    lenbuf[6] = (uint8_t)(bitlen_be >> 8);
    lenbuf[7] = (uint8_t)(bitlen_be);
    sha256_inc_update(ctx, lenbuf, 8);
    /* produce digest */
    for (int i = 0; i < 8; ++i) {
        digest[i*4 + 0] = (uint8_t)(ctx->h[i] >> 24);
        digest[i*4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i*4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        digest[i*4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void sha256_to_hex_lower(const uint8_t digest[32], char out_hex[65]) {
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex[i*2 + 0] = hexchars[(digest[i] >> 4) & 0xF];
        out_hex[i*2 + 1] = hexchars[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

#ifndef NO_MAIN
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    sha256_ctx_t ctx;
    sha256_inc_init(&ctx);

    unsigned char buf[64*1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_inc_update(&ctx, buf, n);
    }
    if (ferror(f)) {
        perror("read");
        fclose(f);
        return 1;
    }
    fclose(f);

    uint8_t digest[32];
    sha256_inc_final(&ctx, digest);

    char hex[65];
    sha256_to_hex_lower(digest, hex);
    printf("%s\n", hex);
    return 0;
}

#endif