/*
 * Portable SCSI MMC command subset for a single-session CD-RW/CD-ROM.
 * The command parser has no USB or Pico dependencies so it can also be built
 * and exercised by the host-side unit tests.
 */
#include "scsi.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "media.h"
#include "media_lock.h"
#include "util.h"

#define OP_TEST_UNIT_READY 0x00
#define OP_REQUEST_SENSE 0x03
#define OP_INQUIRY 0x12
#define OP_MODE_SELECT_6 0x15
#define OP_MODE_SENSE_6 0x1A
#define OP_START_STOP_UNIT 0x1B
#define OP_PREVENT_ALLOW 0x1E
#define OP_READ_FORMAT_CAPACITIES 0x23
#define OP_READ_CAPACITY_10 0x25
#define OP_READ_10 0x28
#define OP_WRITE_10 0x2A
#define OP_VERIFY_10 0x2F
#define OP_SYNCHRONIZE_CACHE_10 0x35
#define OP_READ_TOC 0x43
#define OP_READ_HEADER 0x44
#define OP_GET_CONFIGURATION 0x46
#define OP_GET_EVENT_STATUS 0x4A
#define OP_READ_DISC_INFORMATION 0x51
#define OP_READ_TRACK_INFORMATION 0x52
#define OP_RESERVE_TRACK 0x53
#define OP_SEND_OPC 0x54
#define OP_MODE_SELECT_10 0x55
#define OP_MODE_SENSE_10 0x5A
#define OP_CLOSE_TRACK_SESSION 0x5B
#define OP_READ_BUFFER 0x3C
#define OP_READ_BUFFER_CAPACITY 0x5C
#define OP_BLANK 0xA1
#define OP_READ_12 0xA8
#define OP_WRITE_12 0xAA
#define OP_GET_PERFORMANCE 0xAC
#define OP_READ_DISC_STRUCTURE 0xAD
#define OP_VERIFY_12 0xAF
#define OP_SET_CD_SPEED 0xBB
#define OP_MECHANISM_STATUS 0xBD
#define OP_READ_CD 0xBE

#define SENSE_NOT_READY 0x02
#define SENSE_MEDIUM_ERROR 0x03
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION 0x06
#define SENSE_DATA_PROTECT 0x07

#define ASC_INVALID_COMMAND 0x20
#define ASC_LBA_OUT_OF_RANGE 0x21
#define ASC_INVALID_FIELD 0x24
#define ASC_WRITE_PROTECTED 0x27
#define ASC_MEDIUM_CHANGED 0x28
#define ASC_MEDIUM_NOT_PRESENT 0x3A
#define ASC_SAVING_PARAMS_NOT_SUPPORTED 0x39
#define ASC_MEDIUM_REMOVAL_PREVENTED 0x53

#define FEATURE_CORE 0x0001
#define FEATURE_MORPHING 0x0002
#define FEATURE_REMOVABLE 0x0003
#define FEATURE_WRITE_PROTECT 0x0004
#define FEATURE_RANDOM_READABLE 0x0010
#define FEATURE_MULTI_READ 0x001D
#define FEATURE_CD_READ 0x001E
#define FEATURE_INCREMENTAL_WRITE 0x0021
#define FEATURE_FORMATTABLE 0x0023
#define FEATURE_RESTRICTED_OVERWRITE 0x0026
#define FEATURE_CDRW_CAV 0x0027
#define FEATURE_CD_MASTERING 0x002E
#define FEATURE_TRACK_AT_ONCE 0x002D
#define FEATURE_DVD_READ 0x001F
#define FEATURE_RANDOM_WRITABLE 0x0020
#define FEATURE_DVD_RW_WRITE 0x002F
#define FEATURE_CDRW_WRITE 0x0037
#define FEATURE_BD_READ 0x0040
#define FEATURE_BD_WRITE 0x0041

#define PROFILE_CDROM MEDIA_PROFILE_CDROM
#define PROFILE_CDRW MEDIA_PROFILE_CDRW

/* Identity reported in INQUIRY.  The defaults match the RP2350 firmware so
 * its host tests stay meaningful; the Linux port overrides them. */
#ifndef CDRW_INQUIRY_VENDOR
#define CDRW_INQUIRY_VENDOR "RP2350"
#endif
#ifndef CDRW_INQUIRY_PRODUCT
#define CDRW_INQUIRY_PRODUCT "CD-RW USB 1.1"
#endif
#ifndef CDRW_INQUIRY_REV
#define CDRW_INQUIRY_REV "1.00"
#endif
#ifndef CDRW_INQUIRY_T10ID
#define CDRW_INQUIRY_T10ID "RP2350_CDRW_0001"
#endif

static void copy_padded(char *dst, size_t n, const char *src)
{
    memset(dst, ' ', n);
    size_t len = src ? strlen(src) : 0;
    if (len > n) {
        len = n;
    }
    if (len) {
        memcpy(dst, src, len);
    }
}

typedef struct {
    scsi_sense_t sense;
    bool sense_valid;
    bool ua_pending;
    scsi_sense_t ua_sense;
    bool prevent;
    bool event_pending;
    uint8_t event_code;
    uint8_t write_type;
    uint8_t data_block_type;
    uint16_t read_speed_kbs;
    uint16_t write_speed_kbs;
    char serial[24];
} scsi_state_t;

static scsi_state_t s_scsi;
static scsi_write_debug_t s_write_debug;

static bool begin_command(uint8_t opcode)
{
    /* REQUEST SENSE must return the sense data left by the previous command,
       so it is the one command that must not clear it first. */
    if (opcode == OP_REQUEST_SENSE) {
        return true;
    }
    s_scsi.sense_valid = false;
    memset(&s_scsi.sense, 0, sizeof(s_scsi.sense));
    /* Capability/identity queries must remain usable across a media change;
       some burning applications issue GET CONFIGURATION before REQUEST SENSE
       and otherwise display "Current Profile: N/A" forever. */
    bool ua_exempt = opcode == OP_INQUIRY ||
                     opcode == OP_GET_EVENT_STATUS ||
                     opcode == OP_GET_CONFIGURATION ||
                     opcode == OP_MODE_SENSE_6 ||
                     opcode == OP_MODE_SENSE_10;
    if (s_scsi.ua_pending && !ua_exempt) {
        s_scsi.sense = s_scsi.ua_sense;
        s_scsi.sense_valid = true;
        /* Report the unit attention once.  A compliant host follows with
           REQUEST SENSE; a simpler burning app at least recovers on retry. */
        s_scsi.ua_pending = false;
        return false;
    }
    return true;
}

static int32_t fail(uint8_t key, uint8_t asc, uint8_t ascq)
{
    s_scsi.sense.key = key;
    s_scsi.sense.asc = asc;
    s_scsi.sense.ascq = ascq;
    s_scsi.sense_valid = true;
    return -1;
}

static int32_t require_media(void)
{
    const media_status_t *m = media_get();
    if (!m->present) {
        return fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
    }
    return 0;
}

void scsi_init(void)
{
    memset(&s_scsi, 0, sizeof(s_scsi));
    /* The captured DVD-ROM/CD-RW combo reports 24x read and 10x write
       for CD media via GET PERFORMANCE.  Keep the same defaults here so
       MODE SENSE and GET PERFORMANCE agree; SET CD SPEED can override. */
    s_scsi.read_speed_kbs = 24 * 176;
    s_scsi.write_speed_kbs = 10 * 176;
    s_scsi.write_type = 1;       /* Track-at-once */
    s_scsi.data_block_type = 8;  /* 2048-byte Mode 1 user data */
    util_safe_copy(s_scsi.serial, sizeof(s_scsi.serial), "RP2350CDRW0001");
}

void scsi_bot_reset(void)
{
    s_scsi.sense_valid = false;
    memset(&s_scsi.sense, 0, sizeof(s_scsi.sense));
}

void scsi_set_serial(const char *serial)
{
    if (serial) {
        util_safe_copy(s_scsi.serial, sizeof(s_scsi.serial), serial);
    }
}

const scsi_sense_t *scsi_get_sense(void)
{
    return &s_scsi.sense;
}

void scsi_set_sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
    s_scsi.sense.key = key;
    s_scsi.sense.asc = asc;
    s_scsi.sense.ascq = ascq;
    s_scsi.sense_valid = true;
}

const scsi_write_debug_t *scsi_write_debug(void)
{
    return &s_write_debug;
}

bool scsi_ua_pending(void)
{
    return s_scsi.ua_pending;
}

bool scsi_media_locked(void)
{
    return s_scsi.prevent;
}

