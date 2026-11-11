#ifndef CDRW_MEDIA_LOCK_H
#define CDRW_MEDIA_LOCK_H

/* Serializes SCSI command execution (BOT thread) with local UI actions
 * (UI thread).  Recursive because a SCSI handler may call a helper that
 * also takes it. */
void media_lock(void);
void media_unlock(void);

#endif
