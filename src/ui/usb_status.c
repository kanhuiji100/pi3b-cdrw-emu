#include "usb_status.h"

#include "ffs_gadget.h"

bool usb_status_bound(void)
{
    return ffs_gadget_bound();
}

bool usb_status_configured(void)
{
    return ffs_gadget_configured();
}

uint8_t usb_trace_count(void)
{
    return bot_trace_count();
}

bool usb_trace_get(uint8_t index, usb_trace_t *out)
{
    return bot_trace_get(index, out);
}
