/*
 * EC11 quadrature encoder + push button on Linux GPIO character devices.
 *
 * A dedicated thread samples A/B at 2 ms; the UI thread only pops the
 * accumulated events.  Sampling in the UI thread was too slow (50 ms) and
 * missed most of the four transitions of a detent, which made rotation feel
 * almost dead while the level-debounced button still worked.
 *
 * Uses the GPIO v2 UAPI directly (no libgpiod dependency).  Lines are
 * requested as inputs with an internal pull-up, so a bare encoder works and
 * an unconnected header pin does not float into spurious events.
 *
 * GPIO numbers are global RK3566 numbers (same ones printed by wiringOP):
 * default A=118 (pin 11), B=128 (pin 13), button=129 (pin 18).
 */
#include "encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "debug_log.h"
#include "util.h"

#define BTN_DEBOUNCE_MS 20u
#define BTN_LONG_MS 800u
#define SAMPLE_US 2000

static const int8_t s_quad_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static int s_fd_a = -1;
static int s_fd_b = -1;
static int s_fd_btn = -1;
static bool s_available;
static int s_steps = 4;
static bool s_debug;

static pthread_t s_thread;
static volatile bool s_run;
static pthread_mutex_t s_ev_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_pending_delta;
static bool s_pending_press;
static bool s_pending_long;

/* Thread-owned quadrature/button state. */
static uint8_t s_prev_state;
static int8_t s_accum;
static bool s_btn_raw;
static bool s_btn_stable;
static uint32_t s_btn_change_ms;
static uint32_t s_btn_down_ms;
static bool s_btn_long_sent;
static bool s_btn_pending;

static int request_line(int gpio)
{
    if (gpio < 0) {
        return -1;
    }
    int chip_idx = gpio / 32;
    int offset = gpio % 32;
    char path[32];
    snprintf(path, sizeof(path), "/dev/gpiochip%d", chip_idx);
    int chip = open(path, O_RDWR | O_CLOEXEC);
    if (chip < 0) {
        LOGF("encoder: open %s: %s", path, strerror(errno));
        return -1;
    }
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = (uint32_t)offset;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof(req.consumer), "pi3b-cdrw");
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                       GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    int rc = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip);
    if (rc < 0) {
        LOGF("encoder: request gpio%d (%s line %d): %s", gpio, path, offset,
             strerror(errno));
        return -1;
    }
    return req.fd;
}

static int read_line(int fd)
{
    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof(vals));
    vals.mask = 1;
    if (fd < 0 || ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
        return -1;
    }
    return (int)(vals.bits & 1);
}

static void push_delta(int d)
{
    pthread_mutex_lock(&s_ev_mutex);
    s_pending_delta += d;
    pthread_mutex_unlock(&s_ev_mutex);
}

static void push_press(bool long_press)
{
    pthread_mutex_lock(&s_ev_mutex);
    if (long_press) {
        s_pending_long = true;
    } else {
        s_pending_press = true;
    }
    pthread_mutex_unlock(&s_ev_mutex);
}

static void *encoder_thread_fn(void *arg)
{
    (void)arg;
    while (s_run) {
        int va = read_line(s_fd_a);
        int vb = read_line(s_fd_b);
        if (va >= 0 && vb >= 0) {
            uint8_t state = (uint8_t)((va ? 2 : 0) | (vb ? 1 : 0));
            if (state != s_prev_state) {
                if (s_debug) {
                    LOGF("encoder: state %u -> %u", s_prev_state, state);
                }
                s_accum = (int8_t)(s_accum +
                                   s_quad_table[(s_prev_state << 2) | state]);
                s_prev_state = state;
                if (s_accum >= s_steps) {
                    s_accum = 0;
                    push_delta(1);
                } else if (s_accum <= -s_steps) {
                    s_accum = 0;
                    push_delta(-1);
                }
            }
        }

        uint32_t now = util_millis();
        int vbtn = read_line(s_fd_btn);
        if (vbtn >= 0) {
            bool raw = vbtn != 0; /* active low: 0 = pressed */
            if (raw != s_btn_raw) {
                s_btn_raw = raw;
                s_btn_change_ms = now;
            }
            if ((now - s_btn_change_ms) >= BTN_DEBOUNCE_MS &&
                raw != s_btn_stable) {
                s_btn_stable = raw;
                if (!s_btn_stable) {
                    s_btn_down_ms = now;
                    s_btn_long_sent = false;
                    s_btn_pending = true;
                } else if (s_btn_pending) {
                    if (!s_btn_long_sent) {
                        push_press(false);
                    }
                    s_btn_pending = false;
                }
            }
            if (s_btn_pending && !s_btn_long_sent &&
                (now - s_btn_down_ms) >= BTN_LONG_MS) {
                s_btn_long_sent = true;
                push_press(true);
            }
        }
        usleep(SAMPLE_US);
    }
    return NULL;
}

