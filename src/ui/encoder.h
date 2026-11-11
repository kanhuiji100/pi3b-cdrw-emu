#ifndef CDRW_ENCODER_H
#define CDRW_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int8_t delta;      /* detents since the previous poll */
    bool pressed;      /* debounced press event */
    bool long_press;   /* emitted once after holding */
} encoder_event_t;

void encoder_init(void);
void encoder_init_ex(int a, int b, int btn);
void encoder_set_steps(int steps);
void encoder_set_debug(bool on);
void encoder_shutdown(void);
bool encoder_available(void);
void encoder_poll(encoder_event_t *event);

#endif