static void set_ua(uint8_t key, uint8_t asc, uint8_t ascq)
{
    s_scsi.ua_pending = true;
    s_scsi.ua_sense.key = key;
    s_scsi.ua_sense.asc = asc;
    s_scsi.ua_sense.ascq = ascq;
}

void scsi_notify_media_change(bool inserted)
{
    set_ua(SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0);
    scsi_notify_media_event(inserted ? 3 : 4);
}

void scsi_notify_media_event(uint8_t event_code)
{
    s_scsi.event_code = event_code;
    s_scsi.event_pending = true;
}

bool scsi_is_read_command(uint8_t opcode)
{
    return opcode == OP_READ_10 || opcode == OP_READ_12 || opcode == OP_READ_CD;
}

bool scsi_is_write_command(uint8_t opcode)
{
    return opcode == OP_WRITE_10 || opcode == OP_WRITE_12 ||
           opcode == 0x2E || opcode == 0xAE; /* WRITE AND VERIFY */
}

static uint32_t rd10_lba(const uint8_t *cdb)
{
    return util_be32(cdb + 2);
}

static uint32_t rd10_blocks(const uint8_t *cdb)
{
    return util_be16(cdb + 7);
}

bool scsi_read_params(const uint8_t cdb[SCSI_CDB_MAX], uint32_t *lba, uint32_t *blocks)
{
    if (!begin_command(cdb[0])) {
        return false;
    }
    if (cdb[0] != OP_READ_10 && cdb[0] != OP_READ_12 &&
        cdb[0] != OP_READ_CD) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
        return false;
    }
    if (require_media() < 0) {
        return false;
    }
    const media_status_t *m = media_get();
    if (m->blank && !m->formatted && m->data_lba == 0) {
        /* Unformatted blank CD-RW: no user data; real drive returns
           5/21.  A formatted-empty CD-RW accepts reads (zeros) so the
           Windows UDF formatter can proceed. */
        fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return false;
    }
    uint32_t l = rd10_lba(cdb);
    uint32_t n;
    if (cdb[0] == OP_READ_CD) {
        /* Support the common cooked-user-data form.  Main-channel selection
           bits for sync/header/ECC/subchannel are rejected rather than
           returning fabricated raw sectors. */
        uint8_t expected = (cdb[1] >> 2) & 0x07;
        uint8_t select = cdb[9];
        if ((expected != 0 && expected != 2 && expected != 4) ||
            (select & 0xF8) != 0x10 /* only User Data bit set */ ||
            (cdb[10] & 0x07) != 0) {
            fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            return false;
        }
        n = ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8) | cdb[8];
    } else {
        n = cdb[0] == OP_READ_10 ? rd10_blocks(cdb) : util_be32(cdb + 6);
    }
    if (n == 0) {
        *lba = l;
        *blocks = 0;
        return true;
    }
    /* CD lead-in/lead-out reads use virtual addresses outside the user
       area (negative LBA values such as -259, or high addresses such as
       0x37FFFEBE).  Real drives accept these probes.  Return a zero-filled
       cooked sector so IMAPI can finish its post-format verification. */
    if (l >= m->capacity_lba) {
        LOGF("READ lead-in/out lba=%lu blocks=%lu -> zero",
             (unsigned long)l, (unsigned long)n);
        *lba = 0;
        *blocks = n;
        return true;
    }
    if ((uint64_t)l + n > m->capacity_lba) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return false;
    }
    *lba = l;
    *blocks = n;
    return true;
}

bool scsi_write_params(const uint8_t cdb[SCSI_CDB_MAX], uint32_t data_len,
                       uint32_t *lba, uint32_t *blocks, uint32_t *source_block_size,
                       bool *discard)
{
    memset(&s_write_debug, 0, sizeof(s_write_debug));
    if (discard) *discard = false;
    if (!begin_command(cdb[0])) {
        return false;
    }
    if (!scsi_is_write_command(cdb[0])) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
        return false;
    }
    const media_status_t *m = media_get();
    if (!m->present) {
        fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return false;
    }
    if (!m->writable || m->write_protected) {
        fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        return false;
    }
    uint32_t l = rd10_lba(cdb);
    bool is12 = (cdb[0] == OP_WRITE_12 || cdb[0] == 0xAE);
    uint32_t n = is12 ? util_be32(cdb + 6) : rd10_blocks(cdb);
    s_write_debug.lba = l;
    s_write_debug.blocks = n;
    if (n == 0) {
        if (data_len != 0) {
            fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            return false;
        }
        *lba = l;
        *blocks = 0;
        *source_block_size = SCSI_SECTOR_SIZE;
        return true;
    }
    if (data_len == 0 || data_len % n != 0) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        return false;
    }
    uint32_t block_size = data_len / n;
    s_write_debug.source_block_size = block_size;
    switch (block_size) {
    case 2048:
    case 2056:
    case 2324:
    case 2332:
    case 2336:
    case 2352:
    case 2368:
    case 2448:
        break;
    default:
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        return false;
    }
    uint64_t end = (uint64_t)l + n;
    if (end > m->capacity_lba) {
        /* Real CD-RW burners accept lead-in/lead-out writes that lie outside
           the user data area.  We cannot store them, but must consume the
           data and return GOOD so TAO/SAO sessions can proceed. */
        if (l >= m->capacity_lba) {
            s_write_debug.discard = 1;
            if (discard) *discard = true;
            *lba = l;
            *blocks = n;
            *source_block_size = block_size;
            return true;
        }
        fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return false;
    }
    *lba = l;
    *blocks = n;
    *source_block_size = block_size;
    return true;
}

