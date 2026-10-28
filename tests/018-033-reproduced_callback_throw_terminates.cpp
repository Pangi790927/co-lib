#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test87 - Reproduced Bugs: a throw out of a resume's callbacks lost the semaphore token
================================================================================================= */

/* Was an open bug (repro/46): a waiter woken by signal() already holds its token, and a throw out
of one of its UNWAIT_SEM or ENTER callbacks on the resume left before sem_awaiter_t::await_resume()
handed out the unlocker, so the token was lost and a mutex-style semaphore blocked everyone else for
good. A modif callback can't throw now: do_generic_modifs() catches the throw and calls
std::terminate(), so the program ends inside the callback, before the coroutine sees anything. That
also pins how it ends, the same on every compiler: the callback's own locals are destroyed first,
and the terminate handler still sees the exception. */

static const char *test87_file = "018-033-reproduced_callback_throw_terminates.cpp";
static bool test87_local_destroyed = false;
static bool test87_body_caught = false;

struct test87_error_t : public std::exception {};

struct test87_local_t {
    ~test87_local_t() { test87_local_destroyed = true; }
};

static co::task_t test87_waiter(co::sem_p mtx) {
    try {
        co_await mtx->wait();       /* signaled: its UNWAIT_SEM callback throws on the resume */
    }
    catch (...) {
        test87_body_caught = true;  /* the bug's path: the throw reached the body, token lost */
    }
    co_return 0;
}

static co::task_t test87_signaler(co::sem_p mtx) {
    co_await co::yield();           /* the waiter waits on mtx */
    mtx->signal();
    co_return 0;
}

int test87_callback_throw() {
    std::set_terminate([] {
        bool saw = false;
        if (auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            }
            catch (const test87_error_t &) {
                saw = true;
            }
            catch (...) {
            }
        }
        bool ok = saw && test87_local_destroyed && !test87_body_caught;
        print_test_result(test87_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto mtx = co::create_sem(pool, 0);

    auto throw_mods = co::modif_pack_t{};
    throw_mods.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *) -> co::error_e {
                test87_local_t local;
                throw test87_error_t{};
            }));

    pool->sched(co::add_modifs(pool.get(), test87_waiter(mtx), throw_mods));
    pool->sched(test87_signaler(mtx));
    pool->run();
    DBG("no terminate: the throw out of the UNWAIT_SEM callback must end the program");
    return -1;
}

int main() {
    int ret = test87_callback_throw();
    print_test_result(test87_file, ret >= 0);
    return ret;
}
