# Proposal: a kill asks for its target back (RECLAIM) (fix #44)

Status: `[APPLIED 2026-10-12 on the user's word, together with FIX_47]`. The diff below is against
commit 9a13a9a ("fix 02"), the tree it was built on, and its checks are in "How it was checked".
FIX_47 changes one case of `018-035`, the external awaitable that holds the target without modifs;
the test landed in that form (see `FIX_47-killer_external_reclaim.md`).

After the apply, `colib.h` is commit 9a13a9a with this diff and FIX_47's on top, byte for byte, and
`make` in `tests/` builds and runs its 90 tests against it: `[PASSED: 90]/[FAILED: 0]`.

## The bug

`killer_state_t::kill()` hands a target whose wait already did its work (a semaphore token taken,
or io data moved: `woken_with_effect()`) to `drive()`. `drive()` puts a placeholder in the target's
place in the ready queue, resumes the target, and counts on the killer's own `on_wait` modif to
park it at its next wait (`ERROR_SUSPENDED`). After the resume it called `end_chain()` without
checking whether anything else had taken the target on the way.

Something outside colib can take it first and keep a pointer to it:

- **ext:** an external awaitable takes it (`external_on_suspend()`), to resume it later;
- **call:** a user CALL modif takes a call it makes (`ERROR_SUSPENDED`), to start the callee later;
- **copark:** a user WAIT_SEM modif parks it at the same wait where `on_wait` parks it.

`end_chain()` destroyed it anyway, and the holder then used freed memory: `external_sched_resume()`
reads `state->pool`, `destroy_state()` reads `caller_state`, and starting the taken call resumes a
freed callee. All three are confirmed heap-use-after-frees under ASan, on HEAD and on the working
tree (`repro/44-killer_drive_held.md`).

The killer's other park has the same flaw. When the driven root ends by itself, `on_return` queued
the root's caller into the placeholder's place and returned `ERROR_SUSPENDED`, so that the final
awaiter would not resume the caller inside the kill. A user RETURN modif that takes the caller too
(it returns `ERROR_SUSPENDED` and resumes the caller later) then holds a caller that is also queued,
and the caller runs twice; if it already gave the caller back, the queueing links the caller twice
and the program ends (`std::terminate`, "in two queues at once").

## The rule

A kill ends the code it runs, and the killer can't end something a user holds (DESIGN.md, 1). The
two meet in a handshake: after the drive, the kill asks whoever took the target on the way to let
it go.

- **RECLAIM** is a new modif event: "a kill needs this coroutine back". `exec_reclaim_modifs(s)`
  calls `s`'s RECLAIM callbacks: the target's once the drive stopped, or the root's caller's when
  the root returned during the drive. A modif that holds a coroutine must therefore be among that
  coroutine's modifs; a RETURN modif that takes a caller is on the caller too (inherited from it).
  The callback receives the coroutine for convenience, not as something to check.
- **A modif that holds it lets it go**: it forgets it, stops whatever would resume or destroy it,
  and returns `ERROR_OK`. The target then goes on as it would have without that modif, which is the
  drive's own rule: to its next wait, where the kill ends it, or to its own end (`ERROR_FINISHED`).
  A released callee starts; a released caller resumes and reads its callee's result, so the
  callee's frame is freed by that read, as in any call. A released root's caller is queued.
- **A modif that keeps it returns an error, or `ERROR_SUSPENDED`.** The kill throws
  `kill_not_colib_owned_t` and the holder keeps the target. A caller that a RETURN modif keeps is
  left to it, without a throw (the target ended by itself).
- **A modif that holds nothing returns `ERROR_OK`.** Silence means success, so a modif that parks
  coroutines must answer RECLAIM, or a kill takes them from it.
- **An external awaitable can't be asked.** While one holds a coroutine, its state reads
  `STATE_EXTERNAL`, a new public value of `state_e`; a kill that finds the target in it throws.
  An awaitable that gives the target straight back (`external_sched_resume()`) holds nothing: the
  state follows the ready queue, and the target goes on.

