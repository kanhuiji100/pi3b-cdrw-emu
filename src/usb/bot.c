#include "bot.h"

#include <pthread.h>

#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "media.h"
#include "scsi.h"
#include "util.h"

#define BOT_CBW_SIGNATURE 0x43425355u
#define BOT_CSW_SIGNATURE 0x53425355u

#define BOT_STATUS_PASSED 0u
#define BOT_STATUS_FAILED 1u
#define BOT_STATUS_PHASE_ERROR 2u

#define IO_TIMEOUT_MS 10000
#define TRACE_DEPTH 8

static bot_trace_t s_trace[TRACE_DEPTH];
static uint8_t s_trace_head;
static uint8_t s_trace_count;
static int8_t s_trace_active = -1;
static pthread_mutex_t s_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int read_exact(bot_t *b, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        int n = b->io->read(b->io->ctx, p, len, IO_TIMEOUT_MS);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            /* A zero-length bulk packet: keep waiting for real data. */
            continue;
        }
        if ((uint32_t)n > len) {
            return -1;
        }
        p += n;
        len -= (uint32_t)n;
    }
    return 0;
}

static int write_exact(bot_t *b, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        int n = b->io->write(b->io->ctx, p, len, IO_TIMEOUT_MS);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            continue;
        }
        if ((uint32_t)n > len) {
            return -1;
        }
        p += n;
        len -= (uint32_t)n;
    }
    return 0;
}

static void send_csw(bot_t *b, uint8_t status)
{
    uint32_t residue = b->total > b->done ? b->total - b->done : 0;
    put_le32(b->csw, BOT_CSW_SIGNATURE);
    put_le32(b->csw + 4, b->tag);
    put_le32(b->csw + 8, residue);
    b->csw[12] = status;
    b->status = status;
    if (b->io->write(b->io->ctx, b->csw, BOT_CSW_SIZE, IO_TIMEOUT_MS) !=
        (int)BOT_CSW_SIZE) {
        LOGF("BOT: CSW write failed");
    }
    if (status != BOT_STATUS_PASSED) {
        b->errors++;
    }
    pthread_mutex_lock(&s_trace_mutex);
    if (s_trace_active >= 0) {
        const scsi_sense_t *sn = scsi_get_sense();
        s_trace[s_trace_active].status = status;
        s_trace[s_trace_active].key = sn->key;
        s_trace[s_trace_active].asc = sn->asc;
        s_trace[s_trace_active].ascq = sn->ascq;
        s_trace_active = -1;
    }
    pthread_mutex_unlock(&s_trace_mutex);
}

static bool drain_out(bot_t *b)
{
    uint8_t scratch[4096];
    uint32_t left = b->total > b->done ? b->total - b->done : 0;
    while (left) {
        uint32_t n = left < sizeof(scratch) ? left : sizeof(scratch);
        if (read_exact(b, scratch, n) < 0) {
            return false;
        }
        b->done += n;
        left -= n;
    }
    return true;
}

static int handle_write(bot_t *b)
{
    uint32_t lba = 0;
    uint32_t blocks = 0;
    uint32_t source_block_size = 0;
    bool discard = false;

    if (b->dir_in) {
        send_csw(b, BOT_STATUS_FAILED);
        return 0;
    }
    bool ok = scsi_write_params(b->cdb, b->total, &lba, &blocks,
                                &source_block_size, &discard);
    if (!ok || source_block_size == 0 ||
        b->total != blocks * source_block_size) {
        (void)drain_out(b);
        send_csw(b, BOT_STATUS_FAILED);
        return 0;
    }
    if (discard) {
        if (!drain_out(b)) {
            return -1;
        }
        send_csw(b, BOT_STATUS_PASSED);
        return 0;
    }

    uint32_t chunk = (b->data_cap / source_block_size) * source_block_size;
    if (chunk == 0) {
        (void)drain_out(b);
        send_csw(b, BOT_STATUS_FAILED);
        return 0;
    }

    while (b->done < b->total) {
        uint32_t left = b->total - b->done;
        uint32_t n = left < chunk ? left : chunk;
        if (read_exact(b, b->data, n) < 0) {
            return -1;
        }
        uint32_t nblocks = n / source_block_size;
        uint32_t nlba = lba + b->done / source_block_size;
        if (!media_write_raw_blocks(nlba, b->data, nblocks,
                                    source_block_size)) {
            LOGF("BOT: write failed lba=%lu blocks=%lu",
                 (unsigned long)nlba, (unsigned long)nblocks);
            scsi_set_sense(0x03, 0x0C, 0x00);
            b->done += n;
            (void)drain_out(b);
            send_csw(b, BOT_STATUS_FAILED);
            return 0;
        }
        b->done += n;
    }
    send_csw(b, BOT_STATUS_PASSED);
    return 0;
}