static void put_mode_page(uint8_t *buf, uint32_t *len, uint8_t page)
{
    const media_status_t *m = media_get();
    uint8_t *p = buf + *len;
    switch (page) {
    case 0x01: {
        p[0] = 0x01;
        p[1] = 0x0A;
        memset(p + 2, 0, 10);
        p[2] = 0x80; /* AWRE */
        p[3] = 0x20; /* read retry count */
        *len += 12;
        break;
    }
    case 0x05: {
        p[0] = 0x05;
        p[1] = 0x32;
        memset(p + 2, 0, 0x32);
        uint32_t prof = m->profile ? m->profile : MEDIA_PROFILE_CDROM;
        bool cd = prof == MEDIA_PROFILE_CDROM || prof == MEDIA_PROFILE_CDRW;
        if (cd) {
            /* Captured CD-RW write parameters: packet write type 0,
               track mode 05h, data block 8, link size 0, packet 32. */
            p[2] = 0x40;              /* BUFE | write type 0 */
            p[3] = 0x05;              /* data track mode */
            p[4] = 8;                 /* 2048-byte data block */
            p[5] = 0;                 /* link size */
            p[12] = 0x20;             /* packet size 32 */
            p[13] = 0;
            p[14] = 150;              /* audio pause length */
        } else {
            p[2] = 0x40 | (s_scsi.write_type & 0x0F);
            p[3] = 0x04;
            p[4] = s_scsi.data_block_type;
            p[5] = 7;
            p[13] = 16;
            p[14] = 150;
        }
        *len += 2 + 0x32;
        break;
    }
    case 0x30: {
        p[0] = 0x30;
        p[1] = 0x0E;
        memset(p + 2, 0, 0x0E);
        p[2] = 0x03; /* BUEFS | TWBFS */
        p[3] = 0x01; /* BUEFE enabled */
        *len += 16;
        break;
    }
    case 0x08: {
        p[0] = 0x08;
        p[1] = 0x0A;
        memset(p + 2, 0, 10);
        p[2] = 0x04; /* WCE */
        *len += 12;
        break;
    }
    case 0x0D: {
        p[0] = 0x0D;
        p[1] = 0x06;
        memset(p + 2, 0, 6);
        p[4] = 0;
        p[5] = 60;
        p[6] = 0;
        p[7] = 75;
        *len += 8;
        break;
    }
    case 0x2A: {
        p[0] = 0x2A;
        uint32_t prof = m->profile ? m->profile : MEDIA_PROFILE_CDROM;
        bool prof_cd = prof == MEDIA_PROFILE_CDROM || prof == MEDIA_PROFILE_CDRW;
        bool prof_dvd = prof == MEDIA_PROFILE_DVDROM || prof == MEDIA_PROFILE_DVDRW;
        bool prof_bd = prof == MEDIA_PROFILE_BDROM || prof == MEDIA_PROFILE_BDRE;
        if (prof_cd) {
            /* Captured CD-RW Write Parameters page (2Ah): 46 data bytes,
               including write type, session format, packet size and the
               read/write speed pairs.  The previous 28-byte page was
               missing the second half that Windows/IMAPI parses. */
            static const uint8_t cdrw_params[46] = {
                0x1f, 0x17, 0xf1, 0x77, 0x29, 0x23, 0x10, 0x8a,
                0x01, 0x00, 0x02, 0x00, 0x10, 0x8a, 0x00, 0x10,
                0x06, 0xe5, 0x06, 0xe5, 0x00, 0x01, 0x00, 0x00,
                0x00, 0x00, 0x06, 0xe5, 0x00, 0x01, 0x00, 0x00,
                0x06, 0xe5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            memcpy(p + 2, cdrw_params, sizeof(cdrw_params));
            p[1] = (uint8_t)sizeof(cdrw_params);
            uint16_t rs = s_scsi.read_speed_kbs;
            uint16_t ws = s_scsi.write_speed_kbs;
            p[8] = (uint8_t)(rs >> 8);
            p[9] = (uint8_t)rs;
            p[14] = (uint8_t)(rs >> 8);
            p[15] = (uint8_t)rs;
            p[18] = (uint8_t)(ws >> 8);
            p[19] = (uint8_t)ws;
            p[20] = (uint8_t)(ws >> 8);
            p[21] = (uint8_t)ws;
            p[28] = (uint8_t)(ws >> 8);
            p[29] = (uint8_t)ws;
            *len += 2 + (uint32_t)sizeof(cdrw_params);
            break;
        }
        p[1] = 0x1C; /* 28 bytes */
        memset(p + 2, 0, 28);
        p[2] = 0x00;
        if (prof_dvd || prof_bd) p[2] |= 0x08 | 0x10 | 0x40; /* DVD-ROM/R/RW read */
        p[6] = 0x29; /* tray, eject, lock */
        p[8] = (uint8_t)(s_scsi.read_speed_kbs >> 8);
        p[9] = (uint8_t)s_scsi.read_speed_kbs;
        p[12] = 0;
        p[13] = 2; /* 2 KiB buffer */
        p[14] = (uint8_t)(s_scsi.read_speed_kbs >> 8);
        p[15] = (uint8_t)s_scsi.read_speed_kbs;
        p[16] = (uint8_t)(s_scsi.write_speed_kbs >> 8);
        p[17] = (uint8_t)s_scsi.write_speed_kbs;
        p[18] = (uint8_t)(s_scsi.write_speed_kbs >> 8);
        p[19] = (uint8_t)s_scsi.write_speed_kbs;
        p[28] = (uint8_t)(s_scsi.write_speed_kbs >> 8);
        p[29] = (uint8_t)s_scsi.write_speed_kbs;
        *len += 30;
        break;
    }
    default:
        break;
    }
}

static int32_t mode_sense(const uint8_t *cdb, uint8_t *buf, uint32_t cap, bool ten)
{
    const media_status_t *m = media_get();
    uint8_t page = cdb[2] & 0x3F;
    uint8_t pc = (cdb[2] >> 6) & 0x03;
    if (pc == 3) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_SAVING_PARAMS_NOT_SUPPORTED, 0);
    }
    memset(buf, 0, cap < 256 ? cap : 256);
    if (ten && m->present && m->kind == MEDIA_KIND_CDRW) {
        buf[2] = 0x20; /* captured CD-RW medium type */
    }
    uint32_t header = ten ? 8 : 4;
    uint32_t len = header;
    bool wp = !m->present || !m->writable || m->write_protected;
    if (ten) {
        buf[3] = wp ? 0x80 : 0x00;
    } else {
        buf[2] = wp ? 0x80 : 0x00;
    }

    if (page == 0x3F) {
        static const uint8_t pages[] = {0x01, 0x05, 0x08, 0x0D, 0x2A};
        for (size_t i = 0; i < sizeof(pages); i++) {
            if (len + 64 >= cap) break;
            put_mode_page(buf, &len, pages[i]);
        }
    } else {
        if (page != 0x01 && page != 0x05 && page != 0x08 &&
            page != 0x0D && page != 0x2A && page != 0x30) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        if (len + 64 >= cap) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        put_mode_page(buf, &len, page);
    }

    if (ten) {
        buf[0] = (uint8_t)((len - 2) >> 8);
        buf[1] = (uint8_t)(len - 2);
    } else {
        if (len > 256) len = 256;
        buf[0] = (uint8_t)(len - 1);
    }
    return (int32_t)len;
}

