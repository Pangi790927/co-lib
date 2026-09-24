#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test51 - Modifs: closing callbacks run in reverse order
================================================================================================= */

/* Two modifs, `a` added before `b`, on one coroutine. The opening callbacks (ENTER, WAIT_SEM) run
in the order the modifs were added, the closing ones (LEAVE, UNWAIT_SEM, EXIT) in reverse, so the
pairs nest like constructors and destructors: a opens, b opens, b closes, a closes. */

static std::string test51_order;

static co::modif_pack_t test51_pack(char id) {
    auto flags = co::CO_MODIF_INHERIT_NONE;
    auto log = [id](char ev) { test51_order += ev; test51_order += id; test51_order += ' '; };
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
        [log](co::state_t *) -> co::error_e { log('E'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
        [log](co::state_t *) -> co::error_e { log('L'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
        [log](co::state_t *, co::sem_t *) -> co::error_e {
            log('W'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
        [log](co::state_t *, co::sem_t *) -> co::error_e { log('U'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(flags,
        [log](co::state_t *) -> co::error_e { log('X'); return co::ERROR_OK; }));
    return pack;
}

static co::task_t test51_waiter(co::sem_p sem) {
    co_await sem->wait();
    co_return 0;
}

static co::task_t test51_signaler(co::sem_p sem) {
    sem->signal();
    co_return 0;
}

int test51_close_order() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    auto t = test51_waiter(sem);
    co::add_modifs(pool.get(), t, test51_pack('a'));
    co::add_modifs(pool.get(), t, test51_pack('b'));
    pool->sched(t);
    pool->sched(test51_signaler(sem));
    ASSERT_FN(pool->run());

    /* sched's ENTER; the wait: LEAVE, WAIT; the resume: UNWAIT, ENTER; the return: LEAVE, EXIT */
    DBG("order: %s", test51_order.c_str());
    ASSERT_FN(CHK_BOOL(test51_order ==
            "Ea Eb Lb La Wa Wb Ub Ua Ea Eb Lb La Xb Xa "));
    return 0;
}

int main() {
    int ret = test51_close_order();
    print_test_result("011-007-modifs_close_order.cpp", ret >= 0);
    return ret;
}
