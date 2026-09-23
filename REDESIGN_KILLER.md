# Killer redesign

A proposal. Nothing in it has been applied: `colib.h`, the tests and `docs/` are unchanged. Every
code change is written out in full below, for review and for you to apply by hand. Code is
referenced by symbol, never by `colib.h` line number (same rule as `tests/CLAUDE.md`).

Contents:

1. [The problem](#1-the-problem)
2. [The principles](#2-the-principles)
3. [What changes, at a glance](#3-what-changes-at-a-glance)
4. [Engine changes](#4-engine-changes) (generic, not killer specific)
5. [The killer](#5-the-killer) (everything killer specific, in one place)
6. [The lazy drop: variant A or B](#6-the-lazy-drop-variant-a-or-b) (open decision)
7. [`create_timeo`](#7-create_timeo)
8. [Behaviour table](#8-behaviour-table)
9. [Tests](#9-tests)
10. [Documentation to update](#10-documentation-to-update)
11. [Open points and known limits](#11-open-points-and-known-limits)

---

## 1. The problem

`co::create_timeo(co::read(h, buff, len), pool, timeo)` loses data on Windows.

On IOCP, `co::read` issues an overlapped `ReadFile` before it waits. The kernel moves the bytes
from the socket into `buff` when the request completes, which can happen before the coroutine is
resumed. If the timer's kill reaches the reader in that window, the result is thrown away:

- **A. The completion was already dequeued.** `handle_ready_events()` stored `recvlen` and queued
  the reader, but it hasn't run yet. `sig_kill` removes it from the ready queue and destroys its
  call stack. The bytes are in `buff`, the count is gone.
- **B. The request completed while being cancelled.** The reader is still registered, so `sig_kill`
  calls `stop_io()`, which goes to `io_pool_t::force_awake()`. `CancelIoEx` finds nothing to cancel,
  or cancels too late. `GetOverlappedResult`'s byte count is ignored, the request's own completion
  packet is drained and dropped (`handle_ready_events(NULL, data)` filters it), and the coroutine is
  woken with `ERROR_WAKEUP` and destroyed.

In both cases the caller sees `ERROR_TIMEO` and the bytes are lost. On a stream that means a
corrupted stream.

epoll and kqueue don't have this problem: they only wait for readiness, and the `read()` syscall
runs after the resume. Killing a coroutine that became ready but hasn't run leaves the data in the
socket.

The same flaw exists for semaphores on every platform. `signal()` takes the count down and queues
the waiter (`sem_internal_t::_awake_one`). A kill that finds that waiter in the ready queue removes
and destroys it, so the token is consumed and nobody received it.

**Reproduced** with a standalone program, no mint code: a thread sends 3000 single bytes at 0-3ms
intervals to a loopback socket, and a coroutine reads them in a loop through
`create_timeo(co::read(...), 5ms)`. Without the timeout all 3000 bytes arrived. With it, 1224
arrived: 1776 bytes were lost, close to the number of timeouts (1810). One of the two runs with the
timeout also crashed with an access violation, which is not explained yet (see 11). Test
`018-015` below turns this into the reproduce-first regression test.

Found through bbb_repo's `ssh_tunnel_loop_coro` (`src/mint/common/mint_pc_helpers.h`), which reads
the ssh client socket with a 5ms `create_timeo` in a loop.

---

## 2. The principles

colib's time model: code runs in zero time, only async operations take time. On top of it, two
promises that both must hold:

- **P1: when the kill function returns normally, the target does nothing more.** Its frames are
  destroyed, or it had already finished.
- **P2: a wait that completed is never thrown away.** If the operation had an effect (bytes moved,
  a token taken), the code after the `co_await` receives it.

Every wait ends in exactly one of two ways: **completed** (it had its effect and the coroutine gets
the result) or **cancelled** (it had no effect at all). "Effect happened, result dropped" is the
bug.

What the kill does depends only on what the target is doing when the kill is called:

- **Waiting**, and the wait can be cancelled with no effect: cancel it, destroy the chain now. This
  is today's behaviour.
- **Runnable with an effect**: its wait completed, or on Windows the cancel said "already finished".
  In the time model the coroutine is already running, zero time, until its next wait. So the
  killer resumes it right there, inside the kill ("drives" it). At its next wait it is parked
  before that wait does anything (no request issued, no token taken, no yield), and the killer
  destroys the chain. P1 holds because this all happens inside the kill call. P2 holds because the
  code after the completed `co_await` ran.
- **Executing**: the kill comes from inside the chain itself (a self-kill, or a kill called by
  code the chain runs). It can't be done now: when the kill returns, the target is the code that
  continues. The chain is marked; it dies at its next wait, the same way. The kill function throws
  `kill_incomplete_t` to say its promise couldn't be kept. Catching it means "I accept the lazy
  death"; not catching it unwinds the caller's own chain (for a self-kill, that is the target
  itself).

The stop point: **the next place where the coroutine would actually suspend** (an io wait, a
semaphore wait that has no token, `yield`, `force_stop`), **before that wait has any effect**. Calls
are zero time, they are not stop points. A semaphore wait that finds a token doesn't suspend, the
code after it gets the token, so it isn't a stop point either.

A chain whose root returns (or throws out) while being driven completed its task in time: there's
nothing left to kill, the kill function returns `ERROR_FINISHED`, and the root's caller (if any) is
queued instead of being resumed from inside the kill.

A kill called again from inside its own unwind (a destructor of a frame being destroyed, see
`018-006`) returns `ERROR_GENERIC`, as today: that kill is already being done. No throw there,
destructors can't throw.

---

## 3. What changes, at a glance

| Part | Change | Why |
|---|---|---|
| `error_e` | `ERROR_FORCE_SUSPEND = 2`, `ERROR_FINISHED = 3` | modif control code; kill result |
| `modif_e` | `CO_MODIF_WAIT_YIELD_CBK` | `yield`/`force_stop` are suspension points too |
| `do_generic_modifs` | `ERROR_FORCE_SUSPEND` never stops the dispatch; wait/unwait/yield callbacks all always run | a park must win over another modif's failure; every WAIT gets its UNWAIT |
| `io_awaiter_t`, `sem_awaiter_t` | park on `ERROR_FORCE_SUSPEND`; aborted waits get their UNWAIT | park path; pairing |
| `yield_awaiter_t`, `force_stop` | call the yield callbacks; park / refuse | stop point |
| `task<T>::await_suspend` | park the callee on `ERROR_FORCE_SUSPEND` | uniform rule |
| `final_awaiter_cleanup`, `cpp_yield_awaiter` | `ERROR_FORCE_SUSPEND` from the exit callbacks: don't jump into the caller | root finishing while driven |
| Windows `io_pool_t::force_awake` | `keep_completed`: a request that finished during the cancel is delivered, `ERROR_FINISHED` | problem B |
| `pool_internal_t` | `is_ready()`, `stop_io(..., keep_completed)`, posted lists instead of single slots | killer; a drive can finish several roots in one resume |
| debug checks | `dbg_check_modif_wait_yield` | new callback type |
| killer | rewritten, all of it in `killer_state_t` | the new behaviour |
| `create_timeo` | one decision point | the timer and the task can't kill each other anymore while one drives the other |

Behaviour changes visible from outside: a kill of an already finished target returns
`ERROR_FINISHED` instead of `ERROR_GENERIC`; a kill can throw `kill_incomplete_t`; an aborted wait
now also runs the UNWAIT callbacks; the `WAIT_*`/`UNWAIT_*` callbacks no longer stop at the first
error.

---

## 4. Engine changes

### 4.1 `error_e`

```cpp
/*! Most of the functions from this library return this error type. Warnings or non-errors are
 * positive, while errors are negative. */
enum error_e : int32_t {
    ERROR_FINISHED      =  3, /*!< not an error: a killer found its target already done, or the
                                 target's root returned while the killer was resuming it */
    ERROR_FORCE_SUSPEND =  2, /*!< not an error: a modif callback asks colib to park the coroutine
                                 right here, see modif_e */
    ERROR_YIELDED =  1, /*!< not really an error, but used to signal that the coro yielded */
    ERROR_OK      =  0,
    ERROR_GENERIC = -1, /*!< generic error, can use log_str to find the error, or sometimes errno */
    ERROR_TIMEO   = -2, /*!< the error comes from a modif, namely a timeout */
    ERROR_WAKEUP  = -3, /*!< the error comes from force awaking the awaiter */
    ERROR_USER    = -4, /*!< the error comes from a modif, namely an user defined modif, users can
                        use this if they wish to return from modif cbks */
    ERROR_DEPEND  = -5, /*!< the error comes from a depend modif, i.e. depended function failed */
};
```

`dbg_enum(error_e)`, add:

```cpp
        case ERROR_FINISHED:      return dbg_string_t{"ERROR_FINISHED",      allocator_t<char>{nullptr}};
        case ERROR_FORCE_SUSPEND: return dbg_string_t{"ERROR_FORCE_SUSPEND", allocator_t<char>{nullptr}};
```

### 4.2 `modif_e`, `modif_t::variant_t`, `modif_table_t`

New value, at the end so existing indices don't move. The doc of the enum also gets the
`ERROR_FORCE_SUSPEND` rule:

```cpp
/*! This is the modification type of the modification and it describes the place that this
 * modification should be called from.
 *
 * ERROR_FORCE_SUSPEND: returned by a callback whose return value is used (call, wait_io, wait_sem,
 * wait_yield, exit), it asks colib to park the coroutine at that point: the operation doesn't
 * happen (no call, no wait registered, no yield), the callbacks already run for it are unwound
 * (wait -> unwait), the leave callbacks run and control goes back to whoever resumed the
 * coroutine (std::noop_coroutine()). The modif that asked owns the coroutine from then on: it must
 * resume or destroy it. From an exit callback it means: don't resume the caller, the modif does.
 * ERROR_FORCE_SUSPEND never stops the other callbacks of the same place from running. */
enum modif_e : int32_t {
    /* ... all existing values unchanged, up to and including CO_MODIF_UNWAIT_SEM_CBK ... */

    /*! This is called before a corutine gives up its turn without waiting on an io or on a
    semaphore: co::yield() and co::force_stop(). If the return value is an error, the corutine
    doesn't yield/stop and continues right away; ERROR_FORCE_SUSPEND parks it instead. */
    CO_MODIF_WAIT_YIELD_CBK,

    CO_MODIF_COUNT,
};
```

`modif_t::variant_t`, one more alternative at the end (the variant index must match `modif_e`):

```cpp
    using variant_t = std::variant<
        std::function<error_e(state_t *)>,              /* call_cbk */
        std::function<error_e(state_t *)>,              /* sched_cbk */
        std::function<error_e(state_t *)>,              /* exit_cbk */
        std::function<error_e(state_t *)>,              /* leave_cbk */
        std::function<error_e(state_t *)>,              /* enter_cbk */
        std::function<error_e(state_t *, io_desc_t&)>,  /* wait_io_cbk */
        std::function<error_e(state_t *, io_desc_t&)>,  /* unwait_io_cbk */

         /* wait_sem_cbk - OBS: the std::shared_ptr<void> part can be ignored, it's internal */
        std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,

         /* unwait_sem_cbk - No handle here, as the semaphore is no longer in the waiting list */
        std::function<error_e(state_t *, sem_t *)>,

        std::function<error_e(state_t *)>               /* wait_yield_cbk */
    >;
```

`modif_table_t`, one more vector (10), plus a guard so this can't silently fall behind again:

```cpp
struct modif_table_t {
    modif_table_t(pool_t *pool)
    : table{
            /* :( - one per modif_e value, see the static_assert below */
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}}
    } {}

    static_assert(CO_MODIF_COUNT == 10, "one vector per modif_e in the constructor above");
    std::array<std::vector<modif_p, allocator_t<modif_p>>, CO_MODIF_COUNT> table;
};
```

### 4.3 Debug checks for the new callback

Next to the other `COLIB_DEBUG_CHECK_*` macros, both branches:

```cpp
#if COLIB_ENABLE_DEBUG_CHECKS
/* ... existing ... */
# define COLIB_DEBUG_CHECK_WAIT_YIELD(s) dbg_check_modif_wait_yield(s)
/* ... existing declarations ... */
inline void dbg_check_modif_wait_yield(state_t *s);
#else /*COLIB_ENABLE_DEBUG_CHECKS*/
/* ... existing ... */
# define COLIB_DEBUG_CHECK_WAIT_YIELD(...) ;
#endif
```

With the other `dbg_check_modif_*` definitions:

```cpp
inline void dbg_check_modif_wait_yield(state_t *s) {
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(s, "no-state");
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(s->pool, "no-pool");
    auto &pool_map = dbg_check_coro_states[s->pool];
    auto &dbg_state = pool_map[s];
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but yield");
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but yield");
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but yield");
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "yield while waiting on io");
    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "yield while waiting on sem");
}
```

The existing checks already fit the park path: `WAIT_x` records the wait, the park path's `UNWAIT_x`
clears it, `LEAVE` sets `left`, and the killer's `EXIT` + destroy come after. They also start
passing for aborted waits, which today leave `dbg_state.io`/`sem` set (no `UNWAIT` on that path).

### 4.4 Dispatch rules and `do_yield_modifs`

```cpp
/* Dispatch rules (see modif_e):
    - ERROR_OK: go on with the next callback
    - ERROR_FORCE_SUSPEND never stops the dispatch; if any callback returned it, it's the result
    - any other error stops the dispatch and is the result (unless a callback before it already
      asked for ERROR_FORCE_SUSPEND) - except for the wait/unwait/yield callbacks, which always all
      run: every modif must see every WAIT and its UNWAIT, and a park must win even if another
      modif failed the same wait */
template <modif_e cbk_id, typename ...Args>
inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
    constexpr bool run_all =
            cbk_id == CO_MODIF_WAIT_IO_CBK || cbk_id == CO_MODIF_UNWAIT_IO_CBK ||
            cbk_id == CO_MODIF_WAIT_SEM_CBK || cbk_id == CO_MODIF_UNWAIT_SEM_CBK ||
            cbk_id == CO_MODIF_WAIT_YIELD_CBK;
    error_e result = ERROR_OK;
    if (auto modif_table = state->modif_table) {
        for (auto &modif : modif_table->table[cbk_id]) {
            error_e ret = std::get<cbk_id>(modif->cbk)(state, args...);
            if (ret == ERROR_OK)
                continue;
            if (ret == ERROR_FORCE_SUSPEND) {
                result = ERROR_FORCE_SUSPEND;
                continue;
            }
            if (result == ERROR_OK)
                result = ret;
            if (!run_all)
                break;
        }
    }
    return result;
}
```

`auto modif_table = state->modif_table` copies the `shared_ptr`, so the table, the modifs and the
running `std::function` stay alive even if a callback destroys `state` (needed by variant A, see 6).

Next to `do_wait_sem_modifs`:

```cpp
inline error_e do_yield_modifs(state_t *state) {
    COLIB_DEBUG_TRACE("   YIELD: %s state: %p", dbg_name(state->self).c_str(), state);
    COLIB_DEBUG_CHECK_WAIT_YIELD(state);
    return do_generic_modifs<CO_MODIF_WAIT_YIELD_CBK>(state);
}
```

### 4.5 `io_awaiter_t`

Only `await_suspend` changes:

```cpp
    template <typename P>
    handle<void> await_suspend(handle<P> h) {
        COLIB_DEBUG_TRACE_SCOPE("io-wait state: %p", &h.promise().state);

        auto pool = h.promise().state.pool;
        state = &h.promise().state;

        error_e err = do_wait_io_modifs(state, io_desc);
        if (err == ERROR_FORCE_SUSPEND) {
            /* A modif parks the coroutine here (ex: a killer, see create_killer): nothing gets
            registered, the wait callbacks are unwound and the coroutine leaves. The modif owns it
            from now on and may even destroy it inside the leave callbacks, so nothing after
            do_leave_modifs() may touch `this` or `state`. */
            do_unwait_io_modifs(state, io_desc);
            do_leave_modifs(state);
            return std::noop_coroutine();
        }
        if (err != ERROR_OK) {
            /* a modif aborted the wait: the coroutine continues right away, with the error */
            ret_err = err;
            do_unwait_io_modifs(state, io_desc);
            return h;
        }

        ret_err = pool->get_internal()->wait_io(h, io_desc);
        if (ret_err != ERROR_OK) {
            COLIB_DEBUG("Failed to register wait: %s on: %s",
                    dbg_enum(ret_err).c_str(), dbg_name(h).c_str());
            do_unwait_io_modifs(state, io_desc);
            return h;
        }
        do_leave_modifs(state);
        triggered = true;
        return pool->get_internal()->next_task();
    }
```

`await_resume` is unchanged: on both early paths `triggered` stays false, so it doesn't replay
ENTER/UNWAIT.

### 4.6 `sem_awaiter_t`

Only `await_suspend` changes. `push_waiter` runs before the callbacks, so both early paths remove
the waiter again; no token was taken (`await_ready` returned false):

```cpp
    template <typename P>
    handle<void> await_suspend(handle<P> to_suspend) {
        COLIB_DEBUG_TRACE_SCOPE("sem-wait state: %p", &to_suspend.promise().state);

        state = &to_suspend.promise().state;

        auto pool = sem->get_internal()->get_pool();
        psem_it = sem->get_internal()->push_waiter(state);

        error_e err = do_wait_sem_modifs(state, sem, psem_it);
        if (err != ERROR_OK) {
            sem->get_internal()->erase_waiter(*psem_it);
            do_unwait_sem_modifs(state, sem);
            if (err == ERROR_FORCE_SUSPEND) {
                /* parked by a modif, see io_awaiter_t: nothing may touch `this` after this */
                do_leave_modifs(state);
                return std::noop_coroutine();
            }
            COLIB_DEBUG_TRACE("User stopped wait on semaphore: state[%p] sem[%p]", state, sem);
            return to_suspend;
        }
        do_leave_modifs(state);

        await_state = AWAITER_SUSPEND_LAST;
        return pool->get_internal()->next_task();
    }
```

`await_resume` is unchanged; on the aborted path `await_state` stays `AWAITER_NOT_CALLED`, which
keeps `018-008`'s null unlocker.

### 4.7 `yield_awaiter_t`

```cpp
struct yield_awaiter_t {
    yield_awaiter_t() {}
    yield_awaiter_t(const yield_awaiter_t &oth) = delete;
    yield_awaiter_t &operator = (const yield_awaiter_t &oth) = delete;
    yield_awaiter_t(yield_awaiter_t &&oth) = delete;
    yield_awaiter_t &operator = (yield_awaiter_t &&oth) = delete;

    bool await_ready() { return false; }

    template <typename P>
    inline handle<void> await_suspend(handle<P> h) {
        COLIB_DEBUG_TRACE_SCOPE("yield state: %p", &h.promise().state);

        auto pool = h.promise().state.pool;
        state = &h.promise().state;

        error_e err = do_yield_modifs(state);
        if (err == ERROR_FORCE_SUSPEND) {
            /* parked by a modif, see io_awaiter_t: nothing may touch `this` after this */
            do_leave_modifs(state);
            return std::noop_coroutine();
        }
        if (err != ERROR_OK)
            return h;       /* a modif refused the yield: continue right away */

        do_leave_modifs(state);
        pool->get_internal()->push_ready(state);
        triggered = true;
        // TODO: is it required to call the modifs here if the returned coroutine is the same as
        // this one or does c++ call resume either way?
        return pool->get_internal()->next_task();
    }

    void await_resume() {
        COLIB_DEBUG_TRACE_SCOPE("yield state: %p", state);
        if (triggered)
            do_entry_modifs(state);
    }

    state_t *state = nullptr;
    bool triggered = false;
};
```

### 4.8 `force_stop`'s `stop_awaiter_t`

`force_stop` is a suspension point like `yield` (it queues itself at the front and stops the pool),
so it gets the same callbacks:

```cpp
    struct stop_awaiter_t {
        stop_awaiter_t(int64_t stopval) : stopval(stopval) {}

        bool await_ready() { return false; };

        handle<void> await_suspend(handle<task_state_t<int>> h) {
            COLIB_DEBUG_TRACE_SCOPE("force_stop state: %p", &h.promise().state);

            state = &h.promise().state;

            error_e err = do_yield_modifs(state);
            if (err == ERROR_FORCE_SUSPEND) {
                /* parked by a modif, see io_awaiter_t: the pool isn't stopped, nothing may touch
                `this` after this */
                do_leave_modifs(state);
                return std::noop_coroutine();
            }
            if (err != ERROR_OK)
                return h;   /* a modif refused the stop: continue right away */

            do_leave_modifs(state);
            state->pool->get_internal()->push_ready_front(state);
            state->pool->get_internal()->ret_val = RUN_STOPPED;
            state->pool->get_internal()->post_stop();
            state->pool->stopval = stopval;
            triggered = true;
            return std::noop_coroutine();
        }

        error_e await_resume() {
            COLIB_DEBUG_TRACE_SCOPE("force_stop state: %p", state);

            if (triggered)
                do_entry_modifs(state);
            return ERROR_OK; /* no errors to be had here */
        }

        int64_t stopval = 0;
        state_t *state = nullptr;
        bool triggered = false;
    };
```

### 4.9 `task<T>::await_suspend` (call)

```cpp
template <typename T>
template <typename P>
inline handle<void> task<T>::await_suspend(handle<P> caller) noexcept {
    COLIB_DEBUG_TRACE_SCOPE("caller state: %p", &caller.promise().state);
    COLIB_DEBUG_TRACE("callee state: %p", &h.promise().state);

    do_leave_modifs(&caller.promise().state);
    ever_called = true;

    state_t *state = &h.promise().state;
    state->caller_state = &caller.promise().state;
    state->pool = caller.promise().state.pool;

    inherit_modifs(state, caller.promise().state.modif_table, CO_MODIF_INHERIT_ON_CALL);

    error_e err = do_call_modifs(state);
    if (err == ERROR_FORCE_SUSPEND) {
        /* the callee is parked before its first instruction and the caller (already left)
        waits on it: the modif that asked owns both from now on */
        return std::noop_coroutine();
    }
    if (err != ERROR_OK) {
        do_entry_modifs(&caller.promise().state);
        return caller;
    }

    do_entry_modifs(state);
    return h;
}
```

The killer never answers `ERROR_FORCE_SUSPEND` on a call (calls take zero time). This is only the
uniform rule. The existing error path, and `tests/BUGS.md` #5 (double ENTER), are unchanged.

### 4.10 `pool_internal_t::sched`

No code change. `do_sched_modifs()` failing already means "not queued", and that is also what
`ERROR_FORCE_SUSPEND` means there. Worth one comment line:

```cpp
        /* ... call the sched modifs (an error, ERROR_FORCE_SUSPEND included, means: not queued,
        the modif that refused owns the task) */
        if (do_sched_modifs(state) != ERROR_OK) {
            return ;
        }
```

### 4.11 `final_awaiter_cleanup`, `cpp_yield_awaiter`

The exit callbacks' return value is ignored today. It now can say "don't resume the caller, I do".
The killer needs this when a chain it drives returns from its root: it must not jump into the
root's caller, which is outside the chain, from inside the kill.

```cpp
inline handle<void> cpp_yield_awaiter(state_t *yielding_task_state) {
    COLIB_DEBUG_TRACE_SCOPE("state: %p", yielding_task_state);
    /* bassicaly the same as bellow, except we don't destroy the corutine */
    do_leave_modifs(yielding_task_state);

    yielding_task_state->err = ERROR_YIELDED;
    state_t *caller_state = yielding_task_state->caller_state;

    /* from the point of view of the corutine modifications we are exiting here, this keeps the
    call stack proper */
    error_e exit_err = do_exit_modifs(yielding_task_state);

    if (caller_state) {
        if (exit_err == ERROR_FORCE_SUSPEND)
            return std::noop_coroutine();   /* a modif (ex: a killer) resumes the caller */
        return caller_state->self;
    }

    return yielding_task_state->pool->get_internal()->next_task();
}

inline handle<void> final_awaiter_cleanup(state_t *ending_task_state) {
    COLIB_DEBUG_TRACE_SCOPE("state: %p", ending_task_state);
    do_leave_modifs(ending_task_state);
    state_t *caller_state = ending_task_state->caller_state;
    error_e exit_err = do_exit_modifs(ending_task_state);

    if (caller_state) {
        if (exit_err == ERROR_FORCE_SUSPEND)
            return std::noop_coroutine();   /* a modif (ex: a killer) resumes the caller */
        return caller_state->self;
    }
    auto pool = ending_task_state->pool;
    pool->get_internal()->post_to_destroy(ending_task_state);
    /* If the task that we are final_awaiting has no caller, then it is the moment to destroy it,
    no one needs it's return value. Else it will be destroyed by the caller. */
    if (ending_task_state->exception) {
        pool->get_internal()->post_exception(ending_task_state->exception);
        return std::noop_coroutine();
    }
    return std::noop_coroutine();
}
```

Without a caller nothing changes: the root is still posted for destruction.

### 4.12 `io_pool_t::force_awake`, all backends

New parameter `keep_completed` (default `false`, only killers pass `true`).

**Windows (IOCP)**, the whole function:

```cpp
    /* the state (singular) that is waiting for io_desc must be awakened. With keep_completed (for
    killers, see create_killer): a request that completed before the cancel took effect already
    had its effect (bytes moved), so it's delivered as a normal completion instead of `retcode`,
    and ERROR_FINISHED is returned. */
    error_e force_awake(const io_desc_t& io_desc, error_e retcode, bool keep_completed = false) {
        COLIB_ENABLE_DEBUG_CHECK_ASSERT(io_desc.h, "invalid handle");
        COLIB_DEBUG_TRACE("handle: %p", io_desc.h);
        auto awake_data = [this, retcode, keep_completed](
                std::shared_ptr<io_data_t> data) -> error_e
        {
            COLIB_ENABLE_DEBUG_CHECK_ASSERT(data, "invalid data ptr");
            COLIB_ENABLE_DEBUG_CHECK_ASSERT(data->h, "invalid inner handle");
            COLIB_DEBUG_TRACE("awake: handle: %p", data->h);
            bool finished = false;
            DWORD transferred = 0;
            if ((data->flags & io_data_t::IO_FLAG_TIMER) &&
                    (data->flags & io_data_t::IO_FLAG_TIMER_RUN))
            {
                COLIB_DEBUG_TRACE("kill timer: handle: %p", data->h);
                if (kill_timer(data->h) != ERROR_OK) {
                    COLIB_DEBUG("Failed to stop a timer");
                    return ERROR_GENERIC;
                }
                data->flags = io_data_t::io_flag_e(data->flags & ~io_data_t::IO_FLAG_TIMER_RUN);
            }
            else {
                BOOL cancelled = CancelIoEx(data->h, &data->overlapped);
                if (!cancelled && GetLastError() != ERROR_NOT_FOUND) {
                    COLIB_DEBUG("Failed to cancel io: %s [%x] h: %p",
                            get_last_error().c_str(), GetLastError(), data->h);
                    return ERROR_GENERIC;
                }
                /* CancelIoEx is async, GetOverlappedResult waits until the request is really over.
                ERROR_NOT_FOUND means it was already over: it completed. Either way, only a request
                that ended successfully (not cancelled, not failed) moved data. */
                if (cancelled || keep_completed) {
                    BOOL ok = GetOverlappedResult(data->h, &data->overlapped, &transferred, TRUE);
                    finished = keep_completed && ok;
                }
            }

            /* If events where queued we need to handle them here, that is so
            we can safely free `data` */
            /* We also filter for data, so we don't queue it twice */
            if (handle_ready_events(NULL, data.get()) != ERROR_OK) {
                COLIB_DEBUG("Failed to get new events after io-cancel");
                return ERROR_GENERIC;
            }

            if (finished) {
                data->recvlen = transferred;
                data->state->err = ERROR_OK;
                awake_io(data.get());
                return ERROR_FINISHED;
            }
            data->state->err = retcode;
            awake_io(data.get());

            return ERROR_OK;
        };
        if (!io_desc.data) {
            /* the whole handle: keep_completed isn't used here (killers always stop one request) */
            if (!has(handles, io_desc.h))
                return ERROR_OK;
            std::vector<std::shared_ptr<io_data_t>> datas;
            auto it = handles.find(io_desc.h);
            for (auto &data : it->second) {
                datas.push_back(data);
            }
            for (auto data : datas) {
                error_e err;
                if ((err = awake_data(data)) < 0)
                    return err;
            }
        }
        else {
            return awake_data(io_desc.data);
        }

        return ERROR_OK;
    }
```

The handle-wide loop now checks `< 0` instead of `!= ERROR_OK`, so the positive `ERROR_FINISHED`
isn't mistaken for a failure. That can't happen today, since the handle-wide path never keeps
completions, but the check shouldn't depend on it.

**Linux (epoll)**: only the signature, plus a note:

```cpp
    error_e force_awake(const io_desc_t& io_desc, error_e retcode, bool keep_completed = false) {
        /* epoll only waits for readiness, the syscall runs after the resume: a wait never has an
        effect before its coroutine runs, there is nothing to keep */
        (void)keep_completed;
        /* ... body unchanged ... */
    }
```

**kqueue (`COLIB_OS_UNIX`)**: same signature. The body is still the unimplemented stub from
`tests/BUGS.md` #4, and this redesign doesn't fix that:

```cpp
    error_e force_awake(const io_desc_t& io_desc, error_e retcode, bool keep_completed = false) {
        /* TODO: figure it out, for this and for the others, maybe I can find a way not to use
        a map */
    }
```

**`COLIB_OS_UNKNOWN` template** (the commented-out skeleton): the same signature in the comment.

### 4.13 `pool_internal_t`

`stop_io` passes the flag through:

```cpp
    error_e stop_io(const io_desc_t& io_desc, error_e retcode, bool keep_completed = false) {
        /* stop the io and set it's return code as retcode (keep_completed: see
        io_pool_t::force_awake, killers only) */
        return io_pool.force_awake(io_desc, retcode, keep_completed);
    }
```

`pool_t::stop_io()` and `co::stop_io()` keep calling it with two arguments, so the public API is
unchanged.

A lookup that doesn't remove, next to `remove_ready`:

```cpp
    bool is_ready(state_t *state) {
        return std::find(ready_tasks.begin(), ready_tasks.end(), state) != ready_tasks.end();
    }
```

**The posted slots become lists.** Today `posted_to_destroy` and `posted_exception` hold one entry
each, which is enough because one `resume()` from `run()` can finish at most one scheduled root.
With the killer's drive it can finish several: the timer finishes the driven root, and then its own.
The second post would overwrite the first, and that frame would leak.

```cpp
    void post_to_destroy(state_t *s) {
        posted_to_destroy.push_back(s);
    }

    void post_exception(std::exception_ptr exc) {
        if (!posted_exception)
            posted_exception = exc;     /* the first one wins, the rest would be lost anyway */
    }

    /* variant B only (see section 6) */
    void post_after_resume(std::function<void(void)> fn) {
        posted_after_resume.push_back(std::move(fn));
    }

    /* Work that couldn't be done from inside the coroutine that asked for it. Several can pile
    up in one resume, since a killer resumes other coroutines from inside the current one. Doing
    one can post more (destructors run), so this loops until nothing is left. */
    void run_posted() {
        while (posted_after_resume.size() || posted_to_destroy.size()) {
            auto fns = std::move(posted_after_resume);
            posted_after_resume.clear();
            for (auto &fn : fns)
                fn();
            auto to_destroy = std::move(posted_to_destroy);
            posted_to_destroy.clear();
            for (auto s : to_destroy)
                s->self.destroy();
        }
    }
```

In `run()`:

```cpp
        while (true) {
            state = next_task_state();
            if (state == nullptr)
                return RUN_OK;

            /* whenever we resume, we set the ret_val to aborted for any error that may happen, as
            such we set the ret_val value to aborted. This value will be replaced if needed before
            stopping. */
            state->self.resume();

            run_posted();
            if (posted_exception) {
                auto pe = posted_exception;
                posted_exception = nullptr;
                std::rethrow_exception(pe);
            }
            if (posted_stop) {
                /* Not stopped because a coroutine was destroyed and not stopped from an exception,
                then it was force stopped */
                posted_stop = false;
                break;
            }
        }
```

Members:

```cpp
    std::exception_ptr posted_exception = nullptr;
    std::vector<state_t *> posted_to_destroy;
    std::vector<std::function<void(void)>> posted_after_resume;    /* variant B only */
    bool posted_stop = false;
```

(`std::vector` with the default allocator, like the killer's own state, so nothing posted has to be
freed into a pool that may be gone.)

---

## 5. The killer

Everything killer specific is here: one public type, one internal struct, and `create_killer`. The
killer only uses the engine through the generic features from section 4 (`ERROR_FORCE_SUSPEND`,
the yield callbacks, `is_ready`, `stop_io(..., keep_completed)`), plus variant B's
`post_after_resume`.

### 5.1 Public: `kill_incomplete_t` and the declaration

Next to the `create_killer` declaration:

```cpp
/*! Thrown by a killer's kill function when it can't keep its promise ("when this returns, the
 * target does nothing more"): the target is executing right now, i.e. the kill function was
 * called, directly or not, from inside the target's own call chain (ex: a coroutine killing
 * itself). The target is marked and dies at its next wait (a co_await on an io, on a semaphore
 * without a token, a yield or a force_stop), before that wait does anything. Catch it to accept
 * that. It doesn't derive from std::exception on purpose, so generic handlers don't swallow it.
 * Don't let it escape through code that can't throw (a destructor, a modif callback): that
 * terminates. */
struct kill_incomplete_t {
    state_t *target;    /*!< the innermost frame of the target when the kill function was called */
};

/*! @fn
 * Creates a killer: a modification pack to attach to one coroutine (it follows that coroutine's
 * calls, not its scheds) and a kill function that ends the target's whole call chain. The kill
 * function keeps two promises:
 *  - when it returns normally, the target does nothing more: its frames are destroyed, or it
 *    had already finished;
 *  - a wait that already completed is never thrown away: a target whose wait completed (a
 *    Windows read that already took its bytes, a semaphore that already gave it its token) is
 *    first resumed, inside the kill, up to its next wait, and dies there, before that wait does
 *    anything.
 *
 * What it does, by what the target is doing when the kill function is called:
 *  - waiting (io, semaphore, yield/force_stop, or scheduled and never started): the wait had no
 *    effect, it is cancelled and the chain is destroyed now. Returns ERROR_OK.
 *  - its wait completed with an effect: it is resumed until its next wait, where it is destroyed
 *    (ERROR_OK), or until its root returns: then it wasn't killed, returns ERROR_FINISHED.
 *  - executing (the kill comes from inside the chain, ex: a self-kill): the chain is marked, dies
 *    at its next wait, and the kill function throws kill_incomplete_t.
 *  - already finished: nothing to do, ERROR_FINISHED.
 *  - called again from inside its own unwind (ex: from the destructor of a frame it is
 *    destroying): ERROR_GENERIC, that kill is already being done.
 * A killed root that was called (not scheduled) isn't destroyed: its caller owns it, it resumes
 * and the root's err is `e`.
 *
 * @warning A killer is single-target and single-use, permanently: attach its modif_pack_t to
 * exactly one coroutine. Once that coroutine has been killed (or has otherwise exited), the same
 * killer cannot be re-attached to a different coroutine to kill it too - the kill function reports
 * ERROR_FINISHED from then on, even if you did attach it elsewhere. This isn't an arbitrary
 * restriction: the killer tracks its target's call stack as one flat, untagged stack, so attaching
 * the same killer to more than one coroutine (whether at once or one after another) has no
 * well-defined way to tell those coroutines' frames apart.
 * @warning The kill function may resume coroutine code (see above) on the caller's stack and may
 * throw kill_incomplete_t: don't call it from modif callbacks, and not from code that can't throw
 * unless the target can't be executing at that point.
 * @param pool The pool on which to bind this killer. The killer may outlive it: nothing of the
 *             killer is allocated from the pool.
 * @param e The error value that will be set inside a killed root that was called (not scheduled)
 * @return A pair containing the modification pack that is to be attached to the target coroutine
 * and a function that is to be called when the user wants to kill the target coroutine. */
inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e);
```

### 5.2 Internal: `killer_state_t`

Replaces the implementation of `create_killer` (the local `kill_state_t` and the `sig_kill` lambda).
It sits where `create_killer`'s implementation is today, after `create_timeo`.

```cpp
#if COLIB_OS_WINDOWS
/* An IOCP request is done by the OS before its coroutine is resumed: once it completed, its bytes
already moved. A timer, or a wake-up by stop_io (err != ERROR_OK), moved nothing. */
inline bool io_had_effect(const io_desc_t& io) {
    return io.data && !(io.data->flags & io_data_t::IO_FLAG_TIMER) && io.data->state &&
            io.data->state->err == ERROR_OK;
}
#else /* COLIB_OS_WINDOWS */
/* epoll/kqueue only wait for readiness, the syscall runs after the resume: a completed wait moved
nothing yet. */
inline bool io_had_effect(const io_desc_t&) {
    return false;
}
#endif /* COLIB_OS_WINDOWS */

/* Everything a killer knows and does. One per create_killer() call, shared by the callbacks of its
modif pack and by its kill function. A plain std::make_shared, not the pool's allocator: the killer
may outlive the pool (018-014). */
struct killer_state_t : std::enable_shared_from_this<killer_state_t> {
    killer_state_t(error_e e) : e(e) {}

    error_e e;                          /* the err a killed call-origin root gets */

    std::stack<state_t *> call_stack;   /* the chain, top() is its innermost frame */
    io_desc_t *io_desc = nullptr;       /* the top waits on this io (from WAIT_IO to UNWAIT_IO) */
    sem_t *sem = nullptr;               /* the top waits on this semaphore (WAIT_SEM..UNWAIT_SEM) */
    sem_waiter_handle_p it;             /* its place in that semaphore's wait list */

    bool entered = false;   /* ENTER without a LEAVE since, for the top: it is executing - or it
                               was scheduled and never started, sched() runs ENTER early */
    bool dying = false;     /* decided: the chain may not start another wait */
    bool parked = false;    /* the top gave up a wait because of `dying` (ERROR_FORCE_SUSPEND) */
    bool driving = false;   /* kill() is resuming the chain right now */
    bool unwinding = false; /* kill() is destroying the chain right now */
    bool finished = false;  /* the root exited by itself while being driven */

    error_e kill();
    error_e drive();
    void drop(state_t *top);
    void unwind();
    error_e park(state_t *s);
};

inline error_e killer_state_t::kill() {
    /* re-entered from a destructor or a callback of our own unwind: already being done */
    if (unwinding)
        return ERROR_GENERIC;
    if (call_stack.empty())
        return ERROR_FINISHED;

    state_t *top = call_stack.top();
    pool_internal_t *pool = top->pool->get_internal();
    bool in_ready = pool->is_ready(top);

    /* Executing: its frames are in use, it dies at its next wait (see park()). This includes being
    called by the chain while we drive it. */
    if (entered && !in_ready) {
        dying = true;
        throw kill_incomplete_t{top};
    }

    /* Parked by an earlier lazy death, not dropped yet (variant B): drop it now. */
    if (parked) {
        unwind();
        return ERROR_OK;
    }

    if (in_ready) {
        if (entered) {
            /* scheduled, never started: sched() already ran ENTER, balance it */
            pool->remove_ready(top);
            do_leave_modifs(top);
            unwind();
            return ERROR_OK;
        }
        if ((io_desc && io_had_effect(*io_desc)) || sem) {
            /* Its wait completed and had an effect (bytes moved, a semaphore token taken): it is
            running, in zero time, until its next wait. */
            return drive();
        }
        /* The wait completed without an effect (readiness, a timer, a stop_io wake-up), or it
        yielded, force_stopped or was resumed by an external awaitable. */
        pool->remove_ready(top);
        drop(top);
        return ERROR_OK;
    }

    if (io_desc) {
        /* waiting on an io: cancel it; a request that completed meanwhile is delivered instead */
        error_e ret = pool->stop_io(*io_desc, ERROR_WAKEUP, true);
        if (ret == ERROR_FINISHED)
            return drive();
        if (ret != ERROR_OK) {
            COLIB_DEBUG("WARNING: failed to stop the io of a killed coroutine: %s",
                    dbg_enum(ret).c_str());
        }
        pool->remove_ready(top);    /* stop_io queued it, woken with ERROR_WAKEUP */
        drop(top);
        return ERROR_OK;
    }

    if (sem) {
        /* waiting on a semaphore: leave its wait list, no token was taken */
        sem->get_internal()->erase_waiter(*it);
        drop(top);
        return ERROR_OK;
    }

    /* Suspended on something the killer doesn't see (an external awaitable): destroyed as before,
    that awaitable must not resume it anymore. */
    unwind();
    return ERROR_OK;
}

inline error_e killer_state_t::drive() {
    state_t *top = call_stack.top();
    top->pool->get_internal()->remove_ready(top);

    /* Resume it here, the way the pool would. It runs until its next wait, where our callbacks
    park it (park()), or until its root exits (our EXIT callback). When resume() returns the chain
    is suspended or gone, so none of it runs after kill() returns. */
    dying = true;
    driving = true;
    top->self.resume();
    driving = false;

    if (finished)
        return ERROR_FINISHED;  /* it completed its task in time, nothing to kill */
    unwind();                   /* parked at its next wait (or suspended on an external awaitable) */
    return ERROR_OK;
}

inline void killer_state_t::drop(state_t *top) {
    /* Replay what a real wake-up would have done, so every modif sees the wait end. The top sits at
    (entered=false, left=true), hence ENTER first. Our own UNWAIT callbacks clear io_desc/sem. */
    if (io_desc) {
        io_desc_t &io = *io_desc;
        do_entry_modifs(top);
        do_unwait_io_modifs(top, io);
        do_leave_modifs(top);
    }
    else if (sem) {
        sem_t *s = sem;
        do_entry_modifs(top);
        do_unwait_sem_modifs(top, s);
        do_leave_modifs(top);
    }
    unwind();
}

inline void killer_state_t::unwind() {
    /* Innermost first, like a C++ stack unwinds. Our EXIT callback pops call_stack. */
    unwinding = true;
    while (call_stack.size() > 1) {
        state_t *s = call_stack.top();
        do_exit_modifs(s);
        s->self.destroy();
    }
    state_t *root = call_stack.top();
    do_exit_modifs(root);
    if (!root->caller_state) {
        root->self.destroy();       /* scheduled: nobody waits for it */
    }
    else {
        /* called from outside the chain: its caller owns it (task<T>::await_resume destroys it)
        and is resumed, the root's err being `e` */
        root->err = e;
        root->pool->get_internal()->push_ready(root->caller_state);
    }
    parked = false;
    unwinding = false;
}

/* The answer of the wait callbacks (io, semaphore, yield/force_stop) once the chain is dying. */
inline error_e killer_state_t::park(state_t *s) {
    parked = true;
    if (!driving) {
        /* Lazy death (the kill found it executing): nobody resumes it for us, it must be dropped
        as soon as it is suspended. VARIANT B, see section 6 - variant A drops it in the LEAVE
        callback instead and has nothing here. */
        auto self = shared_from_this();
        s->pool->get_internal()->post_after_resume([self]{
            if (self->parked && !self->unwinding && !self->call_stack.empty())
                self->unwind();
        });
    }
    return ERROR_FORCE_SUSPEND;
}
```

### 5.3 `create_killer`

```cpp
inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e) {
    /* nothing of the killer lives in the pool: it may outlive it (018-014) */
    (void)pool;
    auto kstate = std::make_shared<killer_state_t>(e);

    COLIB_DEBUG_TRACE("created killer: %p", kstate.get());

    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
    modif_pack_t pack;
    pack.push_back(create_modif<CO_MODIF_CALL_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            COLIB_DEBUG_TRACE("CALL[%p]: tracking killer: %p", kstate.get(), s);
            kstate->call_stack.push(s);
            return ERROR_OK;    /* calls take zero time: allowed even while dying */
        }
    ));
    pack.push_back(create_modif<CO_MODIF_SCHED_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            /* The first schedule must init the call stack */
            COLIB_DEBUG_TRACE("SCHED[%p]: tracking killer: %p", kstate.get(), s);
            kstate->call_stack.push(s);
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_EXIT_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            COLIB_DEBUG_TRACE("EXIT[%p]: tracking killer: %p", kstate.get(), s);
            COLIB_ENABLE_DEBUG_CHECK_ASSERT(kstate->call_stack.size(), "bad-pop");
            kstate->call_stack.pop();
            if (kstate->driving && kstate->call_stack.empty()) {
                /* The root exited by itself (returned, co_yielded or threw) while kill() resumes
                the chain: it finished in time. Don't let it jump into its caller from inside
                kill(), queue the caller instead. */
                kstate->finished = true;
                if (s->caller_state) {
                    s->pool->get_internal()->push_ready(s->caller_state);
                    return ERROR_FORCE_SUSPEND;
                }
            }
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_ENTER_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            (void)s;
            kstate->entered = true;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_LEAVE_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            (void)s;
            kstate->entered = false;
            /* VARIANT A (section 6) adds the lazy drop here */
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_WAIT_IO_CBK>(flags,
        [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
            COLIB_DEBUG_TRACE("WAIT_IO[%p]: tracking killer: %p io-ptr: %p",
                    kstate.get(), s, &io_desc);
            if (kstate->dying)
                return kstate->park(s);
            kstate->io_desc = &io_desc;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_UNWAIT_IO_CBK>(flags,
        [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
            (void)s;
            (void)io_desc;
            COLIB_DEBUG_TRACE("UNWAIT_IO[%p]: tracking killer: %p io-ptr: %p",
                    kstate.get(), s, &io_desc);
            kstate->io_desc = nullptr;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
        [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
            COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p it-ptr: %p",
                    kstate.get(), s, sem, it.get());
            if (kstate->dying)
                return kstate->park(s);
            kstate->sem = sem;
            kstate->it = it;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_UNWAIT_SEM_CBK>(flags,
        [kstate](state_t *s, sem_t *sem) -> error_e {
            (void)s;
            (void)sem;
            COLIB_DEBUG_TRACE("UNWAIT_SEM[%p]: tracking killer: %p sem: %p",
                    kstate.get(), s, sem);
            kstate->sem = nullptr;
            /* The waiter handle is pool-allocated (push_waiter()), so it is let go of with the
            wait, not held until the killer dies, which may be after the pool. 2026-09-23 05:06 */
            kstate->it = nullptr;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            if (kstate->dying)
                return kstate->park(s);
            return ERROR_OK;
        }
    ));

    return {pack, [kstate]() -> error_e { return kstate->kill(); }};
}
```

Notes on the callbacks:

- `entered` is tracked for the chain, not per frame. That works because the chain's run segments are
  bracketed by ENTER/LEAVE: a call is `LEAVE(caller)` then `ENTER(callee)`, a return is
  `LEAVE(callee)` then `ENTER(caller)`, with nothing in between.
- The WAIT callbacks don't record a wait they park, and the awaiter's park path calls UNWAIT anyway,
  so `io_desc`/`sem` stay consistent.
- The EXIT callback only takes over the caller while driving. During our own `unwind()`, `driving`
  is false. During another killer's unwind of the same frames, the other killer can't be unwinding
  while we drive (it would find the chain executing and throw), so the only exits seen while
  driving are real ones: a return, a `co_yield`, or an exception leaving the frame.
- There's no reference cycle: the callbacks hold `kstate`, the coroutines' tables hold the
  callbacks, and `kstate` holds nothing of either.

---

## 6. The lazy drop: variant A or B

A chain that dies lazily (the kill found it executing) parks at its next wait while nobody is
driving it. Control then goes back to whoever resumed it, almost always `pool_internal_t::run()`.
Someone has to destroy it, as soon as possible. The two candidates behave the same in colib time:
no other coroutine runs between the park and the drop.

**Variant B (in the code above): the pool runs posted work right after the resume.**
`killer_state_t::park()` posts the drop through `post_after_resume()`, and `run()` runs it in
`run_posted()` right after `state->self.resume()` returns, before picking the next task.

- It reuses the pattern `post_to_destroy` already uses.
- There are no frame-lifetime subtleties.
- If another killer drops the same frames first, its unwind pops our `call_stack` through our EXIT
  callback, and the posted drop sees an empty stack and does nothing.
- Weak spot: a coroutine resumed from outside `run()` (a user `external_on_resume` from a callback,
  or a kill called from `main` after `run()` returned). Then the drop waits for the pool's next step.

**Variant A: drop it inside the LEAVE callback of the park.** Remove the `if (!driving) {...}` block
from `park()`, remove `post_after_resume`/`posted_after_resume` from section 4.13, and give the LEAVE
callback the drop:

```cpp
    pack.push_back(create_modif<CO_MODIF_LEAVE_CBK>(flags,
        [kstate](state_t *s) -> error_e {
            (void)s;
            kstate->entered = false;
            if (kstate->parked && !kstate->driving && !kstate->unwinding) {
                /* Lazy death: the chain just parked at its next wait and nobody resumes it for
                us - drop it now. `s` is destroyed by this, so stop the leave dispatch: a plain
                error does that (LEAVE isn't a run-all callback, see do_generic_modifs), and the
                park paths ignore do_leave_modifs()'s result and touch nothing after it. */
                kstate->unwind();
                return ERROR_GENERIC;
            }
            return ERROR_OK;
        }
    ));
```

- It's literally immediate, and it works whoever resumed the coroutine.
- It needs a discipline: every park path in section 4 must keep `do_leave_modifs()` as its last
  action. They are written that way.
- The dispatch stops at the killer, so leave callbacks ordered after the killer's are skipped for
  that last suspension. Those modifs see ENTER then EXIT, without the LEAVE in between. That's
  fine for the debug checks (`dbg_check_modif_leave` runs before the dispatch), but a modif that
  counts its own LEAVEs sees one missing.
- The frame is destroyed while its own `await_suspend` is on the stack, which C++ allows as long
  as nothing touches the frame afterwards.

My recommendation is B, for reviewability. A is the one to pick if "dies now, whoever resumed it"
matters more than keeping the frame destroy out of `await_suspend`.

---

## 7. `create_timeo`

Today `exec_coro` and `timer_coro` kill each other. With the drive, that becomes a self-kill inside
colib: the timer kills `exec_coro`, the drive runs `exec_coro` to its end inside the timer's kill,
and `exec_coro` then calls `timer_sig()` on the timer. The timer is executing (it's inside that
kill), so the kill throws. The fix is one decision point, with the timer only requesting.

This also fixes an existing hang: when the timer's sleep failed, `timer_coro` killed `exec_coro` and
returned without signaling `sem`, so `ret_coro` waited forever. (The pseudo-code at the end of the
header does signal; the code didn't.)

```cpp
template <typename T>
inline task<std::pair<T, error_e>> create_timeo(
        task<T> t, pool_t *pool, const std::chrono::microseconds& timeo)
{
    struct timer_state_t {
        std::function<error_e(void)> timer_elapsed_sig; /* kills exec_coro */
        std::function<error_e(void)> timer_sig;         /* kills timer_coro */
        sem_p sem;
        uint64_t duration;
        error_e tstate_err;
        task<T> t;
        T ret;
        int id;
        bool decided = false;       /* the result is set and sem signaled - exactly once */
        bool timer_firing = false;  /* timer_coro is inside timer_elapsed_sig(): exec_coro may be
                                       driven by it, and must not kill it back */
    };

    auto tstate = std::shared_ptr<timer_state_t>(alloc<timer_state_t>(pool),
            dealloc_create<timer_state_t>(pool), allocator_t<int>{pool});

    COLIB_DEBUG_TRACE("created tstate: %p coro state: %p", tstate.get(), &t.h.promise().state);

    auto [timer_elapsed_killer, timer_elapsed_sig] = create_killer(pool, ERROR_WAKEUP);
    auto [timer_killer, timer_sig] = create_killer(pool, ERROR_TIMEO);

    tstate->sem = create_sem(pool, 0);
    tstate->timer_elapsed_sig = timer_elapsed_sig;
    tstate->timer_sig = timer_sig;
    tstate->duration = timeo.count();
    tstate->tstate_err = ERROR_OK;
    tstate->t = t;

    COLIB_DEBUG_TRACE("sem: %p", tstate->sem.get());

    auto exec_coro = [](std::shared_ptr<timer_state_t> tstate) -> task_t {
        COLIB_DEBUG_TRACE_SCOPE("Exec for tstate: %p", tstate.get());
        tstate->ret = co_await tstate->t;
        /* finished first: if the timer still sleeps, drop it (it's waiting, no effect). If it's
        firing, it is the one resuming us right now (see create_killer), it only finds out. */
        if (!tstate->timer_firing)
            tstate->timer_sig();
        tstate->decided = true;
        tstate->tstate_err = ERROR_OK;
        tstate->sem->signal();
        co_return ERROR_OK;
    }(tstate);

    auto timer_coro = [](std::shared_ptr<timer_state_t> tstate) -> task_t {
        COLIB_DEBUG_TRACE_SCOPE("Waiting timer for tstate: %p", tstate.get());
        error_e err = (error_e)co_await COLIB_REGNAME(sleep_us(tstate->duration));
        if (err != ERROR_OK)
            COLIB_DEBUG_TRACE("Timer ERRORED OUT tstate[%p]", tstate.get());
        else
            COLIB_DEBUG_TRACE("Timer EXPIRED tstate[%p]", tstate.get());

        /* Kill the task. A task whose wait already completed is resumed inside this call and may
        finish in it (ERROR_FINISHED): then it already decided, with its value. */
        tstate->timer_firing = true;
        tstate->timer_elapsed_sig();
        if (tstate->decided)
            co_return ERROR_OK;

        tstate->decided = true;
        tstate->tstate_err = (err != ERROR_OK) ? err : ERROR_TIMEO;
        tstate->sem->signal();
        co_return (err != ERROR_OK) ? ERROR_GENERIC : ERROR_OK;
    }(tstate);

    add_modifs(pool, exec_coro, timer_elapsed_killer);
    add_modifs(pool, timer_coro, timer_killer);

    pool->sched(COLIB_REGNAME(exec_coro));
    pool->sched(COLIB_REGNAME(timer_coro));

    auto ret_coro = [](std::shared_ptr<timer_state_t> tstate) -> task<std::pair<T, error_e>>{
        COLIB_DEBUG_TRACE_SCOPE("wait tstate: %p", tstate.get());
        co_await tstate->sem->wait();
        co_return std::pair<T, error_e>{tstate->ret, tstate->tstate_err};
    }(tstate);

    return ret_coro;
}
```

Why the kills can't throw here:

- `timer_elapsed_sig()` (the timer killing the task) runs in `timer_coro`, which is never part of
  the task's chain, so the task can't be executing at that moment.
- `timer_sig()` (the task killing the timer) is skipped exactly when the timer could be executing
  (`timer_firing`).

The declaration's doc changes from "`t` will be destroyed if it didn't complete" to:

```cpp
 * @param timeo The time after which `t` is killed (see create_killer) if it didn't complete. A
 * `t` whose wait completed right when the timer fired still gets that result: it is resumed up to
 * its next wait and dies there (ERROR_TIMEO), or completes (its value, ERROR_OK).
```

---

## 8. Behaviour table

| Target state when kill is called | Linux/kqueue | Windows | Result |
|---|---|---|---|
| already finished / nothing tracked | - | - | `ERROR_FINISHED` |
| called from inside its own unwind | - | - | `ERROR_GENERIC` (as today) |
| executing (self-kill, called by the chain) | marked, dies at next wait | same | throws `kill_incomplete_t` |
| parked, lazy drop pending (variant B) | dropped now | same | `ERROR_OK` |
| scheduled, never started | LEAVE, dropped | same | `ERROR_OK` |
| queued, io wait completed | dropped (readiness, no effect) | driven (bytes moved) | `ERROR_OK` / `ERROR_FINISHED` |
| queued, timer / stop_io wake-up | dropped | dropped | `ERROR_OK` |
| queued, semaphore woke it (token) | driven | driven | `ERROR_OK` / `ERROR_FINISHED` |
| queued after yield / force_stop / external resume | dropped | dropped | `ERROR_OK` |
| waiting on io | stop_io, dropped | cancel: aborted -> dropped, already finished -> driven | `ERROR_OK` / `ERROR_FINISHED` |
| waiting on a semaphore | leaves the wait list, dropped | same | `ERROR_OK` |
| suspended on an external awaitable | destroyed (as today) | same | `ERROR_OK` |

"Driven" means resumed inside the kill up to its next wait, where it's parked and dropped
(`ERROR_OK`), or until its root exits (`ERROR_FINISHED`, the root's caller is queued).

---

## 9. Tests

Following the repo's bug workflow (root `CLAUDE.md`), `018-015` and `018-016` reproduce the two
defects and should be committed failing, with `tests/BUGS.md` entries, before the fix. The others
cover the new behaviour and are written against the design. `002-004` needs the lazy drop, so either
variant.

### 9.1 New: `018-015-reproduced_timeo_read_drops_bytes.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test46 - Reproduced Bugs: create_timeo(co::read(...)) drops bytes that were already read
================================================================================================= */

/* On IOCP co::read issues an overlapped ReadFile before it waits, so the kernel can complete it
(moving the bytes out of the socket, into the buffer) before the coroutine is resumed. When
create_timeo's timer fires in that window, its killer either destroys the already-queued reader
(the completion is dequeued, not yet resumed) or cancels a request that already finished and
ignores its result. Either way the caller gets ERROR_TIMEO and the bytes are gone. Found through
bbb_repo's ssh tunnel, which reads its client socket with a 5ms create_timeo in a loop; reproduced
standalone losing ~60% of the bytes. epoll/kqueue only wait for readiness and read() after the
resume, so they can't lose anything this way. 2026-09-23 */

#if COLIB_OS_WINDOWS

#include <atomic>
#include <random>

static const int test46_cnt = 1000;
static std::atomic<int> test46_sent{0};

static void test46_sender(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return;
    }
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    std::mt19937 rng(1234);
    for (int i = 0; i < test46_cnt; i++) {
        char c = 'a' + i % 26;
        if (send(s, &c, 1, 0) != 1)
            break;
        test46_sent++;
        Sleep(rng() % 4);   /* 0..3ms: bytes keep landing around the 5ms read timeout */
    }
    Sleep(300);
    closesocket(s);
}

static co::task_t test46_reader(SOCKET srv, int *received) {
    auto pool = co_await co::get_pool();
    sockaddr_in peer = {};
    uint32_t len = sizeof(peer);
    SOCKET c = co_await co::accept(srv, (sockaddr *)&peer, &len);
    ASSERT_COFN(CHK_BOOL(c != INVALID_SOCKET));

    char buff[4096];
    while (true) {
        auto [ret, err] = co_await co::create_timeo(co::read((HANDLE)c, buff, sizeof(buff)),
                pool, std::chrono::microseconds(5000));
        if (err == co::ERROR_TIMEO)
            continue;
        if (err != co::ERROR_OK || ret <= 0)
            break;
        *received += (int)ret;
    }
    closesocket(c);
    co_return 0;
}

int test46_timeo_read_drops_bytes() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(srv != INVALID_SOCKET));
    FnScope close_srv([srv]{ closesocket(srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(srv, 1) == 0));
    int addr_len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &addr_len) == 0));

    std::thread sender(test46_sender, ntohs(addr.sin_port));
    int received = 0;
    auto pool = co::create_pool();
    pool->sched(test46_reader(srv, &received));
    co::run_e ret = pool->run();
    sender.join();

    DBG("sent: %d received: %d", test46_sent.load(), received);
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test46_sent.load() == test46_cnt));
    ASSERT_FN(CHK_BOOL(received == test46_sent.load()));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test46_timeo_read_drops_bytes() {
    DBG("readiness-based backend: read() runs after the resume, nothing can be lost this way");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test46_timeo_read_drops_bytes();
    print_test_result("018-015-reproduced_timeo_read_drops_bytes.cpp", ret >= 0);
    return ret;
}
```

### 9.2 New: `018-016-reproduced_killer_drops_sem_token.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test47 - Reproduced Bugs: a killer destroys a coroutine a semaphore already gave its token to
================================================================================================= */

/* sem_t::signal() takes the count down and queues the waiter. A kill that runs before that waiter
is resumed finds it in the ready queue and destroys it: the token was consumed and nobody got it.
The same flaw as 018-015, on semaphores and on every platform. Fixed, the killer resumes the waiter
up to its next wait (the code after the wait gets the token) and destroys it there. 2026-09-23 */

static int test47_got_token = 0;
static int test47_after_next_wait = 0;
static int test47_destructed = 0;
static bool test47_ok = false;

struct test47_marker_t {
    ~test47_marker_t() { test47_destructed++; }
};

static co::task_t test47_victim(co::sem_p sem, co::sem_p never) {
    test47_marker_t marker;
    co_await sem->wait();
    test47_got_token++;             /* must run: the wait completed, it holds the token */
    co_await never->wait();         /* dies here, without waiting */
    test47_after_next_wait++;       /* must not run */
    co_return 0;
}

static co::task_t test47_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();           /* the victim runs and waits on `sem` */
    sem->signal();                  /* the victim takes the token and is queued, not resumed yet */
    co::error_e ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
    ASSERT_COFN(CHK_BOOL(test47_got_token == 1));   /* before the fix: 0, the token was lost */
    ASSERT_COFN(CHK_BOOL(test47_after_next_wait == 0));
    ASSERT_COFN(CHK_BOOL(test47_destructed == 1));
    ASSERT_COFN(CHK_BOOL(sem->try_dec() == false)); /* the token wasn't handed out twice either */
    test47_ok = true;
    co_return 0;
}

int test47_killer_drops_sem_token() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test47_victim(sem, never), mods));
    pool->sched(test47_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test47_ok));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test47_killer_drops_sem_token();
    print_test_result("018-016-reproduced_killer_drops_sem_token.cpp", ret >= 0);
    return ret;
}
```

### 9.3 New: `002-003-flowctrl_killer_finished.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test48 - Flow Control: a killed chain that finishes before it can die
================================================================================================= */

/* The target's wait completed (a semaphore gave it its token), and after it the chain never waits
again: the killer resumes it inside the kill, the chain returns all the way up to its root and the
kill reports ERROR_FINISHED - it wasn't killed, it completed in time, and its results exist. */

static int test48_out = 0;
static bool test48_ok = false;

static co::task<int> test48_inner(co::sem_p sem) {
    co_await sem->wait();
    co_return 7;
}

static co::task_t test48_outer(co::sem_p sem) {
    test48_out = co_await test48_inner(sem);    /* a 2-frame chain: inner returns into outer */
    co_return 0;
}

static co::task_t test48_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* outer/inner run, inner waits on `sem` */
    sem->signal();
    co::error_e ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_FINISHED));
    ASSERT_COFN(CHK_BOOL(test48_out == 7));     /* it ran to the end, inside the kill */
    ASSERT_COFN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    test48_ok = true;
    co_return 0;
}

