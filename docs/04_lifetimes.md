# 04. Lifetimes: Coroutine Frames, Killers, Semaphores, the Pool, and Modifications

`03_execution_model.md` covers how control moves between coroutines. This chapter is about how long
things actually live: when a coroutine's frame is destroyed, the two different mechanisms the library
uses to tear a whole call stack down at once, and the lifetimes of the three long-lived objects
everything else is scoped to - semaphores, the pool itself, and modifications. Line references are
against `colib.h` as of commit `49232f1` (+ local changes on top) - see `progress.md`.

---

## Coroutine frames: when they actually go away

This builds directly on `03_execution_model.md`'s "Call" section - the short version, restated for
context: a callee's coroutine frame is destroyed by its **caller**, inside `task<T>::await_resume()`
(colib.h ~2704-2719), and *only* on a real `co_return` - `h.destroy()` runs unless
`h.promise().state.err == ERROR_YIELDED`. A `co_yield` leaves the frame alive and resumable; a
`sched`'d (caller-less) task that finishes destroys itself instead, via `post_to_destroy` (see ch.03's
"Sched" section).

**Consequence: awaiting a `task<T>` again after it has actually finished (not yielded) is undefined
behavior**, because the frame it would resume into no longer exists - `h` is a dangling handle at that
point. Nothing checks for this at the call site; the library's contract is simply "don't."

Both of the destruction paths above happen at a single, well-defined moment triggered by normal
control flow. The rest of this chapter is about the two mechanisms for destroying a frame - or a whole
chain of them - **out of band**, not as a consequence of the coroutine finishing on its own.

---

## `destroy_state()`: unconditionally unwinding a stack of frames

```cpp
inline void destroy_state(state_t *curr) {
    while (curr) {
        state_t *next = curr->caller_state;
        do_exit_modifs(curr);
        curr->self.destroy();
        curr = next;
    }
}
```

(colib.h ~2298-2307.) This is the library's simplest teardown primitive: starting from a given
`state_t`, walk `caller_state` upward - callee, then its caller, then *that* caller, and so on - firing
`EXIT` and calling `.destroy()` on every frame in the chain, unconditionally. It doesn't check where a
frame currently "is" (ready queue, mid-wait, whatever) - it assumes the frame is already just inert
data sitting somewhere the caller already knows about, not something that needs to be located or
un-parked first.

That assumption is exactly why every call site for `destroy_state()` is a place that already has
direct ownership of the frame in question, not a "reach in and find it" scenario:

- **A semaphore's own wait list**, on `clear()`/destructor (colib.h ~4344-4351, ~4496) - the semaphore
  already holds the waiter's `state_t*` directly in `waiting_on_sem`.
- **The IO backends' own waiter maps**, on force-awake/force-close paths (colib.h ~3023, ~3040,
  ~3518) - same reasoning, the backend already has the `state_t*` on hand.
- **The pool's ready queue**, on `pool_t::clear()` (colib.h ~4413-4416) - `ready_tasks` is a plain
  list of `state_t*`.

In every one of these, whatever's being destroyed was *sitting still* - parked, not mid-flight - which
is what makes the unconditional walk-and-destroy safe. Reaching into a coroutine that might currently
be running, or that the caller doesn't already have a direct handle to, needs the heavier machinery
below.

---

## `create_killer()`: reaching into a coroutine that could be anywhere

`create_killer(pool, e)` (colib.h ~5873-6010) returns a `{modif_pack_t, trigger_fn}` pair. Attaching
the pack to a coroutine (with `CO_MODIF_INHERIT_ON_CALL`) makes that coroutine's `CALL`/`SCHED`/`EXIT`
modifs maintain a private `call_stack` (a `std::stack<state_t*>`, pushed on `CALL`/`SCHED`, popped on
`EXIT`) and, if it's ever waiting on I/O or a semaphore, remembers that too (`WAIT_IO`/`WAIT_SEM`
modifs stash the `io_desc_t*`/`sem_t*`). Calling the trigger function (`sig_kill`) then has to do
something `destroy_state()` doesn't: **first figure out where the top of that stack currently is**,
because unlike the call sites above, the killer doesn't already know.

`sig_kill`'s unwind, in order:

1. **Locate and un-park.** Try `remove_ready()` first (it might just be sitting in the ready queue).
   If not, and it was recorded as waiting on a semaphore, erase it from that semaphore's wait list.
   If it was recorded as waiting on I/O, call `stop_io()` (which pushes it onto the ready queue with
   an error) and then remove it from the ready queue again. Exactly one of these applies.
2. **Replay the correct exit-from-wait modifs.** If it was mid-wait (sem or IO), fire
   `ENTER` → `UNWAIT_SEM`/`UNWAIT_IO` → `LEAVE` by hand - the same sequence a real wakeup would have
   produced, so the coroutine's modif bookkeeping (and any `COLIB_ENABLE_DEBUG_CHECKS` invariants)
   stay consistent even though nothing actually resumed it.