The killer uses only public pieces for this (DESIGN.md, 2: the library doesn't keep anything for
the killer's sake): it reads the state with `get_state()`, and it asks with the new public
`exec_reclaim_modifs()`, which a kill written by the user can call the same way.

| what takes the driven target on the way    | before                 | now                        |
|--------------------------------------------|------------------------|----------------------------|
| nothing: it reaches its next wait          | ended there            | unchanged                  |
| a WAIT modif parks it too and lets it go   | freed under the holder | ended, `ERROR_OK`          |
| (added before or after the killer's pack)  |                        |                            |
| a WAIT modif parks it too and keeps it     | freed under the holder | throws, the holder keeps   |
|                                            |                        | it                         |
| a CALL modif takes its call and lets it go | freed under the holder | the callee starts; the     |
|                                            |                        | target goes on to its next |
|                                            |                        | wait or its end            |
| a CALL modif takes its call and keeps it   | freed under the holder | throws                     |
| an external awaitable takes it             | freed under the holder | throws                     |
| an external awaitable takes it and gives   | ended                  | it goes on to its next     |
| it straight back                           |                        | wait or its end            |
| a callee returns and its RETURN modif      | freed under the        | the target resumes and     |
| takes the target, then lets it go          | holder; the callee's   | reads the callee's result; |
|                                            | frame leaks            | it goes on                 |
| the same, and the modif keeps the target   | freed under the holder | throws; the callee's       |
|                                            |                        | result waits for the       |
|                                            |                        | target                     |
| its root returns                           | the caller takes the   | unchanged                  |
|                                            | slot                   |                            |
| its root returns and a RETURN modif takes  | the caller is queued   | `ERROR_FINISHED`; the      |
| the caller and keeps it                    | and taken: it runs     | modif resumes it           |
|                                            | twice                  |                            |
| its root returns and a RETURN modif        | the caller is queued   | `ERROR_FINISHED`; the      |
| already gave the caller back               | twice:                 | caller runs once           |
|                                            | `std::terminate`       |                            |

Outside a drive nothing changes for parks: a target that a modif already holds when the kill comes
still makes the kill throw, without asking (open question 3).

## Where the fix lands

| `colib.h` region                       | what changes                                  | +/-     |
|----------------------------------------|-----------------------------------------------|---------|
| `modif_e`                              | `CO_MODIF_RECLAIM_CBK`, with its contract     | +13 -0  |
| `state_e`                              | `STATE_EXTERNAL`                              | +2 -0   |
| `modif_t::variant_t`                   | the reclaim callback's slot                   | +2 -1   |
| `external_on_suspend` doc              | the state it leaves, and what a kill does     | +2 -0   |
|                                        | then                                          |         |
| `exec_reclaim_modifs()` declaration    | public: asks a coroutine's holders to let it  | +9 -0   |
|                                        | go                                            |         |
| `create_killer` doc                    | what a kill does with a target taken on the   | +5 -1   |
|                                        | way                                           |         |
| `modif_table_t`                        | a 13th vector, and the count in its assert    | +2 -1   |
| `exec_reclaim_modifs()` definition     | runs the RECLAIM callbacks                    | +5 -0   |
| `external_on_suspend()`                | writes `STATE_EXTERNAL`                       | +1 -0   |
| `killer_state_t::on_return`            | the caller is queued by `drive()`, not here   | +1 -2   |
| `killer_state_t::on_wait`              | records its own park (`our_park`)             | +4 -1   |
| `killer_state_t::kill()`               | names `STATE_EXTERNAL` next to `STATE_LEFT`   | +4 -4   |
| `killer_state_t::drive()` and helpers  | the loop over the stops; `root_returned()`;   | +39 -10 |
|                                        | `queue_caller(pool, caller)`; the `completed` |         |
|                                        | reset                                         |         |
| `killer_state_t` members               | `our_park`                                    | +1 -0   |
| total                                  |                                               | +91 -20 |

## The change

The diff is against the working tree's `colib.h`. The reasoning, per hunk in diff order:

1. **`CO_MODIF_RECLAIM_CBK`.** The new event and its whole contract: when a kill asks, what about
   (the coroutine, or the root's caller), how a holder lets go or keeps, which return values mean
   which, and that silence means success.
2. **`STATE_EXTERNAL`.** A public state for a coroutine that an external awaitable holds. It needs
   no new field: `external_on_suspend()` writes it, and the next state change (ENTER, the ready
   queue, the next LEAVE) replaces it, so a coroutine handed straight back reads `STATE_READY`.
3. **`modif_t::variant_t`.** The reclaim callback's slot, at the index of its `modif_e`, as
   `std::get<cbk_id>` requires.
4. **`external_on_suspend` doc.** What an awaitable's author must know: while the awaitable holds
   the coroutine, its state reads `STATE_EXTERNAL` and a kill throws instead of ending it.
5. **`exec_reclaim_modifs()`, declared.** The public way to ask a coroutine's holders to let it
   go, with what its return value means.
6. **`create_killer` doc.** What a caller observes: a target taken on the way is asked for; let
   go, it goes on to its next wait or its end; kept, the kill throws; an external awaitable can't
   be asked.
7. **`modif_table_t`.** One vector per `modif_e`, and the count in the assert.
8. **`exec_reclaim_modifs()`, defined.** Runs the RECLAIM callbacks, as the internal wrappers of the
   other events do. Their returns are not ignored: an error stops the list; an error or
   `ERROR_SUSPENDED` is "I keep it".
9. **`external_on_suspend()`.** Writes `STATE_EXTERNAL` after the LEAVE callbacks.
10. **`on_return`.** It still parks the driven root's return, so the caller isn't resumed inside
   the kill, but it no longer queues the caller: `drive()` does, after the resume, once it asked
   the caller's RECLAIM callbacks.
11. **`on_wait`.** It records that the kill parked the target at a wait (`our_park`): the one kind
    of stop where the kill ends the target.
12. **`kill()`'s switch.** `STATE_EXTERNAL` is named next to `STATE_LEFT`: neither is in a colib
    queue, so the kill throws, or returns `ERROR_GENERIC` when its own drive is running.
13. **`drive()` and its helpers.** `drive()` captures the root and its caller before the resume
    (a root without a caller frees itself at its end), and clears `completed`: a killer can be
    bound to a new target later (FIX_18), and a flag left from an earlier drive would make this one
    skip `end_chain()` and queue a caller whose callee still runs. Then it runs the target, in a
    loop, and looks at each stop:
    - the root returned: `root_returned()` gives the caller the slot, unless the caller is no
      longer left in its call (a RETURN modif already gave it back) or one of the caller's RECLAIM
      callbacks keeps it; then the slot is dropped. The kill returns `ERROR_FINISHED`;
    - the target reads `STATE_EXTERNAL`, or a RECLAIM callback of the target keeps it: the slot is
      dropped and the kill throws `kill_not_colib_owned_t`;
    - the kill's own park stopped it at a wait: `end_chain()`, as before;
    - otherwise a call or a return took it and let it go, or an awaitable gave it straight back:
      it is taken out of the ready queue if it was put there, and resumed again, as colib would
      have resumed it.

    `queue_caller()` takes the pool and the caller instead of the root, because `root_returned()`
    must not read a root that freed itself.

```diff
--- colib.h (commit 9a13a9a)
+++ colib.h (patched)
@@ -778,6 +778,19 @@
     alive and awaiting it again calls it again. The return value is ignored */
     CO_MODIF_RETURN_CBK,
 
+    /*! This is called when a kill that ran its chain (a wait had already done its work) needs this
+    corutine back from a modif that took it on the way: a park, a taken call, or a RETURN modif
+    of its callee that took it as the caller. Only this corutine's modifs are asked, so a modif
+    that holds a corutine must be among its modifs (a RETURN modif that takes the caller must be
+    on the caller too, inherited from it for example). The modif that holds it lets it go: it
+    forgets it, stops whatever would resume or destroy it, and returns ERROR_OK; a modif that
+    holds nothing returns ERROR_OK too, as does any value that is neither an error nor
+    ERROR_SUSPENDED. A modif that keeps it returns an error (the callbacks after it aren't asked)
+    or ERROR_SUSPENDED, and the kill throws kill_not_colib_owned_t (a root's caller is left to
+    the modif, without a throw). Silence lets it go, so a modif that holds corutines must answer
+    this */
+    CO_MODIF_RECLAIM_CBK,
+
     CO_MODIF_COUNT,
 };
 
@@ -1142,6 +1155,8 @@
     STATE_READY,        /*!< in its pool's ready queue */
     STATE_WAITING_SEM,  /*!< in a semaphore's wait list */
     STATE_WAITING_IO,   /*!< waiting on an io */
+    STATE_EXTERNAL,     /*!< held by an external awaitable, from external_on_suspend() until it is
+                        resumed or scheduled again */
 };
 
 /*! Internal state of corutines that is independent of the return value of the corutine.
@@ -1212,7 +1227,8 @@
 
         std::function<error_e(state_t *)>,              /* yield_cbk */
         std::function<error_e(state_t *)>,              /* unyield_cbk */
-        std::function<error_e(state_t *)>               /* return_cbk */
+        std::function<error_e(state_t *)>,              /* return_cbk */
+        std::function<error_e(state_t *)>               /* reclaim_cbk */
     >;
 
     /*! This is the callback that will be called on the location specified by type. It must be
@@ -1357,6 +1373,8 @@
  * Usage:
  * - Call this in your awaitable’s await_suspend before performing any custom suspension logic.
  * - Returns a raw pointer to the coroutine’s state, which must later be resumed.
+ * - Until then the coroutine's state reads STATE_EXTERNAL, and a kill can't end it: it throws
+ *   kill_not_colib_owned_t.
  * 
  * @tparam P Coroutine promise type
  * @param colib_coro The coroutine handle being suspended
@@ -1509,6 +1527,15 @@
 inline task_t rm_modifs(const modif_pack_t& mods);
 
 /*! @fn
+ * Asks the modifs that hold a coroutine to let it go: runs its RECLAIM callbacks (see
+ * CO_MODIF_RECLAIM_CBK). A kill calls it on a coroutine it ran before it ends it; a kill written by
+ * the user can do the same.
+ *
+ * @param state The coroutine whose holders are asked.
+ * @return ERROR_OK when none of them keeps it, an error or ERROR_SUSPENDED when one keeps it. */
+inline error_e exec_reclaim_modifs(state_t *state);
+
+/*! @fn
  * Helper coroutine function, given an awaitable, awaits it inside the coroutine await,
  * usefull if the awaitable can't be decorated with modifiers, bacause it isn't a coroutine.
  * 
@@ -1607,7 +1634,12 @@
  * Creates a killer for one coroutine: a modif pack to attach to it, and a function that kills it.
  * The kill destroys the coroutine and its whole call stack, not what it scheduled. A coroutine
  * waiting on something dies at once; one whose wait already completed first runs up to its next
- * wait, so nothing it received is lost. When the kill returns, the target is dead.
+ * wait, so nothing it received is lost. When the kill returns, the target is dead. If something
+ * else took it on the way, the kill asks for it back through the RECLAIM callbacks (see
+ * CO_MODIF_RECLAIM_CBK): let go, it goes on as it would have without that modif, to its next
+ * wait, where it dies, or to its own end; kept, the kill throws kill_not_colib_owned_t and the
+ * holder keeps it. An external awaitable can't be asked: one that holds it keeps it, and the kill
+ * throws.
  *
  * The kill function returns ERROR_OK when it killed the target and ERROR_FINISHED when there was
  * nothing left to kill. Called from a callback of its own kill (EXIT, UNWAIT, or the target's
@@ -2689,10 +2721,11 @@
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
+            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}}
     } {}
 
-    static_assert(CO_MODIF_COUNT == 12, "one vector per modif_e in the constructor above");
+    static_assert(CO_MODIF_COUNT == 13, "one vector per modif_e in the constructor above");
     std::array<std::vector<modif_p, allocator_t<modif_p>>, CO_MODIF_COUNT> table;
 };
 
@@ -2806,6 +2839,11 @@
     return do_generic_modifs<CO_MODIF_EXIT_CBK>(state);
 }
 
+inline error_e exec_reclaim_modifs(state_t *state) {
+    COLIB_DEBUG_TRACE("    RECLAIM: %s state: %p", dbg_name(state->self).c_str(), state);
+    return do_generic_modifs<CO_MODIF_RECLAIM_CBK>(state);
+}
+
 inline error_e do_wait_io_modifs(state_t *state, io_desc_t &io_desc) {
     COLIB_DEBUG_TRACE("    WAIT: %s state: %p io:[%s]", dbg_name(state->self).c_str(), state,
             dbg_to_str(io_desc).c_str());
@@ -4472,6 +4510,7 @@
 inline state_t *external_on_suspend(handle<P> colib_coro) {
     auto state = &colib_coro.promise().state;
     do_leave_modifs(state);
+    state_access_t::set(state, STATE_EXTERNAL);    /* held outside colib: a kill can't ask */
     return state;
 }
 
@@ -6399,9 +6438,8 @@
         root = top = nullptr;
         if (!slot)
             return ERROR_OK;
-        /* ended by itself while driven: its caller gets its result in its slot, not from here */
+        /* ended by itself while driven: drive() gives its caller the slot, it isn't resumed here */
         completed = true;
-        queue_caller(s);
         return ERROR_SUSPENDED;
     }
 
@@ -6415,7 +6453,10 @@
     }
 
     error_e on_wait(state_t *) {
-        return slot ? ERROR_SUSPENDED : ERROR_OK;   /* driven: parked, drive() ends it */
+        if (!slot)
+            return ERROR_OK;
+        our_park = true;            /* driven: parked at its next wait, drive() ends it */
+        return ERROR_SUSPENDED;
     }
 
     /* ---------------------------------------------------------------- the kill function */
@@ -6430,11 +6471,11 @@
             case STATE_RUNNING:
                 throw kill_not_colib_owned_t(top);  /* called from its body */
 
-            case STATE_LEFT:
-                /* in no colib queue: it runs its own callbacks, or something else holds it (a
-                call, a park, an external awaitable), so ending it would free a frame in use */
+            case STATE_LEFT:        /* a call, a park, or its own suspension callbacks */
+            case STATE_EXTERNAL:    /* an external awaitable holds it */
+                /* in no colib queue, so ending it would free a frame in use */
                 if (slot)
-                    return ERROR_GENERIC;   /* its callbacks while drive() runs it: our own kill */
+                    return ERROR_GENERIC;   /* its suspension while drive() runs it: our own kill */
                 throw kill_not_colib_owned_t(top);
 
             case STATE_WAITING_IO: {
@@ -6463,31 +6504,60 @@
         return ERROR_OK;
     }
 
-    /* its wait already did its work: the code after it runs, here, up to its next wait */
+    /* its wait already did its work: the code after it runs, here, until its next wait, where
+    on_wait parks it and it ends, or until its end. Something else that took it on the way (a
+    call, a return, an external awaitable) is asked to let it go (RECLAIM): let go, it goes on as
+    colib would have gone on; kept, the kill throws */
     error_e drive() {
+        pool_internal_t *pool = top->pool->get_internal();
+        state_t *caller = root->caller_state;   /* a root without one frees itself at its end */
         state_t placeholder;        /* keeps its place in the ready queue while it runs */
-        top->pool->get_internal()->replace_ready(top, &placeholder);
+        pool->replace_ready(top, &placeholder);
         slot = &placeholder;
-        top->self.resume();
+        completed = false;          /* set by on_return if the root ends during this drive */
+        while (true) {
+            our_park = false;
+            top->self.resume();     /* runs until it stops */
+            if (completed)
+                return root_returned(pool, caller);
+            if (top && (top->get_state() == STATE_EXTERNAL ||
+                    exec_reclaim_modifs(top) != ERROR_OK)) {
+                queue_caller(pool, nullptr);    /* a holder keeps it: the slot is dropped */
+                throw kill_not_colib_owned_t(top);
+            }
+            if (!top || our_park)
+                break;              /* its next wait: it ends here */
+            if (top->get_state() == STATE_READY)
+                pool->remove_ready(top);    /* given back to the ready queue: it runs here */
+        }
         if (top)
-            end_chain();            /* parked at its next wait */
-        return completed ? ERROR_FINISHED : ERROR_OK;
+            end_chain();
+        return ERROR_OK;
+    }
+
+    /* the driven root ended by itself: its caller takes the slot, unless a RETURN modif took the
+    caller and keeps it, or already gave it back (it is no longer left in its call); a root
+    without a caller is already gone */
+    error_e root_returned(pool_internal_t *pool, state_t *caller) {
+        bool kept = caller && (caller->get_state() != STATE_LEFT ||
+                exec_reclaim_modifs(caller) != ERROR_OK);
+        queue_caller(pool, kept ? nullptr : caller);
+        return ERROR_FINISHED;
     }
 
     void end_chain() {
         killing = true;
         if (root->caller_state)
             root->err = e;
-        queue_caller(root);
+        queue_caller(root->pool->get_internal(), root->caller_state);
         close_wait(top);
         destroy_state(top, root);
         killing = false;
     }
 
-    /* the root's caller takes the target's slot in the ready queue, or goes to its end */
-    void queue_caller(state_t *root_state) {
-        pool_internal_t *pool = root_state->pool->get_internal();
-        state_t *caller = root_state->caller_state;
+    /* the root's caller takes the target's slot in the ready queue, or goes to its end; without
+    a caller the slot is only dropped */
+    void queue_caller(pool_internal_t *pool, state_t *caller) {
         bool took_slot = slot && caller && pool->replace_ready(slot, caller);
         if (!took_slot && slot)
             pool->remove_ready(slot);
@@ -6502,6 +6572,7 @@
     error_e e : 16;                     /* the err of a killed root that was called */
     uint32_t killing : 1 = false;       /* kill() is destroying the chain */
     uint32_t completed : 1 = false;     /* its root ended by itself while driven */
+    uint32_t our_park : 1 = false;      /* on_wait parked it during the current drive */
 };
 
 /* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
```

## The regression test

`018-035-reproduced_killer_drive_reclaim.cpp` (Test89; its number is assigned when the fix is
applied). Each case drives a target whose semaphore wait already gave it its token:

1. a WAIT_SEM modif added before the killer's pack parks the target too and lets it go: the kill
   returns `ERROR_OK`, the target is destroyed once, and the holder no longer holds it;
2. the same modif added after the killer's pack;
3. the same modif keeps it with an error: the kill throws, the target lives, its holder destroys
   it once;
4. the same, kept with `ERROR_SUSPENDED`;
5. a CALL modif takes a call the target makes and lets the callee go: the callee runs inside the
   kill, the target reads its result and goes on to its end (`ERROR_FINISHED`);
6. the same CALL modif keeps the callee: the kill throws, and the holder later starts the callee
   and the chain ends by itself;
7. a callee returns and its RETURN modif (on the target, inherited by the callee) takes the
   target, then lets it go: the target resumes, reads the callee's 5 (which frees the callee's
   frame) and dies at its next wait;
8. the same modif keeps the target: the kill throws, the callee's frame keeps its 5, and the
   holder later resumes the target, which reads it;
9. an external awaitable takes it: the kill throws, the holder later resumes it and it finishes;
10. an external awaitable takes it and gives it straight back to the ready queue: it goes on to
    its end;
11. the driven root returns and a RETURN modif (on the caller, inherited by the root) takes the
    caller and keeps it: `ERROR_FINISHED`, and the caller resumes once, through the modif;
12. the same RETURN modif lets the caller go: the kill queues the caller, which resumes once;
13. the same RETURN modif already gave the caller back: the caller resumes once, and nothing is
    queued twice.

On the unpatched header the file doesn't compile (`CO_MODIF_RECLAIM_CBK` is new). For the runs
below, the unpatched copy maps `CO_MODIF_RECLAIM_CBK` to an event that never fires in these cases,
so each case shows what today's code does.

`tests/012-002-introspection_state.cpp` gets one line with the fix, a name for the new state:

```diff
         case co::STATE_WAITING_IO:  return "WAITING_IO";
+        case co::STATE_EXTERNAL:    return "EXTERNAL";
```

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <memory>

/* Test89 - Reproduced Bugs: a kill that ran its target freed it under whoever took it on the way
================================================================================================= */

/* Was an open bug (repro/44): a kill of a target whose wait already did its work runs the target
up to its next wait inside killer_state_t::drive(), where the killer's own on_wait parks it, and
then ended the chain without looking. Something else could take the target on the way and keep a
pointer to it: a user modif parking it at the same wait, a user CALL modif taking a call it makes,
an external awaitable. The kill destroyed it anyway, and the holder then used freed memory. The
same went for the root's caller when the driven root returned and a user RETURN modif took the
caller: the kill queued the caller too, so it ran twice.

The kill now asks whenever the drive stops somewhere it didn't stop it: the RECLAIM callbacks of
the coroutine it needs are called, and a modif that holds it lets it go (forgets it, returns
ERROR_OK) or keeps it (returns an error or ERROR_SUSPENDED). Kept, the kill throws
kill_not_colib_owned_t and the holder keeps it. Let go at a wait, the kill ends it there; let go
at a call or a return, it goes on as colib would have, until its next wait or its end. An external
awaitable can't be asked: while one holds the target (STATE_EXTERNAL) the kill throws, and a target
it gave straight back to the ready queue goes on. For the root's caller, the caller's modifs are
asked (a RETURN modif that takes it is on it too, inherited from it): one that keeps it resumes
it, one that lets it go has the kill queue it, and one that already gave it back is left alone.

The runs, each on a target whose semaphore wait already gave it its token: a modif parks it at its
next wait and lets it go, added before and after the killer's pack; the same modif keeps it, by an
error and by ERROR_SUSPENDED; a CALL modif takes its call and lets the callee go, or keeps it; a
callee returns and a RETURN modif takes the target, then lets it go or keeps it; an external
awaitable takes it, or takes it and gives it straight back; the driven root returns and a RETURN
modif keeps its caller, lets it go, or already gave it back. */

static const char *test89_file = "018-035-reproduced_killer_drive_reclaim.cpp";

static int test89_destructed = 0;
static bool test89_got_token = false;       /* the code after the completed wait ran */
static bool test89_after = false;           /* the code after the hold ran */
static co::state_t *test89_held = nullptr;  /* what the holder keeps */
static bool test89_threw = false;           /* the kill threw, carrying the held frame */
static co::error_e test89_ret = co::ERROR_GENERIC;

struct test89_marker_t {
    ~test89_marker_t() { test89_destructed++; }
};

static void test89_reset() {
    test89_destructed = 0;
    test89_got_token = false;
    test89_after = false;
    test89_held = nullptr;
    test89_threw = false;
    test89_ret = co::ERROR_GENERIC;
}

/* the target waits on sem, it gets the token, and the kill runs it: the controller of every case */
static co::task_t test89_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the target waits on sem */
    sem->signal();              /* it takes the token and is queued, not resumed yet */
    try {
        test89_ret = kill_fn(); /* its wait did its work: the kill runs it */
    }
    catch (co::kill_not_colib_owned_t &e) {
        test89_threw = (e.target == test89_held);
    }
    co_return 0;
}

/* ------------------------------------------------- a user modif parks it at its next wait too */

static co::task_t test89_park_victim(co::sem_p sem, co::sem_p never) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test89_got_token = true;
    co_await never->wait();     /* the user modif and the killer's on_wait both park it here */
    test89_after = true;        /* must never run */
    co_return 0;
}

