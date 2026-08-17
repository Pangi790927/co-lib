# 04. Lifetimes: Coroutine Frames, Killers, Semaphores, the Pool, and Modifications

`03_execution_model.md` covers how control moves between coroutines. This chapter is about how long
things actually live: when a coroutine's frame is destroyed, the two different mechanisms the library
uses to tear a whole call stack down at once, and the lifetimes of the three long-lived objects
everything else is scoped to - semaphores, the pool itself, and modifications.

---

## Coroutine frames: when they actually go away

This builds directly on `03_execution_model.md`'s "Call" section - the short version, restated for
context: a callee's coroutine frame is destroyed by its **caller**, inside `task<T>::await_resume()`,
and *only* on a real `co_return` - `h.destroy()` runs unless
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

(`destroy_state()`.) This is the library's simplest teardown primitive: starting from a given
`state_t`, walk `caller_state` upward - callee, then its caller, then *that* caller, and so on - firing
`EXIT` and calling `.destroy()` on every frame in the chain, unconditionally. It doesn't check where a
frame currently "is" (ready queue, mid-wait, whatever) - it assumes the frame is already just inert
data sitting somewhere the caller already knows about, not something that needs to be located or
un-parked first.

That assumption is exactly why every call site for `destroy_state()` is a place that already has
direct ownership of the frame in question, not a "reach in and find it" scenario:

- **A semaphore's own wait list**, on `clear()`/destructor (`sem_internal_t::clear()`,
  `sem_t::~sem_t()`) - the semaphore already holds the waiter's `state_t*` directly in
  `waiting_on_sem`.
- **The IO backends' own waiter maps**, on force-awake/force-close paths (`io_pool_t::clear()`, in
  both the epoll and the IOCP backend) - same reasoning, the backend already has the `state_t*` on
  hand.
- **The pool's ready queue**, on `pool_t::clear()` (`pool_internal_t::clear()`) - `ready_tasks` is a
  plain list of `state_t*`.

In every one of these, whatever's being destroyed was already parked in one specific place its owner
directly controls - which is what makes the unconditional walk-and-destroy safe.

---

## `create_killer()`: cancelling a call chain from outside it

A killer exists for a case `destroy_state()` doesn't cover at all: stopping some coroutine's whole
in-flight call chain **from outside**, as an unrelated third party - not as the coroutine returning,
not as its direct caller resuming it, but some other code deciding "discard this now." `create_timeo`
is the concrete example already covered in ch.03: a timer coroutine races the real work, and if the
timer wins, it calls a killer's trigger on the real work instead of waiting for it any longer.

`create_killer(pool, e)` returns a `{modif_pack_t, trigger_fn}` pair. Attaching
the pack to a coroutine (`CO_MODIF_INHERIT_ON_CALL`) makes every coroutine it calls (and everything
*those* call, and so on) maintain a shared, private `call_stack` - pushed on `CALL`/`SCHED`, popped on
`EXIT` - plus, whenever the current innermost frame is waiting, which semaphore or I/O it's waiting on.
Calling the trigger (`sig_kill`) tears down `call_stack.top()` - always the *currently innermost*
frame of the tracked chain, whichever coroutine that happens to be at the moment of the call - and
everything above it.

**This only makes sense because of how coroutines here are scheduled: at the moment a kill triggers,
the innermost frame is always parked in exactly one of three places** - the ready queue, a semaphore's
wait list, or an I/O wait - never "mid-execution" somewhere a kill could catch it half-finished,
because only one coroutine ever actually executes at a time and a trigger can only run while the
target itself isn't. `sig_kill` handles exactly those three cases and no others.

**What happens to whatever it was waiting on:** if it was parked on a semaphore, it's erased from that
semaphore's own wait list (`erase_waiter`) - the semaphore is left in a consistent state, as if that
waiter had never queued. If it was parked on I/O, `stop_io()` cancels the pending registration with
the OS backend cleanly. Either way, the usual `ENTER` → `UNWAIT_SEM`/`UNWAIT_IO` → `LEAVE` modifs are
replayed by hand first, so the coroutine's own modif bookkeeping ends up exactly where a real wakeup
would have left it, even though nothing actually resumed it.

