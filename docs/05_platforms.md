# 05. Platforms: Windows, Linux, and the WIP Unix/kqueue Engine

Earlier chapters describe behavior that's the same on every platform - `task<T>`, `pool_t`, `sem_t`,
call/sched, modifications, lifetimes. None of that is OS-dependent at the call site: you write
`co_await colib::sleep_ms(100)` or `co_await colib::read(fd, buf, len)` the same way regardless of
platform. This chapter is about what's actually underneath those calls - the three
(`COLIB_OS_LINUX`/`COLIB_OS_WINDOWS`/`COLIB_OS_UNIX`) real backends, the fourth
(`COLIB_OS_UNKNOWN`) extension point, and where each backend genuinely differs from the others rather
than just being "the same idea in different syntax." Line references are against `colib.h` as of
commit `fd519a6` - see `progress.md`.

---

## Selecting a backend

`COLIB_OS_LINUX`/`COLIB_OS_WINDOWS`/`COLIB_OS_UNIX` are auto-detected from compiler/platform macros
unless overridden (see `02_api.md`'s Config Macros). `COLIB_OS_UNKNOWN` is the same extension point,
just with no default - set it plus `COLIB_OS_UNKNOWN_IO_DESC` and `COLIB_OS_UNKNOWN_IMPLEMENTATION` to
supply your own `io_desc_t` shape and `timer_pool_t`/`io_pool_t` implementation entirely (see
`TODO.md` #2 for a worked-through example of what that would take for an ASIO-based backend).

Each of the four is a separate, independent `#if COLIB_OS_*` block wherever the library needs
OS-specific code - not an `#if`/`#elif` chain. `io_desc_t`, `io_pool_t`, and `timer_pool_t` each get
defined once per active block (colib.h ~1013-1084 for `io_desc_t`; ~2737-3733 for `io_pool_t`/
`timer_pool_t`, in the order Unix, Linux, Windows, Unknown). Exactly one `COLIB_OS_*` macro is meant to
be true at a time - the header doesn't guard against more than one being set, so setting two at once
is a duplicate-definition compile error, not a supported configuration.

---

## `io_desc_t`: not a shared interface, three unrelated shapes

`io_desc_t` is **not** used through any shared abstract interface anywhere in the library - it's an
opaque, per-backend token. Generic/OS-agnostic code (`io_awaiter_t`, `wait_event`, `stop_io`,
`pool_internal_t::wait_io`/`stop_io`/`get_timer`/`set_timer`) only ever stores it by value and passes
it straight through to whichever `io_pool_t`/`timer_pool_t` is compiled in; it never inspects a field
or calls a method on it. Concretely, the three real shapes (colib.h ~1017-1077) share nothing:

- **Linux (epoll):** `{ int fd; uint32_t events; }` - a bare fd plus an epoll event mask.
- **Windows (IOCP):** `{ HANDLE h; shared_ptr<io_data_t> data; }`, where `io_data_t` embeds an
  `OVERLAPPED` (must be the struct's first member, per IOCP's own requirement), the transfer byte
  count, and a stashed `std::function<error_e(void*)> io_request` - the closure that performs the
  *real* async syscall once invoked (see "How epoll and IOCP differ" below).
- **Unix (kqueue):** `{ uintptr_t ident; short filter; unsigned int fflags; intptr_t data; }` - a
  kqueue `kevent`'s identifying fields directly.

The three variants all happen to provide `is_valid()` (and Linux/Windows also `operator==`) as a
matter of convention across the three of them, not because any generic call site requires it - there's
no checklist of methods `io_desc_t` "must" implement. For `COLIB_OS_UNKNOWN`, the real requirement is
narrower and circular: your `io_desc_t` needs to be whatever shape your own
`COLIB_OS_UNKNOWN_IMPLEMENTATION` needs it to be, since you write both together - "match the shape and
spirit of the three existing ones," not "implement an interface."

---

## `io_pool_t`/`timer_pool_t`: what each backend actually has to provide

Every backend implements the same small set of member functions, called only from the OS-agnostic
awaiters (`io_awaiter_t`) and `pool_internal_t`, never called differently per platform at the call
site: `add_waiter(state, io_desc)`, `force_awake(io_desc, retcode)`, `handle_ready()` (on `io_pool_t`),
and `get_timer`/`set_timer`/`free_timer` (on `timer_pool_t`). What each of those actually *does*
differs sharply between the readiness-based and completion-based models.

### Linux (epoll) - readiness-based

"Registering a wait" and "performing the operation" are two separate steps, at two separate times.
`add_waiter` (colib.h ~2935-2991) just does an `epoll_ctl` `EPOLL_CTL_ADD`/`_MOD` - "wake me when this
fd is readable/writable" - no `read`/`write` syscall happens yet. `handle_ready()` (colib.h
~2868-2934, covered in `03_execution_model.md`'s "Where I/O and timers fit in") is a no-op if
`ready_tasks` already has entries, and otherwise blocks in `epoll_wait(..., -1)` until the kernel
reports something ready, then pushes the matching coroutines onto `ready_tasks`. Only once the
coroutine is later actually resumed does the wrapper function (e.g. `colib::read`, colib.h ~4740-4753)
perform the real `::read()`/`::write()` syscall, synchronously, trusting epoll's word that it won't
block. `force_awake` (colib.h ~2992-3020ish) pushes the waiting coroutine onto `ready_tasks` with the
given error, for cancellation. `timer_pool_t` (colib.h ~3140-3202) is `timerfd`-based - a timer is just
another fd registered the same way, which is exactly why a pending sleep is what keeps
`handle_ready()`'s blocking wait alive instead of returning immediately.

### Windows (IOCP) - completion-based

The opposite order: "issue the operation" and "register the wait" are the *same step*, and what you
get notified about is completion, with the result already in hand - not mere readiness. Every Windows
wrapper (e.g. `ReadFile`, colib.h ~5216-5246) builds an `io_desc_t`/`io_data_t` and stashes a lambda in
`io_data_t::io_request` that, when invoked, calls the *real* `::ReadFile()` (or whichever WinAPI/
Winsock-extension function) immediately, passing the embedded `OVERLAPPED`. `add_waiter` (colib.h
~3364-3414ish) is what actually invokes that stashed `io_request` - so "registering the wait" *is*
"issue the real async syscall right now." The coroutine then suspends until IOCP's completion port
reports the operation finished (`handle_ready()`, colib.h ~3270-3363ish, via
`GetQueuedCompletionStatusEx`), at which point the byte count/result is already available - no second
syscall afterward the way there is on Linux. `force_awake` (colib.h ~3415-3577ish) cancels the pending
overlapped operation. `timer_pool_t` (colib.h ~3577-3667) uses `SetWaitableTimer`, tied into the same
completion port as everything else so a pending timer is what keeps `handle_ready()`'s blocking wait
alive, same role `timerfd` plays on Linux.

### Why `io_awaiter_t` doesn't need to know the difference

`io_awaiter_t::await_suspend` (colib.h ~4248 onward) only ever calls `do_wait_io_modifs` (colib.h
~2533-2539) → `pool_internal_t::wait_io` → (backend-specific `add_waiter`) → suspend → resume →
`do_unwait_io_modifs` (colib.h ~2540-2546). Whether "registration" merely arms a readiness watch
(epoll) or performs the whole operation (IOCP) is entirely hidden inside what each backend's
`add_waiter`/stashed `io_request` do - the awaiter's own shape, and the modif ordering it drives
(`WAIT_IO`/`LEAVE`/`ENTER`/`UNWAIT_IO`), is identical either way. This is *why* the Windows wrapper
functions have to build their own `io_request` closures instead of sharing a "do the syscall" helper
with Linux the way `read`/`write` do - the "do the syscall" step has to happen at a genuinely different
point in the sequence depending on the backend.

---

## Unix (kqueue): what's real and what's a stub

`io_pool_t::add_waiter` (colib.h ~2785-2796) is implemented - it registers a `kevent` the same
readiness-based way epoll does (`EVFILT_READ`/`EVFILT_WRITE` instead of `EPOLLIN`/`EPOLLOUT`).
**`force_awake` and `clear` are not** (colib.h ~2797-2802):

```cpp
error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
    /* TODO: figure it out, for this and for the others, maybe I can find a way not to use
    a map */
}
error_e clear() {}
```

Both bodies are empty - `force_awake` doesn't even `return` on any path, despite being declared to
return `error_e` and having callers that use the return value (`pool_internal_t::stop_io`).
Concretely: **killing or force-stopping a coroutine that's currently suspended on I/O silently does
nothing on a `COLIB_OS_UNIX`/kqueue build.** `timer_pool_t` (colib.h ~2814-2827) is a stub too - every
one of `get_timer`/`set_timer`/`free_timer` is an empty body. `dbg_to_str(const io_desc_t&)` for the
kqueue variant (colib.h ~6168-6172) is also unimplemented, returning a fixed
`"NOT_IMPLEMENTED_TO_STR"` string - even debug output for this backend isn't done.

This is worth stating plainly rather than implying three symmetric backends: as things stand, a
`make unix`/`make unix_kqueue` build compiles and can register I/O waits, but has no working timers and
no way to cancel a pending wait. See `tests/BUGS.md` #4 for the confirmed-bug framing and `TODO.md` #1
for the smaller open pieces (the "maybe I can find a way not to use a map" design question left
in `force_awake`'s own TODO, and `dbg_to_str`).

---

## The wrapper functions: what's actually callable, and one real duplication

Grouped by purpose (see `02_api.md`'s cross-platform I/O tables for exact signatures):

- **Generic wait/cancel** (all platforms): `wait_event` (arbitrary event escape hatch), `stop_io`
  (cancel a pending wait without closing the underlying fd/handle).
- **Connection setup:** `connect`/`accept` - Linux/Unix: direct POSIX wrappers; Windows: `ConnectEx`/
  `AcceptEx` plus `HANDLE`-taking adapters of the same names, so Windows code can be written against
  the same call shape as Linux code for the operations both support.
- **Data transfer, byte-count-agnostic:** `read`/`write` (Linux/Unix: direct POSIX wrappers returning
  whatever the syscall returns, not just `error_e`; Windows: `ReadFile`/`WriteFile` plus the
  `HANDLE`-taking adapters).
- **Data transfer, exact-length:** `read_sz`/`write_sz` - loop the above until the full length
  transfers or the connection errors/closes, on both platforms.
- **Eviction before close:** `stop_fd` (Linux/Unix, colib.h ~4768/~4885) / `stop_handle` (Windows,
  colib.h ~5009) - must be called to evict an fd/handle from the pool's engine before actually closing
  it, or the whole event queue errors out.
- **Windows-only, no Linux/Unix equivalent wrapped:** named pipes (`ConnectNamedPipe`,
  `TransactNamedPipe`), directory-change notification (`ReadDirectoryChangesW`), arbitrary device I/O
  control (`DeviceIoControl`), byte-range file locking (`LockFileEx`), serial/comm event waiting
  (`WaitCommEvent`), and the raw Winsock `WSASend`/`WSASendTo`/`WSASendMsg`/`WSARecv`/`WSARecvFrom`/
  `WSARecvMsg` family for scatter/gather buffers and out-of-band messages beyond plain
  `ReadFile`/`WriteFile`.

**The Linux and Unix wrapper blocks (colib.h ~4658-4773 and ~4775-4890) are two separate, independent
`#if` blocks - not shared via `#if COLIB_OS_LINUX || COLIB_OS_UNIX` the way, say, `stop_io`'s
generic path is (colib.h ~4892-4932).** They read as near-duplicates of each other - same function
names, same non-blocking-`connect()` dance, same control flow - and a line-count diff confirms it:
both blocks are exactly 115 lines. But diffing the bodies shows the duplication is *not* accidental
copy-paste laziness: every actual difference between the two is exactly where the two backends'
`io_desc_t` shapes differ - `io_desc_t{.fd = fd, .events = EPOLLOUT}` on Linux becomes
`io_desc_t{.ident = (uintptr_t)fd, .filter = EVFILT_WRITE}` on Unix, and so on for every event
registration in the block. In other words: the *algorithm* is identical and could in principle be
shared, but each backend's version has to embed a differently-shaped `io_desc_t` literal inline at
several points, which is exactly the "type and the code that operates on it are written together, in
lockstep" relationship `io_desc_t` has everywhere else in the library (see above) - it just happens to
produce two nearly-identical-looking function bodies here instead of a single generic one.
