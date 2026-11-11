/*
 * pi3b-cdrw-emu - USB CD-RW emulator for the Orange Pi 3B (RK3566).
 *
 * The SCSI MMC engine and the CD-R/CD-RW media layer are shared with the
 * RP2350 firmware.  This daemon supplies the Linux-specific pieces: POSIX
 * file storage, a FunctionFS USB gadget and a small command line front end.
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bot.h"
#include "debug_log.h"
#include "encoder.h"
#include "ffs_gadget.h"
#include "media.h"
#include "scsi.h"
#include "storage.h"
#include "ui.h"
#include "util.h"
#include "version.h"

#define DEFAULT_ROOT "."
#define DEFAULT_GADGET "cdrw0"
#define DEFAULT_UDC "fcc00000.usb"

static ffs_gadget_t g_gadget;
static ffs_gadget_t *g_gadget_ptr;
static volatile sig_atomic_t g_stop;
static pthread_t s_ui_thread;
static bool s_ui_thread_started;

static void *ui_thread_fn(void *arg)
{
    (void)arg;
    while (!g_stop) {
        ui_task();
        usleep(50000);
    }
    return NULL;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_gadget_ptr) {
        g_gadget_ptr->stop = true;
    }
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

static void print_media(void)
{
    const media_status_t *m = media_get();
    if (!m->present) {
        printf("media: no disc\n");
        return;
    }
    const char *state = m->kind == MEDIA_KIND_ISO ? "CD-ROM (ISO)" :
                        m->blank ? "blank" :
                        m->complete ? "complete" : "incomplete/appendable";
    printf("media: %s %s\n", media_profile_name(m->profile), state);
    printf("  path      : %s\n", m->path);
    printf("  label     : %s\n", m->label);
    printf("  capacity  : %u sectors (%u KiB)\n", m->capacity_lba,
           m->capacity_lba * 2u);
    printf("  lead-out  : %u\n", m->data_lba);
    printf("  NWA       : %u\n", m->nwa);
    printf("  writable  : %s\n", m->writable ? "yes" : "no");
}

static void list_discs(void)
{
    disc_entry_t entries[STORAGE_MAX_DISCS];
    size_t n = storage_scan_discs(entries, STORAGE_MAX_DISCS);
    printf("%zu disc image(s) under %s:\n", n, storage_get_root());
    for (size_t i = 0; i < n; i++) {
        printf("  %-24s %8u sectors  %s\n", entries[i].name,
               entries[i].sectors, media_profile_name(entries[i].profile));
    }
}

static const char *pick_udc(const char *requested, char *out, size_t out_size)
{
    if (requested && requested[0]) {
        snprintf(out, out_size, "%s", requested);
        return out;
    }
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/udc/%s", DEFAULT_UDC);
    if (access(path, F_OK) == 0) {
        snprintf(out, out_size, "%s", DEFAULT_UDC);
        return out;
    }
    if (access("/sys/class/udc/usbip-vudc.0", F_OK) == 0) {
        snprintf(out, out_size, "usbip-vudc.0");
        return out;
    }
    return NULL;
}

static void usage(const char *argv0)
{
    printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -r, --root DIR       directory used as the disc library (default: %s)\n"
        "  -i, --image PATH     insert this .iso/.cdr/.dvd/.bd at startup\n"
        "  -n, --new            create/insert a blank disc for --profile\n"
        "  -l, --list           list disc images and exit\n"
        "  -I, --info           print current media state and exit\n"
        "  -u, --udc NAME       UDC to bind (default: %s, else usbip-vudc.0)\n"
        "  -g, --gadget NAME    configfs gadget name (default: %s)\n"
        "  -p, --profile NAME   blank media profile: cd-rw, dvd-rw, bd-re\n"
        "  -L, --loopback       bind to usbip-vudc.0 for USB/IP export\n"
        "      --no-usb         run the SCSI/media core without USB\n"
        "      --no-ui          disable OLED and encoder\n"
        "      --i2c-bus N      OLED I2C bus (default: auto, prefers 2)\n"
        "      --oled-addr A    SSD1306 address (default: 0x3C)\n"
        "      --encoder-a N    EC11 A GPIO (default: 118)\n"
        "      --encoder-b N    EC11 B GPIO (default: 128)\n"
        "      --encoder-btn N  EC11 button GPIO (default: 129)\n"
        "      --encoder        enable the EC11 encoder (default: display only)\n"
        "      --encoder-steps N  quadrature transitions/detent (default: 4)\n"
        "      --encoder-debug  log raw A/B state transitions\n"
        "      --no-encoder     display only, no encoder\n"
        "      --oled-mock PATH dump PGM frames instead of a real panel\n"
        "  -v, --verbose        accepted (CBW logging is always on)\n"
        "  -h, --help           this help\n",
        argv0, DEFAULT_ROOT, DEFAULT_UDC, DEFAULT_GADGET);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char *root = getenv("CDRW_ROOT");
    const char *image = NULL;
    const char *udc_arg = NULL;
    const char *gadget_name = DEFAULT_GADGET;
    uint16_t profile = MEDIA_PROFILE_CDRW;
    bool profile_set = false;
    bool create_new = false;
    bool list_only = false;
    bool info_only = false;
    bool no_usb = false;
    bool no_ui = false;
    bool no_encoder = false;
    bool encoder_flag = false;
    bool encoder_debug = false;
    int i2c_bus = -1;
    int oled_addr = 0x3C;
    int enc_a = 118, enc_b = 128, enc_btn = 129;
    int encoder_steps = 4;
    const char *oled_mock = NULL;

    static const struct option opts[] = {
        {"root", required_argument, NULL, 'r'},
        {"image", required_argument, NULL, 'i'},
        {"new", no_argument, NULL, 'n'},
        {"list", no_argument, NULL, 'l'},
        {"info", no_argument, NULL, 'I'},
        {"udc", required_argument, NULL, 'u'},
        {"gadget", required_argument, NULL, 'g'},
        {"profile", required_argument, NULL, 'p'},
        {"loopback", no_argument, NULL, 'L'},
        {"no-usb", no_argument, NULL, 1000},
        {"no-ui", no_argument, NULL, 1001},
        {"i2c-bus", required_argument, NULL, 1002},
        {"oled-addr", required_argument, NULL, 1003},
        {"encoder-a", required_argument, NULL, 1004},
        {"encoder-b", required_argument, NULL, 1005},
        {"encoder-btn", required_argument, NULL, 1006},
        {"no-encoder", no_argument, NULL, 1007},
        {"oled-mock", required_argument, NULL, 1008},
        {"encoder", no_argument, NULL, 1009},
        {"encoder-steps", required_argument, NULL, 1010},
        {"encoder-debug", no_argument, NULL, 1011},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "r:i:nlIu:g:p:Lvh", opts, NULL)) != -1) {
        switch (c) {
        case 'r': root = optarg; break;
        case 'i': image = optarg; break;
        case 'n': create_new = true; break;
        case 'l': list_only = true; break;
        case 'I': info_only = true; break;
        case 'u': udc_arg = optarg; break;
        case 'g': gadget_name = optarg; break;
        case 'p':
            profile = media_profile_from_name(optarg);
            if (profile == MEDIA_PROFILE_NONE) {
                fprintf(stderr, "unknown profile: %s\n", optarg);
                return 2;
            }
            profile_set = true;
            break;
        case 'L': udc_arg = "usbip-vudc.0"; break;
        case 1000: no_usb = true; break;
        case 1001: no_ui = true; break;
        case 1002: i2c_bus = (int)strtol(optarg, NULL, 0); break;
        case 1003: oled_addr = (int)strtol(optarg, NULL, 0); break;
        case 1004: enc_a = (int)strtol(optarg, NULL, 0); break;
        case 1005: enc_b = (int)strtol(optarg, NULL, 0); break;
        case 1006: enc_btn = (int)strtol(optarg, NULL, 0); break;
        case 1007: no_encoder = true; break;
        case 1008: oled_mock = optarg; break;
        case 1009: encoder_flag = true; break;
        case 1010: encoder_steps = (int)strtol(optarg, NULL, 0); break;
        case 1011: encoder_debug = true; break;
        case 'v': break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    if (!root || !root[0]) {
        root = DEFAULT_ROOT;
    }
    if (profile_set) {
        media_set_default_profile(profile);
    }
    storage_set_root(root);
    if (!storage_init()) {
        fprintf(stderr, "cannot initialize storage root %s\n", root);
        return 1;
    }
    if (list_only) {
        list_discs();
        return 0;
    }

    media_init();
    scsi_init();

    char serial[32] = "OPI3B00000001";
    FILE *mid = fopen("/etc/machine-id", "r");
    if (mid) {
        if (fgets(serial, sizeof(serial), mid)) {
            serial[strcspn(serial, "\r\n")] = '\0';
        }
        fclose(mid);
    }
    scsi_set_serial(serial);

    printf("pi3b-cdrw-emu v%s (RK3566 / Orange Pi 3B)\n", CDRW_VERSION);
    printf("disc library: %s\n", root);

    if (create_new) {
        char path[STORAGE_PATH_MAX];
        bool ok = profile_set
                      ? media_new_blank_profile(path, sizeof(path), profile)
                      : media_new_blank(path, sizeof(path));
        if (!ok) {
            fprintf(stderr, "failed to create a blank disc image\n");
            return 1;
        }
        printf("created blank disc: %s\n", path);
        image = NULL;
    } else if (image) {
        if (!media_load(image)) {
            fprintf(stderr, "cannot load image %s; starting with no disc\n",
                    image);
        }
    } else {
        char last[STORAGE_PATH_MAX];
        if (storage_load_last_disc(last, sizeof(last))) {
            printf("restoring last disc: %s\n", last);
            (void)media_load(last);
        }
    }
    print_media();
    if (info_only) {
        media_shutdown();
        return 0;
    }

    encoder_set_steps(encoder_steps);
    encoder_set_debug(encoder_debug);
    bool ui_ok = false;
    if (!no_ui) {
        ui_config_t ucfg = {
            .i2c_bus = i2c_bus,
            .oled_addr = oled_addr,
            .enc_a = enc_a,
            .enc_b = enc_b,
            .enc_btn = enc_btn,
            .enable_encoder = encoder_flag && !no_encoder,
            .oled_mock = oled_mock,
        };
        ui_ok = ui_init(&ucfg);
        if (!ui_ok) {
            fprintf(stderr,
                    "UI: no OLED found; run sudo tools/enable-i2c2.sh and "
                    "check wiring (or use --no-ui)\n");
        }
    }

    if (no_usb) {
        if (ui_ok) {
            printf("USB disabled; UI running (Ctrl-C to exit)\n");
            install_signals();
            g_gadget_ptr = NULL;
            while (!g_stop) {
                ui_task();
                usleep(50000);
            }
            ui_shutdown();
        } else {
            printf("USB disabled; SCSI core ready\n");
        }
        media_shutdown();
        return 0;
    }

    char udc[64];
    if (!pick_udc(udc_arg, udc, sizeof(udc))) {
        fprintf(stderr,
                "no UDC available.\n"
                "For the on-board USB 2.0 device port run:\n"
                "  sudo tools/enable-otg.sh\n"
                "or export the gadget over USB/IP with:\n"
                "  sudo modprobe usbip-vudc && sudo usbipd -e -D\n");
        if (ui_ok) ui_shutdown();
        media_shutdown();
        return 1;
    }

    install_signals();
    g_gadget_ptr = &g_gadget;

    static uint8_t data_buf[64 * 1024];
    static uint8_t out_buf[8 * 1024];
    static bot_t bot;
    bot_init(&bot, NULL, data_buf, sizeof(data_buf), out_buf, sizeof(out_buf));

    if (!ffs_gadget_setup(&g_gadget, gadget_name, udc)) {
        fprintf(stderr, "gadget setup failed (need root and configfs)\n");
        if (ui_ok) ui_shutdown();
        media_shutdown();
        return 1;
    }
    g_gadget.bot = &bot;
    if (ui_ok) {
        if (pthread_create(&s_ui_thread, NULL, ui_thread_fn, NULL) == 0) {
            s_ui_thread_started = true;
        } else {
            fprintf(stderr, "UI: cannot start UI thread\n");
        }
    }
    printf("USB gadget %s bound to %s; waiting for host\n", gadget_name, udc);
    fflush(stdout);

    (void)ffs_gadget_run(&g_gadget);
    g_stop = 1;
    if (s_ui_thread_started) {
        pthread_join(s_ui_thread, NULL);
        s_ui_thread_started = false;
    }
    printf("shutting down\n");
    g_gadget.bot = NULL;
    ffs_gadget_teardown(&g_gadget);
    if (ui_ok) {
        ui_shutdown();
    }
    media_shutdown();
    printf("media synced; bye\n");
    return 0;
}