static int32_t inquiry(const uint8_t *cdb, uint8_t *buf, uint32_t cap)
{
    bool evpd = (cdb[1] & 1) != 0;
    uint8_t page = cdb[2];
    uint32_t alloc = util_be16(cdb + 3);
    (void)cap;
    if (!evpd) {
        if (page != 0) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        memset(buf, 0, 36);
        buf[0] = 0x05; /* peripheral qualifier 0, device type CD-ROM */
        buf[1] = 0x80; /* removable */
        buf[2] = 0x05; /* SPC-3 */
        buf[3] = 0x02;
        buf[4] = 31;
        copy_padded((char *)buf + 8, 8, CDRW_INQUIRY_VENDOR);
        copy_padded((char *)buf + 16, 16, CDRW_INQUIRY_PRODUCT);
        copy_padded((char *)buf + 32, 4, CDRW_INQUIRY_REV);
        return alloc < 36 ? (int32_t)alloc : 36;
    }

    if (page == 0x00) {
        uint8_t pages[] = {0x00, 0x80, 0x83};
        buf[0] = 0x05;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = (uint8_t)(sizeof(pages) + 1);
        memcpy(buf + 4, pages, sizeof(pages));
        uint32_t n = 4 + sizeof(pages);
        return alloc < n ? (int32_t)alloc : (int32_t)n;
    }
    if (page == 0x80) {
        size_t slen = strlen(s_scsi.serial);
        if (slen > 32) slen = 32;
        buf[0] = 0x05;
        buf[1] = 0x80;
        buf[2] = 0x00;
        buf[3] = (uint8_t)slen;
        memset(buf + 4, 0, 32);
        memcpy(buf + 4, s_scsi.serial, slen);
        uint32_t n = 4 + (uint32_t)slen;
        return alloc < n ? (int32_t)alloc : (int32_t)n;
    }
    if (page == 0x83) {
        /* One T10 vendor identification designator. */
        const char *id = CDRW_INQUIRY_T10ID;
        uint32_t idlen = (uint32_t)strlen(id);
        buf[0] = 0x05;
        buf[1] = 0x83;
        buf[2] = 0;
        buf[3] = (uint8_t)(4 + idlen);
        buf[4] = 0x02; /* ASCII */
        buf[5] = 0x01; /* T10 vendor ID */
        buf[6] = 0x00;
        buf[7] = (uint8_t)idlen;
        memcpy(buf + 8, id, idlen);
        uint32_t n = 8 + idlen;
        return alloc < n ? (int32_t)alloc : (int32_t)n;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
}

static uint32_t current_profile(void)
{
    const media_status_t *m = media_get();
    if (!m->present) {
        return 0;
    }
    return m->profile ? m->profile : MEDIA_PROFILE_CDROM;
}

static bool profile_is_cd(uint32_t p)
{
    return p == MEDIA_PROFILE_CDROM || p == MEDIA_PROFILE_CDRW;
}

static bool profile_is_dvd(uint32_t p)
{
    return p == MEDIA_PROFILE_DVDROM || p == MEDIA_PROFILE_DVDRW;
}

static bool profile_is_bd(uint32_t p)
{
    return p == MEDIA_PROFILE_BDROM || p == MEDIA_PROFILE_BDRE;
}

static void feature_begin(uint8_t *buf, uint32_t *len, uint16_t code,
                          uint8_t version, bool persistent, bool current,
                          uint8_t add_len)
{
    uint8_t *p = buf + *len;
    p[0] = (uint8_t)(code >> 8);
    p[1] = (uint8_t)code;
    p[2] = (uint8_t)((version << 2) | (persistent ? 2 : 0) | (current ? 1 : 0));
    p[3] = add_len;
    *len += 4 + add_len;
}

static int32_t get_configuration(const uint8_t *cdb, uint8_t *buf, uint32_t cap)
{
    const media_status_t *m = media_get();
    bool has_media = m->present;
    uint32_t profile = current_profile();
    bool writable = has_media && m->writable && !m->write_protected;
    bool cd = profile_is_cd(profile);
    bool dvd = profile_is_dvd(profile);
    bool bd = profile_is_bd(profile);
    bool cdrw = profile == MEDIA_PROFILE_CDRW;
    uint16_t start_feature = util_be16(cdb + 2);
    uint8_t body[512];
    uint32_t len = 0;
    memset(body, 0, sizeof(body));

#define WANT(feature) ((start_feature) == 0 || (feature) >= (start_feature))
    if (WANT(0x0000)) {
        /* Do not advertise BD-ROM/BD-RE when a CD/DVD is inserted.
           Windows otherwise labels the drive "BD-ROM" and tries a BD-RE
           live-file-system format on CD-RW media; the real DVD-RW capture
           has no BD profiles.  BD media still exposes all six. */
        uint16_t profiles[6];
        size_t pn = 0;
        profiles[pn++] = MEDIA_PROFILE_CDROM;
        profiles[pn++] = MEDIA_PROFILE_CDRW;
        profiles[pn++] = MEDIA_PROFILE_DVDROM;
        profiles[pn++] = MEDIA_PROFILE_DVDRW;
        if (bd) {
            profiles[pn++] = MEDIA_PROFILE_BDROM;
            profiles[pn++] = MEDIA_PROFILE_BDRE;
        }
        uint8_t add = (uint8_t)(pn * 4);
        feature_begin(body, &len, 0x0000, 0, true, true, add);
        uint8_t *p = body + len - add;
        for (size_t i = 0; i < pn; i++) {
            util_put_be16(p, profiles[i]);
            p[2] = (profiles[i] == profile) ? 1 : 0;
            p[3] = 0;
            p += 4;
        }
    }
    if (WANT(FEATURE_CORE)) {
        feature_begin(body, &len, FEATURE_CORE, 2, true, true, 8);
        uint8_t *p = body + len - 8;
        /* Same capability bits as the captured DVD-ROM/CD-RW combo. */
        p[3] = 0x07;
        p[4] = 0x01;
    }
    if (WANT(FEATURE_MORPHING)) {
        feature_begin(body, &len, FEATURE_MORPHING, 1, true, true, 4);
        body[len - 4] = 0x02;
    }
    if (WANT(FEATURE_REMOVABLE)) {
        feature_begin(body, &len, FEATURE_REMOVABLE, 2, true, true, 4);
        body[len - 4] = 0x2b; /* tray, eject, lock, prevent jumper present */
    }
    if (WANT(FEATURE_WRITE_PROTECT) && has_media &&
        (!m->writable || m->write_protected)) {
        feature_begin(body, &len, FEATURE_WRITE_PROTECT, 0, true, true, 4);
    }
    if (WANT(FEATURE_RANDOM_READABLE) && has_media) {
        feature_begin(body, &len, FEATURE_RANDOM_READABLE, 0, true, true, 8);
        uint8_t *p = body + len - 8;
        util_put_be32(p, SCSI_SECTOR_SIZE);
        util_put_be16(p + 4, 1);
        p[6] = 0x01; /* PP: Read/Write Error Recovery page is present */
    }
    if (WANT(FEATURE_MULTI_READ) && has_media) {
        feature_begin(body, &len, FEATURE_MULTI_READ, 0, true, true, 0);
    }
    if (WANT(FEATURE_CD_READ) && has_media) {
        feature_begin(body, &len, FEATURE_CD_READ, 1, true, cd, 4);
    }
    if (WANT(FEATURE_DVD_READ) && has_media) {
        feature_begin(body, &len, FEATURE_DVD_READ, 0, true, dvd, 4);
        uint8_t *p = body + len - 4;
        util_put_be16(p, 1);      /* 2 KiB read */
        p[2] = 0x00;
        p[3] = 0x00;
    }
    if (WANT(FEATURE_BD_READ) && bd) {
        feature_begin(body, &len, FEATURE_BD_READ, 0, true, true, 4);
    }
    if (WANT(FEATURE_RANDOM_WRITABLE) && writable && (dvd || bd)) {
        feature_begin(body, &len, FEATURE_RANDOM_WRITABLE, 0, false, true, 4);
        uint8_t *p = body + len - 4;
        p[0] = 0x00; /* last link size / blocking */
        p[1] = 0x00;
        p[2] = 0x00;
        p[3] = 0x00;
    }
    if (WANT(FEATURE_INCREMENTAL_WRITE) && writable && cd) {
        feature_begin(body, &len, FEATURE_INCREMENTAL_WRITE, 3, false, true, 8);
        uint8_t *p = body + len - 8;
        static const uint8_t inc[8] = {0x3f, 0x0f, 0x00, 0x01,
                                       0x07, 0x00, 0x00, 0x00};
        memcpy(p, inc, sizeof(inc));
    }
    if (WANT(FEATURE_FORMATTABLE) && writable) {
        /* The captured drive reports an 8-byte (all-zero) feature body.
           A zero-length body makes Windows ignore the formattable
           capability, so keep the full body. */
        feature_begin(body, &len, FEATURE_FORMATTABLE, 1, false, true, 8);
    }
    if (WANT(FEATURE_RESTRICTED_OVERWRITE) && writable &&
        profile == MEDIA_PROFILE_DVDRW) {
        feature_begin(body, &len, FEATURE_RESTRICTED_OVERWRITE, 0, false, true, 0);
    }
    if (WANT(FEATURE_DVD_RW_WRITE) && writable && dvd) {
        feature_begin(body, &len, FEATURE_DVD_RW_WRITE, 0, false, true, 4);
        uint8_t *p = body + len - 4;
        util_put_be32(p, 0);
    }
    if (WANT(FEATURE_BD_WRITE) && writable && bd) {
        feature_begin(body, &len, FEATURE_BD_WRITE, 0, false, true, 4);
    }
    if (WANT(FEATURE_TRACK_AT_ONCE) && cdrw) {
        feature_begin(body, &len, FEATURE_TRACK_AT_ONCE, 2, false, writable, 4);
        uint8_t *p = body + len - 4;
        static const uint8_t tao[4] = {0x5f, 0x00, 0x3f, 0x0f};
        memcpy(p, tao, sizeof(tao));
    }
    if (WANT(FEATURE_CD_MASTERING) && writable && cd) {
        feature_begin(body, &len, FEATURE_CD_MASTERING, 1, false, true, 4);
        uint8_t *p = body + len - 4;
        static const uint8_t sao[4] = {0x7f, 0x00, 0x0d, 0x00};
        memcpy(p, sao, sizeof(sao));
    }
    if (WANT(FEATURE_CDRW_WRITE) && cdrw) {
        /* 37h: CD-RW media write support; this is what Windows/IMAPI
           checks before offering the live-file-system/Mastered choice. */
        feature_begin(body, &len, FEATURE_CDRW_WRITE, 0, false, writable, 4);
        uint8_t *p = body + len - 4;
        static const uint8_t cdrw_write[4] = {0x00, 0x0f, 0x00, 0x00};
        memcpy(p, cdrw_write, sizeof(cdrw_write));
    }
#undef WANT

    if (len > sizeof(body)) {
        len = sizeof(body);
    }
    if (cap < 8 + len) {
        len = cap > 8 ? cap - 8 : 0;
    }
    memset(buf, 0, 8);
    util_put_be32(buf, len + 4);
    util_put_be16(buf + 6, (uint16_t)profile);
    memcpy(buf + 8, body, len);
    return (int32_t)(8 + len);
}

static int32_t read_disc_information(uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    memset(buf, 0, 36);
    util_put_be16(buf, 32);
    bool really_blank = m->blank && m->data_lba == 0;
    uint8_t erasable = (m->writable && !m->write_protected) ? 0x10 : 0;
    uint8_t state;
    uint8_t status;
    if (really_blank) {
        state = 0;
        status = 0;
    } else if (m->complete) {
        state = 3;
        status = 2;
    } else {
        state = 1;
        status = 1;
    }
    bool cd = profile_is_cd(m->profile);
    buf[2] = erasable | (uint8_t)(state << 2) | status;
    buf[3] = 1;
    /* Captured blank CD-RW: first track = 1, last track = 1, last
       session = 1, disc type = 0, byte 8 = FFh. */
    buf[4] = cd ? 1 : (really_blank ? 0 : 1);
    buf[5] = 1;
    buf[6] = 1;
    buf[7] = cd ? (m->complete ? 0x80 : 0x00) : 0x20;
    buf[8] = cd ? (m->complete ? 0x00 : 0xFF) : 0x00;
    LOGF("DISCINFO blank=%d complete=%d data=%lu sessions=%u state=%u status=%u",
         m->blank ? 1 : 0, m->complete ? 1 : 0, (unsigned long)m->data_lba,
         buf[4], state, status);
    if (m->kind == MEDIA_KIND_CDRW) {
        if (m->complete) {
            /* Captured successful full-format Disc Information:
               bytes 12-15 = 00 02 99 00, last session/lead-out = FFh. */
            util_put_be32(buf + 12, 0x00029900);
            util_put_be32(buf + 16, 0xFFFFFFFFu);
            util_put_be32(buf + 20, 0xFFFFFFFFu);
        } else if (really_blank) {
            /* Captured blank CD-RW: last-session start 97:34:23 and
               lead-out 79:59:74.  These are ATIP-derived values; Windows
               uses them when building the burn layout. */
            util_put_be32(buf + 16, 0x00612217);
            buf[20] = 0;
            buf[21] = 0x4F;
            buf[22] = 0x3B;
            buf[23] = 0x4A;
        } else {
            util_put_be32(buf + 16, 0xFFFFFFFFu);
            util_put_be32(buf + 20, 0xFFFFFFFFu);
        }
    } else {
        util_put_be32(buf + 16, 0);
        util_put_be32(buf + 20, 0);
    }
    return m->complete ? 34 : 36;
}

static int32_t read_track_information(const uint8_t *cdb, uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    uint8_t type = cdb[1] & 0x03;
    uint32_t number = util_be32(cdb + 2);
    if (type == 0) {
        if (number >= m->capacity_lba) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        }
    } else if (type == 1) {
        if (!(number == 0 || number == 1 || number == 0xFF)) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
    } else if (type == 2) {
        if (number != 1) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
    } else {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }

    if (m->kind == MEDIA_KIND_CDRW && m->data_lba == 0 &&
        (m->blank || m->formatted) && type == 1 && number == 1) {
        /* Captured blank CD-RW READ TRACK INFORMATION: 48 bytes,
           track mode 05h, flags 4Fh, NWA = capacity - 2, free = FFFFFFFFh.
           The old 36-byte layout put NWA at the wrong offset. */
        memset(buf, 0, 48);
        util_put_be16(buf, 46);
        buf[2] = 1;
        buf[3] = 1;
        buf[5] = 0x05;
        /* Real blank CD-RW: flags 4Fh/01h, NWA near lead-out, free FFh.
           Presenting this as a complete track made Windows show
           "cannot read the disc" instead of the blank burn dialog. */
        buf[6] = 0x4F;
        buf[7] = 0x01;
        /* Unformatted blank: NWA near lead-out.  Formatted-empty (after
           FORMAT UNIT): NWA must be 0 and free space must be the whole
           formatted capacity; otherwise Windows sees a full track, never
           issues WRITE, and reports the live-file-system format failed. */
        if (m->complete) {
            /* Captured successful full-format: complete track 1,
               flags B1h, length field 46, leadout at offsets 24/28. */
            buf[6] = 0xB1;
            buf[7] = 0x02;
            util_put_be32(buf + 12, 0xFFFFFFFFu);
            util_put_be32(buf + 20, 0x20);
            util_put_be32(buf + 24, m->capacity_lba);
            util_put_be32(buf + 28, m->capacity_lba ? m->capacity_lba - 1 : 0);
            LOGF("TRACKINFO complete CD-RW cap=%lu", (unsigned long)m->capacity_lba);
            return 36;
        }
        /* Windows may cache the pre-format blank Track and never re-read it
           after FORMAT UNIT.  Returning NWA near lead-out made it conclude
           the disc was full and skip WRITE.  Report the whole capacity free
           for blank CD-RW too; it still formats and the cached layout stays
           writable. */
        uint32_t nwa = m->formatted
                           ? 0
                           : (m->capacity_lba > 2 ? m->capacity_lba - 2 : 0);
        util_put_be32(buf + 20, nwa);
        util_put_be32(buf + 28, nwa);
        util_put_be32(buf + 32, m->formatted ? m->capacity_lba : 0xFFFFFFFFu);
        LOGF("TRACKINFO %s CD-RW nwa=%lu free=%lu",
             m->formatted ? "formatted-empty" : "blank",
             (unsigned long)nwa,
             (unsigned long)(m->formatted ? m->capacity_lba : 0xFFFFFFFFu));
        return 48;
    }

    memset(buf, 0, 36);
    util_put_be16(buf, 34);
    buf[2] = 1;
    buf[32] = 0;
    buf[3] = 1;
    buf[33] = 0;
    buf[5] = 0x04; /* data track */
    bool writable = m->writable && !m->write_protected;
    /* Track blank flag is about the track contents, not the media status:
       a freshly formatted track with no recorded blocks is still blank. */
    bool really_blank = m->data_lba == 0;
    buf[6] = (really_blank ? 0x40 : 0x00) | 0x01; /* blank flag, Mode 1 */
    if (m->profile == MEDIA_PROFILE_DVDRW) {
        buf[6] |= 0x10; /* fixed packet formatting (Restricted Overwrite) */
    }
    bool has_recorded = m->data_lba > 0;
    buf[7] = (has_recorded ? 0x02 : 0x00) |
             ((writable && (!m->complete || m->data_lba == 0)) ? 0x01 : 0x00);
    util_put_be32(buf + 8, 0); /* track start */
    util_put_be32(buf + 12, m->data_lba);
    uint32_t free_blocks = 0;
    if (writable && (!m->complete || m->data_lba == 0)) {
        free_blocks = m->capacity_lba > m->data_lba ? m->capacity_lba - m->data_lba : 0;
    }
    util_put_be32(buf + 16, free_blocks);
    util_put_be32(buf + 20, 16); /* packet/blocking size */
    util_put_be32(buf + 24, m->data_lba ? m->data_lba : m->capacity_lba);
    util_put_be32(buf + 28, m->data_lba ? m->data_lba - 1 : 0);
    LOGF("TRACKINFO %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
         "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
         "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
         buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
         buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
         buf[16], buf[17], buf[18], buf[19], buf[20], buf[21], buf[22], buf[23],
         buf[24], buf[25], buf[26], buf[27], buf[28], buf[29], buf[30], buf[31],
         buf[32], buf[33], buf[34], buf[35]);
    return 36;
}

