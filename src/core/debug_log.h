#ifndef CDRW_DEBUG_LOG_H
#define CDRW_DEBUG_LOG_H

#include <stdio.h>

#include "util.h"

#ifndef CDRW_DEBUG_LOG
#define CDRW_DEBUG_LOG 1
#endif

#ifndef CDRW_READ_TRACE
#define CDRW_READ_TRACE 0
#endif

#if CDRW_DEBUG_LOG
#define LOGF(...)                                                    \
    do {                                                             \
        printf("[%8lu] ", (unsigned long)util_millis());             \
        printf(__VA_ARGS__);                                         \
        printf("\r\n");                                              \
    } while (0)
#else
#define LOGF(...) do { } while (0)
#endif

#endif