3. **Unwind the tracked `call_stack`, not `caller_state`.** Everything except the bottom frame gets
   `EXIT` + `.destroy()`'d directly (`while (call_stack.size() > 1)`) - note this is the killer's
   *own* `call_stack`, built purely from watching `CALL`/`SCHED`/`EXIT` on this one attached
   coroutine, not a walk of `caller_state` the way `destroy_state()` does it.
4. **The bottom frame gets different treatment depending on how the whole chain started:** if it has
   no `caller_state` (the origin was `sched`'d), it's destroyed too - nothing else is waiting on it.
   If it *does* have a `caller_state` (the origin was itself `call`ed by something outside the killed
   chain), that frame is **not** destroyed - its `err` is set to `e` and its caller is pushed onto the
   ready queue instead, exactly the same "caller owns callee cleanup" contract from ch.03's "Call"
   section. A killer never destroys a frame it doesn't own the destruction rights to.

**A known open sharp edge, left as an inline `TODO` in `colib.h` (~5882-5885):** calling one killer's
trigger function from inside the unwind caused by *another* killer isn't handled - `sig_kill` guards
against being re-entered by *itself* (`killing_activated`), but nothing guards against a *different*
kill triggering mid-unwind. Worth knowing before wiring killers into anything that itself reacts to
being killed.

---

## Semaphore lifetime

`create_sem(pool, val)` returns a `sem_p` (`std::shared_ptr<sem_t>`). Two lifetime facts worth
knowing, both visible directly in `create_sem`'s implementation (colib.h ~4510-4519):

```cpp
inline sem_p create_sem(pool_t *pool, int64_t val) {
    /* We no longer allocate semaphores with the internal allocator,
    because they may survive outside of the pool, and we would not have how
    to de-allocate them anymore, so semaphores need to be allocated with the
    global allocator */
    return std::shared_ptr<sem_t>(new sem_t(pool, val));
}
```

**A `sem_t` object is deliberately *not* allocated through the pool's own bucket allocator** - it's a
plain `new`, wrapped in a `shared_ptr`. The comment states why directly: a `sem_p` can outlive the
`pool_p` it was created from (nothing stops user code from holding onto one), and the pool's allocator
has no way to free something after the pool that owns that allocator is gone. So the `sem_t` object
itself is always memory-safe to hold, regardless of what happens to the pool.

**But the pool still tracks every live semaphore for teardown**, in a `sem_pool` set
(`add_sem`/`rm_sem`, colib.h ~3931-3937), specifically so `pool_t::clear()` can force-clean up any
semaphore that's still alive when the pool goes away - see "Pool lifetime" below. `sem_t::clear(0)`
(and the destructor, which just calls `clear(0)`) destroys every currently-waiting coroutine's *whole
call stack* via `destroy_state()` - this is what the root `README.md`'s Semaphores section means by
"if the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire call
stack)."

**The two-way relationship, and its sharp edge:** when the pool tears down first, it calls
`clear(0)` on every semaphore still in `sem_pool` and then `s->invalidate_self()` (colib.h ~4407-4408,
just `internal = nullptr`) - this marks the `sem_t` as "already handled," so if the semaphore's own
destructor runs later (because a `sem_p` outlived the pool, exactly the scenario the allocator choice
above was designed for), it sees `internal == nullptr` and no-ops instead of double-freeing. **That
only protects the destructor path.** `sem_t::wait()`/`signal()`/`try_dec()`/etc. don't check
`internal` for null - calling any of them on an already-invalidated semaphore dereferences a null
`unique_ptr` directly. `invalidate_self()` turns what would otherwise risk a genuine use-after-free
into a clean null-pointer crash instead - it doesn't make using a post-pool semaphore *correct*, just
memory-safe in the narrow sense of "won't touch freed memory."

---

## Pool lifetime

`create_pool()` returns a `pool_p` (`std::shared_ptr<pool_t>`) - a pool can have multiple owners, and
is destroyed when the last one drops. `pool_t`'s destructor is one line:

```cpp
~pool_t() { clear(); }
```

(colib.h:862.) So destruction always goes through the same `clear()` a caller can also invoke
manually mid-lifetime (e.g. to reset a pool for reuse) - there's no separate "final teardown" code
path. `clear()`'s documented order (the comment directly above `pool_t::clear()`'s declaration,
colib.h ~4068-4084) is:

1. Terminate everything waiting on the I/O pool.
2. Terminate everything waiting on a semaphore (via each live semaphore's own `clear(0)` +
   `invalidate_self()`, as above).
3. Terminate everything sitting in the ready queue (via `destroy_state()` on each entry).

