# Fix #50: the completion-routine parameter of the Windows wrappers goes

Status: [APPLIED 2026-10-29 on the user's word]

Found on 2026-10-29 while auditing the Windows wrappers at the user's request, during the apply
of fix #23; applied in the same batch on his word ("do a fix for them"). No proposal preceded
it, so this record was written at apply time.

Apply verification, 2026-10-29: the whole suite compiles and passes with the parameter gone
(the two callers in the tests updated with it): Windows `[PASSED: 109]/[FAILED: 0]`; Linux (WSL Ubuntu, g++ 15.2,
epoll) `[PASSED: 108]/[FAILED: 0]`, every file but the Windows-only `018-029`. The check table is the last item of
"How it was checked".

## The bug

Seven wrappers (`ReadDirectoryChangesW`, `WSASend`, `WSASendTo`, `WSASendMsg`, `WSARecv`,
`WSARecvFrom`, `WSARecvMsg`) took an `lpCompletionRoutine` parameter, documented "Same as the
Windows call", and passed it through to the call together with the engine's OVERLAPPED. With a
routine, Windows delivers the completion to that routine (an APC) and not to the completion
port; the wrapper waits on the port, so its await never ended. Measured on the header before
the fix, a `co::WSARecv` with a routine on a loopback socket with a byte already sent to it:

```
WATCHDOG: the receiver never completed (routine calls so far: 0)
```

The same call with NULL completed (through fix #49's path). The parameter could only ever be
NULL, which the doc did not say; the wrappers already leave out the OVERLAPPED for the same
reason, the completion mechanism belongs to the engine.

## Where the fix lands

| colib.h region                            | what changes                                      | adds/dels |
|-------------------------------------------|---------------------------------------------------|-----------|
| the seven wrappers' doc comments          | the `@param lpCompletionRoutine` line goes        | +0/-7     |
| the seven declarations and definitions    | the parameter goes                                | +0/-13    |
| the seven `params` tuples                 | the Windows call gets `nullptr` for the routine   | +7/-7     |
| the wrappers' block comment (twice: above | states that the completion routine is left out    | +10/-6    |
| the declarations, above the definitions)  | too, and why                                      |           |

The callers in the tests follow: `005-001` (`ReadDirectoryChangesW`) and `018-041`
(`WSASendMsg`, `WSARecvMsg`) drop their NULL argument. `docs/02_api.md`'s mirror of the seven
declarations and their parameter lists drops the parameter too.

## The change

`diff -u` of the header right before the batch of 2026-10-29 (fixes #23, #49, #50) against the
header as applied, this fix's hunks only.

Reasoning: a parameter that must be NULL is a trap, and the user's rule for these wrappers is
that a Windows user gets Windows behavior; the one thing the wrappers already take away from the
Windows call is the completion mechanism (the OVERLAPPED), and the routine is part of that
mechanism, so it goes the same way, stated in the same comment. The alternative, keeping the
parameter and documenting it NULL-only, was the other shape; the user left the choice to me and
this is the one I took, easy to flip.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -1883,7 +1883,9 @@
 
 /* Those functions are the same as their Windows API equivalent, the difference is that they don't
-expose the overlapped structure, which is used by the coro library. They require a handle that
-is compatible with iocp and they will attach the handle to the iocp instance. Those are the
-functions listed by msdn to work with iocp (and connect, that is part of an extension) */
+expose the overlapped structure, which is used by the coro library, nor a completion routine: the
+completion comes through the port, which is the engine's (a routine would take it away from the
+port, and the await would never end). They require a handle that is compatible with iocp and
+they will attach the handle to the iocp instance. Those are the functions listed by msdn to work
+with iocp (and connect, that is part of an extension). 2026-10-29 */
 
 /*! @fn
@@ -2030,5 +2032,4 @@
  * @param dwNotifyFilter Same as ::ReadDirectoryChangesW
  * @param lpBytesReturned Same as ::ReadDirectoryChangesW when the call is blocking
- * @param lpCompletionRoutine Same as ::ReadDirectoryChangesW
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2044,5 +2045,4 @@
                                         DWORD                               dwNotifyFilter,
                                         LPDWORD                             lpBytesReturned,
-                                        LPOVERLAPPED_COMPLETION_ROUTINE     lpCompletionRoutine,
                                         error_e                             *err = nullptr);
 
@@ -2154,5 +2154,4 @@
  * @param dwFlags Same as ::WSASendMsg
  * @param lpNumberOfBytesSent Same as ::WSASendMsg when the call is blocking
- * @param lpCompletionRoutine Same as ::WSASendMsg
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2166,5 +2165,4 @@
                             DWORD                               dwFlags,
                             LPDWORD                             lpNumberOfBytesSent,
-                            LPWSAOVERLAPPED_COMPLETION_ROUTINE  lpCompletionRoutine,
                             error_e                             *err = nullptr);
 
@@ -2182,5 +2180,4 @@
  * @param lpTo Same as ::WSASendTo
  * @param iTolen Same as ::WSASendTo
- * @param lpCompletionRoutine Same as ::WSASendTo
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2197,5 +2194,4 @@
                             const sockaddr                     *lpTo,
                             int                                iTolen,
-                            LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
                             error_e                            *err = nullptr);
 
@@ -2211,5 +2207,4 @@
  * @param lpNumberOfBytesSent Same as ::WSASend when the call is blocking
  * @param dwFlags Same as ::WSASend
- * @param lpCompletionRoutine Same as ::WSASend
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2224,5 +2219,4 @@
                           LPDWORD                            lpNumberOfBytesSent,
                           DWORD                              dwFlags,
-                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
                           error_e                            *err = nullptr);
 
@@ -2240,5 +2234,4 @@
  * @param lpFrom Same as ::WSARecvFrom
  * @param lpFromlen Same as ::WSARecvFrom
- * @param lpCompletionRoutine Same as ::WSARecvFrom
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2255,5 +2248,4 @@
                               sockaddr                           *lpFrom,
                               LPINT                              lpFromlen,
-                              LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
                               error_e                            *err = nullptr);
 
@@ -2267,5 +2259,4 @@
  * @param lpMsg Same as ::WSARecvMsg
  * @param lpdwNumberOfBytesRecvd Same as ::WSARecvMsg when the call is blocking
- * @param lpCompletionRoutine Same as ::WSARecvMsg
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2278,5 +2269,4 @@
                              LPWSAMSG                           lpMsg,
                              LPDWORD                            lpdwNumberOfBytesRecvd,
-                             LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
                              error_e                            *err = nullptr);
 
@@ -2292,5 +2282,4 @@
  * @param lpNumberOfBytesRecvd - Same as ::WSARecv when the call is blocking
  * @param lpFlags - Same as ::WSARecv
- * @param lpCompletionRoutine - Same as ::WSARecv
  * @param err Optional: the wait's own result, the stopper's error when the request was
  * stopped (ERROR_WAKEUP from the stop_io family), ERROR_GENERIC when it failed (the reason
@@ -2305,5 +2294,4 @@
                           LPDWORD                            lpNumberOfBytesRecvd,
                           LPDWORD                            lpFlags,
-                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
                           error_e                            *err = nullptr);
 
@@ -5607,7 +5598,9 @@
 
 /* Those functions are the same as their Windows API equivalent, the difference is that they don't
-expose the overlapped structure, which is used by the coro library. They require a handle that
-is compatible with iocp and they will attach the handle to the iocp instance. Those are the
-functions listed by msdn to work with iocp (and connect, that is part of an extension) */
+expose the overlapped structure, which is used by the coro library, nor a completion routine: the
+completion comes through the port, which is the engine's (a routine would take it away from the
+port, and the await would never end). They require a handle that is compatible with iocp and
+they will attach the handle to the iocp instance. Those are the functions listed by msdn to work
+with iocp (and connect, that is part of an extension). 2026-10-29 */
 inline task<BOOL> ConnectEx(SOCKET  s, const sockaddr *name, int namelen, PVOID lpSendBuffer,
         DWORD dwSendDataLength, LPDWORD lpdwBytesSent, error_e *err)
