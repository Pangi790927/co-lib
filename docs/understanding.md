# understanding.md

**Not user-facing.** This is a draft/staging area: for each subsystem `colib.h` is organized into
(the header's own section breakdown, plus a few things that aren't explicit header sections but are
core building blocks - the allocator, the pool itself), a generic description, an explanation of its
API, and a worked example. First pass, for the user to correct before any of it becomes a real
chapter - see this directory's `CLAUDE.md`.

---

## How `colib.h` itself is ordered

Worth knowing before hunting for anything in the source, and worth restating in whatever chapter
ends up being the reader's map of the file: the `HEADER` section is ordered **types first, then
functions last**, and within that, roughly by dependency order rather than alphabetically -
`error_e`/`run_e`/`modif_e` (enums everything else references) → `allocator_t<T>` (needed by
`pool_t`) → `pool_t` → `sem_t` → `io_desc_t` (platform-specific) → `state_t` → `modif_t`, *then*
the function groups: Pool & Sched, Externals, Modifications, Timing, Flow Controll, cross-platform
IO, platform-specific IO, Debug Interfaces. The `IMPLEMENTATION` section mirrors the same order a
second time, now with bodies.

Practical upshot: if you're looking for *what something is*, it's in the first half of the file, in
roughly the order things depend on each other. If you're looking for *what you can call*, it's in
the function groups, in the same topic order as the types above them. Doc-comment content in this
directory should probably follow the same ordering convention for the same reason - it's not
alphabetical, it's "read top to bottom and each part already makes sense from what came before it."

---

## Pool (`pool_t`)

**What it is.** The shared runtime a group of coroutines executes on: one ready queue, one I/O
engine (epoll/IOCP/kqueue), one timer pool, one allocator. Everything else in the library - tasks,
semaphores, modifications - is scoped to a pool. A task can't move between pools once scheduled.

**API.**
- `create_pool()` → `pool_p` (a `shared_ptr<pool_t>`) - allocates and initializes everything.
- `pool_t::sched(task, modifs = {})` - enqueue a task to run; doesn't run it yet.
- `pool_t::run()` → `run_e` - runs coroutines until none are left, `force_stop` is called, or an
  error occurs. Blocks the calling thread. Re-callable after a `force_stop`.
- `pool_t::clear()` - tears down everything attached (ready/waiting-on-io/waiting-on-sem), also
  runs automatically in the destructor.
- `pool_t::stop_io(io_desc)` - cancel a specific pending I/O wait.
- `pool_t::stopval` / `pool_t::user_ptr` - plain public fields, yours to use; `stopval` is also
  written by `force_stop`.

**Example.**
```cpp
colib::pool_p pool = colib::create_pool();
pool->sched(some_coroutine());
pool->sched(another_coroutine());
colib::run_e result = pool->run();  // blocks until both finish (or force_stop/error)
```

**It's replaceable too, same mechanism as the allocator (see below).** `pool_t` itself isn't
swappable, but its *internals* are: `pool_internal_t` owns a `timer_pool_t` and an `io_pool_t`, and
both of those are exactly what `COLIB_OS_UNKNOWN` + `COLIB_OS_UNKNOWN_IMPLEMENTATION` (+
`COLIB_OS_UNKNOWN_IO_DESC` for the matching `io_desc_t` shape) let you replace wholesale - same
raw-code-splice pattern as `COLIB_ALLOCATOR_REPLACE`, just for the I/O/timer backend instead of the
memory backend. In other words: `COLIB_OS_LINUX`/`COLIB_OS_WINDOWS`/`COLIB_OS_UNIX` aren't the only
three options baked into the library - they're the three the library ships an implementation for;
`COLIB_OS_UNKNOWN` is the same extension point, just with no default. See "IO Pool" and "`io_desc_t`"
below for what that implementation actually has to provide.

---

## Allocator (`allocator_t<T>`)

Not an explicit header section, but a backend the rest of the library sits on - most of `colib.h`'s
own internals (not the coroutine promise itself) allocate through this instead of `malloc`/`new`.

