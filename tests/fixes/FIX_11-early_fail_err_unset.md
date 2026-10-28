# Proposal: the Windows I/O wrappers' early failures write *err before returning false (fix #11)

Status: [APPLIED 2026-10-28 on the user's word]

Apply verification, 2026-10-28: the test fails on the header before the fix (`A1 ConnectEx: ok=0
err=1234`) and passes on the header as applied, two runs of two; the whole suite on Windows
`[PASSED: 97]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 96]/[FAILED: 0]`,
every file but the Windows-only `018-029`; MSVC ASan builds of `005-001`, `018-024` and `018-041`
exit 0 with no report. The check table is item 6 of "How it was checked".

A consistency fix more than a bug fix: the wrappers' docs already promise `ERROR_GENERIC` in
`*err` for a failure, and twenty early exits return FALSE without writing it. Documented usage
reaches those exits only when the OS fails under a correct call (see "What reaches it" under
"The bug"). Rebased and re-checked on 2026-10-14. Applied 2026-10-28 in a shape decided then: one
helper for the twenty exits, and the byte count zeroed too (see "Where the fix lands"); its test
is 018-041 (Test97).

## The bug

The Windows I/O wrappers (`co::ConnectEx`, `co::AcceptEx`, `co::ReadFile`, `co::WriteFile`,
`co::ConnectNamedPipe`, `co::DeviceIoControl`, `co::LockFileEx`, `co::ReadDirectoryChangesW`,
`co::TransactNamedPipe`, `co::WaitCommEvent`, `co::WSASendMsg`, `co::WSASendTo`, `co::WSASend`,
`co::WSARecvFrom`, `co::WSARecvMsg`, `co::WSARecv`) each take an optional out parameter
`error_e *err` whose documented contract, in every wrapper's own doc comment, reads:

> @param err Optional: the wait's own result, the stopper's error when the request was
> stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
> is GetLastError()), ERROR_OK when it succeeded

The wrong step is exact: each wrapper has exits that fire before the request is ever built, and
those exits `co_return false` without writing `*err` at all. There are twenty of them.

- The Winsock extension not loading: `load_win_fn()` fails (its internal dummy `socket()` fails,
  or `WSAIioctl` fails) and the wrapper returns false immediately. Four sites: `ConnectEx`,
  `AcceptEx`, `WSASendMsg`, `WSARecvMsg`.
- The event not being created: `CreateEvent()`/`WSACreateEvent()` returns NULL and the wrapper
  returns false immediately. Sixteen sites, one per wrapper (`ReadFile`'s sits between its offset
  fill and its handle assignment; the others sit right after the event creation call). The null
  check is live on both families: `WSA_INVALID_EVENT`, the failure return of `WSACreateEvent`, is
  `((WSAEVENT)NULL)` in the SDK headers, so the `if (!hEvent)` test genuinely catches it.

The failure plumbing everywhere else in these wrappers is correct: after the `co_await`, the
wrapper writes `*err = ret` and only then decides its final bool. The early exits simply predate
that write.

The consequence lands one layer up, because the higher-level translations initialize `err` to
`ERROR_OK` and fold the wrapper's bool into it — `co::connect` is literally
`error_e err = ERROR_OK; BOOL ok = co_await ConnectEx(...); co_return ok ? ERROR_OK : err;`:

- `co::connect` returns `ERROR_OK` — success — from a request where `_connect_ex` was never even
  loaded and nothing connected.
- `co::read` on a HANDLE returns `0` — indistinguishable from end of file — when `ReadFile`'s
  `CreateEvent` failed before any read was attempted (found by inspection first, since a real
  failure needs handle-table exhaustion; the probe under "What reaches it" forced it with a
  stand-in for `CreateEvent` on 2026-10-14).

The test forces the `load_win_fn` face by never starting Winsock, which makes the dummy
`socket()` fail. That is a misuse, and `co::connect` can't take that path at all, since its
`bind` fails first; "What reaches it" below covers the documented calls.

Confirmed consequence (repro/11-win_err_unset.md, VERDICT: REPRODUCED for the forceable path): in
a fresh process with winsock never started and `*err` pre-loaded with the sentinel 1234,
`co::ConnectEx` and `co::AcceptEx` returned FALSE with `err` still 1234 — the sentinel survived
verbatim, proving no write happened — and the exact one-line translation `co::connect` performs
evaluated that failure to `ERROR_OK`. The isolation controls in the same run prove the defect is
specific to the early exits: the same wrappers driven through the request path (winsock up,
garbage socket/handle) set `*err = ERROR_GENERIC` properly, and `co::read` returned a real error,
not the EOF-shaped 0.

