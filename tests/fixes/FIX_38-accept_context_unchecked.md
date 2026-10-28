# Proposal: co::accept checks SO_UPDATE_ACCEPT_CONTEXT (fix #38)

Status: [APPLIED 2026-10-28 on the user's word]

Apply verification, 2026-10-28: the test fails on the header before the fix (a socket answered,
`accept err 0`) and passes on the header as applied, two runs of two; the whole suite on Windows
`[PASSED: 101]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 100]/[FAILED: 0]`,
every file but the Windows-only `018-029`; MSVC ASan builds of `018-042` to `018-045`, `005-001`,
`018-024` and `018-029` exit 0 with no report. The check table is the last item of "How it was
checked".

## The bug

On Windows, `co::accept(s, sa, len, err)` creates the accept socket, awaits `colib::AcceptEx` on
the listener `s`, decodes the peer address with `GetAcceptExSockaddrs`, and then sets
`SO_UPDATE_ACCEPT_CONTEXT` on the accepted socket with the listener's handle. The comment above
that tail states why the update is needed: without it, `getpeername` and `shutdown` fail on the
accepted socket. The wrong step is the last line before `co_return client_sock`: the result of
`setsockopt(client_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, ...)` is discarded, so `co::accept`
returns the socket as a success whether the update took or not.

The trigger is any failure of that `setsockopt`. The one reachable from the public API is the
listener going away between the completion of `AcceptEx` and the update: an `UNWAIT_IO` callback
inherited into `co::accept` runs exactly there, and closing the listener from it makes the update
fail with `WSAENOTSOCK` (10038). The regression test below uses that sequence.

The consequence was confirmed on this box (Windows 10 19045, MSVC 14.43): `co::accept` returned a
valid socket with `*err == ERROR_OK`, and `getpeername` on that socket failed with `WSAENOTCONN`
(10057). That is the documented symptom of a missing accept context. The caller has no signal that
anything went wrong until a later `getpeername` or `shutdown` fails. Found by the cloud review of
10-10-2026, reproduced the same day.

## Where the fix lands

As applied on 2026-10-28; the shape is the proposal's, with the close going through the local
`close_keep_error` lambda that fix #39 introduces in the same function (open question 2, decided
by the user: a helper, as a lambda).

| colib.h region               | what changes                                      | adds/dels |
|------------------------------|---------------------------------------------------|-----------|
| `co::accept` definition (the | the result of the `SO_UPDATE_ACCEPT_CONTEXT`      | +8/-1     |
| Windows port), its tail      | update is checked; a failure logs, closes the     |           |
| after the address decode     | accepted socket keeping the update's error in     |           |
|                              | `GetLastError()`, writes `ERROR_GENERIC` to       |           |
|                              | `*err` and returns INVALID_SOCKET                 |           |

One hunk. The doc comment of `co::accept` already states the failure contract this hunk now keeps
("on INVALID_SOCKET the optional err tells why ... ERROR_GENERIC (then the reason is
GetLastError())"), so it does not change, and neither does `docs/02_api.md`. The policy (open
question 1) is the user's: a failed update fails the operation, as the Win32 counterpart would,
on both sides; the connect side is fix #23's.

## The change

`diff -u` of the header as it stood on 2026-10-28 right before this batch of fixes (#12, #38, #39,
#40 and #48, applied the same day on the user's word; #11 and `wait_msg()` earlier that day)
against the header as applied.

Reasoning: the update's result decides the answer. On a failure the reason is read for the log
first, the accepted socket is closed through `close_keep_error`, which saves the last error
around the close and restores it, and the caller gets the documented failure: `ERROR_GENERIC` in
`*err`, INVALID_SOCKET, the update's own error in `GetLastError()`. The call is split over two
lines to stay within 100 columns.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -6205,7 +6239,14 @@
     memcpy(sa, remote_sa, copied);
     *len = copied;
 
-    setsockopt(client_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&s, sizeof(s));
+    if (setsockopt(client_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&s, sizeof(s))
+            != 0) {
+        COLIB_DEBUG("Failed to update accept context: %s", get_last_error().c_str());
+        close_keep_error(client_sock);
+        if (err)
+            *err = ERROR_GENERIC;
+        co_return INVALID_SOCKET;
+    }
     co_return client_sock;
 }
 
```

## The regression test

Landed as `tests/018-042-reproduced_accept_context_unchecked.cpp` (Test98); it was first written
as 018-046 / Test87, the name the older logs below carry.

The listener is closed from an `UNWAIT_IO` callback inherited into `co::accept`, which runs after
`AcceptEx` completed and before the context update, so the update fails with `WSAENOTSOCK`. The
accept must then answer INVALID_SOCKET, `ERROR_GENERIC` and a non-zero reason; a socket answered
instead fails the test, after a `getpeername` on it for the log.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
listener closed 1, accept err 0, last error 10038
Failed CHK_BOOL(test98_ok) err_str: The operation completed successfully.
[FAILED]: 018-042-reproduced_accept_context_unchecked.cpp
```

