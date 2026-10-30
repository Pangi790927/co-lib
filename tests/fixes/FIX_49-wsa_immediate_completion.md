# Fix #49: the WSA wrappers read an immediate completion as a failure

Status: [APPLIED 2026-10-29 on the user's word]

Found on 2026-10-29 while auditing the Windows wrappers at the user's request, during the apply
of fix #23 ("do we have some strangeness anywhere regarding those windows functions?"); applied
in the same batch on his word ("do a fix for them"). No proposal preceded it, so this record was
written at apply time.

Apply verification, 2026-10-29: the test fails on the header before the fix (the send reports
`ok 0, bytes 0, err -1`) and passes on the header as applied, with all three scenarios; the whole
suite on Windows `[PASSED: 109]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 108]/[FAILED: 0]`, every file but the
Windows-only `018-029`, this test stubbed there. The check table is the last item of "How it
was checked".

## The bug

`::WSASend`, `::WSARecv`, `::WSASendTo`, `::WSARecvFrom` and the `WSASendMsg` / `WSARecvMsg`
extension functions return an `int`: `0` when the operation completed at once (its completion
packet is still posted to the port), `SOCKET_ERROR` with `WSA_IO_PENDING` when it is pending,
`SOCKET_ERROR` with the reason when it failed. The six wrappers stored that `int` in a `bool` and
tested it as `ReadFile`'s `BOOL` is tested:

```cpp
bool ret = std::apply(::WSARecv, *params);
if (!ret && GetLastError() != ERROR_IO_PENDING)
    return ERROR_GENERIC;
```

The `0` of an immediate completion became `false`, `GetLastError()` was not `ERROR_IO_PENDING`,
and the request was reported failed: the wrapper answered FALSE with `ERROR_GENERIC`, zero bytes,
and the log line "Failed the io_request: The operation completed successfully." A send completes
at once nearly always (the buffers are free), a receive whenever the data is already there, so
`co::WSASend` was effectively unusable and `co::WSARecv` worked only for data that had not
arrived yet. A likely second consequence, not pursued once the cause was fixed: the request's
`io_data_t` was freed with the "failure" while Windows still posted its completion packet to the
port, whose OVERLAPPED pointer the drain casts back to an `io_data_t`.

Nothing in the suite moved socket data through these wrappers on Windows (`005-001` uses
`ConnectEx` / `AcceptEx` and the file calls; `018-041` uses `WSASendMsg` / `WSARecvMsg` on their
failure path), so nobody saw it. The `BOOL`-returning wrappers (`ReadFile`, `WriteFile`,
`ConnectEx`, `AcceptEx`, the pipes, `DeviceIoControl`, `LockFileEx`, `ReadDirectoryChangesW`,
`WaitCommEvent`) test their result right.

Measured on the header before the fix, with a loopback pair per scenario:

```
WSASend immediate: ok 0, sent 0, err -1
WSARecv immediate: ok 0, recvd 0, err -1
```

## Where the fix lands

| colib.h region                          | what changes                                | adds/dels |
|-----------------------------------------|---------------------------------------------|-----------|
| the `io_request` lambda of `WSASend`,   | the call's `int` is read as Winsock defines | +18/-12   |
| `WSASendTo`, `WSARecv`, `WSARecvFrom`,  | it: `SOCKET_ERROR` without `WSA_IO_PENDING` |           |
| `WSASendMsg`, `WSARecvMsg`              | is the failure; a comment states the three  |           |
|                                         | answers                                     |           |

Six identical hunks, nothing else. No declaration, doc comment or `02_api.md` entry changes.

## The change

`diff -u` of the header right before the batch of 2026-10-29 (fixes #23, #49, #50) against the
header as applied, this fix's hunks only.

Reasoning: the wrappers' contract is "the same as the Windows call", and the Windows call says
`0` for done-at-once; the packet comes either way, so the wrapper awaits it in both the `0` and
the pending case, exactly as the `BOOL` wrappers do for TRUE and for `ERROR_IO_PENDING`.
`WSA_IO_PENDING` and `ERROR_IO_PENDING` are the same value, so the existing comparison stays.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -5996,6 +5989,7 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
-        bool ret = std::apply(_wsa_send_msg, *params);
-        if (!ret && GetLastError() != ERROR_IO_PENDING)
+        /* 0: done at once, its packet still comes; SOCKET_ERROR: failed, or pending */
+        int rc = std::apply(_wsa_send_msg, *params);
+        if (rc == SOCKET_ERROR && GetLastError() != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
@@ -6030,6 +6024,7 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
-        bool ret = std::apply(::WSASendTo, *params);
-        if (!ret && GetLastError() != ERROR_IO_PENDING)
+        /* 0: done at once, its packet still comes; SOCKET_ERROR: failed, or pending */
+        int rc = std::apply(::WSASendTo, *params);
+        if (rc == SOCKET_ERROR && GetLastError() != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
@@ -6064,6 +6059,7 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
-        bool ret = std::apply(::WSASend, *params);
-        if (!ret && GetLastError() != ERROR_IO_PENDING)
+        /* 0: done at once, its packet still comes; SOCKET_ERROR: failed, or pending */
+        int rc = std::apply(::WSASend, *params);
+        if (rc == SOCKET_ERROR && GetLastError() != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
@@ -6098,6 +6094,7 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
-        bool ret = std::apply(::WSARecvFrom, *params);
-        if (!ret && GetLastError() != ERROR_IO_PENDING)
+        /* 0: done at once, its packet still comes; SOCKET_ERROR: failed, or pending */
+        int rc = std::apply(::WSARecvFrom, *params);
+        if (rc == SOCKET_ERROR && GetLastError() != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
@@ -6136,6 +6133,7 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
-        bool ret = std::apply(_wsa_recv_msg, *params);
-        if (!ret && GetLastError() != ERROR_IO_PENDING)
+        /* 0: done at once, its packet still comes; SOCKET_ERROR: failed, or pending */
+        int rc = std::apply(_wsa_recv_msg, *params);
+        if (rc == SOCKET_ERROR && GetLastError() != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
@@ -6170,6 +6168,7 @@
     desc.data->io_request = +[](void *ptr) -> error_e {
         params_t *params = (params_t *)ptr;
-        bool ret = std::apply(::WSARecv, *params);
-        if (!ret && GetLastError() != ERROR_IO_PENDING)
+        /* 0: done at once, its packet still comes; SOCKET_ERROR: failed, or pending */
+        int rc = std::apply(::WSARecv, *params);
+        if (rc == SOCKET_ERROR && GetLastError() != ERROR_IO_PENDING)
             return ERROR_GENERIC;
         return ERROR_OK;
```

## The regression test

Landed as `tests/018-053-reproduced_wsa_immediate_completion.cpp` (Test109) on 2026-10-29.

Three scenarios, each on a fresh loopback pair and a fresh pool (a socket keeps the first port
it was associated with): a `co::WSASend` of one byte (immediate), a `co::WSARecv` with the byte
already there (immediate), a `co::WSARecv` issued before the byte is sent from another thread
(pending). Each must answer TRUE, one byte, `ERROR_OK`, and the receives must see the byte. A 5 s
watchdog ends a hang. Stubbed on Linux.

On the header before the fix (a copy kept aside at apply time; the file with the completion
routine argument fix #50 removed, since that header still had it):

```
scenario 0: ok 0, bytes 0, err -1
Failed CHK_BOOL(test109_ok) err_str: The operation completed successfully.
[FAILED]: 018-053-reproduced_wsa_immediate_completion.cpp
```

On the header as applied:

```
scenario 0: ok 1, bytes 1, err 0
scenario 1: ok 1, bytes 1, err 0
scenario 2: ok 1, bytes 1, err 0
[PASSED]: 018-053-reproduced_wsa_immediate_completion.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test109 - Reproduced Bugs: the WSA wrappers read an immediate completion as a failure
================================================================================================= */

/* ::WSASend, ::WSARecv, ::WSASendTo, ::WSARecvFrom and the WSASendMsg/WSARecvMsg extensions return
an int: 0 when the operation completed at once (its completion packet still comes through the
port), SOCKET_ERROR with WSA_IO_PENDING when it is pending, SOCKET_ERROR with the reason when it
failed. The wrappers stored that int in a bool and tested it like ReadFile's BOOL, so the 0 of an
immediate completion read as "failed" with GetLastError() not ERROR_IO_PENDING: the request was
reported failed (ERROR_GENERIC, 0 bytes, "The operation completed successfully"), while its packet
was still on its way to the port. A send completes at once nearly always, a receive whenever the
data is already there. Nothing in the suite moved socket data through these wrappers, so nobody
saw it. Found 2026-10-29 while auditing the wrappers for fix #23; fix #49
(tests/fixes/FIX_49-wsa_immediate_completion.md): the wrappers read the int, SOCKET_ERROR without
WSA_IO_PENDING is the failure.

Three scenarios on loopback pairs, each pair and pool fresh (a socket keeps the first port it was
associated with): a WSASend (immediate), a WSARecv with the byte already there (immediate), and a
WSARecv issued before the byte is sent from another thread (pending). All three must succeed with
one byte. */

#include <chrono>
#include <thread>

#if COLIB_OS_WINDOWS

static BOOL test109_ok = FALSE;
static DWORD test109_bytes = 0;
static co::error_e test109_err = co::ERROR_OK;
static char test109_buf[8];

static co::task_t test109_sender(SOCKET s) {
    char x = 'x';
    WSABUF wb{ 1, &x };
    test109_ok = co_await co::WSASend(s, &wb, 1, &test109_bytes, 0, &test109_err);
    co_return 0;
}

static co::task_t test109_receiver(SOCKET s) {
    WSABUF wb{ sizeof(test109_buf), test109_buf };
    DWORD flags = 0;
    test109_ok = co_await co::WSARecv(s, &wb, 1, &test109_bytes, &flags, &test109_err);
    co_return 0;
}

/* a connected loopback pair: `c` the client's end, `srv` the server's */
static int test109_pair(SOCKET *c, SOCKET *srv) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(l != INVALID_SOCKET));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(l, (sockaddr *)&a, sizeof(a)) == 0));
    ASSERT_FN(CHK_BOOL(listen(l, 1) == 0));
    int len = sizeof(a);
    ASSERT_FN(CHK_BOOL(getsockname(l, (sockaddr *)&a, &len) == 0));
    *c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(*c != INVALID_SOCKET));
    ASSERT_FN(CHK_BOOL(::connect(*c, (sockaddr *)&a, sizeof(a)) == 0));
    *srv = ::accept(l, NULL, NULL);
    ASSERT_FN(CHK_BOOL(*srv != INVALID_SOCKET));
    closesocket(l);
    return 0;
}

static int test109_one(int scenario) {
    test109_ok = FALSE;
    test109_bytes = 0;
    test109_err = co::ERROR_OK;
    SOCKET c, srv;
    ASSERT_FN(test109_pair(&c, &srv));

    auto pool = co::create_pool();
    if (scenario == 0) {                        /* the send: done at once */
        pool->sched(test109_sender(c));
    }
    else if (scenario == 1) {                   /* the receive, the byte already there: at once */
        ASSERT_FN(CHK_BOOL(send(c, "x", 1, 0) == 1));
        pool->sched(test109_receiver(srv));
    }
    else {                                      /* the receive first, the byte later: pending */
        pool->sched(test109_receiver(srv));
        std::thread([c] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            send(c, "x", 1, 0);
        }).detach();
    }
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    closesocket(c);
    closesocket(srv);

    DBG("scenario %d: ok %d, bytes %lu, err %d", scenario, (int)test109_ok, test109_bytes,
            (int)test109_err);
    ASSERT_FN(CHK_BOOL(test109_ok));
    ASSERT_FN(CHK_BOOL(test109_bytes == 1));
    ASSERT_FN(CHK_BOOL(test109_err == co::ERROR_OK));
    if (scenario != 0)
        ASSERT_FN(CHK_BOOL(test109_buf[0] == 'x'));
    return 0;
}

static int test109_wsa_immediate_completion() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        printf("WATCHDOG: a wrapper never completed\n");
        fflush(stdout);
        std::_Exit(42);
    }).detach();

    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    for (int scenario = 0; scenario < 3; scenario++)
        ASSERT_FN(test109_one(scenario));
    return 0;
}

