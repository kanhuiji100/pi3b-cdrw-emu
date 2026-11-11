#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "media.h"
#include "scsi.h"
#include "util.h"

static media_status_t g_media;

const media_status_t *media_get(void) { return &g_media; }
bool media_is_present(void) { return g_media.present; }
bool media_init(void) { return true; }
void media_shutdown(void) {}
bool media_load(const char *p) { (void)p; return true; }
bool media_reload(void) { return true; }
bool media_eject(void) { g_media.present = false; return true; }
bool media_create_blank(const char *p, uint32_t s) { (void)p; (void)s; return true; }
bool media_new_blank(char *p, size_t n) { (void)p; (void)n; return true; }
bool media_can_write(uint32_t l, uint32_t b) { (void)l; (void)b; return g_media.writable; }
bool media_read_blocks(uint32_t l, void *d, uint32_t b) { (void)l; (void)d; (void)b; return true; }
bool media_write_blocks(uint32_t l, const void *s, uint32_t b) { (void)l; (void)s; (void)b; return true; }
bool media_write_raw_blocks(uint32_t l, const void *s, uint32_t b, uint32_t z) { (void)l; (void)s; (void)b; (void)z; return true; }
bool media_blank(void) { return true; }
bool media_format(void) { return true; }
bool media_format_capacity(uint32_t b) { (void)b; return true; }
bool media_is_formatted(void) { return false; }
bool media_is_formatting(void) { return false; }
bool media_is_erasing(void) { return false; }
bool media_format_ua_take(void) { return false; }
bool media_close_session(bool f) { (void)f; return true; }
bool media_reserve_track(uint32_t l, uint32_t b) { (void)l; (void)b; return true; }
bool media_sync(void) { return true; }
bool media_save_meta(void) { return true; }
uint32_t media_default_capacity_lba(void) { return 358400; }

static void reset_media(void)
{
    memset(&g_media, 0, sizeof(g_media));
    g_media.present = true;
    g_media.writable = true;
    g_media.kind = MEDIA_KIND_CDRW;
    g_media.profile = MEDIA_PROFILE_CDRW;
    g_media.capacity_lba = 358400;
    g_media.data_lba = 0;
    g_media.nwa = 0;
    g_media.blank = true;
    g_media.generation = 1;
    util_safe_copy(g_media.name, sizeof(g_media.name), "TESTDISC");
    util_safe_copy(g_media.path, sizeof(g_media.path), "/discs/TESTDISC.CDR");
    scsi_init();
}

static void test_inquiry(void)
{
    reset_media();
    uint8_t cdb[16] = {0x12, 0, 0, 0, 36, 0};
    uint8_t buf[512];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 36);
    assert((buf[0] & 0x1F) == 5);           /* CD-ROM peripheral */
    assert(buf[1] & 0x80);                  /* removable */
    assert(memcmp(buf + 8, "OrangePi", 8) == 0);
    assert(memcmp(buf + 16, "DVD/BD USB 2.0  ", 16) == 0);

    cdb[1] = 1; cdb[2] = 0x00; cdb[3] = 0; cdb[4] = 64;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n >= 7 && buf[1] == 0);
    assert(memchr(buf + 4, 0x80, (size_t)n - 4) != NULL);
}

