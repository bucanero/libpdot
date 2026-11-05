/*
 * pdot_async without a network: ordering, callbacks only inside
 * pdot_async_poll, cancellation of queued and running jobs, callbacks owed
 * at pdot_async_free, and a watch that reports one failure per streak.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <time.h>

#include "pdot.h"
#include "client.h"
#include "thread.h"
#include "test.h"

/* Nothing listens here: connections are refused at once. */
#define DEAD_NODE "http://127.0.0.1:1"

typedef struct {
    int  called;
    int  rc;
    int  seq;
    int  has_result;
    char err[64];
} slot;

static int seq;

/* Flags set on the worker thread and read on this one. */
static pdot_mutex flag_lock;
static int wakes, blocker_started, blocker_ran_b;

static void set_flag(int *f)
{
    pdot_mutex_lock(&flag_lock);
    (*f)++;
    pdot_mutex_unlock(&flag_lock);
}

static int get_flag(int *f)
{
    int v;

    pdot_mutex_lock(&flag_lock);
    v = *f;
    pdot_mutex_unlock(&flag_lock);
    return v;
}

static void on_done(void *ud, int rc, const char *error, const void *result)
{
    slot *s = ud;

    s->called++;
    s->rc = rc;
    s->seq = ++seq;
    s->has_result = result != NULL;
    snprintf(s->err, sizeof s->err, "%s", error);
}

static void on_wake(void *ud)
{
    (void)ud;
    set_flag(&wakes);
}

static int work_ok(pdot_client *c, void *ud)
{
    (void)c;
    (void)ud;
    return 0;
}

static int work_fail(pdot_client *c, void *ud)
{
    (void)ud;
    return pdot_fail(c, PDOT_E_RPC, "custom failure");
}

static int work_block(pdot_client *c, void *ud)
{
    int i;

    (void)ud;
    set_flag(&blocker_started);
    for (i = 0; i < 500; i++)  /* 10 s at most */
        if (pdot_client_wait(c, 20))
            return PDOT_E_CANCELLED;
    return 0;
}

static int work_mark(pdot_client *c, void *ud)
{
    (void)c;
    (void)ud;
    set_flag(&blocker_ran_b);
    return 0;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Polls until cond holds or ms pass. */
#define POLL_UNTIL(a, cond, ms)                                   \
    do {                                                          \
        int left_ = (ms) / 10;                                    \
        while (!(cond) && left_-- > 0) {                          \
            pdot_async_poll(a);                                   \
            sleep_ms(10);                                         \
        }                                                         \
    } while (0)

static pdot_async *new_async(void)
{
    pdot_client *c = pdot_client_new(DEAD_NODE, NULL);

    pdot_client_set_timeout(c, 2000);
    return pdot_async_new(c);
}

static void test_order(void)
{
    pdot_async *a = new_async();
    slot s[3];
    int i;

    memset(s, 0, sizeof s);
    seq = 0;
    wakes = 0;  /* no thread is running yet */
    pdot_async_set_wake(a, on_wake, NULL);
    CHECK(pdot_async_run(a, work_ok, on_done, &s[0]) != 0);
    CHECK(pdot_async_run(a, work_fail, on_done, &s[1]) != 0);
    CHECK(pdot_async_run(a, work_ok, on_done, &s[2]) != 0);
    sleep_ms(100);
    for (i = 0; i < 3; i++)
        CHECK(s[i].called == 0);  /* callbacks wait for pdot_async_poll */
    POLL_UNTIL(a, s[2].called, 5000);
    for (i = 0; i < 3; i++)
        CHECK(s[i].called == 1 && s[i].seq == i + 1);
    CHECK(s[0].rc == 0 && s[0].err[0] == '\0' && s[0].has_result == 1);
    CHECK(s[1].rc == PDOT_E_RPC && strcmp(s[1].err, "custom failure") == 0 && !s[1].has_result);
    CHECK(get_flag(&wakes) > 0);
    CHECK(pdot_async_poll(a) == 0);
    pdot_async_free(a);
}

static void test_cancel(void)
{
    pdot_async *a = new_async();
    slot sa, sb;
    uint64_t ida, idb;

    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    blocker_started = blocker_ran_b = 0;
    ida = pdot_async_run(a, work_block, on_done, &sa);
    idb = pdot_async_run(a, work_mark, on_done, &sb);
    POLL_UNTIL(a, get_flag(&blocker_started), 5000);
    CHECK(get_flag(&blocker_started));
    CHECK(pdot_async_cancel(a, idb) == 0);   /* queued */
    CHECK(pdot_async_cancel(a, ida) == 0);   /* running */
    CHECK(pdot_async_cancel(a, 999) == PDOT_E_NOTFOUND);
    POLL_UNTIL(a, sa.called && sb.called, 5000);
    CHECK(sa.called == 1 && sa.rc == PDOT_E_CANCELLED);
    CHECK(sb.called == 1 && sb.rc == PDOT_E_CANCELLED);
    CHECK(!get_flag(&blocker_ran_b));
    pdot_async_free(a);
    CHECK(sa.called == 1 && sb.called == 1);
}

static void test_free_pending(void)
{
    pdot_async *a = new_async();
    slot sa, sb;

    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    blocker_started = blocker_ran_b = 0;
    CHECK(pdot_async_run(a, work_block, on_done, &sa) != 0);
    CHECK(pdot_async_run(a, work_mark, on_done, &sb) != 0);
    pdot_async_free(a);  /* runs both owed callbacks */
    CHECK(sa.called == 1 && sa.rc == PDOT_E_CANCELLED);
    CHECK(sb.called == 1 && sb.rc == PDOT_E_CANCELLED);
    CHECK(!get_flag(&blocker_ran_b));
}

static void test_watch_failure(void)
{
    pdot_async *a = new_async();
    slot s;
    uint64_t id;

    memset(&s, 0, sizeof s);
    id = pdot_async_watch_head(a, on_done, &s);
    CHECK(id != 0);
    POLL_UNTIL(a, s.called, 5000);
    CHECK(s.called == 1 && s.rc == PDOT_E_NETWORK && !s.has_result);
    POLL_UNTIL(a, s.called > 1, 3000);  /* retried, but not reported again */
    CHECK(s.called == 1);
    CHECK(pdot_async_cancel(a, id) == 0);
    POLL_UNTIL(a, s.called > 1, 3000);
    CHECK(s.called == 2 && s.rc == PDOT_E_CANCELLED);
    CHECK(pdot_async_cancel(a, id) == PDOT_E_NOTFOUND);
    pdot_async_free(a);
    CHECK(s.called == 2);
}

static void test_watch_at_free(void)
{
    pdot_async *a = new_async();
    slot s;

    memset(&s, 0, sizeof s);
    CHECK(pdot_async_watch_balance(a, (const uint8_t *)"0123456789abcdef0123456789abcdef",
                                   on_done, &s) != 0);
    pdot_async_free(a);
    CHECK(s.called >= 1 && s.rc == PDOT_E_CANCELLED);
}

int main(void)
{
    pdot_mutex_init(&flag_lock);
    test_order();
    test_cancel();
    test_free_pending();
    test_watch_failure();
    test_watch_at_free();
    TEST_DONE();
}