What reaches it, checked 2026-10-14: documented usage reaches these exits only when the OS fails
under a correct call. The extension load fails when the Winsock provider lacks the extension, or
when no socket can be made for `load_win_fn`'s dummy (only until a load succeeds: the pointer stays
cached). The event creation fails when the process has no handles or memory left. No misuse is
needed, and a normal run never takes these exits. A probe with stand-ins for `WSAIoctl`,
`CreateEvent` and `WSACreateEvent`, on a started Winsock with a valid socket and valid pipe
handles, showed what the callers get:

| documented call, failing step    | before the fix                     | after the fix          |
|----------------------------------|------------------------------------|------------------------|
| `co::connect`, the `ConnectEx`   | `ERROR_OK`, while `getpeername`    | `ERROR_GENERIC`        |
| load                             | says the socket is not connected   |                        |
| `co::read`, `CreateEvent`        | 0, the answer for the end of the   | `ERROR_GENERIC`        |
|                                  | stream                             |                        |
| `co::write`, `CreateEvent`       | 0 bytes written                    | `ERROR_GENERIC`        |
| `co::ReadFile`, `CreateEvent`    | FALSE, `*err` as the caller left   | FALSE, `ERROR_GENERIC` |
|                                  | it                                 |                        |
| `co::accept`, `WSACreateEvent`   | INVALID_SOCKET, `*err` as the      | INVALID_SOCKET,        |
|                                  | caller left it                     | `ERROR_GENERIC`        |

`GetLastError()` held the stand-in's code in every case, before the fix and after it, so the
"(the reason is GetLastError())" half of the contract already holds. The fix changes nothing but
the `*err` these exits write: it makes the code do what the wrappers' docs say.

## Where the fix lands

As applied on 2026-10-28. The proposal first carried twenty inline two-line writes, one per exit;
the shape below is the one in the header.

| colib.h function (Windows block)   | what changes                                 | adds/dels  |
|------------------------------------|----------------------------------------------|------------|
| new: `fail_unstarted_req()`, right | the answer of a wrapper whose request could  | +12/-0     |
| before `handle_done_req()`         | not be started: zeroes the count, writes     |            |
|                                    | `ERROR_GENERIC` to `*err`, returns FALSE     |            |
| `ConnectEx`, `AcceptEx`,           | both early exits (the extension load, the    | +2/-2 each |
| `WSASendMsg`, `WSARecvMsg`         | event creation) answer                       |            |
|                                    | `co_return fail_unstarted_req(err, <count>)` |            |
| `ConnectNamedPipe`,                | the event-creation exit answers              | +1/-1 each |
| `DeviceIoControl`, `LockFileEx`,   | `co_return fail_unstarted_req(err, <count>)`,|            |
| `ReadDirectoryChangesW`,           | NULL as the count for the three wrappers     |            |
| `ReadFile`, `TransactNamedPipe`,   | without one (`ConnectNamedPipe`,             |            |
| `WaitCommEvent`, `WriteFile`,      | `LockFileEx`, `WaitCommEvent`)               |            |
| `WSASendTo`, `WSASend`,            |                                              |            |
| `WSARecvFrom`, `WSARecv`           |                                              |            |

Twenty sites, each one line in place of its `co_return false`, and one helper: +31/-20. Two
decisions taken at apply time, where the applied shape differs from the proposal:

- The byte count is zeroed too (open question 4). `handle_done_req()` zeroes it on a failure
  after the wait, "as the sync Win32 calls leave it on a failure", and Win32's own `ReadFile`
  zeroes it before doing any work or error checking, so a failure before the request starts is
  the plainest case of that rule. A failed wrapper now looks the same to its caller whichever
  step failed.
- One helper instead of twenty copies. With the count, the inline form is four lines at each
  exit; `fail_unstarted_req()` makes each exit one line and leaves one place to verify.

