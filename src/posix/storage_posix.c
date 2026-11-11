/*
 * POSIX storage backend for the CD-RW emulator.
 *
 * Replaces the RP2350 FatFs + SPI microSD layer with ordinary files in a
 * directory.  The API seen by media.c/scsi.c is unchanged, so the optical
 * command engine stays byte-for-byte identical to the firmware.
 */
#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char s_root[STORAGE_PATH_MAX - 64] = ".";
static bool s_ready;
static storage_error_t s_error = STORAGE_ERR_SD_INIT;

static void join_path(char *out, size_t out_size, const char *rel)
{
    if (rel && rel[0] == '/') {
        util_safe_copy(out, out_size, rel);
        return;
    }
    snprintf(out, out_size, "%s/%s", s_root, rel ? rel : "");
}

static bool ensure_dir(const char *path)
{
    char tmp[PATH_MAX];
    util_safe_copy(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

void storage_set_root(const char *root)
{
    if (root && root[0]) {
        util_safe_copy(s_root, sizeof(s_root), root);
    }
}

const char *storage_get_root(void)
{
    return s_root;
}

bool storage_init(void)
{
    if (!ensure_dir(s_root)) {
        s_ready = false;
        s_error = STORAGE_ERR_SD_INIT;
        return false;
    }
    char discs[PATH_MAX];
    join_path(discs, sizeof(discs), "discs");
    if (!ensure_dir(discs)) {
        s_ready = false;
        s_error = STORAGE_ERR_SD_INIT;
        return false;
    }
    s_ready = true;
    s_error = STORAGE_ERR_NONE;
    return true;
}

bool storage_ready(void)
{
    return s_ready;
}

storage_error_t storage_last_error(void)
{
    return s_error;
}

void storage_deinit(void)
{
    s_ready = false;
}

bool storage_make_disc_dir(void)
{
    char path[PATH_MAX];
    join_path(path, sizeof(path), "discs");
    return ensure_dir(path);
}

bool storage_file_exists(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

uint32_t storage_file_sectors(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) {
        return 0;
    }
    return (uint32_t)((uint64_t)st.st_size / 2048ull);
}

uint64_t storage_free_bytes(void)
{
    struct statvfs vfs;
    if (statvfs(s_root, &vfs) != 0) {
        return 0;
    }
    return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
}

static int cmp_entry(const void *a, const void *b)
{
    const disc_entry_t *ea = (const disc_entry_t *)a;
    const disc_entry_t *eb = (const disc_entry_t *)b;
    return strcasecmp(ea->name, eb->name);
}

static size_t scan_dir(const char *dir_path, disc_entry_t *out, size_t max_entries,
                       size_t count)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return count;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < max_entries) {
        if (de->d_name[0] == '.') {
            continue;
        }
        bool iso = util_has_suffix_ci(de->d_name, ".iso");
        bool cdr = util_has_suffix_ci(de->d_name, ".cdr");
        bool dvd = util_has_suffix_ci(de->d_name, ".dvd");
        bool bd = util_has_suffix_ci(de->d_name, ".bd");
        if (!iso && !cdr && !dvd && !bd) {
            continue;
        }
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        disc_entry_t *e = &out[count];
        memset(e, 0, sizeof(*e));
        util_safe_copy(e->path, sizeof(e->path), full);
        util_safe_copy(e->name, sizeof(e->name), de->d_name);
        e->sectors = (uint32_t)((uint64_t)st.st_size / 2048ull);
        e->format = iso ? DISC_FORMAT_ISO : DISC_FORMAT_CDRW;
        e->profile = iso ? MEDIA_PROFILE_CDROM :
                     dvd ? MEDIA_PROFILE_DVDRW :
                     bd ? MEDIA_PROFILE_BDRE : MEDIA_PROFILE_CDRW;
        count++;
    }
    closedir(dir);
    return count;
}

size_t storage_scan_discs(disc_entry_t *out, size_t max_entries)
{
    if (!out || max_entries == 0) {
        return 0;
    }
    size_t count = 0;
    char discs[PATH_MAX];
    join_path(discs, sizeof(discs), "discs");
    count = scan_dir(s_root, out, max_entries, 0);
    count = scan_dir(discs, out, max_entries, count);
    if (count > 1) {
        qsort(out, count, sizeof(out[0]), cmp_entry);
    }
    return count;
}

