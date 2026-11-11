/*
 * Linux USB gadget backend built on FunctionFS.
 *
 * The kernel composite layer owns enumeration and the standard descriptors.
 * This file creates the configfs gadget, uploads the interface/endpoint
 * descriptors, answers the two BOT class requests (GET_MAX_LUN / reset) and
 * forwards bulk traffic to the transport-independent BOT engine.
 */
#include "ffs_gadget.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "debug_log.h"

#define CFGFS "/sys/kernel/config"
#define USB_VID 0x1209
#define USB_PID 0xCD01
#define USB_BCDUSB 0x0200
#define USB_BCDDEV 0x0100
#define MSC_SUBCLASS_SCSI 0x06
#define MSC_PROTOCOL_BOT 0x50
#define MSC_REQ_GET_MAX_LUN 0xFE
#define MSC_REQ_RESET 0xFF

static ffs_gadget_t *s_active;

bool ffs_gadget_bound(void)
{
    return s_active && s_active->bound;
}

bool ffs_gadget_configured(void)
{
    return s_active && s_active->enabled;
}

static int write_file(const char *path, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf)) {
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return w == n ? 0 : -1;
}

static int mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* ---- descriptors ------------------------------------------------------ */

#define MSC_DESC_LEN (sizeof(struct usb_interface_descriptor) + \
                      2 * sizeof(struct usb_endpoint_descriptor_no_audio))

static const struct {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    struct {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio out;
        struct usb_endpoint_descriptor_no_audio in;
    } __attribute__((packed)) fs;
    struct {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio out;
        struct usb_endpoint_descriptor_no_audio in;
    } __attribute__((packed)) hs;
} __attribute__((packed)) g_descs = {
    .header = {
        .magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
        .flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
        .length = sizeof(g_descs),
    },
    .fs_count = 3,
    .hs_count = 3,
    .fs = {
        .intf = {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_MASS_STORAGE,
            .bInterfaceSubClass = MSC_SUBCLASS_SCSI,
            .bInterfaceProtocol = MSC_PROTOCOL_BOT,
            .iInterface = 1,
        },
        .out = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x01 | USB_DIR_OUT,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = 64,
        },
        .in = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x81 | USB_DIR_IN,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = 64,
        },
    },
    .hs = {
        .intf = {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_MASS_STORAGE,
            .bInterfaceSubClass = MSC_SUBCLASS_SCSI,
            .bInterfaceProtocol = MSC_PROTOCOL_BOT,
            .iInterface = 1,
        },
        .out = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x01 | USB_DIR_OUT,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = 512,
        },
        .in = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x81 | USB_DIR_IN,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = 512,
        },
    },
};

static size_t build_strings(uint8_t *buf, size_t cap)
{
    static const char text[] = "DVD/BD USB Drive";
    size_t slen = sizeof(text); /* includes NUL */
    size_t total = sizeof(struct usb_functionfs_strings_head) + 2 + slen;
    if (total > cap) {
        return 0;
    }
    struct usb_functionfs_strings_head head = {
        .magic = FUNCTIONFS_STRINGS_MAGIC,
        .length = total,
        .str_count = 1,
        .lang_count = 1,
    };
    memcpy(buf, &head, sizeof(head));
    buf[sizeof(head)] = 0x09;
    buf[sizeof(head) + 1] = 0x04;
    memcpy(buf + sizeof(head) + 2, text, slen);
    return total;
}

/* ---- ep0 control handling --------------------------------------------- */

static void ep0_ack(ffs_gadget_t *g)
{
    uint8_t dummy = 0;
    ssize_t rc = write(g->ep0, &dummy, 0);
    if (rc < 0) {
        LOGF("ffs: ep0 ack failed: %s", strerror(errno));
    }
}