The higher-level translations (`co::connect`, `co::read`, `co::write`, `co::accept`) are
untouched; they already fold the wrapper's answer correctly and become right once the wrappers
keep the contract they document. No signature or doc comment changes; `err` keeps its optional
status (the helper guards it, as `accept()`'s own early returns do).

## The change

`diff -u` of the header as it stood on 2026-10-28 right before the apply (fixes #02, 06, 08, 09,
43, 44, 45 and 47 applied, `wait_msg()` added the same day) against the header as applied.

Reasoning per hunk:

- The helper hunk: the failure shape `handle_done_req()` gives a request that failed after its
  wait, for a request that never started. It has no event to close and no `win_err` to restore:
  the call that failed (`load_win_fn`'s dummy `socket()` or `WSAIoctl`, `CreateEvent`,
  `WSACreateEvent`) left the reason in the thread's last error itself, so the helper leaves
  `GetLastError()` alone.
- The four `load_win_fn` hunks (`ConnectEx`, `AcceptEx`, `WSASendMsg`, `WSARecvMsg`): the
  extension pointer being NULL after a failed load means the request cannot be built; the wrapper
  answers through the helper with its own count parameter.
- The sixteen event-creation hunks: the same, with each wrapper's count parameter, NULL for the
  three that take none.
- Placement: the `COLIB_DEBUG` line before each exit stays, and the helper writes nothing to the
  log; the sites already say what failed.
- Considered and not taken: one write at the top of each wrapper, overwritten after the wait. It
  would also change `*err` when the wrapper is killed in its wait, which no exit writes today.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -5513,6 +5513,18 @@
     return ERROR_OK;
 }
 
+/* The answer of a wrapper whose request could not be started (its extension not loaded, its
+event not created): the failure the wrappers promise, shaped as handle_done_req leaves one that
+failed, with the count zeroed and ERROR_GENERIC in *err; the reason is what the failed call left
+in GetLastError(). 2026-10-28 21:40 */
+inline BOOL fail_unstarted_req(error_e *err, DWORD *len) {
+    if (len)
+        *len = 0;   /* as the sync Win32 calls leave it on a failure */
+    if (err)
+        *err = ERROR_GENERIC;
+    return FALSE;
+}
+
 inline error_e handle_done_req(io_data_t *data, error_e err, DWORD *len, uint64_t *offset) {
     if (err != ERROR_OK) {
         if (len)
@@ -5564,7 +5576,7 @@
 {
     if (!_connect_ex && load_win_fn(WSAID_CONNECTEX, _connect_ex) != ERROR_OK) {
         COLIB_DEBUG("Can't load extension");
-        co_return false;
+        co_return fail_unstarted_req(err, lpdwBytesSent);
     }
 
     auto desc = create_io_desc(co_await get_pool());
@@ -5577,7 +5589,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpdwBytesSent);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5603,7 +5615,7 @@
 {
     if (!_accept_ex && load_win_fn(WSAID_ACCEPTEX, _accept_ex) != ERROR_OK) {
         COLIB_DEBUG("Can't load extension");
-        co_return false;
+        co_return fail_unstarted_req(err, lpdwBytesReceived);
     }
 
     auto desc = create_io_desc(co_await get_pool());
@@ -5615,7 +5627,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpdwBytesReceived);
     }
     desc.data->h = desc.h = (HANDLE)sListenSocket;
     desc.data->io_request = +[](void *ptr) -> error_e {
@@ -5646,7 +5658,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, NULL);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5680,7 +5692,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpBytesReturned);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5713,7 +5725,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, NULL);
     }
     desc.data->h = desc.h = hFile;
     if (offset) {
@@ -5751,7 +5763,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpBytesReturned);
     }
     desc.data->h = desc.h = hDirectory;
     desc.data->io_request = +[](void *ptr) -> error_e {
@@ -5788,7 +5800,7 @@
     }
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesRead);
     }
     desc.data->h = desc.h = hFile;
     desc.data->io_request = +[](void *ptr) -> error_e {
@@ -5827,7 +5839,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpBytesRead);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5858,7 +5870,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, NULL);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5895,7 +5907,7 @@
     desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesWritten);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5921,7 +5933,7 @@
 {
     if (!_wsa_send_msg && load_win_fn(WSAID_WSASENDMSG, _wsa_send_msg) != ERROR_OK) {
         COLIB_DEBUG("Can't load extension");
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesSent);
     }
     auto desc = create_io_desc(co_await get_pool());
 
