#ifndef CDRW_SCSI_H
#define CDRW_SCSI_H

#include <stdbool.h>
#include <stdint.h>

#define SCSI_SECTOR_SIZE 2048u
#define SCSI_CDB_MAX 16u

typedef struct {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
} scsi_sense_t;

void scsi_init(void);
void scsi_bot_reset(void);

/* Data-in commands, including zero-data commands.  Returns the number of
 * bytes available in response buffer, or -1 on CHECK CONDITION. */
int32_t scsi_in_command(const uint8_t cdb[SCSI_CDB_MAX], uint8_t *buf, uint32_t cap);

/* Commands with a data-out parameter list (MODE SELECT, ...). */
int32_t scsi_out_command(const uint8_t cdb[SCSI_CDB_MAX], const uint8_t *data,
                         uint32_t len);

bool scsi_is_read_command(uint8_t opcode);
bool scsi_is_write_command(uint8_t opcode);

/* Validate READ/WRITE(10/12) and return the transfer parameters. */
bool scsi_read_params(const uint8_t cdb[SCSI_CDB_MAX], uint32_t *lba, uint32_t *blocks);
bool scsi_write_params(const uint8_t cdb[SCSI_CDB_MAX], uint32_t data_len,
                       uint32_t *lba, uint32_t *blocks, uint32_t *source_block_size,
                       bool *discard);

typedef struct {
    uint32_t lba;
    uint32_t blocks;
    uint32_t source_block_size;
    uint8_t discard;
    uint8_t reason;   /* internal */
} scsi_write_debug_t;

const scsi_write_debug_t *scsi_write_debug(void);
void scsi_set_sense(uint8_t key, uint8_t asc, uint8_t ascq);

const scsi_sense_t *scsi_get_sense(void);
bool scsi_ua_pending(void);
bool scsi_media_locked(void);
void scsi_set_serial(const char *serial);

void scsi_notify_media_change(bool inserted);
void scsi_notify_media_event(uint8_t event_code);

#endif
