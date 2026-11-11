#include "media_lock.h"

#include <pthread.h>

static pthread_mutex_t s_mutex;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void init_mutex(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void media_lock(void)
{
    pthread_once(&s_once, init_mutex);
    pthread_mutex_lock(&s_mutex);
}

void media_unlock(void)
{
    pthread_mutex_unlock(&s_mutex);
}