static void test_capacity_and_config(void)
{
    reset_media();
    uint8_t cdb[16] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t buf[512];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 8);
    /* Blank CD-RW still reports its real capacity (702 MiB in Explorer). */
    assert(util_be32(buf) == 358399);
    assert(util_be32(buf + 4) == 2048);

    /* A blank CD-RW rejects READ(10) with LBA out of range, even LBA 0. */
    uint8_t rcdb[16] = {0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    uint32_t rlba = 0, rblocks = 0;
    assert(!scsi_read_params(rcdb, &rlba, &rblocks));
    const scsi_sense_t *s = scsi_get_sense();
    assert(s->key == 0x05 && s->asc == 0x21);
    g_media.blank = false;
    g_media.data_lba = 100;
    assert(scsi_read_params(rcdb, &rlba, &rblocks));
    assert(rlba == 0 && rblocks == 2);
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(util_be32(buf) == 358399);
    reset_media();

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x46;
    cdb[7] = 0; cdb[8] = 255;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n > 40);
    assert(util_be16(buf + 6) == 0x000A); /* current profile CD-RW */
    assert(util_be16(buf + 8) == 0x0000); /* first feature must start at offset 8 */
    assert(buf[8 + 2] == 0x03);           /* version 0, persistent+current */
    assert(buf[8 + 3] == 16);             /* CD/DVD media: four profiles */
    uint32_t data_len = util_be32(buf);
    assert(data_len + 4 == (uint32_t)n);
    /* Profile list starts at offset 12: CD-ROM, CD-RW, DVD-ROM, DVD-RW.
       BD profiles must not appear for CD media. */
    assert(util_be16(buf + 8) == 0x0000);
    assert(util_be16(buf + 12) == 0x0008);
    assert(util_be16(buf + 16) == 0x000A);
    assert(buf[18] & 1);
    assert(util_be16(buf + 20) == 0x0010);
    assert(util_be16(buf + 24) == 0x0013);

    /* Feature walk: Windows/IMAPI needs 37h (CD-RW media write support)
       and a full 8-byte 23h (Formattable) body.  A writable disc must not
       advertise 04h (Write Protect). */
    bool has_wp = false, has_fmt = false, has_cdrw = false;
    uint8_t fmt_len = 0, cdrw_len = 0;
    for (uint32_t i = 8; i + 4 <= (uint32_t)n;) {
        uint16_t code = util_be16(buf + i);
        uint8_t alen = buf[i + 3];
        if (code == 0x0004) has_wp = true;
        if (code == 0x0023) { has_fmt = true; fmt_len = alen; }
        if (code == 0x0037) { has_cdrw = true; cdrw_len = alen; }
        i += 4u + alen;
    }
    assert(!has_wp);
    assert(has_fmt && fmt_len == 8);
    assert(has_cdrw && cdrw_len == 4);

    /* ISO media must report CD-ROM current and write-protected. */
    g_media.kind = MEDIA_KIND_ISO;
    g_media.profile = MEDIA_PROFILE_CDROM;
    g_media.writable = false;
    g_media.write_protected = true;
    g_media.blank = false;
    g_media.complete = true;
    g_media.data_lba = 1000;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n > 40);
    assert(util_be16(buf + 6) == 0x0008);
}

