#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test52 - Flow Control: a killed child's caller resumes in the child's turn
================================================================================================= */

/* The child (the killer's target, called by the parent) is in the ready queue when the kill comes,
so it is resumed inside the kill. Whether it then finishes or dies at its next wait, the parent
must not run from inside the kill: it resumes in the child's place in the ready queue, before a
witness queued behind the child. The third case removes a task queued *ahead* of the child while
the child is resumed, the parent still keeps the child's place (a saved index would put it after
the witness). */

enum test52_mode_e { TEST52_FINISHES, TEST52_DIES, TEST52_REMOVES_AHEAD };

static std::vector<std::string> test52_log;
static std::function<co::error_e(void)> test52_kill_ahead;

static co::task<int> test52_child(co::sem_p sem, co::sem_p never, test52_mode_e mode) {
    co_await sem->wait();
    if (mode == TEST52_REMOVES_AHEAD)
        test52_kill_ahead();        /* the task queued ahead of us leaves the ready queue */
    if (mode == TEST52_DIES)
        co_await never->wait();     /* dies here */
    co_return 7;
}

static co::task_t test52_parent(co::sem_p sem, co::sem_p never, test52_mode_e mode,
        co::modif_pack_t mods)
{
    auto pool = co_await co::get_pool();
    auto child = test52_child(sem, never, mode);
    co::add_modifs(pool, child, mods);
    int ret = co_await child;
    test52_log.push_back("parent " + std::to_string(ret));
    co_return 0;
}

static co::task_t test52_named(const char *name) {
    test52_log.push_back(name);
    co_return 0;
}

static co::task_t test52_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn,
        co::modif_pack_t ahead_mods)
{
    auto pool = co_await co::get_pool();
    co_await co::yield();                                   /* the child waits on sem */
    pool->sched(co::add_modifs(pool, test52_named("ahead"), ahead_mods));
    sem->signal();                                          /* the child is queued after it */
    pool->sched(test52_named("witness"));                   /* and the witness after the child */
    kill_fn();
    test52_log.push_back("kill returned");
    co_return 0;
}

static int test52_run(test52_mode_e mode, const std::vector<std::string> &expected) {
    test52_log.clear();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto [ahead_mods, kill_ahead] = co::create_killer(pool.get(), co::ERROR_USER);
    test52_kill_ahead = kill_ahead;

    pool->sched(test52_parent(sem, never, mode, mods));
    pool->sched(test52_controller(sem, kill_fn, ahead_mods));
    ASSERT_FN(pool->run());

    for (auto &l : test52_log)
        DBG("mode %d: %s", (int)mode, l.c_str());
    ASSERT_FN(CHK_BOOL(test52_log == expected));
    return 0;
}

int test52_caller_turn() {
    /* finishes inside the kill: the parent gets 7, at the child's turn */
    ASSERT_FN(test52_run(TEST52_FINISHES,
            {"kill returned", "ahead", "parent 7", "witness"}));
    /* dies at its next wait: the parent gets no value (0), at the child's turn */
    ASSERT_FN(test52_run(TEST52_DIES,
            {"kill returned", "ahead", "parent 0", "witness"}));
    /* the task ahead is removed while the child runs: the parent still comes before the witness */
    ASSERT_FN(test52_run(TEST52_REMOVES_AHEAD,
            {"kill returned", "parent 7", "witness"}));
    return 0;
}

int main() {
    int ret = test52_caller_turn();
    print_test_result("002-005-flowctrl_killer_caller_turn.cpp", ret >= 0);
    return ret;
}
