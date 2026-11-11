/*
 * Minimal FatFs-compatible file API implemented on top of POSIX.
 *
 * The SCSI/media core is shared verbatim with the RP2350 firmware, which is
 * written against FatFs.  This shim provides just enough of the FatFs file
 * API for media.c to run unchanged on Linux.  Directory enumeration and
 * free-space queries are handled natively in storage_posix.c.
 */
#ifndef CDRW_POSIX_FF_H
#define CDRW_POSIX_FF_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef char TCHAR;
typedef uint8_t BYTE;
typedef unsigned int UINT;
typedef uint64_t FSIZE_T;
typedef uint64_t FSIZE_t;
typedef uint64_t QWORD;
typedef int FRESULT;

enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
};

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define FA_OPEN_APPEND 0x30

#define AM_DIR 0x10
#define AM_ARC 0x20

/* A FatFs FIL maps one-to-one onto a POSIX file descriptor. */
typedef struct {
    int fd;
    int flags;
    int in_use;
    FSIZE_T size;
    FSIZE_T pos;
} FIL;

typedef struct {
    FSIZE_T fsize;
    BYTE fattrib;
    char fname[256];
} FILINFO;

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_T ofs);
FRESULT f_sync(FIL *fp);
FRESULT f_expand(FIL *fp, FSIZE_T fsz, BYTE opt);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_mkdir(const TCHAR *path);
FRESULT f_rename(const TCHAR *oldname, const TCHAR *newname);
FSIZE_T f_size(FIL *fp);

#endif /* CDRW_POSIX_FF_H */