static void test_performance_and_disc_structure(void)
{
    reset_media();
    uint8_t cdb[16];
    uint8_t buf[512];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xAC;
    cdb[8] = 0xC8;
    cdb[9] = 0x03;
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 24);
    assert(util_be32(buf) == 20);
    assert(util_be32(buf + 8 + 4) == 358399);
    assert(util_be32(buf + 8 + 8) == 24 * 176);
    assert(util_be32(buf + 8 + 12) == 10 * 176);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xAC;
    cdb[1] = 0x14; /* Windows also asks for 14h write performance */
    cdb[8] = 0xC8;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 24);
    assert(buf[4] == 0x02);
    assert(util_be32(buf + 8 + 4) == 10 * 176);
    assert(util_be32(buf + 8 + 8) == 358399);
    assert(util_be32(buf + 8 + 12) == 10 * 176);

    /* READ DISC STRUCTURE is DVD/BD-only; a CD-RW must error, not return
       a 4 KiB zero structure. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xAD;
    cdb[7] = 0x00;
    cdb[8] = 0x10;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == -1);
    const scsi_sense_t *s = scsi_get_sense();
    assert(s->key == 0x05 && s->asc == 0x24);
}

static void test_disc_and_toc(void)
{
    reset_media();
    uint8_t cdb[16] = {0x51, 0, 0, 0, 0, 0, 0, 0, 32, 0};
    uint8_t buf[512];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 36);
    assert(util_be16(buf) == 32);
    assert((buf[2] & 0x03) == 0);       /* blank */
    assert((buf[2] & 0x10) != 0);       /* erasable */
    assert(buf[3] == 1);
    /* Captured blank CD-RW disc-information fields. */
    assert(buf[4] == 1);                /* first track */
    assert(buf[5] == 1);
    assert(buf[6] == 1);
    assert(buf[7] == 0);                /* disc type */
    assert(buf[8] == 0xFF);
    assert(util_be32(buf + 16) == 0x00612217);
    assert(buf[20] == 0 && buf[21] == 0x4F &&
           buf[22] == 0x3B && buf[23] == 0x4A);

    /* Blank READ TOC: captured drive returns 5/24, not an empty TOC. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x43;
    cdb[2] = 0;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == -1);
    const scsi_sense_t *ts = scsi_get_sense();
    assert(ts->key == 0x05 && ts->asc == 0x24);
    uint8_t rq[16] = {0x03, 0, 0, 0, 18, 0};
    assert(scsi_in_command(rq, buf, sizeof(buf)) == 18);

    g_media.blank = false;
    g_media.complete = true;
    g_media.data_lba = 100;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x51;
    cdb[8] = 32;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert((buf[2] & 0x03) == 2);       /* complete */
    assert(((buf[2] >> 2) & 3) == 3);   /* last session complete */

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x43;
    cdb[2] = 0; /* format 0, LBA */
    cdb[6] = 0;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 20);
    assert(buf[2] == 1 && buf[3] == 1);
    assert(buf[6] == 1);
    assert(util_be32(buf + 8) == 0);
    assert(buf[14] == 0xAA);
    assert(util_be32(buf + 16) == 100);

    /* MSF form: 00:02:00 for track 1. */
    cdb[1] = 0x02;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 20);
    assert(buf[8] == 0 && buf[9] == 0 && buf[10] == 2 && buf[11] == 0);

    /* Format 2 raw TOC. */
    cdb[1] = 0;
    cdb[2] = 2;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 48);
    assert(buf[7] == 0xA0);
    assert(buf[4 + 33 + 3] == 1);  /* track 1 POINT */
    assert(buf[4 + 33 + 8] == 0);  /* start 00:02:00 */
    assert(buf[4 + 33 + 9] == 2);

    /* Format 4 ATIP: required by ImgBurn for "Getting MID". */
    cdb[2] = 4;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 32);
    assert(util_be16(buf) == 30);
    assert(buf[4 + 0] == 0xC2);
    assert(buf[4 + 2] & 0x40);       /* CD-RW */
    assert(buf[4 + 4] == 97);        /* ATIP lead-in minute */
    assert(buf[4 + 8] == 79);        /* max lead-out minute */

    /* Format 3 PMA should be a harmless empty response. */
    cdb[2] = 3;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 4 && buf[1] == 2);
}

static void test_blank_track_info(void)
{
    reset_media();
    uint8_t cdb[16] = {0x52, 0x01, 0, 0, 0, 0x01, 0, 0x10, 0};
    uint8_t buf[256];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 48);
    assert(util_be16(buf) == 46);
    assert(buf[2] == 1 && buf[3] == 1);
    assert(buf[5] == 0x05);
    assert(buf[6] == 0x4F);
    assert(buf[7] == 0x01);
    assert(util_be32(buf + 20) == 358398);
    assert(util_be32(buf + 28) == 358398);
    assert(util_be32(buf + 32) == 0xFFFFFFFFu);
}

static void test_track_info(void)
{
    reset_media();
    g_media.blank = false;
    g_media.data_lba = 120;
    g_media.nwa = 120;
    g_media.session_open = true;
    uint8_t cdb[16] = {0x52, 0, 0, 0, 0, 0, 0, 0, 36, 0};
    uint8_t buf[512];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 36);
    assert(util_be16(buf) == 34);
    assert(buf[2] == 1 && buf[3] == 1);
    assert(buf[6] == 1); /* Mode 1 */
    assert(buf[7] & 1);  /* NWA valid */
    assert(util_be32(buf + 12) == 120);
    assert(util_be32(buf + 16) == 358400 - 120);
}

static void test_mode_sense(void)
{
    reset_media();
    uint8_t cdb[16] = {0x1A, 0, 0x2A, 0, 128, 0};
    uint8_t buf[512];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n > 30);
    assert(buf[0] == n - 1);
    assert(buf[4] == 0x2A);
    assert((buf[4 + 3] & 0x02) != 0); /* CD-RW write */
    assert(util_be16(buf + 4 + 8) == 24 * 176);

    cdb[0] = 0x5A;
    cdb[7] = 0; cdb[8] = 128;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 56);
    assert(util_be16(buf) == n - 2);
    assert(buf[2] == 0x20);             /* captured CD-RW medium type */
    assert(buf[8] == 0x2A && buf[9] == 0x2E); /* 46-byte page 2A */
}