On the header as applied, two runs of two:

```
listener closed 1, accept err -1, last error 10038
[PASSED]: 018-042-reproduced_accept_context_unchecked.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test98 - Reproduced Bugs: co::accept on Windows answered success when the accept context failed
================================================================================================= */

/* co::accept must not hand out a socket on which getpeername and shutdown fail. On Windows the
accepted socket becomes an ordinary connected one only once SO_UPDATE_ACCEPT_CONTEXT is set on it
with the listener's handle; co::accept set it but ignored the result, and returned the socket as a
success when the update failed. The listener is closed here from an UNWAIT_IO callback, after
AcceptEx completed and before co::accept updates the context, which makes the update fail. The
accept must then answer INVALID_SOCKET with ERROR_GENERIC and a reason in GetLastError(). Found by
review 10-10-2026; fix #38 (tests/fixes/FIX_38-accept_context_unchecked.md). */

#if COLIB_OS_WINDOWS

static SOCKET test98_srv = INVALID_SOCKET;
static bool   test98_closed = false;
static bool   test98_ok = false;
static int    test98_err = 0;
static DWORD  test98_last_error = 0;

static co::task_t test98_acceptor() {
    sockaddr_in sa = {};
    uint32_t len = sizeof(sa);
    co::error_e err = co::ERROR_OK;
    SOCKET c = co_await co::accept(test98_srv, (sockaddr *)&sa, &len, &err);
    test98_last_error = GetLastError();
    test98_err = err;
    if (c != INVALID_SOCKET) {
        sockaddr_in peer = {};
        int plen = sizeof(peer);
        int rc = getpeername(c, (sockaddr *)&peer, &plen);
        DBG("accept answered a socket, getpeername rc %d (%d)", rc, WSAGetLastError());
        closesocket(c);
        co_return -1;
    }
    test98_ok = true;
    co_return 0;
}

int test98_accept_context_unchecked() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    test98_srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(test98_srv != INVALID_SOCKET));
    FnScope close_srv([]{ if (!test98_closed) closesocket(test98_srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(test98_srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(test98_srv, 1) == 0));
    int len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(test98_srv, (sockaddr *)&addr, &len) == 0));

    /* the client connects before the accept is asked for: the listen queue holds it */
    SOCKET cl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    FnScope close_cl([cl]{ closesocket(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));

    /* the accept's only wait is AcceptEx's: its end is the moment to close the listener */
    auto closer = co::create_modif<co::CO_MODIF_UNWAIT_IO_CBK>(co::CO_MODIF_INHERIT_ON_CALL,
        [](co::state_t *, co::io_desc_t&) -> co::error_e {
            if (!test98_closed) {
                closesocket(test98_srv);
                test98_closed = true;
            }
            return co::ERROR_OK;
        });

    auto pool = co::create_pool();
    pool->sched(co::add_modifs(pool.get(), test98_acceptor(), co::modif_pack_t(1, closer)));
    ASSERT_FN(pool->run());

    DBG("listener closed %d, accept err %d, last error %lu", test98_closed, test98_err,
            test98_last_error);
    ASSERT_FN(CHK_BOOL(test98_closed));
    ASSERT_FN(CHK_BOOL(test98_ok));
    ASSERT_FN(CHK_BOOL(test98_err == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test98_last_error != 0));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test98_accept_context_unchecked() {
    DBG("SO_UPDATE_ACCEPT_CONTEXT is a Windows-only step: nothing to check here");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test98_accept_context_unchecked();
    print_test_result("018-042-reproduced_accept_context_unchecked.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

Workspace `repro/fix/38/`: `base/colib.h` is a copy of the current working-tree header (with the
uncommitted accept rework), `patched/colib.h` carries the hunk above, and `38-fix.diff` is
`diff -u` of the two. Built with the fix spec's command (`cl -nologo -EHsc -await:strict
-std:c++20 -Zi -FS -I.`), test include switched to `"colib.h"`.

1. The test on the unpatched copy fails (`run-018-046-base.log`):
   ```
   018-046-reproduced_accept_context_unchecked.cpp:  36 test87_acceptor() :> accept answered a socket, getpeername rc -1 (10057)
   018-046-reproduced_accept_context_unchecked.cpp:  78 test87_accept_context_unchecked() :> listener closed 1, accept err 0, last error 10038
   018-046-reproduced_accept_context_unchecked.cpp:  80 test87_accept_context_unchecked() :> Failed CHK_BOOL(test87_ok) err_str: The operation completed successfully.
   [FAILED]: 018-046-reproduced_accept_context_unchecked.cpp
   ```
2. The same test on the patched copy passes (`run-018-046-patched.log`):
   ```
   colib.h:6069 accept() :> Failed to update accept context: An operation was attempted on something that is not a socket. [0x2736]
   018-046-reproduced_accept_context_unchecked.cpp:  78 test87_accept_context_unchecked() :> listener closed 1, accept err -1, last error 10038
   [PASSED]: 018-046-reproduced_accept_context_unchecked.cpp
   ```
3. The closest existing tests pass on the patched copy: `018-028-reproduced_accept_peer_address`
   (peer port and `getpeername` still answer the client's port) and `005-001-io` (the
   accept/connect/read/write round trip).
4. Composed with #39 in `repro/fix/38/combined/`: both hunks applied to one copy, `018-046` and
   `018-047` both pass.
5. A standalone program in the session scratchpad confirmed that a successful `closesocket` resets
   `GetLastError()` to 0, which is why the branch saves and restores it.

NOT-CHECKED: the non-Windows branch of the test was not compiled (no Linux toolchain here); it is
a two-line skip. The hunk itself sits inside `#if COLIB_OS_WINDOWS`, so the epoll and kqueue
backends see no change.