int test48_killer_finished() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test48_outer(sem), mods));
    pool->sched(test48_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test48_ok));
    return 0;
}

int main() {
    int ret = test48_killer_finished();
    print_test_result("002-003-flowctrl_killer_finished.cpp", ret >= 0);
    return ret;
}
```

### 9.4 New: `002-004-flowctrl_killer_self_kill.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test49 - Flow Control: a coroutine that kills itself
================================================================================================= */

/* The kill function can't keep its promise for an executing target: it throws kill_incomplete_t
and the target dies at its next wait. Caught: the zero-time code until that wait still runs, the
wait doesn't. Not caught: the exception unwinds the coroutine itself, and as for any exception
leaving a scheduled root, pool_t::run() rethrows it. */

static int test49_zero_time = 0;
static int test49_after_wait = 0;
static int test49_destructed = 0;
static bool test49_thrown = false;

struct test49_marker_t {
    ~test49_marker_t() { test49_destructed++; }
};

static co::task_t test49_caught(std::function<co::error_e(void)> kill_fn, co::sem_p never) {
    test49_marker_t marker;
    try {
        kill_fn();
    }
    catch (co::kill_incomplete_t &) {
        test49_thrown = true;
    }
    test49_zero_time++;         /* runs: code until the next wait takes zero time */
    co_await never->wait();     /* dies here, without waiting */
    test49_after_wait++;        /* must not run */
    co_return 0;
}

