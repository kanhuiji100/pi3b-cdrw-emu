/*
 * Host-side test of the POSIX media backend and the shared SCSI engine.
 *
 * Exercises blank-disc creation, WRITE/READ, sidecar metadata, erase and
 * ISO9660 loading against real files, then checks a few SCSI responses.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "media.h"
#include "scsi.h"
#include "storage.h"
#include "util.h"

static const char *iso_path = "examples/demo.iso";

static void write_pattern(uint32_t lba, uint32_t blocks, uint8_t seed)
{
    static uint8_t buf[64 * 2048];
    for (uint32_t i = 0; i < blocks * 2048; i++) {
        buf[i] = (uint8_t)(seed + i * 7u);
    }
    assert(media_write_blocks(lba, buf, blocks));
}

static void check_pattern(uint32_t lba, uint32_t blocks, uint8_t seed)
{
    static uint8_t buf[64 * 2048];
    assert(media_read_blocks(lba, buf, blocks));
    for (uint32_t i = 0; i < blocks * 2048; i++) {
        assert(buf[i] == (uint8_t)(seed + i * 7u));
    }
}

int main(void)
{
    char root[] = "/tmp/cdrw-media-XXXXXX";
    assert(mkdtemp(root) != NULL);
    storage_set_root(root);
    assert(storage_init());
    media_init();
    scsi_init();

    char cdr[512];
    snprintf(cdr, sizeof(cdr), "%s/discs/TEST.CDR", root);
    assert(media_create_blank(cdr, 1000));
    const media_status_t *m = media_get();
    assert(m->present && m->writable && m->blank);
    assert(m->capacity_lba == 1000);
    assert(m->kind == MEDIA_KIND_CDRW);

    write_pattern(10, 4, 0x11);
    check_pattern(10, 4, 0x11);
    m = media_get();
    assert(!m->blank);
    assert(m->session_open);
    assert(m->data_lba == 14);
    assert(m->nwa == 14);
    assert(media_sync());

    /* Reload from disk: the sidecar must restore the same state. */
    assert(media_load(cdr));
    m = media_get();
    assert(!m->blank && !m->complete && m->session_open);
    assert(m->data_lba == 14 && m->nwa == 14);
    check_pattern(10, 4, 0x11);

    assert(media_close_session(true));
    assert(media_get()->complete);

    assert(media_blank());
    m = media_get();
    assert(m->blank && m->data_lba == 0 && m->nwa == 0);
    static uint8_t zero[2048];
    assert(media_read_blocks(0, zero, 1));
    for (size_t i = 0; i < sizeof(zero); i++) {
        assert(zero[i] == 0);
    }

    /* Windows chooses a ~574 MiB formatted capacity for a 700 MiB CD-RW.
       FORMAT UNIT must make the media appendable at that capacity instead
       of leaving it blank. */
    assert(media_format_capacity(800));
    m = media_get();
    assert(!m->blank && m->formatted && m->complete && !m->session_open);
    assert(m->capacity_lba == 800 && m->data_lba == 0);
    assert(media_blank());
    assert(media_get()->blank);

    /* Same through the SCSI FORMAT UNIT parameter list (FMTDATA=1). */
    uint8_t fcdb[16] = {0x04, 0x11, 0, 0, 0, 0};
    uint8_t fdata[12] = {0x00, 0x02, 0x00, 0x08,
                         0x00, 0x00, 0x02, 0x58, /* 600 blocks */
                         0x40, 0x00, 0x00, 0x20};
    assert(scsi_out_command(fcdb, fdata, sizeof(fdata)) == 0);
    m = media_get();
    assert(!m->blank && m->formatted && m->complete && !m->session_open);
    assert(m->capacity_lba == 600);

    uint8_t tur[16] = {0};
    assert(scsi_in_command(tur, NULL, 0) < 0);
    const scsi_sense_t *fs = scsi_get_sense();
    assert(fs->key == 0x02 && fs->asc == 0x04 && fs->ascq == 0x04);

    uint8_t rcap[16] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t rbuf[256];
    assert(scsi_in_command(rcap, rbuf, sizeof(rbuf)) == 8);
    assert(util_be32(rbuf) == 599); /* capacity remains valid */
    uint8_t rtoc[16] = {0x43, 0, 0, 0, 0, 0, 0, 0, 0x20, 0};
    assert(scsi_in_command(rtoc, rbuf, sizeof(rbuf)) == -1); /* no TOC before UDF */
    uint8_t rdi[16] = {0x51, 0, 0, 0, 0, 0, 0, 0, 32, 0};
    assert(scsi_in_command(rdi, rbuf, sizeof(rbuf)) == 34);
    assert(((rbuf[2] >> 2) & 0x03) == 3); /* COMPLETE/state 3 */
    assert((rbuf[2] & 0x03) == 2);         /* status 2 */
    assert(rbuf[7] == 0x80);
    assert(rbuf[8] == 0);
    assert(rbuf[4] == 1);
    uint8_t rread[16] = {0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    uint32_t rlba = 1, rblocks = 1;
    assert(scsi_read_params(rread, &rlba, &rblocks));
    assert(rlba == 0 && rblocks == 2); /* formatted-empty reads zeros */
    uint8_t rtrack[16] = {0x52, 0x01, 0, 0, 0, 0x01, 0, 0x10, 0};
    assert(scsi_in_command(rtrack, rbuf, sizeof(rbuf)) == 36);
    assert(rbuf[6] == 0xB1); /* complete track flags from full-format capture */
    assert(util_be32(rbuf + 24) == 600);
    assert(util_be32(rbuf + 28) == 599);
    /* The formatter can now write UDF data starting at LBA 0. */
    write_pattern(0, 2, 0x5a);
    check_pattern(0, 2, 0x5a);
    assert(!media_get()->blank);

    /* ISO9660 image: read-only and addressable through the SCSI layer. */
    assert(media_load(iso_path));
    m = media_get();
    assert(m->present && m->kind == MEDIA_KIND_ISO && !m->writable);
    assert(m->write_protected);
    assert(m->capacity_lba > 16);

    uint8_t sector[2048];
    assert(media_read_blocks(16, sector, 1));
    assert(memcmp(sector + 1, "CD001", 5) == 0);

    uint8_t cdb[16] = {0x12, 0, 0, 0, 36, 0};
    uint8_t resp[512];
    int32_t n = scsi_in_command(cdb, resp, sizeof(resp));
    assert(n == 36);
    assert((resp[0] & 0x1F) == 5);
    assert(memcmp(resp + 8, "OrangePi", 8) == 0);
    assert(memcmp(resp + 16, "DVD/BD USB 2.0  ", 16) == 0);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    n = scsi_in_command(cdb, resp, sizeof(resp));
    assert(n == 8);
    assert(util_be32(resp) == m->capacity_lba - 1);
    assert(util_be32(resp + 4) == 2048);


    /* DVD-RW profile: format, write, close, and SCSI profile reporting. */
    assert(media_profile_from_name("dvd-rw") == MEDIA_PROFILE_DVDRW);
    assert(media_profile_from_name("bd-re") == MEDIA_PROFILE_BDRE);
    char dvd[512];
    snprintf(dvd, sizeof(dvd), "%s/discs/TEST.DVD", root);
    assert(media_create_blank_profile(dvd, MEDIA_PROFILE_DVDRW, 10000));
    m = media_get();
    assert(m->present && m->kind == MEDIA_KIND_DVDRW);
    assert(m->profile == MEDIA_PROFILE_DVDRW);
    assert(m->blank && m->writable && m->capacity_lba == 10000);
    assert(media_format());
    m = media_get();
    assert(!m->blank && m->session_open && !m->complete);
    write_pattern(10, 2, 0x33);
    check_pattern(10, 2, 0x33);
    assert(media_close_session(true));
    assert(media_get()->complete);
    uint32_t dvd_data = media_get()->data_lba;
    media_shutdown();
    assert(media_load(dvd));
    m = media_get();
    assert(m->profile == MEDIA_PROFILE_DVDRW);
    assert(m->complete && m->data_lba == dvd_data);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x46;
    cdb[8] = 0xFF;
    n = scsi_in_command(cdb, resp, sizeof(resp));
    assert(n > 40);
    assert(util_be16(resp + 6) == MEDIA_PROFILE_DVDRW);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x23;
    cdb[8] = 64;
    n = scsi_in_command(cdb, resp, sizeof(resp));
    assert(n == 44);
    assert((resp[8] >> 2) == 0x00);
    assert((resp[16] >> 2) == 0x00);
    assert((resp[24] >> 2) == 0x15);
    assert((resp[32] >> 2) == 0x10);
    assert((resp[40] >> 2) == 0x13);

    /* BD-RE profile. */
    char bd[512];
    snprintf(bd, sizeof(bd), "%s/discs/TEST.BD", root);
    assert(media_create_blank_profile(bd, MEDIA_PROFILE_BDRE, 20000));
    m = media_get();
    assert(m->profile == MEDIA_PROFILE_BDRE && m->kind == MEDIA_KIND_BDRE);
    assert(m->blank && m->writable);
    assert(media_format());
    write_pattern(100, 2, 0x55);
    check_pattern(100, 2, 0x55);
    assert(media_close_session(true));
    uint32_t bd_data = media_get()->data_lba;
    media_shutdown();
    assert(media_load(bd));
    m = media_get();
    assert(m->profile == MEDIA_PROFILE_BDRE);
    assert(m->complete && m->data_lba == bd_data);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x46;
    cdb[8] = 0xFF;
    n = scsi_in_command(cdb, resp, sizeof(resp));
    assert(n > 40);
    assert(util_be16(resp + 6) == MEDIA_PROFILE_BDRE);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x23;
    cdb[8] = 64;
    n = scsi_in_command(cdb, resp, sizeof(resp));
    assert(n == 28);
    assert((resp[8] >> 2) == 0x00);
    assert((resp[16] >> 2) == 0x30);
    assert((resp[24] >> 2) == 0x31);

    /* Image deletion: file + sidecar + .last_disc cleanup. */
    char del[512], delmeta[512];
    snprintf(del, sizeof(del), "%s/discs/DEL.CDR", root);
    snprintf(delmeta, sizeof(delmeta), "%s.meta", del);
    FILE *df = fopen(del, "wb");
    assert(df != NULL);
    assert(fwrite("DEL", 1, 3, df) == 3);
    fclose(df);
    df = fopen(delmeta, "wb");
    assert(df != NULL);
    assert(fwrite("META", 1, 4, df) == 4);
    fclose(df);
    assert(storage_save_last_disc(del));
    assert(storage_delete_disc(del));
    assert(access(del, F_OK) != 0);
    assert(access(delmeta, F_OK) != 0);
    char lastpath[512];
    assert(!storage_load_last_disc(lastpath, sizeof(lastpath)));

    media_shutdown();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    printf("test_media: all passed\n");
    return 0;
}