static void toc_address(uint8_t *buf, bool msf, uint32_t lba)
{
    if (msf) {
        uint32_t x = lba + 150;
        buf[0] = 0;
        buf[1] = (uint8_t)(x / (75 * 60));
        buf[2] = (uint8_t)((x / 75) % 60);
        buf[3] = (uint8_t)(x % 75);
    } else {
        util_put_be32(buf, lba);
    }
}

static int32_t read_toc(const uint8_t *cdb, uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    bool msf = (cdb[1] & 0x02) != 0;
    uint8_t format = cdb[2] & 0x0F;
    uint8_t start_track = cdb[6];
    if ((cdb[1] & ~0x02) != 0 || (start_track > 1 && format != 1)) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    /* After FORMAT UNIT the disc has an open, empty track covering the
       formatted capacity.  Reporting leadout 0 here makes Windows see an
       incomplete session with no track and abort the format. */
    bool formatted_empty = m->blank && m->formatted && m->data_lba == 0;
    /* TOC lead-out is the physical disc capacity, not the shrunken
       formatted capacity.  The real full-format capture still reports
       MSF 4F:3B:49 (~80 min) after formatting to 294048 blocks; using
       294048 here made Windows loop on READ TOC. */
    uint32_t toc_capacity = m->nominal_lba ? m->nominal_lba : m->capacity_lba;
    if (m->kind == MEDIA_KIND_CDRW) {
        /* Captured TOC: format 0 lead-out 05:7D:A8 (359336), format 2
           lead-out MSF 4F:3B:49 (359848).  Windows compares both. */
        toc_capacity = (format == 0) ? 359336u : 359848u;
    }
    uint8_t toc_ctrl = m->kind == MEDIA_KIND_CDRW ? 0x15 : 0x14;
    uint32_t leadout = ((m->session_open || m->complete || formatted_empty) &&
                        m->data_lba == 0)
                           ? toc_capacity
                           : m->data_lba;
    LOGF("TOC fmt=%u leadout=%lu blank=%d complete=%d data=%lu",
         format, (unsigned long)leadout, m->blank ? 1 : 0,
         m->complete ? 1 : 0, (unsigned long)m->data_lba);

    /* Before the UDF write the real drive does not expose a usable TOC;
       Windows' live-file-system formatter goes straight to READ TRACK.
       Returning a synthetic TOC here made it loop on TOC and never issue
       WRITE, so keep TOC invalid until data actually exists. */
    if ((m->blank || (m->complete && m->data_lba == 0)) &&
        (format == 0 || format == 1 || format == 2)) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }

    if (format == 0) {
        uint32_t len = 20;
        memset(buf, 0, len);
        buf[1] = (uint8_t)(len - 2);
        buf[2] = 1;
        buf[3] = 1;
        buf[5] = toc_ctrl; /* ADR=1, data track */
        buf[6] = 1;
        toc_address(buf + 8, msf, 0);
        buf[13] = toc_ctrl;
        buf[14] = 0xAA;
        toc_address(buf + 16, msf, leadout);
        return (int32_t)len;
    }
    if (format == 1) {
        uint32_t len = 12;
        memset(buf, 0, len);
        buf[1] = 10;
        buf[2] = 1;
        buf[3] = 1;
        buf[5] = toc_ctrl;
        buf[6] = 1;
        toc_address(buf + 8, msf, 0);
        return (int32_t)len;
    }
    if (format == 2) {
        /* Full TOC: A0, A1, A2 and one descriptor per track.  The track
           descriptor is essential; ImgBurn builds its disc layout from this
           response and otherwise sees zero tracks/sessions. */
        uint32_t len = 4 + 4 * 11;
        memset(buf, 0, len);
        buf[1] = (uint8_t)(len - 2);
        buf[2] = 1; /* first complete session */
        buf[3] = 1; /* last complete session */
        uint8_t *p = buf + 4;
        for (int i = 0; i < 3; i++) {
            p[0] = 1;    /* session */
            p[1] = toc_ctrl; /* ADR=1, data */
            p[2] = 0;    /* TNO */
            p[3] = (uint8_t)(0xA0 + i);
            if (i == 0 || i == 1) {
                p[8] = 1; /* PMIN: first/last track */
            } else {
                p[8] = (uint8_t)((leadout + 150) / (75 * 60));
                p[9] = (uint8_t)(((leadout + 150) / 75) % 60);
                p[10] = (uint8_t)((leadout + 150) % 75);
            }
            p += 11;
        }
        /* Track 1 descriptor: POINT=1, start address 00:02:00 (LBA 0). */
        p[0] = 1;
        p[1] = toc_ctrl;
        p[2] = 1; /* TNO */
        p[3] = 1; /* POINT */
        p[8] = 0; /* PMIN */
        p[9] = 2; /* PSEC: +150 frame offset */
        p[10] = 0;
        return (int32_t)len;
    }
    if (format == 3) {
        /* PMA.  A blank virtual disc has no PMA descriptors; the two-byte
           length field is still a valid empty PMA response. */
        memset(buf, 0, 4);
        buf[1] = 2;
        return 4;
    }
    if (format == 4) {
        /* ATIP is what ImgBurn uses for "Getting MID" on CD-RW media. */
        if (m->kind != MEDIA_KIND_CDRW) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        uint32_t len = 4 + 28;
        memset(buf, 0, len);
        /* Data Length includes the two reserved bytes at offsets 2-3. */
        buf[1] = 30;
        uint8_t *p = buf + 4;
        /* Special Information 1: True1, 4x reference speed, a plausible
           initial write power. */
        p[0] = 0xC2;
        p[1] = 0x40; /* URU */
        p[2] = 0xC4; /* True2, CD-RW, A1 valid */
        p[3] = 0;
        /* ATIP lead-in 97:26:50 (a known CD media manufacturer ID) and
           lead-out at the disc's maximum capacity. */
        p[4] = 97;
        p[5] = 26;
        p[6] = 50;
        p[7] = 0;
        uint32_t lo = m->capacity_lba + 150u;
        p[8] = (uint8_t)(lo / (75u * 60u));
        p[9] = (uint8_t)((lo / 75u) % 60u);
        p[10] = (uint8_t)(lo % 75u);
        p[11] = 0;
        /* A1/A2/A3 values are intentionally zero; the valid bits tell the
           initiator they are present but generic. */
        return (int32_t)len;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
}