static co::task_t test49_uncaught(std::function<co::error_e(void)> kill_fn) {
    test49_marker_t marker;
    kill_fn();                  /* throws out of this coroutine */
    test49_after_wait++;        /* must not run */
    co_return 0;
}

int test49_self_kill() {
    {
        auto pool = co::create_pool();
        auto never = co::create_sem(pool, 0);
        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(co::add_modifs(pool.get(), test49_caught(kill_fn, never), mods));
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test49_thrown));
        ASSERT_FN(CHK_BOOL(test49_zero_time == 1));
        ASSERT_FN(CHK_BOOL(test49_after_wait == 0));
        ASSERT_FN(CHK_BOOL(test49_destructed == 1));
        ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    }
    {
        auto pool = co::create_pool();
        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(co::add_modifs(pool.get(), test49_uncaught(kill_fn), mods));
        bool escaped = false;
        try {
            pool->run();
        }
        catch (co::kill_incomplete_t &) {
            escaped = true;
        }
        ASSERT_FN(CHK_BOOL(escaped));
        ASSERT_FN(CHK_BOOL(test49_after_wait == 0));
        ASSERT_FN(CHK_BOOL(test49_destructed == 2));
    }
    return 0;
}

int main() {
    int ret = test49_self_kill();
    print_test_result("002-004-flowctrl_killer_self_kill.cpp", ret >= 0);
    return ret;
}
```

### 9.5 New: `011-006-modifs_force_suspend.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test50 - Modifs: ERROR_FORCE_SUSPEND and CO_MODIF_WAIT_YIELD_CBK
================================================================================================= */

