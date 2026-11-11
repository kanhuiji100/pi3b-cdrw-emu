#include "media.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "ff.h"
#include "media_meta.h"
#include "storage.h"
#include "util.h"

#define MEDIA_SECTOR_SIZE 2048u
#define DEFAULT_CAPACITY_LBA 358400u /* 700 MiB (80 minute CD) */
#define MAX_REASONABLE_LBA 128000000u /* 256 GB at 2 KiB/sector */
#define DECODE_BATCH 16u
#define DVD_CAPACITY_LBA 2295104u   /* 4.7 GB DVD-RW, 2048-byte sectors */
#define BD_CAPACITY_LBA 12219392u   /* 25 GB BD-RE single layer */

static uint16_t s_default_profile = MEDIA_PROFILE_CDRW;
static uint64_t s_format_until_ms;
static uint64_t s_erase_until_ms;
static bool s_format_ua;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

bool media_is_formatting(void)
{
    return s_format_until_ms != 0 && mono_ms() < s_format_until_ms;
}

bool media_is_erasing(void)
{
    return s_erase_until_ms != 0 && mono_ms() < s_erase_until_ms;
}

bool media_format_ua_take(void)
{
    /* Kept as a no-op: the real full-format capture has no 6/28 after
       FORMAT UNIT; TUR is simply GOOD and Windows reads Track. */
    s_format_ua = false;
    return false;
}

bool media_is_writable_kind(media_kind_t kind)
{
    return kind == MEDIA_KIND_CDRW || kind == MEDIA_KIND_DVDRW ||
           kind == MEDIA_KIND_BDRE;
}

static media_kind_t kind_for_profile(uint16_t profile)
{
    switch (profile) {
    case MEDIA_PROFILE_DVDRW: return MEDIA_KIND_DVDRW;
    case MEDIA_PROFILE_BDRE: return MEDIA_KIND_BDRE;
    case MEDIA_PROFILE_CDRW: return MEDIA_KIND_CDRW;
    default: return MEDIA_KIND_NONE;
    }
}

static const char *extension_for_profile(uint16_t profile)
{
    switch (profile) {
    case MEDIA_PROFILE_DVDRW: return ".DVD";
    case MEDIA_PROFILE_BDRE: return ".BD";
    default: return ".CDR";
    }
}

const char *media_profile_name(uint16_t profile)
{
    switch (profile) {
    case MEDIA_PROFILE_CDROM: return "CD-ROM";
    case MEDIA_PROFILE_CDRW: return "CD-RW";
    case MEDIA_PROFILE_DVDROM: return "DVD-ROM";
    case MEDIA_PROFILE_DVDRW: return "DVD-RW";
    case MEDIA_PROFILE_BDROM: return "BD-ROM";
    case MEDIA_PROFILE_BDRE: return "BD-RE";
    default: return "none";
    }
}

uint16_t media_profile_from_name(const char *name)
{
    if (!name) return MEDIA_PROFILE_NONE;
    if (!strcasecmp(name, "cd-rw") || !strcasecmp(name, "cdrw") ||
        !strcasecmp(name, "cd")) return MEDIA_PROFILE_CDRW;
    if (!strcasecmp(name, "dvd-rw") || !strcasecmp(name, "dvdrw") ||
        !strcasecmp(name, "dvd")) return MEDIA_PROFILE_DVDRW;
    if (!strcasecmp(name, "bd-re") || !strcasecmp(name, "bdre") ||
        !strcasecmp(name, "bd")) return MEDIA_PROFILE_BDRE;
    return MEDIA_PROFILE_NONE;
}

uint32_t media_default_capacity_lba_for(uint16_t profile)
{
    switch (profile) {
    case MEDIA_PROFILE_DVDRW: return DVD_CAPACITY_LBA;
    case MEDIA_PROFILE_BDRE: return BD_CAPACITY_LBA;
    default: return DEFAULT_CAPACITY_LBA;
    }
}

void media_set_default_profile(uint16_t profile)
{
    if (profile == MEDIA_PROFILE_CDRW || profile == MEDIA_PROFILE_DVDRW ||
        profile == MEDIA_PROFILE_BDRE) {
        s_default_profile = profile;
    }
}

uint16_t media_default_profile(void)
{
    return s_default_profile;
}