/* parks the target at `never`; asked for it, it answers `answer` (ERROR_OK lets it go) */
static co::modif_pack_t test89_park_mods(co::sem_t *never, co::error_e answer) {
    auto f = co::CO_MODIF_INHERIT_NONE;
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(f,
            [never](co::state_t *s, co::sem_t *sem) -> co::error_e {
                if (sem != never)
                    return co::ERROR_OK;
                test89_held = s;
                return co::ERROR_SUSPENDED;     /* parked: this modif holds it */
            }));
    pack.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [answer](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (answer != co::ERROR_OK)
                    return answer;              /* it keeps it */
                test89_held = nullptr;          /* it lets it go: it forgets it */
                return co::ERROR_OK;
            }));
    return pack;
}

static int test89_copark(bool park_first, co::error_e answer) {
    test89_reset();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto park_mods = test89_park_mods(never.get(), answer);

    auto victim = test89_park_victim(sem, never);
    co::add_modifs(pool.get(), victim, park_first ? park_mods : killer_mods);
    co::add_modifs(pool.get(), victim, park_first ? killer_mods : park_mods);

    pool->sched(COLIB_REGNAME(victim));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test89_got_token));
    ASSERT_FN(CHK_BOOL(!test89_after));
    if (answer == co::ERROR_OK) {
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_OK));    /* the kill ended it */
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* its holder let it go */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the holder kept it */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        co::destroy_state(test89_held);                     /* its holder ends it, once */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ------------------------------------------------- a user CALL modif takes the call it makes */