/* A WAIT_SEM callback answering ERROR_FORCE_SUSPEND parks the coroutine: the wait isn't
registered (no token can reach it), the wait callbacks are unwound (UNWAIT_SEM) and the coroutine
leaves (LEAVE); nothing else runs until its owner destroys it. A WAIT_YIELD callback returning an
error refuses the yield: the coroutine continues right away, and doesn't get a second ENTER (the
debug checks would abort). */

static std::string test50_order;
static co::state_t *test50_parked = nullptr;
static int test50_after_wait = 0;
static int test50_destructed = 0;
static int test50_after_yield = 0;

struct test50_marker_t {
    ~test50_marker_t() { test50_destructed++; }
};

static co::task_t test50_parked_task(co::sem_p sem) {
    test50_marker_t marker;
    co_await sem->wait();
    test50_after_wait++;        /* must not run */
    co_return 0;
}

static co::task_t test50_owner(co::sem_p sem) {
    co_await co::yield();       /* the parked task runs and parks */
    sem->signal();              /* no waiter: the count goes to 1, nobody is woken */
    co_await co::yield();
    ASSERT_COFN(CHK_BOOL(test50_parked != nullptr));
    ASSERT_COFN(CHK_BOOL(test50_after_wait == 0));
    co::destroy_state(test50_parked);   /* the owner ends it */
    ASSERT_COFN(CHK_BOOL(test50_destructed == 1));
    ASSERT_COFN(CHK_BOOL(sem->try_dec()));  /* the token stayed in the semaphore */
    co_return 0;
}

