# Proposal: run() returns ret_val at its no-task exit (fix #09)

Status: [APPLIED 2026-10-14 on the user's word]

A consistency fix more than a bug fix: documented usage doesn't reach it (see "What reaches it"
under "The bug"). The library already stored `RUN_ERRORED` for a failed io poll, and its docs
promise that value for io errors; `run()` threw it away and answered `RUN_OK`. Applied as proposed,
the regression test landed as `tests/018-040-reproduced_run_swallows_errored.cpp` (Test95).
Verified at apply time: the landed test fails on the header before the fix
(`bug run() = 0 (RUN_OK)`) and passes after it, 3 runs of 3; the whole suite passes on
Windows (95/95), on Linux (94/94, `018-029` being Windows-only) and on Linux under ASan and
UBSan (94/94, no report); every one of the 95 tests passes under ASan on Windows.

## The bug

`pool_t::run()` (implemented by `pool_internal_t::run()`) drives its loop through
`pool_internal_t::next_task_state()`, and that helper has two nullptr returns with opposite
meanings. Each one pre-stores its truth into the pool's `ret_val` member before returning: the
out-of-work exit (`io_pool.handle_ready()` succeeded, `ready_tasks` empty) stores `ret_val =
RUN_OK`, and the io-backend-failure exit (`io_pool.handle_ready() != ERROR_OK`, after logging
"Failed io pool") stores `ret_val = RUN_ERRORED`. `run()` then did:

```c
state = next_task_state();
if (state == nullptr)
    return RUN_OK;
```

a literal `RUN_OK` on both. So whenever the io backend failed — the `GetQueuedCompletionStatusEx` /
`epoll_wait` / `kevent` call inside `handle_ready()` returning an error — `run()` reported a clean
run, the `ret_val = RUN_ERRORED` store was dead code at that exit, and every coroutine left
suspended on io was abandoned unreported. This violates the documented contract on `pool_t::run()`:
"Returns RUN_OK if no error occurred during the run, else the return code", and it also contradicts
the `run_e` docs, where `RUN_OK` means "the pool stopped because it ran out of things to do" — the
one thing that had not happened.

The trigger, forced through the public API (repro/09-run_swallows_errored.cpp): a loopback socket
pair whose peer never sends, one reader coroutine suspended on `co::read()` of the silent end, so
the pool must poll io; then `CloseHandle()` on the handle handed out by the documented accessor
`pool_t::get_internal_handle()` (the pool's iocp) before `run()`. The blocking
`GetQueuedCompletionStatusEx` inside the Windows `io_pool_t::handle_ready()` then fails for real
(ERROR_INVALID_HANDLE) and returns `ERROR_GENERIC` through its genuine error branch — the same
branch any real IOCP failure takes. Confirmed REPRODUCED, deterministic across runs: `run()`
returned `0 (RUN_OK)` while the library's own log printed "Failed GetQueuedCompletionStatus" and
"Failed io pool" twice, and the reader stayed suspended (started 1, resumed 0). A control phase in
the same binary (healthy pool, peer sends) completed the reader and legitimately returned RUN_OK,
so the bug phase's `0` cannot be excused as "there was nothing to do". Full log:
`repro/09-run_swallows_errored.md`.

The same swallow sits on every backend: the fix lands in backend-independent pool code, and repro
#17's kqueue analysis independently hit it (an interrupted `kevent` stores `RUN_ERRORED` that
`run()` discards the same way — see Interactions).