@@ -5933,7 +5945,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesSent);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -5967,7 +5979,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesSent);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -6001,7 +6013,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesSent);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -6035,7 +6047,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesRecvd);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -6060,7 +6072,7 @@
 {
     if (!_wsa_recv_msg && load_win_fn(WSAID_WSARECVMSG, _wsa_recv_msg) != ERROR_OK) {
         COLIB_DEBUG("Can't load extension");
-        co_return false;
+        co_return fail_unstarted_req(err, lpdwNumberOfBytesRecvd);
     }
 
     auto desc = create_io_desc(co_await get_pool());
@@ -6073,7 +6085,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpdwNumberOfBytesRecvd);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
@@ -6107,7 +6119,7 @@
     desc.data->overlapped.hEvent = WSACreateEvent();
     if (!desc.data->overlapped.hEvent) {
         COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
-        co_return false;
+        co_return fail_unstarted_req(err, lpNumberOfBytesRecvd);
     }
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
```

## The regression test

Landed as `tests/018-041-reproduced_win_err_unset.cpp` (Test97; Test96 went to
`005-006-io_wait_msg.cpp` the same day), category 18 (Reproduced Bugs). It was first written as
018-030 / Test83, the name the older logs below carry.

Phase A runs with winsock deliberately never started and probes every wrapper that loads an
extension: `ConnectEx` (twice: once with a sentinel `*err`, once through the exact one-line
translation `co::connect` performs), `AcceptEx`, `WSASendMsg`, `WSARecvMsg`, so all four
`load_win_fn` sites are pinned; where the wrapper takes a count it is pre-loaded with 777 and must
come back 0. Phase B starts winsock and checks that the request-level failure paths still write
`*err` (garbage-socket `ConnectEx`, garbage-handle `DeviceIoControl`, `co::read` on the same
handle), so a "fix" that writes `ERROR_GENERIC` unconditionally, or that breaks the normal
plumbing, cannot pass. The sixteen event-creation sites cannot be forced without handle-table
exhaustion; they share the helper and are stated as such in the header comment. Results haul out
of the coroutines into globals because a failed `ASSERT_COFN` only logs (tests/CLAUDE.md).

On the header before the fix (a copy kept aside at apply time), built through the tests makefile,
it fails at its first check:

```
A1 ConnectEx: ok=0 err=1234
Failed CHK_BOOL(test97_a1_err == (int)co::ERROR_GENERIC) err_str: Either the application has not called WSAStartup, or WSAStartup failed.
[FAILED]: 018-041-reproduced_win_err_unset.cpp
```

On the header as applied, two runs of two:

```
A1 ConnectEx: ok=0 err=-1
A2 co::connect's translation: -1
A3 AcceptEx: ok=0 err=-1 count=0
A4 WSASendMsg: ok=0 err=-1 count=0
A5 WSARecvMsg: ok=0 err=-1 count=0
B1 ConnectEx(garbage socket): ok=0 err=-1
B2 DeviceIoControl(garbage handle): ok=0 err=-1
B3 co::read(garbage handle): -1
[PASSED]: 018-041-reproduced_win_err_unset.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test97 - Reproduced Bugs: the Windows I/O wrappers' early failures left *err untouched
================================================================================================= */

/* The Windows I/O wrappers document their optional out err as "ERROR_GENERIC when it failed (the
reason is GetLastError())", but the failures that strike before the request is ever built - the
Winsock extension not loading (load_win_fn) or the event not being created (CreateEvent /
WSACreateEvent) - co_returned FALSE without writing *err at all, and left the byte count as the
caller had it, where a request that failed after its wait zeroes it. A caller that initializes err
to ERROR_OK and computes `ok ? ERROR_OK : err` on return, which is exactly what co::connect does,
then read ERROR_OK back: a success answer for a request that never ran. The load_win_fn paths are
forceable from a real process by never calling WSAStartup, which makes the dummy socket inside
load_win_fn fail; the event-creation paths would need handle-table exhaustion and share the same
fixed exit (fail_unstarted_req). Phase A runs with winsock deliberately down and probes every
wrapper that loads an extension, the count pre-loaded where the wrapper takes one; phase B starts
winsock and checks that the request-level failure paths still write *err, so the fix cannot pass
by breaking the normal error plumbing. Found by static analysis, reproduced 2026-09-28
(repro/11-win_err_unset.md); fix #11 (tests/fixes/FIX_11-early_fail_err_unset.md). */

#if COLIB_OS_WINDOWS