static co::task_t test50_refused_yield() {
    co_await co::yield();       /* refused: continues right away */
    test50_after_yield++;
    co_return 0;
}

int test50_force_suspend() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto flags = co::CO_MODIF_INHERIT_NONE;

    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
        [](co::state_t *s, co::sem_t *, co::sem_waiter_handle_p) -> co::error_e {
            test50_order += "W";
            test50_parked = s;
            return co::ERROR_FORCE_SUSPEND;
        }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
        [](co::state_t *, co::sem_t *) -> co::error_e { test50_order += "U"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
        [](co::state_t *) -> co::error_e { test50_order += "L"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
        [](co::state_t *) -> co::error_e { test50_order += "E"; return co::ERROR_OK; }));

    auto refuse = co::modif_pack_t(1, co::create_modif<co::CO_MODIF_WAIT_YIELD_CBK>(flags,
        [](co::state_t *) -> co::error_e { return co::ERROR_USER; }));

    pool->sched(co::add_modifs(pool.get(), test50_parked_task(sem), pack));
    pool->sched(test50_owner(sem));
    pool->sched(co::add_modifs(pool.get(), test50_refused_yield(), refuse));
    ASSERT_FN(pool->run());

    /* sched's ENTER, then the park: WAIT_SEM, UNWAIT_SEM, LEAVE - no ENTER, it never resumed */
    DBG("order: %s", test50_order.c_str());
    ASSERT_FN(CHK_BOOL(test50_order == "EWUL"));
    ASSERT_FN(CHK_BOOL(test50_after_yield == 1));
    return 0;
}

