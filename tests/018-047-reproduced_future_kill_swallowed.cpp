#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test103 - Reproduced Bugs: a killed create_future task reported T{} and ERROR_OK, as a success
================================================================================================= */

/* Was an open bug in create_future's exit_func: when a killer destroyed the wrapped task before it
returned, the EXIT callback found no exception and no value (promise().ret still monostate) and took
the default-construct branch: it stored T{}, marked the data ready and signaled. The waiter then
returned that T{} with every error channel clean: the awaited value was the default, the waiter
task's get_err() was ERROR_OK, no exception was thrown. wait_all, built on create_future, showed the
same face: a member killed mid-wait came back as T{} inside the result tuple and the whole co_await
completed as if nothing had died. The kill's error could not reach the waiter by any path: the
killer's end_chain wrote the error into the root's state only when a caller existed, and a scheduled
task has none. Now fixed (fix #24, in the shape the user chose): end_chain always writes the kill's
error into the killed root's state, and a future resolves to a pair, the value and an error: the
return value and ERROR_OK, or T{} and the kill's error, or T{} and ERROR_GENERIC for a death that
was no kill. wait_all resolves to a tuple of those pairs, one per member: a killed member reports
its kill and the others their values.

This file reads the pair, so it compiles only with the fix in place; against the unfixed colib.h a
program that reads the plain value shows the kill vanishing (the fix's record has that run). */

static int  test103_s1_kill_ret  = -999;  /* kill_fn() first call: ERROR_OK, it killed            */
static int  test103_s1_kill2_ret = -999;  /* second call: ERROR_FINISHED, nothing left            */
static int  test103_s1_dtor      = 0;     /* victim frame destroyed: exactly once                 */
static bool test103_s1_ran_past  = false; /* resumed past its wait: must stay false               */
static int  test103_s1_value     = -1;    /* the pair's value: T{}, the task never returned       */
static int  test103_s1_err       = -999;  /* the pair's error: the kill's                         */
static bool test103_s1_exc       = false; /* an exception at the await: must not happen           */
static int  test103_s1_fut_err   = -999;  /* waiter get_err(): stays ERROR_OK, the pair carries it */

static int  test103_s2_kill_ret  = -999;
static int  test103_s2_dtor      = 0;
static bool test103_s2_ran_past  = false;
static bool test103_s2_tuple     = false; /* wait_all delivered its tuple: it must                */
static int  test103_s2_a_value   = -1;    /* the healthy member's pair                            */
static int  test103_s2_a_err     = -999;
static int  test103_s2_b_value   = -1;    /* the killed member's pair                             */
static int  test103_s2_b_err     = -999;
static bool test103_s2_exc       = false;

static int  test103_s3_value     = -999;  /* control: a healthy future still returns its value    */
static int  test103_s3_err       = -999;
static bool test103_s3_tuple_ok  = false; /* control: a healthy wait_all still returns its tuple  */

struct test103_s1_marker_t { ~test103_s1_marker_t() { test103_s1_dtor++; } };
struct test103_s2_marker_t { ~test103_s2_marker_t() { test103_s2_dtor++; } };

/* Scenario 1: the documented create_future usage, the wrapped task killed while parked on a
semaphore that never signals. Every channel of the waiter is recorded. */
static co::task<int> test103_s1_victim(co::sem_p never) {
    test103_s1_marker_t m;
    co_await never->wait();
    test103_s1_ran_past = true;     /* killed means destroyed, never resumed */
    co_return 42;
}

static co::task_t test103_s1_driver(co::sem_p never) {
    auto pool = co_await co::get_pool();
    auto [killer_mods, kill_fn] = co::create_killer(pool, co::ERROR_USER);

    auto t = co::add_modifs(pool, test103_s1_victim(never), killer_mods);
    auto fut = co::create_future(pool, t);
    co_await co::sched(t);
    co_await co::yield();          /* let the victim run first and park on the sem */

    test103_s1_kill_ret = kill_fn();
    test103_s1_kill2_ret = kill_fn();

    try {
        auto [value, err] = co_await fut;
        test103_s1_value = value;
        test103_s1_err = err;
    } catch (const std::exception&) {
        test103_s1_exc = true;
    }

    test103_s1_fut_err = fut.get_err();
    co_return 0;
}

/* Scenario 2: wait_all with one member killed mid-wait by a controller (the tests/002-002 shape).
The tuple must arrive, the killed member reporting its kill and the healthy one its value. */
static co::task<int> test103_s2_member_a() {
    co_return 7;
}

static co::task<int> test103_s2_member_b(co::sem_p never) {
    test103_s2_marker_t m;
    co_await never->wait();
    test103_s2_ran_past = true;     /* must not run */
    co_return 9;
}