static void handle_setup(ffs_gadget_t *g, const struct usb_ctrlrequest *ctrl)
{
    uint8_t type = ctrl->bRequestType & USB_TYPE_MASK;
    uint8_t recip = ctrl->bRequestType & USB_RECIP_MASK;
    uint16_t wLength = ctrl->wLength;

    LOGF("ffs: SETUP type=%02X req=%02X val=%04X idx=%04X len=%u",
         ctrl->bRequestType, ctrl->bRequest, ctrl->wValue, ctrl->wIndex,
         wLength);

    if (type == USB_TYPE_CLASS && ctrl->bRequest == MSC_REQ_GET_MAX_LUN) {
        uint8_t lun = 0;
        if (write(g->ep0, &lun, 1) != 1) {
            LOGF("ffs: GET_MAX_LUN reply failed: %s", strerror(errno));
        }
        return;
    }
    if (type == USB_TYPE_CLASS && ctrl->bRequest == MSC_REQ_RESET) {
        g->reset_requested = true;
        ep0_ack(g);
        return;
    }
    if (type == USB_TYPE_STANDARD && ctrl->bRequest == USB_REQ_CLEAR_FEATURE &&
        recip == USB_RECIP_ENDPOINT &&
        (ctrl->wValue & 0xFFFF) == USB_ENDPOINT_HALT) {
        int ep = (ctrl->wIndex & 0x80) ? g->ep_in : g->ep_out;
        if (ioctl(ep, FUNCTIONFS_CLEAR_HALT) < 0) {
            LOGF("ffs: clear halt failed: %s", strerror(errno));
        }
        g->reset_requested = true;
        ep0_ack(g);
        return;
    }
    /* Unknown request.  FunctionFS has no userspace bulk-stall ioctl, and a
       short (zero length) reply is preferable to letting the host time out. */
    if (wLength == 0) {
        ep0_ack(g);
    } else {
        ep0_ack(g);
    }
}

static void handle_ep0_events(ffs_gadget_t *g)
{
    struct usb_functionfs_event events[8];
    ssize_t n = read(g->ep0, events, sizeof(events));
    if (n < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            LOGF("ffs: ep0 read: %s", strerror(errno));
            g->stop = true;
        }
        return;
    }
    if (n == 0) {
        g->stop = true;
        return;
    }
    size_t count = (size_t)n / sizeof(events[0]);
    for (size_t i = 0; i < count; i++) {
        switch (events[i].type) {
        case FUNCTIONFS_BIND:
            LOGF("ffs: BIND");
            break;
        case FUNCTIONFS_ENABLE:
            LOGF("ffs: ENABLE");
            g->enabled = true;
            break;
        case FUNCTIONFS_DISABLE:
            LOGF("ffs: DISABLE");
            g->enabled = false;
            break;
        case FUNCTIONFS_UNBIND:
            LOGF("ffs: UNBIND");
            break;
        case FUNCTIONFS_SUSPEND:
            LOGF("ffs: SUSPEND");
            break;
        case FUNCTIONFS_RESUME:
            LOGF("ffs: RESUME");
            break;
        case FUNCTIONFS_SETUP:
            handle_setup(g, &events[i].u.setup);
            break;
        default:
            break;
        }
    }
}

static void check_reset(ffs_gadget_t *g)
{
    if (g->reset_requested) {
        g->reset_requested = false;
        if (g->bot) {
            bot_reset(g->bot);
            LOGF("ffs: BOT reset");
        }
    }
}

/* ---- ep0 control thread ----------------------------------------------- */

static void *ep0_thread_fn(void *arg)
{
    ffs_gadget_t *g = (ffs_gadget_t *)arg;
    while (!g->stop) {
        struct pollfd p = {.fd = g->ep0, .events = POLLIN};
        int rc = poll(&p, 1, 200);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (rc == 0) {
            continue;
        }
        if (p.revents & (POLLIN | POLLERR | POLLHUP)) {
            handle_ep0_events(g);
        }
    }
    return NULL;
}

/* ---- bulk I/O HAL ----------------------------------------------------- */

static int ffs_read(void *ctx, void *buf, uint32_t len, int timeout_ms)
{
    ffs_gadget_t *g = (ffs_gadget_t *)ctx;
    for (;;) {
        check_reset(g);
        if (g->stop) {
            return -1;
        }
        struct pollfd p = {.fd = g->ep_out, .events = POLLIN};
        int rc = poll(&p, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;
        }
        if (p.revents & (POLLIN | POLLERR | POLLHUP)) {
            ssize_t n = read(g->ep_out, buf, len);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOGF("ffs: ep_out read: %s", strerror(errno));
                return -1;
            }
            return (int)n;
        }
    }
}