int main() {
    int ret = test50_force_suspend();
    print_test_result("011-006-modifs_force_suspend.cpp", ret >= 0);
    return ret;
}
```

(`co::destroy_state()` runs the exit callbacks and destroys the frame and its callers, the same
thing `pool_t::clear()` does for waiters. It's used here as the owner's way to end a parked
coroutine.)

### 9.6 Existing tests that change

- **`002-002-flowctrl_create_killer.cpp`**: the second kill after the target is gone:
  `ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_GENERIC));` becomes
  `ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));`, and the comment above it becomes
  "nothing left to kill: sig_kill() reports ERROR_FINISHED when the call stack is empty".
- **`018-005-reproduced_killer_after_completion.cpp`**: same change,
  `ASSERT_COFN(CHK_BOOL(ret == co::ERROR_FINISHED));`. The header comment's "same
  kill_fn()-with-nothing-left-to-kill return code" still holds.

### 9.7 Existing tests that should pass unchanged (worth running)

- **`018-006`** (reentrant kill from a destructor): the target waits on a semaphore, so it's
  dropped. The inner call happens while `unwinding` is set, so it gets `ERROR_GENERIC`, and the
  outer call gets `ERROR_OK`.
- **`003-003`** (timeout): the slow task waits on a timer, which has no effect, so it's dropped and
  the result is `ERROR_TIMEO`. Unchanged.
- **`018-014`** (killer outlives pool): the kill state is still a plain `make_shared`. Posted drops
  hold it only until the pool runs them or dies.
- **`018-008`, `018-009`**: an aborted semaphore wait now also runs `UNWAIT_SEM`. They don't observe
  callbacks; the unlocker logic is untouched.
- **`011-003`**: the order of a normal wait is unchanged.
- **`009-001`, `002-001`**: `yield`/`force_stop` normal paths are unchanged, apart from the
  `triggered` flag.

---

## 10. Documentation to update

Once the code lands (none of this is done yet, so the docs keep matching the code in the meantime):

- **`colib.h` comments**:
  - `error_e`, `modif_e` and `create_killer`/`create_timeo` as in sections 4.1, 4.2, 5.1 and 7.
  - The `yield()` and `force_stop()` declarations mention `CO_MODIF_WAIT_YIELD_CBK`.
  - `wait_all`'s doc says "sig_killer installed in all". It isn't: `wait_all` only uses futures.
    That's a stale doc independent of this redesign.
  - The pseudo-code block after `#endif /* COLIB_H */`: the `create_killer()` and `create_timeo()`
    sections are rewritten after sections 5 and 7. `yield`, `force_stop`, `io_awaiter`,
    `sem_awaiter` and `exit-task` get the park paths. `force_awake()` gets `keep_completed`.