static media_status_t s_status;
static FIL s_file;
static bool s_file_open;
static bool s_pos_valid;
static char s_reload_path[MEDIA_PATH_MAX];
static FSIZE_t s_pos;
static bool s_meta_dirty;
static uint8_t s_scratch[MEDIA_SECTOR_SIZE];
static uint8_t s_decode[DECODE_BATCH * MEDIA_SECTOR_SIZE];

static bool seek_file(FSIZE_t offset)
{
    if (!s_file_open) {
        return false;
    }
    if (s_pos_valid && s_pos == offset) {
        return true;
    }
    if (f_lseek(&s_file, offset) != FR_OK) {
        s_pos_valid = false;
        return false;
    }
    s_pos = offset;
    s_pos_valid = true;
    return true;
}

static void set_name_from_path(const char *path)
{
    char base[MEDIA_NAME_MAX];
    util_basename(path, base, sizeof(base));
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
    }
    util_safe_copy(s_status.name, sizeof(s_status.name), base);
    util_safe_copy(s_status.label, sizeof(s_status.label), base);
}

static void meta_path_for(const char *image, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s.meta", image);
}

static bool read_iso_label(uint32_t at_lba, char out[MEDIA_LABEL_MAX])
{
    if (!seek_file((FSIZE_t)at_lba * MEDIA_SECTOR_SIZE)) {
        return false;
    }
    UINT br = 0;
    FRESULT fr = f_read(&s_file, s_scratch, MEDIA_SECTOR_SIZE, &br);
    s_pos_valid = false; /* f_read moved the file pointer behind our back */
    if (fr != FR_OK || br < 96) {
        return false;
    }
    char label[33];
    memcpy(label, s_scratch + 40, 32);
    label[32] = '\0';
    for (int i = 31; i >= 0; i--) {
        if (label[i] == ' ' || label[i] == '\0') {
            label[i] = '\0';
        } else {
            break;
        }
    }
    if (label[0]) {
        util_safe_copy(out, MEDIA_LABEL_MAX, label);
    }
    return true;
}

static bool load_meta(void)
{
    char path[MEDIA_PATH_MAX + 8];
    meta_path_for(s_status.path, path, sizeof(path));
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        return false;
    }
    cdrw_meta_t meta;
    UINT br = 0;
    FRESULT fr = f_read(&f, &meta, sizeof(meta), &br);
    f_close(&f);
    if (fr != FR_OK || br != sizeof(meta)) {
        return false;
    }
    if (memcmp(meta.magic, CDRW_META_MAGIC, 8) != 0 ||
        (meta.version != 1 && meta.version != CDRW_META_VERSION) ||
        meta.crc != util_crc32(&meta, offsetof(cdrw_meta_t, crc))) {
        return false;
    }
    if (meta.version >= 2) {
        uint16_t profile = (uint16_t)meta.profile;
        if (profile == MEDIA_PROFILE_CDRW || profile == MEDIA_PROFILE_DVDRW ||
            profile == MEDIA_PROFILE_BDRE) {
            s_status.profile = profile;
            s_status.kind = kind_for_profile(profile);
        }
    }
    if (meta.capacity_lba == 0 || meta.capacity_lba > MAX_REASONABLE_LBA) {
        return false;
    }
    s_status.capacity_lba = meta.capacity_lba;
    s_status.blank = (meta.flags & CDRW_META_FLAG_BLANK) != 0;
    s_status.formatted = (meta.flags & CDRW_META_FLAG_FORMATTED) != 0;
    s_status.complete = (meta.flags & CDRW_META_FLAG_COMPLETE) != 0 && !s_status.blank;
    s_status.session_open = (meta.flags & CDRW_META_FLAG_SESSION_OPEN) != 0;
    s_status.write_protected = (meta.flags & CDRW_META_FLAG_WRITE_PROTECTED) != 0;
    s_status.data_lba = meta.data_lba;
    s_status.nwa = meta.nwa;
    s_status.track_start = meta.track_start;
    if (s_status.data_lba > s_status.capacity_lba) {
        s_status.data_lba = s_status.capacity_lba;
    }
    if (s_status.nwa > s_status.capacity_lba) {
        s_status.nwa = s_status.capacity_lba;
    }
    if (s_status.track_start > s_status.capacity_lba) {
        s_status.track_start = 0;
    }
    if (s_status.blank) {
        s_status.data_lba = 0;
        s_status.nwa = 0;
        s_status.track_start = 0;
    }
    return true;
}