And the general rule stated alongside it: **whenever a coroutine gets terminated this way, its caller
gets terminated too** - which is exactly what `destroy_state()`'s `caller_state` walk does at each of
those three stages, so a coroutine parked on I/O with three levels of callers above it takes the whole
chain down, not just itself.

**Member declaration order matters here.** `pool_t` declares `allocator_memory` before `internal`
(colib.h:925, 931):

```cpp
std::unique_ptr<allocator_memory_t> allocator_memory;
std::unique_ptr<pool_internal_t> internal;
```

C++ destroys members in reverse declaration order, so `internal` (which owns `ready_tasks`, the I/O
pool, the timer pool, and `sem_pool`) is destroyed *before* `allocator_memory`. Combined with `clear()`
running in the destructor's body - i.e. before *any* member destructor runs at all - everything that
was allocated through the pool's own bucket allocator (modif tables, kill-state/timeout-state blocks,
semaphore wait-list nodes, the IO backends' internal maps, ...) gets freed while `allocator_memory` is
still alive to receive it, and only after all of that is `allocator_memory` itself torn down. This
ordering is exactly what a pool-allocator use-after-free would violate - see `tests/018-010` below for
a concrete case where a *different* object's pool-affinity (not this ordering itself) caused exactly
that failure mode.

**One clarification worth stating plainly: coroutine frames themselves are *not* allocated through
this custom allocator.** `colib.h` doesn't override `operator new`/`operator delete` on any promise
type, so coroutine frames come from the ordinary C++ runtime allocator, same as any other coroutine in
any other codebase. The pool's bucket allocator is used for the library's own internal bookkeeping
structures around those frames (modif tables, wait-list nodes, kill/timeout state, ...), not the
frames themselves.

---

## Modification lifetime

A `modif_p` (`std::shared_ptr<modif_t>`, from `create_modif<...>()`) can be attached to many
coroutines' `modif_table_t` at once - each table just holds a copy of the same `shared_ptr` in the
appropriate per-type vector (colib.h ~2450). There's no ownership tension here: a `modif_t` stays
alive for exactly as long as at least one coroutine's table still references it, and disappears
automatically, via ordinary reference counting, once none do. `rm_modifs`/`rm_modifs_from_table`
(colib.h ~5698-5719) just erase matching `modif_p` entries from a table's vectors - detaching a
coroutine from a modification doesn't destroy the modification, only that one reference to it.

**This is what lets `create_killer`/`create_timeo`/`create_future`/`dbg_create_tracer` tie private
per-instance state directly to a modification's lifetime, for free.** Each of these builds its own
small state struct (e.g. `create_killer`'s `kill_state_t`), wraps it in a `shared_ptr`, and captures
that `shared_ptr` *by value* inside the lambda stored in each `modif_t` it creates (see ch.03's Flow
Control notes, or `create_killer`'s implementation above). As long as any coroutine's table still
references one of those modifs, the captured state stays alive; once the last reference is gone, the
lambda - and everything it captured - is torn down along with it. No explicit cleanup function is
needed anywhere in this chain.

**`modif_t` itself is deliberately *not* pool-bound, and this is a fixed bug, not just a design
choice - `tests/018-010-reproduced_allocator_deallocate_uaf.cpp` is the regression test for it.**
`modif_t` used to be allocated through the pool's own `allocator_t<T>` (like `modif_table_t` still
is), which holds a raw `pool_t*` captured at allocation time - not a `pool_p`, so holding a
pool-allocated object does nothing to keep its pool alive. A `modif_p` is explicitly meant to be
created once and reused across many `pool->sched(task, {mod})` calls, potentially spanning pools with
different lifetimes - exactly the same "may outlive what allocated it" shape as `sem_p` above. If the
pool died first and the `modif_p` was freed afterward, `allocator_t<T>::deallocate()` would
dereference that dangling `pool_t*` to decide how to free the block - a real use-after-free. The fix
was structural, not a null-check: `create_modif()` no longer takes a pool parameter, and `modif_t` is
now allocated with plain `std::make_shared<modif_t>(...)`. There is no longer a `pool_t*` anywhere in
a `modif_t` for anything to dereference - the whole class of bug is gone by construction, not
guarded against.

**The asymmetry worth remembering:** `modif_table_t` (the per-coroutine *container*) is still
allocator-backed (colib.h ~5712-5713, `add_modifs`) and that's fine, because a table is always owned
1:1 by one `state_t`, which never outlives its own pool. `modif_t` (the reusable *definition* held in
a `modif_p`, potentially shared across many coroutines and, now, safely across many pools) is not
allocator-backed, for the opposite reason - it's specifically designed to be able to outlive any one
pool. `sem_t` followed the identical reasoning earlier, for the identical reason, per its own comment
quoted above; `modif_t`'s version of that fix just landed later, as an actual bug rather than
foresight.
