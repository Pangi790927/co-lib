# IOCP: a request's own status reaches the caller

Status: **applied to colib.h** on 2026-09-25, as asked. The diff is against colib.h just before it.

## The bug (`BUGS.md` #16, `018-024`)

The Windows `io_pool_t::handle_ready_events()` sets `err = ERROR_OK` for every completion packet it
takes. `GetQueuedCompletionStatusEx` returns `TRUE` even when the packets it hands back are failed
requests: how a request ended is only in its own `OVERLAPPED` (`Internal`, an NTSTATUS), and colib
never read it. `handle_done_req()` trusts the `ERROR_OK`, so a request that failed comes back as a
success with 0 bytes. For `co::read()` on a socket, a connection reset reads as 0, a clean end of
stream, so a reader that reads until 0 takes a cut-off transfer as complete. Plain Win32 on the same
setup: `GetOverlappedResult` fails with error 64 (`ERROR_NETNAME_DELETED`; `Internal` is
`0xC000020D`, connection reset). On epoll the `read()` syscall itself reports the reset.

## What changes

The user of colib's Win32-like wrappers sees what real Win32 gives: a failed `co::ReadFile()`
returns `FALSE`, and `GetLastError()` right after it is the request's own error (64 for the reset).

1. **Success or failure, from the `OVERLAPPED`:** `handle_ready_events()` reads the sign of
   `data->overlapped.Internal`, where the kernel left the request's NTSTATUS (negative: it failed).
   No call on this path. colib's own timer packets never get `Internal` written, it stays 0.
2. **`io_data_t::win_err`**, the request's own Win32 error:
   - a request refused on the spot (`::ReadFile` not pending) never reaches the queue: its error is
     `GetLastError()` right there, in `add_waiter()`;
   - a completion that failed: `handle_done_req()` gets it with `GetOverlappedResult(..., FALSE)`
     on the finished request, which turns the NTSTATUS into the Win32 code the way Win32 itself
     does, without waiting. Only failed requests pay for that call.
3. **`handle_done_req()` leaves that error as the last error**, as its last action before the
   wrapper returns `FALSE`.
4. **`ReadFile()` can also hand the code out** (a new last parameter, `DWORD *error = nullptr`).
   `co::read()` uses it rather than `GetLastError()`: between the wrapper's end and its caller's
   resume, EXIT/LEAVE/ENTER modif callbacks run, and any Win32 call in them can overwrite the last
   error.