/* results hauled out of the coroutines: a failed ASSERT_COFN only logs (tests/CLAUDE.md) */
static int test97_a1_ok = -1, test97_a1_err = -1;
static int test97_a2_translated = -12345;
static int test97_a3_ok = -1, test97_a3_err = -1, test97_a3_cnt = -1;
static int test97_a4_ok = -1, test97_a4_err = -1, test97_a4_cnt = -1;
static int test97_a5_ok = -1, test97_a5_err = -1, test97_a5_cnt = -1;
static int test97_b1_ok = -1, test97_b1_err = -1;
static int test97_b2_ok = -1, test97_b2_err = -1;
static int test97_b3_read = 0;

static sockaddr_in test97_loopback(unsigned short port) {
    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    return addr;
}

static co::task_t test97_early_fails() {
    sockaddr_in addr = test97_loopback(28011);

    /* A1: direct wrapper call, *err pre-loaded with a sentinel no real write could leave */
    co::error_e err = (co::error_e)1234;
    BOOL ok = co_await co::ConnectEx(INVALID_SOCKET, (const sockaddr *)&addr,
            (int)sizeof(addr), NULL, 0, NULL, &err);
    test97_a1_ok = ok ? 1 : 0;
    test97_a1_err = (int)err;

    /* A2: the exact one-line translation co::connect performs on that result */
    co::error_e err2 = co::ERROR_OK;
    BOOL ok2 = co_await co::ConnectEx(INVALID_SOCKET, (const sockaddr *)&addr,
            (int)sizeof(addr), NULL, 0, NULL, &err2);
    test97_a2_translated = ok2 ? (int)co::ERROR_OK : (int)err2;

    /* A3: AcceptEx, same load_win_fn early exit; the count pre-loaded, to be zeroed */
    char buff[64];
    DWORD recved = 777;
    co::error_e err3 = (co::error_e)1234;
    BOOL ok3 = co_await co::AcceptEx(INVALID_SOCKET, INVALID_SOCKET, buff, 0,
            sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &recved, &err3);
    test97_a3_ok = ok3 ? 1 : 0;
    test97_a3_err = (int)err3;
    test97_a3_cnt = (int)recved;

    /* A4, A5: the other two wrappers that load an extension, same shape */
    WSABUF wbuf = { 0, NULL };
    WSAMSG msg;
    ZeroMemory(&msg, sizeof(msg));
    msg.name = (sockaddr *)&addr;
    msg.namelen = (int)sizeof(addr);
    msg.lpBuffers = &wbuf;
    msg.dwBufferCount = 1;
    DWORD sent = 777;
    co::error_e err4 = (co::error_e)1234;
    BOOL ok4 = co_await co::WSASendMsg(INVALID_SOCKET, &msg, 0, &sent, NULL, &err4);
    test97_a4_ok = ok4 ? 1 : 0;
    test97_a4_err = (int)err4;
    test97_a4_cnt = (int)sent;

    recved = 777;
    co::error_e err5 = (co::error_e)1234;
    BOOL ok5 = co_await co::WSARecvMsg(INVALID_SOCKET, &msg, &recved, NULL, &err5);
    test97_a5_ok = ok5 ? 1 : 0;
    test97_a5_err = (int)err5;
    test97_a5_cnt = (int)recved;

    co_return 0;
}

static co::task_t test97_request_fails() {
    sockaddr_in addr = test97_loopback(28011);

    /* B1: the same wrapper as A1, but winsock is up, so load_win_fn succeeds and the failure
     * comes from the request itself. 0x1234, not INVALID_SOCKET, so the iocp associate step
     * never sees INVALID_HANDLE_VALUE. */
    co::error_e err = (co::error_e)1234;
    BOOL ok = co_await co::ConnectEx((SOCKET)0x1234, (const sockaddr *)&addr,
            (int)sizeof(addr), NULL, 0, NULL, &err);
    test97_b1_ok = ok ? 1 : 0;
    test97_b1_err = (int)err;

    /* B2: handle-based wrapper, invalid-but-nonnull handle */
    DWORD junk = 0;
    co::error_e err2 = (co::error_e)1234;
    BOOL ok2 = co_await co::DeviceIoControl((HANDLE)0x1234, 0, NULL, 0, NULL, 0, &junk, &err2);
    test97_b2_ok = ok2 ? 1 : 0;
    test97_b2_err = (int)err2;

    /* B3: co::read on the same invalid handle must surface a real error, never the EOF-shaped 0 */
    char buff[16];
    test97_b3_read = (int)co_await co::read((HANDLE)0x1234, buff, sizeof(buff), NULL);

    co_return 0;
}

