#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test61 - Reproduced bug: destroying a generator that co_yielded also destroys its old caller
================================================================================================= */

/* A generator that co_yielded belongs to whoever holds it: to stop it early, the holder destroys it.
destroy_state() is the way to destroy a coroutine with its EXIT, but it follows caller_state
upwards, and a yielded generator still points at the caller it yielded to. So destroying it walks
into the holder itself, here while the holder is running. Only the generator must die: its EXIT,
its frame freed, and the holder goes on. */

static int test61_destructed = 0;
static int test61_exited = 0;
static bool test61_holder_after = false;

/* a parameter: its copy in the frame lives until the frame is freed (a local dies at co_return) */
struct test61_marker_t {
    bool live = false;
    test61_marker_t() {}
    test61_marker_t(test61_marker_t &&oth) : live(true) { oth.live = false; }
    ~test61_marker_t() { if (live) test61_destructed++; }
};

static co::task_t test61_generator(test61_marker_t) {
    co_yield 1;
    co_return 2;
}

static co::task_t test61_holder() {
    auto exit_mod = co::create_modif<co::CO_MODIF_EXIT_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e { test61_exited++; return co::ERROR_OK; });
    auto gen = co::add_modifs(co_await co::get_pool(), test61_generator(test61_marker_t{}),
            co::modif_pack_t(1, exit_mod));
    co_await gen;               /* it yields, we don't want the rest */
    co::destroy_state(gen.get_state());
    test61_holder_after = true;
    co_return 0;
}

int test61_destroy_yielded() {
    auto pool = co::create_pool();
    pool->sched(test61_holder());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test61_destructed == 1));
    ASSERT_FN(CHK_BOOL(test61_exited == 1));
    ASSERT_FN(CHK_BOOL(test61_holder_after));
    return 0;
}

int main() {
    int ret = test61_destroy_yielded();
    print_test_result("018-020-reproduced_destroy_yielded_generator.cpp", ret >= 0);
    return ret;
}