5. **`co::read()` decides what the end of a stream is:** `ERROR_HANDLE_EOF` (a file's end) and
   `ERROR_BROKEN_PIPE` (a pipe whose writer closed) return 0; anything else that failed, a reset
   included, returns `ERROR_GENERIC`.

One kind of code on the user-facing side (Win32), no mapping table, no NTSTATUS constants.

## The diff

```diff
--- a/colib.h
+++ b/colib.h
@@ -1070,6 +1070,8 @@
                                                          of the I/O operation */
     state_t *state = nullptr;                       /*!< state of the task */
     DWORD recvlen = 0;                              /*!< the byte transfer count */
+    DWORD win_err = ERROR_SUCCESS;                  /*!< the request's own Windows error, if it
+                                                         failed */
 
     std::function<error_e(void *)> io_request;      /*!< function to be called inside add_waiter,
                                                          for example: the ReadFile request */
@@ -1920,7 +1922,8 @@
                            LPVOID   lpBuffer,
                            DWORD    nNumberOfBytesToRead,
                            LPDWORD  lpNumberOfBytesRead,
-                           uint64_t *offset);
+                           uint64_t *offset,
+                           DWORD    *error = nullptr);
 
 /*! @fn
  * This function is calling it's WinAPI (or extension) counterpart, but in coroutine context and
@@ -3547,7 +3550,10 @@
                 COLIB_DEBUG_TRACE("   Pointer:      %p", ovlpd->Pointer);
                 io_data_t *data = (io_data_t *)ovlpd;
                 if (data != to_filter) {
-                    data->state->err = ERROR_OK;
+                    /* how the request ended, as the kernel left it in its OVERLAPPED (an NTSTATUS,
+                    negative if it failed); colib's own timer packets leave it 0 */
+                    data->state->err = (LONG)data->overlapped.Internal < 0 ?
+                            ERROR_GENERIC : ERROR_OK;
                     data->recvlen = entry.dwNumberOfBytesTransferred;
 
                     awake_io(data);
@@ -3616,6 +3622,7 @@
 
         error_e err = data->io_request(data->ptr);
         if (err != ERROR_OK) {
+            data->win_err = GetLastError();
             COLIB_DEBUG("Failed the io_request: %s", get_last_error().c_str());
             return err;
         }
@@ -5287,8 +5294,16 @@
 
 inline error_e handle_done_req(io_data_t *data, error_e err, DWORD *len, uint64_t *offset) {
     if (err != ERROR_OK) {
-        COLIB_DEBUG("FAILED: %s", get_last_error().c_str());
+        /* a completion that failed: its Win32 error, the way Win32 itself gets it (no wait) */
+        if (data->win_err == ERROR_SUCCESS && (LONG)data->overlapped.Internal < 0) {
+            DWORD transferred = 0;
+            if (!GetOverlappedResult(data->h, &data->overlapped, &transferred, FALSE))
+                data->win_err = GetLastError();
+        }
         CloseHandle(data->overlapped.hEvent);
+        COLIB_DEBUG("FAILED: error %lu", data->win_err);
+        if (data->win_err != ERROR_SUCCESS)
+            SetLastError(data->win_err);    /* last, as the Win32 counterpart leaves it */
         return ERROR_GENERIC;
     }
     if (len)
@@ -5513,7 +5528,7 @@
 }
 
 inline task<BOOL> ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
-        LPDWORD lpNumberOfBytesRead, uint64_t *offset)
+        LPDWORD lpNumberOfBytesRead, uint64_t *offset, DWORD *error)
 {
     auto desc = create_io_desc(co_await get_pool());
 
@@ -5541,6 +5556,8 @@
     desc.data->ptr = (void *)&params;
     error_e ret = co_await io_awaiter_t(desc);
     if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesRead, offset) != ERROR_OK) {
+        if (error)
+            *error = desc.data->win_err;
         COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
         co_return false;
     }
@@ -5885,8 +5902,12 @@
 
 inline task<SSIZE_T> read(HANDLE h, void *buff, size_t len, uint64_t *offset) {
     DWORD nread = 0;
-    BOOL ok = co_await COLIB_REGNAME(ReadFile(h, buff, (DWORD)len, &nread, offset));
+    DWORD error = ERROR_SUCCESS;
+    BOOL ok = co_await COLIB_REGNAME(ReadFile(h, buff, (DWORD)len, &nread, offset, &error));
     if (!ok) {
+        /* the end of a file, or a pipe whose writer closed: the end of the stream, not an error */
+        if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
+            co_return 0;
         COLIB_DEBUG("Failed read");
         co_return ERROR_GENERIC;
     }
```

## Checked on the scratch copy (MSVC, ASan)

| | Today | With the diff |
|---|---|---|
| `018-024`: a pending read, the peer resets | 0 (reads as the end) | **an error** (passes) |
| a file read at its end | 0 | 0 |
| a named pipe whose writer closed | **-1** (an error) | **0** |
| `018-015`, `018-023`, `005-001` .. `005-005`, `003-001`, `003-003`, `004-001`, `006-001` | pass | pass, ASan clean |

## Known effects and open points

- **A pipe whose writer closed** returned -1 before (the immediate `::ReadFile` failure turned into
  an error) and returns 0 with the diff, like Linux's `read()` on a pipe with no writer. A second
  behaviour change, not only the fix.
- **A stopped request** (`stop_io()`, a kill): its cancel leaves `STATUS_CANCELLED` in `Internal`,
  so the wrapper now fails with `ERROR_OPERATION_ABORTED` as the last error, as a cancelled Win32
  request does.
- **The last error after a wrapper** is the request's own unless something between the wrapper's
  `SetLastError()` and the caller's `GetLastError()` overwrites it. The wrappers' own
  `COLIB_DEBUG("FAILED request: ...")` line did, with logging on: it ran right after, so all 16 were
  removed (`handle_done_req()` already logs the request's own code). What's left in between is
  colib's coroutine machinery and a user's modif callbacks. `co::read()` doesn't depend on it.
- **The other wrappers** (`WriteFile`, `WSARecv`, `ConnectEx`, ...) get the same last error through
  `handle_done_req()`, but no `error` parameter: only `co::read()` needs one, for its end-of-stream
  cases.
- **Tests to add with it:** the file-end and the pipe cases, so the end-of-stream mapping stays
  pinned.
