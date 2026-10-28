# Proposal: io_data_t's event gets one owner by type, and a failed close is logged (fix #40)

Status: [APPLIED 2026-10-28 on the user's word]

Apply verification, 2026-10-28: the test fails on the header before the fix by both checks
(copyable, no log line) and passes on the header as applied, two runs of two; the whole suite on
Windows `[PASSED: 101]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll)
`[PASSED: 100]/[FAILED: 0]`, every file but the Windows-only `018-029`; MSVC ASan builds of
`018-042` to `018-045`, `005-001`, `018-024` and `018-029` exit 0 with no report. The check
table is the last item of "How it was checked".

## The bug

Fix #06 gave `io_data_t` (the Windows backend's per-request state) a destructor that closes
`overlapped.hEvent` when the field is set. Two things around that destructor are left open.

**The struct can still be copied, and its moves are copies.** `io_data_t` declares a destructor and
no copy or move operations. A user-declared destructor keeps the implicit copy constructor and copy
assignment, and suppresses the implicit move constructor and move assignment, so a "move" of an
`io_data_t` falls back to the copy. A copy duplicates the `HANDLE` inside `overlapped`, and both
copies then close it when they die: a double close. The second close either fails, or closes an
unrelated handle that has reused the value in between. Nothing in colib does this today. The only
whole-struct writes are `*new_desc.data = io_data_t {...}` in `create_io_desc` and
`*new_timer.data = io_data_t {...}` in the timer pool. Both are copy assignments (the move is
suppressed), but both copy a temporary whose `hEvent` is still NULL. The risk is a later edit, such
as passing an `io_data_t` by value or keeping one in a container, that compiles silently and
double-closes at run time.

**A failed close in the destructor leaves no trace.** The destructor discards `CloseHandle`'s
result. The success path's close in `handle_done_req` and the backend's `close_handle` both log a
failed close with `COLIB_DEBUG(... get_last_error() ...)`. The destructor runs on the killed-waiter
path that fix #06 was written for, so a close failing there leaves no trace in the log.

The confirmed consequences, both on this box (Windows 10 19045, MSVC 14.43), are in the regression
test's run on the current header below. `io_data_t` reports `is_copy_constructible` and
`is_copy_assignable` as true. After `dst = std::move(src)`, both structures hold the same event
handle. A destructor that fails to close an invalid handle writes nothing to `log_str`.

Found by the cloud review of 10-10-2026, on the `review-colib` branch at `64dc666`. The review
proposed deleting the copy operations of `io_data_t` and calling `get_last_error()` in the
destructor. Neither compiles as written:

- Deleting the copy constructor is a user-declared constructor, so `io_data_t` stops being an
  aggregate (C++20), and the designated initializers in `create_io_desc` and the timer pool break.
- Deleting only the copy assignment breaks those same two assignments, since they are copy
  assignments.
- `get_last_error()` is defined after `io_data_t`, so the in-class destructor cannot call it.

The shape below avoids all three.

## Where the fix lands

As applied on 2026-10-28, in the shape the user chose over the proposal's (open question 1): no
new type. `io_data_t` itself forbids its copies, logs a failed close in its own in-class destructor, and
is filled field by field where it is built, since `alloc<io_data_t>` constructs it in place.
There is no move either: nothing in colib moves one.

| colib.h region                  | what changes                                         | adds/dels |
|---------------------------------|------------------------------------------------------|-----------|
| `io_data_t` (inside             | the default constructor is kept, the copy            | in the    |
| `#if COLIB_OS_WINDOWS`)         | constructor and assignment are deleted; the          | shared    |
|                                 | destructor logs a failed close, with the raw Windows | hunk,     |
|                                 | code                                                 | below     |
| right above `io_data_t`         | `dbg`'s declaration, a second one, so the in-class   | +5/-0     |
|                                 | destructor's log line compiles                       |           |
| `timer_pool_t::get_timer()`     | fills the io_data_t `alloc` built instead of         | +4/-7     |
|                                 | assigning a temporary into it                        |           |
| `create_io_desc()`              | drops the assignment of a temporary whose every      | +1/-7     |
|                                 | field was the default                                |           |

The `io_data_t` hunk is shared with fix #12, whose lines in it are the four of the `io_request`
doc. Every hunk lies inside `#if COLIB_OS_WINDOWS`, so the epoll and kqueue backends compile the
same bytes as before.

## The change

`diff -u` of the header as it stood on 2026-10-28 right before this batch of fixes (#12, #38, #39,
#40 and #48, applied the same day on the user's word; #11 and `wait_msg()` earlier that day)
against the header as applied, the destructor's placement revised on 2026-10-29 at the user's
request.

Reasoning per hunk:

- `dbg`'s declaration above the struct: `COLIB_DEBUG` expands to a call of `dbg`, which is
  declared with the debug interface 1,200 lines further down; without this second declaration
  the in-class log line does not compile (`C3861`). A second declaration of the same template is
  legal and changes nothing else.
- `io_data_t`: a deleted copy makes two owners of one event impossible to write; with the
  destructor user-declared, no move is generated either, and none is needed. The destructor stays
  in the struct, as the user asked, so reading it needs no trip down the file, and logs the raw
  Windows code: `get_last_error()`, which formats it, is defined 2,500 lines further down.
- `get_timer()` and `create_io_desc()`: both assigned a temporary `io_data_t` into the one
  `alloc` had just constructed, which a deleted copy refuses; `alloc` builds it in place with its
  defaults, so the timer sets the three fields it needs and the descriptor sets nothing.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -1094,6 +1094,11 @@
 #endif /* COLIB_OS_LINUX */
 #if COLIB_OS_WINDOWS
 
+/* declared here for the destructor below; the debug interface is declared with the others
+further down */
+template <typename... Args>
+inline void dbg(const char *file, const char *func, int line, const char *fmt, Args&&... args);
+
 /*! Holds the state of the I/O operation */
 struct io_data_t {
     enum io_flag_e : int32_t {
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
 
@@ -4031,14 +4058,11 @@
             new_timer.h = nullptr;
             return ERROR_GENERIC;
         }
-        
-        *new_timer.data = io_data_t {
-            .flags = io_data_t::IO_FLAG_TIMER,
-            .state = nullptr,
-            .io_request = [](void *) -> error_e { return ERROR_OK; },
-            .ptr = nullptr,
-            .h = new_timer.h
-        };
+
+        /* built in place by alloc, with its defaults; a timer's request asks nothing of the OS */
+        new_timer.data->flags = io_data_t::IO_FLAG_TIMER;
+        new_timer.data->io_request = [](void *) -> error_e { return ERROR_OK; };
+        new_timer.data->h = new_timer.h;
 
         return ERROR_OK;
     }
@@ -5466,19 +5490,13 @@
 /* A new request's descriptor, for building an overlapped request on io_awaiter_t (as the wrappers
 below do). Its io_data_t is allocated from the pool: the descriptor must not outlive the pool. */
 inline io_desc_t create_io_desc(pool_t *pool) {
+    /* alloc builds the io_data_t in place with its defaults: no flags, no state, no handle */
     io_desc_t new_desc = io_desc_t {
         .data = std::shared_ptr<io_data_t>(alloc<io_data_t>(pool),
                 dealloc_create<io_data_t>(pool), allocator_t<int>{pool}),
         .h = NULL
     };
 
-    *new_desc.data = io_data_t {
-        .flags = io_data_t::IO_FLAG_NONE,
-        .state = nullptr,
-        .ptr = nullptr,
-        .h = new_desc.h
-    };
-
     return new_desc;
 }
 
```

## The regression test

Landed as `tests/018-044-reproduced_io_data_event_ownership.cpp` (Test100); it was first written
as 018-048 / Test89. The proposal's move check went with the move: the applied shape has none.

It checks that `io_data_t` is neither copy-constructible nor copy-assignable, and that destroying
one whose event is an invalid handle (`0x7ffffff0`, above any handle a process can hold) leaves
"Failed to close event" in the log, captured through `co::log_str`. Under a debugger that close
raises `STATUS_INVALID_HANDLE` first and stops there once (open question 2, decided by the user:
the check stays).

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
Failed CHK_BOOL(!std::is_copy_constructible_v<co::io_data_t>) err_str: The operation completed successfully.
Failed CHK_BOOL(captured.find("Failed to close event") != std::string::npos) err_str: The handle is invalid.
[FAILED]: 018-044-reproduced_io_data_event_ownership.cpp
```

On the header as applied, two runs of two:

```
copy-constructible 0, copy-assignable 0
captured log: [[...] ~io_data_t() :> Failed to close event: [0x6]]
[PASSED]: 018-044-reproduced_io_data_event_ownership.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <string>
#include <type_traits>

/* Test100 - Reproduced Bugs: io_data_t's event had one owner by convention, not by the type
================================================================================================= */

/* io_data_t closes overlapped.hEvent when it dies. Its copies were still allowed (and, with its
destructor declared, a move was a copy), so two io_data_t could end up closing the same handle;
and a close that failed in that destructor left no trace in the log. io_data_t is now built in
place and cannot be copied; there is no move either. The test checks that it cannot be copied and
that a failed close is logged. Found by review 10-10-2026; fix #40
(tests/fixes/FIX_40-io_data_event_ownership.md). */

#if COLIB_OS_WINDOWS

int test100_not_copyable() {
    DBG("copy-constructible %d, copy-assignable %d",
            (int)std::is_copy_constructible_v<co::io_data_t>,
            (int)std::is_copy_assignable_v<co::io_data_t>);
    ASSERT_FN(CHK_BOOL(!std::is_copy_constructible_v<co::io_data_t>));
    ASSERT_FN(CHK_BOOL(!std::is_copy_assignable_v<co::io_data_t>));
    return 0;
}

int test100_failed_close_logged() {
    /* no process holds a handle this high: the kernel's table stops at 2^24 entries of 4. Outside
    a debugger CloseHandle fails with ERROR_INVALID_HANDLE; under one, Windows raises
    STATUS_INVALID_HANDLE first and the debugger stops here once. */
    HANDLE bogus = (HANDLE)(uintptr_t)0x7ffffff0;
    std::string captured;
    auto old_log = co::log_str;
    co::log_str = [&captured](const co::dbg_string_t& s) {
        captured += s.c_str();
        return 0;
    };
    {
        co::io_data_t data;
        data.overlapped.hEvent = bogus;
    }
    co::log_str = old_log;

    DBG("captured log: [%s]", captured.c_str());
    ASSERT_FN(CHK_BOOL(captured.find("Failed to close event") != std::string::npos));
    return 0;
}

int test100_io_data_event_ownership() {
    int ret = 0;
    if (test100_not_copyable() < 0)
        ret = -1;
    if (test100_failed_close_logged() < 0)
        ret = -1;
    return ret;
}

#else /* COLIB_OS_WINDOWS */

int test100_io_data_event_ownership() {
    DBG("io_data_t's event is a Windows-only member: nothing to check here");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test100_io_data_event_ownership();
    print_test_result("018-044-reproduced_io_data_event_ownership.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

The workspace is `repro/fix/40/`:

- `base/colib.h` is a copy of the current working-tree header. That copy carries fix #06 applied
  and uncommitted, plus the restored `<optional>`.
- `patched/colib.h` carries the three hunks above.
- `40-fix.diff` is `diff -u` of the two copies.

Each build used the fix spec's command (`cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS
-I<base|patched>`), with the test's include switched to `"colib.h"`.

1. The test fails on the unpatched copy, on all three parts (`run-base.log`). It builds cleanly,
   since part 1 checks the traits at run time:
   ```
   test89_not_copyable() :> copy-constructible 1, copy-assignable 1
   test89_not_copyable() :> Failed CHK_BOOL(!std::is_copy_constructible_v<co::io_data_t>) ...
   test89_move_hands_over() :> after the move: src 00000000000000A4, dst 00000000000000A4 (events were src 00000000000000A4, dst 00000000000000A8)
   test89_move_hands_over() :> Failed CHK_BOOL(!shared) ...
   test89_failed_close_logged() :> captured log: []
   test89_failed_close_logged() :> Failed CHK_BOOL(captured.find("Failed to close event") != std::string::npos) err_str: The handle is invalid.
   [FAILED]: 018-048-reproduced_io_data_event_ownership.cpp
   ```
2. The same test passes on the patched copy (`run-patched.log`):
   ```
   test89_not_copyable() :> copy-constructible 0, copy-assignable 0
   test89_move_hands_over() :> after the move: src 000000000000009C, dst 0000000000000098 (events were src 0000000000000098, dst 000000000000009C)
   test89_failed_close_logged() :> captured log: [... colib.h:3544 ~owned_overlapped_t() :> Failed to close event: The handle is invalid. [0x6]]
   [PASSED]: 018-048-reproduced_io_data_event_ownership.cpp
   ```
3. The closest existing tests pass on the patched copy (`regress/`). These are all Windows io tests
   that build their requests through `create_io_desc`, and all compile against the new member type:
   - `018-029-reproduced_winevent_handle_leak`, fix #06's own test. It still shows no leak:
     `kill: handles 59 -> 59 (growth 0), timed-out reads 300/300`.
   - `005-001-io`
   - `018-024-reproduced_iocp_failed_request_reads_ok`
   - `018-026-reproduced_iocp_offset_not_advanced`
   - `018-028-reproduced_accept_peer_address`

NOT-CHECKED: the test's non-Windows branch was not compiled, because there is no Linux toolchain
here. It is a two-line skip, and every hunk sits inside `#if COLIB_OS_WINDOWS`.

At apply time, 2026-10-28, on the header of that day, in the applied shape (no new type):

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | the copy: nothing in colib copies an `io_data_t` today, a    |
|                             | later edit would; the silent close: a killed waiter whose    |
|                             | event close fails, handle-table games only                   |
| DESIGN.md                   | its rules concern the killer; no wait and no kill path       |
|                             | changes                                                      |
| epoll / kqueue              | every hunk is in the Windows block, the struct itself is     |
|                             | Windows-only; the Linux suite builds and passes on the       |
|                             | applied header; kqueue NOT-CHECKED (WIP)                     |
| contract / docs             | `io_data_t`'s doc comments changed and are mirrored in       |
|                             | `02_api.md`'s block (brought up to the header: `win_err`,    |
|                             | `IO_FLAG_MSG`, the deleted copy, the destructor)             |
| callers                     | every place that builds one: `create_io_desc()` and          |
|                             | `timer_pool_t::get_timer()` fill it in place; `wait_msg()`   |
|                             | already set fields; no other construction or copy (grep)     |
| pending fixes               | none touches `io_data_t`; #06 (applied) gave it the close    |
|                             | this fix checks                                              |
| runs                        | the test fails before by both checks (copyable, no log line) |
|                             | and passes after (2 of 2); Windows 101/101; Linux 100/100;   |
|                             | MSVC ASan `018-042` to `018-045`, `005-001`, `018-024`,      |
|                             | `018-029` clean                                              |

## Interactions

- **#06** (applied 2026-09-29, still uncommitted in the working tree) is the change this one
  reshapes. It keeps #06's behavior, the close on every exit and `handle_done_req` clearing the
  field, and moves the destructor into the member type. It requires #06 to be in the tree first,
  which it is.
- **#03, #12, #25** touch the Windows `io_pool_t` (`CancelIoEx`/`GetOverlappedResult` on
  `&data->overlapped`, flag updates). Their added lines use the member only through its address
  or inherited fields, which the derived type keeps, so they compose in either order. I checked
  this by reading their diffs; they were not built together with this one.