**What it is.** A fixed-bucket slab allocator: five size classes (32/64/128/512/2048 bytes,
adjustable via `COLIB_ALLOCATOR_SCALE`), each a pre-populated stack of free slot indices. An
allocation pops a slot from the smallest bucket that fits the request; a free pushes the slot back
(or falls through to a real `free`/`malloc` if the request didn't fit any bucket, or that bucket was
already exhausted at allocation time). The whole point is that these are small objects allocated and
freed constantly (per-coroutine state, per-wait bookkeeping) - reusing pre-sized slots avoids
`malloc`/`free` overhead and avoids fragmentation for as long as a bucket isn't exhausted.

**API.**
- `allocator_t<T>` - a standard-library-shaped allocator (`value_type`, `allocate`/`deallocate`,
  converting constructor, `==`/`!=` by owning pool), constructed from a `pool_t*`. Plugs directly
  into `std::` containers that take an allocator template parameter.
- `deallocator_t<T>` - a small helper functor (`operator()(T*)`) used where a `unique_ptr`/
  `shared_ptr` deleter needs to route through the same allocator.
- Config: `COLIB_DISABLE_ALLOCATOR` (fall back to `malloc` entirely), `COLIB_ALLOCATOR_SCALE`
  (scales every bucket).

**It's replaceable, mechanically via raw code injection at two separate splice points** -
`COLIB_ALLOCATOR_REPLACE` isn't a runtime policy switch, it's a compile-time macro that swaps out
literal chunks of `colib.h` for code you supply:

- `COLIB_ALLOCATOR_REPLACE_IMPL_1` gets pasted in place of the *entire* default bucket
  implementation (colib.h ~2312-2416: the `allocator_memory_t` struct, plus the free function
  templates `alloc<T>(pool_t*, Args&&...)` and `dealloc_create<T>(pool_t*)`). Your replacement must
  define all three of those exact symbols yourself - `allocator_memory_t` in particular must exist
  as a type, because `pool_t` holds one directly (`std::unique_ptr<allocator_memory_t>
  allocator_memory`, constructed via `std::make_unique<allocator_memory_t>()` in `pool_t`'s
  constructor) - it isn't behind an interface `pool_t` is oblivious to.