bool storage_create_blank_image_ex(const char *path, uint32_t sectors, bool sparse)
{
    if (!path || sectors == 0) {
        return false;
    }
    char dir[PATH_MAX];
    util_safe_copy(dir, sizeof(dir), path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!ensure_dir(dir)) {
            return false;
        }
    }

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    uint64_t bytes = (uint64_t)sectors * 2048ull;
    bool ok = true;
    if (sparse) {
        /* 4.7 GB / 25 GB images stay sparse until the host writes data. */
        if (ftruncate(fd, (off_t)bytes) != 0) {
            ok = false;
        }
    } else if (fallocate(fd, 0, 0, (off_t)bytes) != 0) {
        if (ftruncate(fd, (off_t)bytes) != 0) {
            ok = false;
        }
    }
    if (ok) {
        static const uint8_t zero[64 * 1024];
        uint64_t edge = bytes < (1024ull * 1024ull) ? bytes : (1024ull * 1024ull);
        for (uint64_t off = 0; off < edge; off += sizeof(zero)) {
            size_t n = (size_t)((edge - off) < sizeof(zero) ? (edge - off) : sizeof(zero));
            if (pwrite(fd, zero, n, (off_t)off) != (ssize_t)n) {
                ok = false;
                break;
            }
        }
        if (ok && bytes > edge) {
            for (uint64_t off = bytes - edge; off < bytes; off += sizeof(zero)) {
                size_t n = (size_t)((bytes - off) < sizeof(zero) ? (bytes - off) : sizeof(zero));
                if (pwrite(fd, zero, n, (off_t)off) != (ssize_t)n) {
                    ok = false;
                    break;
                }
            }
        }
    }
    if (ok) {
        ok = fsync(fd) == 0;
    }
    close(fd);
    if (!ok) {
        unlink(path);
    }
    return ok;
}

bool storage_create_blank_image(const char *path, uint32_t sectors)
{
    return storage_create_blank_image_ex(path, sectors, false);
}

bool storage_zero_file_edges(FIL *f, uint64_t bytes)
{
    if (!f || !f->in_use || bytes == 0) {
        return false;
    }
    static const uint8_t zero[64 * 1024];
    uint64_t edge = bytes < (1024ull * 1024ull) ? bytes : (1024ull * 1024ull);
    bool ok = true;
    for (uint64_t off = 0; off < edge && ok; off += sizeof(zero)) {
        size_t n = (size_t)((edge - off) < sizeof(zero) ? (edge - off) : sizeof(zero));
        if (pwrite(f->fd, zero, n, (off_t)off) != (ssize_t)n) {
            ok = false;
        }
    }
    if (ok && bytes > edge) {
        for (uint64_t off = bytes - edge; off < bytes; off += sizeof(zero)) {
            size_t n = (size_t)((bytes - off) < sizeof(zero) ? (bytes - off) : sizeof(zero));
            if (pwrite(f->fd, zero, n, (off_t)off) != (ssize_t)n) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        ok = fsync(f->fd) == 0;
    }
    return ok;
}

bool storage_save_last_disc(const char *path)
{
    char file[PATH_MAX];
    join_path(file, sizeof(file), ".last_disc");
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    size_t len = strlen(path);
    bool ok = write(fd, path, len) == (ssize_t)len &&
              write(fd, "\n", 1) == 1;
    close(fd);
    return ok;
}

bool storage_delete_disc(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    size_t rl = strlen(s_root);
    if (strncmp(path, s_root, rl) != 0) {
        return false;
    }
    if (path[rl] != '/' && path[rl] != '\0') {
        return false;
    }
    if (!util_has_suffix_ci(path, ".iso") &&
        !util_has_suffix_ci(path, ".cdr") &&
        !util_has_suffix_ci(path, ".dvd") &&
        !util_has_suffix_ci(path, ".bd")) {
        return false;
    }
    char meta[PATH_MAX];
    if (snprintf(meta, sizeof(meta), "%s.meta", path) >= (int)sizeof(meta)) {
        return false;
    }
    if (unlink(path) != 0) {
        return false;
    }
    (void)unlink(meta);

    char last[PATH_MAX];
    join_path(last, sizeof(last), ".last_disc");
    FILE *fp = fopen(last, "r");
    if (fp) {
        char line[PATH_MAX];
        bool ok = fgets(line, sizeof(line), fp) != NULL;
        fclose(fp);
        if (ok) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strcmp(line, path) == 0) {
                (void)unlink(last);
            }
        }
    }
    return true;
}

bool storage_load_last_disc(char *path, size_t path_size)
{
    if (!path || path_size == 0) {
        return false;
    }
    char file[PATH_MAX];
    join_path(file, sizeof(file), ".last_disc");
    FILE *fp = fopen(file, "r");
    if (!fp) {
        return false;
    }
    char line[PATH_MAX];
    bool ok = fgets(line, sizeof(line), fp) != NULL;
    fclose(fp);
    if (!ok) {
        return false;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0] || !storage_file_exists(line)) {
        return false;
    }
    util_safe_copy(path, path_size, line);
    return true;
}

bool storage_new_disc_path_ext(const char *ext, char *path, size_t path_size)
{
    char discs[PATH_MAX];
    join_path(discs, sizeof(discs), "discs");
    if (!ensure_dir(discs)) {
        return false;
    }
    if (!ext || !ext[0]) {
        ext = ".CDR";
    }
    for (int i = 1; i <= 99; i++) {
        char name[32];
        snprintf(name, sizeof(name), "DISC%02d%s", i, ext);
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%.*s/%s",
                 (int)(sizeof(full) - sizeof(name) - 2), discs, name);
        if (!storage_file_exists(full)) {
            util_safe_copy(path, path_size, full);
            return true;
        }
    }
    return false;
}

bool storage_new_disc_path(char *path, size_t path_size)
{
    return storage_new_disc_path_ext(".CDR", path, path_size);
}
