#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test84 - Reproduced Bugs: a kill from the target's own wait callbacks freed the running frame
================================================================================================= */

/* Was an open bug: killer_state_t::kill() threw only for STATE_RUNNING, and destroyed the target
in any other state its switch did not name. While its own suspension callbacks run, the target is
not STATE_RUNNING: do_leave_modifs() sets STATE_LEFT before the LEAVE callbacks, and the semaphore
linked the waiter (STATE_WAITING_SEM) before the WAIT_SEM ones. A kill_fn() called from those
callbacks destroyed the frame whose await_suspend was still running (ASan heap-use-after-free, see
repro/02-killer_window_uaf.md). The same happened to a target that the kill itself was running up
to its next wait, and to a target parked by a modif, freed under the modif that owns it.

The kill now ends only a target that colib owns: one that sits in a colib queue. For a target out
of its body (STATE_LEFT) it throws kill_not_colib_owned_t. A modif callback can't throw, so a kill
from the target's own callbacks ends the program, before anything is freed; from elsewhere the
throw reaches the caller of the kill. Called from the target's callbacks while that kill runs the
target, the kill returns ERROR_GENERIC (a callback of its own kill), and the target dies at its
next wait.

The runs, in order: a kill from the UNWAIT callbacks while the kill runs the target; a kill of a
parked target; then a kill from the victim's WAIT_SEM callbacks, which must end the program inside
that kill, with the victim not destroyed. The recorded callbacks of the first two show every LEAVE
closed by an ENTER and every WAIT by an UNWAIT, and the debug checks agree. */

static const char *test84_file = "018-030-reproduced_killer_self_window.cpp";

static int test84_destruct_cnt = 0;
static bool test84_resumed = false;
static std::string test84_order;

struct test84_marker_t {
    ~test84_marker_t() { test84_destruct_cnt++; }
};

/* records the victim's callbacks: E/L enter and leave, W/U a semaphore wait and its end, R return,
X exit */
static co::modif_pack_t test84_recorder() {
    auto f = co::CO_MODIF_INHERIT_NONE;
    auto rec = [](const char *c) {
        return [c](auto...) { test84_order += c; return co::ERROR_OK; };
    };
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(f, rec("E")));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(f, rec("L")));
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(f, rec("W")));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(f, rec("U")));
    pack.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(f, rec("R")));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(f, rec("X")));
    return pack;
}

static void test84_reset() {
    test84_destruct_cnt = 0;
    test84_resumed = false;
    test84_order.clear();
}

static co::task_t test84_victim(co::sem_p block) {
    test84_marker_t marker;
    co_await block->wait();     /* never signaled */
    test84_resumed = true;      /* must never run */
    co_return co::ERROR_OK;
}

/* --------------------------------------------- the kill from its callbacks while it drives it */

static bool test84_got_token = false;
static std::vector<co::error_e> test84_inner;
static co::error_e test84_outer = co::ERROR_GENERIC;

static co::task_t test84_driven(co::sem_p sem, co::sem_p never) {
    test84_marker_t marker;
    co_await sem->wait();           /* signaled, then killed: the kill runs it from here */
    test84_got_token = true;        /* what its wait received is not lost */
    co_await never->wait();         /* it dies here */
    test84_resumed = true;          /* must never run */
    co_return co::ERROR_OK;
}

static co::task_t test84_driver(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();           /* the victim waits on sem */
    sem->signal();
    test84_outer = kill_fn();
    co_return 0;
}