What reaches it, checked 2026-10-14: documented usage doesn't. `handle_ready()` fails only when the
pool's own iocp or epoll fd is no longer valid (user code closed what `get_internal_handle()` handed
out, the misuse the trigger above uses), after another bug (on epoll, an event for an fd colib no
longer tracks, which a handle closed before its `stop_io` can cause), or when the OS itself fails.
Signals don't: epoll retries `EINTR`, and the Windows wait handles the APC wake-ups the timers use;
only the work-in-progress kqueue backend lets `EINTR` through (#17). What the fix changes is the
report, not the tolerance: whatever made the backend fail, `run()` stops either way, and today it
calls that a clean drain.

## Where the fix lands

| colib.h region                    | what changes                                     | adds/dels |
|-----------------------------------|--------------------------------------------------|-----------|
| `pool_internal_t::run()`, its     | returns `ret_val`, what `next_task_state()`      | +1 -1     |
| `state == nullptr` exit           | stored, instead of the literal `RUN_OK`          |           |

One word. Nothing else in the file changes; the public doc comment on `pool_t::run()` and the
`run_e` enumerator docs already describe the fixed behavior, so they stay as they are.

## The change

```diff
--- colib.h (current)
+++ colib.h (patched)
@@ -4212,7 +4212,7 @@
         while (true) {
             state = next_task_state();
             if (state == nullptr)
-                return RUN_OK;
+                return ret_val;
 
             /* whenever we resume, we set the ret_val to aborted for any error that may happen, as
             such we set the ret_val value to aborted. This value will be replaced if needed before
```

`next_task_state()` stores the correct `run_e` at each of its two nullptr returns, inside the very
call whose result `run()` checks, with no statement between the store and the check. Returning
`ret_val` there hands the caller exactly what the helper decided: RUN_OK on a drain, RUN_ERRORED on
an io-backend failure. The empty-pool case (`run()` on a pool with nothing scheduled) is unchanged:
`handle_ready()` takes its "nothing registered" fast path, `next_task_state()` stores RUN_OK, and
that is what comes back.

Liveness of every exit and every store, checked per the briefing:

- `run()` has four exits. The entry guard (`!io_pool.is_ok()`) returns a literal RUN_ERRORED before
  any `ret_val` semantics exist — correct, untouched. The `posted_exception` exit rethrows and
  produces no return value. The `posted_stop` break already returns `ret_val`. The nullptr exit is
  the fourth, the only one discarding it, and this diff is the whole fix.
- The `ret_val = RUN_ABORTED` initialization cannot leak out of either `ret_val`-returning exit.
  At the nullptr exit it is impossible by construction: both of `next_task_state()`'s nullptr paths
  write `ret_val` first (RUN_ERRORED in the failure branch, RUN_OK in the drain branch), in the
  same call. At the `posted_stop` exit it is impossible because `post_stop()` has exactly one
  caller — `force_stop()`'s `stop_awaiter_t::await_suspend` — which stores `ret_val = RUN_STOPPED`
  immediately before posting, and `run()` consumes `posted_stop` right after the resume, before the
  loop top can call `next_task_state()` again. RUN_ABORTED therefore stays what its comment says
  (a default for errors that may happen during a resume) and remains as unreachable as a return
  value as it was before this fix.
- `run()` is `next_task_state()`'s only caller: since FIX_43 the awaiters return
  `std::noop_coroutine()` and only `run()` runs the scheduler, between two resumes.
  `has_next_task_state()`, the external awaitables' check, neither polls the io nor reads
  `ret_val`. (Checked 2026-10-14; the first version of this bullet described the `next_task()`
  that FIX_43 removed.)

## The regression test

Lands as `tests/018-040-reproduced_run_swallows_errored.cpp` (Test95), the next free numbers
today; it was first written as 018-030 / Test84, the name the logs below carry. Its watchdog is
defined above the Windows block since 2026-10-14: `main()` starts it on every platform, and while
it sat inside the block the file didn't compile on Linux (found by the Linux suite run).

Pre-fix it FAILS at `CHK_BOOL(r == co::RUN_ERRORED)` — `run()` returned `0 (RUN_OK)` while its own
log showed the io backend failing. Post-fix it passes unchanged (run twice). I ran it against both
the unpatched and the patched copy — logs below. The workspace copy in `repro/fix/09/` is this
file verbatim except the include is `"colib.h"` instead of `"../colib.h"`, so it builds against
the workspace header.

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test95 - Reproduced Bugs: pool_t::run() returned RUN_OK after the io backend failed
================================================================================================= */

/* pool_internal_t::run() checked its helper's nullptr return with a literal 'return RUN_OK', but
 * next_task_state() has two nullptr returns with opposite meanings and pre-stores the truth of
 * each into the pool's ret_val: the "nothing left to do" exit stores RUN_OK, the
 * "io_pool.handle_ready() failed" exit stores RUN_ERRORED. run() discarded ret_val at that exit,
 * so a failed io backend was reported as a clean run - violating pool_t::run()'s documented
 * contract ("Returns RUN_OK if no error occurred during the run, else the return code") - and every
 * coroutine left suspended on io was abandoned unreported (repro/09-run_swallows_errored.md).
 *
 * The fix returns ret_val at that exit, so the value next_task_state() stored is what run()
 * reports. The trigger below forces a real io-backend failure through the public API:
 * pool_t::get_internal_handle() hands out the pool's iocp handle, and closing it makes the
 * blocking GetQueuedCompletionStatusEx inside handle_ready() fail for real (ERROR_INVALID_HANDLE),
 * through the same branch any genuine IOCP failure takes. The control phase proves in the same
 * binary that a healthy pool still completes its reader and returns RUN_OK. */

static std::atomic<const char *> test95_wd_state{"startup"};

static void test95_watchdog_main() {
    for (int i = 0; i < 150; i++)   /* 15 s in 100 ms slices */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("018-040-reproduced_run_swallows_errored.cpp: TIMED OUT after 15s while '%s' - "
           "run() did not come back\n", test95_wd_state.load());
    std::_Exit(1);
}

#if COLIB_OS_WINDOWS

static int test95_ctrl_started = 0;   /* control reader began its body */
static int test95_ctrl_resumed = 0;   /* control reader ran past its read */
static int test95_bug_started = 0;    /* bug reader began its body */
static int test95_bug_resumed = 0;    /* bug reader ran past its read (never, by design) */

static co::task_t test95_reader(int *started, int *resumed, HANDLE sock) {
    (*started)++;
    char buff[64];
    long long n = (long long)co_await co::read(sock, buff, sizeof(buff));
    (*resumed)++;
    DBG("reader resumed with n=%lld", n);
    co_return 0;
}

struct test95_sock_pair_t {
    SOCKET lst;
    SOCKET cli;   /* main() holds this end */
    SOCKET srv;   /* handed to the reader coroutine */
};

/* blocking loopback pair setup: bind 127.0.0.1:0, listen, connect, accept */
static int test95_make_pair(test95_sock_pair_t *out) {
    out->lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    out->cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out->lst == INVALID_SOCKET || out->cli == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    if (bind(out->lst, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    if (listen(out->lst, 1) != 0)
        return -1;

    int len = sizeof(addr);
    if (getsockname(out->lst, (struct sockaddr *)&addr, &len) != 0)
        return -1;
    if (connect(out->cli, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;

    out->srv = accept(out->lst, NULL, NULL);
    if (out->srv == INVALID_SOCKET)
        return -1;
    return 0;
}

static void test95_close_pair(test95_sock_pair_t *p) {
    closesocket(p->cli);
    closesocket(p->srv);
    closesocket(p->lst);
}

/* a healthy pool: the peer sends, the reader completes, run() drains to RUN_OK */
static int test95_control() {
    test95_sock_pair_t p;
    ASSERT_FN(CHK_BOOL(test95_make_pair(&p) == 0));
    FnScope close_pair([&p] { test95_close_pair(&p); });

    char byte = 'x';
    ASSERT_FN(CHK_BOOL(send(p.cli, &byte, 1, 0) == 1));

    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test95_reader(&test95_ctrl_started, &test95_ctrl_resumed,
            (HANDLE)p.srv)));

    test95_wd_state = "control: pool->run()";
    co::run_e r = pool->run();
    DBG("control run() = %d (%s)", (int)r, co::dbg_enum(r).c_str());

    ASSERT_FN(CHK_BOOL(r == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test95_ctrl_started == 1));
    ASSERT_FN(CHK_BOOL(test95_ctrl_resumed == 1));
    return 0;
}

/* the iocp closed through the documented accessor while a reader is suspended on a read that can
 * never complete: handle_ready() fails for real, so run() must report RUN_ERRORED, not RUN_OK */
static int test95_closed_iocp() {
    test95_sock_pair_t p;
    ASSERT_FN(CHK_BOOL(test95_make_pair(&p) == 0));
    FnScope close_pair([&p] { test95_close_pair(&p); });
    /* the peer (p.cli) never sends: the reader's read can never complete */

    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test95_reader(&test95_bug_started, &test95_bug_resumed,
            (HANDLE)p.srv)));

    intptr_t iocp_h = pool->get_internal_handle();
    ASSERT_FN(CHK_BOOL(iocp_h != 0));
    ASSERT_FN(CHK_BOOL(CloseHandle((HANDLE)iocp_h) != 0));
    DBG("closed the pool iocp %p via get_internal_handle(); the pool's io backend is now dead",
            (void *)iocp_h);

    test95_wd_state = "bug: pool->run() (GQCS on the dead port must fail fast, not hang)";
    co::run_e r = pool->run();
    DBG("bug run() = %d (%s), reader started=%d resumed=%d", (int)r, co::dbg_enum(r).c_str(),
            test95_bug_started, test95_bug_resumed);

    /* the contract promised an error return for a failed run; the reader staying suspended is the
     * error being reported, so it must not count as a clean drain */
    ASSERT_FN(CHK_BOOL(r == co::RUN_ERRORED));
    ASSERT_FN(CHK_BOOL(test95_bug_started == 1));
    ASSERT_FN(CHK_BOOL(test95_bug_resumed == 0));
    return 0;
}

static int test95_run_swallows_errored() {
    WSADATA wsa_data;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0));
    FnScope wsa_cleanup([] { WSACleanup(); });

    ASSERT_FN(test95_control());
    ASSERT_FN(test95_closed_iocp());
    return 0;
}