static void test_config_during_ua(void)
{
    reset_media();
    scsi_notify_media_change(true);
    uint8_t cdb[16] = {0x46, 0, 0, 0, 0, 0, 0, 0, 255, 0};
    uint8_t buf[512];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n > 8);
    assert(util_be16(buf + 6) == 0x000A);
}

static void test_write_params_and_ua(void)
{
    reset_media();
    uint8_t cdb[16] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    uint32_t lba = 0, blocks = 0, src = 0;
    bool discard = false;
    bool ok = scsi_write_params(cdb, 4096, &lba, &blocks, &src, &discard);
    assert(ok && !discard && lba == 0 && blocks == 2 && src == 2048);

    g_media.writable = false;
    ok = scsi_write_params(cdb, 4096, &lba, &blocks, &src, &discard);
    assert(!ok);
    const scsi_sense_t *s = scsi_get_sense();
    assert(s->key == 0x07 && s->asc == 0x27);

    g_media.writable = true;
    /* A write wholly beyond the user area (lead-in/lead-out) is consumed
       and returned GOOD so a burn session can continue. */
    uint8_t high[16] = {0x2A, 0, 0xFF, 0xFF, 0xFF, 0x6A, 0, 0, 1, 0};
    ok = scsi_write_params(high, 2048, &lba, &blocks, &src, &discard);
    assert(ok && discard && blocks == 1);

    scsi_notify_media_change(true);
    uint8_t tcdb[16] = {0x00};
    assert(scsi_in_command(tcdb, NULL, 0) < 0);
    s = scsi_get_sense();
    assert(s->key == 0x06 && s->asc == 0x28);
    uint8_t rcdb[16] = {0x03, 0, 0, 0, 18, 0};
    uint8_t buf[32];
    assert(scsi_in_command(rcdb, buf, sizeof(buf)) == 18);
    assert(buf[2] == 0x06 && buf[12] == 0x28);
    assert(scsi_in_command(tcdb, NULL, 0) == 0);
}

static void test_event(void)
{
    reset_media();
    scsi_notify_media_event(3);
    uint8_t cdb[16] = {0x4A, 1, 0, 0, 0x00, 0, 0, 0, 8, 0};
    uint8_t buf[32];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 4);
    assert(buf[2] == 0x80 && buf[3] == 0x5E);

    cdb[4] = 0x5A;
    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 8);
    assert(util_be16(buf) == 6);
    assert(buf[2] == 0x06 && buf[3] == 0x5E);
    assert(buf[4] == 0x02);
    assert(buf[5] == 0x01);

    n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 8 && buf[4] == 0);
}

static void test_format_capacity(void)
{
    reset_media();
    uint8_t cdb[16] = {0x23, 0, 0, 0, 0, 0, 0, 0, 12, 0};
    uint8_t buf[64];
    int32_t n = scsi_in_command(cdb, buf, sizeof(buf));
    assert(n == 36);
    assert(util_be32(buf) == 32);
    assert(util_be32(buf + 4) == 358400);
    assert(buf[8] == 1); /* blank media is reported unformatted */
    assert(((buf[9] << 16) | (buf[10] << 8) | buf[11]) == 0x800);
    /* 10h: ~574 MiB formatted capacity; 11h/12h empty. */
    assert(util_be32(buf + 12) == 295264);
    assert(buf[16] == 0x40);
    assert(buf[19] == 0x20);
    assert(buf[20] == 0 && buf[24] == 0x44 && buf[32] == 0x48);
}

int main(void)
{
    scsi_init();
    test_inquiry();
    test_capacity_and_config();
    test_performance_and_disc_structure();
    test_disc_and_toc();
    test_blank_track_info();
    test_track_info();
    test_mode_sense();
    test_config_during_ua();
    test_write_params_and_ua();
    test_event();
    test_format_capacity();
    puts("scsi tests: all passed");
    return 0;
}
