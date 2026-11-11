#ifndef CDRW_STORAGE_H
#define CDRW_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "media.h"

#define STORAGE_PATH_MAX 260
#define STORAGE_MAX_DISCS 32
#define STORAGE_DISC_NAME_MAX 96

typedef enum {
    DISC_FORMAT_ISO = 0,
    DISC_FORMAT_CDRW = 1,
} disc_format_t;

typedef enum {
    STORAGE_ERR_NONE = 0,   /* mounted */
    STORAGE_ERR_SD_INIT,    /* SPI/SD initialization failed */
    STORAGE_ERR_FAT_MOUNT,  /* card responded but FatFs could not mount */
} storage_error_t;

typedef struct {
    char path[STORAGE_PATH_MAX];
    char name[STORAGE_DISC_NAME_MAX];
    uint32_t sectors;
    disc_format_t format;
    uint16_t profile;
} disc_entry_t;

/* Select the directory that stands in for the microSD card. */
void storage_set_root(const char *root);
const char *storage_get_root(void);

/* Mount the microSD card and mount FatFs. */
bool storage_init(void);
bool storage_ready(void);
storage_error_t storage_last_error(void);
void storage_deinit(void);

/* Scan the root and /discs for .iso and .cdr files.  Results are sorted by
 * name.  Returns the number of entries written. */
size_t storage_scan_discs(disc_entry_t *out, size_t max_entries);

bool storage_file_exists(const char *path);
uint32_t storage_file_sectors(const char *path);
uint64_t storage_free_bytes(void);

/* Preallocate a new raw CD-RW image.  The file is filled with zeros only if
 * the card/filesystem cannot preallocate it efficiently. */
bool storage_create_blank_image(const char *path, uint32_t sectors);
/* sparse=true uses ftruncate instead of fallocate (DVD/BD sized images). */
bool storage_create_blank_image_ex(const char *path, uint32_t sectors, bool sparse);

/* Zero the first and last 1 MiB of an image to remove stale ISO/UDF
 * signatures while keeping quick erase fast. */
bool storage_zero_file_edges(FIL *f, uint64_t bytes);
bool storage_make_disc_dir(void);

/* Remember the last inserted image so a power cycle starts on the same disc. */
bool storage_save_last_disc(const char *path);
bool storage_delete_disc(const char *path);
bool storage_load_last_disc(char *path, size_t path_size);

/* Generate a fresh /discs/DISCxx.CDR path that does not exist yet. */
bool storage_new_disc_path(char *path, size_t path_size);
/* Like storage_new_disc_path but with an explicit extension (".DVD", ".BD"). */
bool storage_new_disc_path_ext(const char *ext, char *path, size_t path_size);

#endif