**Destruction order is innermost-first, working outward - the reverse of call order, the same way
local-variable destructors unwind a normal C++ stack.** `call_stack.top()` (the currently-innermost
frame) is destroyed first, then its caller, then *that* caller, and so on outward - `while
(call_stack.size() > 1) { destroy top; }` (`sig_kill`). The one exception is the outermost
frame, the root the killer was originally attached to: if it has no `caller_state` (it was `sched`'d),
it's destroyed too, since nothing else is waiting on it. If it *does* have a `caller_state` (it was
itself `call`ed by something outside the killed chain), it's **not** destroyed - its `err` is set to
`e` and its caller is resumed instead, the same "caller owns callee cleanup" contract from ch.03's
"Call" section. A killer never destroys a frame it doesn't own the destruction rights to.

**Killing a task that already finished on its own is safe, not a use-after-free.** `sig_kill` starts
with `if (call_stack.size() == 0) return ERROR_GENERIC;` - once every tracked
frame has exited normally, the stack is empty, and the trigger just reports failure instead of
touching anything. A killer's trigger function is a plain `std::function` the caller can hold onto and
call at any time, including long after the coroutine it targets is already gone.

**Calling one killer's trigger function from inside the unwind caused by a *different* killer is
undefined behavior** (inline `TODO` in `create_killer`). `sig_kill` only guards against being
re-entered by *itself* (`killing_activated`); a different kill triggering mid-unwind isn't handled at
all.

---

## Semaphore lifetime

`create_sem(pool, val)` returns a `sem_p` (`std::shared_ptr<sem_t>`). Two lifetime facts worth
knowing, both visible directly in `create_sem`'s implementation:

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
itself is always memory-safe to *hold* and let destruct, regardless of what happens to the pool -
that's specifically what this choice buys. It does **not** mean the semaphore is safe to *use* once
its pool is gone: the pool tears down `sem_internal_t` - the actual counter, wait list, everything
`wait()`/`signal()`/etc. touch - along with itself. Only outliving the pool with the object sitting
unused, then letting it destruct, is the safe case; calling into it after the pool has died is UB
regardless (see the null-`internal` sharp edge below).

**But the pool still tracks every live semaphore for teardown**, in a `sem_pool` set
(`add_sem`/`rm_sem`), specifically so `pool_t::clear()` can force-clean up any
semaphore that's still alive when the pool goes away - see "Pool lifetime" below. `sem_t::clear(0)`
(and the destructor, which just calls `clear(0)`) destroys every currently-waiting coroutine's *whole
call stack* via `destroy_state()` - this is what the root `README.md`'s Semaphores section means by
"if the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire call
stack)."

**The two-way relationship, and its sharp edge:** when the pool tears down first, it calls
`clear(0)` on every semaphore still in `sem_pool` and then `s->invalidate_self()` (just
`internal = nullptr`) - this marks the `sem_t` as "already handled," so if the semaphore's own
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

(`pool_t::~pool_t()`.) So destruction always goes through the same `clear()` a caller can also invoke
manually mid-lifetime (e.g. to reset a pool for reuse) - there's no separate "final teardown" code
path. `clear()`'s documented order (the comment directly above `pool_t::clear()`'s declaration) is:

1. Terminate everything waiting on the I/O pool.
2. Terminate everything waiting on a semaphore (via each live semaphore's own `clear(0)` +
   `invalidate_self()`, as above).
3. Terminate everything sitting in the ready queue (via `destroy_state()` on each entry).

And the general rule stated alongside it: **whenever a coroutine gets terminated this way, its caller
gets terminated too** - which is exactly what `destroy_state()`'s `caller_state` walk does at each of
those three stages, so a coroutine parked on I/O with three levels of callers above it takes the whole
chain down, not just itself.

**Member declaration order matters here.** `pool_t` declares `allocator_memory` before `internal`:

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
appropriate per-type vector (`modif_table_t`). There's no ownership tension here: a `modif_t` stays
alive for exactly as long as at least one coroutine's table still references it, and disappears
automatically, via ordinary reference counting, once none do. `rm_modifs`/`rm_modifs_from_table`
just erase matching `modif_p` entries from a table's vectors - detaching a
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
allocator-backed (`add_modifs`) and that's fine, because a table is always owned
1:1 by one `state_t`, which never outlives its own pool. `modif_t` (the reusable *definition* held in
a `modif_p`, potentially shared across many coroutines and, now, safely across many pools) is not
allocator-backed, for the opposite reason - it's specifically designed to be able to outlive any one
pool. `sem_t` followed the identical reasoning earlier, for the identical reason, per its own comment
quoted above; `modif_t`'s version of that fix just landed later, as an actual bug rather than
foresight.
