#include "ff.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static FRESULT errno_to_fr(int err)
{
    switch (err) {
    case 0: return FR_OK;
    case ENOENT: return FR_NO_FILE;
    case EACCES:
    case EPERM: return FR_DENIED;
    case EEXIST: return FR_EXIST;
    case ENOSPC: return FR_DISK_ERR;
    default: return FR_DISK_ERR;
    }
}

static int refresh_size(FIL *fp)
{
    struct stat st;
    if (!fp || !fp->in_use) {
        return -1;
    }
    if (fstat(fp->fd, &st) != 0) {
        return -1;
    }
    fp->size = (FSIZE_T)st.st_size;
    return 0;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    if (!fp || !path) {
        return FR_INVALID_PARAMETER;
    }
    memset(fp, 0, sizeof(*fp));
    fp->fd = -1;

    int flags = 0;
    if ((mode & FA_READ) && (mode & FA_WRITE)) {
        flags = O_RDWR;
    } else if (mode & FA_WRITE) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    if (mode & FA_CREATE_NEW) {
        flags |= O_CREAT | O_EXCL;
    } else if (mode & FA_CREATE_ALWAYS) {
        flags |= O_CREAT | O_TRUNC;
    } else if (mode & FA_OPEN_ALWAYS) {
        flags |= O_CREAT;
    }

    int fd = open(path, flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        return errno_to_fr(errno);
    }
    fp->fd = fd;
    fp->flags = mode;
    fp->in_use = 1;
    if (refresh_size(fp) != 0) {
        close(fd);
        fp->in_use = 0;
        return FR_DISK_ERR;
    }
    if (mode & FA_OPEN_APPEND) {
        fp->pos = fp->size;
    }
    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    if (!fp || !fp->in_use) {
        return FR_INVALID_OBJECT;
    }
    int rc = close(fp->fd);
    fp->in_use = 0;
    fp->fd = -1;
    return rc == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    if (br) {
        *br = 0;
    }
    if (!fp || !fp->in_use || (!buff && btr)) {
        return FR_INVALID_OBJECT;
    }
    if (btr == 0) {
        return FR_OK;
    }
    ssize_t n = pread(fp->fd, buff, btr, (off_t)fp->pos);
    if (n < 0) {
        return errno_to_fr(errno);
    }
    fp->pos += (FSIZE_T)n;
    if (br) {
        *br = (UINT)n;
    }
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    if (bw) {
        *bw = 0;
    }
    if (!fp || !fp->in_use || (!buff && btw)) {
        return FR_INVALID_OBJECT;
    }
    if (btw == 0) {
        return FR_OK;
    }
    ssize_t n = pwrite(fp->fd, buff, btw, (off_t)fp->pos);
    if (n < 0) {
        return errno_to_fr(errno);
    }
    fp->pos += (FSIZE_T)n;
    if (fp->pos > fp->size) {
        fp->size = fp->pos;
    }
    if (bw) {
        *bw = (UINT)n;
    }
    return (UINT)n == btw ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, FSIZE_T ofs)
{
    if (!fp || !fp->in_use) {
        return FR_INVALID_OBJECT;
    }
    /* FatFs grows the file when seeking past the end. */
    if (ofs > fp->size) {
        if (ftruncate(fp->fd, (off_t)ofs) != 0) {
            return FR_DISK_ERR;
        }
        fp->size = ofs;
    }
    fp->pos = ofs;
    return FR_OK;
}

FRESULT f_sync(FIL *fp)
{
    if (!fp || !fp->in_use) {
        return FR_INVALID_OBJECT;
    }
    return fsync(fp->fd) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_expand(FIL *fp, FSIZE_T fsz, BYTE opt)
{
    if (!fp || !fp->in_use) {
        return FR_INVALID_OBJECT;
    }
    if (fsz == 0) {
        return FR_OK;
    }
    if (fallocate(fp->fd, 0, 0, (off_t)fsz) != 0) {
        if (posix_fallocate(fp->fd, 0, (off_t)fsz) != 0) {
            if (ftruncate(fp->fd, (off_t)fsz) != 0) {
                return FR_DISK_ERR;
            }
        }
    }
    (void)opt;
    return refresh_size(fp) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    struct stat st;
    if (!path) {
        return FR_INVALID_PARAMETER;
    }
    if (stat(path, &st) != 0) {
        return errno_to_fr(errno);
    }
    if (fno) {
        memset(fno, 0, sizeof(*fno));
        fno->fsize = (FSIZE_T)st.st_size;
        fno->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : AM_ARC;
    }
    return FR_OK;
}

FRESULT f_unlink(const TCHAR *path)
{
    if (!path || unlink(path) != 0) {
        return errno_to_fr(errno);
    }
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path)
{
    if (!path) {
        return FR_INVALID_PARAMETER;
    }
    if (mkdir(path, 0755) == 0) {
        return FR_OK;
    }
    return errno == EEXIST ? FR_EXIST : errno_to_fr(errno);
}

FRESULT f_rename(const TCHAR *oldname, const TCHAR *newname)
{
    if (!oldname || !newname || rename(oldname, newname) != 0) {
        return errno_to_fr(errno);
    }
    return FR_OK;
}

FSIZE_T f_size(FIL *fp)
{
    if (!fp || !fp->in_use) {
        return 0;
    }
    (void)refresh_size(fp);
    return fp->size;
}
