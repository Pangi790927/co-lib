# Proposal: co::accept keeps the reason of a failed GetAcceptExSockaddrs load (fix #39)

Status: [APPLIED 2026-10-28 on the user's word]

Apply verification, 2026-10-28: the test fails on the header before the fix (`last error 0`) and
passes on the header as applied, two runs of two; the whole suite on Windows
`[PASSED: 101]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 100]/[FAILED: 0]`,
every file but the Windows-only `018-029`; MSVC ASan builds of `018-042` to `018-045`, `005-001`,
`018-024` and `018-029` exit 0 with no report. The check table is the last item of "How it was
checked".

## The bug

On Windows, `co::accept(s, sa, len, err)` loads `GetAcceptExSockaddrs` on first use, after
`colib::AcceptEx` has completed: `load_win_fn(WSAID_GETACCEPTEXSOCKADDRS, ...)` asks `WSAIoctl` for
the extension pointer on a dummy socket. When that load fails, `co::accept` closes the accepted
socket, writes `ERROR_GENERIC` to `*err` and returns INVALID_SOCKET. The doc comment of
`co::accept` promises that in this case the reason is `GetLastError()`. The wrong step is the
`closesocket(client_sock)` between the failure and the return: a successful `closesocket` resets
the thread's last error to 0, so the reason the load left there is gone before the caller can read
it.

The `AcceptEx` failure branch earlier in the same function already guards against exactly this:
it saves `GetLastError()` before its `closesocket` and restores it after, with the comment "the
accept's reason, not the close's". The load-failure branch, added with the accept rework, does not.

The trigger is any failure of the load, which means a failure of the dummy `socket()` or of
`WSAIoctl` inside `load_win_fn`. No input to `co::accept` causes either one; the regression test
forces it by standing in for `WSAIoctl`.

The consequence was confirmed on this box (Windows 10 19045, MSVC 14.43): with `WSAIoctl` failing
the load with `WSAEOPNOTSUPP` (10045), `co::accept` answered INVALID_SOCKET with `ERROR_GENERIC`
and `GetLastError() == 0`. A caller that reports the reason, as `examples/06_0_chat` does with
`last_os_error()`, prints "The operation completed successfully". A standalone program confirmed
the mechanism separately: a successful `closesocket` set the last error to 0 in four closes out of
four. Found by the cloud review of 10-10-2026, reproduced the same day.

## Where the fix lands

As applied on 2026-10-28. The proposal's two lines became a local lambda, `close_keep_error`,
used by the three branches of `co::accept` that close the accepted socket after a failure: the
`AcceptEx` failure branch (which had the two lines already), this fix's load-failure branch, and
the update-failure branch fix #38 adds (open question 2, decided by the user: a helper, as a
lambda). Fix #48, `load_win_fn`'s leak on the same failure, landed with it and shares its test.

| colib.h region               | what changes                                      | adds/dels |
|------------------------------|---------------------------------------------------|-----------|
| `co::accept` definition (the | `close_keep_error`, a lambda that saves the last  | +8/-0     |
| Windows port), after the     | error, closes the socket it is given and restores |           |
| accepted socket is made      | the error                                         |           |
| the `AcceptEx` failure       | its own three lines become the lambda call        | +1/-3     |
| branch                       |                                                   |           |
| the `GetAcceptExSockaddrs`   | `closesocket` becomes the lambda call: the load's | +1/-1     |
| load failure branch          | reason survives the close                         |           |

The doc comment already states the contract this keeps, so it does not change, and neither does
`docs/02_api.md`.

## The change

`diff -u` of the header as it stood on 2026-10-28 right before this batch of fixes (#12, #38, #39,
#40 and #48, applied the same day on the user's word; #11 and `wait_msg()` earlier that day)
against the header as applied.

Reasoning: `closesocket` resets the thread's last error when it succeeds, so every branch that
closes the accepted socket after a failure has to save the error around the close. One lambda in
the function does it for all three branches; the branch that existed already said why in its own
comment ("the accept's reason, not the close's"), which the lambda's comment now carries for all.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -6169,6 +6197,14 @@
         co_return INVALID_SOCKET;
     }
 
