#ifndef HASHER_H
#define HASHER_H

#include <stddef.h>

/* Small helpers for hex <-> binary and verification */
int sha256_hex_verify_file(const char *path, const char *expected_hex);
char *sha256_hex_of_file(const char *path); /* caller frees */
char *sha256_hex_of_buffer(const void *buf, size_t len); /* caller frees */

#endif