#else /* !COLIB_OS_WINDOWS */

static int test109_wsa_immediate_completion() {
    DBG("not runnable here: the WSA wrappers are Windows only");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test109_wsa_immediate_completion();
    print_test_result("018-053-reproduced_wsa_immediate_completion.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

1. The cause, by reading: the Win32 signatures (`int WSAAPI WSASend(...)` and the others) and
   the wrappers' `bool ret`.
2. The behavior, by a probe on the header before the fix (a send, a receive with the data there,
   a receive before the data): the two immediate ones failed with the log line above, the
   pending one worked.
3. The test fails before and passes after, as shown; the suites: see the status line.
4. At apply time, 2026-10-29, on the header of that day:

   | check                       | result                                                       |
   |-----------------------------|--------------------------------------------------------------|
   | documented usage reaches it | yes: any `co::WSASend`, and a `co::WSARecv` with data        |
   |                             | already received                                             |
   | DESIGN.md                   | not concerned (no kill path)                                 |
   | epoll / kqueue              | Windows-only code (`#if COLIB_OS_WINDOWS`); the test is      |
   |                             | stubbed on Linux and the Linux suite passes                  |
   | contract / docs             | the wrappers' doc already says "the same as the Windows      |
   |                             | call"; it now holds for the int-returning ones               |
   | callers                     | none in the header; `018-041` on the failure path, unchanged |
   |                             | in behavior                                                  |
   | pending fixes               | #22 (IPv6 in `co::connect` / `co::accept`) is elsewhere      |
   | runs                        | 2 of 2 on Windows; the suites: see the status line           |

## Interactions

- #11 (applied, `fail_unstarted_req`): the same wrappers, the early exits before the request;
  no shared lines.
- #50 (this batch): the same wrappers' signatures and tuples; no shared lines, the request
  lambda is untouched by #50.
- #23 (this batch): `co::connect`, elsewhere.

Decided at apply time, 2026-10-29, on the user's word: (1) fixed as a bug, in the batch with #23
and #50; (2) the int is read as Winsock defines it, nothing else changes; (3) the stray-packet
consequence is not investigated separately, since the cause is gone.