static int test84_drive_kill() {
    test84_reset();
    test84_got_token = false;
    test84_inner.clear();
    test84_outer = co::ERROR_GENERIC;

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* the victim's UNWAIT callbacks call the kill that runs it */
    auto again_mods = co::modif_pack_t{};
    again_mods.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [kill_fn](co::state_t *, co::sem_t *) -> co::error_e {
                test84_inner.push_back(kill_fn());
                return co::ERROR_OK;
            }));

    auto victim = co::add_modifs(pool.get(), test84_driven(sem, never), killer_mods);
    co::add_modifs(pool.get(), victim, again_mods);
    co::add_modifs(pool.get(), victim, test84_recorder());

    pool->sched(COLIB_REGNAME(victim));
    pool->sched(test84_driver(sem, kill_fn));
    ASSERT_FN(pool->run());

    DBG("drive order: %s", test84_order.c_str());
    ASSERT_FN(CHK_BOOL(test84_outer == co::ERROR_OK));     /* the kill ended it */
    ASSERT_FN(CHK_BOOL(test84_inner.size() == 2));         /* its resume, and its park */
    for (auto err : test84_inner)
        ASSERT_FN(CHK_BOOL(err == co::ERROR_GENERIC));     /* a callback of its own kill */
    ASSERT_FN(CHK_BOOL(test84_got_token));
    ASSERT_FN(CHK_BOOL(!test84_resumed));
    ASSERT_FN(CHK_BOOL(test84_destruct_cnt == 1));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    ASSERT_FN(CHK_BOOL(test84_order == "ELWUELWUX"));
    return 0;
}

/* ------------------------------------------------------------ the kill of a parked target */

static co::state_t *test84_parked = nullptr;
static bool test84_kill_threw = false;

static co::task_t test84_owner(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();           /* the victim parks */
    try {
        kill_fn();
    }
    catch (co::kill_not_colib_owned_t &) {
        test84_kill_threw = true;   /* a modif holds it: the kill can't end it */
    }
    co::destroy_state(test84_parked);   /* its owner ends it, once */
    co_return 0;
}

static int test84_held_kill() {
    test84_reset();
    test84_parked = nullptr;
    test84_kill_threw = false;

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto park_mods = co::modif_pack_t{};
    park_mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *s, co::sem_t *) -> co::error_e {
                test84_parked = s;
                return co::ERROR_SUSPENDED;     /* parked: this modif owns it now */
            }));

    auto victim = co::add_modifs(pool.get(), test84_victim(block), killer_mods);
    co::add_modifs(pool.get(), victim, park_mods);
    co::add_modifs(pool.get(), victim, test84_recorder());

    pool->sched(COLIB_REGNAME(victim));
    pool->sched(test84_owner(kill_fn));
    ASSERT_FN(pool->run());

    DBG("held order: %s", test84_order.c_str());
    ASSERT_FN(CHK_BOOL(test84_kill_threw));
    ASSERT_FN(CHK_BOOL(!test84_resumed));
    ASSERT_FN(CHK_BOOL(test84_destruct_cnt == 1));         /* destroyed by its owner only */
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    ASSERT_FN(CHK_BOOL(test84_order == "ELWUX"));
    return 0;
}

/* --------------------------------------- the kill from its own WAIT_SEM callbacks: it must end
the program, inside that kill, before anything was freed (the last run: it doesn't come back) */

static bool test84_in_kill = false;

static int test84_self_kill() {
    test84_reset();
    std::set_terminate([] {
        bool ok = test84_in_kill && test84_destruct_cnt == 0 && !test84_resumed;
        print_test_result(test84_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto self_kill_mods = co::modif_pack_t{};
    self_kill_mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [kill_fn](co::state_t *, co::sem_t *) -> co::error_e {
                test84_in_kill = true;
                kill_fn();          /* the victim is executing: the program must end here */
                test84_in_kill = false;
                return co::ERROR_OK;
            }));

    auto victim = co::add_modifs(pool.get(), test84_victim(block), killer_mods);
    co::add_modifs(pool.get(), victim, self_kill_mods);

    pool->sched(COLIB_REGNAME(victim));
    pool->run();
    DBG("no terminate: the kill from the WAIT_SEM callback must end the program");
    return -1;
}

int main() {
    int ret = test84_drive_kill();
    if (ret == 0)
        ret = test84_held_kill();
    if (ret == 0)
        ret = test84_self_kill();     /* ends the program when it passes */
    print_test_result(test84_file, ret >= 0);
    return ret;
}