static int32_t get_event_status(const uint8_t *cdb, uint8_t *buf)
{
    bool polled = (cdb[1] & 0x01) != 0;
    uint8_t classes = cdb[4];
    uint16_t alloc = util_be16(cdb + 7);
    if (!polled) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    memset(buf, 0, 8);
    /* Captured real drive: supported event classes 5Eh.  Windows then
       polls class 5Ah (media event) with an 8-byte descriptor:
       00 06 06 5e <event> 01 00 00.  Without advertising 5Ah, IMAPI
       never polls events during FORMAT UNIT and skips its write phase. */
    if (classes == 0) {
        buf[0] = 0;
        buf[1] = 2;
        buf[2] = 0x80; /* NEA */
        buf[3] = 0x5E; /* supported classes */
        uint32_t n = alloc < 4 ? alloc : 4;
        return (int32_t)n;
    }
    if (classes == 0x5A || classes == 0x10) {
        util_put_be16(buf, 6);
        buf[2] = 0x06; /* media event */
        buf[3] = 0x5E; /* event class */
        buf[4] = s_scsi.event_pending ? 0x02 : 0x00;
        buf[5] = 1;
        buf[6] = 0;
        buf[7] = 0;
        s_scsi.event_pending = false;
        s_scsi.event_code = 0;
        uint32_t n = alloc < 8 ? alloc : 8;
        return (int32_t)n;
    }
    buf[0] = 0;
    buf[1] = 2;
    buf[2] = 0x80;
    buf[3] = 0x5E;
    uint32_t n = alloc < 4 ? alloc : 4;
    return (int32_t)n;
}

static int32_t mechanism_status(uint8_t *buf)
{
    memset(buf, 0, 8);
    buf[5] = 0x01; /* CD-ROM mechanism */
    return 8;
}

static int32_t read_capacity(uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    memset(buf, 0, 8);
    /* A blank CD-RW still has a real, usable capacity.  Windows shows
       "702 MB free of 702 MB" for a blank CD-RW; returning last LBA 0
       made Explorer show 0 bytes and refused the burn wizard.  The
       format descriptor can later shrink capacity (e.g. 574 MiB). */
    uint32_t last = m->capacity_lba ? m->capacity_lba - 1 : 0;
    util_put_be32(buf, last);
    util_put_be32(buf + 4, SCSI_SECTOR_SIZE);
    return 8;
}

static int32_t read_format_capacities(uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) {
        return -1;
    }
    uint32_t profile = m->profile ? m->profile : MEDIA_PROFILE_CDROM;
    uint32_t capacity = m->capacity_lba;
    if (profile == MEDIA_PROFILE_CDRW && m->nominal_lba) {
        capacity = m->nominal_lba;
    }
    bool formatted = !m->blank;
    uint8_t *d = buf + 4;
    int n = 0;

#define ADD_DESC(type, flags)                                            \
    do {                                                                 \
        util_put_be32(d, capacity);                                      \
        d[4] = (uint8_t)(((type) << 2) | ((flags) & 3));                 \
        d[5] = 0;                                                        \
        d[6] = (uint8_t)(SCSI_SECTOR_SIZE >> 8);                         \
        d[7] = (uint8_t)SCSI_SECTOR_SIZE;                                \
        d += 8;                                                          \
        n++;                                                             \
    } while (0)
#define ADD_FMT(blocks, type, param)                                     \
    do {                                                                 \
        util_put_be32(d, (blocks));                                      \
        d[4] = (uint8_t)(type);                                          \
        d[5] = 0;                                                        \
        d[6] = 0;                                                        \
        d[7] = (param);                                                  \
        d += 8;                                                          \
        n++;                                                             \
    } while (0)

    if (profile == MEDIA_PROFILE_DVDRW) {
        /* 0x00 = full formatted capacity (growisofs mandatory),
           0x15/0x10 = quick/full format, 0x13 = quick grow. */
        ADD_DESC(0x00, formatted ? 2 : 0);
        /* growisofs skips descriptor #0 when looking for the obligatory
           00h entry, so provide it again as descriptor #1. */
        ADD_DESC(0x00, formatted ? 2 : 0);
        ADD_DESC(0x15, 0);
        ADD_DESC(0x10, 0);
        ADD_DESC(0x13, 0);
    } else if (profile == MEDIA_PROFILE_BDRE) {
        ADD_DESC(0x00, formatted ? 2 : 0);
        ADD_DESC(0x30, 0);
        ADD_DESC(0x31, 0);
    } else if (profile == MEDIA_PROFILE_DVDROM ||
               profile == MEDIA_PROFILE_BDROM) {
        ADD_DESC(0x00, 2);
    } else if (profile == MEDIA_PROFILE_CDRW) {
        /* Captured CD-RW response: nominal 00h capacity, then the real
           10h formatted capacity 295264 (576.7 MiB), plus empty 11h/12h.
           Windows chooses the 10h value and sends FORMAT UNIT with
           0x00048160, which is also what the successful full-format
           capture used. */
        ADD_DESC(0x00, m->blank ? 1 : 2);
        uint32_t fmtcap = 295264;
        uint32_t physical = m->nominal_lba ? m->nominal_lba : capacity;
        if (physical < fmtcap) fmtcap = physical;
        ADD_FMT(fmtcap, 0x40, 0x20); /* type 10h, formatted cap */
        ADD_FMT(0, 0x44, 0x20);      /* type 11h */
        ADD_FMT(0, 0x48, 0x20);      /* type 12h */
    } else {
        /* CD-ROM / CD-R: nominal capacity only. */
        ADD_DESC(0x00, m->blank ? 1 : 2);
        ADD_DESC(0x00, 2);
    }