- **#11** touches the wrappers' early exits. Those take `&desc.data->overlapped` into a
  `std::tuple{...}`, whose deduced element is now `owned_overlapped_t*`. `std::apply` converts it to
  `LPOVERLAPPED` at the call, as `005-001-io` and `018-028` exercise. The two fixes compose.

## Open questions for the user

1. **Name and place.** `owned_overlapped_t` sits at namespace level next to `io_data_t`. It could
   instead nest inside `io_data_t`, since nothing else uses it. Your call on the name and the
   place.
2. **The test's invalid handle.** Part 3 closes `0x7ffffff0`, a value above any handle a process
   can hold. Outside a debugger, `CloseHandle` returns FALSE with `ERROR_INVALID_HANDLE`. Under a
   debugger, Windows raises `STATUS_INVALID_HANDLE` first, so a debugger session will stop there
   once. The alternative is to drop part 3 and keep the logging checked only by reading.
3. **BUGS.md.** Nothing in colib fires this today, so I added no `BUGS.md` entry and no failing
   `018-N` test ahead of the fix. Say if you want it tracked there anyway.
4. **Noted, not changed.** `handle_done_req`'s failure branch also closes the event without
   checking the result (`CloseHandle(data->overlapped.hEvent);` before its `COLIB_DEBUG("FAILED:
   ...")`). The cloud review missed it when it said every other close logs. It belongs to a
   different function, so I can write it up separately if you want it.

Decided at apply time, 2026-10-28, on the user's word: (1) no new type: `io_data_t` itself
deletes its copy, is filled in place where `alloc` builds it, and logs in its own destructor; no
move exists; (2) the invalid-handle close stays in the test; (3) no `BUGS.md` entry; (4)
`handle_done_req()`'s unchecked close is left as it is.
