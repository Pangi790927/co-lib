# Proposal: ConnectEx sockets must receive SO_UPDATE_CONNECT_CONTEXT (fix #23)

Status: [APPLIED 2026-10-29 on the user's word, in a different shape: the update in co::connect,
failing the connect when it fails; the ConnectEx wrapper unchanged]

Apply verification, 2026-10-29: the test fails on the header before the fix (`peer rc -1`,
`shutdown rc -1` after `co::connect`) and passes on the header as applied; the whole suite on
Windows `[PASSED: 109]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 108]/[FAILED: 0]`, every file but
the Windows-only `018-029`, this test stubbed there. Applied in one batch with #49 and #50, found
while applying it. The check table is the last item of "How it was checked"; the title above is
the proposal's, the shape applied is in "Where the fix lands".

## The bug

colib wraps the Winsock extension functions `ConnectEx` and `AcceptEx` for IOCP. `colib::ConnectEx`
issues the overlapped request, awaits it, runs `handle_done_req()` (which closes the request's
event) and `co_return`s true; `colib::AcceptEx` does the same for a listen/accept socket pair. The
wrong step is what neither wrapper (nor anything else in colib.h) ever did: call `setsockopt`. A
grep over the pre-rework header found zero occurrences — the only socket setup anywhere was the
`bind()` that `co::connect` performs because ConnectEx requires a bound socket.

MSDN's contract for both extensions is that the operation's success is not enough: the socket
ConnectEx completed on becomes usable as an ordinary connected socket only after the caller sets
`SO_UPDATE_CONNECT_CONTEXT` on it, and a socket AcceptEx accepted only after the caller sets
`SO_UPDATE_ACCEPT_CONTEXT` with the listening socket's handle. Until then the kernel gates the
connected-socket properties, and the documented symptom is `getpeername()` failing with
`WSAENOTCONN` on a socket that is genuinely connected.

The trigger is any Windows connect through the library — `co_await co::connect(s, ...)`, which on
Windows is exactly `bind()` + `colib::ConnectEx()` with no fallback branch — or a direct
`co_await colib::ConnectEx(...)`, followed by any connected-socket property call. The confirmed
consequences (verdict REPRODUCED, `repro/23-so_update_context.md`, deterministic 3/3; fresh runs
against the current tree this session, `repro/fix/23/base/run-23-base-*.log`):

- `getpeername` on the connected socket returns -1 / `WSAENOTCONN` (10057), the documented failure.
- `shutdown(SD_BOTH)` immediately after `colib::ConnectEx` returns -1 / `WSAENOTCONN`. This is the
  exact call `tests/005-001-io.cpp`'s client makes with the return code unchecked — its "shutdown
  works" reading was this silent failure all along. A copy of that test with the one check added
  fails on the unpatched tree (all three clients log code 0x2749 = 10057, then the counter
  assertion fails the test) and passes on the patched one.
- Before the accept rework, the accepted sockets failed both `getpeername` (`WSAENOTCONN`) and
  `getsockname` (`WSAEINVAL`, 10022).
- `getsockopt(SO_CONNECT_TIME)` succeeded everywhere, so the connections were established — the
  failures were purely the missing property mark, and one `setsockopt` control flipped each failing
  call to success on the same live socket, which isolates the cause.

The impact is wider than "getpeername consumers": colib's own `read`/`write` still move data
through the IOCP handles, so every existing test stayed green while anything layered on the
connected-socket properties — `getpeername`, `getsockname` on the accept side, `shutdown`, graceful
close — was silently broken on Windows.

Half of this bug is already fixed in the working tree: the staged accept rework (the
`GetAcceptExSockaddrs` decode in `co::accept`) carries the accept-side
`setsockopt(SO_UPDATE_ACCEPT_CONTEXT, ...)` itself. The fresh baseline against the current tree
confirms the split: both accepted sockets answer `getpeername` and `getsockname` (rc 0), while the
connect side is unchanged — `co::connect`'s socket still fails `getpeername` with `WSAENOTCONN`
and a direct `ConnectEx` socket still fails `shutdown`. This proposal is the connect half; it
composes with the in-flight accept work and reverts none of it.

