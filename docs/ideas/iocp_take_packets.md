# IOCP: taking the completion packets, one function instead of two

Status: **proposal, not applied to colib.h.** The diff was made on a scratch copy of today's colib.h
(after `iocp_request_status.md`, `iocp_wrappers_like_sync.md` and the `win_err` move); it hasn't been
compiled yet.

## What's wrong with `handle_ready_events()` today

1. **Two jobs, switched by a null pointer.** Called from `handle_ready()` with an entry, it handles
   that entry and then drains the queue; called from `force_awake()` with `NULL`, it only drains,
   and points `oe` at a local `aux` so the drain has somewhere to write.
2. **An inner `while (cnt) { ...; break; }` that runs at most once:** it's an `if`. Inside it, a
   `cnt = 0;` after a `return` that can't run, and `cnt = 0; break;` twice.
3. **One packet per call:** `GetQueuedCompletionStatusEx` takes a batch; colib asks for 1 and loops.
4. **The same operation twice:** the blocking wait in `handle_ready()` and the draining here are
   one `GetQueuedCompletionStatusEx` with a different timeout.
5. **Twelve trace lines** in the middle of the logic.

## The proposal

- **`_take_packets(bool wait, io_data_t *skip)`** takes packets in batches of 64 until the queue is
  empty. With `wait` its first call blocks (alertable, as today: colib's timers post their packets
  from an APC, so `WAIT_IO_COMPLETION` just loops); afterwards, or without `wait`, a timeout of 0
  takes only what is already queued, and `WAIT_TIMEOUT` ends it. `handle_ready()` calls it with
  `wait`, `force_awake()` without, and with its stopped request as `skip`.
- **`_take_packet(entry, skip)`**, one packet: the status and the Win32 code of a failure (as now),
  the byte count, and the coroutine to the ready queue; `skip`'s packet is only taken off the queue.
  One trace line with the packet's fields.
- Both are private helpers of `io_pool_t`, with an underscore, next to `dequeue_data()`.
- **Dropped:** the `cnt == 0` after a successful blocking call ("WHY?"): `GetQueuedCompletionStatusEx`
  returns at least one entry when it succeeds.

Order is unchanged: packets come out of the queue first in, first out, whatever the batch size, and
each is handled before the next is looked at. 83 lines out, 43 in.

## The diff

