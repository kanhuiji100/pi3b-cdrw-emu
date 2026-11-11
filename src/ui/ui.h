#ifndef CDRW_UI_H
#define CDRW_UI_H

#include <stdbool.h>

typedef struct {
    int i2c_bus;          /* -1 = auto */
    int oled_addr;        /* 0x3C typical */
    int enc_a;
    int enc_b;
    int enc_btn;
    bool enable_encoder;
    const char *oled_mock; /* optional PGM dump path for bench testing */
} ui_config_t;

bool ui_init(const ui_config_t *cfg);
void ui_task(void);
void ui_shutdown(void);
void ui_show_message(const char *text);

#endif
