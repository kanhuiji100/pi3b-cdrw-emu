/*
 * Local OLED + EC11 UI for the Orange Pi 3B optical-drive emulator.
 *
 * This is the RP2350 firmware's UI logic adapted to Linux: the same views,
 * menus and disc browsing, but profile-aware (CD-RW / DVD-RW / BD-RE).  It
 * runs in its own thread; media/SCSI access is serialized with the BOT thread
 * by media_lock().
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "encoder.h"
#include "media.h"
#include "media_lock.h"
#include "oled.h"
#include "scsi.h"
#include "storage.h"
#include "usb_status.h"
#include "util.h"
#include "version.h"

#define UI_VERSION CDRW_VERSION
#define UI_MAX_DISCS 24
#define UI_REFRESH_MS 250
#define UI_MENU_ITEMS 8
#define UI_MENU_ROWS 4

typedef enum {
    VIEW_STATUS = 0,
    VIEW_MENU,
    VIEW_DISCS,
    VIEW_CONFIRM,
    VIEW_INFO,
    VIEW_DEBUG,
    VIEW_MESSAGE,
    VIEW_NEWPROFILE,
    VIEW_DELETE,
} ui_view_t;

typedef enum {
    ACTION_NONE = 0,
    ACTION_NEW_BLANK,
    ACTION_ERASE,
    ACTION_EJECT,
    ACTION_DELETE,
} ui_action_t;

typedef struct {
    const char *label;
    ui_action_t action;
    int menu_index;
} menu_item_t;

static ui_view_t s_view;
static uint32_t s_last_draw_ms;
static uint32_t s_message_until_ms;
static uint32_t s_last_usb_state;
static uint32_t s_last_media_generation;

static disc_entry_t s_discs[UI_MAX_DISCS];
static size_t s_disc_count;
static int s_disc_sel;
static int s_menu_sel;
static int s_menu_top;
static int s_confirm_sel;
static int s_new_profile_sel;
static int s_delete_sel;
static int s_pending_delete_sel;
static ui_action_t s_pending_action;
static uint16_t s_pending_profile = MEDIA_PROFILE_CDRW;
static char s_message[64];
static media_status_t s_snapshot;

static const media_status_t *ui_media(void)
{
    return &s_snapshot;
}

static void draw_header(const char *title);
static void draw_status(void);
static void draw_menu(void);
static void draw_discs(void);
static void draw_delete(void);
static void draw_confirm(void);
static void draw_new_profile(void);
static void draw_info(void);
static void draw_debug(void);
static void draw_message(void);
static void redraw(void);

static void trim_copy(char *dst, size_t dst_size, const char *src, size_t max_chars)
{
    if (!dst_size) return;
    size_t n = src ? strlen(src) : 0;
    if (n > max_chars) n = max_chars;
    if (n >= dst_size) n = dst_size - 1;
    if (src) memcpy(dst, src, n);
    dst[n] = '\0';
}

void ui_show_message(const char *text)
{
    if (text) {
        util_safe_copy(s_message, sizeof(s_message), text);
        s_message_until_ms = util_millis() + 1800;
        s_view = VIEW_MESSAGE;
    }
}

bool ui_init(const ui_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    if (!oled_init_ex(cfg->i2c_bus, cfg->oled_addr, cfg->oled_mock)) {
        return false;
    }
    if (cfg->enable_encoder) {
        encoder_init_ex(cfg->enc_a, cfg->enc_b, cfg->enc_btn);
    }
    s_view = VIEW_STATUS;
    s_last_draw_ms = 0;
    s_last_usb_state = 0xFFFFFFFF;
    s_last_media_generation = 0xFFFFFFFF;
    s_menu_sel = 0;
    s_menu_top = 0;
    s_disc_sel = 0;
    s_disc_count = 0;
    s_new_profile_sel = 0;
    s_pending_profile = MEDIA_PROFILE_CDRW;

    oled_clear();
    draw_header("OPI3B OPTICAL");
    oled_text_centered(24, "CD / DVD / BD", 1, true);
    oled_text_centered(38, "USB optical drive", 1, true);
    oled_text_centered(54, "v" UI_VERSION, 1, true);
    oled_flush();
    return true;
}

void ui_shutdown(void)
{
    oled_shutdown();
    encoder_shutdown();
}

static void draw_header(const char *title)
{
    oled_fill_rect(0, 0, 128, 11, true);
    oled_text(3, 2, title, 1, false);
    const char *usb = usb_status_configured() ? "USB HS" : "USB --";
    oled_text(128 - oled_text_width(usb, 1) - 3, 2, usb, 1, false);
}

static const char *status_short(const media_status_t *m)
{
    if (!m->present) return "NO DISC";
    if (m->kind == MEDIA_KIND_ISO) return "READ ONLY";
    if (m->write_protected) return "PROTECTED";
    if (m->blank) return "BLANK";
    if (!m->complete) return "INCOMPLETE";
    return "READY";
}

static void draw_status(void)
{
    const media_status_t *m = ui_media();
    char line[40];
    oled_clear();
    draw_header("OPI3B OPTICAL");

    if (!storage_ready()) {
        oled_text(3, 20, "STORAGE ERROR", 1, true);
        oled_text(3, 34, "Check disc directory", 1, true);
        oled_text(3, 54, "v" UI_VERSION, 1, true);
        return;
    }

    if (!m->present) {
        oled_text(3, 16, "NO DISC", 1, true);
        oled_text(3, 30, "Turn to browse discs", 1, true);
        oled_text(3, 42, "Press to open menu", 1, true);
        oled_text(3, 56, usb_status_configured() ? "USB READY" : "USB WAIT",
                  1, true);
        return;
    }

    oled_text(3, 14, media_profile_name(m->profile), 1, true);
    if (scsi_media_locked()) {
        oled_text(96, 14, "LOCK", 1, true);
    }

    char name[23];
    trim_copy(name, sizeof(name), m->name, 20);
    oled_text(3, 24, name, 1, true);

    snprintf(line, sizeof(line), "%-10s 2048", status_short(m));
    oled_text(3, 34, line, 1, true);

    uint64_t cap_bytes = (uint64_t)m->capacity_lba * 2048ull;
    uint64_t used_bytes = (uint64_t)m->data_lba * 2048ull;
    char cap[20], used[20];
    util_format_size(cap_bytes, cap, sizeof(cap));
    util_format_size(used_bytes, used, sizeof(used));
    snprintf(line, sizeof(line), "%s %s", cap, used);
    oled_text(3, 44, line, 1, true);

    int frac = 0;
    if (m->capacity_lba) {
        frac = (int)(((uint64_t)m->data_lba * 100ull) / m->capacity_lba);
    }
    snprintf(line, sizeof(line), "WR %d%%  %s", frac,
             usb_status_configured() ? "USB READY" : "USB WAIT");
    oled_text(3, 56, line, 1, true);
}

static void draw_menu(void)
{
    const media_status_t *m = ui_media();
    bool present = m->present;
    menu_item_t items[UI_MENU_ITEMS];
    int n = 0;
    items[n++] = (menu_item_t){"Discs", ACTION_NONE, 0};
    items[n++] = (menu_item_t){present ? "Eject" : "Reload", ACTION_EJECT, 1};
    items[n++] = (menu_item_t){"New blank", ACTION_NEW_BLANK, 2};
    items[n++] = (menu_item_t){"Delete disc", ACTION_DELETE, 3};
    items[n++] = (menu_item_t){"Erase", ACTION_ERASE, 4};
    items[n++] = (menu_item_t){"Info", ACTION_NONE, 5};
    items[n++] = (menu_item_t){"SCSI debug", ACTION_NONE, 6};
    items[n++] = (menu_item_t){"Back", ACTION_NONE, 7};

    oled_clear();
    char title[32];
    snprintf(title, sizeof(title), "MENU %d/%d", s_menu_sel + 1, n);
    draw_header(title);
    if (s_menu_top < 0) s_menu_top = 0;
    if (s_menu_top > n - UI_MENU_ROWS) s_menu_top = n - UI_MENU_ROWS;
    int first = s_menu_top;
    for (int row = 0; row < UI_MENU_ROWS && first + row < n; row++) {
        int idx = first + row;
        int y = 14 + row * 11;
        bool sel = idx == s_menu_sel;
        if (sel) oled_fill_rect(0, y - 2, 128, 10, true);
        oled_text(4, y, items[idx].label, 1, !sel);
    }
    oled_text(3, 57, "TURN  PRESS  HOLD=BACK", 1, true);
}

static const char *disc_tag(uint16_t profile)
{
    if (profile == MEDIA_PROFILE_CDROM) return "I";
    if (profile == MEDIA_PROFILE_DVDRW) return "D";
    if (profile == MEDIA_PROFILE_BDRE) return "B";
    return "C";
}

static void draw_discs(void)
{
    oled_clear();
    char title[24];
    snprintf(title, sizeof(title), "DISC %d/%d",
             s_disc_count ? s_disc_sel + 1 : 0, (int)s_disc_count);
    draw_header(title);
    if (s_disc_count == 0) {
        oled_text(3, 16, "No discs found", 1, true);
        oled_text(3, 28, "Put .ISO/.CDR/.DVD/.BD", 1, true);
        oled_text(3, 40, "in root or /discs", 1, true);
        oled_text(3, 57, "HOLD=BACK", 1, true);
        return;
    }
    int first = 0;
    if (s_disc_sel > 3) first = s_disc_sel - 3;
    for (int row = 0; row < 4; row++) {
        int idx = first + row;
        if (idx >= (int)s_disc_count) break;
        int y = 14 + row * 11;
        bool sel = idx == s_disc_sel;
        if (sel) oled_fill_rect(0, y - 2, 128, 10, true);
        char name[22];
        trim_copy(name, sizeof(name), s_discs[idx].name, 17);
        oled_text(3, y, name, 1, !sel);
        oled_text(118, y, disc_tag(s_discs[idx].profile), 1, !sel);
    }
    oled_text(3, 57, "PRESS=LOAD  HOLD=BACK", 1, true);
}

static void draw_delete(void)
{
    oled_clear();
    char title[24];
    snprintf(title, sizeof(title), "DELETE %d/%d",
             s_disc_count ? s_delete_sel + 1 : 0, (int)s_disc_count);
    draw_header(title);
    if (s_disc_count == 0) {
        oled_text(3, 20, "No discs found", 1, true);
        oled_text(3, 57, "HOLD=BACK", 1, true);
        return;
    }
    int first = 0;
    if (s_delete_sel > 3) first = s_delete_sel - 3;
    for (int row = 0; row < 4; row++) {
        int idx = first + row;
        if (idx >= (int)s_disc_count) break;
        int y = 14 + row * 11;
        bool sel = idx == s_delete_sel;
        if (sel) oled_fill_rect(0, y - 2, 128, 10, true);
        char name[22];
        trim_copy(name, sizeof(name), s_discs[idx].name, 17);
        oled_text(3, y, name, 1, !sel);
        oled_text(118, y, disc_tag(s_discs[idx].profile), 1, !sel);
    }
    oled_text(3, 57, "PRESS=DELETE HOLD=BACK", 1, true);
}

static void draw_new_profile(void)
{
    static const uint16_t profiles[] = {MEDIA_PROFILE_CDRW, MEDIA_PROFILE_DVDRW,
                                        MEDIA_PROFILE_BDRE};
    static const char *labels[] = {"CD-RW   700 MB", "DVD-RW  4.7 GB",
                                   "BD-RE   25 GB"};
    oled_clear();
    draw_header("NEW BLANK");
    for (int i = 0; i < 3; i++) {
        int y = 16 + i * 13;
        bool sel = i == s_new_profile_sel;
        if (sel) oled_fill_rect(0, y - 2, 128, 11, true);
        oled_text(8, y, labels[i], 1, !sel);
    }
    s_pending_profile = profiles[s_new_profile_sel];
    oled_text(3, 57, "PRESS=OK HOLD=BACK", 1, true);
}

static void draw_confirm(void)
{
    oled_clear();
    draw_header("CONFIRM");
    const char *what;
    const char *sub;
    if (s_pending_action == ACTION_DELETE) {
        what = "Delete this image?";
        sub = (s_pending_delete_sel >= 0 &&
               s_pending_delete_sel < (int)s_disc_count)
                  ? s_discs[s_pending_delete_sel].name
                  : "File + .meta removed";
    } else if (s_pending_action == ACTION_ERASE) {
        what = "Erase this disc?";
        sub = "Data will be lost";
    } else if (s_pending_profile == MEDIA_PROFILE_DVDRW) {
        what = "Create DVD-RW 4.7G?";
        sub = "Sparse image, quick";
    } else if (s_pending_profile == MEDIA_PROFILE_BDRE) {
        what = "Create BD-RE 25G?";
        sub = "Sparse image, quick";
    } else {
        what = "Create CD-RW 700M?";
        sub = "Takes a few seconds";
    }
    oled_text_centered(20, what, 1, true);
    oled_text_centered(33, sub, 1, true);
    const char *yes = "YES";
    const char *no = "NO";
    if (s_confirm_sel == 0) {
        oled_fill_rect(12, 46, 45, 12, true);
        oled_text(25, 48, yes, 1, false);
        oled_text(86, 48, no, 1, true);
    } else {
        oled_text(25, 48, yes, 1, true);
        oled_fill_rect(70, 46, 45, 12, true);
        oled_text(86, 48, no, 1, false);
    }
}

static void draw_info(void)
{
    const media_status_t *m = ui_media();
    oled_clear();
    draw_header("DISC INFO");
    if (!m->present) {
        oled_text(3, 20, "No disc loaded", 1, true);
        oled_text(3, 32, "Press to return", 1, true);
        return;
    }
    char line[40];
    oled_text(3, 13, m->path, 1, true);
    snprintf(line, sizeof(line), "TYPE %s", media_profile_name(m->profile));
    oled_text(3, 24, line, 1, true);
    snprintf(line, sizeof(line), "CAP  %lu sectors",
             (unsigned long)m->capacity_lba);
    oled_text(3, 34, line, 1, true);
    snprintf(line, sizeof(line), "DATA %lu NWA %lu",
             (unsigned long)m->data_lba, (unsigned long)m->nwa);
    oled_text(3, 44, line, 1, true);
    snprintf(line, sizeof(line), "STATE %s%s", status_short(m),
             m->complete ? " CLOSED" : "");
    oled_text(3, 54, line, 1, true);
}

static void draw_debug(void)
{
    oled_clear();
    draw_header("SCSI TRACE");
    uint8_t count = usb_trace_count();
    if (count == 0) {
        oled_text(3, 20, "No commands yet", 1, true);
        oled_text(3, 57, "HOLD=BACK", 1, true);
        return;
    }

    usb_trace_t t;
    (void)usb_trace_get((uint8_t)(count - 1), &t);
    const char *st = t.status == 0xFF ? "--" : (t.status ? "ERR" : "OK");
    char line[40];
    snprintf(line, sizeof(line), "LAST %02X %s %s %lu", t.opcode,
             t.dir_in ? "IN" : "OUT", st, (unsigned long)t.data_len);
    oled_text(2, 12, line, 1, true);

    snprintf(line, sizeof(line), "L%lu B%lu S%lu%s", (unsigned long)t.lba,
             (unsigned long)t.blocks, (unsigned long)t.source_block_size,
             t.discard ? " D" : "");
    oled_text(3, 21, line, 1, true);

    snprintf(line, sizeof(line), "K%02X A%02X Q%02X", t.key, t.asc, t.ascq);
    oled_text(3, 30, line, 1, true);

    char recent[24] = {0};
    size_t pos = 0;
    uint8_t first = count > 5 ? (uint8_t)(count - 5) : 0;
    for (uint8_t i = first; i < count && pos + 4 < sizeof(recent); i++) {
        usb_trace_t r;
        if (!usb_trace_get(i, &r)) continue;
        pos += (size_t)snprintf(recent + pos, sizeof(recent) - pos, "%02X ",
                                r.opcode);
    }
    oled_text(3, 39, "RECENT", 1, true);
    oled_text(3, 48, recent, 1, true);
    oled_text(3, 57, "HOLD=BACK", 1, true);
}

static void draw_message(void)
{
    oled_clear();
    draw_header("NOTICE");
    oled_fill_rect(6, 20, 116, 24, true);
    oled_text(10, 28, s_message, 1, false);
    oled_text(3, 56, "Please wait...", 1, true);
}

static void redraw(void)
{
    switch (s_view) {
    case VIEW_STATUS: draw_status(); break;
    case VIEW_MENU: draw_menu(); break;
    case VIEW_DISCS: draw_discs(); break;
    case VIEW_CONFIRM: draw_confirm(); break;
    case VIEW_NEWPROFILE: draw_new_profile(); break;
    case VIEW_DELETE: draw_delete(); break;
    case VIEW_INFO: draw_info(); break;
    case VIEW_DEBUG: draw_debug(); break;
    case VIEW_MESSAGE: draw_message(); break;
    default: draw_status(); break;
    }
    oled_flush();
    s_last_draw_ms = util_millis();
}

static void open_menu(void)
{
    s_menu_sel = 0;
    s_menu_top = 0;
    s_view = VIEW_MENU;
}

static void rescan_discs(void)
{
    s_disc_count = storage_scan_discs(s_discs, UI_MAX_DISCS);
    s_disc_sel = 0;
    const media_status_t *m = ui_media();
    for (size_t i = 0; i < s_disc_count; i++) {
        if (m->path[0] && strcmp(s_discs[i].path, m->path) == 0) {
            s_disc_sel = (int)i;
            break;
        }
    }
}

static bool host_locked_message(void)
{
    if (scsi_media_locked()) {
        ui_show_message("HOST LOCKED DRIVE");
        return true;
    }
    return false;
}

static void do_eject(void)
{
    if (host_locked_message()) return;
    const media_status_t *m = ui_media();
    media_lock();
    if (m->present) {
        media_eject();
        scsi_notify_media_change(false);
        media_unlock();
        ui_show_message("DISC EJECTED");
    } else {
        bool ok = media_reload();
        if (ok) {
            scsi_notify_media_change(true);
        }
        media_unlock();
        ui_show_message(ok ? "DISC LOADED" : "NO DISC TO LOAD");
    }
}

static void do_new_blank(uint16_t profile)
{
    if (host_locked_message()) return;
    oled_clear();
    draw_header("FORMAT");
    oled_text(3, 24, "Allocating blank disc...", 1, true);
    oled_text(3, 36, media_profile_name(profile), 1, true);
    oled_flush();

    char path[STORAGE_PATH_MAX];
    media_lock();
    bool ok = media_new_blank_profile(path, sizeof(path), profile);
    if (ok) {
        storage_save_last_disc(path);
        scsi_notify_media_change(true);
    }
    media_unlock();
    ui_show_message(ok ? "BLANK DISC READY" : "CREATE FAILED");
}

static void do_delete(void)
{
    if (host_locked_message()) return;
    if (s_pending_delete_sel < 0 ||
        s_pending_delete_sel >= (int)s_disc_count) {
        ui_show_message("NO DISC SELECTED");
        return;
    }
    disc_entry_t *d = &s_discs[s_pending_delete_sel];
    media_lock();
    const media_status_t *m = media_get();
    bool current = m->present && strcmp(m->path, d->path) == 0;
    bool ok = false;
    if (!current) {
        ok = storage_delete_disc(d->path);
    }
    media_unlock();
    if (current) {
        ui_show_message("EJECT FIRST");
        return;
    }
    if (!ok) {
        ui_show_message("DELETE FAILED");
        return;
    }
    rescan_discs();
    if (s_delete_sel >= (int)s_disc_count) {
        s_delete_sel = s_disc_count ? (int)s_disc_count - 1 : 0;
    }
    ui_show_message("IMAGE DELETED");
}

static void do_erase(void)
{
    if (host_locked_message()) return;
    LOGF("UI: erase requested");
    media_lock();
    bool ok = media_blank();
    if (ok) {
        scsi_notify_media_change(true);
    }
    media_unlock();
    ui_show_message(ok ? "DISC ERASED" : "ERASE FAILED");
}

static void execute_action(ui_action_t action)
{
    switch (action) {
    case ACTION_EJECT: do_eject(); break;
    case ACTION_NEW_BLANK: do_new_blank(s_pending_profile); break;
    case ACTION_ERASE: do_erase(); break;
    case ACTION_DELETE: do_delete(); break;
    default: break;
    }
}

static void handle_menu_press(void)
{
    if (s_menu_sel == 0) {
        rescan_discs();
        s_view = VIEW_DISCS;
        return;
    }
    if (s_menu_sel == 1) {
        execute_action(ACTION_EJECT);
        return;
    }
    if (s_menu_sel == 2) {
        s_new_profile_sel = 0;
        s_view = VIEW_NEWPROFILE;
        return;
    }
    if (s_menu_sel == 3) {
        rescan_discs();
        s_delete_sel = 0;
        s_view = VIEW_DELETE;
        return;
    }
    if (s_menu_sel == 4) {
        const media_status_t *m = ui_media();
        if (!m->present || !media_is_writable_kind(m->kind)) {
            ui_show_message("NO REWRITABLE DISC");
            return;
        }
        if (host_locked_message()) return;
        s_pending_action = ACTION_ERASE;
        s_confirm_sel = 0;
        s_view = VIEW_CONFIRM;
        return;
    }
    if (s_menu_sel == 5) {
        s_view = VIEW_INFO;
        return;
    }
    if (s_menu_sel == 6) {
        s_view = VIEW_DEBUG;
        return;
    }
    s_view = VIEW_STATUS;
}

static void handle_disc_press(void)
{
    if (s_disc_count == 0) {
        s_view = VIEW_MENU;
        return;
    }
    if (host_locked_message()) return;
    disc_entry_t *d = &s_discs[s_disc_sel];
    LOGF("UI: selecting disc %s", d->path);
    media_lock();
    bool ok = media_load(d->path);
    if (ok) {
        storage_save_last_disc(d->path);
        scsi_notify_media_change(true);
    }
    media_unlock();
    if (!ok) {
        LOGF("UI: disc load failed");
        ui_show_message("CANNOT OPEN DISC");
        return;
    }
    ui_show_message("DISC LOADED");
}

static void handle_input(const encoder_event_t *ev)
{
    if (ev->long_press) {
        if (s_view != VIEW_STATUS) {
            s_view = VIEW_STATUS;
        }
        return;
    }

    if (s_view == VIEW_MESSAGE) {
        if (ev->pressed) s_view = VIEW_STATUS;
        return;
    }

    switch (s_view) {
    case VIEW_STATUS:
        if (ev->pressed) open_menu();
        break;
    case VIEW_MENU:
        if (ev->delta) {
            s_menu_sel += ev->delta;
            if (s_menu_sel < 0) s_menu_sel = UI_MENU_ITEMS - 1;
            if (s_menu_sel >= UI_MENU_ITEMS) s_menu_sel = 0;
            if (s_menu_sel < s_menu_top) s_menu_top = s_menu_sel;
            if (s_menu_sel >= s_menu_top + UI_MENU_ROWS) {
                s_menu_top = s_menu_sel - (UI_MENU_ROWS - 1);
            }
            if (s_menu_top < 0) s_menu_top = 0;
            if (s_menu_top > UI_MENU_ITEMS - UI_MENU_ROWS) {
                s_menu_top = UI_MENU_ITEMS - UI_MENU_ROWS;
            }
        }
        if (ev->pressed) handle_menu_press();
        break;
    case VIEW_DISCS:
        if (ev->delta && s_disc_count) {
            s_disc_sel += ev->delta;
            if (s_disc_sel < 0) s_disc_sel = (int)s_disc_count - 1;
            if (s_disc_sel >= (int)s_disc_count) s_disc_sel = 0;
        }
        if (ev->pressed) handle_disc_press();
        break;
    case VIEW_DELETE:
        if (ev->delta && s_disc_count) {
            s_delete_sel += ev->delta;
            if (s_delete_sel < 0) s_delete_sel = (int)s_disc_count - 1;
            if (s_delete_sel >= (int)s_disc_count) s_delete_sel = 0;
        }
        if (ev->pressed) {
            if (s_disc_count == 0) {
                ui_show_message("NO DISCS");
            } else {
                s_pending_delete_sel = s_delete_sel;
                s_pending_action = ACTION_DELETE;
                s_confirm_sel = 0;
                s_view = VIEW_CONFIRM;
            }
        }
        break;
    case VIEW_NEWPROFILE:
        if (ev->delta) {
            s_new_profile_sel += ev->delta;
            if (s_new_profile_sel < 0) s_new_profile_sel = 2;
            if (s_new_profile_sel > 2) s_new_profile_sel = 0;
        }
        if (ev->pressed) {
            s_pending_profile = s_new_profile_sel == 0 ? MEDIA_PROFILE_CDRW :
                                s_new_profile_sel == 1 ? MEDIA_PROFILE_DVDRW :
                                MEDIA_PROFILE_BDRE;
            s_pending_action = ACTION_NEW_BLANK;
            s_confirm_sel = 0;
            s_view = VIEW_CONFIRM;
        }
        break;
    case VIEW_CONFIRM:
        if (ev->delta) s_confirm_sel = s_confirm_sel ? 0 : 1;
        if (ev->pressed) {
            if (s_confirm_sel == 0) execute_action(s_pending_action);
            else s_view = VIEW_MENU;
        }
        break;
    case VIEW_INFO:
    case VIEW_DEBUG:
        if (ev->pressed) s_view = VIEW_STATUS;
        break;
    default:
        break;
    }
}

void ui_task(void)
{
    media_lock();
    s_snapshot = *media_get();
    media_unlock();

    encoder_event_t ev;
    encoder_poll(&ev);
    if (ev.delta || ev.pressed || ev.long_press) {
        LOGF("UI: input d=%d p=%d l=%d view=%d", ev.delta, ev.pressed,
             ev.long_press, (int)s_view);
        handle_input(&ev);
        redraw();
    }

    uint32_t now = util_millis();
    if (s_view == VIEW_MESSAGE && (int32_t)(now - s_message_until_ms) >= 0) {
        s_view = VIEW_STATUS;
        redraw();
        return;
    }

    const media_status_t *m = ui_media();
    uint32_t usb_state = usb_status_configured() ? 1u : 0u;
    bool changed = (usb_state != s_last_usb_state) ||
                   (m->generation != s_last_media_generation);
    if (changed || (now - s_last_draw_ms >= UI_REFRESH_MS)) {
        s_last_usb_state = usb_state;
        s_last_media_generation = m->generation;
        redraw();
    }
}
