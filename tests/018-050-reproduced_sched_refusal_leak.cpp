#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <string>

/* Test106 - Reproduced Bugs: a SCHED callback's error dropped the task, leaking it
================================================================================================= */

/* pool_internal_t::sched() ran the SCHED callbacks of a task and, on any answer but ERROR_OK,
returned without queueing it: the task sat in no list of the pool, its EXIT callbacks never ran,
its frame leaked past the pool's death, and the code that scheduled it learned nothing (the sched
is void). The multithread take, _take_thread_tasks(), dropped a refused task the same way. The
doc asked the refusing modif to "take care of" the task, which no public API lets it do.

The fix, in the shape the user chose (fix #16, tests/fixes/FIX_16-sched_refusal_leak.md): the
SCHED slot is a notification slot, like ENTER, LEAVE and EXIT. What a SCHED callback returns is
ignored, the task is queued whatever the callbacks answer, and an error from one of them doesn't
stop the ones after it; there is no refusal and no park at a schedule, since nothing is suspended
there for a modif to hold. This file pins that: a task whose first SCHED callback answers
ERROR_GENERIC still gets its second SCHED callback, runs, exits and is destroyed, through
pool_t::sched(), through co_await co::sched() and through thread_sched(); and a SCHED callback
answering ERROR_SUSPENDED doesn't keep the task from running either. On the header before the fix
none of the four tasks ran. */

/* results hauled out of the coroutines: a failed ASSERT_COFN only logs (tests/CLAUDE.md) */
static std::string test106_order;           /* the callbacks, in the order they ran */
static int  test106_ran = 0;                /* tasks that ran their body */
static int  test106_dtor = 0;               /* frames destroyed (a local's destructor) */
static int  test106_exit = 0;               /* EXIT callbacks fired */

struct test106_marker_t { ~test106_marker_t() { test106_dtor++; } };

static co::task_t test106_task(char name) {
    test106_marker_t m;
    test106_ran++;
    test106_order += name;
    co_return 0;
}

/* a refusing SCHED callback, a second SCHED callback after it, and an EXIT callback */
static co::modif_pack_t test106_pack(co::error_e first_answer) {
    return co::modif_pack_t{
        co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [first_answer](co::state_t *) -> co::error_e {
                test106_order += 'r';   /* the one that answers an error (or a park) */
                return first_answer;
            }),
        co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test106_order += 's';   /* must still run after the error */
                return co::ERROR_OK;
            }),
        co::create_modif<co::CO_MODIF_EXIT_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test106_exit++;
                return co::ERROR_OK;
            }),
    };
}

/* schedules through the awaitable, the second carrier of a sched */
static co::task_t test106_driver() {
    co_await co::sched(test106_task('B'), test106_pack(co::ERROR_GENERIC));
    co_await co::sched(test106_task('C'), test106_pack(co::ERROR_SUSPENDED));
    co_return 0;
}

int test106_sched_refusal_leak() {
    {
        auto pool = co::create_pool();
        pool->sched(test106_task('A'), test106_pack(co::ERROR_GENERIC));   /* pool_t::sched */
        pool->sched(test106_driver());                                      /* co::sched, twice */
        pool->thread_sched(co::add_modifs(pool.get(), test106_task('D'),
                test106_pack(co::ERROR_GENERIC)));                          /* the thread take */
        ASSERT_FN(pool->run());
    }
    DBG("order [%s], ran %d, exits %d, destroyed %d", test106_order.c_str(), test106_ran,
            test106_exit, test106_dtor);

    /* every task ran, exited and was destroyed, whatever its SCHED callbacks answered */
    ASSERT_FN(CHK_BOOL(test106_ran == 4));
    ASSERT_FN(CHK_BOOL(test106_exit == 4));
    ASSERT_FN(CHK_BOOL(test106_dtor == 4));

    /* the second SCHED callback ran after the erroring one, every time: four "rs" pairs */
    size_t pairs = 0;
    for (size_t at = test106_order.find("rs"); at != std::string::npos;
            at = test106_order.find("rs", at + 2))
        pairs++;
    ASSERT_FN(CHK_BOOL(pairs == 4));
    return 0;
}

int main() {
    int ret = test106_sched_refusal_leak();
    print_test_result("018-050-reproduced_sched_refusal_leak.cpp", ret >= 0);
    return ret;
}