static bool infer_cdrw_state(void)
{
    /* A fresh image is full of zeros and has no sidecar. */
    s_status.blank = true;
    s_status.complete = false;
    s_status.session_open = false;
    s_status.data_lba = 0;
    s_status.nwa = 0;
    s_status.track_start = 0;

    /* ISO9660 PVD at sector 16. */
    if (seek_file(16ull * MEDIA_SECTOR_SIZE)) {
        UINT br = 0;
        FRESULT fr = f_read(&s_file, s_scratch, MEDIA_SECTOR_SIZE, &br);
        s_pos_valid = false;
        if (fr == FR_OK && br >= 88 &&
            memcmp(s_scratch + 1, "CD001", 5) == 0) {
            uint32_t vol_blocks = (uint32_t)s_scratch[80] |
                                  ((uint32_t)s_scratch[81] << 8) |
                                  ((uint32_t)s_scratch[82] << 16) |
                                  ((uint32_t)s_scratch[83] << 24);
            s_status.blank = false;
            s_status.complete = true;
            s_status.data_lba = vol_blocks ? vol_blocks : s_status.capacity_lba;
            if (s_status.data_lba > s_status.capacity_lba) {
                s_status.data_lba = s_status.capacity_lba;
            }
            s_status.nwa = s_status.data_lba;
            read_iso_label(16, s_status.label);
            return true;
        }
        /* UDF volume recognition sequence. */
        if (br >= 6 && memcmp(s_scratch + 1, "BEA01", 5) == 0) {
            s_status.blank = false;
            s_status.complete = true;
            s_status.data_lba = s_status.capacity_lba;
            s_status.nwa = s_status.capacity_lba;
            util_safe_copy(s_status.label, sizeof(s_status.label), "UDF DATA");
            return true;
        }
    }

    /* Any non-zero first sector is treated as pre-recorded raw media.  This
       covers audio discs or other non-ISO payloads imported by the host tool. */
    if (seek_file(0)) {
        UINT br = 0;
        FRESULT fr = f_read(&s_file, s_scratch, MEDIA_SECTOR_SIZE, &br);
        s_pos_valid = false;
        if (fr == FR_OK) {
            bool nonzero = false;
            for (UINT i = 0; i < br; i++) {
                if (s_scratch[i]) {
                    nonzero = true;
                    break;
                }
            }
            if (nonzero) {
                s_status.blank = false;
                s_status.complete = true;
                s_status.data_lba = s_status.capacity_lba;
                s_status.nwa = s_status.data_lba;
                util_safe_copy(s_status.label, sizeof(s_status.label), "RAW DATA");
                return true;
            }
        }
    }
    return false;
}

static void close_file(void)
{
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = false;
    }
    s_pos_valid = false;
}

bool media_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_file_open = false;
    s_pos_valid = false;
    s_meta_dirty = false;
    return true;
}

void media_shutdown(void)
{
    if (s_status.present) {
        media_sync();
    }
    close_file();
    s_status.present = false;
}

const media_status_t *media_get(void)
{
    return &s_status;
}

bool media_is_present(void)
{
    return s_status.present;
}