static std::weak_ptr<int> test89_callee_frame;
static int test89_callee_got = -1;

static co::task_t test89_callee(std::shared_ptr<int> keep) {
    (void)keep;                 /* the frame's copy: it expires when the frame is destroyed */
    co_return 5;
}

static co::task_t test89_call_victim(co::pool_t *pool, co::sem_p sem, co::modif_pack_t take) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test89_got_token = true;
    auto keep = std::make_shared<int>(0);
    test89_callee_frame = keep;
    auto callee = co::add_modifs(pool, test89_callee(keep), take);
    keep.reset();               /* the callee's frame holds the last copy */
    test89_callee_got = co_await callee;    /* the user CALL modif takes this call */
    test89_after = true;        /* runs only once the holder starts the callee */
    co_return 0;
}

static int test89_call(bool lets_go) {
    test89_reset();
    test89_callee_got = -1;
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto f = co::CO_MODIF_INHERIT_NONE;
    co::modif_pack_t take;
    take.push_back(co::create_modif<co::CO_MODIF_CALL_CBK>(f, [](co::state_t *s) -> co::error_e {
        test89_held = s;        /* the callee: this modif would start it later */
        return co::ERROR_SUSPENDED;
    }));
    take.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [lets_go](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (!lets_go)
                    return co::ERROR_USER;      /* it keeps the callee */
                test89_held = nullptr;          /* it lets the callee go */
                return co::ERROR_OK;
            }));

    pool->sched(co::add_modifs(pool.get(), test89_call_victim(pool.get(), sem, take), killer_mods));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test89_got_token));

    if (lets_go) {
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* the holder let the callee go */
        ASSERT_FN(CHK_BOOL(test89_callee_got == 5));        /* the callee ran, inside the kill */
        ASSERT_FN(CHK_BOOL(test89_callee_frame.expired())); /* its caller's await freed it */
        ASSERT_FN(CHK_BOOL(test89_after));                  /* no wait after it: it went on */
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_FINISHED));  /* and ended by itself */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the holder kept the callee */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        co::external_sched_resume(test89_held);             /* the holder starts the callee */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_callee_got == 5 && test89_after));
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));        /* the chain ended by itself */
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* -------------------------------------- a callee returns, and its RETURN modif takes the target */