At apply time, 2026-10-28, on the header of that day, in the applied shape:

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | only when the update fails: the listener gone between the    |
|                             | `AcceptEx` completion and the update (the test's `UNWAIT_IO` |
|                             | close), or an OS failure; no plain input causes it           |
| DESIGN.md                   | its rules concern the killer; no wait and no kill path       |
|                             | changes                                                      |
| epoll / kqueue              | the hunk is in the Windows block; the Linux suite builds and |
|                             | passes on the applied header; kqueue NOT-CHECKED (WIP)       |
| contract / docs             | the failure contract `co::accept` documents, now kept; no    |
|                             | doc comment changed, `02_api.md` stays                       |
| callers                     | `co::accept`'s callers already handle INVALID_SOCKET with    |
|                             | `*err` (the tests, `examples/06_0_chat`)                     |
| pending fixes               | #23 (the connect side) follows the same policy, fail; #22    |
|                             | touches `co::accept`'s socket creation, not its tail         |
| runs                        | the test fails before (a socket answered, `err 0`) and       |
|                             | passes after (2 of 2); Windows 101/101; Linux 100/100; MSVC  |
|                             | ASan `018-042` to `018-045`, `005-001`, `018-024`, `018-029` |
|                             | clean                                                        |

## Interactions

- **#39** (the `GetAcceptExSockaddrs` load-failure branch of the same function): a different branch,
  a few lines above. The two compose; the combined copy was built and both tests pass. Either
  order applies cleanly.
- **#23** (`SO_UPDATE_CONNECT_CONTEXT` in `colib::ConnectEx`): its open question 1 chooses
  log-and-continue for a failed connect update and names this accept update as unchecked; its open
  question 2 offers to move this update into the `colib::AcceptEx` wrapper. If the update moves,
  this check moves with it unchanged. The test still holds there, because `UNWAIT_IO` fires before
  the wrapper's tail as well.
- **#22** (the AF_INET hardcodes): changes how `co::accept` creates its socket at the head of the
  function, not the tail. Independent.
- **#11** (early exits writing `*err`): edits the `colib::AcceptEx` wrapper, not `co::accept`.
  Independent.

## Open questions for the user

1. **Fail or log.** This proposal fails the accept. #23 proposes log-and-continue for the connect
   side, arguing that a live connection should not be repudiated over a mark. The two updates
   should probably follow one policy; whichever you choose, the other proposal flips to match. The
   case for failing on the accept side is that the caller gets no other signal, and the socket
   it would receive cannot answer `getpeername` or `shutdown`.
2. **A shared helper.** With this fix and #39, `co::accept` has three branches that save the last
   error, close `client_sock` and restore it. A small helper (for example
   `close_keep_error(client_sock)`) would replace the three triples. I did not add it without your
   word (rule 9).
3. **The trigger.** Closing the listener from an `UNWAIT_IO` callback is the only way to force the
   failure from the public API. It is a contrived sequence, but a real one; say so if you would
   rather keep this test out of the suite.

Decided at apply time, 2026-10-28, on the user's word: (1) fail: a failed update fails the
operation, as the Win32 counterpart would, on both sides (the connect side is fix #23's, still
pending); (2) a helper, as a lambda local to `co::accept` (`close_keep_error`), landed with fix
#39; (3) the test is in the suite.
