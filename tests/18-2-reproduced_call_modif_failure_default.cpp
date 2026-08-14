#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test32 - Reproduced Bugs: task<T>::await_resume() after a failed CALL modif
================================================================================================= */

/* When a CO_MODIF_CALL_CBK returns an error, task<T>::await_suspend() aborts the call and resumes
the caller directly - the callee coroutine's promise().ret is left as std::monostate, since its body
never ran. await_resume() used to unconditionally do `std::get<T>(h.promise().ret)`, which throws
std::bad_variant_access - an internal implementation detail leaking out as an unrelated,
undocumented exception type instead of a controlled failure. Fixed in colib.h to detect the
monostate case and either default-construct T (if possible) or throw a clear std::runtime_error.

Also covers the accompanying move-vs-copy fix: await_resume() used to copy the return value out of
the variant (`std::get<T>(h.promise().ret)`); it now moves (`std::get<T>(std::move(...))`), so
move-only return types work. */

int test32_call_cnt = 0;

co::task<int> test32_default_ctor_callee() {
    co_return 123;
}

co::task_t test32_default_ctor_caller() {
    auto pool = co_await co::get_pool();
    auto fail_call = co::create_modif<co::CO_MODIF_CALL_CBK>(pool, co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*) -> co::error_e { test32_call_cnt++; return co::ERROR_GENERIC; });

    auto t = test32_default_ctor_callee();
    co::add_modifs(pool, t, co::modif_pack_t{fail_call});

    /* must not throw std::bad_variant_access - falls back to int{} == 0 */
    auto v = co_await t;
    ASSERT_COFN(CHK_BOOL(v == 0));
    co_return 0;
}

/* A move-only, non-default-constructible type - exercises the std::move() path through the variant
on the (unrelated) successful-call side, since a failed call has no way to invent one of these. */
struct test32_move_only {
    int v;
    explicit test32_move_only(int v) : v(v) {}
    test32_move_only() = delete;
    test32_move_only(const test32_move_only&) = delete;
    test32_move_only(test32_move_only&&) = default;
};

co::task<test32_move_only> test32_move_only_callee() {
    co_return test32_move_only(42);
}

co::task_t test32_move_only_caller() {
    auto v = co_await test32_move_only_callee();
    ASSERT_COFN(CHK_BOOL(v.v == 42));
    co_return 0;
}

int test32_call_modif_failure_default() {
    auto pool = co::create_pool();
    pool->sched(test32_default_ctor_caller());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test32_call_cnt == 1));

    auto pool2 = co::create_pool();
    pool2->sched(test32_move_only_caller());
    ASSERT_FN(pool2->run());

    return 0;
}

int main() {
    int ret = test32_call_modif_failure_default();
    print_test_result("18-2-reproduced_call_modif_failure_default.cpp", ret >= 0);
    return ret;
}