static int test89_c_got = -1;
static std::weak_ptr<int> test89_c_frame;

static co::task_t test89_c(co::sem_p sem, std::shared_ptr<int> in_frame) {
    (void)in_frame;             /* this copy dies with C's frame */
    co_await sem->wait();       /* signaled, then killed: the kill runs C from here */
    test89_got_token = true;
    co_return 5;                /* its RETURN modif takes the target, C's caller */
}

static co::task_t test89_r(co::sem_p sem, co::sem_p never) {
    test89_marker_t marker;
    auto in_frame = std::make_shared<int>(0);
    test89_c_frame = in_frame;
    test89_c_got = co_await test89_c(sem, std::move(in_frame));
    co_await never->wait();     /* its next wait */
    test89_after = true;        /* runs only once never is signaled */
    co_return 0;
}

static int test89_callee_return(bool lets_go) {
    test89_reset();
    test89_c_got = -1;
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* on the target, inherited by its callee: the callee's RETURN takes the target */
    auto f = co::CO_MODIF_INHERIT_ON_CALL;
    co::modif_pack_t user_mods;
    user_mods.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(f,
            [](co::state_t *s) -> co::error_e {
                if (!s->caller_state)
                    return co::ERROR_OK;        /* the target's own return: no caller to take */
                test89_held = s->caller_state;  /* it takes the target: it resumes it later */
                return co::ERROR_SUSPENDED;
            }));
    user_mods.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [lets_go](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (!lets_go)
                    return co::ERROR_USER;      /* it keeps the target */
                test89_held = nullptr;          /* it lets the target go */
                return co::ERROR_OK;
            }));

    auto r = co::add_modifs(pool.get(), test89_r(sem, never), killer_mods);
    co::add_modifs(pool.get(), r, user_mods);
    pool->sched(r);
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test89_got_token));

    if (lets_go) {
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* the holder let the target go */
        ASSERT_FN(CHK_BOOL(test89_c_got == 5));             /* the target went on: it read the 5 */
        ASSERT_FN(CHK_BOOL(test89_c_frame.expired()));      /* and its await freed C's frame */
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_OK));    /* ended at its next wait */
        ASSERT_FN(CHK_BOOL(!test89_after));
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the holder kept the target */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        ASSERT_FN(CHK_BOOL(!test89_c_frame.expired()));     /* C's 5 waits for the target */
        co::external_sched_resume(test89_held);             /* the holder resumes the target */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_c_got == 5 && test89_c_frame.expired()));
        never->signal();
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_after && test89_destructed == 1));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ---------------------------------------------------- an external awaitable takes the target */

