/*
 * SSD1306 128x64 OLED over Linux i2c-dev.
 *
 * The drawing API is identical to the RP2350 firmware's oled.c; only the
 * transport changed.  A missing panel never stops the USB drive: oled_init_ex
 * reports false and the UI stays headless.
 *
 * For bench testing without a panel, pass a mock path: each flush writes a
 * P5 PGM (128x64) that tools/pgm2png.py can convert.
 */
#include "oled.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "debug_log.h"
#include "font5x7.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)

static uint8_t s_fb[OLED_WIDTH * OLED_PAGES];
static bool s_present;
static int s_fd = -1;
static int s_bus = -1;
static int s_addr = 0x3C;
static char s_mock[256];

static bool i2c_probe(int fd, int addr)
{
    struct i2c_smbus_ioctl_data args = {
        .read_write = I2C_SMBUS_WRITE,
        .command = 0,
        .size = I2C_SMBUS_QUICK,
        .data = NULL,
    };
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        return false;
    }
    return ioctl(fd, I2C_SMBUS, &args) >= 0;
}

static bool i2c_write(uint8_t control, const uint8_t *data, size_t len)
{
    if (!s_present || s_fd < 0) {
        return false;
    }
    uint8_t chunk[33];
    while (len) {
        size_t n = len > 32 ? 32 : len;
        chunk[0] = control;
        memcpy(chunk + 1, data, n);
        if (write(s_fd, chunk, n + 1) != (ssize_t)(n + 1)) {
            LOGF("oled: i2c write failed: %s", strerror(errno));
            s_present = false;
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

static bool cmd(uint8_t c)
{
    return i2c_write(0x00, &c, 1);
}

static bool oled_hw_init(void)
{
    static const uint8_t seq[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock divide */
        0xA8, 0x3F, /* multiplex 64 */
        0xD3, 0x00, /* display offset */
        0x40,       /* start line */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x00, /* horizontal addressing */
        0xA1,       /* segment remap */
        0xC8,       /* COM scan remap */
        0xDA, 0x12, /* COM pins */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1, /* precharge */
        0xDB, 0x40, /* VCOM detect */
        0xA4,       /* resume RAM content */
        0xA6,       /* normal (not inverted) */
        0x2E,       /* no scroll */
        0xAF,       /* display on */
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(seq); i++) {
        ok = cmd(seq[i]) && ok;
    }
    return ok;
}

static bool open_bus(int bus, int addr)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    if (!i2c_probe(fd, addr)) {
        close(fd);
        return false;
    }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        close(fd);
        return false;
    }
    s_fd = fd;
    s_bus = bus;
    s_addr = addr;
    return true;
}

bool oled_init_ex(int bus, int addr, const char *mock_path)
{
    s_addr = addr > 0 ? addr : 0x3C;
    s_mock[0] = '\0';
    if (mock_path && mock_path[0]) {
        snprintf(s_mock, sizeof(s_mock), "%s", mock_path);
        s_present = true;
        oled_clear();
        oled_flush();
        LOGF("oled: mock panel -> %s", s_mock);
        return true;
    }

    int candidates[10];
    int count = 0;
    if (bus >= 0) {
        candidates[count++] = bus;
    } else {
        if (access("/dev/i2c-2", F_OK) == 0) {
            candidates[count++] = 2;
        }
        for (int b = 0; b <= 9 && count < 10; b++) {
            if (b == 2 && count > 0 && candidates[0] == 2) {
                continue;
            }
            char path[32];
            snprintf(path, sizeof(path), "/dev/i2c-%d", b);
            if (access(path, F_OK) == 0) {
                candidates[count++] = b;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        int addrs[2] = {s_addr, s_addr == 0x3C ? 0x3D : 0x3C};
        for (int a = 0; a < 2; a++) {
            if (open_bus(candidates[i], addrs[a])) {
                s_present = true;
                if (oled_hw_init()) {
                    oled_clear();
                    oled_flush();
                    LOGF("oled: SSD1306 at i2c-%d 0x%02X", s_bus, s_addr);
                    return true;
                }
                close(s_fd);
                s_fd = -1;
                s_present = false;
            }
        }
    }
    LOGF("oled: no SSD1306 found (run tools/enable-i2c2.sh and check wiring)");
    return false;
}

bool oled_init(void)
{
    return oled_init_ex(-1, 0x3C, NULL);
}

void oled_shutdown(void)
{
    if (s_fd >= 0) {
        (void)cmd(0xAE);
        close(s_fd);
        s_fd = -1;
    }
    s_present = false;
}

bool oled_present(void)
{
    return s_present;
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if ((unsigned)x >= OLED_WIDTH || (unsigned)y >= OLED_HEIGHT) {
        return;
    }
    uint32_t idx = (uint32_t)(y / 8) * OLED_WIDTH + (uint32_t)x;
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) s_fb[idx] |= mask;
    else s_fb[idx] &= (uint8_t)~mask;
}

void oled_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) oled_pixel(x + i, y, on);
}

void oled_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) oled_pixel(x, y + i, on);
}

void oled_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    oled_hline(x, y, w, on);
    oled_hline(x, y + h - 1, w, on);
    oled_vline(x, y, h, on);
    oled_vline(x + w - 1, y, h, on);
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        oled_hline(x, yy, w, on);
    }
}

int oled_text_width(const char *s, int scale)
{
    if (!s || !*s) return 0;
    int n = (int)strlen(s);
    return n * 6 * scale - scale;
}

void oled_text(int x, int y, const char *s, int scale, bool color)
{
    if (!s) return;
    if (scale < 1) scale = 1;
    int cx = x;
    for (const char *p = s; *p; p++) {
        unsigned c = (unsigned char)*p;
        if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = '?';
        const uint8_t *glyph = font5x7[c - FONT5X7_FIRST];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; row++) {
                bool on = (bits >> row) & 1;
                if (scale == 1) {
                    oled_pixel(cx + col, y + row, on ? color : !color);
                } else {
                    oled_fill_rect(cx + col * scale, y + row * scale, scale,
                                   scale, on ? color : !color);
                }
            }
        }
        for (int row = 0; row < 7 * scale; row++) {
            if (scale == 1) {
                oled_pixel(cx + 5, y + row, !color);
            } else {
                oled_fill_rect(cx + 5 * scale, y + row * scale, scale, scale,
                               !color);
            }
        }
        cx += 6 * scale;
    }
}

void oled_text_centered(int y, const char *s, int scale, bool color)
{
    int w = oled_text_width(s, scale);
    oled_text((OLED_WIDTH - w) / 2, y, s, scale, color);
}

static void dump_pgm(void)
{
    char tmp[280];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_mock);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return;
    }
    fprintf(fp, "P5\n%d %d\n255\n", OLED_WIDTH, OLED_HEIGHT);
    for (int y = 0; y < OLED_HEIGHT; y++) {
        for (int x = 0; x < OLED_WIDTH; x++) {
            uint8_t bit = (s_fb[(y / 8) * OLED_WIDTH + x] >> (y & 7)) & 1;
            fputc(bit ? 0xFF : 0x00, fp);
        }
    }
    fclose(fp);
    (void)rename(tmp, s_mock);
}

void oled_flush(void)
{
    if (!s_present) return;
    if (s_mock[0]) {
        dump_pgm();
        return;
    }
    for (int page = 0; page < OLED_PAGES; page++) {
        (void)cmd((uint8_t)(0xB0 | page));
        (void)cmd(0x00);
        (void)cmd(0x10);
        (void)i2c_write(0x40, s_fb + page * OLED_WIDTH, OLED_WIDTH);
    }
}