@@ -5799,10 +5792,10 @@
 inline task<BOOL> ReadDirectoryChangesW(HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength,
         BOOL bWatchSubtree, DWORD dwNotifyFilter, LPDWORD lpBytesReturned,
-        LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine, error_e *err)
+        error_e *err)
 {
     auto desc = create_io_desc(co_await get_pool());
 
     auto params = std::tuple{hDirectory, lpBuffer, nBufferLength, bWatchSubtree, dwNotifyFilter,
-            lpBytesReturned, &desc.data->overlapped, lpCompletionRoutine};
+            lpBytesReturned, &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
@@ -5975,5 +5968,5 @@
 
 inline task<BOOL> WSASendMsg(SOCKET handle, LPWSAMSG lpMsg, DWORD dwFlags,
-        LPDWORD lpNumberOfBytesSent, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
+        LPDWORD lpNumberOfBytesSent,
         error_e *err)
 {
@@ -5985,5 +5978,5 @@
 
     auto params = std::tuple{handle, lpMsg, dwFlags, lpNumberOfBytesSent,
-            &desc.data->overlapped, lpCompletionRoutine};
+            &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
@@ -6014,10 +6008,10 @@
 inline task<BOOL> WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
         LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const sockaddr *lpTo, int iTolen,
-        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine, error_e *err)
+        error_e *err)
 {
     auto desc = create_io_desc(co_await get_pool());
 
     auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo,
-            iTolen, &desc.data->overlapped, lpCompletionRoutine};
+            iTolen, &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
@@ -6048,10 +6043,10 @@
 inline task<BOOL> WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
         LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