#else /* COLIB_OS_WINDOWS */

static int test95_run_swallows_errored() {
    /* the swallow is platform-independent (it lives in pool_internal_t::run()), but this trigger
     * closes the pool's iocp, which only exists on Windows; the unix twin would close the epoll
     * or kqueue fd handed out by the same get_internal_handle() */
    DBG("test skipped on this platform");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    std::thread(test95_watchdog_main).detach();

    int ret = test95_run_swallows_errored();
    print_test_result("018-040-reproduced_run_swallows_errored.cpp", ret >= 0);
    return ret;
}
```

The bug phase deliberately leaves the reader suspended: the io backend's death is not rescuable,
and the contract being tested is the *reporting* of it, not the rescue. The suspended reader is
destroyed by the pool's own teardown (its "Failed to cancel io" fallout lines are the pool dtor
hitting the already-dead port; harmless, and present in the saved logs).

## How it was checked

Workspace `repro/fix/09/`: `colib-base.h` (copy of the real colib.h), `colib.h`/`colib-patched.h`
(the patched copy; the edit went into `colib.h`, `colib-patched.h` is the diff-side rename), the
repro, regression-test and no-regression-test sources with their include pointed at the workspace
headers, all logs and build outputs. Build line per the fix spec (`cl -nologo -EHsc -await:strict
-std:c++20 -Zi -FS -I. ... -link`, ws2_32 where sockets are used). The real colib.h was never
written and git state was not touched.

1. Repro FAILS on the unpatched copy — fresh run, matching the archived verdict
   (`repro/fix/09/09-run-base.log`, exit 1):

   ```
   [control] run() = 0 (RUN_OK), reader_started=1 reader_resumed=1
   [control] ok
   ...
   [bug] CloseHandle(iocp) -> 1
       [bug] reader: started, about to suspend on co::read()
   colib-base.h:3595 handle_ready() :> Failed GetQueuedCompletionStatus: The handle is invalid. [0x6]
   colib-base.h:4196 next_task_state() :> Failed io pool
   colib-base.h:3595 handle_ready() :> Failed GetQueuedCompletionStatus: The handle is invalid. [0x6]
   colib-base.h:4196 next_task_state() :> Failed io pool
   [bug] run() = 0 (RUN_OK), reader_started=1 reader_resumed=0
   FAIL-REPRO (public-api): run() returned literal RUN_OK although io_pool_t::handle_ready() failed;
        next_task_state()'s ret_val = RUN_ERRORED was stored and never returned, and the suspended reader
        (started=1, resumed=0) is left unreported
   exit code: 1
   ```

2. Patched copy: the repro PASSES — `run() = -1 (RUN_ERRORED)` with the reader still suspended
   (started=1, resumed=0), control phase still `0 (RUN_OK)`; identical on a second run
   (`09-run-fixed.log`, `09-run-fixed-run2.log`, both exit 0):

   ```
   [control] run() = 0 (RUN_OK), reader_started=1 reader_resumed=1
   [control] ok
   ...
   [bug] run() = -1 (RUN_ERRORED), reader_started=1 reader_resumed=0
   PASS: run() surfaced the io error as -1 (RUN_ERRORED) - REFUTED
   ```

3. No-regression, three run()-return-asserting tests against the patched copy (sources copied with
   only the include line changed):

   ```
   001-001-semaphore_ping_pong.cpp            -> [PASSED], exit 0   (001-001-run-fixed.log)
   012-001-introspection_get_pool_get_state   -> [PASSED], exit 0   (012-001-run-fixed.log)
   002-001-flowctrl_force_stop.cpp            -> [PASSED], exit 0   (002-001-run-fixed.log)
   ```

   `001-001` (the briefing's candidate) drives a million-iteration semaphore ping-pong and asserts
   `pool->run()` — a heavy run draining through the changed exit, which must still yield RUN_OK.
   `012-001` (the briefing's candidate) asserts `ret == co::RUN_OK` explicitly; it was also run
   against the base copy for comparison (identical pass). `002-001` is the load-bearing one for the
   exits this fix does NOT touch: it calls `run()` six times in a loop and asserts each force_stop
   iteration returns exactly RUN_STOPPED (the `posted_stop` break, the other `ret_val` consumer)
   and the final iteration RUN_OK (drain); it passes against both the base and the patched copy,
   so RUN_STOPPED does not leak anywhere and the drain exit keeps its value.

4. The regression test itself, pre-fix, fails on exactly the swallow it exists to catch
   (`018-030-run-base.log`, `[FAILED]`, `bug run() = 0 (RUN_OK)` then
   `Failed CHK_BOOL(r == co::RUN_ERRORED)`); post-fix it passes twice
   (`018-030-run-fixed.log`, `018-030-run-fixed-run2.log`, `[PASSED]`, exit 0, with
   `bug run() = -1 (RUN_ERRORED), reader started=1 resumed=0`).

5. Linux was NOT-CHECKED at first (no Linux toolchain then); it is checked since, see 6. kqueue
   stays NOT-CHECKED: its backend is work in progress.

6. Re-checked 2026-10-14 on the header of that day (FIX_02, 06, 08, 43, 44, 45, 47 applied), with
   the rebased diff above:
   - the regression test (renumbered, its watchdog moved): before the fix FAILED (`bug run() = 0
     (RUN_OK)` right after the library logged "Failed io pool"), after it PASSED;
   - an epoll twin, a scratch probe (a reader parked on a silent pipe, then a second coroutine
     closes the epoll fd `get_internal_handle()` gave): before the fix `run=0` with the reader
     abandoned, after it `run=-1`; ASan and UBSan clean both times;
   - the whole suite on Windows: `[PASSED: 95]/[FAILED: 0]` with the fix (the 94 tests plus this
     one);
   - the whole suite on Linux (WSL Ubuntu, g++ 15.2, epoll; `018-029` left out, its watchdog is
     Windows-only): `[PASSED: 94]/[FAILED: 0]`; the first Linux run stopped on this test's build,
     which is how its watchdog defect was found;
   - ASan (MSVC): `001-001`, `002-001`, `012-001`, every `005-` io test and this test (3 runs)
     exit 0 with no ASan report.

## Interactions

Same-region row from the briefing: run()/error plumbing — #09 is independent, but it sits on the
consumers of `next_task_state()`.

- #17 (kqueue EINTR): an interrupted `kevent` makes the kqueue `handle_ready()` return
  `ERROR_GENERIC`, `next_task_state()` stores RUN_ERRORED — and that store currently dies in this
  same swallow. #17's repro write-up keyed its test on the early return plus abandoned coroutines,
  explicitly "under both the current code and any future `return ret_val` cleanup", so the two
  compose with no test churn: once #17 lands its EINTR retry, a *genuine* kqueue error that remains
  is now reported as RUN_ERRORED instead of RUN_OK. Neither fix subsumes the other (#17 stops the
  false failure; this fix stops the false success).
- #17's second finding (the unix-standby one the briefing flags): the kqueue `handle_ready()` has
  no "nothing registered -> return" shortcut (its epoll twin does), so a kqueue pool with nothing
  registered blocks in `kevent()` forever and can never *reach* the drain exit — kqueue pools can
  never drain to RUN_OK today. This fix is exit-agnostic: it returns whatever `next_task_state()`
  stored, so when that backend gains the epoll-style shortcut, the drain exit will return RUN_OK
  through this same line with no further change here. Acknowledged as an interaction with the drain
  exit, not a conflict.
- The batch-1 memory-safety fixes (02, 06 and 08 applied, 03 removed, 07 declined, 25 waiting)
  and #12/#16 live in the killer, io-pool teardown and modif-engine regions; none touches `run()`'s
  exits or reads `ret_val`. Their
  scenarios end through normal drains or kills, and 001-001/002-001 passing against the patched
  copy covers both of those exit shapes.

## Open questions for the user

1. The TODO on the `ret_val = RUN_ABORTED` line ("use this to somehow collect epoll and internal
   errors") — after this fix its epoll/iocp half is done: the stored error is returned. What
   remains open is the internal-errors-during-resume half, which nothing collects yet. I left the
   comment untouched for the minimal diff; say the word if you want its wording narrowed to the
   half that is still future work.
2. The regression test's trigger closes the iocp, so the test is Windows-only. The unix twin would
   `close()` the epoll fd (or kqueue fd) handed out by the same `get_internal_handle()` and assert
   the identical RUN_ERRORED. It cannot be compiled on this box, so I did not write it into the
   test; want one added when a unix target is available, or is #17's kqueue test (which exercises
   this exit through a real kevent failure once it can run) enough there?
