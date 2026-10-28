#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

#include <memory>

/* Test88 - Reproduced Bugs: a semaphore waiter's own callbacks acting on its semaphore
================================================================================================= */

/* Were open bugs, found with FIX_02: sem_awaiter_t linked the waiter into the semaphore's wait list
before its WAIT_SEM callbacks ran, so the waiter counted as waiting while it was still running:
- a LEAVE callback that signaled the semaphore lost the wake-up: the count went to one before the
  waiter was linked, and the waiter then waited with a token sitting in the count;
- a WAIT_SEM callback that signaled and then refused the wait spent the token on the waiter (woken
  into the ready queue) while the refusal resumed it at once, so the token was gone and the waiter
  stayed queued as it ran on;
- a WAIT_SEM callback that cleared the semaphore destroyed the waiter inside its own callback (a
  use-after-free).
The waiter now joins the wait list only after its WAIT_SEM callbacks, as if those callbacks ran in
its body right before the wait: a signal from them leaves a token that it takes at once, a refusal
leaves that token in the semaphore, and a clear() doesn't see the waiter. Likewise signal_all()
from them wakes only the coroutines already waiting, so the waiter itself still waits. */

static const char *test88_file = "018-034-reproduced_sem_own_callbacks.cpp";

struct test88_marker_t {
    int *cnt;
    ~test88_marker_t() { (*cnt)++; }
};

/* ------------------------------------------------------ a LEAVE callback signals its semaphore */

static bool test88_a_done = false;

static co::task_t test88_a_waiter(co::sem_p sem) {
    co_await sem->wait();       /* its own LEAVE callback signals sem */
    test88_a_done = true;
    co_return 0;
}

static int test88_leave_signals() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto fired = std::make_shared<bool>(false);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(co::CO_MODIF_INHERIT_NONE,
            [s = sem.get(), fired](co::state_t *) -> co::error_e {
                if (!*fired) {          /* the LEAVE of its wait, not the one of its end */
                    *fired = true;
                    s->signal();
                }
                return co::ERROR_OK;
            }));
    pool->sched(co::add_modifs(pool.get(), test88_a_waiter(sem), mods));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_a_done));         /* it took the token: no lost wake-up */
    ASSERT_FN(CHK_BOOL(!sem->try_dec()));       /* and the token is spent */
    return 0;
}

/* --------------------------------------- a WAIT_SEM callback signals, then refuses the wait */

static int test88_b_runs = 0;

static co::task_t test88_b_waiter(co::sem_p sem) {
    co_await sem->wait();       /* refused by its own callback: it goes on at once */
    test88_b_runs++;
    co_return 0;
}

static int test88_signal_then_refuse() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *s) -> co::error_e {
                s->signal();
                return co::ERROR_USER;  /* refuses the wait */
            }));
    pool->sched(co::add_modifs(pool.get(), test88_b_waiter(sem), mods));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_b_runs == 1));
    ASSERT_FN(CHK_BOOL(sem->try_dec()));        /* the token stayed in the semaphore */
    return 0;
}

/* ------------------------------------------------- a WAIT_SEM callback clears its semaphore */

static int test88_c_destroyed = 0;
static bool test88_c_done = false;

static co::task_t test88_c_waiter(co::sem_p sem) {
    test88_marker_t marker{&test88_c_destroyed};
    co_await sem->wait();       /* its own WAIT_SEM callback clears sem */
    test88_c_done = true;
    co_return 0;
}

static co::task_t test88_c_signaler(co::sem_p sem) {
    co_await co::yield();       /* the waiter waits on sem */
    sem->signal();
    co_return 0;
}

static int test88_clear_from_wait() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *s) -> co::error_e {
                s->clear(0);
                return co::ERROR_OK;
            }));
    pool->sched(co::add_modifs(pool.get(), test88_c_waiter(sem), mods));
    pool->sched(test88_c_signaler(sem));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_c_done));         /* the clear didn't see it: it waited, then woke */
    ASSERT_FN(CHK_BOOL(test88_c_destroyed == 1));
    return 0;
}

/* -------------------------------------- a WAIT_SEM callback calls signal_all() on its semaphore */

static bool test88_d_done = false;
static bool test88_d_waited = false;

static co::task_t test88_d_waiter(co::sem_p sem) {
    co_await sem->wait();       /* its own WAIT_SEM callback calls signal_all() */
    test88_d_done = true;
    co_return 0;
}

static co::task_t test88_d_signaler(co::sem_p sem) {
    co_await co::yield();       /* a waiter woken by that signal_all() would run here */
    test88_d_waited = !test88_d_done;
    sem->signal();
    co_return 0;
}

static int test88_signal_all_from_wait() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *s) -> co::error_e {
                s->signal_all();
                return co::ERROR_OK;
            }));
    pool->sched(co::add_modifs(pool.get(), test88_d_waiter(sem), mods));
    pool->sched(test88_d_signaler(sem));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_d_waited));       /* signal_all() didn't count it: it still waited */
    ASSERT_FN(CHK_BOOL(test88_d_done));
    return 0;
}

int main() {
    int ret = test88_leave_signals();
    if (ret == 0)
        ret = test88_signal_then_refuse();
    if (ret == 0)
        ret = test88_clear_from_wait();
    if (ret == 0)
        ret = test88_signal_all_from_wait();
    print_test_result(test88_file, ret >= 0);
    return ret;
}
