# Proposal: io_data_t closes its event handle (fix #06)

Status: [APPLIED 2026-09-29 on the user's word — the first fix introduced under tests/fixes/]

## The bug

Every Windows io wrapper (the sixteen `ReadFile`/`WriteFile`/`ConnectEx`/`WSA*`-style functions at
the bottom of colib.h) builds its overlapped request on an `io_data_t` minted by `create_io_desc`,
sets `overlapped.hEvent` to a fresh `CreateEvent`/`WSACreateEvent` handle, and then co_awaits. The
only `CloseHandle` of that event in the whole Windows io path sits in `handle_done_req()`, which
each wrapper calls *after* the co_await resumes. A waiter that is killed while suspended never runs
that code: `killer_state_t::kill()` takes the `STATE_WAITING_IO` branch, `stop_io()` ->
`io_pool_t::force_awake()` cancels the request and the io pool drops its `shared_ptr<io_data_t>` in
`awake_io`/`dequeue_data`, and `end_chain()` -> `destroy_state()` destroys the suspended wrapper
frame without resuming it — the frame's `io_desc_t` held the last reference, and `io_data_t` had no
destructor, so the event handle leaked with the freed request. One kernel handle per killed io op,
deterministic; the resumed path was always clean because there `handle_done_req()` runs.

Confirmed: `repro/06-winevent_handle_leak.md` — 300 timed-out `co::read`s (a pipe whose client
never writes, wrapped in `create_timeo`), +300 handles exactly, control loop flat, two identical
runs. Re-confirmed fresh against the base copy in this proposal's workspace
(`repro/fix/06/06-run-base.log`: `kill: handles 53 -> 353 (growth 300)`).

The same wrong step has a second face by reading the code: `io_pool_t::clear()` at pool teardown
also destroys parked waiter frames without resuming them (`destroy_state(data->state)` with the
handles map still holding the references until `handles.clear()`), so a pool destroyed with io
still parked bleeds the same event handles. The fix below covers both faces with the same owner,
because it does not care *why* the last reference dropped.

## Where the fix lands

| colib.h region                                | what changes                                                    | +adds/-dels |
|-----------------------------------------------|-----------------------------------------------------------------|-------------|
| `io_data_t` (inside `#if COLIB_OS_WINDOWS`)   | gains a destructor that closes `overlapped.hEvent` when set     | +10/-0      |
| `handle_done_req()` (Windows wrappers' tail)  | clears `overlapped.hEvent` after each of its two closes         | +2/-0       |

Both hunks live inside Windows-only blocks; the epoll/kqueue/unknown backends compile the same
bytes as before. Total: +12/-0.

## The change

```diff
--- colib-base.h	2026-09-28 23:11:18.189348700 +0300
+++ colib-patched.h	2026-09-28 23:15:07.675441600 +0300
@@ -1086,6 +1086,16 @@
     void *ptr = nullptr;                            /*!< can be context for io_request or timer
                                                          info */
     HANDLE h = NULL;                                /*!< same as in `io_desc_t` */
+
+    /*! Releases the event of the request, if `overlapped.hEvent` was set: the event is owned by
+     * this structure and is closed when it dies, on every exit - also the one where a waiter is
+     * destroyed suspended (a killed waiter), whose own code after the wait never runs. A waiter
+     * that resumed released the event earlier, in handle_done_req, which clears the field for
+     * exactly this check. */
+    ~io_data_t() {
+        if (overlapped.hEvent)
+            CloseHandle(overlapped.hEvent);
+    }
 };
 
 /*!
@@ -5393,6 +5403,7 @@
         if (len)
             *len = 0;   /* as the sync Win32 calls leave it on a failure */
         CloseHandle(data->overlapped.hEvent);
+        data->overlapped.hEvent = NULL;   /* released here: the destructor closes what is set */
         COLIB_DEBUG("FAILED: error %lu", data->win_err);
         if (data->win_err != ERROR_SUCCESS)
             SetLastError(data->win_err);    /* last, as the Win32 counterpart leaves it */
@@ -5404,6 +5415,7 @@
         COLIB_DEBUG("Failed to close event: %s", get_last_error().c_str());
         return ERROR_GENERIC;
     }
+    data->overlapped.hEvent = NULL;   /* released here: the destructor closes what is set */
     if (offset) {
         *offset = 0;
         *offset |= data->overlapped.Offset;
```

Hunk 1 gives the event handle a single deterministic owner. The `handles` map and the suspended
awaiter both hold a `shared_ptr<io_data_t>`; whoever drops the last reference now also releases the
event, so every exit — resumed, killed, errored, pool teardown — closes it exactly once. The
destructor closes only `overlapped.hEvent`, never `h`: the file/socket handle stays the caller's,
and the timer handle stays `kill_timer`'s (a timer request never sets `hEvent`, so the destructor
is a no-op for it). The destructor stays silent on a failed close on purpose: `dbg` is not declared
yet at `io_data_t`'s point in the header, and a close failing there means the handle value was
already dead — nobody can act on it.

Hunks 2 and 3 make the existing closes idempotent. `handle_done_req` keeps closing the event itself
(so a resumed wrapper still reports a failed close exactly as before, and the close still happens
at the same instant it always did) and then clears the field, so the later frame destruction finds
nothing to close. In the success branch a failed `CloseHandle` returns early without clearing; the
destructor then attempts a second close of that already-dead value, which fails harmlessly and
silently — the first failure was already reported to the caller.

Nothing else in the file mints, transfers, or closes this handle: the sixteen wrappers assign it
and call `handle_done_req`; `stop_io`/`force_awake`/`clear` cancel through `data->h` and never read
`hEvent` (the only other reference is a debug trace print); `WSACreateEvent` handles were already
being closed with `CloseHandle` in `handle_done_req`, and the destructor keeps that exact
treatment. The destructor runs at the moment the `io_data_t` memory was already being released, and
every path that reaches it has first finished the request at the OS level — `force_awake`'s
`CancelIoEx` + `GetOverlappedResult(TRUE)` + `handle_ready_events` drain (the drain repro/01
verified as safe), `clear`'s cancel, or a normal completion before the resume — so no code and no
kernel completion touches the event after it closes.

## The regression test

Lands as `tests/018-029-reproduced_winevent_handle_leak.cpp` when the fix is applied (category 18,
next free slot; the existing 18- set runs through 018-028). Pre-fix it FAILS: kill growth 300 hits
`kill_growth <= test83_iters / 10`, exactly the leak. Post-fix it passes unchanged (run twice:
growth 0 both times). I ran it against both the unpatched and the patched copy — logs below. The
workspace copy in `repro/fix/06/` is this file verbatim except the include is `"colib.h"` instead
of `"../colib.h"`, so it builds against the workspace header.

Its watchdog moved above the Windows block on 2026-10-14, on the user's word: `main()` starts it
on every platform, so inside the block the file didn't compile on Linux, and every Linux run had
to leave it out. The listing below is the landed file, with that move.

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <atomic>
#include <cstdio>
#include <thread>

/* Test83 - Reproduced Bugs: a killed Windows io request leaked its event handle
================================================================================================= */

/* Every Windows io wrapper creates an event handle for its overlapped request before its
co_await and closed it only in handle_done_req(), which runs after the co_await resumes. A waiter
killed while suspended (a create_timeo timeout here) never runs that code: killer_state_t::kill()
-> stop_io() -> io_pool_t::force_awake() cancels the request and the io pool drops its
shared_ptr<io_data_t>, then end_chain() destroys the suspended frame - the last reference - and
io_data_t had no destructor, so the event handle leaked with the freed request. Exactly one kernel
handle per killed io op, deterministic (repro/06-winevent_handle_leak.md: 300 killed reads, +300
handles, two identical runs; the completing control loop flat, because there handle_done_req()
runs).

The fix gives io_data_t a destructor that closes overlapped.hEvent: the handle now dies with its
single owner on every exit, resumed or killed, and handle_done_req() clears the field after its
own close. The killed loop below must therefore stay as flat as the completing one; before the
fix this test failed with kill growth == test83_iters (one leaked handle per timed-out read). */

static std::atomic<const char *> test83_wd_state{"startup"};

static void test83_watchdog_main() {
    for (int i = 0; i < 150; i++)   /* 15 s in 100 ms slices */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("018-029-reproduced_winevent_handle_leak.cpp: TIMED OUT after 15s while '%s' - "
           "the kill loop stopped waking\n", test83_wd_state.load());
    std::_Exit(1);
}

#if COLIB_OS_WINDOWS

static const int test83_iters = 300;

static int test83_ctrl_done = 0;   /* control reads that returned their byte */
static int test83_kill_done = 0;   /* timed-out reads that came back */
static int test83_kill_timeo = 0;  /* ...with err == ERROR_TIMEO */

static int test83_handle_count(DWORD *cnt) {
    if (!GetProcessHandleCount(GetCurrentProcess(), cnt)) {
        DBG("GetProcessHandleCount: %lu", GetLastError());
        return -1;
    }
    return 0;
}

static co::task_t test83_read_completes(HANDLE rd) {
    char buff[64];
    auto ret = co_await co::read(rd, buff, sizeof(buff));
    if (ret == 1)
        test83_ctrl_done++;
    co_return 0;
}

static co::task_t test83_read_killed(HANDLE rd) {
    co::pool_t *pool = co_await co::get_pool();
    char buff[64];
    auto [ret, err] = co_await co::create_timeo(co::read(rd, buff, sizeof(buff)), pool,
            std::chrono::microseconds(2000));
    (void)ret;
    test83_kill_done++;
    if (err == co::ERROR_TIMEO)
        test83_kill_timeo++;
    co_return 0;
}

static int test83_winevent_handle_leak() {
    char pipe_name[128];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\test83_leak_%lu",
            GetCurrentProcessId());

    /* an overlapped server read-end; the client connects and never writes, so every read on rd
     * pends until the timeout kills it */
    HANDLE rd = CreateNamedPipeA(pipe_name,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
            1, 1024, 1024, NMPWAIT_USE_DEFAULT_WAIT, NULL);
    ASSERT_FN(CHK_HANDLE(rd));
    HANDLE wr = CreateFileA(pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ASSERT_FN(CHK_HANDLE(wr));
    FnScope close_pipes([rd, wr] { CloseHandle(rd); CloseHandle(wr); });

    auto pool = co::create_pool();

    /* control: the read completes, the wrapper resumes, handle_done_req() runs after the
     * co_await - this loop was already flat before the fix and must stay flat */
    test83_wd_state = "control loop";
    DWORD ctrl_before = 0, ctrl_after = 0;
    ASSERT_FN(test83_handle_count(&ctrl_before));
    for (int i = 0; i < test83_iters; i++) {
        char byte = 'x';
        DWORD written = 0;
        ASSERT_FN(CHK_BOOL(WriteFile(wr, &byte, 1, &written, NULL) && written == 1));
        pool->sched(COLIB_REGNAME(test83_read_completes(rd)));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    ASSERT_FN(test83_handle_count(&ctrl_after));

    /* kill: create_timeo kills the read while it is suspended; the wrapper frame is destroyed
     * without resuming, so only whoever frees the io request can still close its event */
    test83_wd_state = "kill loop";
    DWORD kill_before = 0, kill_after = 0;
    ASSERT_FN(test83_handle_count(&kill_before));
    for (int i = 0; i < test83_iters; i++) {
        pool->sched(COLIB_REGNAME(test83_read_killed(rd)));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    ASSERT_FN(test83_handle_count(&kill_after));

    long ctrl_growth = long(ctrl_after) - long(ctrl_before);
    long kill_growth = long(kill_after) - long(kill_before);

    DBG("control: handles %lu -> %lu (growth %ld), completed reads %d/%d",
            ctrl_before, ctrl_after, ctrl_growth, test83_ctrl_done, test83_iters);
    DBG("kill   : handles %lu -> %lu (growth %ld), timed-out reads %d/%d",
            kill_before, kill_after, kill_growth, test83_kill_timeo, test83_iters);

    /* every read must have gone the intended way, else the growth numbers say nothing */
    ASSERT_FN(CHK_BOOL(test83_ctrl_done == test83_iters));
    ASSERT_FN(CHK_BOOL(test83_kill_done == test83_iters));
    ASSERT_FN(CHK_BOOL(test83_kill_timeo == test83_iters));

    /* both loops stay flat: the event closes with the io request on every exit now */
    ASSERT_FN(CHK_BOOL(ctrl_growth <= test83_iters / 10));
    ASSERT_FN(CHK_BOOL(kill_growth <= test83_iters / 10));

    return 0;
}

#else /* COLIB_OS_WINDOWS */

static int test83_winevent_handle_leak() {
    /* the bug is Windows/IOCP-only (the event handle lives in the request's OVERLAPPED) */
    DBG("test skipped on this platform");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    std::thread(test83_watchdog_main).detach();

    int ret = test83_winevent_handle_leak();
    print_test_result("018-029-reproduced_winevent_handle_leak.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

Workspace `repro/fix/06/`: `colib-base.h` (copy of the real colib.h), `colib.h`/`colib-patched.h`
(the patched copy), the repro and test sources with their include pointed at the workspace header,
all logs and build outputs. Build line per the fix spec: `cl -nologo -EHsc -await:strict -std:c++20
-Zi -FS -I. <src>.cpp -Fe:<src>.exe -Fo:<src>.obj -link`. The real colib.h was never written;
`git status` state unchanged by this work.

1. Repro FAILS on the unpatched copy — fresh run, not just the archived verdict:

   ```
   $ ./06-base.exe                       (repro/fix/06/06-run-base.log)
   kill loop:  100 iters, handles: 153
   kill loop:  200 iters, handles: 253
   kill loop:  300 iters, handles: 353
   control: handles 53 -> 53 (growth 0), completed reads 300/300
   kill   : handles 53 -> 353 (growth 300), timed-out reads 300/300
   VERDICT: REPRODUCED
   ```

   The regression test itself, pre-fix, fails on the leak it exists to catch
   (`018-029-run-base.log`):

   ```
   control: handles 53 -> 53 (growth 0), completed reads 300/300
   kill   : handles 53 -> 353 (growth 300), timed-out reads 300/300
   Failed CHK_BOOL(kill_growth <= test83_iters / 10) ...
   [FAILED]: 018-029-reproduced_winevent_handle_leak.cpp
   ```

2. Patched copy: the regression test PASSES, run twice — `kill: handles 53 -> 53 (growth 0)` and
   `47 -> 47 (growth 0)`, control flat both times, `[PASSED]`, exit 0
   (`018-029-run-fixed.log`, `018-029-run-fixed-run2.log`). The original bug-asserting repro now
   prints `VERDICT: REFUTED — no handle growth after 300 kills`, with the per-100 checkpoints
   flat at 53 throughout, proving the close is per-iteration, not batched (`06-run-fixed.log`).

3. No-regression, three closest tests against the patched copy (sources copied with only the
   include line changed; ws2_32 comes from tests_common.h's pragma):

   ```
   005-001-io.cpp                -> [PASSED], exit 0   (005-001-io-run-fixed.log)
   003-001-sleep.cpp             -> [PASSED], exit 0   (003-001-sleep-run-fixed.log)
   003-003-sleep_create_timeo.cpp-> [PASSED], exit 0   (003-003-sleep_create_timeo-run-fixed.log)
   ```

   `005-001-io.cpp` is the load-bearing one here: it drives `ConnectEx`/`AcceptEx` (WSA events),
   `connect`/`read`/`write` (CreateEvent), `DeviceIoControl`, lock file and directory changes
   through `handle_done_req` on the resumed path — the wrapper contract is unchanged. Its one
   `CHK_HANDLE(dev)` line about `\\.\PhysicalDrive0` (code 0x2) is environmental: it prints
   identically against the base copy, which I also built and ran for comparison
   (`005-001-io-run-base.log`, `[PASSED]`), and the scheduled-coroutine assert only logs by the
   tests/ caveat. `003-003` is the create_timeo kill test (the timeout family the briefing
   pointed a 002-* test at; 003-003 is the timeout test the suite actually has).

4. NOT-CHECKED (compile): the epoll/kqueue/unknown backends cannot compile on this box; both hunks
   sit inside `#if COLIB_OS_WINDOWS` blocks, so those backends see no change at all — validated by
   reading. The regression test's non-Windows skip branch (two lines, mirroring `005-001-io.cpp`'s
   shipped fallback) was not compiled either, no Linux toolchain here.

## Interactions

Same-region row from the briefing: 03, 06 (this one), 25 — the io_data/io_pool teardown region.

- #03 (killer_stop_io_fail): its `awake_data` error path returns `ERROR_GENERIC` without
  dequeuing, so the io_data stays registered in `handles` while `kill()` destroys the frame anyway
  — the UAF is on `data->state`, not on the event, and this fix does not touch it (the map still
  holds a reference there, so the destructor does not even run at frame destruction). The two
  fixes compose: whenever #03's fix makes those error paths dequeue, the last reference drops where
  it always should have, and this destructor closes the event there — the close-then-kill scenario
  currently leaks its event too, and stops leaking under the pair. Neither subsumes the other.
- #25 (clear iterator invalid): `clear()` runs user callbacks under iteration over `handles`; this
  fix adds no callbacks and no code to `clear()`. The destructor fires from the shared_ptrs inside
  `handles.clear()`, after the loop, when nothing iterates. Whatever restructuring #25's fix does
  to `clear()`, the refs drop somewhere and the destructor closes there. Note also that this fix
  covers the pool-teardown face of the leak (#25's scenario force-stops and drops the pool with
  waiters parked): pre-fix, `clear()`'s `destroy_state` without resume leaked those events; the
  code-reading for that face is in The bug above, the runs above validate the killer face.
- #02 (killer kill()/end_chain region, same map row): this fix relies on `end_chain`'s frame
  destruction dropping the last reference but changes none of it; #02's timing-window work is
  unaffected by a destructor that only calls `CloseHandle` on a request already cancelled and
  drained.
- #11/#12 (wrappers / io_request pipe branch, adjacent rows): #11 changes where `win_err` is
  captured in `add_waiter`; the `hEvent = NULL` lines sit after the existing closes in
  `handle_done_req` and reorder nothing, so the two diffs do not overlap textually or semantically.

## Open questions for the user

1. Ownership of `overlapped.hEvent` in a hand-built `io_desc_t` (the `wait_event()` escape hatch).
   Before this fix, nothing in the library closed a hand-built request's event — a `wait_event()`
   caller had to close it themselves (or leak it; the docs only say "on Windows this must have a
   valid event state"). After this fix, the `io_data_t` owns the event and closes it when it dies,
   so such a caller's own close would become a double close of an already-dead handle value
   (harmless to the OS, but wrong to write). I took the uniform-ownership reading — one owner for
   every exit — because it is what makes the leak impossible; if you prefer `wait_event()` events
   to stay the caller's, the destructor needs a flag marking library-minted events instead, and
   the wrappers' contract keeps its current split. If the uniform reading stands, one line in
   `docs/02_api.md`'s `wait_event()` entry should say the descriptor's io state owns and closes
   the event.