int test97_win_err_unset() {
    /* phase A first: load_win_fn caches its successes in globals, so the winsock-down probes
     * must run before winsock is ever started in this process */
    {
        auto pool = co::create_pool();
        pool->sched(test97_early_fails());
        ASSERT_FN(pool->run());
    }
    DBG("A1 ConnectEx: ok=%d err=%d", test97_a1_ok, test97_a1_err);
    ASSERT_FN(CHK_BOOL(test97_a1_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a1_err == (int)co::ERROR_GENERIC));
    DBG("A2 co::connect's translation: %d", test97_a2_translated);
    ASSERT_FN(CHK_BOOL(test97_a2_translated != (int)co::ERROR_OK));
    DBG("A3 AcceptEx: ok=%d err=%d count=%d", test97_a3_ok, test97_a3_err, test97_a3_cnt);
    ASSERT_FN(CHK_BOOL(test97_a3_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a3_err == (int)co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test97_a3_cnt == 0));
    DBG("A4 WSASendMsg: ok=%d err=%d count=%d", test97_a4_ok, test97_a4_err, test97_a4_cnt);
    ASSERT_FN(CHK_BOOL(test97_a4_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a4_err == (int)co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test97_a4_cnt == 0));
    DBG("A5 WSARecvMsg: ok=%d err=%d count=%d", test97_a5_ok, test97_a5_err, test97_a5_cnt);
    ASSERT_FN(CHK_BOOL(test97_a5_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a5_err == (int)co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test97_a5_cnt == 0));

    /* phase B: winsock up, the request-level failure paths must still write *err */
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    {
        auto pool = co::create_pool();
        pool->sched(test97_request_fails());
        ASSERT_FN(pool->run());
    }
    DBG("B1 ConnectEx(garbage socket): ok=%d err=%d", test97_b1_ok, test97_b1_err);
    ASSERT_FN(CHK_BOOL(test97_b1_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_b1_err == (int)co::ERROR_GENERIC));
    DBG("B2 DeviceIoControl(garbage handle): ok=%d err=%d", test97_b2_ok, test97_b2_err);
    ASSERT_FN(CHK_BOOL(test97_b2_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_b2_err == (int)co::ERROR_GENERIC));
    DBG("B3 co::read(garbage handle): %d", test97_b3_read);
    ASSERT_FN(CHK_BOOL(test97_b3_read < 0));

    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test97_win_err_unset() {
    DBG("IOCP wrappers only: epoll/kqueue take their errors straight from the OS calls");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test97_win_err_unset();
    print_test_result("018-041-reproduced_win_err_unset.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

Workspace `repro/fix/11/`: `base/colib.h` (untouched copy of the real colib.h), `colib.h` (the
patched copy), the repro and test sources with their include pointed at the workspace header,
`tests_common.h`, all build/run logs and binaries. (The previous attempt at this fix died to an
API error before creating the workspace — there was nothing to salvage, this is a fresh start.)
Build line per the fix spec: `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I. <src>.cpp
-Fe:<src>.exe -link`. The real colib.h was never written; `git diff` on it is empty.

1. Repro FAILS on the unpatched copy — fresh run, not just the archived verdict
   (`repro/fix/11/11-base-run.log`, exit 1):

   ```
   A1 ConnectEx: ok=0 err=1234 (sentinel 1234) lasterr=10093
   A2 co::connect's own translation on that failure: 0 (0 == ERROR_OK)
   A3 AcceptEx: ok=0 err=1234 (sentinel 1234)
   FAIL-REPRO A1: ConnectEx returned failure but left *err untouched (contract says ERROR_GENERIC when it failed)
   FAIL-REPRO A2: co::connect's `ok ? ERROR_OK : err` computes ERROR_OK for this failure - nothing connected, caller told success
   FAIL-REPRO A3: AcceptEx returned failure but left *err untouched
   VERDICT: FAIL-REPRO (at least one early-failure path hides its failure through *err)
   ```

2. Patched copy: the same repro PASSES (`11-patched-run.log`, exit 0). The required flip is
   exact — sentinel gone, ERROR_GENERIC in, translation no longer ERROR_OK — and the isolation
   controls stayed correct:

   ```
   A1 ConnectEx: ok=0 err=-1 (sentinel 1234) lasterr=10093
   A2 co::connect's own translation on that failure: -1 (0 == ERROR_OK)
   A3 AcceptEx: ok=0 err=-1 (sentinel 1234)
   C1 ConnectEx(garbage socket): ok=0 err=-1 (sentinel 1234) wsa=10038
   C2 DeviceIoControl(garbage handle): ok=0 err=-1 (sentinel 1234) lasterr=6
   C3 co::read(garbage handle): -1
   VERDICT: PASS (no unset-err failure observed)
   ```

   The regression test was run against both copies too: `[FAILED]` on the base copy at
   `A1 ConnectEx: ok=0 err=1234` (exit non-zero, twice), `[PASSED]` on the patched copy
   (exit 0, three runs, all eight probes at `err=-1` / a negative read).

3. No-regression, three tests against the patched copy (sources copied with only the include
   line changed; ws2_32 comes from tests_common.h's pragma):

   ```
   005-001-io.cpp                                -> [PASSED], exit 0
   018-024-reproduced_iocp_failed_request_reads_ok.cpp -> [PASSED], exit 0
   005-002-io_wait_event.cpp                     -> [PASSED], exit 0
   ```

   `005-001-io.cpp` is the load-bearing one: it drives the happy paths of eleven of the sixteen
   patched wrappers (`ConnectEx`, `AcceptEx`, `connect`/`accept`, `ConnectNamedPipe`,
   `TransactNamedPipe`, `ReadFile`, `WriteFile`, `DeviceIoControl`, `LockFileEx`,
   `ReadDirectoryChangesW`) — with `*err` passed as nullptr in most calls, which is what exercises
   the new `if (err)` guards. `018-024` drives the `ReadFile` wrapper's request-failure tail (the
   read-after-reset case), confirming the failure plumbing this fix sits beside is unchanged.
   `005-002` is thin on Windows (its substance is the Linux `wait_event`; it runs its stub here)
   and is listed for completeness.

4. Linux was NOT-CHECKED at first (no Linux toolchain then); it is checked since, see 5. kqueue
   stays NOT-CHECKED: its backend is work in progress, and every hunk is in the Windows block.

5. Re-checked 2026-10-14 on the header of that day (FIX_02, 06, 08, 09, 43, 44, 45, 47
   applied), with the rebased diff above:
   - the regression test, renumbered: before the fix FAILED at its first check (`A1 ConnectEx:
     ok=0 err=1234`), after it PASSED, all eight probes at `err=-1` or a negative read, 2 runs
     of 2;
   - the documented-usage probe of "What reaches it", on both headers;
   - Linux (WSL Ubuntu, g++ 15.2, epoll): the patched header preprocesses to the same code as
     the unpatched one, but for the `__LINE__` values after the Windows wrappers; the test's
     skip branch builds and passes, plain and under ASan and UBSan; the whole suite passes on
     that code (`[PASSED: 95]/[FAILED: 0]`, plain and under ASan and UBSan, no report);
   - the whole suite on Windows with the fix: `[PASSED: 96]/[FAILED: 0]` (the 95 tests plus
     this one);
   - ASan (MSVC) with the fix: `005-001`, `005-002`, `018-024`, `018-026`, `018-027`, `018-028`,
     `018-029` and this test exit 0 with no ASan report.

6. At apply time, 2026-10-28, on the header of that day (`wait_msg()` added the same day), in
   the applied shape (one helper, the count zeroed):

   | check                       | result                                                       |
   |-----------------------------|--------------------------------------------------------------|
   | documented usage reaches it | only when the OS fails under a correct call (the extension   |
   |                             | missing, no handles left); the test forces one face by a     |
   |                             | misuse, winsock never started, as "The bug" states           |
   | DESIGN.md                   | its rules concern the killer; the fix changes no wait and no |
   |                             | kill path                                                    |
   | epoll / kqueue              | every hunk is in the Windows block; the Linux suite builds   |
   |                             | and passes on the applied header; kqueue NOT-CHECKED (WIP)   |
   | contract / docs             | `*err` as documented, now kept; the count on an early        |
   |                             | failure now 0, `handle_done_req()`'s own rule; no            |
   |                             | declaration or doc comment changed, `02_api.md` stays        |
   | callers                     | `co::connect`, `co::accept`, `co::read`, `co::write`,        |
   |                             | `read_sz`, `write_sz` read: unchanged, correct once the      |
   |                             | wrappers write `*err`                                        |
   | pending fixes               | #12, #22, #23, #38, #39, #40: heads against tails, no shared |
   |                             | lines, re-checked on the applied header                      |
   | runs                        | the test fails before and passes after (2 of 2); Windows     |
   |                             | 97/97; Linux 96/96; MSVC ASan `005-001`, `018-024`,          |
   |                             | `018-041` clean                                              |

## Interactions

Same-region row from the tracker: io_pool_t Windows — 03 (awake_data) 06 (event close) 25 (clear)
12 (io_request pipe branch) 11 (wrappers, this one); plus the wrapper-tail fixes 22/23. Checked
again on 2026-10-14, with the fixes written after this one (38, 39, 40); none touches the
twenty exits.

- #12 (ConnectNamedPipe hang): special-cases `ERROR_PIPE_CONNECTED` inside `ConnectNamedPipe`'s
  `io_request` lambda — the request body this fix's early exits run before. No shared lines, no
  semantic overlap: whichever way #12 resolves the special case, the early exits still owe the
  caller `*err = ERROR_GENERIC`. The two fixes compose; neither subsumes the other.
- #22 (win_ipv6) / #23 (so_update_context): both touch the wrapper tails — the `connect`/`accept`
  sockaddr plumbing and `accept`'s `SO_UPDATE_ACCEPT_CONTEXT` block. My hunks are the early
  returns at the heads of the same functions; the diffs do not overlap. If #23 restructures
  `accept`'s error paths, its two early returns (which already write `*err`) remain the precedent
  this fix generalizes.
- #06 (applied 2026-09-29): `io_data_t` closes its event when it dies. At an early exit the
  event is NULL (it was never made, or the exit comes before it), so there is nothing for it to
  close. #25 (`clear()`) lives in `io_pool_t`, which this fix does not touch. #03 was removed on
  2026-10-13. Independent.
- #38, #39 (`co::accept`'s `SO_UPDATE_ACCEPT_CONTEXT` and its `GetAcceptExSockaddrs` load):
  branches of `co::accept` after `AcceptEx` returns, not `AcceptEx`'s own exits. #39's
  `load_win_fn` failure is the one in `co::accept`, which already writes `*err`; this fix covers
  the ones inside the wrappers. Independent.
- #40 (`owned_overlapped_t`): changes the type of `overlapped`, which the early exits don't
  read. Its own check built its test against this fix's wrappers; the two compose.
- #09 (applied 2026-10-14): independent; this fix changes what the wrappers write into a
  caller's out-parameter, not what `run()` reports.

## Open questions for the user

The first version had none forced — the fix implements the documented contract mechanically,
changes no signature, and adds no state. Three verified non-questions, recorded so they are not
re-litigated:

1. The `GetLastError()` half of the documented contract already holds on both early exits without
   any `SetLastError` addition: the failed `socket()`'s 10093 survived the wrapper's return in the
   repro, and `CreateEvent`/`WSACreateEvent` set the thread's last error themselves. Only the
   `*err` write was missing.
2. The `if (!hEvent)` checks on the `WSACreateEvent` sites are live, not dead code:
   `WSA_INVALID_EVENT`, that call's failure return, is `((WSAEVENT)NULL)` in the SDK headers, so
   the null test catches it. No check needed widening.
3. Noted, not changed (out of the mechanical scope, flagging for a ruling if you want it closed):
   the wrappers' success-side tail can still return FALSE with `*err == ERROR_OK` — when the
   request succeeded but `handle_done_req`'s `CloseHandle` of the event failed, `*err` already
   holds the request's ERROR_OK and the wrapper answers FALSE. Same contract-hole shape, opposite
   end, and effectively unforceable without handle-table games; #06 owns the surrounding
   event-close region. This fix deliberately does not touch that tail.
4. Asked 2026-10-14, the byte counts: the early exits also leave the byte-count out-parameter
   as the caller set it (the probe's `co::ReadFile` kept its 777), while `handle_done_req()`
   zeroes it on a failure, "as the sync Win32 calls leave it on a failure". No caller inside
   colib reads it there: `co::read`, `co::write` and `co::accept` start their counts at 0.
   Should the same exits zero it too (two more lines at each of the thirteen wrappers that take
   a count), or is it left as it is?
   Decided at apply time, 2026-10-28: zeroed, by the helper every exit answers through, so a
   failed wrapper looks the same to its caller whichever step failed. The test pins it (A3, A4
   and A5 pre-load the count with 777 and expect 0).
