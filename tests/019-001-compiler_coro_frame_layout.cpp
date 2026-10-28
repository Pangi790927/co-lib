#include "../colib.h"
#include "tests_common.h"

/* Test81 - Compiler: the layout of a coroutine frame, and which parts of it the compiler writes
================================================================================================= */

/* A measurement, not a check of colib: it prints how this compiler lays out a coroutine frame, for
the idea of a frame whose header (the resume and destroy pointers and the promise) is read-only to
user code while its parameters and locals stay writable. It answers two questions:

1. Where do the parts sit inside the allocation: the resume and destroy pointers, the promise, a
   parameter's copy, a local that lives across a suspend? The header's end is where a page boundary
   would have to fall.
2. Which bytes does the compiler's own code write after the frame is created: at each suspend and
   at the final suspend? A write inside the header would fault if the header were read-only.

The frame is filled with a pattern when it is allocated, so the bytes the compiler writes while
creating the frame show up as well. The promise is inert: nothing in this file writes it after its
construction, so any change inside it would come from the compiler. The test fails only if the
measurement itself is invalid (a part found outside the allocation); the layout it prints is a fact
of the compiler, not a pass or fail. The frame of a colib task gets its promise offset and size
printed too, since colib's promise is the part that would be read-only.

Measured 2026-09-25 on MSVC 19.43 (with and without /O2) and g++ 15.2 (-O3): the same layout on
both. The frame starts with the two pointers, the promise follows at 16, and the parameters, the
locals and the suspend index all come after the promise. A suspend writes nothing in the header.
The final suspend writes it once: it nulls the resume pointer, which is how done() works. */

#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

static constexpr unsigned char test81_fill = 0xA5;
static unsigned char *test81_alloc = nullptr;
static size_t test81_size = 0;
static uintptr_t test81_param_addr = 0;
static uintptr_t test81_local_addr = 0;

/* a plain coroutine type: it allocates its frame through the promise, so the frame's start and size
are known */
struct test81_task {
    struct promise_type {
        uint64_t marker[4] = {0x1111111111111111, 0x2222222222222222, 0x3333333333333333,
                0x4444444444444444};

        static void *operator new(size_t size) {
            test81_alloc = (unsigned char *)std::malloc(size);
            if (test81_alloc)
                memset(test81_alloc, test81_fill, size);
            test81_size = size;
            return test81_alloc;
        }
        static void operator delete(void *ptr) { std::free(ptr); }