+    /* closes the accepted socket keeping the error of the failure that led here: a successful
+    closesocket resets the thread's last error */
+    auto close_keep_error = [](SOCKET sock) {
+        DWORD last_error = GetLastError();
+        closesocket(sock);
+        SetLastError(last_error);
+    };
+
     /* TODO: status:investigatory. The buffer size here reads *len, and the copy at the end writes
     through sa and len, with no check for NULL first, while the Linux accept passes NULL on to
     ::accept, which allows it. To check: whether a NULL sa or len can reach here, and whether it
@@ -6180,9 +6216,7 @@
             AcceptEx(s, client_sock, &addr_buff[0], 0, *len + 16, *len + 16, &rlen, err));
     if (!ok) {
         COLIB_DEBUG("Failed accept: %s", get_last_error().c_str());
-        DWORD last_error = GetLastError();  /* the accept's reason, not the close's */
-        closesocket(client_sock);
-        SetLastError(last_error);
+        close_keep_error(client_sock);
         co_return INVALID_SOCKET;
     }
 
@@ -6192,7 +6226,7 @@
     27-09-2026-10:20 */
     if (!_get_accept_ex_sockaddrs
             && load_win_fn(WSAID_GETACCEPTEXSOCKADDRS, _get_accept_ex_sockaddrs) != ERROR_OK) {
-        closesocket(client_sock);
+        close_keep_error(client_sock);
         if (err)
             *err = ERROR_GENERIC;
         co_return INVALID_SOCKET;
```

## The regression test

Landed as `tests/018-043-reproduced_accept_load_last_error.cpp` (Test99), shared with fix #48; it
was first written as 018-047 / Test88, the name the older logs below carry.

No input to `co::accept` makes the load fail, so the test stands in for `WSAIoctl` with a macro,
which needs Winsock included before `colib.h`: the suite's one exception to its include order,
stated in the file's first comment (open question 1, decided by the user: in the suite). The load
of `GetAcceptExSockaddrs` is made to fail with `WSAEOPNOTSUPP`; the accept must answer
INVALID_SOCKET, `ERROR_GENERIC` and that code in `GetLastError()`. The failing accept runs twice,
each run with its own listener (a socket keeps the completion port it was first associated with,
so one listener cannot serve a second pool), and the process's handle count after the second run
must equal the count after the first: that is fix #48's check, one dummy socket leaked per failed
load before it.

On the header before the fixes (a copy kept aside at apply time), built through the tests
makefile, it fails at the reason:

```
accept err -1, last error 0 (expected 10045)
Failed CHK_BOOL(test99_last_error == WSAEOPNOTSUPP) err_str: The operation completed successfully.
[FAILED]: 018-043-reproduced_accept_load_last_error.cpp
```

With fix #39 in and only fix #48's hunk reverted, it fails at the count:

```
handles after the first failing accept 83, after the second 84
Failed CHK_BOOL(after == before) err_str: The operation completed successfully.
[FAILED]: 018-043-reproduced_accept_load_last_error.cpp
```

On the header as applied, two runs of two:

```
accept err -1, last error 10045 (expected 10045)
accept err -1, last error 10045 (expected 10045)
handles after the first failing accept 76, after the second 76
[PASSED]: 018-043-reproduced_accept_load_last_error.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

/* The load of GetAcceptExSockaddrs fails only when WSAIoctl does, which no input to co::accept can
cause. This test therefore stands between colib.h and WSAIoctl: Winsock comes in first, under the
macros colib.h would set itself, and colib.h then calls test99_wsaioctl in WSAIoctl's place. That
is the one exception to the suite's include order (colib.h first), for that reason only. */
#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# define _WIN32_WINNT 0x0600
# include <winsock2.h>
# include <mswsock.h>
# include <windows.h>
# include <string.h>

static bool test99_fail_load = false;

static int test99_wsaioctl(SOCKET s, DWORD code, LPVOID in, DWORD in_len, LPVOID out,
        DWORD out_len, LPDWORD ret_len, LPWSAOVERLAPPED ov,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE cbk)
{
    GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
    if (test99_fail_load && code == SIO_GET_EXTENSION_FUNCTION_POINTER
            && in_len == sizeof(guid) && memcmp(in, &guid, sizeof(guid)) == 0) {
        WSASetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }
    return WSAIoctl(s, code, in, in_len, out, out_len, ret_len, ov, cbk);
}

# define WSAIoctl test99_wsaioctl
#endif /* _WIN32 */

#include "../colib.h"
#include "tests_common.h"

/* Test99 - Reproduced Bugs: co::accept on Windows lost why its GetAcceptExSockaddrs load failed,
and load_win_fn leaked its dummy socket on that failure
================================================================================================= */

/* co::accept documents that on INVALID_SOCKET with ERROR_GENERIC the reason is GetLastError(). When
the load of GetAcceptExSockaddrs failed, co::accept closed the accepted socket before returning, and
the successful closesocket reset the thread's last error to 0, so the caller read no reason at all
(fix #39). load_win_fn itself returned from a failed WSAIoctl without closing the dummy socket it
had made for the call: one socket leaked per failed load (fix #48). The load is made to fail here
with WSAEOPNOTSUPP; the accept must answer INVALID_SOCKET, ERROR_GENERIC and that same code in
GetLastError(), and a second failing accept must leave the process's handle count where the first
one left it. Each run makes its own listener: a socket keeps the completion port it was first
associated with, so one listener cannot serve a second pool. Found by review 10-10-2026. */

#if COLIB_OS_WINDOWS

static bool  test99_ok = false;
static int   test99_err = 0;
static DWORD test99_last_error = 0;

static co::task_t test99_acceptor(SOCKET srv) {
    sockaddr_in sa = {};
    uint32_t len = sizeof(sa);
    co::error_e err = co::ERROR_OK;
    SOCKET c = co_await co::accept(srv, (sockaddr *)&sa, &len, &err);
    test99_last_error = GetLastError();
    test99_err = err;
    if (c != INVALID_SOCKET) {
        closesocket(c);
        co_return -1;
    }
    test99_ok = true;
    co_return 0;
}

/* one listener, one client that connects before the accept is asked for, one accept with the
load failing; everything is closed again before returning, so two runs make the same handles */
static int test99_failing_accept() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(srv != INVALID_SOCKET));
    FnScope close_srv([srv]{ closesocket(srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(srv, 1) == 0));
    int len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &len) == 0));

    SOCKET cl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(cl != INVALID_SOCKET));
    FnScope close_cl([cl]{ closesocket(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));

    test99_ok = false;
    test99_fail_load = true;
    {
        auto pool = co::create_pool();
        pool->sched(test99_acceptor(srv));
        ASSERT_FN(pool->run());
    }
    test99_fail_load = false;

    DBG("accept err %d, last error %lu (expected %d)", test99_err, test99_last_error,
            WSAEOPNOTSUPP);
    ASSERT_FN(CHK_BOOL(test99_ok));
    ASSERT_FN(CHK_BOOL(test99_err == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test99_last_error == WSAEOPNOTSUPP));
    return 0;
}