struct test89_hold_t {
    bool hands_back;            /* it gives the target straight back to the ready queue */

    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        test89_held = co::external_on_suspend(h);   /* it keeps it, to resume it later */
        if (hands_back) {
            co::external_sched_resume(test89_held); /* or it gives it back at once */
            test89_held = nullptr;
        }
        return std::noop_coroutine();
    }

    void await_resume() {}
};

static co::task_t test89_ext_victim(co::sem_p sem, bool hands_back) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test89_got_token = true;
    co_await test89_hold_t{hands_back};
    test89_after = true;        /* runs once it is resumed */
    co_return 0;
}

static int test89_ext(bool hands_back) {
    test89_reset();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test89_ext_victim(sem, hands_back), killer_mods));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test89_got_token));

    if (hands_back) {
        ASSERT_FN(CHK_BOOL(test89_after));                  /* given back: it went on */
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_FINISHED));  /* and ended by itself */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the awaitable keeps it */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        co::external_sched_resume(test89_held);             /* its holder gives it back */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_after));                  /* it ran on, and ended by itself */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ----------------------------------- the driven root returns, a RETURN modif takes its caller */

enum test89_return_e { TEST89_KEEPS, TEST89_LETS_GO, TEST89_GAVE_BACK };

static int test89_parent_runs = 0;
static int test89_parent_got = 0;
static test89_return_e test89_return_mode = TEST89_KEEPS;