## Where the fix lands

As applied on 2026-10-29, on the user's rulings: the update lives in `co::connect`, not in the
`colib::ConnectEx` wrapper ("in connect it makes sense, to make it look more linux like, but
connectex will be used by windows users that expect windows behaviour"), and a failed update
fails the connect, as fix #38 made the accept side do ("same idea, want error_generic and reason
in getlasterror").

| colib.h region                 | what changes                                                 | adds/dels |
|--------------------------------|--------------------------------------------------------------|-----------|
| `co::connect` (Windows)        | after a successful `ConnectEx`, `setsockopt(s, SOL_SOCKET,   | +11/-1    |
|                                | SO_UPDATE_CONNECT_CONTEXT, NULL, 0)`; a failure logs and     |           |
|                                | returns `ERROR_GENERIC`, the reason left in `GetLastError()` |           |
| `co::connect`'s doc comment    | states the mark and the failure                              | +4/-1     |

Total +15/-2. The `ConnectEx` wrapper and its doc are unchanged: its caller sets the context, as
with the Windows call. `docs/02_api.md`'s `connect` line mirrors the new doc.

## The change

`diff -u` of the header right before the batch of 2026-10-29 (fixes #23, #49, #50) against the
header as applied, this fix's hunks only. The proposal's diff (the update in the `ConnectEx`
wrapper, logging and continuing on failure) was not applied.

Reasoning per hunk:

- the doc comment: the contract where the function is declared, both halves: the mark, and that
  a failed update is a failed connect.
- `co::connect`: the connect's own result is answered first, as before; then the mark; a failed
  mark fails the connect with `ERROR_GENERIC`, the reason left where `setsockopt` put it (the
  log through `get_last_error()` leaves `GetLastError()` untouched, verified at apply time). The
  socket is the caller's, so nothing is closed. The comment says why the wrapper doesn't do it.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -2311,5 +2299,8 @@
 
 /*! This is the ported version of the Linux colib::connect, it is the same, but it takes
- * a socket handle as a parameter and uses colib::ConnectEx internally. */
+ * a socket handle as a parameter and uses colib::ConnectEx internally. On success the socket
+ * also carries SO_UPDATE_CONNECT_CONTEXT, so it is an ordinary connected socket (getpeername
+ * and shutdown work on it); a failure of that update fails the connect with ERROR_GENERIC, the
+ * reason in GetLastError(). 2026-10-29 */
 inline task_t        connect(SOCKET s, const sockaddr *sa, uint32_t len);
 
@@ -6203,5 +6202,15 @@
     error_e err = ERROR_OK;
     BOOL ok = co_await COLIB_REGNAME(ConnectEx(s, sa, len, NULL, 0, NULL, &err));
-    co_return ok ? ERROR_OK : err;  /* as on Linux: the stopper's error, or ERROR_GENERIC */
+    if (!ok)
+        co_return err;  /* as on Linux: the stopper's error, or ERROR_GENERIC */
+
+    /* ConnectEx leaves the socket unmarked: SO_UPDATE_CONNECT_CONTEXT makes it an ordinary
+    connected one, without which getpeername and shutdown fail on it. The ConnectEx wrapper
+    leaves this to its caller, as Windows does; connect is the Linux-like face. 2026-10-29 */
+    if (setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) {
+        COLIB_DEBUG("Failed to update connect context: %s", get_last_error().c_str());
+        co_return ERROR_GENERIC;
+    }
+    co_return ERROR_OK;
 }
 
```

## The regression test

Landed as `tests/018-052-reproduced_so_update_context.cpp` (Test108) on 2026-10-29; it was first
written as 018-031 / Test84, the names the older logs below carry. Its second client changed at
landing: the proposal expected the `ConnectEx` wrapper to mark the socket; under the ruling it
doesn't, so that client now pins the Windows contract (unmarked right after the wrapper,
`WSAENOTCONN` from `getpeername`) and then marks the socket itself and checks it works.

Two clients and a server on loopback. Client A connects through `co::connect`: `getpeername`
must answer with the listener's port and `shutdown` must succeed. Client B connects through
`colib::ConnectEx`: `getpeername` must fail with `WSAENOTCONN` until B sets the context itself,
after which `getpeername` and `shutdown` succeed. The server accepts both through `co::accept`
and checks `getpeername` and `getsockname` on each (the accept side, fix #38). A 10 s watchdog.
Stubbed on Linux.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
listen 54925 | co::connect rc 0, peer rc -1 port 0, own port 54926, shutdown rc -1 | ConnectEx bare peer rc -1 err 10057, after the caller's update: peer rc 0 port 54925, own port 54927, shutdown rc 0 | accepted peer rc 0/0 port 54926/54927, getsockname rc 0/0
Failed CHK_BOOL(test108_a_peer_rc == 0) err_str: The operation completed successfully.
[FAILED]: 018-052-reproduced_so_update_context.cpp
```

On the header as applied:

```
listen 54922 | co::connect rc 0, peer rc 0 port 54922, own port 54923, shutdown rc 0 | ConnectEx bare peer rc -1 err 10057, after the caller's update: peer rc 0 port 54922, own port 54924, shutdown rc 0 | accepted peer rc 0/0 port 54923/54924, getsockname rc 0/0
[PASSED]: 018-052-reproduced_so_update_context.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test108 - Reproduced Bugs: a co::connect socket came out without its connect context
================================================================================================= */

/* MSDN lets a socket from ConnectEx behave as an ordinary connected one only once it carries
SO_UPDATE_CONNECT_CONTEXT, and colib set it nowhere: getpeername answered WSAENOTCONN on a socket
co::connect had connected, and so did the shutdown right after it, the very call tests/005-001's
client makes and never checked. The accepted side carries SO_UPDATE_ACCEPT_CONTEXT since the
accept rework (fix #38 made its failure fail the accept). Reproduced 28-09-2026,
repro/23-so_update_context.md; fix #23 (tests/fixes/FIX_23-so_update_contexts.md), in the shape
the user chose: co::connect, the Linux-like face, sets the context and fails the connect when
that fails; the colib::ConnectEx wrapper stays the same as the Windows call, whose caller sets
the context, as this file's second client does for itself. */

#include <chrono>
#include <cstdio>
#include <thread>

#if COLIB_OS_WINDOWS

static SOCKET test108_listener = INVALID_SOCKET;
static uint16_t test108_listen_port = 0;
static SOCKET test108_sock_a = INVALID_SOCKET;   /* the co::connect leg   */
static SOCKET test108_sock_b = INVALID_SOCKET;   /* the co::ConnectEx leg */

static int test108_accepts = 0;
static int test108_acc_peer_rc[2] = { -1, -1 };
static int test108_acc_sock_rc[2] = { -1, -1 };
static uint16_t test108_acc_peer_port[2] = { 0, 0 };

static int test108_a_connect_rc = -99;
static int test108_a_peer_rc = -1;
static uint16_t test108_a_peer_port = 0;
static uint16_t test108_a_own_port = 0;
static int test108_a_shutdown_rc = -99;

static int test108_b_peer_rc_bare = -99;        /* right after the wrapper: the Windows contract */
static int test108_b_peer_err_bare = 0;
static int test108_b_peer_rc = -1;              /* after the caller's own update */
static uint16_t test108_b_peer_port = 0;
static uint16_t test108_b_own_port = 0;
static int test108_b_shutdown_rc = -99;

/* the probe the update marks gate: the call's rc, and the port it answered on success */
static int test108_name_call(SOCKET s, bool peer, uint16_t *port) {
    sockaddr_in a = {};
    int len = sizeof(a);

    int rc = peer ? getpeername(s, (sockaddr *)&a, &len) : getsockname(s, (sockaddr *)&a, &len);
    *port = (rc == 0) ? ntohs(a.sin_port) : 0;
    return rc;
}

static co::task_t test108_client_a() {
    test108_sock_a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_COFN(CHK_BOOL(test108_sock_a != INVALID_SOCKET));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test108_listen_port);

    /* on Windows co::connect is bind + ConnectEx + the context update */
    test108_a_connect_rc = co_await co::connect(test108_sock_a, (sockaddr *)&addr, sizeof(addr));
    ASSERT_COFN(test108_a_connect_rc);

    test108_a_peer_rc = test108_name_call(test108_sock_a, true, &test108_a_peer_port);
    test108_name_call(test108_sock_a, false, &test108_a_own_port);
    test108_a_shutdown_rc = shutdown(test108_sock_a, SD_BOTH);
    co_return 0;
}

static co::task_t test108_client_b() {
    test108_sock_b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_COFN(CHK_BOOL(test108_sock_b != INVALID_SOCKET));

    /* ConnectEx requires the socket to be bound first; co::connect does it itself */
    sockaddr_in any = {};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_COFN(CHK_BOOL(bind(test108_sock_b, (sockaddr *)&any, sizeof(any)) == 0));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test108_listen_port);

    BOOL ok = co_await co::ConnectEx(test108_sock_b, (sockaddr *)&addr, sizeof(addr), NULL, 0,
            NULL);
    ASSERT_COFN(CHK_BOOL(ok));

    /* the wrapper is the Windows call: the socket is connected but unmarked, as MSDN says */
    uint16_t unused = 0;
    test108_b_peer_rc_bare = test108_name_call(test108_sock_b, true, &unused);
    test108_b_peer_err_bare = WSAGetLastError();

    /* the caller's job, as with ::ConnectEx */
    ASSERT_COFN(CHK_BOOL(setsockopt(test108_sock_b, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0)
            == 0));
    test108_b_peer_rc = test108_name_call(test108_sock_b, true, &test108_b_peer_port);
    test108_name_call(test108_sock_b, false, &test108_b_own_port);
    test108_b_shutdown_rc = shutdown(test108_sock_b, SD_BOTH);
    co_return 0;
}

static co::task_t test108_server() {
    for (int i = 0; i < 2; i++) {
        sockaddr_in sa = {};
        uint32_t len = sizeof(sa);
        SOCKET c = co_await co::accept(test108_listener, (sockaddr *)&sa, &len);
        ASSERT_COFN(CHK_BOOL(c != INVALID_SOCKET));

        uint16_t own_port = 0;
        test108_acc_peer_rc[i] = test108_name_call(c, true, &test108_acc_peer_port[i]);
        test108_acc_sock_rc[i] = test108_name_call(c, false, &own_port);
        closesocket(c);
        test108_accepts++;
    }
    co_return 0;
}

static int test108_so_update_context() {
    /* the watchdog goes in before anything that could hang */
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        printf("WATCHDOG: still waiting for connect/accept\n");
        fflush(stdout);
        std::_Exit(42);
    }).detach();

    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));

    test108_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(test108_listener != INVALID_SOCKET));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(test108_listener, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(test108_listener, 4) == 0));
    int len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(test108_listener, (sockaddr *)&addr, &len) == 0));
    test108_listen_port = ntohs(addr.sin_port);

    auto pool = co::create_pool();
    pool->sched(test108_server());
    pool->sched(test108_client_a());
    pool->sched(test108_client_b());
    co::run_e ret = pool->run();

    if (test108_sock_a != INVALID_SOCKET)
        closesocket(test108_sock_a);
    if (test108_sock_b != INVALID_SOCKET)
        closesocket(test108_sock_b);
    closesocket(test108_listener);

    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test108_accepts == 2));

    DBG("listen %u | co::connect rc %d, peer rc %d port %u, own port %u, shutdown rc %d | "
            "ConnectEx bare peer rc %d err %d, after the caller's update: peer rc %d port %u, "
            "own port %u, shutdown rc %d | accepted peer rc %d/%d port %u/%u, getsockname rc %d/%d",
            test108_listen_port, test108_a_connect_rc, test108_a_peer_rc, test108_a_peer_port,
            test108_a_own_port, test108_a_shutdown_rc, test108_b_peer_rc_bare,
            test108_b_peer_err_bare, test108_b_peer_rc, test108_b_peer_port, test108_b_own_port,
            test108_b_shutdown_rc, test108_acc_peer_rc[0], test108_acc_peer_rc[1],
            test108_acc_peer_port[0], test108_acc_peer_port[1], test108_acc_sock_rc[0],
            test108_acc_sock_rc[1]);

    /* co::connect: a socket that knows its peer and takes a shutdown */
    ASSERT_FN(CHK_BOOL(test108_a_connect_rc == 0));
    ASSERT_FN(CHK_BOOL(test108_a_peer_rc == 0));
    ASSERT_FN(CHK_BOOL(test108_a_peer_port == test108_listen_port));
    ASSERT_FN(CHK_BOOL(test108_a_shutdown_rc == 0));

    /* colib::ConnectEx: the Windows contract, unmarked until its caller marks it */
    ASSERT_FN(CHK_BOOL(test108_b_peer_rc_bare != 0 && test108_b_peer_err_bare == WSAENOTCONN));
    ASSERT_FN(CHK_BOOL(test108_b_peer_rc == 0));
    ASSERT_FN(CHK_BOOL(test108_b_peer_port == test108_listen_port));
    ASSERT_FN(CHK_BOOL(test108_b_shutdown_rc == 0));

    /* the accepted side: peer and local names both answer, and each peer is one of the clients */
    for (int i = 0; i < 2; i++) {
        ASSERT_FN(CHK_BOOL(test108_acc_peer_rc[i] == 0));
        ASSERT_FN(CHK_BOOL(test108_acc_sock_rc[i] == 0));
        bool known_client = test108_acc_peer_port[i] == test108_a_own_port
                || test108_acc_peer_port[i] == test108_b_own_port;
        ASSERT_FN(CHK_BOOL(known_client));
    }
    ASSERT_FN(CHK_BOOL(test108_acc_peer_port[0] != test108_acc_peer_port[1]));
    return 0;
}

#else /* !COLIB_OS_WINDOWS */

static int test108_so_update_context() {
    DBG("not runnable here: SO_UPDATE_CONNECT_CONTEXT is the ConnectEx Windows contract");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test108_so_update_context();
    print_test_result("018-052-reproduced_so_update_context.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All builds from the workspace dirs with
`cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I. <src>.cpp -Fe:<exe> -link`. The base is the
real colib.h as it stands (with the staged accept rework); the patched copy differs from it by
exactly the diff above (`repro/fix/23/23-fix.diff`).

1. The repro FAILS on the unpatched copy, 3/3 fresh runs (`repro/fix/23/base/run-23-base-*.log`),
   and the failure is the connect half only, confirming both the bug and the in-flight accept-side
   fix:

   ```
   accepted#1: getpeername rc = 0 -> 127.0.0.1:61705
   accepted#1: getsockname rc = 0 -> 127.0.0.1:61704
   leg A: co::connect (bind + ConnectEx path) returned 0
   leg A client: getpeername rc = -1, WSAGetLastError = 10057 (WSAENOTCONN)
   leg A client: control setsockopt(SO_UPDATE_CONNECT_CONTEXT) rc = 0
   leg A client (after update): getpeername rc = 0 -> 127.0.0.1:61704
   leg B: co::ConnectEx ok, shutdown(SD_BOTH) rc = -1, WSAGetLastError = 10057 (WSAENOTCONN)
   summary: leg A getpeername rc = -1, accepted#1/#2 getpeername rc = 0/0, leg B shutdown rc = -1
   FAIL-REPRO: getpeername failed on the co::connect (ConnectEx) socket (rc = -1) before any
   SO_UPDATE_CONNECT_CONTEXT
   exit code: 1
   ```

2. The fix applied to `repro/fix/23/patched/colib.h`; the repro PASSES, 3/3 runs
   (`run-23-patched-*.log`; the copy's final PASS line was reworded to assert the fixed behavior,
   its rc checks untouched):

   ```
   leg A client: getpeername rc = 0 -> 127.0.0.1:61724
   leg A client: control setsockopt(SO_UPDATE_CONNECT_CONTEXT) rc = 0
   leg A client (after update): getpeername rc = 0 -> 127.0.0.1:61724
   leg B: co::ConnectEx ok, shutdown(SD_BOTH) rc = 0
   summary: leg A getpeername rc = 0, accepted#1/#2 getpeername rc = 0/0, leg B shutdown rc = 0
   PASS: getpeername/getsockname/shutdown worked on every ConnectEx/AcceptEx socket, the
   SO_UPDATE_*_CONTEXT marks now set by colib itself
   exit code: 0
   ```

   The controls were kept and still run; the leg A control re-applies the update on a socket the
   wrapper already marked and answers rc 0, which is the idempotence the log-and-continue choice
   leans on.

3. The regression test above, built and run against both copies: unpatched `[FAILED]` on
   `test84_a_peer_rc == 0` with `peer rc -1 ... shutdown rc -1` in its summary line; patched
   `[PASSED]` 3/3 with `peer rc 0 port 61763 ... shutdown rc 0 | accepted peer rc 0/0 ...
   getsockname rc 0/0`.

4. No-regression, the closest existing tests against the patched copy, all passing:

   ```
   [PASSED]: 005-001-io.cpp                              (the ConnectEx/AcceptEx/co::connect/co::accept suite)
   [PASSED]: 005-004-io_stop_handle.cpp                  (a TCP socket through the Windows io/stop paths)
   [PASSED]: 018-028-reproduced_accept_peer_address.cpp  (the in-flight accept fix's own test)
   [PASSED]: 005-001-checked-shutdown.cpp                (005-001 + the checked shutdown line, the companion above)
   ```

   The same strengthened 005-001 fails on the unpatched copy — the pre-fix proof of the silent
   shutdown failure (`repro/fix/23/base/run-005-001-checked-shutdown-base.log`).

5. No NOT-CHECKED lines: both hunks sit inside the `#if COLIB_OS_WINDOWS` region and were compiled
   and run on this box.

After the doc-comment hunk was reshaped into the pure addition shown above (the validated code is
byte-identical throughout), every patched-side build and run was repeated against the final header
(`run-23-patched-final.log`, `run-018-031-patched-final.log`, `run-r-*-final.log`): same results,
repro PASS, regression test `[PASSED]`, all four no-regression tests exit 0.

At apply time, 2026-10-29, on the header of that day, in the applied shape:

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: any `co::connect` on Windows followed by a connected-   |
|                             | socket property call; `005-001`'s client has been failing    |
|                             | its unchecked `shutdown` all along (it calls the wrapper,    |
|                             | so it stays the caller's job there)                          |
| DESIGN.md                   | not concerned                                                |
| epoll / kqueue              | Windows-only code; the test is stubbed on Linux and the      |
|                             | Linux suite passes                                           |
| contract / docs             | `co::connect`'s doc and its `02_api.md` line; the            |
|                             | `ConnectEx` wrapper's doc unchanged on purpose               |
| callers                     | `co::connect` has none in the header; the tests' clients     |
| pending fixes               | #22 (IPv6: the `bind` in `co::connect` hardcodes `AF_INET`)  |
|                             | edits the lines above this hunk; no shared lines             |
| runs                        | the test fails before (`peer rc -1`, `shutdown rc -1`) and   |
|                             | passes after; the suites: see the status line                |

## Interactions

The tracker row for the wrapper tails is `11 22 23`.

- #11 (the `*err` writes in the wrapper tails): it edits the same tail of `colib::ConnectEx` /
  `colib::AcceptEx` my functional hunk sits in — the `if (err) *err = ret;` and
  `handle_done_req()` lines just above it. There is no semantic overlap: #11's writes describe the
  awaited request's outcome, my `setsockopt` runs only on the path where that outcome was already
  success. Ordering only shifts context lines; whichever lands second re-diffs trivially. One
  composition note: if #11 also gives the tails a uniform failure shape, the fail-the-op
  alternative from Open questions would slot into it naturally — one more reason it stays the
  user's call.
- #22 (the `AF_INET` hardcodes in `co::connect` / `co::accept`): my diff touches neither function
  — the hardcodes are the `bind()` temp address in `co::connect`'s head and the `socket(AF_INET,
  ...)` call in `co::accept`'s head, both far from the wrapper tail. Fully orthogonal; shared row
  only because the fixes cluster around the same connect/accept surface.
- The in-flight accept rework (staged in the tree): composed with, not reverted — the `co::accept`
  decode and its `SO_UPDATE_ACCEPT_CONTEXT` line are byte-identical in the patched copy, and its
  own test (`018-028`) passes against the patched copy.

## Open questions for the user

1. The failure handling of the update call, which the briefing flagged as arguable. This proposal
   logs (`COLIB_DEBUG`) and continues: the connection is live and its data plane works without the
   mark, MSDN documents no failure for the call, and the repro's controls — including re-applying
   it on an already-marked socket — always answered rc 0. The alternative is to fail the operation
   (`*err = ERROR_GENERIC; co_return false;`) so a caller never receives a socket that lacks the
   connected properties; the cost is repudiating a working connection over a mark, and diverging
   from the accept-side update, which is unchecked. Say the word and it flips.
2. The accept side's layer. In the tree the update lives in `co::accept` (your line, dated
   27-09-2026); direct `colib::AcceptEx` callers — `tests/005-001`'s server is one — still receive
   unmarked accepted sockets, whose scope-`shutdown` calls fail silently today and stay that way
   under this proposal. The symmetric alternative is to move the update into the `AcceptEx`
   wrapper (the listen socket is its parameter) and drop the `co::accept` line: every user gets a
   marked socket from one place, matching where this fix puts the connect update. I did not move
   your line without your word; the move is four lines in the wrapper plus the deletion, and the
   regression test above covers it unchanged either way.
3. Test numbering: `018-031` / Test84 is the next free slot at the time of writing (sibling
   proposals already used `018-029`/`018-030` in their workspaces); renumber at land time if the
   suite says otherwise.
4. `docs/02_api.md`'s `ConnectEx()` entry mirrors the header's doc comment, so the new sentence
   lands there in the same pass as the fix, per `docs/CLAUDE.md`'s sync obligation.

Decided at apply time, 2026-10-29, on the user's word: (1) a failed update fails the connect,
`ERROR_GENERIC` with the reason in `GetLastError()`, as the accept side does since #38; (2) the
update lives in `co::connect`, the Linux-like face; the `ConnectEx` wrapper stays the Windows
call, whose caller sets the context; the accept side stays in `co::accept` for the same reason;
(3) the test landed as `018-052`, its second client reshaped to the ruling; (4) `02_api.md`'s
`connect` line mirrors the doc, the `ConnectEx` entry is unchanged. Found while applying it and
fixed in the same batch: #49 (the WSA wrappers' int result) and #50 (the completion-routine
parameter).