-        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine, error_e *err)
+        error_e *err)
 {
     auto desc = create_io_desc(co_await get_pool());
 
     auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
-            &desc.data->overlapped, lpCompletionRoutine};
+            &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
@@ -6082,10 +6078,10 @@
 inline task<BOOL> WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
         LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, sockaddr *lpFrom, LPINT lpFromlen,
-        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine, error_e *err)
+        error_e *err)
 {
     auto desc = create_io_desc(co_await get_pool());
 
     auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom,
-            lpFromlen, &desc.data->overlapped, lpCompletionRoutine};
+            lpFromlen, &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
@@ -6115,5 +6112,5 @@
 
 inline task<BOOL> WSARecvMsg(SOCKET s, LPWSAMSG lpMsg, LPDWORD lpdwNumberOfBytesRecvd,
-        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine, error_e *err)
+        error_e *err)
 {
     if (!_wsa_recv_msg && load_win_fn(WSAID_WSARECVMSG, _wsa_recv_msg) != ERROR_OK) {
@@ -6125,5 +6122,5 @@
 
     auto params = std::tuple{s, lpMsg, lpdwNumberOfBytesRecvd,
-            &desc.data->overlapped, lpCompletionRoutine};
+            &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
@@ -6154,10 +6152,10 @@
 inline task<BOOL> WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
         LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
-        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine, error_e *err)
+        error_e *err)
 {
     auto desc = create_io_desc(co_await get_pool());
 
     auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
-            &desc.data->overlapped, lpCompletionRoutine};
+            &desc.data->overlapped, nullptr};
     using params_t = decltype(params);
 
```

## The regression test

None lands: the regression is a compile-time one. A caller that passes a routine no longer
compiles, and a caller that passed NULL (the only working value) compiles without the argument;
the suite's two such callers were updated with the fix and the suite's build is the check. The
probe that showed the hang (a `co::WSARecv` with a routine, 3 s watchdog) is quoted above.

## How it was checked

1. The behavior, by a probe on the header before the fix: the await with a routine never
   completed in 3 s and the routine never ran; without a routine it completed.
2. The suite builds and passes with the parameter gone; `005-001` and `018-041` run.
3. At apply time, 2026-10-29, on the header of that day:

   | check                       | result                                                       |
   |-----------------------------|--------------------------------------------------------------|
   | documented usage reaches it | yes: any non-NULL routine passed to one of the seven         |
   | DESIGN.md                   | not concerned                                                |
   | epoll / kqueue              | Windows-only code; the Linux suite passes                    |
   | contract / docs             | the seven doc comments and both block comments; `02_api.md`  |
   |                             | mirrored                                                     |
   | callers                     | `005-001`, `018-041` updated; no other copy of colib.h in    |
   |                             | the workspace has user code calling these wrappers           |
   | pending fixes               | none touches the wrappers' signatures                        |
   | runs                        | the suites: see the status line                              |

## Interactions

- #49 (this batch): the same wrappers' request lambdas; no shared lines.
- #11 (applied): the same wrappers' early exits; no shared lines.

Decided at apply time, 2026-10-29: the parameter goes (my choice, on the user's "do a fix for
them"; the NULL-only documentation was the alternative); the Windows call receives `nullptr`.