bool media_load(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    media_sync();
    close_file();

    bool is_iso = util_has_suffix_ci(path, ".iso");
    bool is_cdrw = util_has_suffix_ci(path, ".cdr");
    bool is_dvd = util_has_suffix_ci(path, ".dvd");
    bool is_bd = util_has_suffix_ci(path, ".bd");
    if (!is_iso && !is_cdrw && !is_dvd && !is_bd) {
        return false;
    }

    media_status_t old = s_status;
    memset(&s_status, 0, sizeof(s_status));
    util_safe_copy(s_status.path, sizeof(s_status.path), path);
    set_name_from_path(path);
    s_status.generation = old.generation + 1;
    s_status.kind = is_iso ? MEDIA_KIND_ISO :
                    is_dvd ? MEDIA_KIND_DVDRW :
                    is_bd ? MEDIA_KIND_BDRE : MEDIA_KIND_CDRW;
    s_status.profile = is_iso ? MEDIA_PROFILE_CDROM :
                       is_dvd ? MEDIA_PROFILE_DVDRW :
                       is_bd ? MEDIA_PROFILE_BDRE : MEDIA_PROFILE_CDRW;
    s_status.writable = !is_iso;
    s_status.write_protected = false;

    FRESULT fr = f_open(&s_file, path, is_iso ? FA_READ : (FA_READ | FA_WRITE));
    if (fr != FR_OK) {
        LOGF("media: open %s failed FRESULT=%d", path, (int)fr);
        memset(&s_status, 0, sizeof(s_status));
        return false;
    }
    LOGF("media: opened %s (%lu bytes)", path, (unsigned long)f_size(&s_file));
    util_safe_copy(s_reload_path, sizeof(s_reload_path), path);
    s_file_open = true;
    s_pos_valid = false;
    uint64_t bytes = f_size(&s_file);
    uint32_t sectors = (uint32_t)(bytes / MEDIA_SECTOR_SIZE);
    s_status.capacity_lba = sectors;
    s_status.nominal_lba = sectors;
    s_status.data_lba = sectors;

    if (is_iso) {
        s_status.present = true;
        s_status.complete = true;
        s_status.blank = false;
        s_status.nwa = sectors;
        s_status.write_protected = true;
        if (!read_iso_label(16, s_status.label)) {
            util_safe_copy(s_status.label, sizeof(s_status.label), s_status.name);
        }
        s_status.session_open = false;
        s_meta_dirty = false;
        LOGF("media: ISO ready cap=%lu sectors", (unsigned long)s_status.capacity_lba);
        return true;
    }

    bool have_meta = load_meta();
    if (!have_meta) {
        if (s_status.capacity_lba == 0) {
            close_file();
            memset(&s_status, 0, sizeof(s_status));
            return false;
        }
        infer_cdrw_state();
        s_meta_dirty = true;
    }

    if (s_status.capacity_lba == 0) {
        s_status.capacity_lba = sectors ? sectors : DEFAULT_CAPACITY_LBA;
    }
    if (s_status.data_lba > s_status.capacity_lba) {
        s_status.data_lba = s_status.capacity_lba;
    }
    if (!s_status.blank && s_status.data_lba == 0) {
        s_status.data_lba = sectors ? sectors : s_status.capacity_lba;
    }
    if (s_status.nwa == 0 && !s_status.blank) {
        s_status.nwa = s_status.data_lba;
    }
    s_status.present = true;
    s_status.session_open = s_status.session_open && !s_status.blank && !s_status.complete;

    if (s_meta_dirty) {
        media_save_meta();
    }
    LOGF("media: %s %s cap=%lu data=%lu nwa=%lu meta=%s",
         media_profile_name(s_status.profile),
         s_status.blank ? "BLANK" : (s_status.complete ? "READY" : "INCOMPLETE"),
         (unsigned long)s_status.capacity_lba, (unsigned long)s_status.data_lba,
         (unsigned long)s_status.nwa, have_meta ? "yes" : "inferred");
    return true;
}

bool media_reload(void)
{
    if (s_reload_path[0] == '\0') {
        return false;
    }
    return media_load(s_reload_path);
}

bool media_eject(void)
{
    if (!s_status.present) {
        return false;
    }
    media_sync();
    close_file();
    s_status.present = false;
    s_status.ejected_by_host = true;
    s_status.generation++;
    s_status.kind = MEDIA_KIND_NONE;
    s_status.profile = MEDIA_PROFILE_NONE;
    return true;
}

