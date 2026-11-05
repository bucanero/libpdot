#include <stdlib.h>

#include "pdot.h"
#include "thread.h"

struct start {
    void (*fn)(void *);
    void  *arg;
};

#if defined(_WIN32)

int pdot_mutex_init(pdot_mutex *m)
{
    InitializeSRWLock(m);
    return 0;
}

void pdot_mutex_destroy(pdot_mutex *m)
{
    (void)m;
}

void pdot_mutex_lock(pdot_mutex *m)
{
    AcquireSRWLockExclusive(m);
}

void pdot_mutex_unlock(pdot_mutex *m)
{
    ReleaseSRWLockExclusive(m);
}

int pdot_cond_init(pdot_cond *c)
{
    InitializeConditionVariable(c);
    return 0;
}

void pdot_cond_destroy(pdot_cond *c)
{
    (void)c;
}

void pdot_cond_wait(pdot_cond *c, pdot_mutex *m)
{
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}

void pdot_cond_broadcast(pdot_cond *c)
{
    WakeAllConditionVariable(c);
}

static DWORD WINAPI trampoline(LPVOID p)
{
    struct start s = *(struct start *)p;

    free(p);
    s.fn(s.arg);
    return 0;
}

int pdot_thread_start(pdot_thread *t, void (*fn)(void *), void *arg)
{
    struct start *s = malloc(sizeof *s);

    if (!s)
        return PDOT_E_NOMEM;
    s->fn = fn;
    s->arg = arg;
    if (!(*t = CreateThread(NULL, 0, trampoline, s, 0, NULL))) {
        free(s);
        return PDOT_E_NOMEM;
    }
    return 0;
}

void pdot_thread_join(pdot_thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else

int pdot_mutex_init(pdot_mutex *m)
{
    return pthread_mutex_init(m, NULL) ? PDOT_E_NOMEM : 0;
}

void pdot_mutex_destroy(pdot_mutex *m)
{
    pthread_mutex_destroy(m);
}

void pdot_mutex_lock(pdot_mutex *m)
{
    pthread_mutex_lock(m);
}

void pdot_mutex_unlock(pdot_mutex *m)
{
    pthread_mutex_unlock(m);
}

int pdot_cond_init(pdot_cond *c)
{
    return pthread_cond_init(c, NULL) ? PDOT_E_NOMEM : 0;
}

void pdot_cond_destroy(pdot_cond *c)
{
    pthread_cond_destroy(c);
}

void pdot_cond_wait(pdot_cond *c, pdot_mutex *m)
{
    pthread_cond_wait(c, m);
}

void pdot_cond_broadcast(pdot_cond *c)
{
    pthread_cond_broadcast(c);
}

static void *trampoline(void *p)
{
    struct start s = *(struct start *)p;

    free(p);
    s.fn(s.arg);
    return NULL;
}

int pdot_thread_start(pdot_thread *t, void (*fn)(void *), void *arg)
{
    struct start *s = malloc(sizeof *s);

    if (!s)
        return PDOT_E_NOMEM;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, trampoline, s)) {
        free(s);
        return PDOT_E_NOMEM;
    }
    return 0;
}

void pdot_thread_join(pdot_thread t)
{
    pthread_join(t, NULL);
}

#endif
