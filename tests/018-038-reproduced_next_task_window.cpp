#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <cstring>
#include <exception>

/* Test92 - Reproduced Bugs: the scheduler ran inside a suspending coroutine's await_suspend
================================================================================================= */

/* Was an open bug (repro/43): the yield, semaphore and io awaiters, a co_yield without a caller and
external_wait_next_task() queued the suspending coroutine, then ran the scheduler
(pool_internal_t::next_task()) inside its await_suspend. The scheduler runs user code, here the
SCHED callback of a coroutine handed over with thread_sched(). A kill from that callback found the
suspending coroutine in a colib queue and freed its frame, or, when its wait had already done its
work, ran it on to its next wait and freed it there. Either way the await_suspend still running on
that frame then wrote the next coroutine's handle into it (MSVC keeps the handle an await_suspend
returns in the coroutine's frame): an ASan heap-use-after-free. A kill that ran its target into an
external awaitable returning external_wait_next_task() ran the scheduler inside the kill itself,
and popped the kill's placeholder: std::terminate(). The awaiters now return
std::noop_coroutine(): the suspension ends first, and pool_t::run() runs the scheduler between two
resumes.

Each case runs on its own pool, and the victim carries a killer. In the first six, the victim hands
T over with thread_sched() from the pool's own thread, so the pool takes T in at its next
scheduling pass, then the victim suspends: a yield, a semaphore wait, an io wait (a sleep), a
co_yield without a caller, an external awaitable that queues it again and returns
external_wait_next_task(), or a semaphore wait whose semaphore T signals. T's SCHED callback ends
the victim: its kill, or destroy_state() for the generator, which a killer can't be attached to.
In the last case a controller signals the victim's semaphore and kills it, and the kill runs the
victim into an awaitable that holds it and returns external_wait_next_task(). The victim must die
once, at its suspension or at the next one for the two driven cases, and T must run after it. A
plain build doesn't notice a write into freed memory, so this test replaces the global operator
new and delete: a freed block is kept, poisoned, and checked after each case. Under ASan, ASan
does that check itself. */

static const char *test92_file = "018-038-reproduced_next_task_window.cpp";

/* ---------------------------------------------------------------- quarantine allocator */

#if defined(__SANITIZE_ADDRESS__)

static bool test92_quarantine_clean() { return true; }  /* ASan reports a use after free itself */

#else /* __SANITIZE_ADDRESS__ */

/* a block's header: it keeps the blocks 16-byte aligned */
struct test92_block_t {
    test92_block_t *next;           /* the freed block before it */
    size_t n;                       /* the size the user asked for */
};
static_assert(sizeof(test92_block_t) == 16, "the header keeps the blocks 16-byte aligned");

static test92_block_t *test92_freed = nullptr;  /* only this test's one thread allocates */

void *operator new(size_t n) {
    test92_block_t *b = (test92_block_t *)malloc(sizeof(test92_block_t) + n);
    if (!b)
        throw std::bad_alloc();
    b->n = n;
    return b + 1;
}

/* the block is poisoned and kept, never given back: a later write into it changes its bytes */
void operator delete(void *ptr) noexcept {
    if (!ptr)
        return;
    test92_block_t *b = (test92_block_t *)ptr - 1;
    memset(ptr, 0xdd, b->n);
    b->next = test92_freed;
    test92_freed = b;
}

void operator delete(void *ptr, size_t) noexcept {
    operator delete(ptr);
}

/* true when no freed block was written since it was freed */
static bool test92_quarantine_clean() {
    for (test92_block_t *b = test92_freed; b; b = b->next) {
        unsigned char *p = (unsigned char *)(b + 1);
        for (size_t i = 0; i < b->n; i++) {
            if (p[i] != 0xdd) {
                DBG("a freed block of %zu bytes was written at offset %zu", b->n, i);
                return false;
            }
        }
    }
    return true;
}

#endif /* __SANITIZE_ADDRESS__ */