static co::task_t test89_ret_root(co::sem_p sem) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it, and it returns */
    test89_got_token = true;
    co_return 7;
}

static co::task_t test89_parent(co::pool_t *pool, co::sem_p sem, co::modif_pack_t killer_mods) {
    test89_parent_got = co_await co::add_modifs(pool, test89_ret_root(sem), killer_mods);
    test89_parent_runs++;       /* once, whoever resumes it */
    co_return 0;
}

static int test89_return(test89_return_e mode) {
    test89_reset();
    test89_parent_runs = 0;
    test89_parent_got = 0;
    test89_return_mode = mode;
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* the user's modifs go on the parent, and the root inherits them: the RETURN callback takes
    the root's caller (the parent), and the parent's RECLAIM callback is the one a kill asks */
    auto f = co::CO_MODIF_INHERIT_ON_CALL;
    co::modif_pack_t user_mods;
    user_mods.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(f,
            [](co::state_t *s) -> co::error_e {
                if (!s->caller_state)
                    return co::ERROR_OK;        /* the parent's own return: no caller to take */
                test89_held = s->caller_state;  /* it takes the caller: it resumes it later */
                if (test89_return_mode == TEST89_GAVE_BACK) {
                    co::external_sched_resume(test89_held);     /* or at once */
                    test89_held = nullptr;
                }
                return co::ERROR_SUSPENDED;
            }));
    user_mods.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (test89_return_mode == TEST89_KEEPS)
                    return co::ERROR_USER;      /* it keeps the caller */
                test89_held = nullptr;          /* it lets the caller go */
                return co::ERROR_OK;
            }));

    pool->sched(co::add_modifs(pool.get(), test89_parent(pool.get(), sem, killer_mods), user_mods));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_FINISHED));  /* the root ended by itself */
    ASSERT_FN(CHK_BOOL(test89_got_token));
    ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    if (mode == TEST89_KEEPS) {
        ASSERT_FN(CHK_BOOL(test89_parent_runs == 0));       /* the kill didn't queue the caller */
        co::external_sched_resume(test89_held);             /* the RETURN modif resumes it */
        ASSERT_FN(pool->run());
    }
    ASSERT_FN(CHK_BOOL(test89_parent_runs == 1));           /* exactly once */
    ASSERT_FN(CHK_BOOL(test89_parent_got == 7));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test89_copark(true, co::ERROR_OK);            /* the user's modif first */
    if (ret == 0)
        ret = test89_copark(false, co::ERROR_OK);           /* after it */
    if (ret == 0)
        ret = test89_copark(true, co::ERROR_USER);          /* the holder keeps it */
    if (ret == 0)
        ret = test89_copark(true, co::ERROR_SUSPENDED);     /* it keeps it, the other way */
    if (ret == 0)
        ret = test89_call(true);
    if (ret == 0)
        ret = test89_call(false);
    if (ret == 0)
        ret = test89_callee_return(true);
    if (ret == 0)
        ret = test89_callee_return(false);
    if (ret == 0)
        ret = test89_ext(false);
    if (ret == 0)
        ret = test89_ext(true);
    if (ret == 0)
        ret = test89_return(TEST89_KEEPS);
    if (ret == 0)
        ret = test89_return(TEST89_LETS_GO);
    if (ret == 0)
        ret = test89_return(TEST89_GAVE_BACK);
    print_test_result(test89_file, ret >= 0);
    return ret;
}
```

## How it was checked

All runs on Windows 10, MSVC 19.43, built through the tests makefile on scratch copies.

The regression test on the patched header, plain and with
`CXX_FLAGS='/EHsc /await:strict /std:c++20 /Zi /FS /I.. /fsanitize=address'`:

```
plain  [PASSED]: 018-035-reproduced_killer_drive_reclaim.cpp     exit 0
asan   [PASSED]: 018-035-reproduced_killer_drive_reclaim.cpp     exit 0
```

On the unpatched header, each case built alone, under ASan:

| case                               | on the unpatched header                                     |
|------------------------------------|-------------------------------------------------------------|
| 1, 2: a park, let go               | `Failed CHK_BOOL(test89_held == nullptr)`: never asked, the |
|                                    | kill ended the target under its holder                      |
| 3, 4: a park, kept (an error,      | `Failed CHK_BOOL(test89_threw)`: the kill ended a target    |
| `ERROR_SUSPENDED`)                 | its holder keeps                                            |
| 5: a taken call, let go            | `Failed CHK_BOOL(test89_held == nullptr)`                   |
| 6: a taken call, kept              | `Failed CHK_BOOL(test89_threw)`                             |
| 7: a callee's RETURN takes the     | `Failed CHK_BOOL(test89_held == nullptr)`: never asked, the |
| target, lets it go                 | kill ended the target                                       |
| 8: the same, kept                  | `Failed CHK_BOOL(test89_threw)`                             |
| 9: an external awaitable           | `Failed CHK_BOOL(test89_threw)`: the kill ended a target    |
|                                    | the awaitable holds                                         |
| 10: an awaitable that gives it     | `Failed CHK_BOOL(test89_after)`: the kill ended it instead  |
| straight back                      | of letting it go on                                         |
| 11: a RETURN modif keeps the       | `Failed CHK_BOOL(test89_parent_runs == 0)`: the kill queued |
| root's caller                      | the caller the modif holds                                  |
| 12: a RETURN modif lets the root's | passes (the caller is queued, as before)                    |
| caller go                          |                                                             |
| 13: a RETURN modif already gave    | the program ends: `std::terminate`, the caller in two       |
| the root's caller back             | queues at once                                              |

`repro/44-killer_drive_held.cpp` (type name updated), ASan, each mode on both headers:

| mode     | unpatched                                | patched                                  |
|----------|------------------------------------------|------------------------------------------|
| `ext`    | heap-use-after-free in                   | `PASS`: the kill throws and the target   |
|          | `external_sched_resume`                  | lives                                    |
| `copark` | heap-use-after-free in `destroy_state`   | heap-use-after-free in `destroy_state`   |
| `call`   | heap-use-after-free in                   | heap-use-after-free in                   |
|          | `external_sched_resume`                  | `external_sched_resume`                  |

The repro's `copark` and `call` holders were written before RECLAIM and have no RECLAIM callback,
so under the rule "silence means success" the kill takes the target from them, and their later use
is the same use-after-free as before. The regression test's holders answer RECLAIM, and pass.

**A hunt for new bugs**, run on the earlier shape of this diff (a private bit instead of
`STATE_EXTERNAL`, without the `completed` reset and the caller check): four reviewers and a sweep of
every program in `repro/` on both headers under ASan (44 programs, 79 runs per header). Every
finding was reproduced on the patched header before it counted. The only repro outcome that changed
is `44 ext`. The bugs it found in the diff, and where they went:

| found                                                 | where it went                            |
|-------------------------------------------------------|------------------------------------------|
| `completed` was never reset: a killer bound to a new  | `completed = false;` in `drive()`; the   |
| target skipped `end_chain()`, left the target parked  | hunt's program for it now behaves like   |
| and read freed memory on its next kill                | its control                              |
| a private bit outlived a hand-back: a target an       | `STATE_EXTERNAL`: the ready queue        |
| awaitable gave straight back to the ready queue made  | replaces it; case 8                      |
| the kill throw                                        |                                          |
| a RETURN modif that already gave the caller back had  | the caller check in `root_returned()`;   |
| it queued twice                                       | case 11                                  |
| the RECLAIM doc didn't say what `ERROR_SUSPENDED` and | the doc says both                        |
| the other values mean, nor that a returned root is    |                                          |
| asked after its EXIT callbacks                        |                                          |
| the test missed a kept call, a kept park by           | cases 4, 6, 10                           |
| `ERROR_SUSPENDED`, a RETURN modif that lets go        |                                          |
| a RETURN modif of a callee was never asked, and the   | the holder answers on the coroutine it   |
| callee's frame leaked (also in today's code)          | holds (the RECLAIM doc); let go, the     |
|                                                       | target resumes and frees the callee's    |
|                                                       | frame; cases 7, 8                        |
| `create_timeo()`'s timer kill can now throw out of    | open question 1                          |
| `pool_t::run()`                                       |                                          |

The whole suite (89 files: the 88 in `tests/`, the new test, and `012-002` with its new line), on
scratch copies of the patched header and of `tests/`: `make`, then `make` with the ASan flags above,
then with `/O2`:

```
plain  [PASSED: 89]/[FAILED: 0]
asan   [PASSED: 89]/[FAILED: 0]
/O2    [PASSED: 89]/[FAILED: 0]
```

The header these ran on is the diff above, drive loop included, except for the wording of the
`create_killer` doc paragraph, which was changed afterwards (a comment only).

**NOT-CHECKED.** The epoll and kqueue backends (they don't compile on this machine; no
backend-specific line changed), and GCC and Clang (not installed here).

## Interactions

- **FIX_02, applied.** This fix is built on it. A target that a modif already holds when a kill
  comes still makes the kill throw, as FIX_02 decided; `STATE_EXTERNAL` joins `STATE_LEFT` there.
- **FIX_45, modifs that last one call.** They compose: FIX_45 changes the flags line of
  `create_killer()` and the co_yield path, this fix the killer's drive and the docs around it.
  With FIX_45 the re-binding that `completed = false;` guards against no longer happens; the line
  stays harmless.
- **FIX_08, an `ERROR_SUSPENDED` is final.** Independent: a later error that undoes the killer's
  park only makes the target run on to its next stop, where it is asked again. Two user modifs
  parking one coroutine stay FIX_08's question.
- **FIX_19, ENTER after `external_sched_resume()`.** They compose: the ready queue replaces
  `STATE_EXTERNAL` either way.
- **FIX_30, modif type bounds.** Its text and test comment say `CO_MODIF_COUNT` is 12; after this
  fix it is 13.
- **FIX_18 and FIX_24.** They touch `killer_state_t` and rebase on `queue_caller(pool, caller)`;
  FIX_45 supersedes FIX_18.
- **README and docs.** The states and the modif events are listed there; they get the new ones when
  the fix is applied.

## Open questions

1. **`create_timeo()`. Its timer's kill now throws when it drives the task into a hold; the throw
   leaves the timer coroutine and comes out of `pool_t::run()`, and the timeout is lost. Today's
   code freed the task under its holder instead. Catching it belongs in `create_timeo()`, or a
   sentence in its doc.
2. **Asking outside a drive.** A target that a modif already holds when the kill comes still makes
   the kill throw without asking. Asking there too needs a way to tell a finished park from a
   coroutine that still runs its own suspension callbacks: a kill called from a later callback of
   the same event, after an earlier one parked it, would otherwise get it let go and end a frame
   whose `await_suspend` is still running.
3. **Decided by the user:** the name RECLAIM; `exec_reclaim_modifs()` public; a modif that holds
   a coroutine answers on that coroutine (the argument only says whose modifs run); silence means
   success; let go, the target goes on as colib would have (DESIGN.md, 3); a holder that keeps the
   target makes the kill throw; an external awaitable can't be asked (`STATE_EXTERNAL`); the
   killer has no RECLAIM callback.