void encoder_set_steps(int steps)
{
    if (steps < 1) steps = 1;
    if (steps > 8) steps = 8;
    s_steps = steps;
}

void encoder_set_debug(bool on)
{
    s_debug = on;
}

void encoder_init_ex(int a, int b, int btn)
{
    s_fd_a = request_line(a);
    s_fd_b = request_line(b);
    s_fd_btn = request_line(btn);
    if (s_fd_a < 0 || s_fd_b < 0 || s_fd_btn < 0) {
        LOGF("encoder: unavailable (need free GPIOs %d/%d/%d)", a, b, btn);
        encoder_shutdown();
        return;
    }
    int va = read_line(s_fd_a);
    int vb = read_line(s_fd_b);
    int vbtn = read_line(s_fd_btn);
    if (va < 0 || vb < 0 || vbtn < 0) {
        LOGF("encoder: read failed: %s", strerror(errno));
        encoder_shutdown();
        return;
    }
    s_prev_state = (uint8_t)((va ? 2 : 0) | (vb ? 1 : 0));
    s_btn_raw = vbtn != 0;
    s_btn_stable = s_btn_raw;
    s_btn_change_ms = util_millis();
    s_btn_pending = false;
    s_accum = 0;
    s_pending_delta = 0;
    s_pending_press = false;
    s_pending_long = false;
    s_run = true;
    if (pthread_create(&s_thread, NULL, encoder_thread_fn, NULL) != 0) {
        LOGF("encoder: cannot start sampling thread");
        s_run = false;
        encoder_shutdown();
        return;
    }
    s_available = true;
    LOGF("encoder: EC11 ready on GPIO %d/%d/%d (%d steps/detent)",
         a, b, btn, s_steps);
}

void encoder_init(void)
{
    encoder_init_ex(118, 128, 129);
}

bool encoder_available(void)
{
    return s_available;
}

void encoder_shutdown(void)
{
    if (s_run) {
        s_run = false;
        pthread_join(s_thread, NULL);
    }
    if (s_fd_a >= 0) close(s_fd_a);
    if (s_fd_b >= 0) close(s_fd_b);
    if (s_fd_btn >= 0) close(s_fd_btn);
    s_fd_a = s_fd_b = s_fd_btn = -1;
    s_available = false;
    pthread_mutex_lock(&s_ev_mutex);
    s_pending_delta = 0;
    s_pending_press = false;
    s_pending_long = false;
    pthread_mutex_unlock(&s_ev_mutex);
}

void encoder_poll(encoder_event_t *event)
{
    event->delta = 0;
    event->pressed = false;
    event->long_press = false;
    if (!s_available) {
        return;
    }
    pthread_mutex_lock(&s_ev_mutex);
    int d = s_pending_delta;
    s_pending_delta = 0;
    event->pressed = s_pending_press;
    event->long_press = s_pending_long;
    s_pending_press = false;
    s_pending_long = false;
    pthread_mutex_unlock(&s_ev_mutex);
    if (d > 127) d = 127;
    if (d < -127) d = -127;
    event->delta = (int8_t)d;
}
