#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test42 - Reproduced Bugs: allocator_t<T>::deallocate() UAFs if its pool died first
================================================================================================= */

/* Was an open bug (allocator_t<T>::deallocate() carried its own TODO: "modifs dereferenced by this
will break if the pool dies, and since those are meant by the user, notok") - now fixed, not by
patching deallocate() itself, but by removing the problem at its source: create_modif() no longer
takes a pool parameter at all, and modif_t is now allocated with plain std::make_shared<modif_t>(...)
instead of going through the pool-bound allocator_t<T>. modif_t never actually had any pool-bound
members, so this isn't a workaround - a modif_p now holds no reference to any pool whatsoever, which
means this specific use-after-free isn't just fixed, it's structurally impossible: there's no longer
a raw pool_t* anywhere in the object for deallocate() to dereference. This file is the regression
test for that fix (and stays useful as one even though the mechanism moved: if create_modif() or
modif_t ever goes back to pool-bound allocation, this is what would catch the regression).

allocator_t<T> holds a raw pool_t* captured at construction - not a pool_p/shared_ptr, so it does
nothing to keep the pool alive. deallocate() unconditionally dereferences that raw pointer
(pool->allocator_memory) to decide whether the block being freed belongs to the pool's own
bump/bucket allocator or was a plain malloc() fallback. Anything allocated via allocator_t<T> could
legitimately outlive the pool_p it was created from - nothing about holding such an object keeps the
underlying pool alive - and modif_p (returned by create_modif(), meant to be held onto and reused
across multiple add_modifs()/rm_modifs() calls, see 018-001/011-005) was the most direct example of
that. If the pool were destroyed first and the object released afterward, deallocate() would
dereference the dead pool_t through that stale raw pointer: a genuine use-after-free, not a
hypothetical one - confirmed reliably segfaulting before the fix (unlike some other UB found in this
codebase, e.g. the earlier unlocker_t null-`this` case, which happened not to fault - this one did,
consistently). */

int test42_allocator_deallocate_uaf() {
    co::modif_p mod;
    {
        auto pool = co::create_pool();
        mod = co::create_modif<co::CO_MODIF_CALL_CBK>(co::CO_MODIF_INHERIT_NONE,
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