static bool meta_write_for_current(void)
{
    char path[MEDIA_PATH_MAX + 8];
    if (s_status.path[0] == '\0') {
        return false;
    }
    meta_path_for(s_status.path, path, sizeof(path));
    if (strlen(path) >= sizeof(path)) {
        return false;
    }

    cdrw_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    memcpy(meta.magic, CDRW_META_MAGIC, 8);
    meta.version = CDRW_META_VERSION;
    meta.profile = s_status.profile;
    meta.flags = 0;
    if (s_status.blank) meta.flags |= CDRW_META_FLAG_BLANK;
    if (s_status.formatted) meta.flags |= CDRW_META_FLAG_FORMATTED;
    if (s_status.complete) meta.flags |= CDRW_META_FLAG_COMPLETE;
    if (s_status.session_open) meta.flags |= CDRW_META_FLAG_SESSION_OPEN;
    if (s_status.write_protected) meta.flags |= CDRW_META_FLAG_WRITE_PROTECTED;
    meta.capacity_lba = s_status.capacity_lba;
    meta.data_lba = s_status.data_lba;
    meta.nwa = s_status.nwa;
    meta.track_start = s_status.track_start;
    meta.crc = util_crc32(&meta, offsetof(cdrw_meta_t, crc));

    FIL f;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return false;
    }
    UINT bw = 0;
    FRESULT fr = f_write(&f, &meta, sizeof(meta), &bw);
    if (fr == FR_OK) {
        fr = f_sync(&f);
    }
    f_close(&f);
    if (fr == FR_OK && bw == sizeof(meta)) {
        s_meta_dirty = false;
        return true;
    }
    LOGF("media: meta save failed FR=%d bw=%u", (int)fr, (unsigned)bw);
    return false;
}

bool media_save_meta(void)
{
    if (!s_status.present || !media_is_writable_kind(s_status.kind)) {
        return false;
    }
    return meta_write_for_current();
}

bool media_sync(void)
{
    if (!s_file_open) {
        return false;
    }
    bool ok = f_sync(&s_file) == FR_OK;
    if (media_is_writable_kind(s_status.kind) && s_meta_dirty) {
        ok = media_save_meta() && ok;
    }
    return ok;
}

uint32_t media_default_capacity_lba(void)
{
    return DEFAULT_CAPACITY_LBA;
}

static bool create_and_blank_profile(const char *path, uint16_t profile,
                                     uint32_t sectors)
{
    bool sparse = profile != MEDIA_PROFILE_CDRW;
    if (!storage_create_blank_image_ex(path, sectors, sparse)) {
        return false;
    }
    s_status.path[0] = '\0';
    if (!media_load(path)) {
        return false;
    }
    s_status.profile = profile;
    s_status.kind = kind_for_profile(profile);
    s_status.writable = true;
    s_status.capacity_lba = sectors;
    s_status.nominal_lba = sectors;
    s_status.blank = true;
    s_status.formatted = false;
    s_status.complete = false;
    s_status.session_open = false;
    s_status.data_lba = 0;
    s_status.nwa = 0;
    s_status.track_start = 0;
    s_meta_dirty = true;
    LOGF("media: created %s image %s cap=%lu", media_profile_name(profile),
         path, (unsigned long)sectors);
    return media_save_meta();
}

bool media_create_blank_profile(const char *path, uint16_t profile,
                                uint32_t sectors)
{
    if (!path || !sectors) {
        return false;
    }
    if (profile != MEDIA_PROFILE_CDRW && profile != MEDIA_PROFILE_DVDRW &&
        profile != MEDIA_PROFILE_BDRE) {
        return false;
    }
    if (s_status.present) {
        media_eject();
    }
    return create_and_blank_profile(path, profile, sectors);
}

bool media_create_blank(const char *path, uint32_t sectors)
{
    return media_create_blank_profile(path, MEDIA_PROFILE_CDRW, sectors);
}

bool media_new_blank_profile(char *out_path, size_t out_path_size,
                             uint16_t profile)
{
    char path[STORAGE_PATH_MAX];
    uint32_t sectors = media_default_capacity_lba_for(profile);
    if (!storage_new_disc_path_ext(extension_for_profile(profile), path,
                                   sizeof(path))) {
        return false;
    }
    if (!media_create_blank_profile(path, profile, sectors)) {
        return false;
    }
    if (out_path && out_path_size) {
        util_safe_copy(out_path, out_path_size, path);
    }
    return true;
}

bool media_new_blank(char *out_path, size_t out_path_size)
{
    return media_new_blank_profile(out_path, out_path_size, s_default_profile);
}

bool media_can_write(uint32_t lba, uint32_t blocks)
{
    if (!s_status.present || !s_status.writable || s_status.write_protected) {
        return false;
    }
    if (blocks == 0 || lba >= s_status.capacity_lba) {
        return false;
    }
    return (uint64_t)lba + blocks <= s_status.capacity_lba;
}