- `COLIB_ALLOCATOR_REPLACE_IMPL_2` gets pasted in place of the default `allocator_t<T>::allocate`/
  `::deallocate` member function template *definitions* (colib.h ~3958-3991 - the declarations
  themselves, in `allocator_t<T>`'s own struct body, are not replaceable; only what they do is).
  These are the two functions everything else in the library actually calls through, so this is the
  half that matters most for a real replacement.

Both halves are required together if `COLIB_ALLOCATOR_REPLACE` is set - there's no way to replace
just one piece (e.g. keep the default bucket type but swap `allocate`/`deallocate`'s policy) without
reimplementing both splice points to stay consistent with each other. No abstract interface or
concept describes the required shape anywhere - the only spec is "read the default implementation
between these two points and reproduce its symbol names/signatures."

**Agreed-on TODO, recorded directly in `colib.h`** just above the `COLIB_ALLOCATOR_REPLACE` defines
(colib.h ~583): collapse this to one customization point instead of two - a single named backend
type with a small fixed contract (construct + `alloc(size_t)` + `free(void*)`), with `alloc<T>`/
`dealloc_create<T>`/`allocator_t<T>::allocate`/`deallocate` becoming permanently-fixed glue that
forwards to it, expressible as a C++20 `concept` for a real compile error instead of a deep template
failure. Also needs rethinking `allocate()`/`deallocate()`'s malloc-fallback logic, which currently
infers "did this come from the backend" via a pointer-range check against
`sizeof(*allocator_memory)` - an implicit contiguous-single-blob assumption a custom backend
inherits without being told it exists. `COLIB_OS_UNKNOWN_IO_DESC`/`_IMPLEMENTATION` use the
identical raw-splice pattern for OS backends, so this is the library's general style for "user
supplies the missing piece," not an isolated choice - worth deciding, if this gets fixed, whether
that's a one-off exception or a signal to reconsider the pattern more broadly.

**Example** (this is mostly invisible to library users - it's used internally, e.g. for the internal
maps/vectors that back `pool_internal_t`/`io_pool_t`; shown here as how the library itself uses it):
```cpp
// roughly how colib.h itself declares an allocator-backed container:
std::map<int, fd_data_t*, std::less<int>, colib::allocator_t<std::pair<const int, fd_data_t*>>> m;
```

---

## Modifications (`modif_t`, `modif_e`, `create_modif`)

**What it is.** Callbacks hung off specific points in a coroutine's lifecycle:
`CALL`/`SCHED`/`EXIT`/`LEAVE`/`ENTER`/`WAIT_IO`/`UNWAIT_IO`/`WAIT_SEM`/`UNWAIT_SEM`.

**Correction (this was wrong in the first draft):** modifications are *not* the mechanism the
library is generally built out of - most of it is oblivious to them. Only a specific handful of
features are actually implemented on top of modifs: coroutine cancellation (`create_killer`),
timeouts (`create_timeo`, itself built on cancellation), tracing (`dbg_create_tracer`), and futures
(`create_future`). Semaphores, the IO pool, and the pool's own scheduling rules don't go through the
modif system at all - they're plain code paths with no callback dispatch in them. That's
deliberate: the design intent is that if you don't attach any modifications, you pay nothing for the
feature existing - it's opt-in overhead on specific coroutines, not a tax on every suspend/resume in
the library.

Worth remembering when weighing how urgent a modif-related bug is: bugs in the modif system tend to
be under-caught relative to bugs in the rest of the library, precisely because this is its
least-exercised corner. The handful of features that do use modifs (cancellation/timeouts/tracing/
futures) are genuinely liked and actively used, but that's still a narrow slice of the library's
overall surface compared to semaphores/IO/scheduling - so a latent ordering bug here can sit
unnoticed far longer than the same class of bug would in a heavily-trodden path. Not a reason to
consider modif bugs unimportant, just a reason they surface late.

A modification can be set to be inherited by coroutines this one *calls* and/or *schedules*
(`CO_MODIF_INHERIT_ON_CALL` / `CO_MODIF_INHERIT_ON_SCHED`, or-able together, or
`CO_MODIF_INHERIT_NONE`). This is how e.g. a timeout attached to one coroutine ends up effectively
covering everything it calls, without re-attaching it manually at every call site.

**API.**
- `create_modif<modif_e type>(flags, cbk)` → `modif_p` - build one modification.
- Attach at schedule time: `pool->sched(task, {mod1, mod2})`, or `co::sched(task, {...})`.
- Attach/inspect/remove on an existing task: `task_modifs(t)`, `add_modifs(pool, t, mods)`,
  `rm_modifs(t, mods)`; coroutine-form equivalents (`task_modifs()`, `add_modifs(mods)`,
  `rm_modifs(mods)`) act on *whichever coroutine `co_await`s them* - must be `co_await`-ed directly
  by the target, not scheduled.
- The callback signature depends on `type` - most are `error_e(state_t*)`, `WAIT_IO`/`UNWAIT_IO` add
  an `io_desc_t&`, `WAIT_SEM`/`UNWAIT_SEM` add a `sem_t*` (+ a waiter handle for `WAIT_SEM`).

**Example** (a minimal logging modif, inherited into everything the coroutine calls):
```cpp
auto log_on_enter = colib::create_modif<colib::CO_MODIF_ENTER_CBK>(
    colib::CO_MODIF_INHERIT_ON_CALL,
    [](colib::state_t *s) -> colib::error_e {
        std::cout << "entering: " << colib::dbg_name(s->self) << std::endl;
        return colib::ERROR_OK;
    });
pool->sched(some_coroutine(), {log_on_enter});
```

---

## Timing (`sleep*`, `create_timeo`)

**What it is.** Two related but distinct things: plain delays (`sleep`/`sleep_us`/`sleep_ms`/
`sleep_s`), and a timeout wrapper around another coroutine (`create_timeo`) that races it against a
timer and kills it (via the same `create_killer` machinery) if it doesn't finish in time.

**API.**
- `sleep(microseconds)` / `sleep_us(u64)` / `sleep_ms(u64)` / `sleep_s(u64)` - coroutines that
  suspend for the given duration, backed by an OS timer (`timerfd` on Linux, `SetWaitableTimer` on
  Windows) via `timer_pool_t`.
- `create_timeo(task<T> t, pool_t *pool, microseconds timeo)` → `task<std::pair<T, error_e>>` -
  schedules `t` and a timer; if the timer fires first, `t` is killed and the returned `error_e` is
  non-`ERROR_OK`. `T` must be default-constructible (used as the "didn't finish" placeholder).

**Example.**
```cpp
auto [value, err] = co_await colib::create_timeo(slow_coroutine(), pool,
        std::chrono::seconds(5));
if (err != colib::ERROR_OK) {
    // slow_coroutine() didn't finish within 5 seconds and was killed
}
```

---

## Call vs. Sched - coroutine control flow

Not a header section by itself, but the foundational distinction everything else (modifs'
inheritance flags, `create_killer`'s call-stack tracking, `create_future`) is built on top of. Two
different, deliberately different ways one coroutine can start another:

**Call** (`co_await some_task`, i.e. `task<T>`'s own `await_suspend`, colib.h ~2662-2684): a
*nested* relationship. The callee's `state_t::caller_state` is set to point at the caller's state,
`CO_MODIF_INHERIT_ON_CALL`-flagged modifs are inherited from caller to callee, and execution
symmetric-transfers directly into the callee (`return h` from `await_suspend` - no trip through the
ready queue). When the callee finishes, `final_awaiter_cleanup` (colib.h ~4026-4045) symmetric-
transfers control straight back to `caller_state->self` - also no trip through the ready queue. This
is what makes call a real stack: `create_killer`'s `call_stack` is built entirely out of watching
`CALL`/`SCHED` (push) and `EXIT` (pop) modif events, and a call chain's `caller_state` links are
exactly what lets a kill unwind through nested calls all the way back to the original caller.

**Sched** (`pool->sched(task)` / `co_await co::sched(task)`, colib.h ~4053-4055 and ~4181-4202): a
*fire-and-forget* relationship. The new task's `caller_state` is explicitly `nullptr`
(`pool_t::sched` calls `internal->sched(task, v, nullptr)`), `CO_MODIF_INHERIT_ON_SCHED`-flagged
modifs are inherited instead of the `ON_CALL` ones, and the task is pushed onto the ready queue to
be picked up whenever the scheduler gets to it - not run immediately. Notably, `sched_awaiter_t::
await_suspend` returns `bool` (not a `handle`), and returns `false` specifically - per the C++
coroutine spec that means *don't actually suspend*, so scheduling something doesn't pause the
scheduling coroutine at all; it just enqueues the new task and keeps running. When a sched'd task
finishes, `final_awaiter_cleanup` sees `caller_state == nullptr` and just posts itself for
destruction (`pool->get_internal()->post_to_destroy(...)`) and returns `noop_coroutine()` - control
falls back to the pool's own loop, not to anything specific, since nothing was waiting on it.

**Confirmed while writing `03_execution_model.md` (2026-08-15), worth stating precisely since it
wasn't obvious going in:** `caller_state->self` symmetric transfer on finish/yield is
*unconditional* whenever `caller_state` is set - `final_awaiter_cleanup`/`cpp_yield_awaiter` both
`return caller_state->self` directly, never through `next_task()`/the ready queue. `next_task()`
(or `noop_coroutine()` for `final_awaiter_cleanup`) only gets hit on the `caller_state == nullptr`
branch, i.e. a sched'd/caller-less task finishing - a different case entirely, since `caller_state`
is *only* ever set by `task<T>::await_suspend` (colib.h:2672), never by `pool_internal_t::sched()`.
Net effect: a coroutine only ever actually leaves the ready-queue/ready-to-be-scheduled machinery
when it hits a *true* wait (I/O, semaphore, sched with nothing else running, or a caller-less
finish) - a whole nested call chain, however deep, resolves inside a single `resume()` from the
scheduler's perspective, with no queue round-trips at any point in the chain.

**Also worth restating precisely for `pool_t::sched` vs. `co_await colib::sched`:** the "`pool_t::
sched` calls `internal->sched(task, v, nullptr)`" note above is about the *parent modif table*
argument, not `caller_state` (that one's always `nullptr` for sched regardless - `state_t`'s own
default, `sched()` never sets it). The `nullptr` third argument matters on its own:
`inherit_modifs` no-ops on a `nullptr` parent table (colib.h ~2464-2465), so `ON_SCHED`-flagged
modifs are **not** inherited via the plain `pool_t::sched()` method (no coroutine context to
inherit from - e.g. called from `main()`). They *are* inherited via `co_await colib::sched(task)`
(`sched_awaiter_t::await_suspend`, colib.h ~4191-4198), which passes the scheduling coroutine's own
`modif_table` as the parent. Same nominal operation, two call sites, different inheritance
behavior - easy to conflate "sched never inherits" with "sched never inherits *when called from
outside a coroutine*."

**Practical read:** `call` is what you reach for when you want a normal function-call-shaped
relationship (the caller logically "owns" and is blocked on the callee, and killing the caller
should tear down the callee too). `sched` is what you reach for when you want two coroutines running
genuinely independently on the same pool (no return-value relationship, no shared kill-stack).
`wait_all`/`create_future` exist specifically to bridge the gap - they let you `sched` something
independent while still getting a way to `co_await` its eventual result, without a `caller_state`
link.

---

## Flow Control (`create_sem`, `create_killer`, `create_future`, `wait_all`, `force_stop`)

**What it is.** The library's grab-bag of coordination primitives beyond plain call/await:
semaphores for waiting/signaling, killers for tearing down a coroutine's call stack on demand,
futures for "give me the result once this finishes," `wait_all` for joining several tasks, and
`force_stop` for pausing the whole pool's `run()` loop.

**API.**
- `create_sem(pool, val)` (or the `pool_p`/coroutine-form overloads) → `sem_p` - see the Semaphores
  section below.
- `create_killer(pool, e)` → `{modif_pack_t, std::function<error_e(void)>}` - attach the pack to
  exactly one coroutine; call the function to kill it (and its call stack) with error `e`.
  Single-target, single-use - see `understanding.md`'s (older draft) notes / `tests/BUGS.md` if
  more detail is wanted on why.
- `create_future(pool, t)` → `task<T>` - not itself awaitable; schedule `t` separately, then
  `co_await` the returned future to get `t`'s result once ready.
- `wait_all(tasks...)` → `task<std::tuple<...>>` - waits for every task; killing one kills all of
  them (each gets a killer installed).
- `force_stop(stopval)` - stops the enclosing `pool->run()` call; resumable with another `run()`.

**Example** (future + killer-backed timeout, roughly what `create_timeo` builds on internally):
```cpp
auto t = compute_something();
auto fut = colib::create_future(pool, t);
co_await colib::sched(t);
// ... do other work ...
auto result = co_await fut;  // blocks (suspends) until compute_something() finishes
```

---

## Externals (`external_init_task`, `external_on_suspend`/`on_resume`, ...)

**What it is.** An escape hatch for driving coroutines from outside the normal `sched`/`co_await`
flow - e.g. resuming the coroutine engine from inside a callback that isn't itself a coroutine
(a C API callback, a GUI event handler, etc).

**API.**
- `external_init_task(task, pool)` / `external_init_task(state, pool)` → `state_t*` - initialize a
  task to run on `pool` without scheduling it.
- `external_on_suspend(handle)` → `state_t*` - call from a custom awaitable's `await_suspend` to
  register the suspension with the library.
- `external_on_resume(state)` → `coroutine_handle<void>` - resume a coroutine previously suspended
  via `external_on_suspend`.
- `external_sched_resume(state)` - mark it ready without resuming immediately (mutually exclusive
  with `external_on_resume` per state).
- `external_has_next_task(pool)` / `external_wait_next_task(pool)` - poll / blocking-fetch the next
  runnable coroutine, for driving the pool's own scheduling loop manually.

**Example** (`colib.h`'s own doc comment gives the canonical shape - a callback that needs to kick
the coroutine engine):
```cpp
void callback(user_ctx_t *ctx) {
    colib::pool_t *pool = ctx->pool;
    auto continuation = []() -> co::task_t {
        co_await dependency_awaiter();
        co_return 0;
    }();
    colib::external_init_task(continuation.h.promise().state, pool);
    continuation.h.resume();
    continuation.h.destroy();
}
```

---

## OS Dependence - what's platform-specific and what isn't

Most of the *public* API is not OS-dependent at all - `task<T>`, `pool_t`, `sem_t`, modifications,
`create_killer`/`create_timeo`/`create_future`/`wait_all`, `sleep*` - all of that is plain C++ that
happens to be *implemented* on top of something OS-dependent underneath, without exposing OS
differences at the call site. You write the same `co_await colib::sleep_ms(100)` regardless of
platform.

What's actually OS-dependent, concretely:

- **`io_desc_t`'s shape** - three incompatible struct definitions (Linux/Windows/Unix), each shaped
  around what its own backend needs (see "`io_desc_t`" below). Only relevant if you're touching raw
  I/O descriptors directly (`wait_event`, `stop_io`) rather than the named wrapper functions.
- **`io_pool_t` and `timer_pool_t`** - the actual epoll/IOCP/kqueue/timerfd/SetWaitableTimer
  mechanics. Entirely internal; nothing about their implementation leaks into the public API's shape.
- **The I/O wrapper functions themselves** - Linux/Unix get POSIX-shaped ones (`connect`/`accept`/
  `read`/`write`/`read_sz`/`write_sz`, plus `stop_fd`), Windows gets WinAPI/Winsock-shaped ones
  (`ConnectEx`/`AcceptEx`/`ReadFile`/`WriteFile`/the `WSA*` family/named-pipe/device-control
  functions, plus `stop_handle`) - **and** a set of Linux-API-shaped adapter functions
  (`connect`/`accept`/`read`/`write`/`read_sz`/`write_sz` taking a `HANDLE`) so Windows code can be
  written against the same names as Linux code, for the common subset of operations both platforms
  support.

Selected at compile time via `COLIB_OS_LINUX`/`COLIB_OS_WINDOWS`/`COLIB_OS_UNIX`/`COLIB_OS_UNKNOWN`
(auto-detected from compiler/platform macros unless overridden - see `02_api.md`'s Config Macros).

**Unix (kqueue) is the one that isn't actually done.** It's a real `#if COLIB_OS_UNIX` branch, wired
into the same makefile targets (`make unix`, `make unix_kqueue`) as the finished backends, and reads
like a complete third option next to epoll and IOCP - but `io_pool_t::force_awake()` and `clear()`
are empty-bodied stubs there (`force_awake` doesn't even `return` on any path, despite being
declared to return `error_e` and having callers that use the return value). Concretely: killing or
force-stopping a coroutine that's currently parked on I/O silently does nothing on a
`COLIB_OS_UNIX`/kqueue build. See `tests/BUGS.md` #4 - not yet reproduced from this repo (no
kqueue-capable environment available). Worth stating plainly in whatever chapter covers this, not
just implying "three symmetric backends" the way the header's `#if` ladder does.

---

## IO Pool (platform backends: epoll / IOCP / kqueue)

**What it is.** The part of the pool that implements OS-specific async I/O: register interest in an
fd/handle plus a set of events, and get a callback (really: a coroutine resume) when one of those
events is ready/complete. One `io_pool_t` per `pool_t`, selected the same way as described above.

**API** (the public-facing part - the backend itself is internal): `wait_event(io_desc)` (wait on an
arbitrary event the backend supports but this library doesn't name a wrapper for) and `stop_io`/
`pool_t::stop_io` (cancel a pending wait) are cross-platform; everything else is the platform-specific
wrapper functions listed under "OS Dependence" above and broken down by purpose below.

### `io_desc_t` - what it actually needs to be

`io_desc_t` is **not** used through a shared abstract interface anywhere in the library - it's an
opaque, per-backend token. Generic/OS-agnostic code (`io_awaiter_t`, `wait_event`, `stop_io`,
`pool_internal_t::wait_io`/`stop_io`/`get_timer`/`set_timer`) only ever stores it by value and passes
it straight through to whichever OS-specific `io_pool_t`/`timer_pool_t` is compiled in; it never
inspects a field or calls a method on it itself. Every field access (`io_desc.fd`, `io_desc.ident`,
`io_desc.data->h`, ...) happens *inside* the OS block that also defines that particular `io_desc_t`
variant - the type and the code that operates on it are written together, in lockstep, and nothing
outside that pairing touches either.

Practical consequence for `COLIB_OS_UNKNOWN`: there's no checklist of methods `io_desc_t` "must"
implement in the way, say, an iterator has a required set of operations. The real requirement is
narrower and more circular: **your `io_desc_t` needs to be whatever shape your own
`COLIB_OS_UNKNOWN_IMPLEMENTATION` (the matching custom `timer_pool_t`/`io_pool_t`) needs it to be**,
since you're writing both together. The three existing variants all happen to additionally provide
`is_valid()` and `operator==` as a matter of internal convention/consistency across the three of
them - not because any generic call site requires it. If a chapter documents this, it should be
honest that it's "look at the three existing implementations and match their shape/spirit," not
"implement this interface."

### Breakdown of what I/O is actually available

Grouped by purpose rather than by platform (see `02_api.md`'s "Cross-platform I/O" tables for exact
signatures):

- **Generic wait/cancel** (both platforms): `wait_event` (arbitrary event escape hatch), `stop_io`
  (cancel a pending wait without closing the underlying fd/handle).
- **Connection setup**: `connect`/`accept` (Linux: direct POSIX wrappers; Windows: `ConnectEx`/
  `AcceptEx` plus the `HANDLE`-taking adapters of the same names).
- **Data transfer, byte-count-agnostic**: `read`/`write` (Linux: direct POSIX wrappers returning
  whatever the syscall returns, not just `error_e` - see the "special" comment in `colib.h`; Windows:
  `ReadFile`/`WriteFile` plus the `HANDLE`-taking `read`/`write` adapters).
- **Data transfer, exact-length**: `read_sz`/`write_sz` - loop the above until the full requested
  length transfers or the connection errors/closes, on both platforms.
- **Eviction before close**: `stop_fd` (Linux/Unix) / `stop_handle` (Windows) - must be called to
  evict an fd/handle from the pool's engine before actually closing it, or the whole event queue
  errors out.
- **Windows-only, no Linux equivalent wrapped**: named pipes (`ConnectNamedPipe`,
  `TransactNamedPipe`), directory-change notification (`ReadDirectoryChangesW`), arbitrary device
  I/O control (`DeviceIoControl`), byte-range file locking (`LockFileEx`), serial/comm event waiting
  (`WaitCommEvent`), and the raw Winsock `WSASend`/`WSASendTo`/`WSASendMsg`/`WSARecv`/`WSARecvFrom`/
  `WSARecvMsg` family for scatter/gather buffers, flags, and out-of-band message send/receive beyond
  what plain `ReadFile`/`WriteFile` expose.

### How epoll and IOCP actually get wrapped into one coroutine model

This is the part worth explaining at length, because epoll and IOCP are not just "the same idea,
different API" - they're built on genuinely different completion models, and `io_awaiter_t` has to
paper over that difference to present one `co_await`-shaped interface.

**epoll (readiness-based).** "Registering a wait" and "performing the operation" are two separate
steps, done at two separate times. `io_awaiter_t::await_suspend` calls `do_wait_io_modifs`, then
`pool_internal_t::wait_io` → `io_pool_t::add_waiter`, which just does an `epoll_ctl`
`EPOLL_CTL_ADD`/`_MOD` telling the kernel "wake me when this fd is readable/writable" - no actual
`read`/`write` syscall happens yet. Only once the coroutine is later woken (told the fd is *ready*)
does the wrapper function (e.g. `colib::read`, colib.h ~4855-4866) perform the real `::read()`/
`::write()` syscall itself, synchronously, trusting that it won't block since epoll just said it's
ready. This is the classic "level/edge-triggered readiness" model: the kernel tells you *when you can
act without blocking*, you still do the acting.

**IOCP (completion-based).** The opposite order: "issue the operation" and "register the wait" are
the *same step*, and what you get notified about is completion, with the result already in hand -
not mere readiness. Concretely, every Windows wrapper (e.g. `ReadFile`, colib.h ~5213-5246) builds an
`io_desc_t`/`io_data_t` and stashes a small lambda in `io_data_t::io_request` that, when invoked,
calls the *real* `::ReadFile()` (or whichever WinAPI/Winsock-extension function) immediately, passing
the `OVERLAPPED` struct embedded in `io_data_t`. That lambda gets invoked from inside `add_waiter`
itself - so "registering the wait" is actually "issue the real async syscall right now." The
coroutine then suspends until IOCP's completion port reports the operation finished (via
`GetQueuedCompletionStatusEx`), at which point the byte count/result is already available - there's
no second syscall to perform afterward the way there is on Linux.

**Why `io_awaiter_t` doesn't need to know the difference.** It only ever calls `do_wait_io_modifs` →
`pool_internal_t::wait_io` → (backend-specific registration) → suspend → resume → `do_unwait_io_modifs`.
Whether "registration" merely arms a readiness watch (epoll) or actually performs the whole operation
(IOCP) is entirely hidden inside what `add_waiter`/the stashed `io_request` do for that backend - the
awaiter's own shape, and the modif ordering it drives (`WAIT_IO`/`LEAVE`/`ENTER`/`UNWAIT_IO`, see the
Modifications section above), is identical either way. This is *why* the Windows wrapper functions
have to build their own `io_request` closures instead of just calling a shared "do the syscall"
helper the way the Linux wrappers do - the "do the syscall" step has to happen at a different point
in the sequence depending on the backend, and IOCP's version of that step needs the `OVERLAPPED`/
completion-port machinery epoll doesn't have.

**Example** (raw fd wait, Linux):
```cpp
ssize_t n = co_await colib::read(fd, buffer, sizeof(buffer));
if (n < 0) { /* error, see error_e / errno */ }
```

---

## Semaphores (`sem_t`, `create_sem`)

**What it is.** A counting semaphore scoped to a pool, unusual in that it can be initialized to a
*negative* value - useful for "N waiters need to wait for something that hasn't happened yet"
patterns where you know the waiter count up front.

**API.**
- `create_sem(pool, val)` / `create_sem(pool_p, val)` / `create_sem(val)` (coroutine form, uses the
  running coroutine's pool) → `sem_p`.
- `sem_t::wait()` - awaitable; decrements the counter (or suspends until it can), resolves to an
  `unlocker_t` usable in a `std::lock_guard`.
- `sem_t::signal(inc = 1)` - see the API reference for the exact increment/decrement/wake rules
  (there's an open question about the `inc == 0` boundary case worth checking before relying on it).
- `sem_t::try_dec()` - non-blocking version of `wait()`.
- `sem_t::signal_all()` / `sem_t::clear(val)` - wake everyone / forcefully unwind every waiter and
  reset the counter.

**Example** (mutual exclusion via the `unlocker_t` + `lock_guard` pattern):
```cpp
colib::sem_p mutex = colib::create_sem(pool, 1);
{
    std::lock_guard guard(co_await mutex->wait());
    // critical section
} // guard's destructor calls unlocker_t::unlock() -> mutex->signal()
```

---

## Debugging (`dbg_*`, `COLIB_ENABLE_DEBUG_CHECKS`, tracer)

**What it is.** Two mostly-separate things bundled under "debugging": human-readable
naming/logging (`dbg_name`, `dbg_enum`, `COLIB_ENABLE_DEBUG_NAMES`/`COLIB_REGNAME`,
`dbg_create_tracer`), and an internal correctness-checking state machine
(`COLIB_ENABLE_DEBUG_CHECKS`) that validates the `entered`/`left`/`io`/`sem` lifecycle of every
coroutine on every modif callback and aborts on violation.

**API.**
- `COLIB_REGNAME(x)` / `dbg_register_name(...)` - attach a name to a task/handle/address (only when
  `COLIB_ENABLE_DEBUG_NAMES`).
- `dbg_name(...)` - read a registered name back out.
- `dbg_enum(error_e | run_e)` - human-readable string for a result code.
- `dbg_create_tracer(pool)` → `modif_pack_t` - attach to a coroutine to log every modif point it (and
  what it calls/schedules) hits.
- `COLIB_ENABLE_DEBUG_CHECKS` - not really a logging feature; worth documenting as its own thing
  since it's the only place several ordering invariants between modif callbacks are actually
  enforced, rather than just a verbosity toggle.

**Example.**
```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#include "colib.h"
// ...
pool->sched(COLIB_REGNAME(my_coroutine()));
pool->sched(colib::dbg_create_tracer(pool) /* attach to whatever coroutine needs tracing */);
```
