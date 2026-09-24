#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <cstring>

/* Test57 - Reproduced Bugs: a semaphore freed by its own waiter while it clears its waiters
================================================================================================= */

/* The only owner of a semaphore is a coroutine waiting on it (the sem_p is one of its parameters).
When the pool dies, pool_internal_t::clear() clears every semaphore: sem_internal_t::clear()
destroys the waiter, the waiter's frame drops the last sem_p, and the semaphore is freed - while
its clear() is still running. clear() then keeps reading its wait list and writes `val`, all in
freed memory. With the release CRT that is silent (the freed block still looks valid), so this
test replaces the global operator new/delete to poison freed memory: the read then hits garbage
and crashes. AddressSanitizer reports it as a heap-use-after-free in sem_internal_t::clear(), on
the colib.h of before and after the killer redesign alike. 2026-09-24 */

/* ---------------------------------------------------------------- poisoning allocator */

static const size_t test57_header = 16;     /* keeps the returned blocks 16-byte aligned */

void *operator new(size_t n) {
    unsigned char *p = (unsigned char *)malloc(n + test57_header);
    if (!p)
        throw std::bad_alloc();
    memcpy(p, &n, sizeof(n));
    return p + test57_header;
}

void operator delete(void *ptr) noexcept {
    if (!ptr)
        return;
    unsigned char *p = (unsigned char *)ptr - test57_header;
    size_t n;
    memcpy(&n, p, sizeof(n));
    memset(ptr, 0xdd, n);                   /* whatever reads it after this reads garbage */
    free(p);
}

void operator delete(void *ptr, size_t) noexcept {
    operator delete(ptr);
}

/* ---------------------------------------------------------------- the test */

static int test57_waiters_destroyed = 0;

struct test57_marker_t {
    ~test57_marker_t() { test57_waiters_destroyed++; }
};

static co::task_t test57_waiter(co::sem_p sem) {
    test57_marker_t marker;
    co_await sem->wait();                   /* never signaled */
    co_return 0;
}

int test57_sem_freed_by_its_waiter() {
    {
        auto pool = co::create_pool();
        /* nobody but the waiters keeps these semaphores */
        pool->sched(test57_waiter(co::create_sem(pool, 0)));
        pool->sched(test57_waiter(co::create_sem(pool, 0)));
        ASSERT_FN(pool->run());
    }                                       /* the pool dies: it clears both semaphores */
    ASSERT_FN(CHK_BOOL(test57_waiters_destroyed == 2));
    return 0;
}

int main() {
    int ret = test57_sem_freed_by_its_waiter();
    print_test_result("018-018-reproduced_sem_freed_by_its_waiter.cpp", ret >= 0);
    return ret;
}