/* ---------------------------------------------------------------- the test */

enum test92_way_e {
    TEST92_YIELD,       /* co::yield() */
    TEST92_SEM,         /* a semaphore nobody signals */
    TEST92_IO,          /* a sleep */
    TEST92_GENERATOR,   /* a co_yield without a caller */
    TEST92_EXTERNAL,    /* an awaitable that queues it again, then external_wait_next_task() */
    TEST92_DRIVEN,      /* a semaphore T signals before the kill: the kill runs it to a yield */
    TEST92_HOLD,        /* its kill runs it into a hold that returns external_wait_next_task() */
};

static const char *test92_way_name[] = {
    "yield", "sem", "io", "generator", "external", "driven", "hold"
};

static test92_way_e test92_way = TEST92_YIELD;
static co::pool_t *test92_pool = nullptr;
static co::sem_p test92_sem;
static co::task_t test92_t;             /* T: the victim hands it over, its SCHED ends the victim */
static int test92_destructed = 0;
static int test92_hold_cancelled = 0;   /* the holding awaitable was destroyed while it held */
static bool test92_got_token = false;   /* a driven victim went past its semaphore wait */
static bool test92_past = false;        /* the victim went past the suspension where it dies */
static bool test92_t_ran = false;
static bool test92_victim_alive = false;
static bool test92_ended_while_alive = false;   /* its end found the victim alive */
static co::error_e test92_end_ret = co::ERROR_GENERIC;

/* a parameter: its copy in the frame lives until the frame is freed (a local dies at co_return) */
struct test92_marker_t {
    bool live = false;
    test92_marker_t() {}
    test92_marker_t(test92_marker_t &&oth) : live(true) { oth.live = false; }
    ~test92_marker_t() {
        if (live) {
            test92_destructed++;
            test92_victim_alive = false;
        }
    }
};

/* queues the coroutine again at once, then lets the pool go on (Option 3 of the external
awaitables) */
struct test92_requeue_t {
    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        co::state_t *s = co::external_on_suspend(h);
        co::external_sched_resume(s);
        return co::external_wait_next_task(s->pool);
    }

    void await_resume() {}
};

/* holds the coroutine, then lets the pool go on (Option 3); destroyed while it holds, it cancels
what it started, here nothing */
struct test92_hold_t {
    bool holding = false;

    ~test92_hold_t() {
        if (holding)
            test92_hold_cancelled++;
    }

    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        holding = true;
        co::state_t *s = co::external_on_suspend(h);
        return co::external_wait_next_task(s->pool);
    }

    void await_resume() { holding = false; }
};

static co::task_t test92_thread_task() {
    test92_t_ran = true;
    co_return 0;
}

static co::task_t test92_victim(test92_way_e way, test92_marker_t) {
    test92_victim_alive = true;
    if (way != TEST92_HOLD)
        test92_pool->thread_sched(test92_t);    /* taken in at the pool's next scheduling pass */
    if (way == TEST92_YIELD) {
        co_await co::yield();
    }
    else if (way == TEST92_SEM) {
        co_await test92_sem->wait();
    }
    else if (way == TEST92_IO) {
        co_await co::sleep_ms(2000);
    }
    else if (way == TEST92_EXTERNAL) {
        co_await test92_requeue_t{};
    }
    else {
        co_await test92_sem->wait();    /* signaled, then killed: the kill runs it on from here */
        test92_got_token = true;
        if (way == TEST92_DRIVEN)
            co_await co::yield();       /* its next wait, where the kill ends it */
        else
            co_await test92_hold_t{};   /* held: the kill ends it here */
    }
    test92_past = true;
    co_return 0;
}

static co::task_t test92_generator(test92_marker_t) {
    test92_victim_alive = true;
    test92_pool->thread_sched(test92_t);
    co_yield 1;                             /* no caller to go back to */
    test92_past = true;
    co_return 0;
}