int test99_accept_load_last_error() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));

    /* the first failing accept also makes Winsock's one-time handles; the count is taken after
    it, and a second failing accept must leave it as it is */
    ASSERT_FN(test99_failing_accept());
    DWORD before = 0, after = 0;
    ASSERT_FN(CHK_BOOL(GetProcessHandleCount(GetCurrentProcess(), &before)));
    ASSERT_FN(test99_failing_accept());
    ASSERT_FN(CHK_BOOL(GetProcessHandleCount(GetCurrentProcess(), &after)));
    DBG("handles after the first failing accept %lu, after the second %lu", before, after);
    ASSERT_FN(CHK_BOOL(after == before));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test99_accept_load_last_error() {
    DBG("GetAcceptExSockaddrs is a Windows-only step: nothing to check here");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test99_accept_load_last_error();
    print_test_result("018-043-reproduced_accept_load_last_error.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

Workspace `repro/fix/39/`: `base/colib.h` is a copy of the current working-tree header (with the
uncommitted accept rework), `patched/colib.h` carries the hunk above, and `39-fix.diff` is
`diff -u` of the two. Built with the fix spec's command (`cl -nologo -EHsc -await:strict
-std:c++20 -Zi -FS -I.`), test include switched to `"colib.h"`.

1. The test on the unpatched copy fails (`run-018-047-base.log`):
   ```
   colib.h:5389 load_win_fn() :> Failed to load extension function: The attempted operation is not supported for the type of object referenced. [0x273d]
   018-047-reproduced_accept_load_last_error.cpp:  92 test88_accept_load_last_error() :> accept err -1, last error 0 (expected 10045)
   018-047-reproduced_accept_load_last_error.cpp:  95 test88_accept_load_last_error() :> Failed CHK_BOOL(test88_last_error == WSAEOPNOTSUPP) err_str: The operation completed successfully.
   [FAILED]: 018-047-reproduced_accept_load_last_error.cpp
   ```
2. The same test on the patched copy passes (`run-018-047-patched.log`):
   ```
   colib.h:5389 load_win_fn() :> Failed to load extension function: The attempted operation is not supported for the type of object referenced. [0x273d]
   018-047-reproduced_accept_load_last_error.cpp:  92 test88_accept_load_last_error() :> accept err -1, last error 10045 (expected 10045)
   [PASSED]: 018-047-reproduced_accept_load_last_error.cpp
   ```
3. The closest existing tests pass on the patched copy: `018-028-reproduced_accept_peer_address`
   and `005-001-io`.
4. Composed with #38 in `repro/fix/38/combined/`: both hunks applied to one copy, `018-046` and
   `018-047` both pass.

NOT-CHECKED: the non-Windows branch of the test was not compiled (no Linux toolchain here); it is
a two-line skip, and its `#if defined(_WIN32)` prelude is skipped there too. The hunk itself sits
inside `#if COLIB_OS_WINDOWS`.

At apply time, 2026-10-28, on the header of that day, in the applied shape (with fix #48):

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | only when the load fails under a correct call (the provider  |
|                             | lacking the extension, `WSAIoctl` failing); no input causes  |
|                             | it, the test stands in for `WSAIoctl`                        |
| DESIGN.md                   | its rules concern the killer; no wait and no kill path       |
|                             | changes                                                      |
| epoll / kqueue              | every hunk is in the Windows block; the Linux suite builds   |
|                             | and passes on the applied header; kqueue NOT-CHECKED (WIP)   |
| contract / docs             | the contract `co::accept` documents ("the reason is          |
|                             | GetLastError()"), now kept; no doc comment changed           |
| callers                     | the three closing branches of `co::accept` use the lambda;   |
|                             | its callers unchanged                                        |
| pending fixes               | #22 touches `co::accept`'s socket creation, #23 `ConnectEx`; |
|                             | neither the branches changed here                            |
| runs                        | the test fails before at the reason (`last error 0`), with   |
|                             | #48 alone reverted at the count (83 to 84), and passes after |
|                             | (2 of 2); Windows 101/101; Linux 100/100; MSVC ASan          |
|                             | `018-042` to `018-045`, `005-001`, `018-024`, `018-029`      |
|                             | clean                                                        |

## Interactions

- **#38** (the unchecked `SO_UPDATE_ACCEPT_CONTEXT` in the same function): a different branch, a
  few lines below. The two compose; the combined copy was built and both tests pass. Either order
  applies cleanly.
- **#11** (the Windows wrappers' early exits writing `*err`): covers the `load_win_fn` failures in
  `colib::AcceptEx` and the other wrappers, not this one in `co::accept`, which already writes
  `*err`. Its open question 1 records that `GetLastError()` survives those early exits because
  nothing closes in between; this branch is the one place where something does.
- **#22**, **#23**: touch the head of `co::accept` and the `colib::ConnectEx` tail. Independent.

## Open questions for the user

1. **The test's seam.** The test includes Winsock before `colib.h` and replaces `WSAIoctl` with a
   macro, which breaks the suite's rule that `colib.h` comes first. No public input reaches this
   branch, so the alternatives are this seam, or keeping the test out of the suite and recording
   the validation here only. Your call.
2. **A shared helper.** The same question as #38's open question 2: after both fixes, three
   branches of `co::accept` save the last error, close `client_sock` and restore it.
3. **Noted, not changed.** `load_win_fn` leaks its dummy socket when `WSAIoctl` fails: it returns
   `ERROR_GENERIC` before reaching its `closesocket(sock)`. The test above passes through that
   path once per run. That is a separate bug in a separate function; I can write it up as its own
   proposal if you want it.

Decided at apply time, 2026-10-28, on the user's word: (1) the test is in the suite, its
include-order exception stated in its first comment; (2) the lambda `close_keep_error`, local to
`co::accept`, replaces the three triples; (3) the leak is fix #48, applied with this one: its hunk
is in `load_win_fn`, its check in this fix's test.
