/* Test104 - Reproduced Bugs: colib.h used <cstdio>/<algorithm>/<tuple> symbols it never included
================================================================================================= */

/* colib.h's include block is the complete list of standard headers the library asks for, yet the
library used printf (the default COLIB_LOG_FUNCTION), snprintf (dbg_to_str and dbg_format) and
sprintf (the Windows get_last_error), std::remove_if (rm_modifs_from_table and the Linux epoll
waiter erase), std::min (sem_t::signal_all) and std::tuple with std::apply
(allocator_memory_t's buckets, wait_all, and every Windows IO dispatcher) without including
<cstdio>, <algorithm> or <tuple>. The header compiled only through other headers' transitive
includes, which the standard does not promise; a leaner or refactored standard library fails to
compile every translation unit that includes colib.h.

The fix adds those three headers to the include block. This file is the compile canary: it
includes colib.h with default configuration and forces every flagged use site into the
translation unit — the unconditional ones compile by mere inclusion and are anchored here by
address, the two template-only ones (dbg_format's snprintf, wait_all's tuple and apply over its
create_future futures) are instantiated and run. On a toolchain whose transitive includes cover
the omission this file passes before the fix as well; MSVC 19.43 on this box is one, so the fix
documentation shows both runs instead of claiming a failure/recovery pair. On a toolchain
without that coverage the unfixed header fails to compile this file, and the fixed header
compiles and passes it unchanged. */

#include "../colib.h"
#include "tests_common.h"

co::task_t test104_co_wait_all() {
    auto ret = co_await co::wait_all(
        []() -> co::task<int> { co_return 4; }(),
        []() -> co::task<int> { co_return 5; }()
    );

    /* each member reports its value and ERROR_OK (the pair create_future resolves to) */
    bool is_ok = ret == std::tuple<std::pair<int, co::error_e>, std::pair<int, co::error_e>>{
            {4, co::ERROR_OK}, {5, co::ERROR_OK} };
    ASSERT_COFN(CHK_BOOL(is_ok));

    co_return 0;
}

int test104_missing_includes() {
    /* Mere inclusion compiles the unconditional users: sem_t::signal_all (std::min),
       rm_modifs_from_table (std::remove_if), allocator_memory_t (std::tuple member, std::apply)
       and, on Windows, dbg_to_str and the IO dispatchers (snprintf, tuple CTAD, std::apply).
       Taking two of their addresses keeps that fact anchored in this translation unit. */
    auto signal_all = &co::sem_t::signal_all;
    (void)signal_all;
    auto rm_modifs = &co::rm_modifs_from_table;
    (void)rm_modifs;

    /* dbg_format is a template, so its unqualified snprintf compiles only when instantiated. */
    co::dbg_string_t fmt = co::dbg_format("%d-%s", 42, "instantiated");
    ASSERT_FN(CHK_BOOL(fmt.compare("42-instantiated") == 0));

    /* Calling log_str executes the default COLIB_LOG_FUNCTION lambda, whose body is printf. */
    ASSERT_FN(CHK_BOOL(co::log_str(fmt) >= 0));

    /* wait_all is a template, so its std::tuple and std::apply over create_future futures
       compile only when instantiated — and run here for real. */
    auto pool = co::create_pool();
    pool->sched(test104_co_wait_all());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));

    return 0;
}

int main() {
    int ret = test104_missing_includes();
    print_test_result("018-048-reproduced_missing_includes.cpp", ret >= 0);
    return ret;
}