#undef ADD_FMT
#undef ADD_DESC

    util_put_be32(buf, (uint32_t)(n * 8));
    return (int32_t)(4 + n * 8);
}

static int32_t read_disc_structure(const uint8_t *cdb, uint8_t *buf,
                                   uint32_t cap)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) {
        return -1;
    }
    if (cap < 4) {
        return (int32_t)cap;
    }
    bool bd = (cdb[1] & 1) != 0;
    uint8_t format = cdb[7];
    /* READ DISC STRUCTURE is a DVD/BD command.  The captured real
       DVD-ROM/CD-RW combo returns CHECK CONDITION for a blank CD-RW
       (formats 00h and C0h); the old fallback returned a 4 KiB zero
       structure, which Windows could mistake for valid DVD data. */
    if (profile_is_cd(m->profile)) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    uint32_t n = cap > 4096 ? 4096 : cap;
    LOGF("DISCSTRUCT fmt=%02X bd=%d cap=%lu profile=%04X", format, bd ? 1 : 0,
         (unsigned long)cap, m->profile);
    memset(buf, 0, n);

    if (format == 0x00 && !bd && profile_is_dvd(m->profile)) {
        /* DVD Physical Format Information (4-byte header + 32 bytes). */
        util_put_be16(buf, 34);
        if (n > 36) n = 36;
        if (n < 36) return (int32_t)n;
        buf[4] = (m->profile == MEDIA_PROFILE_DVDRW) ? 0x30 : 0x00;
        buf[5] = 0x00; /* part version */
        buf[6] = 0x00; /* disc size / maximum rate */
        uint32_t start = 0x000300; /* lead-in */
        uint32_t end = m->capacity_lba ? m->capacity_lba - 1 : 0;
        buf[9] = (uint8_t)(start >> 16);
        buf[10] = (uint8_t)(start >> 8);
        buf[11] = (uint8_t)start;
        buf[13] = (uint8_t)(end >> 16);
        buf[14] = (uint8_t)(end >> 8);
        buf[15] = (uint8_t)end;
        memcpy(buf + 21, "OPI3B ", 6); /* media ID (dash media) */
        memcpy(buf + 29, "000", 3);
        LOGF("DISCSTRUCT0 start=%06lX end=%06lX book=%02X", (unsigned long)start,
             (unsigned long)end, buf[4]);
        return 36;
    }
    if (format == 0x00 && bd && profile_is_bd(m->profile)) {
        /* BD Disc Information: report a full 128-byte structure. */
        util_put_be16(buf, 126);
        if (n < 128) return (int32_t)n;
        buf[4] = 'D';
        buf[5] = 'I';
        memcpy(buf + 4 + 100, "OPI3B", 5);
        return 128;
    }
    if (format == 0xFF) {
        /* Structure list: header + two 4-byte entries (Physical Format,
           Recording Management). */
        const uint32_t list_len = 4 + 2 * 4;
        util_put_be16(buf, (uint16_t)(list_len - 2));
        buf[2] = 0;
        buf[3] = 0;
        if (n >= 10) {
            buf[4] = 0x00; /* Physical Format Information */
            buf[5] = 0;
            buf[6] = 0;
            buf[7] = bd ? 126 : 32;
            buf[8] = 0x04; /* Recording Management Area */
            buf[9] = 0;
            buf[10] = 0;
            buf[11] = 0;
        }
        return n < list_len ? (int32_t)n : (int32_t)list_len;
    }
    util_put_be16(buf, (uint16_t)(n - 2));
    return (int32_t)n;
}

static int32_t request_sense(uint8_t *buf)
{
    memset(buf, 0, 18);
    buf[0] = 0x70;
    buf[2] = s_scsi.sense_valid ? (s_scsi.sense.key & 0x0F) : 0;
    buf[7] = 10;
    buf[12] = s_scsi.sense_valid ? s_scsi.sense.asc : 0;
    buf[13] = s_scsi.sense_valid ? s_scsi.sense.ascq : 0;
    s_scsi.sense_valid = false;
    s_scsi.ua_pending = false;
    memset(&s_scsi.sense, 0, sizeof(s_scsi.sense));
    return 18;
}

static int32_t format_unit(const uint8_t *cdb, const uint8_t *data, uint32_t len)
{
    if (require_media() < 0) return -1;
    const media_status_t *m = media_get();
    if (!m->writable || m->write_protected) {
        return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
    }
    /* FORMAT UNIT with FMTDATA carries a format descriptor.  Windows
       formatting a blank CD-RW asks for 0x00047CA0 (294048) blocks
       (~574 MiB) on a 700 MiB disc.  Honour that capacity so READ CAPACITY
       and READ TOC agree with the host's format request. */
    uint32_t blocks = 0;
    if (data && len >= 8) {
        uint32_t b = util_be32(data + 4);
        if (b >= 100 && b <= m->capacity_lba) {
            blocks = b;
        }
    }
    if (!media_format_capacity(blocks)) {
        return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
    }
    /* Format completion is reported as media event 07h.  Even for an
       immediate FORMAT UNIT, report the event so IMAPI can finish. */
    s_scsi.event_code = 0x07;
    s_scsi.event_pending = true;
    LOGF("SCSI: FORMAT UNIT len=%lu blocks=%lu data=%02X %02X %02X %02X %02X %02X",
         (unsigned long)len, (unsigned long)blocks,
         data ? data[0] : 0, data ? data[1] : 0,
         data ? data[2] : 0, data ? data[3] : 0,
         data ? data[4] : 0, data ? data[5] : 0);
    (void)cdb;
    return 0;
}

static int32_t start_stop(const uint8_t *cdb)
{
    uint8_t power = cdb[4] & 0xF0;
    if (power) {
        return 0; /* power condition is a no-op */
    }
    bool start = (cdb[4] & 0x01) != 0;
    bool loej = (cdb[4] & 0x02) != 0;
    if (!loej) {
        return 0; /* spin up/down is a no-op */
    }

    /* Virtual tray with real media state changes.  ImgBurn cycles the tray
       before verification so the host re-reads the TOC after finalization;
       a no-op here leaves its pre-burn layout cache in place. */
    if (!start) {
        if (s_scsi.prevent) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_MEDIUM_REMOVAL_PREVENTED, 0);
        }
        if (media_is_present()) {
            media_eject();
            scsi_notify_media_change(false);
        }
        return 0;
    }

    if (!media_is_present()) {
        if (!media_reload()) {
            return fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        }
        scsi_notify_media_change(true);
    }
    return 0;
}

static int32_t blank_command(const uint8_t *cdb)
{
    if (require_media() < 0) return -1;
    const media_status_t *m = media_get();
    if (!m->writable || m->write_protected) {
        return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
    }
    uint8_t type = cdb[1] & 0x07;
    if (type != 0 && type != 1 && type != 4) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    if (!media_blank()) {
        return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
    }
    return 0;
}

static int32_t close_track_session(const uint8_t *cdb)
{
    if (require_media() < 0) return -1;
    const media_status_t *m = media_get();
    if (!m->writable) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    }
    uint8_t function = cdb[2] & 0x07;
    if (function == 0) {
        return 0;
    }
    if (function == 1) {
        (void)media_save_meta();
        return 0;
    }
    if (function == 2) {
        /* Close the current session but do NOT finalize the disc:
           ImgBurn multi-session expects to be able to append later.
           Finalize is function 3. */
        if (!media_close_session(false)) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    }
    if (function == 3) {
        if (!media_close_session(true)) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
}

