#ifndef CDRW_FFS_GADGET_H
#define CDRW_FFS_GADGET_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "bot.h"

typedef struct {
    char name[64];       /* configfs gadget name, e.g. "cdrw" */
    char udc[64];        /* UDC to bind, e.g. "fcc00000.usb" */
    char ffs_name[64];   /* FunctionFS instance, e.g. "cdrw" */
    char mnt[128];       /* mount point, e.g. /dev/ffs-cdrw */
    int ep0;
    int ep_out;
    int ep_in;
    volatile bool bound;
    bool mounted;
    volatile bool enabled;
    bool reset_requested;
    bool stop;
    bool ep0_thread_running;
    pthread_t ep0_thread;
    bot_t *bot;
} ffs_gadget_t;

/* Create the configfs gadget, mount FunctionFS, upload descriptors and bind
 * the UDC.  Requires root and a mounted configfs. */
bool ffs_gadget_setup(ffs_gadget_t *g, const char *name, const char *udc);

/* Serve CBWs until the transport dies or g->stop becomes true. */
int ffs_gadget_run(ffs_gadget_t *g);

/* Unbind, unmount and remove the configfs objects. */
void ffs_gadget_teardown(ffs_gadget_t *g);

/* Status for the local UI. */
bool ffs_gadget_bound(void);
bool ffs_gadget_configured(void);

#endif /* CDRW_FFS_GADGET_H */
