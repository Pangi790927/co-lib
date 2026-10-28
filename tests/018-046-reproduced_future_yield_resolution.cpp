#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test102 - Reproduced Bugs: create_future resolved a future at a co_yield
================================================================================================= */

/* Was an open bug in create_future()'s exit_func: the same lambda was installed as both a RETURN
and an EXIT modif on the wrapped task, and the RETURN slot also fires on a generator-style
co_yield (with state->err == ERROR_YIELDED, set by cpp_yield_awaiter before do_return_modifs) -
but exit_func never checked that err, unlike killer_state_t::on_return(). At the task's first
co_yield the promise already holds the yielded interim value (yield_value emplaces it), so
exit_func resolved the future with that non-final value, set data->ready and signaled: the waiter
woke with the interim value and the task's real co_return was discarded (data->ready was already
true when it arrived). The bare rescheduling co::yield() never fires RETURN modifs (yield_awaiter_t
runs only LEAVE and YIELD), so only generator-style co_yield tasks were hit. Now fixed: exit_func
returns early on ERROR_YIELDED - a yield is not an end - so the future resolves at the real
co_return, or at the task's death without a return (the EXIT slot, which never carries
ERROR_YIELDED), never at a yield. Fix #14 (tests/fixes/FIX_14-create_future_yield.md).

The co_yield'ing task in scenario 2 is driven by awaiting it again (the only way a co_yield task
resumes - a caller-less, sched'd one that co_yields is orphaned at its yield, its future would
honestly stay pending forever, and no test can wait that out). Scenario 3 pins the EXIT slot's
death coverage the fix had to keep: a future whose wrapped task is killed mid-wait still resolves.
The silent T{} it resolves to (the kill error swallowed) is a separate open bug, repro/24. */

/* ---- observation state: written by the coroutines, read by main after run() drained ---- */

static int  test102_a_future = -1;    /* 1: value the future resolved to (bare yield path)    */
static int  test102_b_yield  = -999;  /* 2: interim value the first await handed out          */
static int  test102_b_future = -1;    /* 2: value the future resolved to (generator path)     */
static bool test102_b_returned = false; /* 2: the wrapped task reached its real co_return      */

static int  test102_c_kill   = -999;  /* 3: kill_fn() return                                  */
static bool test102_c_dtor   = false; /* 3: the victim's frame destroyed by the kill           */
static bool test102_c_ran    = false; /* 3: the victim resumed past its wait: must not happen  */
static int  test102_c_future = -1;    /* 3: value the future resolved to (death path)         */

/* ---- Scenario 1: a bare co::yield() must not resolve the future (and never did) ---- */

static co::task<int> test102_a_task() {
    co_await co::yield();    /* bare rescheduling yield: LEAVE/YIELD modifs only, no RETURN */
    co_return 42;
}

static co::task_t test102_a_driver() {
    auto pool = co_await co::get_pool();
    auto t = test102_a_task();
    auto f = co::create_future(pool, t);
    co_await co::sched(t);
    test102_a_future = co_await f;
    co_return 0;
}

static int test102_bare_yield() {
    auto pool = co::create_pool();
    pool->sched(test102_a_driver());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test102_a_future == 42));   /* the real co_return, unchanged */
    return 0;
}

/* ---- Scenario 2: a generator co_yield must not resolve the future (the bug) ---- */

static co::task<int> test102_b_task() {
    co_yield 7;               /* fires the RETURN slot with ERROR_YIELDED */
    test102_b_returned = true;
    co_return 42;
}

static co::task_t test102_b_driver() {
    auto pool = co_await co::get_pool();
    auto t = test102_b_task();
    auto f = co::create_future(pool, t);
    test102_b_yield = co_await t;    /* drives it to its co_yield: hands the interim 7 out */
    co_await t;                     /* drives it to its real co_return 42 */
    test102_b_future = co_await f;   /* a yield is not an end: must be the 42 */
    co_return 0;
}

static int test102_generator_yield() {
    auto pool = co::create_pool();
    pool->sched(test102_b_driver());
    ASSERT_FN(pool->run());
    DBG("yield gave %d, future gave %d, real return reached: %d",
            test102_b_yield, test102_b_future, (int)test102_b_returned);
    ASSERT_FN(CHK_BOOL(test102_b_yield == 7));     /* the interim value still reaches its awaiter */
    ASSERT_FN(CHK_BOOL(test102_b_returned));       /* the wrapped task ran to its real return */
    ASSERT_FN(CHK_BOOL(test102_b_future == 42));   /* the future took the real return, not the 7 */
    return 0;
}

/* ---- Scenario 3: death without a return still resolves the future (coverage kept) ---- */

struct test102_c_marker_t { ~test102_c_marker_t() { test102_c_dtor = true; } };

static co::task<int> test102_c_victim(co::sem_p never) {
    test102_c_marker_t m;
    co_await never->wait();    /* never signaled: only the kill ends it */
    test102_c_ran = true;       /* killed means destroyed, not resumed */
    co_return 42;
}

static co::task_t test102_c_driver(co::sem_p never, const co::modif_pack_t& killer_mods,
        std::function<co::error_e(void)> kill_fn) {
    auto pool = co_await co::get_pool();
    auto t = co::add_modifs(pool, test102_c_victim(never), killer_mods);
    auto f = co::create_future(pool, t);
    co_await co::sched(t);
    co_await co::yield();          /* let the victim run and park on the sem */
    test102_c_kill = kill_fn();
    test102_c_future = co_await f;  /* the EXIT slot fires on its death */
    co_return 0;
}

static int test102_death_still_resolves() {
    auto pool = co::create_pool();
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test102_c_driver(never, killer_mods, kill_fn));
    ASSERT_FN(pool->run());
    DBG("kill %d, destroyed %d, ran past %d, future %d",
            test102_c_kill, (int)test102_c_dtor, (int)test102_c_ran, test102_c_future);
    ASSERT_FN(CHK_BOOL(test102_c_kill == co::ERROR_OK));   /* the kill verifiably happened */
    ASSERT_FN(CHK_BOOL(test102_c_dtor));
    ASSERT_FN(CHK_BOOL(!test102_c_ran));
    ASSERT_FN(CHK_BOOL(test102_c_future != -1));           /* death resolved the future */
    return 0;
}

int main() {
    int ret = test102_bare_yield();
    ret |= test102_generator_yield();
    ret |= test102_death_still_resolves();
    print_test_result("018-046-reproduced_future_yield_resolution.cpp", ret >= 0);
    return ret;
}
