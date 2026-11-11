#ifndef CDRW_MEDIA_H
#define CDRW_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEDIA_PATH_MAX 300
#define MEDIA_NAME_MAX 96
#define MEDIA_LABEL_MAX 40

typedef enum {
    MEDIA_KIND_NONE = 0,
    MEDIA_KIND_ISO,
    MEDIA_KIND_CDRW,
    MEDIA_KIND_DVDRW,
    MEDIA_KIND_BDRE,
} media_kind_t;

/* MMC current-profile codes. */
#define MEDIA_PROFILE_NONE   0x0000
#define MEDIA_PROFILE_CDROM  0x0008
#define MEDIA_PROFILE_CDRW   0x000A
#define MEDIA_PROFILE_DVDROM 0x0010
#define MEDIA_PROFILE_DVDRW  0x0013  /* restricted overwrite */
#define MEDIA_PROFILE_BDROM  0x0040
#define MEDIA_PROFILE_BDRE   0x0043

typedef struct {
    bool present;
    bool writable;          /* drive can write this medium */
    bool blank;
    bool formatted;         /* formatted-empty CD-RW: capacity valid, no data */
    bool complete;
    bool session_open;
    bool write_protected;
    bool ejected_by_host;
    media_kind_t kind;
    uint16_t profile;
    uint32_t capacity_lba;
    uint32_t nominal_lba;    /* physical/ATIP capacity; TOC lead-out */
    uint32_t data_lba;      /* lead-out position for a single data track */
    uint32_t nwa;
    uint32_t track_start;
    uint32_t generation;
    uint32_t write_count;
    uint32_t read_count;
    char path[MEDIA_PATH_MAX];
    char name[MEDIA_NAME_MAX];
    char label[MEDIA_LABEL_MAX];
} media_status_t;

bool media_init(void);
void media_shutdown(void);

const media_status_t *media_get(void);
bool media_load(const char *path);
bool media_reload(void);
bool media_eject(void);
bool media_is_present(void);

/* Create a new preallocated blank rewritable image and insert it. */
bool media_create_blank(const char *path, uint32_t sectors);
bool media_new_blank(char *out_path, size_t out_path_size);

/* Profile-aware variants.  CD-RW keeps the original API above. */
bool media_is_writable_kind(media_kind_t kind);
const char *media_profile_name(uint16_t profile);
uint16_t media_profile_from_name(const char *name);
uint32_t media_default_capacity_lba_for(uint16_t profile);
void media_set_default_profile(uint16_t profile);
uint16_t media_default_profile(void);
bool media_create_blank_profile(const char *path, uint16_t profile, uint32_t sectors);
bool media_new_blank_profile(char *out_path, size_t out_path_size, uint16_t profile);

/* Data path used by SCSI READ/WRITE. */
bool media_can_write(uint32_t lba, uint32_t blocks);
bool media_read_blocks(uint32_t lba, void *dst, uint32_t blocks);
bool media_write_blocks(uint32_t lba, const void *src, uint32_t blocks);

/* Write blocks whose source sectors may be raw 2352/2336/2056 byte CD
 * sectors.  Only the 2048-byte user data portion is persisted. */
bool media_write_raw_blocks(uint32_t lba, const void *src, uint32_t blocks,
                            uint32_t source_block_size);

bool media_blank(void);
bool media_format(void);
/* FORMAT UNIT with a format descriptor may request a smaller formatted
   capacity (Windows asks for ~574 MiB on a 700 MiB CD-RW).  blocks=0 keeps
   the current capacity. */
bool media_format_capacity(uint32_t blocks);
bool media_is_formatted(void);
/* FORMAT UNIT reports FORMAT IN PROGRESS via TEST UNIT READY for a short
   simulated interval, matching the real drive's async format. */
bool media_is_formatting(void);
bool media_is_erasing(void);
bool media_format_ua_take(void);
bool media_close_session(bool finalize);
bool media_reserve_track(uint32_t lba, uint32_t blocks);
bool media_sync(void);
bool media_save_meta(void);

uint32_t media_default_capacity_lba(void);

#endif