```diff
--- a/colib.h
+++ b/colib.h
@@ -3513,88 +3513,7 @@
 
         /* Now that we know we have no corutines ready we know we have to wait for some sort of io
         event to happen(timers, file io, network io, etc.). */
-        OVERLAPPED_ENTRY entry = {};
-        ULONG cnt = 0;
-        while (true) {
-            /* If this is not reached but the app blocks it is most probably because there is
-            a non-async execution inside coroutine code because this is the only place in which this
-            coroutine library can block (iocp variant) */
-            COLIB_DEBUG_TRACE("Waiting for events...");
-            bool ret = GetQueuedCompletionStatusEx(iocp, &entry, 1, &cnt, INFINITE, TRUE);
-            COLIB_DEBUG_TRACE("Done waiting for events");
-            if (!ret) {
-                if (GetLastError() == WAIT_IO_COMPLETION) {
-                    /* Ok, this was signaled by the timer calback, or whatever apc */
-                    continue;
-                }
-                COLIB_DEBUG("Failed GetQueuedCompletionStatus: %s", get_last_error().c_str());
-                /* here and in linux impl, good place for warning exception? */
-                return ERROR_GENERIC;
-            }
-            if (cnt == 0) {
-                COLIB_DEBUG("WHY?");
-                return ERROR_GENERIC;
-            }
-            break;
-        }
-
-        /* We know that we have new events, now we want to take all the new events and awake
-        their coroutines */
-        return handle_ready_events(&entry, nullptr);
-    }
-
-    error_e handle_ready_events(OVERLAPPED_ENTRY *oe, io_data_t *to_filter) {
-        ULONG cnt = 1;
-        OVERLAPPED_ENTRY aux;
-        do {
-            if (oe) {
-                auto &entry = *oe;
-
-                /* awake the waiter (mark it's state->error and push it onto the ready tasks) */
-                COLIB_DEBUG_TRACE("OVERLAPPED_ENTRY:");
-                COLIB_DEBUG_TRACE("   key:          %p", entry.lpCompletionKey);
-                COLIB_DEBUG_TRACE("   overlapped:   %p", entry.lpOverlapped);
-                COLIB_DEBUG_TRACE("   internal:     %p", entry.Internal);
-                COLIB_DEBUG_TRACE("   num_bytes:    %d", entry.dwNumberOfBytesTransferred);
-                auto ovlpd = entry.lpOverlapped;
-                COLIB_DEBUG_TRACE("OVERLAPPED:");
-                COLIB_DEBUG_TRACE("   Internal:     %p", ovlpd->Internal);
-                COLIB_DEBUG_TRACE("   InternalHigh: %p", ovlpd->InternalHigh);
-                COLIB_DEBUG_TRACE("   hEvent:       %p", ovlpd->hEvent);
-                COLIB_DEBUG_TRACE("   Pointer:      %p", ovlpd->Pointer);
-                io_data_t *data = (io_data_t *)ovlpd;
-                if (data != to_filter) {
-                    /* how the request ended, as the kernel left it in its OVERLAPPED (an NTSTATUS,
-                    negative if it failed); colib's own timer packets leave it 0 */
-                    if ((LONG)data->overlapped.Internal < 0 &&
-                            !GetOverlappedResult(data->h, &data->overlapped, &data->recvlen, FALSE))
-                        data->win_err = GetLastError();     /* the failure's Win32 code, no wait */
-                    data->state->err = data->win_err != ERROR_SUCCESS ? ERROR_GENERIC : ERROR_OK;
-                    data->recvlen = entry.dwNumberOfBytesTransferred;
-
-                    awake_io(data);
-                }
-            }
-            else
-                oe = &aux;
-
-            while (cnt) {
-                cnt = 0;
-                if (!GetQueuedCompletionStatusEx(iocp, oe, 1, &cnt, 0, FALSE)) {
-                    if (GetLastError() == WAIT_TIMEOUT) {
-                        cnt = 0;
-                        break;
-                    }
-                    else {
-                        COLIB_DEBUG("FAILED: GetQueuedCompletionStatusEx: %s", get_last_error().c_str());
-                        return ERROR_GENERIC;
-                    }
-                    cnt = 0;
-                }
-                break;
-            }
-        } while (cnt);
-        return ERROR_OK;
+        return _take_packets(true, nullptr);
     }
 
     /* The order should be: add_waiter, do the request, suspend */
@@ -3693,7 +3612,7 @@
             /* If events where queued we need to handle them here, that is so
             we can safely free `data` */
             /* We also filter for data, so we don't queue it twice */
-            if (handle_ready_events(NULL, data.get()) != ERROR_OK) {
+            if (_take_packets(false, data.get()) != ERROR_OK) {
                 COLIB_DEBUG("Failed to get new events after io-cancel");
                 return ERROR_GENERIC;
             }
@@ -3785,6 +3704,47 @@
     }
 
 private:
+    /* Takes the completion packets from the queue and queues their coroutines; the packet of
+    `skip` is only taken off the queue (its request was stopped, force_awake() delivers it). With
+    `wait` it first blocks until packets come, the only place colib blocks on IOCP: if the program
+    hangs here, some coroutine is running blocking code. Then, or without `wait`, it takes what is
+    already queued and returns. */
+    error_e _take_packets(bool wait, io_data_t *skip) {
+        OVERLAPPED_ENTRY entries[64];
+        ULONG cnt = 0;
+        while (true) {
+            /* only the blocking wait is alertable: colib's timers post their packets from an APC */
+            if (!GetQueuedCompletionStatusEx(iocp, entries, 64, &cnt, wait ? INFINITE : 0, wait)) {
+                if (GetLastError() == WAIT_IO_COMPLETION)
+                    continue;           /* an APC ran, its packet is queued now */
+                if (GetLastError() == WAIT_TIMEOUT)
+                    return ERROR_OK;    /* nothing left in the queue */
+                COLIB_DEBUG("Failed GetQueuedCompletionStatusEx: %s", get_last_error().c_str());
+                return ERROR_GENERIC;
+            }
+            for (ULONG i = 0; i < cnt; i++)
+                _take_packet(entries[i], skip);
+            wait = false;               /* the first ones came: now only what is already there */
+        }
+    }
+
+    /* one packet: how its request ended, then its coroutine goes to the ready queue */
+    void _take_packet(const OVERLAPPED_ENTRY &entry, io_data_t *skip) {
+        io_data_t *data = (io_data_t *)entry.lpOverlapped;
+        COLIB_DEBUG_TRACE("packet: key %p overlapped %p bytes %d status %p", entry.lpCompletionKey,
+                entry.lpOverlapped, entry.dwNumberOfBytesTransferred, data->overlapped.Internal);
+        if (data == skip)
+            return;
+        /* how the request ended, as the kernel left it in its OVERLAPPED (an NTSTATUS, negative if
+        it failed); colib's own timer packets leave it 0 */
+        if ((LONG)data->overlapped.Internal < 0 &&
+                !GetOverlappedResult(data->h, &data->overlapped, &data->recvlen, FALSE))
+            data->win_err = GetLastError();     /* the failure's Win32 code, no wait */
+        data->state->err = data->win_err != ERROR_SUCCESS ? ERROR_GENERIC : ERROR_OK;
+        data->recvlen = entry.dwNumberOfBytesTransferred;
+        awake_io(data);
+    }
+
     void dequeue_data(io_data_t *data) {
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(data, "Can't deque null data");
         COLIB_DEBUG_TRACE("dequeue_data: %p", data);
```

## Checks to run once it's applied (or on the scratch copy)

The whole Windows suite (the io, timer, stop and timeout tests go through here: `005-*`, `003-*`,
`018-015`, `018-023`, `018-024`, `018-025`, `018-027`), under ASan.
