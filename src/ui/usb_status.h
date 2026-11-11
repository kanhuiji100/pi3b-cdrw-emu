#ifndef CDRW_USB_STATUS_H
#define CDRW_USB_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "bot.h"

typedef bot_trace_t usb_trace_t;

bool usb_status_bound(void);
bool usb_status_configured(void);
uint8_t usb_trace_count(void);
bool usb_trace_get(uint8_t index, usb_trace_t *out);

#endif
