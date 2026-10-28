# Proposal: ConnectNamedPipe completes an already connected pipe instead of hanging (fix #12)

Status: [APPLIED 2026-10-28 on the user's word]

Apply verification, 2026-10-28: the test hangs on the header before the fix until its 5 s
watchdog ends it, and passes on the header as applied, two runs of two; the whole suite on
Windows `[PASSED: 101]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll)
`[PASSED: 100]/[FAILED: 0]`, every file but the Windows-only `018-029`; MSVC ASan builds of
`018-042` to `018-045`, `005-001`, `018-024` and `018-029` exit 0 with no report. The check
table is the last item of "How it was checked".

## The bug

`co::ConnectNamedPipe` wraps `::ConnectNamedPipe` into the pool's io machinery: the wrapper builds
an `io_data_t` whose `io_request` lambda makes the OS call, and `io_pool_t::add_waiter` runs that
lambda, enqueues the waiter and suspends the awaiting coroutine when the lambda reports
`ERROR_OK` — "the request pends, a completion packet will arrive for its OVERLAPPED".

The lambda classified every outcome into exactly those two values, and the condition that
decided was:

```cpp
if (!ret && (GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_PIPE_CONNECTED))
    return ERROR_GENERIC;
return ERROR_OK;
```

A `FALSE` return with `GetLastError() == ERROR_PIPE_CONNECTED` fails the error check and falls
onto `return ERROR_OK` — the branch that means "pended". But `ERROR_PIPE_CONNECTED` means the
client had **already connected** when the call was made: the kernel completes the connect inline
and queues no IRP, so **no IOCP completion packet can ever be delivered for that OVERLAPPED**.
That kernel contract is not an inference — repro phase A pinned it with plain Win32 (no colib):
`::ConnectNamedPipe` returns FALSE with error 535, the overlapped event never signals within
500 ms, and `GetOverlappedResult(FALSE)` fails with `ERROR_IO_INCOMPLETE`.

The consequence follows directly: `add_waiter` believes the request pends, marks the awaiter
added, suspends the coroutine, and the io pool's `handle_ready()` blocks forever inside
`GetQueuedCompletionStatusEx(iocp, ..., INFINITE, ...)` — nothing in the process can wake it.
The trigger is trivial to hit and fully deterministic: create an overlapped pipe instance, let a
client `CreateFileA` onto it before any coroutine awaits, then `co_await co::ConnectNamedPipe(h)`
on a fresh pool. The trace build confirmed both halves: `add_waiter` is followed directly by
`enqueue_data` (so the wrapper really reported OK, not an error), and "Waiting for events..." is
the last library line ever printed. The swallowed branch also had no `COLIB_DEBUG` line of its
own, so in a real program a server would simply stop accepting, silently.

Verdict: **REPRODUCED**, deterministic one-process hang, watchdog exit 42. Full log and the raw
Win32 evidence: `repro/12-connectnamedpipe_hang.md` (and `.cpp`).

## Where the fix lands

As applied on 2026-10-28; the shape is the proposal's, with `ERROR_FINISHED` reused as the third
answer (open question 1, decided by the user) and the branch in `add_waiter()` tracing instead of
logging: a client that connected first is a normal event, not a failure.

| colib.h region                          | what changes                                        | adds/dels    |
|-----------------------------------------|-----------------------------------------------------|--------------|
| `io_data_t::io_request` field doc (the  | the field's contract names the third return value:  | in the       |
| Windows io request descriptor)          | `ERROR_FINISHED` means the request completed by     | `io_data_t`  |
|                                         | itself and no completion will ever be queued for it | hunk, shared |
|                                         |                                                     | with fix #40 |
| `io_pool_t::add_waiter()` (the IOCP io  | an `ERROR_FINISHED` return from `io_request` makes  | +12/-0       |
| pool)                                   | the waiter ready on the spot, as a success with no  |              |
|                                         | bytes, flagged `IO_FLAG_ADDED` like a request that  |              |
|                                         | pended; the pended and the failure paths are        |              |
|                                         | untouched; the branch traces                        |              |
| `co::ConnectNamedPipe()`'s `io_request` | `ERROR_PIPE_CONNECTED` returns `ERROR_FINISHED`     | +9/-1        |
| lambda                                  | (completed now) instead of `ERROR_OK` (pended); the |              |
|                                         | last error is read once into a local                |              |

All three hunks sit inside `#if COLIB_OS_WINDOWS`.

## The change

`diff -u` of the header as it stood on 2026-10-28 right before this batch of fixes (#12, #38, #39,
#40 and #48, applied the same day on the user's word; #11 and `wait_msg()` earlier that day)
against the header as applied. The first hunk is shared with fix #40: this fix's lines in it are
the four of the `io_request` doc.

Reasoning per hunk:

- `io_request`'s doc: the third answer is written where the two others are, so a wrapper's lambda
  and `add_waiter()` read one contract.
- `add_waiter()`: the request did its work in the call itself, so the waiter is made ready as a
  delivered completion would make it: `win_err` clear, no bytes, `ERROR_OK` in its state, queued
  with `awake_io()`. To the rest of the engine it is a completion that arrived at once: a kill
  that reaches the coroutine before it resumes sees a wait that did its work (`completed()`
  answers so) and runs it on, as DESIGN.md rule 3 asks. The branch traces rather than logs.
- `ConnectNamedPipe`'s lambda: the last error is read once; `ERROR_PIPE_CONNECTED` answers
  `ERROR_FINISHED`, `ERROR_IO_PENDING` keeps answering `ERROR_OK`, anything else fails as before.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -1115,19 +1120,29 @@
                                                          failed */
 
     std::function<error_e(void *)> io_request;      /*!< function to be called inside add_waiter,
-                                                         for example: the ReadFile request */
+                                                         for example: the ReadFile request; returns
+                                                         ERROR_OK if it pends, ERROR_FINISHED if it
+                                                         completed by itself (no completion will
+                                                         ever be queued for it), an error if it
+                                                         failed */
     void *ptr = nullptr;                            /*!< can be context for io_request or timer
                                                          info */
     HANDLE h = NULL;                                /*!< same as in `io_desc_t` */
 
+    /*! Built in place, as alloc<io_data_t> does, and filled field by field: a copy would make two
+     * owners of the event, which would close it twice. */
+    io_data_t() = default;
+    io_data_t(const io_data_t&) = delete;
+    io_data_t& operator=(const io_data_t&) = delete;
+
     /*! Releases the event of the request, if `overlapped.hEvent` was set: the event is owned by
      * this structure and is closed when it dies, on every exit - also the one where a waiter is
      * destroyed suspended (a killed waiter), whose own code after the wait never runs. A waiter
      * that resumed released the event earlier, in handle_done_req, which clears the field for
-     * exactly this check. */
+     * exactly this check. A failed close is logged with its Windows error code. */
     ~io_data_t() {
-        if (overlapped.hEvent)
-            CloseHandle(overlapped.hEvent);
+        if (overlapped.hEvent && !CloseHandle(overlapped.hEvent))
+            COLIB_DEBUG("Failed to close event: [0x%lx]", GetLastError());
     }
 };
 
@@ -3801,6 +3816,18 @@
         }
 
         error_e err = data->io_request(data->ptr);