static co::task_t test103_s2_controller(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();          /* let wait_all sched both members and let b park */
    test103_s2_kill_ret = kill_fn();
    co_return 0;
}

static co::task_t test103_s2_driver(
        co::pool_t *pool, co::sem_p never, const co::modif_pack_t& killer_mods) {
    auto b = co::add_modifs(pool, test103_s2_member_b(never), killer_mods);
    try {
        auto tup = co_await co::wait_all(test103_s2_member_a(), b);
        test103_s2_tuple = true;
        test103_s2_a_value = std::get<0>(tup).first;
        test103_s2_a_err = std::get<0>(tup).second;
        test103_s2_b_value = std::get<1>(tup).first;
        test103_s2_b_err = std::get<1>(tup).second;
    } catch (const std::exception&) {
        test103_s2_exc = true;
    }
    co_return 0;
}

/* Scenario 3, control: nothing killed. A future still hands back the real co_return value with
ERROR_OK and a healthy wait_all still delivers its tuple: the kill path added no false positives. */
static co::task<int> test103_s3_worker() {
    co_return 42;
}

static co::task_t test103_s3_driver() {
    auto pool = co_await co::get_pool();
    auto t = test103_s3_worker();
    auto fut = co::create_future(pool, t);
    co_await co::sched(t);
    auto [value, err] = co_await fut;
    test103_s3_value = value;
    test103_s3_err = err;

    auto tup = co_await co::wait_all(
            []() -> co::task<int> { co_return 7; }(),
            []() -> co::task<int> { co_return 9; }());
    test103_s3_tuple_ok = std::get<0>(tup) == std::pair<int, co::error_e>{7, co::ERROR_OK}
            && std::get<1>(tup) == std::pair<int, co::error_e>{9, co::ERROR_OK};
    co_return 0;
}

int test103_future_kill_surface() {
    {
        auto pool = co::create_pool();
        auto never = co::create_sem(pool, 0);
        pool->sched(test103_s1_driver(never));
        ASSERT_FN(pool->run());
    }
    {
        auto pool = co::create_pool();
        auto never = co::create_sem(pool, 0);
        auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(test103_s2_driver(pool.get(), never, killer_mods));
        pool->sched(test103_s2_controller(kill_fn));
        ASSERT_FN(pool->run());
    }
    {
        auto pool = co::create_pool();
        pool->sched(test103_s3_driver());
        ASSERT_FN(pool->run());
    }

    DBG("s1: value %d err %d exc %d waiter err %d; s2: tuple %d a {%d, %d} b {%d, %d} exc %d; "
            "s3: {%d, %d} tuple ok %d", test103_s1_value, test103_s1_err, (int)test103_s1_exc,
            test103_s1_fut_err, (int)test103_s2_tuple, test103_s2_a_value, test103_s2_a_err,
            test103_s2_b_value, test103_s2_b_err, (int)test103_s2_exc, test103_s3_value,
            test103_s3_err, (int)test103_s3_tuple_ok);

    /* the kills verifiably happened, the old repro's own guards */
    ASSERT_FN(CHK_BOOL(test103_s1_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s1_kill2_ret == co::ERROR_FINISHED));
    ASSERT_FN(CHK_BOOL(test103_s1_dtor == 1 && !test103_s1_ran_past));
    ASSERT_FN(CHK_BOOL(test103_s2_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s2_dtor == 1 && !test103_s2_ran_past));

    /* scenario 1: the kill is in the pair, nowhere else */
    ASSERT_FN(CHK_BOOL(test103_s1_value == 0));
    ASSERT_FN(CHK_BOOL(test103_s1_err == co::ERROR_USER));
    ASSERT_FN(CHK_BOOL(!test103_s1_exc));
    ASSERT_FN(CHK_BOOL(test103_s1_fut_err == co::ERROR_OK));

    /* scenario 2: the tuple arrives, each member reporting for itself */
    ASSERT_FN(CHK_BOOL(test103_s2_tuple && !test103_s2_exc));
    ASSERT_FN(CHK_BOOL(test103_s2_a_value == 7 && test103_s2_a_err == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s2_b_value == 0 && test103_s2_b_err == co::ERROR_USER));

    /* scenario 3: the untouched contract is intact */
    ASSERT_FN(CHK_BOOL(test103_s3_value == 42 && test103_s3_err == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s3_tuple_ok));

    return 0;
}

int main() {
    int ret = test103_future_kill_surface();
    print_test_result("018-047-reproduced_future_kill_swallowed.cpp", ret >= 0);
    return ret;
}