- **`docs/04_lifetimes.md`, the `create_killer()` section**:
  - "at the moment a kill triggers, the innermost frame is always parked in exactly one of three
    places ... a trigger can only run while the target itself isn't" is no longer true (the
    executing case).
  - The "Killing a task that already finished" paragraph now returns `ERROR_FINISHED`.
  - The "killer from a different killer is UB" paragraph is replaced by the reentrancy rules in
    section 2: nested kills of other chains are fine, an executing target throws, and the killer's
    own unwind answers `ERROR_GENERIC`.
  - A new paragraph on driving, P1/P2 and the stop point.
- **`docs/02_api.md`**: the `create_killer()`, `create_timeo()`, `error_e` and modif sections,
  `kill_incomplete_t`, `CO_MODIF_WAIT_YIELD_CBK`, `ERROR_FORCE_SUSPEND`.
- **`docs/03_execution_model.md`**: the mention that a killer unwinds through nested calls stays
  true; add that it may first resume them.
- **`docs/understanding.md`**: the Timing and Flow Control summaries of `create_killer`/
  `create_timeo`.
- **`docs/TODO.md`**: the "Killer-from-killer reentrancy" entry is resolved.
- **`tests/CLAUDE.md`**: the modif type list and the `error_e` list (the two new codes are
  positive, so `ASSERT_*` treats them as success).
