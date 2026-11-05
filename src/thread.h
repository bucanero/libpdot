/* Minimal threads, mutexes and condition variables: POSIX threads or Win32. */
#ifndef PDOT_THREAD_H
#define PDOT_THREAD_H

#if defined(_WIN32)
#include <windows.h>
typedef SRWLOCK            pdot_mutex;
typedef CONDITION_VARIABLE pdot_cond;
typedef HANDLE             pdot_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t pdot_mutex;
typedef pthread_cond_t  pdot_cond;
typedef pthread_t       pdot_thread;
#endif

int  pdot_mutex_init(pdot_mutex *m);
void pdot_mutex_destroy(pdot_mutex *m);
void pdot_mutex_lock(pdot_mutex *m);
void pdot_mutex_unlock(pdot_mutex *m);

int  pdot_cond_init(pdot_cond *c);
void pdot_cond_destroy(pdot_cond *c);
void pdot_cond_wait(pdot_cond *c, pdot_mutex *m);
void pdot_cond_broadcast(pdot_cond *c);

/* Starts fn(arg) on a new thread; 0 or PDOT_E_NOMEM. */
int  pdot_thread_start(pdot_thread *t, void (*fn)(void *), void *arg);
void pdot_thread_join(pdot_thread t);

#endif
