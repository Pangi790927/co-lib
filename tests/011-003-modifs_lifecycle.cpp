#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#if COLIB_OS_LINUX
#include <unistd.h>
#endif

/* Test30 - Modifs: EXIT/LEAVE/ENTER/WAIT_IO/UNWAIT_IO/WAIT_SEM/UNWAIT_SEM
================================================================================================= */

/* Covers the 7 modif_e types not exercised by 11-1-modifs.cpp (CALL/SCHED). Ordering is verified
against colib.h's actual awaiter implementations, not just the doc comments - see BUGS.md #1 for a
doc/implementation mismatch found while writing this: WAIT_IO/WAIT_SEM fire BEFORE LEAVE on suspend
(io_awaiter_t::await_suspend, sem_awaiter_t::await_suspend), not after as CO_MODIF_WAIT_IO_CBK's doc
comment claims. Resume order is ENTER then UNWAIT_IO/UNWAIT_SEM. Exit (on plain co_return) is LEAVE
then EXIT.

Flags use CO_MODIF_INHERIT_ON_CALL: co::wait_event() is itself a small wrapping coroutine (not a raw
awaiter inlined into the caller), so the io_awaiter_t suspension happens in *its own* state_t, a
separate frame from test30_child's. Without ON_CALL the modif pack wouldn't propagate into that
nested frame and WAIT_IO/UNWAIT_IO would never be observed here. With it, LEAVE/ENTER additionally
show up bracketing the call boundary itself (LEAVE on test30_child as it calls in, ENTER as
wait_event()'s task starts; ENTER on test30_child's resume, right after wait_event()'s own EXIT) -
per colib.h's own doc for LEAVE_CBK, which fires "on each suspended corutine", not just the frame
directly holding the awaiter. Two EXITs appear because two separate coroutine objects each
individually complete: wait_event()'s task, then test30_child. */

#if COLIB_OS_LINUX

std::vector<std::string> test30_log;

co::modif_pack_t test30_make_modifs(co::pool_t *pool) {
    co::modif_flags_e flags = co::CO_MODIF_INHERIT_ON_CALL;
    co::modif_pack_t pack;

    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(pool, flags,
        [](co::state_t*) -> co::error_e { test30_log.push_back("LEAVE"); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(pool, flags,
        [](co::state_t*) -> co::error_e { test30_log.push_back("ENTER"); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(pool, flags,
        [](co::state_t*) -> co::error_e { test30_log.push_back("EXIT"); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_IO_CBK>(pool, flags,
        [](co::state_t*, co::io_desc_t&) -> co::error_e {
            test30_log.push_back("WAIT_IO"); return co::ERROR_OK;
        }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_IO_CBK>(pool, flags,
        [](co::state_t*, co::io_desc_t&) -> co::error_e {
            test30_log.push_back("UNWAIT_IO"); return co::ERROR_OK;
        }));
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(pool, flags,
        [](co::state_t*, co::sem_t*, co::sem_waiter_handle_p) -> co::error_e {
            test30_log.push_back("WAIT_SEM"); return co::ERROR_OK;
        }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(pool, flags,
        [](co::state_t*, co::sem_t*) -> co::error_e {
            test30_log.push_back("UNWAIT_SEM"); return co::ERROR_OK;
        }));

    return pack;
}

co::task_t test30_child(co::sem_p sem, int read_fd) {
    co_await sem->wait();
    co::io_desc_t desc{ .fd = read_fd, .events = EPOLLIN };
    co_await co::wait_event(desc);
    co_return 0;
}

co::task_t test30_driver(co::sem_p sem, int write_fd) {
    /* by the time this runs, test30_child (scheduled first, FIFO ready-queue order) has already
    suspended on sem->wait() - no yield() needed before signaling it */
    sem->signal();
    char c = 'x';
    ASSERT_COFN(CHK_BOOL(write(write_fd, &c, sizeof(c)) == sizeof(c)));
    co_return 0;
}

int test30_modifs_lifecycle() {
    int fds[2];
    ASSERT_FN(pipe(fds) == 0);
    FnScope scope([&] { close(fds[0]); close(fds[1]); });

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    auto child = co::add_modifs(pool.get(), test30_child(sem, fds[0]), test30_make_modifs(pool.get()));
    pool->sched(child);
    pool->sched(test30_driver(sem, fds[1]));

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));

    std::vector<std::string> expected = {
        "ENTER",                                   /* test30_child starts */
        "WAIT_SEM", "LEAVE",                        /* suspends on sem->wait() */
        "ENTER", "UNWAIT_SEM",                      /* resumes once signaled */
        "LEAVE",                                    /* test30_child leaves to call wait_event() */
        "ENTER",                                    /* wait_event()'s own task starts */
        "WAIT_IO", "LEAVE",                         /* wait_event() suspends on the io_awaiter_t */
        "ENTER", "UNWAIT_IO",                       /* wait_event() resumes once fd is readable */
        "LEAVE", "EXIT",                            /* wait_event()'s task completes */
        "ENTER",                                    /* test30_child resumes after the call returns */
        "LEAVE", "EXIT"                              /* test30_child completes */
    };
    if (test30_log != expected) {
        DBG("log mismatch, got:");
        for (auto &s : test30_log)
            DBG("  %s", s.c_str());
        return -1;
    }

    return 0;
}

#else

int test30_modifs_lifecycle() {
    DBG("modifs_lifecycle: not supported/verified on this OS");
    return 0;
}

#endif

int main() {
    int ret = test30_modifs_lifecycle();
    print_test_result("011-003-modifs_lifecycle.cpp", ret >= 0);
    return ret;
}