/* the hold case's end: the victim waits on the semaphore, it gets its token, then its kill */
static co::task_t test92_controller(std::function<co::error_e(void)> kill_fn) {
    test92_sem->signal();                   /* the victim is queued with its token */
    test92_ended_while_alive = test92_victim_alive;
    test92_end_ret = kill_fn();             /* its wait did its work: the kill runs it on */
    co_return 0;
}

static int test92_case(test92_way_e way) {
    DBG("case: %s", test92_way_name[way]);
    test92_way = way;
    test92_destructed = 0;
    test92_hold_cancelled = 0;
    test92_got_token = false;
    test92_past = false;
    test92_t_ran = false;
    test92_victim_alive = false;
    test92_ended_while_alive = false;
    test92_end_ret = co::ERROR_GENERIC;

    auto pool = co::create_pool();
    test92_pool = pool.get();
    test92_sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    co::task_t victim;
    co::state_t *generator = nullptr;
    if (way == TEST92_GENERATOR) {
        victim = test92_generator(test92_marker_t{});
        generator = victim.get_state();
    }
    else {
        victim = co::add_modifs(pool.get(), test92_victim(way, test92_marker_t{}), killer_mods);
    }

    auto end = co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [&kill_fn, generator, way](co::state_t *) -> co::error_e {
                test92_ended_while_alive = test92_victim_alive;
                if (generator) {
                    co::destroy_state(generator);   /* its holder ends it */
                    test92_end_ret = co::ERROR_OK;
                    return co::ERROR_OK;
                }
                if (way == TEST92_DRIVEN)
                    test92_sem->signal();           /* the wait did its work: the kill runs it */
                test92_end_ret = kill_fn();
                return co::ERROR_OK;
            });
    if (way != TEST92_HOLD)
        test92_t = co::add_modifs(pool.get(), test92_thread_task(), co::modif_pack_t(1, end));

    pool->sched(COLIB_REGNAME(victim));
    if (way == TEST92_HOLD)
        pool->sched(test92_controller(kill_fn));
    ASSERT_FN(pool->run());
    test92_sem = nullptr;

    DBG("%s: end %s, alive at the end %d, destructed %d, token %d, past %d, cancelled %d, "
            "T ran %d", test92_way_name[way], co::dbg_enum(test92_end_ret).c_str(),
            (int)test92_ended_while_alive, test92_destructed, (int)test92_got_token,
            (int)test92_past, test92_hold_cancelled, (int)test92_t_ran);
    ASSERT_FN(CHK_BOOL(test92_ended_while_alive && test92_end_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test92_destructed == 1 && !test92_past));
    bool driven = way == TEST92_DRIVEN || way == TEST92_HOLD;
    ASSERT_FN(CHK_BOOL(test92_got_token == driven));
    ASSERT_FN(CHK_BOOL(test92_hold_cancelled == (way == TEST92_HOLD ? 1 : 0)));
    ASSERT_FN(CHK_BOOL(test92_t_ran == (way != TEST92_HOLD)));
    ASSERT_FN(CHK_BOOL(test92_quarantine_clean()));
    return 0;
}

int test92_next_task_window() {
    ASSERT_FN(test92_case(TEST92_YIELD));
    ASSERT_FN(test92_case(TEST92_SEM));
    ASSERT_FN(test92_case(TEST92_IO));
    ASSERT_FN(test92_case(TEST92_GENERATOR));
    ASSERT_FN(test92_case(TEST92_EXTERNAL));
    ASSERT_FN(test92_case(TEST92_DRIVEN));
    ASSERT_FN(test92_case(TEST92_HOLD));
    return 0;
}

int main() {
    /* the unfixed header ends the hold case in std::terminate(): a failure, not an error dialog */
    std::set_terminate([] {
        DBG("std::terminate() in the %s case", test92_way_name[test92_way]);
        print_test_result(test92_file, false);
        std::_Exit(1);
    });
    int ret = test92_next_task_window();
    print_test_result(test92_file, ret >= 0);
    return ret;
}