static int32_t scsi_in_command_locked(const uint8_t cdb[SCSI_CDB_MAX], uint8_t *buf, uint32_t cap)
{
    if (media_format_ua_take()) {
        set_ua(SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0);
    }
    if (!begin_command(cdb[0])) {
        return -1;
    }
    switch (cdb[0]) {
    case 0xA4: { /* REPORT KEY: captured real response */
        static const uint8_t rk[8] = {0x00, 0x06, 0x00, 0x00,
                                      0x25, 0xff, 0x01, 0x00};
        if (buf && cap >= sizeof(rk)) {
            memcpy(buf, rk, sizeof(rk));
        }
        return 8;
    }
    case OP_TEST_UNIT_READY:
        if (require_media() < 0) return -1;
        if (media_is_erasing()) {
            /* Real drive: BLANK is asynchronous and TUR reports
               2/04/07 (operation in progress) until erase completes. */
            return fail(SENSE_NOT_READY, 0x04, 0x07);
        }
        if (media_is_formatting()) {
            /* Real full-format capture: 2/04/04 (not ready, format in
               progress) until the format completes. */
            return fail(SENSE_NOT_READY, 0x04, 0x04);
        }
        return 0;
    case OP_REQUEST_SENSE:
        return request_sense(buf);
    case OP_INQUIRY:
        return inquiry(cdb, buf, cap);
    case OP_MODE_SENSE_6:
        return mode_sense(cdb, buf, cap, false);
    case OP_MODE_SENSE_10:
        return mode_sense(cdb, buf, cap, true);
    case OP_READ_CAPACITY_10:
        return read_capacity(buf);
    case OP_READ_FORMAT_CAPACITIES:
        return read_format_capacities(buf);
    case OP_READ_DISC_INFORMATION:
        return read_disc_information(buf);
    case OP_READ_TRACK_INFORMATION:
        return read_track_information(cdb, buf);
    case OP_READ_TOC:
        return read_toc(cdb, buf);
    case OP_READ_HEADER: {
        if (require_media() < 0) return -1;
        uint32_t lba = util_be32(cdb + 2);
        const media_status_t *m = media_get();
        if (lba >= m->capacity_lba) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        }
        memset(buf, 0, 8);
        buf[0] = 1;
        toc_address(buf + 4, (cdb[1] & 2) != 0, lba);
        return 8;
    }
    case OP_GET_CONFIGURATION:
        return get_configuration(cdb, buf, cap);
    case OP_GET_EVENT_STATUS:
        return get_event_status(cdb, buf);
    case OP_MECHANISM_STATUS:
        return mechanism_status(buf);
    case 0x04: /* FORMAT UNIT with no parameter list */
        return format_unit(cdb, NULL, 0);
    case OP_START_STOP_UNIT:
        return start_stop(cdb);
    case OP_PREVENT_ALLOW:
        s_scsi.prevent = (cdb[4] & 0x01) != 0;
        return 0;
    case OP_SYNCHRONIZE_CACHE_10:
        if (require_media() < 0) return -1;
        if (!media_sync()) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    case OP_BLANK:
        return blank_command(cdb);
    case OP_CLOSE_TRACK_SESSION:
        return close_track_session(cdb);
    case OP_RESERVE_TRACK: {
        if (require_media() < 0) return -1;
        const media_status_t *m = media_get();
        uint32_t reserved = util_be32(cdb + 5);
        if (!m->writable || m->write_protected) {
            return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        }
        if (reserved && (uint64_t)m->data_lba + reserved > m->capacity_lba) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        if (!media_reserve_track(m->data_lba, reserved)) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    }
    case OP_SEND_OPC:
        if (require_media() < 0) return -1;
        if (!media_get()->writable) {
            return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        }
        return 0;
    case 0xA7: /* SET READ AHEAD */
        return 0;
    case 0xB6: /* SET STREAMING (parameter list handled as a no-op) */
        return 0;
    case OP_SET_CD_SPEED: {
        uint16_t read_speed = util_be16(cdb + 2);
        uint16_t write_speed = util_be16(cdb + 4);
        if (read_speed) {
            if (read_speed > 8 * 176) read_speed = 8 * 176;
            s_scsi.read_speed_kbs = read_speed;
        }
        if (write_speed) {
            if (write_speed > 4 * 176) write_speed = 4 * 176;
            s_scsi.write_speed_kbs = write_speed;
        }
        return 0;
    }
    case OP_READ_BUFFER_CAPACITY:
        memset(buf, 0, 12);
        /* Length of the following structure, including its reserved bytes. */
        util_put_be16(buf, 10);
        util_put_be32(buf + 4, 2048);
        util_put_be32(buf + 8, 2048);
        return 12;
    case OP_READ_BUFFER: {
        /* cdrecord queries the drive buffer before starting a write.  Mode 0
           returns a 4-byte buffer header; a zero size simply disables
           burn-free rather than aborting the burn. */
        uint32_t n = cap < 4 ? cap : 4;
        memset(buf, 0, n);
        return (int32_t)n;
    }
    case OP_READ_DISC_STRUCTURE:
        return read_disc_structure(cdb, buf, cap);
    case OP_GET_PERFORMANCE: {
        const media_status_t *m = media_get();
        uint8_t type = cdb[10];
        uint8_t req = cdb[1] & 0x1F;
        bool write_perf = (req == 0x04 || req == 0x14);
        if ((type == 0x00 || type == 0x03 || write_perf) && m->present) {
            uint32_t profile = m->profile ? m->profile : MEDIA_PROFILE_CDROM;
            uint32_t read_kbs = s_scsi.read_speed_kbs;
            uint32_t write_kbs = s_scsi.write_speed_kbs;
            if (profile == MEDIA_PROFILE_DVDRW ||
                profile == MEDIA_PROFILE_DVDROM) {
                read_kbs = 16 * 1385;   /* 16x DVD */
                write_kbs = 8 * 1385;   /* 8x DVD */
            } else if (profile == MEDIA_PROFILE_BDRE ||
                       profile == MEDIA_PROFILE_BDROM) {
                read_kbs = 6 * 4500;    /* 6x BD */
                write_kbs = 4 * 4500;   /* 4x BD */
            } else {
                if (!read_kbs) read_kbs = 24 * 176;  /* 24x CD */
                if (!write_kbs) write_kbs = 10 * 176; /* 10x CD */
            }
            /* 16-byte descriptor.  Type 0 (Performance) and type 4
               (Write Performance, also requested as 14h by Windows)
               have different field layouts; both are 24 bytes total.
               The captured Windows/IMAPI sequence requests both 00h and
               14h, and expects a real descriptor rather than an empty
               response. */
            uint32_t len = 8 + 16;
            memset(buf, 0, len);
            util_put_be32(buf, 20); /* data length following this field */
            if (req == 0x14) {
                buf[4] = 0x02; /* captured write-performance type marker */
            }
            uint8_t *p = buf + 8;
            p[0] = 0x00; /* CLV, exact, no MRW */
            if (write_perf) {
                util_put_be32(p + 4, write_kbs);
                util_put_be32(p + 8, m->capacity_lba ? m->capacity_lba - 1 : 0);
                util_put_be32(p + 12, write_kbs);
            } else {
                util_put_be32(p + 4, m->capacity_lba ? m->capacity_lba - 1 : 0);
                util_put_be32(p + 8, read_kbs);
                util_put_be32(p + 12, write_kbs);
            }
            return (int32_t)len;
        }
        memset(buf, 0, 8);
        return 8;
    }
    case OP_READ_10:
    case OP_READ_12:
    case OP_READ_CD:
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    case OP_WRITE_10:
    case OP_WRITE_12:
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    case OP_VERIFY_10:
    case OP_VERIFY_12:
        if (require_media() < 0) return -1;
        return 0;
    default:
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    }
}

static int32_t scsi_out_command_locked(const uint8_t cdb[SCSI_CDB_MAX], const uint8_t *data,
                         uint32_t len)
{
    if (!begin_command(cdb[0])) {
        return -1;
    }
    if (cdb[0] == 0x04) { /* FORMAT UNIT with parameter list */
        return format_unit(cdb, data, len);
    }
    if (cdb[0] == 0x5D) { /* SEND CUE SHEET: accepted, data is not needed for TAO/SAO emulation */
        (void)data;
        (void)len;
        LOGF("SCSI: cue sheet accepted (%lu bytes)", (unsigned long)len);
        return 0;
    }
    if (cdb[0] == 0xB6) { /* SET STREAMING */
        (void)data;
        (void)len;
        return 0;
    }
    if (cdb[0] == OP_MODE_SELECT_6 || cdb[0] == OP_MODE_SELECT_10) {
        uint32_t offset = cdb[0] == OP_MODE_SELECT_6 ? 4 : 8;
        while (offset + 2 <= len) {
            uint8_t page = data[offset] & 0x3F;
            uint8_t page_len = data[offset + 1];
            if (offset + 2 + page_len > len) {
                break;
            }
            if (page == 0x05 && page_len >= 5) {
                uint8_t *p = (uint8_t *)data + offset;
                s_scsi.write_type = p[2] & 0x0F;
                s_scsi.data_block_type = p[4];
            }
            offset += 2 + page_len;
        }
        return 0;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
}

/* Public entry points hold the media lock so local UI actions and SCSI
 * commands cannot mutate media concurrently. */
int32_t scsi_in_command(const uint8_t cdb[SCSI_CDB_MAX], uint8_t *buf, uint32_t cap)
{
    media_lock();
    int32_t rc = scsi_in_command_locked(cdb, buf, cap);
    media_unlock();
    return rc;
}

int32_t scsi_out_command(const uint8_t cdb[SCSI_CDB_MAX], const uint8_t *data,
                         uint32_t len)
{
    media_lock();
    int32_t rc = scsi_out_command_locked(cdb, data, len);
    media_unlock();
    return rc;
}
