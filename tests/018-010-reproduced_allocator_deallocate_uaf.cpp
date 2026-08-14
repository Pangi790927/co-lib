#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test42 - Reproduced Bugs: allocator_t<T>::deallocate() UAFs if its pool died first
================================================================================================= */

/* OPEN BUG - see BUGS.md. colib.h ~3997, allocator_t<T>::deallocate()'s own TODO: "modifs
dereferenced by this will break if the pool dies, and since those are meant by the user, notok".
Not fixed yet: this test is expected to fail (crash) until it is. Once fixed, this file stays as-is
so the bug can't silently come back.

allocator_t<T> holds a raw pool_t* captured at construction - not a pool_p/shared_ptr, so it does
nothing to keep the pool alive. deallocate() unconditionally dereferences that raw pointer
(pool->allocator_memory) to decide whether the block being freed belongs to the pool's own
bump/bucket allocator or was a plain malloc() fallback. Anything allocated via allocator_t<T> - most
directly, a modif_p returned by create_modif(), which users are explicitly meant to hold onto and
reuse across multiple add_modifs()/rm_modifs() calls (see 018-001, 011-005) - can legitimately outlive
the pool_p it was created from: nothing about holding a modif_p keeps the underlying pool alive.

If the pool is destroyed first and the modif_p is only released afterward (its refcount finally
hits 0, e.g. because the caller stored it somewhere longer-lived than the pool, or just at ordinary
scope-exit ordering), deallocate() dereferences the now-dead pool_t through that stale raw pointer:
a genuine use-after-free, not a hypothetical one - confirmed here, this test reliably segfaults on
this build (unlike some of the other UB found in this codebase, which happened not to fault - this
one does, consistently). */

int test42_allocator_deallocate_uaf() {
    co::modif_p mod;
    {
        auto pool = co::create_pool();
        mod = co::create_modif<co::CO_MODIF_CALL_CBK>(pool.get(), co::CO_MODIF_INHERIT_NONE,
            [](co::state_t*) -> co::error_e { return co::ERROR_OK; });
    } /* pool_p goes out of scope - pool_t is destroyed while mod (holding a raw pool_t* inside its
    allocator_t) is still alive */

    mod.reset(); /* drops the last reference -> allocator_t<modif_t>::deallocate() runs against the
    already-destroyed pool - this is where it crashes */

    return 0;
}

int main() {
    int ret = test42_allocator_deallocate_uaf();
    print_test_result("018-010-reproduced_allocator_deallocate_uaf.cpp", ret >= 0);
    return ret;
}
