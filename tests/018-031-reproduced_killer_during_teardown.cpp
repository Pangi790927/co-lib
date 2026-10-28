#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test85 - Reproduced Bugs: a kill from a teardown's callbacks freed frames the teardown frees
================================================================================================= */

/* Was an open bug (repro/41): a teardown (sem_t::clear(), pool_t::clear(), destroy_state(), or
another kill) runs the EXIT callbacks of the frames it destroys. A kill called from one of them
found its target among those frames, out of every colib queue (STATE_LEFT), and destroyed them
itself; the teardown then walked into the freed frames (ASan heap-use-after-free). A kill now
throws for a target colib doesn't own, and a modif callback can't throw, so the program ends inside
that kill, before the teardown destroyed anything. Here an inner frame's EXIT callback calls its own
chain's kill while a sem_t::clear() tears the chain down. */

static const char *test85_file = "018-031-reproduced_killer_during_teardown.cpp";
static int test85_destroyed = 0;
static bool test85_in_kill = false;
static std::function<co::error_e(void)> test85_kill;

struct test85_marker_t {
    ~test85_marker_t() { test85_destroyed++; }
};

static co::task_t test85_inner(co::sem_p block) {
    test85_marker_t marker;
    co_await block->wait();         /* never signaled: the clear tears the chain down here */
    co_return 0;
}

static co::task_t test85_outer(co::sem_p block, co::modif_pack_t inner_mods) {
    test85_marker_t marker;
    auto inner = co::add_modifs(co_await co::get_pool(), test85_inner(block), inner_mods);
    co_await inner;
    co_return 0;
}

int test85_kill_during_teardown() {
    std::set_terminate([] {
        bool ok = test85_in_kill && test85_destroyed == 0;  /* inside the kill, nothing freed */
        print_test_result(test85_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    test85_kill = kill_fn;

    auto exit_mods = co::modif_pack_t{};
    exit_mods.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test85_in_kill = true;
                test85_kill();      /* its chain is being torn down: the program must end here */
                test85_in_kill = false;
                return co::ERROR_OK;
            }));

    pool->sched(co::add_modifs(pool.get(), test85_outer(block, exit_mods), killer_mods));
    ASSERT_FN(pool->run());         /* outer calls inner, inner waits on block */
    block->clear(0);                /* tears inner and outer down: inner's EXIT calls the kill */
    DBG("no terminate: the kill from the teardown's EXIT callback must end the program");
    return -1;
}

int main() {
    int ret = test85_kill_during_teardown();
    print_test_result(test85_file, ret >= 0);
    return ret;
}