/* Complete the IN data stage before a failed CSW.

   Windows/ImgBurn's BOT stack waits for the CBW's full DataTransferLength
   before it consumes the CSW.  If a command fails and we send only the
   13-byte CSW, the host can wait forever: that is the Explorer/ImgBurn
   hang on a blank CD-RW after READ(10) returns CHECK CONDITION.  Fill the
   remaining data stage with zeroes (the same strategy used for the
   generic IN-command path), then send a failed CSW. */
static int fail_in_stage(bot_t *b)
{
    uint32_t left = b->total > b->done ? b->total - b->done : 0;
    uint32_t chunk = b->data_cap ? b->data_cap : 1;
    memset(b->data, 0, chunk < left ? chunk : left);
    while (left) {
        uint32_t n = left < chunk ? left : chunk;
        if (write_exact(b, b->data, n) < 0) {
            return -1;
        }
        b->done += n;
        left -= n;
    }
    send_csw(b, BOT_STATUS_FAILED);
    return 0;
}

/* FunctionFS cannot stall a bulk endpoint from userspace.  For a failed IN
   command with a non-zero data stage, send one short packet so the host's
   BOT stack terminates the transfer and consumes the CSW.  Sending only the
   CSW makes Windows wait for DataTransferLength and eventually suspend the
   device; filling the whole stage with zeroes makes READ DISC STRUCTURE
   look like valid data and loops.  READ(10) keeps the full zero-fill path
   because its upper layer expects the complete buffer. */
static int fail_in_short(bot_t *b)
{
    if (b->total > 0) {
        uint8_t z = 0;
        if (write_exact(b, &z, 1) < 0) {
            return -1;
        }
        b->done = 1;
    }
    send_csw(b, BOT_STATUS_FAILED);
    return 0;
}

static int handle_read(bot_t *b)
{
    uint32_t lba = 0;
    uint32_t blocks = 0;
    if (!b->dir_in) {
        send_csw(b, BOT_STATUS_FAILED);
        return 0;
    }
    if (!scsi_read_params(b->cdb, &lba, &blocks)) {
        return fail_in_stage(b);
    }
    if (b->total == 0 || blocks == 0) {
        send_csw(b, BOT_STATUS_PASSED);
        return 0;
    }
    if (b->total != blocks * SCSI_SECTOR_SIZE) {
        return fail_in_stage(b);
    }
    uint32_t chunk = (b->data_cap / SCSI_SECTOR_SIZE) * SCSI_SECTOR_SIZE;
    while (b->done < b->total) {
        uint32_t left = b->total - b->done;
        uint32_t n = left < chunk ? left : chunk;
        uint32_t nlba = lba + b->done / SCSI_SECTOR_SIZE;
        if (!media_read_blocks(nlba, b->data, n / SCSI_SECTOR_SIZE)) {
            LOGF("BOT: read failed lba=%lu", (unsigned long)nlba);
            scsi_set_sense(0x03, 0x11, 0x00);
            return fail_in_stage(b);
        }
        if (write_exact(b, b->data, n) < 0) {
            return -1;
        }
        b->done += n;
    }
    send_csw(b, BOT_STATUS_PASSED);
    return 0;
}

static int handle_out_command(bot_t *b)
{
    if (b->total > b->out_cap) {
        (void)drain_out(b);
        send_csw(b, BOT_STATUS_FAILED);
        return 0;
    }
    if (read_exact(b, b->out, b->total) < 0) {
        return -1;
    }
    b->done = b->total;
    int32_t rc = scsi_out_command(b->cdb, b->out, b->total);
    send_csw(b, rc < 0 ? BOT_STATUS_FAILED : BOT_STATUS_PASSED);
    return 0;
}

static int handle_in_command(bot_t *b)
{
    int32_t rsp = scsi_in_command(b->cdb, b->data, b->data_cap);
    if (rsp < 0) {
        /* READ TOC error: complete the full data stage with zeroes, as the
           older working blank-CD path did; Windows then shows the burn
           dialog.  AD uses a short packet to avoid its retry loop. */
        if (b->cdb[0] == 0x43) {
            return fail_in_stage(b);
        }
        return fail_in_short(b);
    }
    if (b->total == 0) {
        send_csw(b, BOT_STATUS_PASSED);
        return 0;
    }
    if (rsp == 0) {
        return fail_in_short(b);
    }
    uint32_t n = (uint32_t)rsp < b->total ? (uint32_t)rsp : b->total;
    if (write_exact(b, b->data, n) < 0) {
        return -1;
    }
    b->done = n;
    send_csw(b, BOT_STATUS_PASSED);
    return 0;
}

uint8_t bot_trace_count(void)
{
    pthread_mutex_lock(&s_trace_mutex);
    uint8_t n = s_trace_count;
    pthread_mutex_unlock(&s_trace_mutex);
    return n;
}

