#ifndef CDRW_OLED_H
#define CDRW_OLED_H

#include <stdbool.h>
#include <stdint.h>

bool oled_init(void);
bool oled_init_ex(int bus, int addr, const char *mock_path);
void oled_shutdown(void);
bool oled_present(void);
void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_hline(int x, int y, int w, bool on);
void oled_vline(int x, int y, int h, bool on);
void oled_rect(int x, int y, int w, int h, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
void oled_text(int x, int y, const char *s, int scale, bool color);
void oled_text_centered(int y, const char *s, int scale, bool color);
int oled_text_width(const char *s, int scale);
void oled_flush(void);

#endif