bool media_read_blocks(uint32_t lba, void *dst, uint32_t blocks)
{
    if (!s_status.present || !dst || blocks == 0) {
        return false;
    }
    if ((uint64_t)lba + blocks > s_status.capacity_lba) {
        return false;
    }
    uint8_t *out = (uint8_t *)dst;
    uint64_t bytes = (uint64_t)blocks * MEDIA_SECTOR_SIZE;

    if (media_is_writable_kind(s_status.kind) &&
        (s_status.blank || lba >= s_status.data_lba)) {
        memset(out, 0, (size_t)bytes);
        s_status.read_count++;
        return true;
    }

    if (!seek_file((FSIZE_t)lba * MEDIA_SECTOR_SIZE)) {
        return false;
    }
    UINT br = 0;
    FRESULT fr = f_read(&s_file, out, (UINT)bytes, &br);
    s_pos = (FSIZE_t)lba * MEDIA_SECTOR_SIZE + br;
    s_pos_valid = true;
    if (fr != FR_OK) {
        return false;
    }
    if (br < bytes) {
        memset(out + br, 0, (size_t)(bytes - br));
    }
    s_status.read_count++;
    return true;
}

bool media_write_blocks(uint32_t lba, const void *src, uint32_t blocks)
{
    if (!media_can_write(lba, blocks) || !src) {
        return false;
    }
    if (!seek_file((FSIZE_t)lba * MEDIA_SECTOR_SIZE)) {
        LOGF("media: write seek failed lba=%lu", (unsigned long)lba);
        return false;
    }
    UINT bw = 0;
    UINT bytes = blocks * MEDIA_SECTOR_SIZE;
    FRESULT fr = f_write(&s_file, src, bytes, &bw);
    s_pos = (FSIZE_t)lba * MEDIA_SECTOR_SIZE + bw;
    s_pos_valid = true;
    if (fr != FR_OK || bw != bytes) {
        LOGF("media: f_write failed lba=%lu blocks=%lu FR=%d bw=%u/%u",
             (unsigned long)lba, (unsigned long)blocks, (int)fr,
             (unsigned)bw, (unsigned)bytes);
        return false;
    }

    if (s_status.blank) {
        s_status.blank = false;
        s_status.track_start = 0;
    }
    s_status.complete = false;
    s_status.session_open = true;
    uint32_t end = lba + blocks;
    if (end > s_status.data_lba) {
        s_status.data_lba = end;
    }
    s_status.nwa = s_status.data_lba;
    s_status.write_count++;
    s_meta_dirty = true;
    return true;
}

static bool decode_source_block(uint32_t source_size, const uint8_t *src,
                                uint8_t out[MEDIA_SECTOR_SIZE])
{
    switch (source_size) {
    case 2048:
        memcpy(out, src, MEDIA_SECTOR_SIZE);
        return true;
    case 2352:
    case 2368:
    case 2448: {
        /* 2352-byte raw data sector: sync(12)+header(4)+data... */
        uint8_t mode = src[15];
        uint32_t off = (mode & 0x20) ? 24 : 16;
        memcpy(out, src + off, MEDIA_SECTOR_SIZE);
        return true;
    }
    case 2336:
    case 2324:
        memcpy(out, src, MEDIA_SECTOR_SIZE);
        return true;
    case 2056:
        memcpy(out, src + 8, MEDIA_SECTOR_SIZE);
        return true;
    case 2332:
        memcpy(out, src + 8, MEDIA_SECTOR_SIZE);
        return true;
    default:
        return false;
    }
}

bool media_write_raw_blocks(uint32_t lba, const void *src, uint32_t blocks,
                            uint32_t source_block_size)
{
    if (!media_can_write(lba, blocks) || !src || blocks == 0) {
        return false;
    }
    if (source_block_size == MEDIA_SECTOR_SIZE) {
        return media_write_blocks(lba, src, blocks);
    }
    const uint8_t *in = (const uint8_t *)src;
    uint32_t done = 0;
    while (done < blocks) {
        uint32_t batch = blocks - done;
        if (batch > DECODE_BATCH) {
            batch = DECODE_BATCH;
        }
        for (uint32_t i = 0; i < batch; i++) {
            if (!decode_source_block(source_block_size,
                                     in + (done + i) * source_block_size,
                                     s_decode + i * MEDIA_SECTOR_SIZE)) {
                return false;
            }
        }
        if (!media_write_blocks(lba + done, s_decode, batch)) {
            return false;
        }
        done += batch;
    }
    return true;
}

