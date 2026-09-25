# The Windows wrappers, closer to the sync Win32 calls

Status: **applied to colib.h** on 2026-09-25 (after `iocp_request_status.md`). The diff is against
colib.h just before it.

colib's `ReadFile()`/`WriteFile()`/... take the place of the sync Win32 calls in coroutine code, so
they should behave like them wherever the overlapped machinery underneath allows. Three places
where they didn't:

## 1. The offset (`BUGS.md` #18, `018-026`)

Overlapped handles have no file pointer: the wrappers read or write at `*offset` and write it back
when done, but the one the request started at. A loop reading a file in chunks through one offset
variable read the first chunk forever; a loop writing overwrote its first chunk (the test's two
writes left only "1234"). `handle_done_req()` now moves it past the bytes transferred, for every
wrapper. `LockFileEx` also passes an offset (the start of the locked range): it transfers nothing,
`recvlen` is 0, so it doesn't move.

## 2. The byte count on a failure

The sync calls set it to 0 on a failure; the wrappers only wrote it on success, so after a failure
the caller's variable kept whatever it held. `handle_done_req()` now zeroes it on the failure path,
for every wrapper.

## 3. The end of a file

An overlapped read at a file's end fails with `ERROR_HANDLE_EOF`; the sync `::ReadFile` returns
`TRUE` with 0 bytes. `ReadFile()` now does what the sync one does, so `co::read()` only keeps its
pipe case (the sync `::ReadFile` fails with `ERROR_BROKEN_PIPE` on a pipe whose writer closed too, and
`co::read()` reads that as the end of the stream).

## The diff

```diff
--- a/colib.h
+++ b/colib.h
@@ -5294,6 +5294,8 @@
 
 inline error_e handle_done_req(io_data_t *data, error_e err, DWORD *len, uint64_t *offset) {
     if (err != ERROR_OK) {
+        if (len)
+            *len = 0;   /* as the sync Win32 calls leave it on a failure */
         /* a completion that failed: its Win32 error, the way Win32 itself gets it (no wait) */
         if (data->win_err == ERROR_SUCCESS && (LONG)data->overlapped.Internal < 0) {
             DWORD transferred = 0;
@@ -5316,6 +5318,7 @@
         *offset = 0;
         *offset |= data->overlapped.Offset;
         *offset |= (uint64_t(data->overlapped.OffsetHigh) << 32);
+        *offset += data->recvlen;   /* past what was transferred, like a file pointer */
     }
     return ERROR_OK;
 }
@@ -5550,6 +5553,8 @@
     desc.data->ptr = (void *)&params;
     error_e ret = co_await io_awaiter_t(desc);
     if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesRead, offset) != ERROR_OK) {
+        if (desc.data->win_err == ERROR_HANDLE_EOF)
+            co_return true;     /* the end of a file: 0 bytes, as the sync ::ReadFile */
         if (error)
             *error = desc.data->win_err;
         co_return false;
@@ -5889,8 +5894,8 @@
     DWORD error = ERROR_SUCCESS;
     BOOL ok = co_await COLIB_REGNAME(ReadFile(h, buff, (DWORD)len, &nread, offset, &error));
     if (!ok) {
-        /* the end of a file, or a pipe whose writer closed: the end of the stream, not an error */
-        if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
+        /* a pipe whose writer closed: the end of the stream, not an error */
+        if (error == ERROR_BROKEN_PIPE)
             co_return 0;
         COLIB_DEBUG("Failed read");
         co_return ERROR_GENERIC;
```

## Checked on the scratch copy

| | Result (MSVC, ASan) |
|---|---|
| `018-026` | passes: "abcd" then "efgh", offset 8; the file holds "wxyz1234", offset 8 |
| a file read at its end, a pipe whose writer closed, through `co::read()` | 0 and 0 |
| `018-024`, `018-015`, `018-023`, `005-001` .. `005-005` | pass, ASan clean |
| `co::ReadFile()` at a file's end, next to the sync `::ReadFile` | both `TRUE`, 0 bytes (after applying) |

## Not covered

- **`WriteFile()` at a position past the end, or on a full disk:** no sync difference known, not
  checked.
- **Handles not opened for overlapped I/O:** the user's responsibility, nothing changes.
