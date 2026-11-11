#ifndef CDRW_MEDIA_META_H
#define CDRW_MEDIA_META_H

#include <stdint.h>

/*
 * Sidecar metadata for a writable raw optical image (CD-RW/DVD-RW/BD-RE).  It lives next to the
 * image as "<image>.meta" and is intentionally tiny, fixed-layout and
 * little-endian so the Python tooling can produce exactly the same format.
 *
 * The image itself contains only 2048-byte user sectors.  Metadata never
 * changes the exposed optical capacity.
 */
#define CDRW_META_MAGIC "RPCDRW01"
#define CDRW_META_VERSION 2u
#define CDRW_META_SIZE 48u

enum {
    CDRW_META_FLAG_BLANK = 1u << 0,
    CDRW_META_FLAG_COMPLETE = 1u << 1,
    CDRW_META_FLAG_SESSION_OPEN = 1u << 2,
    CDRW_META_FLAG_WRITE_PROTECTED = 1u << 3,
    CDRW_META_FLAG_FORMATTED = 1u << 4,
};

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t flags;
    uint32_t capacity_lba;
    uint32_t data_lba;     /* first LBA after recorded data (lead-out) */
    uint32_t nwa;          /* next writable address */
    uint32_t track_start;
    uint32_t profile;      /* MMC profile: 0x000A CD-RW, 0x0013 DVD-RW, 0x0043 BD-RE */
    uint32_t reserved[3];
    uint32_t crc;          /* CRC32 of the preceding 44 bytes */
} cdrw_meta_t;

#endif