- **`tests/BUGS.md`**: entries for `018-015`/`018-016` while they're open. #1 (the WAIT/LEAVE order
  doc) and #4 (kqueue `force_awake`) are untouched by this.
- **`tests/progress.md` / `tests/todo.md`**: the new test rows.

---

## 11. Open points and known limits

- **The lazy drop, variant A or B** (section 6). The rest of the design doesn't depend on the choice.
- **The access violation.** One of the two standalone reproduction runs with the timeout crashed.
  It's not explained. Case B (destroying a coroutine while its overlapped request may still be
  touched by the kernel) would explain it, but that isn't verified. Reproduce under a debugger with
  `018-015` before the fix, and check it's gone after.
- **External awaitables.** A coroutine suspended in one is still destroyed as before, and one
  resumed through `external_sched_resume` is treated as having completed nothing. An awaitable that
  doesn't call `external_on_suspend` leaves `entered` set, so its coroutine looks executing and the
  kill throws. Giving `external_on_suspend` a wait callback would let them take part; that's a
  separate change.
- **`COLIB_ENABLE_MULTITHREAD_SCHED`.** `is_ready()` doesn't see `ready_thread_tasks`, and a kill
  from another thread isn't covered.
- **A kill called from outside `run()`** (ex: from `main` after it returned) may drive coroutines
  there. Their posted work (a finished root's destroy, a variant B drop) waits for the next run
  step.
- **Throwing through colib.** `kill_incomplete_t` must not cross a modif callback or an
  `await_suspend` (`task<T>::await_suspend` is `noexcept`, which means terminate). This is documented
  in the warning, not enforced.
- **Pre-existing, noticed while reading, not changed here:**
  - The Windows `handle_ready_events()` sets `err = ERROR_OK` for every dequeued packet, whatever
    its status, so a failed request reads as success with 0 bytes.
  - An explicit `stop_io()` on a request whose completion was already dequeued can queue its
    coroutine twice (the killer never does that: it classifies first).
  - A sched callback that refuses a task after the killer's own SCHED callback pushed it leaves a
    frame in `call_stack` that never runs.
- **Not considered and rejected:**
  - Retrying the kill from the timer after a `yield`: a workaround, not a rule.
  - A deferred kill that returns before the target dies: breaks P1.
  - Delivering the kill as an exception inside the target: user code could catch it, and it needs
    exceptions everywhere. Parking and destroying do the same job with the existing unwind.
