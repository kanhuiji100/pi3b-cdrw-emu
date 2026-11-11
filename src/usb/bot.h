/*
 * Transport-independent USB Mass Storage Bulk-Only Transport engine.
 *
 * The engine owns the CBW/CSW state machine and delegates endpoint I/O to a
 * small HAL.  The Linux FunctionFS backend implements the HAL with f_fs
 * endpoints; tests can drive the same engine through memory pipes.
 */
#ifndef CDRW_BOT_H
#define CDRW_BOT_H

#include <stdbool.h>
#include <stdint.h>

#define BOT_CBW_SIZE 31u
#define BOT_CSW_SIZE 13u

typedef struct {
    void *ctx;
    /* Return bytes transferred, 0 on timeout, -1 on error/disconnect. */
    int (*read)(void *ctx, void *buf, uint32_t len, int timeout_ms);
    int (*write)(void *ctx, const void *buf, uint32_t len, int timeout_ms);
    void (*stall)(void *ctx, bool ep_out);
    void (*clear_halt)(void *ctx, bool ep_out);
} bot_io_t;

typedef struct {
    uint8_t opcode;
    uint8_t dir_in;
    uint8_t status;
    uint8_t cdb_len;
    uint32_t data_len;
    uint32_t lba;
    uint32_t blocks;
    uint32_t source_block_size;
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
    uint8_t discard;
} bot_trace_t;

typedef struct {
    bot_io_t *io;
    uint8_t cbw[BOT_CBW_SIZE];
    uint8_t csw[BOT_CSW_SIZE];
    uint8_t cdb[16];
    uint32_t tag;
    uint32_t total;
    uint32_t done;
    uint8_t dir_in;
    uint8_t cdb_len;
    uint8_t status;
    uint8_t *data;
    uint32_t data_cap;
    uint8_t *out;
    uint32_t out_cap;
    uint32_t commands;
    uint32_t errors;
} bot_t;

void bot_init(bot_t *b, bot_io_t *io, uint8_t *data, uint32_t data_cap,
              uint8_t *out, uint32_t out_cap);

/* Forget any in-flight transfer (BOT reset / bus reset). */
void bot_reset(bot_t *b);

/* Process one CBW supplied by the caller.  Returns 0 on success, -1 on a
 * transport error, -2 on a protocol error (endpoints were stalled). */
int bot_handle_cbw(bot_t *b, const uint8_t cbw[BOT_CBW_SIZE]);

/* Read CBWs forever and execute them; returns -1 when the transport dies. */
int bot_run(bot_t *b);

/* Recent command ring for the OLED SCSI-debug page (single-threaded). */
uint8_t bot_trace_count(void);
bool bot_trace_get(uint8_t index, bot_trace_t *out);

#endif /* CDRW_BOT_H */
