/*
 * Drives the transport-independent BOT engine through an in-memory pipe.
 * Covers CBW parsing, INQUIRY, READ CAPACITY, READ(10) from an ISO,
 * WRITE(10) into a blank CD-RW image and invalid-CBW handling.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bot.h"
#include "media.h"
#include "scsi.h"
#include "storage.h"
#include "util.h"

typedef struct {
    uint8_t out[128 * 1024];
    size_t out_len;
    size_t out_pos;
    uint8_t in[160 * 1024];
    size_t in_len;
    int stalls;
} mock_t;

static mock_t g_mock;

static int mock_read(void *ctx, void *buf, uint32_t len, int timeout_ms)
{
    (void)timeout_ms;
    mock_t *m = (mock_t *)ctx;
    size_t left = m->out_len - m->out_pos;
    size_t n = left < len ? left : len;
    if (n == 0) {
        return 0;
    }
    memcpy(buf, m->out + m->out_pos, n);
    m->out_pos += n;
    return (int)n;
}

static int mock_write(void *ctx, const void *buf, uint32_t len, int timeout_ms)
{
    (void)timeout_ms;
    mock_t *m = (mock_t *)ctx;
    assert(m->in_len + len <= sizeof(m->in));
    memcpy(m->in + m->in_len, buf, len);
    m->in_len += len;
    return (int)len;
}

static void mock_stall(void *ctx, bool ep_out)
{
    (void)ep_out;
    ((mock_t *)ctx)->stalls++;
}

static void mock_clear(void *ctx, bool ep_out)
{
    (void)ctx;
    (void)ep_out;
}

static bot_io_t g_io = {
    .ctx = &g_mock,
    .read = mock_read,
    .write = mock_write,
    .stall = mock_stall,
    .clear_halt = mock_clear,
};

static bot_t g_bot;
static uint8_t g_data[64 * 1024];
static uint8_t g_out[8 * 1024];

static void reset_mock(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
    g_io.ctx = &g_mock;
}

static void make_cbw(uint8_t cbw[31], uint32_t tag, uint32_t len, bool in,
                     const uint8_t *cdb, uint8_t cdb_len)
{
    memset(cbw, 0, 31);
    cbw[0] = 0x55;
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43;
    cbw[4] = (uint8_t)tag;
    cbw[5] = (uint8_t)(tag >> 8);
    cbw[6] = (uint8_t)(tag >> 16);
    cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)len;
    cbw[9] = (uint8_t)(len >> 8);
    cbw[10] = (uint8_t)(len >> 16);
    cbw[11] = (uint8_t)(len >> 24);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
}

static const uint8_t *last_csw(uint8_t status)
{
    assert(g_mock.in_len >= 13);
    const uint8_t *csw = g_mock.in + g_mock.in_len - 13;
    assert(csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 && csw[3] == 0x53);
    assert(csw[12] == status);
    return csw;
}

static void test_inquiry(void)
{
    reset_mock();
    uint8_t cdb[16] = {0x12, 0, 0, 0, 36, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 0x1234, 36, true, cdb, 6);
    assert(bot_handle_cbw(&g_bot, cbw) == 0);
    last_csw(0);
    assert(g_mock.in_len == 36 + 13);
    assert((g_mock.in[0] & 0x1F) == 5);
    assert(memcmp(g_mock.in + 8, "OrangePi", 8) == 0);
}

static void test_capacity(void)
{
    reset_mock();
    uint8_t cdb[16] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 1, 8, true, cdb, 10);
    assert(bot_handle_cbw(&g_bot, cbw) == 0);
    last_csw(0);
    assert(g_mock.in_len == 8 + 13);
    const media_status_t *m = media_get();
    assert(util_be32(g_mock.in) == m->capacity_lba - 1);
}

static void test_read_iso(void)
{
    reset_mock();
    uint8_t cdb[16] = {0x28, 0, 0, 0, 0, 16, 0, 0, 1, 0, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 0x77, 2048, true, cdb, 10);
    assert(bot_handle_cbw(&g_bot, cbw) == 0);
    last_csw(0);
    assert(g_mock.in_len == 2048 + 13);
    assert(memcmp(g_mock.in + 1, "CD001", 5) == 0);

    uint8_t expect[2048];
    FILE *fp = fopen("examples/demo.iso", "rb");
    assert(fp);
    assert(fseek(fp, 16 * 2048, SEEK_SET) == 0);
    assert(fread(expect, 1, sizeof(expect), fp) == sizeof(expect));
    fclose(fp);
    assert(memcmp(expect, g_mock.in, sizeof(expect)) == 0);
}

static void test_read_blank_cdr(void)
{
    char root[] = "/tmp/cdrw-bot-blank-XXXXXX";
    assert(mkdtemp(root) != NULL);
    storage_set_root(root);
    assert(storage_init());
    char path[512];
    snprintf(path, sizeof(path), "%s/discs/BLANK.CDR", root);
    assert(media_create_blank(path, 1000));
    assert(media_load(path));

    reset_mock();
    uint8_t cdb[16] = {0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 0xB1, 4096, true, cdb, 10);
    assert(bot_handle_cbw(&g_bot, cbw) == 0);
    /* A failed READ(10) must still complete the 4096-byte IN stage, then
       send the CSW; otherwise Windows/ImgBurn wait for data forever. */
    last_csw(1);
    assert(g_mock.in_len == 4096 + 13);
    for (size_t i = 0; i < 4096; i++) {
        assert(g_mock.in[i] == 0);
    }
    const uint8_t *csw = g_mock.in + g_mock.in_len - 13;
    assert(csw[8] == 0 && csw[9] == 0 && csw[10] == 0 && csw[11] == 0);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
}

static void test_write_cdr(void)
{
    char root[] = "/tmp/cdrw-bot-XXXXXX";
    assert(mkdtemp(root) != NULL);
    storage_set_root(root);
    assert(storage_init());
    char path[512];
    snprintf(path, sizeof(path), "%s/discs/BOT.CDR", root);
    assert(media_create_blank(path, 1000));

    reset_mock();
    uint8_t cdb[16] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 0x99, 4096, false, cdb, 10);
    g_mock.out_len = 4096;
    for (size_t i = 0; i < g_mock.out_len; i++) {
        g_mock.out[i] = (uint8_t)(i * 3 + 1);
    }
    assert(bot_handle_cbw(&g_bot, cbw) == 0);
    last_csw(0);

    const media_status_t *m = media_get();
    assert(m->data_lba == 2);
    assert(m->nwa == 2);
    assert(!m->blank);

    static uint8_t check[4096];
    assert(media_read_blocks(0, check, 2));
    assert(memcmp(check, g_mock.out, sizeof(check)) == 0);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
}

static void test_invalid_cbw(void)
{
    reset_mock();
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    cbw[0] = 0xAA;
    cbw[14] = 6;
    int rc = bot_handle_cbw(&g_bot, cbw);
    assert(rc == -2);
    (void)rc;
}

int main(void)
{
    assert(media_load("examples/demo.iso"));
    bot_init(&g_bot, &g_io, g_data, sizeof(g_data), g_out, sizeof(g_out));

    test_inquiry();
    test_capacity();
    test_read_iso();
    test_read_blank_cdr();
    test_write_cdr();
    test_invalid_cbw();

    media_shutdown();
    printf("test_bot: all passed\n");
    return 0;
}
