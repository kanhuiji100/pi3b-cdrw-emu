#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef CDRW_HOST_TEST
#include <time.h>
#else
#include "pico/time.h"
#endif

uint32_t util_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint16_t util_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t util_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void util_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void util_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void util_put_be24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

uint32_t util_div_ceil(uint32_t n, uint32_t d)
{
    return d ? (n / d + ((n % d) ? 1u : 0u)) : 0;
}

void util_format_size(uint64_t bytes, char *out, size_t out_size)
{
    if (bytes >= (1024ull * 1024ull * 1024ull)) {
        snprintf(out, out_size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (1024ull * 1024ull)) {
        snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        snprintf(out, out_size, "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_size, "%u B", (unsigned)bytes);
    }
}

void util_format_lba_msf(uint32_t lba, uint8_t out[3])
{
    uint32_t x = lba + 150; /* CD lead-in offset */
    uint32_t m = x / (75 * 60);
    uint32_t s = (x / 75) % 60;
    uint32_t f = x % 75;
    out[0] = (uint8_t)m;
    out[1] = (uint8_t)s;
    out[2] = (uint8_t)f;
}

void util_safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst_size) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool util_has_suffix_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) {
        return false;
    }
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) {
        return false;
    }
    const char *a = s + ls - lf;
    for (size_t i = 0; i < lf; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

void util_basename(const char *path, char *out, size_t out_size)
{
    const char *base = path;
    if (!path) {
        if (out_size) out[0] = '\0';
        return;
    }
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    util_safe_copy(out, out_size, base);
}

const char *util_skip_root(const char *path)
{
    if (!path) return "";
    while (*path == '/') path++;
    return path;
}

uint32_t util_millis(void)
{
#ifdef CDRW_HOST_TEST
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#else
    return (uint32_t)(time_us_64() / 1000u);
#endif
}
