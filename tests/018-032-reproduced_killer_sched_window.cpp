#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>
#include <memory>

/* Test86 - Reproduced Bugs: a kill from the task's own SCHED callback freed the task being queued
================================================================================================= */

/* Was an open bug (repro/42): pool_internal_t::sched() runs the task's SCHED callbacks, then queues
the task. The killer's own SCHED callback binds it first, so a kill from a later SCHED callback of
the task found it out of every colib queue (STATE_LEFT) and destroyed it, and sched() then queued
the freed task (ASan heap-use-after-free). A kill now throws for a target colib doesn't own, and a
modif callback can't throw, so the program ends inside that kill, before the task is freed. The
task's frame is watched through a shared_ptr parameter: the frame's copy is the last owner, so the
weak_ptr expires exactly when the frame is destroyed (the task never runs). */

static const char *test86_file = "018-032-reproduced_killer_sched_window.cpp";
static bool test86_in_kill = false;
static std::weak_ptr<int> test86_frame;

static co::task_t test86_task(std::shared_ptr<int> keep) {
    (void)keep;
    co_return 0;
}

int test86_sched_window() {
    std::set_terminate([] {
        bool ok = test86_in_kill && !test86_frame.expired();    /* inside the kill, not freed */
        print_test_result(test86_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto keep = std::make_shared<int>(0);
    test86_frame = keep;
    auto t = co::add_modifs(pool.get(), test86_task(keep), killer_mods);
    keep.reset();                   /* the frame's copy is the last owner now */

    /* added after the killer's pack, so the killer's own SCHED callback has bound the task */
    auto sched_kill = co::modif_pack_t{};
    sched_kill.push_back(co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [kill_fn](co::state_t *) -> co::error_e {
                test86_in_kill = true;
                kill_fn();          /* the task is being scheduled: the program must end here */
                test86_in_kill = false;
                return co::ERROR_OK;
            }));
    co::add_modifs(pool.get(), t, sched_kill);

    pool->sched(t);
    DBG("no terminate: the kill from the task's own SCHED callback must end the program");
    return -1;
}

int main() {
    int ret = test86_sched_window();
    print_test_result(test86_file, ret >= 0);
    return ret;
}