static int ffs_write(void *ctx, const void *buf, uint32_t len, int timeout_ms)
{
    ffs_gadget_t *g = (ffs_gadget_t *)ctx;
    for (;;) {
        check_reset(g);
        if (g->stop) {
            return -1;
        }
        struct pollfd p = {.fd = g->ep_in, .events = POLLOUT};
        int rc = poll(&p, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;
        }
        if (p.revents & (POLLOUT | POLLERR | POLLHUP)) {
            ssize_t n = write(g->ep_in, buf, len);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOGF("ffs: ep_in write: %s", strerror(errno));
                return -1;
            }
            return (int)n;
        }
    }
}

static void ffs_stall(void *ctx, bool ep_out)
{
    (void)ctx;
    /* FunctionFS exposes no endpoint-stall ioctl.  Log the protocol error and
       let the next CBW resynchronise; the host will usually issue a BOT
       reset after its own timeout. */
    LOGF("ffs: protocol error on bulk %s (no stall ioctl)",
         ep_out ? "OUT" : "IN");
}

static void ffs_clear_halt(void *ctx, bool ep_out)
{
    ffs_gadget_t *g = (ffs_gadget_t *)ctx;
    int fd = ep_out ? g->ep_out : g->ep_in;
    (void)ioctl(fd, FUNCTIONFS_CLEAR_HALT);
}

static bot_io_t g_bot_io = {
    .read = ffs_read,
    .write = ffs_write,
    .stall = ffs_stall,
    .clear_halt = ffs_clear_halt,
};

/* ---- configfs --------------------------------------------------------- */

static const char *gadget_path(char *out, size_t size, const char *name,
                               const char *suffix)
{
    snprintf(out, size, CFGFS "/usb_gadget/%s/%s", name, suffix);
    return out;
}

static void remove_gadget(const char *name)
{
    char path[1024];
    char cfg[1024];
    char func[1024];
    char str[1024];

    snprintf(cfg, sizeof(cfg), CFGFS "/usb_gadget/%s/configs/c.1", name);
    snprintf(func, sizeof(func), CFGFS "/usb_gadget/%s/functions/ffs.%s", name,
             name);
    snprintf(str, sizeof(str), CFGFS "/usb_gadget/%s/strings/0x409", name);

    gadget_path(path, sizeof(path), name, "UDC");
    (void)write_file(path, "");

    char link[1024];
    snprintf(link, sizeof(link), "%.900s/ffs.%s", cfg, name);
    (void)unlink(link);
    char sub[1024];
    snprintf(sub, sizeof(sub), "%.900s/strings/0x409", cfg);
    (void)rmdir(sub);
    (void)rmdir(cfg);
    (void)rmdir(func); /* fails while linked; link is already gone */
    (void)rmdir(str);
    gadget_path(path, sizeof(path), name, "");
    (void)rmdir(path);
}