+        if (err == ERROR_FINISHED) {
+            /* the request completed by itself (ConnectNamedPipe on an already connected pipe):
+            nothing was queued for it, so no completion can arrive, and its waiter is made ready
+            here, as a success with no bytes (its overlapped was never written) */
+            COLIB_DEBUG_TRACE("the io_request finished by itself, its waiter is ready");
+            data->flags = io_data_t::io_flag_e(data->flags | io_data_t::IO_FLAG_ADDED);
+            data->win_err = ERROR_SUCCESS;
+            data->recvlen = 0;
+            data->state->err = ERROR_OK;
+            awake_io(data.get());
+            return ERROR_OK;
+        }
         if (err != ERROR_OK) {
             data->win_err = GetLastError();
             COLIB_DEBUG("Failed the io_request: %s", get_last_error().c_str());
@@ -5663,7 +5684,15 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
         bool ret = std::apply(::ConnectNamedPipe, *params);
-        if (!ret && (GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_PIPE_CONNECTED))
+        if (ret)
+            return ERROR_OK;
+        DWORD win_err = GetLastError();
+        if (win_err == ERROR_PIPE_CONNECTED) {
+            /* the client had connected before the call: the request is complete now, nothing
+            was queued, so no completion will ever arrive for this overlapped */
+            return ERROR_FINISHED;
+        }
+        if (win_err != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
     };
```

## The regression test

Landed as `tests/018-045-reproduced_connectnamedpipe_already_connected.cpp` (Test101); it was
first written as 018-030 / Test84, the name the older logs below carry.

A kernel probe first pins the fact with plain Win32: `::ConnectNamedPipe` on an instance whose
client already connected returns FALSE with `ERROR_PIPE_CONNECTED`, the overlapped event never
signals and `GetOverlappedResult(FALSE)` reports `ERROR_IO_INCOMPLETE`. Then a client connects to
a fresh instance before the server coroutine is scheduled, the coroutine awaits
`co::ConnectNamedPipe` and writes on the pipe, and the test reads that write from the client: the
success the await answered with is a connection that carries data. A watchdog thread ends the
process after 5 s, since the bug's failure mode is a hang in `pool->run()`.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
018-045-reproduced_connectnamedpipe_already_connected.cpp: TIMED OUT after 5s - co::ConnectNamedPipe never returned (the exact regression this test exists to catch)
exit 1
```

On the header as applied, two runs of two:

```
[PASSED]: 018-045-reproduced_connectnamedpipe_already_connected.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdio>
#include <atomic>
#include <chrono>

/* Test101 - Reproduced Bugs: co::ConnectNamedPipe on an already connected pipe hung forever
================================================================================================= */

/* co::ConnectNamedPipe's io_request treated a FALSE return with GetLastError() ==
ERROR_PIPE_CONNECTED exactly like ERROR_IO_PENDING: it reported "the request pends" and the
awaiting coroutine suspended for an IOCP completion. ERROR_PIPE_CONNECTED means the client had
connected before the call, so the kernel completed the connect inline and queued no IRP - no
completion packet can ever arrive for that OVERLAPPED (the kernel probe below asserts exactly
that, with plain Win32), and pool->run() blocked forever in its wait. Reproduced 2026-09-28
(repro/12-connectnamedpipe_hang.md): a deterministic one-process hang, silent - the swallowed
branch had no log line of its own.

Fixed by letting io_request report "completed by itself" (ERROR_FINISHED, consumed only inside
add_waiter) for that branch; add_waiter then makes the waiter ready on the spot - a success with
no bytes transferred, since nothing was ever written to the overlapped - instead of enqueueing a
wait that cannot be satisfied. Before the fix this file hangs for 5 s and is killed by its own
watchdog; after it the coroutine completes on the first pass of run(). Fix #12
(tests/fixes/FIX_12-connectnamedpipe_connected.md). */

static BOOL test101_conn_ok = FALSE;
static int  test101_conn_err = 12345;
static BOOL test101_write_ok = FALSE;
static int  test101_written = 0;
static std::atomic<bool> test101_done{false};

#if COLIB_OS_WINDOWS

static co::task_t test101_server(HANDLE pipe) {
    co::error_e err = co::ERROR_GENERIC;
    test101_conn_ok = co_await co::ConnectNamedPipe(pipe, &err);
    test101_conn_err = (int)err;

    /* the success must be a real one: the server can write on the pipe right away, the
    connection is there */
    const char msg[] = "already connected";
    DWORD written = 0;
    test101_write_ok = co_await co::WriteFile(pipe, msg, sizeof(msg), &written, NULL);
    test101_written = (int)written;
    co_return 0;
}

static int test101_kernel_probe(void) {
    char name[128];
    std::snprintf(name, sizeof(name), "\\\\.\\pipe\\colib-test101-%lu-probe",
            (unsigned long)GetCurrentProcessId());

    HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE, 1, 1024, 1024, 0, NULL);
    ASSERT_FN(CHK_HANDLE(server));
    FnScope close_server([server]{ CloseHandle(server); });

    HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
    ASSERT_FN(CHK_HANDLE(client));
    FnScope close_client([client]{ CloseHandle(client); });

    /* the client sits on the pipe instance before the server ever asks for a connection */
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_FN(CHK_PTR(ov.hEvent));
    FnScope close_event([&ov]{ CloseHandle(ov.hEvent); });

    SetLastError(0);
    BOOL ret = ConnectNamedPipe(server, &ov);
    DWORD win_err = GetLastError();
    ASSERT_FN(CHK_BOOL(ret == FALSE));
    ASSERT_FN(CHK_BOOL(win_err == ERROR_PIPE_CONNECTED));

    /* no IRP was queued for this overlapped, so nothing will ever signal its event: this is
    the fact that makes "wait for the completion" a forever wait */
    DWORD waited = WaitForSingleObject(ov.hEvent, 100);
    ASSERT_FN(CHK_BOOL(waited == WAIT_TIMEOUT));

    DWORD transferred = 0;
    DBG("probe: ret=%d err=%lu event_wait=%lu GetOverlappedResult=%d", (int)ret,
            (unsigned long)win_err, (unsigned long)waited,
            (int)GetOverlappedResult(server, &ov, &transferred, FALSE));

    return 0;
}

static int test101_already_connected(void) {
    ASSERT_FN(test101_kernel_probe());

    char name[128];
    std::snprintf(name, sizeof(name), "\\\\.\\pipe\\colib-test101-%lu",
            (unsigned long)GetCurrentProcessId());
    HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE, 1, 1024, 1024, 0, NULL);
    ASSERT_FN(CHK_HANDLE(server));

    /* the client connects before the coroutine that awaits is even scheduled */
    HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
    ASSERT_FN(CHK_HANDLE(client));
    FnScope close_all([server, client]{ CloseHandle(client); CloseHandle(server); });

    auto pool = co::create_pool();
    pool->sched(test101_server(server));
    ASSERT_FN(pool->run());     /* before the fix this never returned */

    ASSERT_FN(CHK_BOOL(test101_conn_ok == TRUE));
    ASSERT_FN(CHK_BOOL(test101_conn_err == (int)co::ERROR_OK));

    /* the write inside the coroutine reached the client: the connection the await answered
    with is one that carries data */
    char buff[64] = {0};
    DWORD read_len = 0;
    ASSERT_FN(CHK_BOOL(ReadFile(client, buff, sizeof(buff) - 1, &read_len, NULL)));
    ASSERT_FN(CHK_BOOL(test101_write_ok == TRUE));
    ASSERT_FN(CHK_BOOL(test101_written == (int)read_len));

    return 0;
}

#else /* COLIB_OS_WINDOWS */

static int test101_already_connected(void) {
    DBG("named pipes are Windows-only: no ConnectNamedPipe to await elsewhere");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    /* This bug's failure mode is a hang inside pool->run(), so a plain ASSERT_FN can never
    fire on it and run_tests.py has no per-test timeout of its own: without a watchdog a
    regression would hang the whole suite instead of failing this one file. 018-012 guards its
    Linux-only hang with alarm()/SIGALRM; Windows has no alarm(), so a detached thread plays
    that role here. */
    std::thread watchdog([]{
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (test101_done)
            return;
        std::fputs("018-045-reproduced_connectnamedpipe_already_connected.cpp: TIMED OUT "
                "after 5s - co::ConnectNamedPipe never returned (the exact regression this "
                "test exists to catch)\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    });
    watchdog.detach();

    int ret = test101_already_connected();
    test101_done = true;

    print_test_result("018-045-reproduced_connectnamedpipe_already_connected.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

Builds (from `repro/fix/12/`, per the fix-spec line):
`MSYS_NO_PATHCONV=1 cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I. <file>.cpp -Fe:<file>.exe -link`

Every run below was performed fresh on 2026-09-29 against the workspace copies; the original
2026-09-28 runs agree. (`[..]` trims a timestamp and path prefix inside pasted colib debug
lines.)

1. **Repro FAILS on the unpatched copy** (fresh build against `colib.base.h`, from
   `repro/fix/12/base-run/`):

   ```
   A: ::ConnectNamedPipe returned 0, GetLastError=535 (ERROR_PIPE_CONNECTED=535, ERROR_IO_PENDING=997)
   A: WaitForSingleObject(overlapped event, 500ms)=258 (WAIT_TIMEOUT=258 => no IRP was ever queued, nothing completed)
   A: GetOverlappedResult(wait=FALSE)=0 bytes=0 GetLastError=996
   --
   B: server coroutine: co_await co::ConnectNamedPipe(h)
   WATCHDOG: still blocked in GetQueuedCompletionStatusEx waiting for a completion that never comes - repro confirmed by hang
   BASE_REPRO_EXIT=42
   ```

2. **Repro PASSES on the patched copy** — the coroutine completes promptly, phase A's kernel
   facts are unchanged, and both new log lines fire:

   ```
   A: ::ConnectNamedPipe returned 0, GetLastError=535 ...
   A: WaitForSingleObject(overlapped event, 500ms)=258 ...
   A: GetOverlappedResult(wait=FALSE)=0 bytes=0 GetLastError=996
   --
   B: server coroutine: co_await co::ConnectNamedPipe(h)
   [..] colib.h:5539 operator ()() :> ConnectNamedPipe: the client was already connected: There is a process on other end of the pipe. [0x217]
   [..] colib.h:3715 add_waiter() :> io_request finished without pending, completing the waiter now
   B: RESUMED: co::ConnectNamedPipe returned ok=1 err=0 GetLastError=535
   B: pool->run() returned 0, b_resumed=1
   PASS: the coroutine completed - REFUTED (see its reported values above)
   REPRO_EXIT=0
   ```

3. **No-regression** — `005-001-io.cpp` (the Windows io family; its pipe subtest drives
   `co::ConnectNamedPipe`'s normal pended path plus `TransactNamedPipe`/`ReadFile`/`WriteFile`,
   and its counters are asserted): **[PASSED]**, exit 0. One environmental note, reported for
   honesty: its `\\.\PhysicalDrive0` sub-probe failed `CHK_HANDLE` in this shell ("file not
   found"); the test does not gate on it and passed regardless — same with or without this fix.
   `005-004-io_stop_handle.cpp`: **[PASSED]**, exit 0, but vacuously on this box — its `main()`
   never calls `WSAStartup` (so its `socket()` fails) and its fixed pipe name was busy; both
   pre-existing conditions, identical with and without the patch. `005-002`/`005-003` are
   Linux-only paths. The load-bearing no-regression evidence is therefore `005-001` plus the
   addendum below.

4. **Beyond the ladder — modifs and pended-path checks** (`12b-connectnamedpipe_extra.cpp`, in
   the workspace): an inline-completed `ConnectNamedPipe` under a 5 s `create_timeo` resolves in
   milliseconds with `ok=1 err=0 pair_err=0` (the WAIT_IO/UNWAIT_IO modif stack stays symmetric
   and the completion beats the timer), and the normal pended path (client connects 100 ms after
   the server awaits) still completes with `ok=1 err=0` through the untouched enqueue/wait
   machinery. Exit 0.

   ```
   [..] colib.h:5539 operator ()() :> ConnectNamedPipe: the client was already connected: There is a process on other end of the pipe. [0x217]
   [..] colib.h:3715 add_waiter() :> io_request finished without pending, completing the waiter now
   case1: run=0 ok=1 err=0 pair_err=0 elapsed=7ms
   case2: run=0 ok=1 err=0
   PASS: both addendum cases
   EXTRA_EXIT=0
   ```

5. **The regression test on both copies** — unpatched: hangs, watchdog fires, exit 1; patched:
   **[PASSED]**, exit 0.

   ```
   018-029-reproduced_connectnamedpipe_already_connected.cpp: TIMED OUT after 5s - co::ConnectNamedPipe never returned (the exact regression this test exists to catch)
   BASE_EXIT=1
   ---
   probe: ret=0 err=535 event_wait=258 GetOverlappedResult=0
   [PASSED]: 018-029-reproduced_connectnamedpipe_already_connected.cpp
   PATCHED_EXIT=0
   ```

6. **The current tree — drift and applied fix #06** (from `repro/fix/12/merged-run/`: a copy of
   today's real `colib.h` — accept rework committed, fix #06 applied — plus exactly this fix's
   three hunks). `patch` applied the diff with hunk 1 exact and hunks 2/3 at offsets +10/+12
   (#06's inserted lines); the merged copy differs from the real `colib.h` by precisely the
   three hunks of "The change". Against it:

   ```
   [..] merged-run\colib.h:5551 operator ()() :> ConnectNamedPipe: the client was already connected: There is a process on other end of the pipe. [0x217]
   [..] merged-run\colib.h:3725 add_waiter() :> io_request finished without pending, completing the waiter now
   [PASSED]: 018-029-reproduced_connectnamedpipe_already_connected.cpp
   MERGED_REG_EXIT=0        (this fix's regression test)

   case1: run=0 ok=1 err=0 pair_err=0 elapsed=2ms
   case2: run=0 ok=1 err=0
   PASS: both addendum cases
   MERGED_EXTRA_EXIT=0      (the 12b addendum)

   [PASSED]: 005-001-io.cpp
   EXIT=0                   (the no-regression io family)

   control: handles 53 -> 53 (growth 0), completed reads 300/300
   kill   : handles 53 -> 53 (growth 0), timed-out reads 300/300
   [PASSED]: 018-029-reproduced_winevent_handle_leak.cpp
   EXIT=0                   (fix #06's own regression test, unmodified)
   ```

   So the fix composes with the tree as it stands today, and with applied #06 in both
   directions: #06's guarantee survives this fix, and this fix's test passes with #06 in.

7. **NOT-CHECKED:** nothing, for kqueue specifically — all three hunks live inside
   `#if COLIB_OS_WINDOWS` regions (`io_data_t`, the IOCP `io_pool_t`, the Windows wrappers), so
   no non-Windows build even sees changed text. Linux/Unix builds were not compiled here (this
   is a Windows box), as usual for this workspace.

The real `colib.h` was never written. Its diff against the workspace base copy is exactly
applied fix #06's +12 lines (verified with `diff` while building step 6); every build and run
above happened against copies inside `repro/fix/12/`.

At apply time, 2026-10-28, on the header of that day, in the applied shape:

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: a client that connects to an overlapped pipe instance   |
|                             | before the server awaits `co::ConnectNamedPipe`; a           |
|                             | deterministic hang                                           |
| DESIGN.md                   | rule 3 holds: the inline completion is a wait that did its   |
|                             | work (`completed()` says so), so a kill that reaches the     |
|                             | coroutine before it resumes runs it on to its next real wait |
|                             | as it does for a delivered completion; the killer untouched  |
| epoll / kqueue              | every hunk is in the Windows block; the Linux suite builds   |
|                             | and passes on the applied header; kqueue NOT-CHECKED (WIP)   |
| contract / docs             | `io_request`'s field doc gained its third answer, mirrored   |
|                             | in `02_api.md`'s `io_data_t` block; `ERROR_FINISHED`'s enum  |
|                             | doc untouched, the reuse is internal to `add_waiter()`       |
| callers                     | `add_waiter()`'s caller (`wait_io`, from the awaiter) reads  |
|                             | `ERROR_OK` as before; no other wrapper's lambda answers      |
|                             | `ERROR_FINISHED`; `wait_msg()`'s answers `ERROR_OK`          |
| pending fixes               | none touches `add_waiter()` or `ConnectNamedPipe`'s lambda;  |
|                             | #11 (applied) sits before the lambda, #23 in `ConnectEx`     |
| runs                        | the test hangs before (watchdog, 5 s) and passes after (2 of |
|                             | 2); Windows 101/101; Linux 100/100; MSVC ASan `018-042` to   |
|                             | `018-045`, `005-001`, `018-024`, `018-029` clean             |

## Interactions

The tracker rows that contain this fix: the Windows wrapper family `11 12 22 23`, and the
io_pool_t Windows row `03 06 25 12 11`.

- **#11** (early-failure `*err` writes in the wrappers): `ConnectNamedPipe`'s `CreateEvent`-
  failure early return sits directly above the lambda this fix changes, and #11 also touches
  where `win_err` is captured in `add_waiter` — the lines just below this fix's new branch. No
  shared lines, no semantic overlap: #11 is the failure contract, this fix is the completed-now
  outcome. The two compose; whichever lands second re-diffs with a context shift only.
- **#22** (the `AF_INET` hardcodes in `co::connect`/`co::accept`; confirmed repro, no proposal
  written yet): this fix touches neither function — `ConnectNamedPipe` is the named-pipe
  wrapper, not a socket one, and the other two hunks sit in the io pool and the descriptor.
  Shared row only because both cluster on the Windows wrapper surface; fully orthogonal and
  order-free.
- **#23** (`SO_UPDATE_ACCEPT_CONTEXT` in `co::accept`'s tail): edits a different wrapper's tail;
  this fix's wrapper hunk is inside `ConnectNamedPipe`'s lambda and the others sit in
  `add_waiter`/`io_data_t`. No overlap; compose in any order.
- **#06** (io_data_t closes its event) — **applied 2026-09-29**, after this fix's validation
  was done; the validation predates it. The two compose. By reading: #06's destructor fires only
  on the last `io_data_t` reference drop with the event still set — a waiter that resumed
  released the event earlier in `handle_done_req`, which now clears the field — and this fix's
  synchronous completion parks the request nowhere (the state is queued ready at once, the
  coroutine resumes through the ordinary path and closes the event in `handle_done_req`); a
  waiter killed before that resume is exactly the case #06's destructor was built for. By
  running: on the merged copy of step 6, #06's own regression test passes unchanged and so does
  this fix's.
- **#03** (`force_awake`/`awake_data`) and **#25** (`clear()`): same io_pool_t row, regions this
  fix does not edit. The completed-inline waiter is never entered into `handles` — the same
  shape a delivered waiter has — so `force_awake`'s `_is_registered` reports "already finished"
  and `clear()` skips it; nothing for either diff to collide with.

Sibling scan (from the original repro effort): sixteen Windows wrappers carry the same-shaped
`io_request` lambda — `ConnectEx`, `AcceptEx`, `ConnectNamedPipe`, `DeviceIoControl`,
`LockFileEx`, `ReadDirectoryChangesW`, `ReadFile`, `TransactNamedPipe`, `WaitCommEvent`,
`WriteFile`, `WSASendMsg`, `WSASendTo`, `WSASend`, `WSARecvFrom`, `WSARecvMsg`, `WSARecv`. Only
`ConnectNamedPipe` accepts a second error code as success-shaped; `ERROR_PIPE_CONNECTED` is the
only FALSE-with-success-meaning error among these APIs, so no sibling needs the third outcome
today. The siblings also map a TRUE synchronous success to "pended" — that keeps working because
an IOCP-associated handle still gets a completion packet queued on synchronous completion for
these families (`005-001`'s pipe writes exercise exactly that and pass, on the patched copy and
on the merged one). Should a handle type ever complete inline without queueing, the
`ERROR_FINISHED` mechanism is now there for it: one line in that wrapper's lambda and nothing
else.

## Open questions for the user

1. **Reusing `ERROR_FINISHED` as io_request's third return value.** `io_request` now returns
   `ERROR_FINISHED` to mean "completed by itself, nothing queued". I reused the existing
   constant because its plain reading ("not an error, the target had already finished") covers
   the new meaning, because `io_request` is internal machinery consumed by exactly one function
   (`add_waiter`), and because it adds no public value to `error_e` — the enum's own doc line is
   untouched; the contract lives on the `io_request` field. If you prefer the kill/stop meaning
   to stay exclusive to those paths, the alternative is a dedicated positive constant (an
   `ERROR_COMPLETED` alongside it) — one enum line plus the same two code sites. My pick is the
   reuse; your word decides.

2. **The inline completion is fixed to "success, no bytes".** `add_waiter` hardcodes
   `win_err = ERROR_SUCCESS` and `recvlen = 0` on this branch. For `ConnectNamedPipe` that is
   the whole truth — nothing was ever written to the overlapped, and a connect transfers no
   bytes. If a future wrapper ever completes inline *with* a byte count, the lambda would need a
   way to stash `recvlen`/`win_err` itself; I left that out until such a case exists.

Decided at apply time, 2026-10-28, on the user's word: (1) `ERROR_FINISHED` is reused; (2) the
inline completion stays fixed at a success with no bytes.
