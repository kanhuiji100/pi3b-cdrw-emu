#ifndef CDRW_UTIL_H
#define CDRW_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t util_crc32(const void *data, size_t len);
uint16_t util_be16(const uint8_t *p);
uint32_t util_be32(const uint8_t *p);
void util_put_be16(uint8_t *p, uint16_t v);
void util_put_be32(uint8_t *p, uint32_t v);
void util_put_be24(uint8_t *p, uint32_t v);
uint32_t util_div_ceil(uint32_t n, uint32_t d);
void util_format_size(uint64_t bytes, char *out, size_t out_size);
void util_format_lba_msf(uint32_t lba, uint8_t out[3]);
void util_safe_copy(char *dst, size_t dst_size, const char *src);
bool util_has_suffix_ci(const char *s, const char *suffix);
void util_basename(const char *path, char *out, size_t out_size);
const char *util_skip_root(const char *path);
uint32_t util_millis(void);

#endif