        test81_task get_return_object() {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h;
};

/* two suspends and a final suspend; the local's value changes between them, so its own bytes are
expected to change and are named as such in the report */
static test81_task test81_coro(uint64_t param) {
    volatile uint64_t local = 0x5555555555555555;
    test81_param_addr = (uintptr_t)&param;
    test81_local_addr = (uintptr_t)&local;
    co_await std::suspend_always{};
    local = 0x6666666666666666;
    co_await std::suspend_always{};
    local = 0x7777777777777777;
    param += local;
    co_return;
}

struct test81_layout {
    size_t handle_off;
    size_t promise_off;
    size_t promise_end;
    size_t param_off;
    size_t local_off;
};

static const char *test81_part(const test81_layout &l, size_t off) {
    if (off >= l.handle_off && off < l.handle_off + sizeof(void *))
        return "resume pointer";
    if (off >= l.handle_off + sizeof(void *) && off < l.handle_off + 2 * sizeof(void *))
        return "destroy pointer";
    if (off >= l.promise_off && off < l.promise_end)
        return "promise";
    if (off >= l.param_off && off < l.param_off + sizeof(uint64_t))
        return "parameter";
    if (off >= l.local_off && off < l.local_off + sizeof(uint64_t))
        return "local";
    return "compiler's own";
}

/* prints the byte ranges that differ between two snapshots (or, without `before`, the ones that
differ from the fill pattern), each with the part of the frame it belongs to; returns how many of
the changed bytes lie inside the header */
static size_t test81_report(const char *what, const test81_layout &l,
        const std::vector<unsigned char> *before, const std::vector<unsigned char> &after)
{
    size_t in_header = 0;
    DBG("%s:", what);
    size_t off = 0;
    bool any = false;
    while (off < after.size()) {
        auto changed = [&](size_t o) {
            return before ? (*before)[o] != after[o] : after[o] != test81_fill;
        };
        if (!changed(off)) {
            off++;
            continue;
        }
        const char *part = test81_part(l, off);
        size_t start = off;
        while (off < after.size() && changed(off) && test81_part(l, off) == part)
            off++;
        bool header = start < l.promise_end;
        if (header)
            in_header += off - start;
        DBG("    bytes [%3zu, %3zu) %-16s%s", start, off, part, header ? " (in the header)" : "");
        any = true;
    }
    if (!any)
        DBG("    nothing changed");
    return in_header;
}

static std::vector<unsigned char> test81_snap() {
    return std::vector<unsigned char>(test81_alloc, test81_alloc + test81_size);
}

int test81_frame_layout() {
    test81_task t = test81_coro(0x0123456789abcdef);
    ASSERT_FN(CHK_PTR(test81_alloc));
    FnScope destroy([&t]{ t.h.destroy(); });

    /* the parameter's and the local's addresses are only known once the body starts */
    auto created = test81_snap();
    t.h.resume();
    auto first = test81_snap();

    uintptr_t base = (uintptr_t)test81_alloc;
    uintptr_t end = base + test81_size;
    uintptr_t handle_addr = (uintptr_t)t.h.address();
    uintptr_t promise_addr = (uintptr_t)&t.h.promise();
    for (uintptr_t addr : {handle_addr, promise_addr, test81_param_addr, test81_local_addr})
        ASSERT_FN(CHK_BOOL(addr >= base && addr < end));

    test81_layout l;
    l.handle_off = handle_addr - base;
    l.promise_off = promise_addr - base;
    l.promise_end = l.promise_off + sizeof(test81_task::promise_type);
    l.param_off = test81_param_addr - base;
    l.local_off = test81_local_addr - base;

    DBG("frame: %zu bytes, pointers %zu bytes", test81_size, sizeof(void *));
    DBG("    handle.address()  at %3zu (the resume pointer, then the destroy pointer)",
            l.handle_off);
    DBG("    promise           at %3zu, %zu bytes, ends at %zu", l.promise_off,
            sizeof(test81_task::promise_type), l.promise_end);
    DBG("    parameter's copy  at %3zu", l.param_off);
    DBG("    local             at %3zu", l.local_off);
    bool split = l.param_off >= l.promise_end && l.local_off >= l.promise_end;
    DBG("    header = [0, %zu); parameter and local %s", l.promise_end,
            split ? "both after it" : "NOT both after it");

    size_t header_writes = 0;
    test81_report("written while creating the frame", l, nullptr, created);
    header_writes += test81_report("changed by the first resume (to the first suspend)", l,
            &created, first);
    t.h.resume();
    auto second = test81_snap();
    header_writes += test81_report("changed by the second resume (to the second suspend)", l,
            &first, second);
    t.h.resume();
    ASSERT_FN(CHK_BOOL(t.h.done()));
    auto final = test81_snap();
    header_writes += test81_report("changed by the last resume (to the final suspend)", l,
            &second, final);
    DBG("header bytes written after the frame was created: %zu", header_writes);
    return 0;
}

static co::task_t test81_colib_coro() {
    co_return 0;
}

int test81_colib_promise() {
    auto pool = co::create_pool();
    auto t = test81_colib_coro();
    uintptr_t handle_addr = (uintptr_t)t.h.address();
    uintptr_t promise_addr = (uintptr_t)&t.h.promise();
    DBG("colib task: promise at handle.address() %+lld, %zu bytes",
            (long long)(promise_addr - handle_addr), sizeof(decltype(t)::promise_type));
    pool->sched(t);
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test81_frame_layout();
    if (ret >= 0)
        ret = test81_colib_promise();
    print_test_result("019-001-compiler_coro_frame_layout.cpp", ret >= 0);
    return ret;
}
