# Proposal: load_win_fn closes its dummy socket when WSAIoctl fails (fix #48)

Status: [APPLIED 2026-10-28 on the user's word]

Noted by fix #39's proposal (its open question 3) and written up at apply time, on the user's word
("make it not leak"). It shares fix #39's regression test.

## The bug

`load_win_fn` (the Windows block) loads a Winsock extension function by its GUID: it makes a dummy
TCP socket, asks `WSAIoctl` on it for the function's pointer, and closes the socket. When
`WSAIoctl` fails, the function logs and returns `ERROR_GENERIC` before reaching its `closesocket`:
the dummy socket is leaked, one per failed load. The trigger is any failed load: the provider
lacking the extension, or `WSAIoctl` failing for another reason. A load that failed is retried on
the next call of the wrapper that needs it, since the pointer stays null, so a program that keeps
calling such a wrapper leaks a socket per call. No input to a wrapper causes it; the test forces
it by standing in for `WSAIoctl`.

Confirmed consequence (2026-10-28, Windows 10 19045, MSVC 14.43): with fix #39 in and only this
fix's hunk reverted, two failing accepts in a row raised the process's handle count from 83 to 84
(`tests/018-043-reproduced_accept_load_last_error.cpp`'s check); with the fix, 76 to 76.

## Where the fix lands

| colib.h region             | what changes                                          | adds/dels |
|----------------------------|-------------------------------------------------------|-----------|
| `load_win_fn` (the Windows | the `WSAIoctl` failure branch closes the dummy socket | +3/-0     |
| block), the `WSAIoctl`     | before returning, keeping `WSAIoctl`'s error around   |           |
| failure branch             | the close                                             |           |

One hunk, pure additions. The function's contract (`ERROR_GENERIC`, the reason in
`GetLastError()`) does not change; `docs/02_api.md` has nothing to mirror.

## The change

`diff -u` of the header as it stood on 2026-10-28 right before this batch of fixes (#12, #38, #39,
#40 and #48, applied the same day on the user's word; #11 and `wait_msg()` earlier that day)
against the header as applied.

Reasoning: the close belongs on every exit after the socket was made. `closesocket` resets the
last error when it succeeds, so the error `WSAIoctl` left is saved around it and restored, as
`co::accept` does around its own closes (fix #39); without that, this fix would have taken away
the reason fix #39 keeps.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -5501,6 +5518,9 @@
                   &dwBytes, NULL, NULL);
     if (ret != 0) {
         COLIB_DEBUG("Failed to load extension function: %s", get_last_error().c_str());
+        DWORD last_error = GetLastError();  /* the load's reason, not the close's */
+        closesocket(sock);
+        SetLastError(last_error);
         return ERROR_GENERIC;
     }
 
```

## The regression test

Shared with fix #39: `tests/018-043-reproduced_accept_load_last_error.cpp` (Test99) runs the
failing accept twice, each run with its own listener, and requires the process's handle count
after the second run to equal the count after the first. The file is in full in
`FIX_39-accept_load_last_error.md`. With only this fix's hunk reverted (fix #39 in), it fails at
the count; on the header as applied it passes, two runs of two:

```
with the hunk reverted:
handles after the first failing accept 83, after the second 84
Failed CHK_BOOL(after == before) err_str: The operation completed successfully.
[FAILED]: 018-043-reproduced_accept_load_last_error.cpp

as applied:
handles after the first failing accept 76, after the second 76
[PASSED]: 018-043-reproduced_accept_load_last_error.cpp
```

## How it was checked

Checked at apply time, 2026-10-28, on the header of that day, with fix #39:

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | only when the load fails under a correct call (the provider  |
|                             | lacking the extension, `WSAIoctl` failing), then once per    |
|                             | call of the wrapper; the test stands in for `WSAIoctl`       |
| DESIGN.md                   | its rules concern the killer; nothing of it changes          |
| epoll / kqueue              | the hunk is in the Windows block; the Linux suite builds and |
|                             | passes on the applied header; kqueue NOT-CHECKED (WIP)       |
| contract / docs             | `load_win_fn` answers `ERROR_GENERIC` with the reason in     |
|                             | `GetLastError()`, as before; no doc changes                  |
| callers                     | the four extension loads of the wrappers and `co::accept`'s; |
|                             | each reads the answer and `GetLastError()` as before         |
| pending fixes               | none touches `load_win_fn`                                   |
| runs                        | the shared test fails at the count with this hunk alone      |
|                             | reverted (83 to 84) and passes as applied (76 to 76, 2 of    |
|                             | 2); Windows 101/101; Linux 100/100; MSVC ASan `018-043`      |
|                             | among the clean builds                                       |

## Interactions

- #39: `co::accept` restores the reason around its own close; this fix restores it around
  `load_win_fn`'s. Without this fix the reason survived by accident, since the leaked socket was
  never closed; with the close and without the restore, #39's check would fail. The two compose,
  and the shared test checks both.
- #11: the extension-load early exits of the wrappers answer `ERROR_GENERIC` with the reason in
  `GetLastError()`; this fix keeps that reason intact when the load's own cleanup runs. Compose.

## Open questions for the user

None. The user's word was "make it not leak"; the save and restore of the error around the close
follows the file's rule for closes after a failure.