bool media_blank(void)
{
    if (!s_status.present || !s_status.writable || s_status.write_protected) {
        return false;
    }
    if (!s_file_open) {
        return false;
    }
    (void)f_sync(&s_file);
    if (!storage_zero_file_edges(&s_file, (uint64_t)s_status.capacity_lba * 2048ull)) {
        LOGF("media: erase zero failed");
        return false;
    }
    s_pos_valid = false;
    s_format_until_ms = 0;
    s_format_ua = false;
    s_erase_until_ms = mono_ms() + 31000u;
    s_status.blank = true;
    s_status.formatted = false;
    s_status.complete = false;
    s_status.session_open = false;
    s_status.data_lba = 0;
    s_status.nwa = 0;
    s_status.track_start = 0;
    s_meta_dirty = true;
    LOGF("media: blank");
    return media_save_meta();
}

bool media_format_capacity(uint32_t blocks)
{
    if (!s_status.present || !s_status.writable || s_status.write_protected) {
        return false;
    }
    uint32_t target = blocks ? blocks : s_status.capacity_lba;
    if (target > s_status.capacity_lba) {
        target = s_status.capacity_lba;
    }
    if (target < 100) {
        return false;
    }
    (void)f_sync(&s_file);
    if (!storage_zero_file_edges(&s_file, (uint64_t)target * 2048ull)) {
        LOGF("media: format zero failed");
        return false;
    }
    s_pos_valid = false;
    s_erase_until_ms = 0;
    s_format_until_ms = mono_ms() + 600000u;
    s_format_ua = false;
    s_status.capacity_lba = target;
    /* FORMAT UNIT on CD-RW leaves an EMPTY (Disc Information state 0),
       formatted disc with a valid capacity.  Marking it appendable
       (session_open=true) makes Windows report INCOMPLETE and abort the
       live-file-system format before any WRITE.  Capacity must still be
       nonzero; READ CAPACITY no longer special-cases blank CD-RW.
       DVD-RW/BD-RE keep the appendable state that growisofs expects. */
    bool cdrw = s_status.kind == MEDIA_KIND_CDRW;
    /* The real successful full-format capture ends with Disc
       Information 0x1E (COMPLETE/state 3) and a complete track 1; only
       then does Windows issue its first WRITE(10).  EMPTY and
       INCOMPLETE both left the live-file-system format stalled. */
    s_status.blank = false;
    s_status.formatted = cdrw;
    s_status.complete = cdrw;
    s_status.session_open = !cdrw;
    s_status.data_lba = 0;
    s_status.nwa = 0;
    s_status.track_start = 0;
    s_meta_dirty = true;
    LOGF("media: %s formatted, %s cap=%lu",
         media_profile_name(s_status.profile),
         s_status.complete ? "complete" : "appendable",
         (unsigned long)target);
    return media_save_meta();
}

bool media_format(void)
{
    return media_format_capacity(0);
}

bool media_is_formatted(void)
{
    return s_status.present && s_status.formatted;
}

bool media_close_session(bool finalize)
{
    if (!s_status.present || !s_status.writable) {
        return false;
    }
    (void)f_sync(&s_file);
    s_status.session_open = false;
    if (finalize) {
        s_status.complete = true;
        s_status.blank = false;
        s_status.nwa = s_status.data_lba;
    }
    s_meta_dirty = true;
    LOGF("media: close session finalize=%d data=%lu", finalize ? 1 : 0,
         (unsigned long)s_status.data_lba);
    return media_save_meta();
}

bool media_reserve_track(uint32_t lba, uint32_t blocks)
{
    (void)blocks;
    if (!s_status.present || !s_status.writable || s_status.write_protected) {
        return false;
    }
    if (lba > s_status.capacity_lba) {
        return false;
    }
    if (s_status.blank) {
        s_status.track_start = lba;
    }
    /* A reserved track means the disc is no longer "empty" from the host's
       point of view: MMC disc status must become Incomplete/Appendable. */
    s_status.blank = false;
    s_status.complete = false;
    s_status.session_open = true;
    s_status.nwa = lba;
    s_meta_dirty = true;
    LOGF("media: reserve track lba=%lu blocks=%lu", (unsigned long)lba,
         (unsigned long)blocks);
    return true;
}
