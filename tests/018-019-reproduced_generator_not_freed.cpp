#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test58 - Reproduced bug: a generator that co_yielded is never freed once it co_returns
================================================================================================= */

/* co_yield sets the generator's err to ERROR_YIELDED, and task::await_resume() keeps the frame alive
while err says so: its caller will await it again. Nothing reset err when it was awaited again, so
once it co_returned, await_resume() still saw ERROR_YIELDED and never destroyed the frame. */

static int test58_destructed = 0;

/* a parameter: its copy in the frame lives until the frame is freed (a local dies at co_return) */
struct test58_marker_t {
    bool live = false;
    test58_marker_t() {}
    test58_marker_t(test58_marker_t &&oth) : live(true) { oth.live = false; }
    ~test58_marker_t() { if (live) test58_destructed++; }
};

static co::task_t test58_generator(test58_marker_t) {
    co_yield 1;
    co_return 2;
}

static co::task_t test58_caller() {
    auto gen = test58_generator(test58_marker_t{});
    int first = co_await gen;
    ASSERT_COFN(CHK_BOOL(first == 1));
    ASSERT_COFN(CHK_BOOL(test58_destructed == 0));  /* yielded: alive */
    int second = co_await gen;
    ASSERT_COFN(CHK_BOOL(second == 2));
    ASSERT_COFN(CHK_BOOL(test58_destructed == 1));  /* returned: freed */
    co_return 0;
}

int test58_generator_freed() {
    auto pool = co::create_pool();
    pool->sched(test58_caller());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test58_destructed == 1));
    return 0;
}

int main() {
    int ret = test58_generator_freed();
    print_test_result("018-019-reproduced_generator_not_freed.cpp", ret >= 0);
    return ret;
}