bool bot_trace_get(uint8_t index, bot_trace_t *out)
{
    if (!out) {
        return false;
    }
    pthread_mutex_lock(&s_trace_mutex);
    if (index >= s_trace_count) {
        pthread_mutex_unlock(&s_trace_mutex);
        return false;
    }
    uint8_t oldest = (uint8_t)((s_trace_head + TRACE_DEPTH - s_trace_count) % TRACE_DEPTH);
    *out = s_trace[(oldest + index) % TRACE_DEPTH];
    pthread_mutex_unlock(&s_trace_mutex);
    return true;
}

void bot_init(bot_t *b, bot_io_t *io, uint8_t *data, uint32_t data_cap,
              uint8_t *out, uint32_t out_cap)
{
    memset(b, 0, sizeof(*b));
    b->io = io;
    b->data = data;
    b->data_cap = data_cap;
    b->out = out;
    b->out_cap = out_cap;
    bot_reset(b);
}

void bot_reset(bot_t *b)
{
    b->tag = 0;
    b->total = 0;
    b->done = 0;
    b->dir_in = 0;
    b->cdb_len = 0;
    b->status = 0xFF;
    memset(b->cdb, 0, sizeof(b->cdb));
    scsi_bot_reset();
}

int bot_handle_cbw(bot_t *b, const uint8_t cbw[BOT_CBW_SIZE])
{
    memcpy(b->cbw, cbw, BOT_CBW_SIZE);
    b->tag = le32(b->cbw + 4);
    b->total = le32(b->cbw + 8);
    b->done = 0;
    b->dir_in = (b->cbw[12] & 0x80) ? 1 : 0;
    b->cdb_len = b->cbw[14] & 0x1F;

    if (le32(b->cbw) != BOT_CBW_SIGNATURE || b->cbw[13] != 0 ||
        b->cdb_len < 1 || b->cdb_len > 16) {
        LOGF("BOT: invalid CBW sig=%08lX lun=%u cdb_len=%u",
             (unsigned long)le32(b->cbw), b->cbw[13], b->cdb_len);
        b->io->stall(b->io->ctx, true);
        b->io->stall(b->io->ctx, false);
        return -2;
    }
    memset(b->cdb, 0, sizeof(b->cdb));
    memcpy(b->cdb, b->cbw + 15, b->cdb_len);
    b->commands++;

    uint8_t op = b->cdb[0];
    pthread_mutex_lock(&s_trace_mutex);
    uint8_t slot = s_trace_head;
    memset(&s_trace[slot], 0, sizeof(s_trace[slot]));
    s_trace[slot].opcode = op;
    s_trace[slot].dir_in = b->dir_in;
    s_trace[slot].cdb_len = b->cdb_len;
    s_trace[slot].data_len = b->total;
    s_trace[slot].status = 0xFF;
    s_trace[slot].lba = util_be32(b->cdb + 2);
    s_trace[slot].blocks = (op == 0x28 || op == 0x2A || op == 0x2F) ?
                               util_be16(b->cdb + 7) : util_be32(b->cdb + 6);
    if ((op == 0x2A || op == 0xAA || op == 0x2E || op == 0xAE) &&
        s_trace[slot].blocks) {
        s_trace[slot].source_block_size = b->total / s_trace[slot].blocks;
    }
    s_trace_active = (int8_t)slot;
    s_trace_head = (uint8_t)((s_trace_head + 1) % TRACE_DEPTH);
    if (s_trace_count < TRACE_DEPTH) {
        s_trace_count++;
    }
    pthread_mutex_unlock(&s_trace_mutex);
    bool is_read = (op == 0x28 || op == 0xA8 || op == 0xBE);
    if (!is_read) {
        LOGF("BOT: CBW op=%02X dir=%s len=%lu lba=%lu cdb=%02X %02X %02X",
             op, b->dir_in ? "IN" : "OUT", (unsigned long)b->total,
             (unsigned long)util_be32(b->cdb + 2), b->cdb[0], b->cdb[1],
             b->cdb[2]);
    }

    int rc;
    if (scsi_is_write_command(op)) {
        rc = handle_write(b);
    } else if (scsi_is_read_command(op)) {
        rc = handle_read(b);
    } else if (!b->dir_in && b->total > 0) {
        rc = handle_out_command(b);
    } else {
        rc = handle_in_command(b);
    }
    if (!is_read) {
        LOGF("BOT: op=%02X status=%u residue=%lu", op, b->status,
             (unsigned long)(b->total > b->done ? b->total - b->done : 0));
    }
    return rc;
}

int bot_run(bot_t *b)
{
    for (;;) {
        int n = b->io->read(b->io->ctx, b->cbw, BOT_CBW_SIZE, -1);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            continue;
        }
        if (n != (int)BOT_CBW_SIZE) {
            LOGF("BOT: short CBW (%d bytes)", n);
            b->io->stall(b->io->ctx, true);
            b->io->stall(b->io->ctx, false);
            continue;
        }
        if (bot_handle_cbw(b, b->cbw) < 0) {
            return -1;
        }
    }
}
