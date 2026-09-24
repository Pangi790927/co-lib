#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test54 - Coroutine introspection: state_t::get_state()
================================================================================================= */

/* A coroutine's state follows it: READY while queued, RUNNING while it runs, WAITING_SEM on a
semaphore, LEFT while a callee runs - co::sleep_ms() is a callee too, so a sleeping coroutine is
LEFT, and the innermost frame (co::sleep's) is the one WAITING_IO. The watched coroutine can't end
before the observer's last look: its callee waits for the observer's signal. And a coroutine destroyed while
it is queued leaves the queue by itself: the pool never resumes the freed frame. */

static std::vector<std::string> test54_log;
static co::state_t *test54_watched = nullptr;

static const char *test54_name(co::state_e s) {
    switch (s) {
        case co::STATE_LEFT:        return "LEFT";
        case co::STATE_RUNNING:     return "RUNNING";
        case co::STATE_READY:       return "READY";
        case co::STATE_WAITING_SEM: return "WAITING_SEM";
        case co::STATE_WAITING_IO:  return "WAITING_IO";
    }
    return "?";
}

static void test54_look(const char *when) {
    test54_log.push_back(std::string(when) + ": " + test54_name(test54_watched->get_state()));
}

static co::task_t test54_callee(co::sem_p sem) {
    co_await sem->wait();               /* the caller is LEFT meanwhile, until the last look */
    co_return 0;
}

static co::task_t test54_watched_task(co::sem_p sem) {
    test54_watched = co_await co::get_state();
    test54_look("self");
    co_await sem->wait();
    co_await co::sleep_ms(1);
    co_await test54_callee(sem);
    co_return 0;
}

static co::task_t test54_observer(co::sem_p sem) {
    test54_look("after its start");     /* it waits on sem */
    sem->signal();
    test54_look("signaled");            /* queued */
    co_await co::yield();               /* it runs and sleeps */
    test54_look("sleeping");            /* LEFT: it calls co::sleep_ms() */
    co_await co::sleep_ms(5);           /* it calls the callee, which waits for us */
    test54_look("calling");             /* LEFT either way: in co::sleep_ms() or in the callee */
    sem->signal();                      /* only now can it end: its frame outlives our looks */
    co_return 0;
}

static bool test54_victim_ran = false;

static co::task_t test54_victim() {
    test54_victim_ran = true;
    co_return 0;
}

int test54_states() {
    {
        auto pool = co::create_pool();
        auto sem = co::create_sem(pool, 0);
        pool->sched(test54_watched_task(sem));
        pool->sched(test54_observer(sem));
        ASSERT_FN(pool->run());

        for (auto &l : test54_log)
            DBG("%s", l.c_str());
        ASSERT_FN(CHK_BOOL(test54_log == std::vector<std::string>({
                "self: RUNNING", "after its start: WAITING_SEM", "signaled: READY",
                "sleeping: LEFT", "calling: LEFT"})));
    }
    {
        /* destroyed while queued: it leaves the queue, the pool never resumes it */
        auto pool = co::create_pool();
        auto victim = test54_victim();
        pool->sched(victim);
        co::state_t *s = &victim.h.promise().state;
        ASSERT_FN(CHK_BOOL(s->get_state() == co::STATE_READY));
        co::destroy_state(s);
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(!test54_victim_ran));
    }
    return 0;
}

int main() {
    int ret = test54_states();
    print_test_result("012-002-introspection_state.cpp", ret >= 0);
    return ret;
}