static int read_link(const char *path, char *out, size_t out_size)
{
    ssize_t n = readlink(path, out, out_size - 1);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static bool configfs_create(ffs_gadget_t *g)
{
    char path[256];
    char dir[256];

    snprintf(dir, sizeof(dir), CFGFS "/usb_gadget/%s", g->name);
    if (mkdir_p(dir) != 0) {
        LOGF("ffs: cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    gadget_path(path, sizeof(path), g->name, "idVendor");
    if (write_file(path, "0x%04X", USB_VID) != 0) {
        LOGF("ffs: write %s: %s", path, strerror(errno));
        return false;
    }
    gadget_path(path, sizeof(path), g->name, "idProduct");
    write_file(path, "0x%04X", USB_PID);
    gadget_path(path, sizeof(path), g->name, "bcdUSB");
    write_file(path, "0x%04X", USB_BCDUSB);
    gadget_path(path, sizeof(path), g->name, "bcdDevice");
    write_file(path, "0x%04X", USB_BCDDEV);

    snprintf(dir, sizeof(dir), CFGFS "/usb_gadget/%s/strings/0x409", g->name);
    if (mkdir_p(dir) != 0) {
        return false;
    }
    gadget_path(path, sizeof(path), g->name, "strings/0x409/manufacturer");
    write_file(path, "Orange Pi");
    gadget_path(path, sizeof(path), g->name, "strings/0x409/product");
    write_file(path, "DVD/BD USB Drive");
    gadget_path(path, sizeof(path), g->name, "strings/0x409/serialnumber");
    char serial[64];
    if (read_link("/etc/machine-id", serial, sizeof(serial)) != 0) {
        FILE *fp = fopen("/etc/machine-id", "r");
        if (fp && fgets(serial, sizeof(serial), fp)) {
            serial[strcspn(serial, "\r\n")] = '\0';
        } else {
            snprintf(serial, sizeof(serial), "OPI3B00000001");
        }
        if (fp) {
            fclose(fp);
        }
    }
    write_file(path, "%s", serial);

    snprintf(dir, sizeof(dir), CFGFS "/usb_gadget/%s/configs/c.1", g->name);
    if (mkdir_p(dir) != 0) {
        return false;
    }
    snprintf(dir, sizeof(dir), CFGFS "/usb_gadget/%s/configs/c.1/strings/0x409",
             g->name);
    mkdir_p(dir);
    gadget_path(path, sizeof(path), g->name, "configs/c.1/MaxPower");
    write_file(path, "250");
    gadget_path(path, sizeof(path), g->name, "configs/c.1/bmAttributes");
    write_file(path, "0x80");

    snprintf(dir, sizeof(dir), CFGFS "/usb_gadget/%s/functions/ffs.%s",
             g->name, g->name);
    if (mkdir_p(dir) != 0) {
        LOGF("ffs: cannot create ffs function: %s", strerror(errno));
        return false;
    }
    char link[256];
    snprintf(link, sizeof(link), CFGFS "/usb_gadget/%s/configs/c.1/ffs.%s",
             g->name, g->name);
    char target[256];
    /* configfs resolves the symlink target relative to the caller's cwd, so
       use an absolute path instead of "../../functions/...". */
    snprintf(target, sizeof(target), CFGFS "/usb_gadget/%s/functions/ffs.%s",
             g->name, g->name);
    if (symlink(target, link) != 0) {
        LOGF("ffs: cannot link function: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool ffs_mount(ffs_gadget_t *g)
{
    snprintf(g->mnt, sizeof(g->mnt), "/dev/ffs-%s", g->name);
    if (mkdir_p(g->mnt) != 0) {
        return false;
    }
    if (mount(g->name, g->mnt, "functionfs", 0, NULL) != 0) {
        if (errno == EBUSY) {
            (void)umount2(g->mnt, MNT_DETACH);
            if (mount(g->name, g->mnt, "functionfs", 0, NULL) != 0) {
                LOGF("ffs: mount functionfs: %s", strerror(errno));
                return false;
            }
        } else {
            LOGF("ffs: mount functionfs: %s", strerror(errno));
            return false;
        }
    }
    g->mounted = true;
    return true;
}

static int open_ep(const char *mnt, const char *name)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", mnt, name);
    for (int i = 0; i < 50; i++) {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
        if (errno != ENOENT) {
            LOGF("ffs: open %s: %s", path, strerror(errno));
            return -1;
        }
        usleep(20000);
    }
    LOGF("ffs: %s did not appear", path);
    return -1;
}

static void try_modprobe(const char *module)
{
    static const char *const paths[] = {
        "/sbin/modprobe", "/usr/sbin/modprobe", "modprobe",
    };
    char cmd[128];
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", paths[i], module);
        if (system(cmd) == 0) {
            return;
        }
    }
}

/* libcomposite creates /sys/kernel/config/usb_gadget; usb_f_fs provides the
 * FunctionFS function type.  Both may be modules and are often not loaded
 * after a reboot, in which case configfs returns EPERM for the missing
 * subsystem directory. */
static bool ensure_gadget_modules(void)
{
    if (access("/sys/kernel/config/usb_gadget", F_OK) != 0) {
        try_modprobe("libcomposite");
    }
    try_modprobe("usb_f_fs");
    if (access("/sys/kernel/config/usb_gadget", F_OK) != 0) {
        LOGF("ffs: /sys/kernel/config/usb_gadget missing; run: "
             "sudo modprobe libcomposite usb_f_fs");
        return false;
    }
    return true;
}

bool ffs_gadget_setup(ffs_gadget_t *g, const char *name, const char *udc)
{
    memset(g, 0, sizeof(*g));
    g->ep0 = g->ep_out = g->ep_in = -1;
    snprintf(g->name, sizeof(g->name), "%s", name);
    snprintf(g->udc, sizeof(g->udc), "%s", udc);

    if (!ensure_gadget_modules()) {
        return false;
    }
    remove_gadget(g->name);
    if (!configfs_create(g)) {
        remove_gadget(g->name);
        return false;
    }
    if (!ffs_mount(g)) {
        remove_gadget(g->name);
        return false;
    }

    g->ep0 = open_ep(g->mnt, "ep0");
    if (g->ep0 < 0) {
        ffs_gadget_teardown(g);
        return false;
    }
    ssize_t n = write(g->ep0, &g_descs, sizeof(g_descs));
    if (n != (ssize_t)sizeof(g_descs)) {
        LOGF("ffs: descriptor upload failed (%zd): %s", n, strerror(errno));
        ffs_gadget_teardown(g);
        return false;
    }
    uint8_t strings[64];
    size_t strings_len = build_strings(strings, sizeof(strings));
    n = write(g->ep0, strings, strings_len);
    if (n != (ssize_t)strings_len) {
        LOGF("ffs: string upload failed (%zd/%zu): %s", n, strings_len,
             strerror(errno));
        ffs_gadget_teardown(g);
        return false;
    }

    /* Endpoint order follows the descriptor order: ep1 = OUT, ep2 = IN. */
    g->ep_out = open_ep(g->mnt, "ep1");
    g->ep_in = open_ep(g->mnt, "ep2");
    if (g->ep_out < 0 || g->ep_in < 0) {
        ffs_gadget_teardown(g);
        return false;
    }

    char path[256];
    gadget_path(path, sizeof(path), g->name, "UDC");
    if (write_file(path, "%s", g->udc) != 0) {
        LOGF("ffs: bind to %s failed: %s", g->udc, strerror(errno));
        ffs_gadget_teardown(g);
        return false;
    }
    g->bound = true;
    s_active = g;
    if (pthread_create(&g->ep0_thread, NULL, ep0_thread_fn, g) != 0) {
        LOGF("ffs: cannot start ep0 thread: %s", strerror(errno));
        ffs_gadget_teardown(g);
        return false;
    }
    g->ep0_thread_running = true;
    LOGF("ffs: bound to %s (%s)", g->udc, g->mnt);
    return true;
}

int ffs_gadget_run(ffs_gadget_t *g)
{
    g->bot->io = &g_bot_io;
    g->bot->io->ctx = g;
    int rc = bot_run(g->bot);
    LOGF("ffs: bot stopped rc=%d", rc);
    return rc;
}

void ffs_gadget_teardown(ffs_gadget_t *g)
{
    if (s_active == g) {
        s_active = NULL;
    }
    g->stop = true;
    if (g->ep0_thread_running) {
        pthread_join(g->ep0_thread, NULL);
        g->ep0_thread_running = false;
    }
    if (g->bound) {
        char path[256];
        gadget_path(path, sizeof(path), g->name, "UDC");
        (void)write_file(path, "");
        g->bound = false;
    }
    if (g->ep_out >= 0) {
        close(g->ep_out);
        g->ep_out = -1;
    }
    if (g->ep_in >= 0) {
        close(g->ep_in);
        g->ep_in = -1;
    }
    if (g->ep0 >= 0) {
        close(g->ep0);
        g->ep0 = -1;
    }
    if (g->mounted) {
        (void)umount2(g->mnt, MNT_DETACH);
        g->mounted = false;
    }
    remove_gadget(g->name);
}
