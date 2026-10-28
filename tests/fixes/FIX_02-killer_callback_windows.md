# Proposal: kill() ends only what colib owns (fix #02)

Status: `[APPLIED 2026-10-12 on the user's word]`. The diff below is in `colib.h` (uncommitted
when applied), on top of commit 9dfe29e. Its regression tests are `tests/018-030` to `018-034`;
the verification is in "How it was checked". The user removed the pseudo-code appendix at the end
of `colib.h` on 2026-10-12, as their own change; it is left out of the diff below.

Earlier shapes of this fix: a refusal, where the kill returned ERROR_GENERIC from the callback
windows (`repro/fix/02/FIX_02-prev-refusal.md`); then one where a callback could throw and the
awaiters undid the interrupted suspension. The user chose callbacks that can't throw instead.

## The bug

`killer_state_t::kill()` decided whether it was called from inside its target by reading the
target's state marker and nothing else. `STATE_RUNNING` threw `kill_self_t`, `STATE_WAITING_IO` and
`STATE_READY` had their own branches, and every other state fell into `default: break;`, which
destroyed the chain through `end_chain()`.

While a coroutine runs its own suspension callbacks, its marker is not `STATE_RUNNING`:

- `do_leave_modifs()` writes `STATE_LEFT` before the LEAVE callbacks run;
- `sem_awaiter_t::await_suspend()` linked the waiter into the semaphore's wait list
  (`STATE_WAITING_SEM`) before the WAIT_SEM callbacks ran;
- the WAIT_IO and YIELD callbacks run with `STATE_LEFT`;
- `drive()`, the kill's path for a target whose wait already did its work, unlinks the target
  (`STATE_LEFT`) and resumes it, so the UNWAIT callbacks of that resume run with `STATE_LEFT`.

A coroutine that a modif parked (`ERROR_SUSPENDED`) or that an external awaitable holds is
`STATE_LEFT` too, and something other than colib keeps a pointer to it.

The trigger is a modif callback of the target that calls the target's kill function: a deadline
check in a WAIT_SEM or LEAVE callback, for example. The kill then destroyed the frame whose
`await_suspend` was still running. The rest of the callback list received the freed state, the
awaiter wrote into the freed frame, and the semaphore later resumed freed memory. A parked target
was freed under the modif that owns it, and the owner's own `destroy_state()` then freed it again.
Each case was confirmed under ASan as a heap-use-after-free against the unpatched header (see "How
it was checked"; `repro/02-killer_window_uaf.md` holds the original report).

### The other bugs it closes

The same change closes these, each pinned by its own regression test:

- **#41, a kill during another teardown** (`repro/41`, test 018-031). A teardown (`sem_t::clear()`,
  `pool_t::clear()`, `destroy_state()` or another kill) runs the EXIT and UNWAIT callbacks of the
  frames it destroys; a kill called from one of them found its target among those frames and
  destroyed them itself, and the teardown then walked into the freed frames. Now the program ends
  inside that kill, before anything is destroyed twice.
- **#42, a kill from the task's own SCHED callbacks** (`repro/42`, test 018-032). The kill destroyed
  the task that `sched()` then queued. Now the program ends inside that kill, before the task is
  freed.
- **#46, a throw out of a resume's callbacks** (`repro/46`, test 018-033). A throw out of a woken
  waiter's UNWAIT_SEM or ENTER callbacks left before the unlocker was handed out, so the semaphore
  token was lost for good. A callback can't throw now, so the program ends inside the callback.
- **Three semaphore bugs from the old join point** (test 018-034): a LEAVE callback that signaled
  its own semaphore lost the wake-up; a WAIT_SEM callback that signaled and then refused spent the
  token on the waiter while the refusal resumed it; a WAIT_SEM callback that cleared its own
  semaphore destroyed the waiter inside its callback. The same test pins that `signal_all()` from
  those callbacks doesn't count the waiter itself.

#43, #44 and #45, found by the same analysis, involve no throw; they stay open (`repro/43..45`).

## The rule

A kill ends only a target that colib owns: one that sits in a colib queue (a semaphore's wait list,
an io wait, the ready queue). Any other target makes it throw `kill_not_colib_owned_t`: the target
is running (its body, or its own modif callbacks), or something outside colib holds it (a park, an
external awaitable).

A modif callback can't throw: `do_generic_modifs()` catches a throw out of one and calls
`std::terminate()`, so it ends the same way on every compiler (the callback's own locals are
destroyed first, and a terminate handler still sees the exception). So a kill from the target's
own callbacks ends the program before anything is freed, while a kill from a body, or from
outside, throws to its caller. One case is a callback of the kill itself: while
`drive()` runs the target up to its next wait, the target's callbacks get ERROR_GENERIC, the
documented answer for a callback of its own kill, and the drive ends the target at that wait.

One engine change makes the marker tell the truth: the semaphore adds its waiter to its wait list
only after the WAIT_SEM callbacks ran.

| `kill_fn()` called from                  | before             | now                              |
|------------------------------------------|--------------------|----------------------------------|
| the target's body                        | throws kill_self_t | throws kill_not_colib_owned_t    |
|                                          |                    | (renamed)                        |
| its LEAVE, WAIT_SEM, WAIT_IO or YIELD    | frees it: UAF      | the program ends, before         |
| callbacks                                |                    | anything is freed                |
| its callbacks while the kill's drive()   | frees it: UAF      | ERROR_GENERIC; it dies at its    |
| runs it                                  |                    | next wait                        |
| a call, a return, a co_yield             | frees a frame: UAF | the program ends                 |
| outside, the target in a sem, io or      | ends it            | unchanged                        |
| ready queue                              |                    |                                  |
| outside, the target parked or held       | frees it under its | throws kill_not_colib_owned_t to |
| externally                               | owner: UAF         | the caller of the kill           |

## Where the fix lands

| colib.h region                       | what changes                               | +adds/-dels |
|--------------------------------------|--------------------------------------------|-------------|
| `kill_not_colib_owned_t` (was        | renamed; thrown when colib doesn't         | +8 / -6     |
| `kill_self_t`)                       | currently own the target; `target` is an   |             |
|                                      | identity                                   |             |
| `modif_t` doc                        | a callback must not throw                  | +3 / -1     |
| `create_killer` doc                  | ERROR_GENERIC inside its drive;            | +6 / -3     |
|                                      | kill_not_colib_owned_t otherwise, and from |             |
|                                      | a callback the program ends                |             |
| `do_generic_modifs()`                | a throw out of a callback is caught and    | +9 / -2     |
|                                      | ends the program (std::terminate)          |             |
| `sem_internal_t::erase_waiter()`     | removed: no caller left                    | +0 / -5     |
| `sem_awaiter_t::await_suspend()`     | the waiter joins the list after its        | +3 / -2     |
|                                      | WAIT_SEM callbacks                         |             |
| `killer_state_t::kill()`             | every state named; LEFT throws, or returns | +12 / -2    |
|                                      | ERROR_GENERIC inside its own drive         |             |
| total                                |                                            | +41 / -21   |

## The change

The diff is against commit 9dfe29e, the tree it was built on. The reasoning, per hunk in diff
order:

1. **`kill_not_colib_owned_t`, formerly `kill_self_t`.** Renamed on the user's word: the exception
   covers every target colib doesn't currently own (a running one, and one parked outside colib),
   not only a coroutine killing itself. `target` is documented as an identity, because an
   uncaught throw unwinds and frees the frame it points to.
2. **`modif_t` doc.** The rule a modif author must follow: a callback must not throw.
3. **`create_killer` doc.** The outcomes a caller observes: ERROR_GENERIC for a callback of its own
   kill, now including the target's callbacks during its drive; `kill_not_colib_owned_t` for a
   target colib doesn't own, and from a callback the program ends.
4. **`do_generic_modifs()` comment.** States the no-throw rule next to the error rule.
5. **`do_generic_modifs()`, the call.** Every callback runs inside a `try` whose `catch (...)`
   calls `std::terminate()`, so a throw out of a callback ends the program where it happens. The
   user chose this over a `noexcept` wrapper: a `noexcept` boundary also terminates, but whether
   the stack unwinds first is up to the compiler, while the `catch` unwinds the callback's locals
   on every compiler and keeps the exception visible to a terminate handler. No suspension is ever
   left half done, and no pair (ENTER/LEAVE, WAIT/UNWAIT) is ever broken by a throw, so nothing
   needs undoing.
6. **`sem_internal_t::erase_waiter()`.** Removed: the waiter is no longer linked while its WAIT_SEM
   callbacks run, so a refusal has nothing to unlink.
7. **`sem_awaiter_t::await_suspend()`, the old link.** The waiter is no longer linked before its
   WAIT_SEM callbacks, and the refusal no longer unlinks it.
8. **`sem_awaiter_t::await_suspend()`, the new link.** The waiter joins the wait list only after its
   WAIT_SEM callbacks: until then it is running, and its marker says so (`STATE_LEFT`). A signal
   from its own LEAVE or WAIT_SEM callbacks leaves the count above zero, so after the callbacks the
   waiter takes that token at once (`await_ready()`) and resumes itself, with an unlocker. FIFO
   order holds: `signal()` leaves the count above zero only when no waiter is linked. Two latent
   bugs go with the old order: a LEAVE callback that signaled its own semaphore lost the wake-up
   (the count stayed at one while the waiter waited), and a WAIT_SEM callback that signaled and then
   refused left the waiter queued to be resumed twice.
9. **`killer_state_t::kill()`, `STATE_LEFT`.** The new case: a LEFT top throws
   `kill_not_colib_owned_t`. While `slot` is set (and `killing` is not), the killer's own `drive()`
   is running the target, so the call comes from a callback of its own kill: it returns
   ERROR_GENERIC, and the drive ends the target at its next wait. A RUNNING body keeps throwing,
   now under the new name.
10. **`killer_state_t::kill()`, the rest of the switch.** `STATE_WAITING_SEM` is named, the only
    state that falls through to `end_chain()` unchanged, and `default` returns ERROR_GENERIC for a
    state the kill doesn't know, as the user ruled for every switch.

```diff
diff --git a/colib.h b/colib.h
index 39276af..129862f 100644
--- a/colib.h
+++ b/colib.h
@@ -719,12 +719,14 @@ enum run_e : int32_t {
     RUN_STOPPED = -3, /*!< can be re-run (comes from force_stop) */
 };
 
-/*! Thrown by a kill called from inside its target: a coroutine can't kill itself */
-struct kill_self_t : public std::exception {
-    explicit kill_self_t(state_t *target) : target(target) {}
-    const char *what() const noexcept override { return "colib: a coroutine can't kill itself"; }
-
-    state_t *target;    /*!< the innermost frame of the target */
+/*! Thrown by a kill whose target colib doesn't currently own: the target is running, or parked
+ * outside colib. */
+struct kill_not_colib_owned_t : public std::exception {
+    explicit kill_not_colib_owned_t(state_t *target) : target(target) {}
+    const char *what() const noexcept override { return "colib: kill target not owned by colib"; }
+
+    state_t *target;    /*!< the innermost frame of the target, to compare: it may be gone once the
+                        throw unwound it */
 };
 
 
@@ -1188,7 +1190,9 @@ public:
  * this example, all the corutines that are on the same call-path(not sched-path) would have a
  * timer, such that the original call would not exist for more than X time units. (+/- the code
  * between awaits).
- * 
+ *
+ * A callback must not throw: a throw out of a callback ends the program (std::terminate).
+ *
  * You should use create_modif to create an object of this type. */
 struct modif_t {
     using variant_t = std::variant<
@@ -1606,9 +1610,12 @@ inline task<sem_p> create_sem(int64_t val);
  * wait, so nothing it received is lost. When the kill returns, the target is dead.
  *
  * The kill function returns ERROR_OK when it killed the target and ERROR_FINISHED when there was
- * nothing left to kill. Called from a callback of its own kill (EXIT, UNWAIT) it returns
- * ERROR_GENERIC and does nothing; called from inside its target it throws kill_self_t. The killed
- * coroutine's caller gets the default value from its await, and that task's get_err() returns e.
+ * nothing left to kill. Called from a callback of its own kill (EXIT, UNWAIT, or the target's
+ * callbacks while the kill runs it up to its next wait) it returns ERROR_GENERIC and does nothing.
+ * It throws kill_not_colib_owned_t when colib doesn't currently own the target: the target is
+ * running, or parked outside colib. Called from a modif callback, that throw ends the program. The
+ * killed coroutine's caller gets the default value from its await, and that task's get_err()
+ * returns e.
  * A killer can't be attached to a generator: the generator's first co_yield terminates the program
  * (a generator the target calls is not the target, its co_yield only returns to its caller).
  *
@@ -2725,7 +2732,8 @@ inline void inherit_modifs(state_t *state, modif_table_p parent_table, modif_fla
         state->modif_table = new_table;   
 }
 
-/* An error wins (the wait doesn't happen), then ERROR_SUSPENDED, then ERROR_OK */
+/* An error wins (the wait doesn't happen), then ERROR_SUSPENDED, then ERROR_OK. A callback can't
+throw: a throw out of one ends the program */
 template <modif_e cbk_id, typename ...Args>
 inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
     /* the return value of those is ignored, so one of them can't stop the others */
@@ -2744,7 +2752,13 @@ inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
         auto &cbks = modif_table->table[cbk_id];
         for (size_t i = 0; i < cbks.size(); i++) {
             auto &modif = reversed ? cbks[cbks.size() - 1 - i] : cbks[i];
-            error_e ret = std::get<cbk_id>(modif->cbk)(state, args...);
+            error_e ret;
+            try {
+                ret = std::get<cbk_id>(modif->cbk)(state, args...);
+            }
+            catch (...) {
+                std::terminate();   /* a callback must not throw */
+            }
             if (ret < 0 && !ignored_ret)
                 return ret;
             if (ret == ERROR_SUSPENDED)
@@ -4743,11 +4757,6 @@ struct sem_internal_t : public sem_t {
         return ERROR_OK;
     }
 
-    void erase_waiter(state_t *waiter) {
-        if (waiting_on_sem.has(waiter))
-            waiting_on_sem.unlink(waiter);
-    }
-
 protected:
     friend sem_awaiter_t;
     friend sem_t;
@@ -4831,10 +4840,8 @@ struct sem_awaiter_t {
         auto pool = sem->get_internal()->get_pool();
 
         do_leave_modifs(state);
-        sem->get_internal()->push_waiter(state);
         error_e err = do_wait_sem_modifs(state, sem);
         if (err != ERROR_OK) {
-            sem->get_internal()->erase_waiter(state);
             do_unwait_sem_modifs(state, sem);
             if (err == ERROR_SUSPENDED) {
                 return std::noop_coroutine();   /* parked by a modif */
@@ -4845,6 +4852,9 @@ struct sem_awaiter_t {
         }
 
         await_state = AWAITER_SUSPEND_LAST;
+        if (sem->get_internal()->await_ready())
+            return to_suspend;  /* signaled from its own callbacks: it takes the token now */
+        sem->get_internal()->push_waiter(state);    /* only now: until here it was running */
         return pool->get_internal()->next_task();
     }
 
@@ -6418,7 +6428,14 @@ struct killer_state_t {
 
         switch (top->get_state()) {
             case STATE_RUNNING:
-                throw kill_self_t(top);     /* called from inside the target */
+                throw kill_not_colib_owned_t(top);  /* called from its body */
+
+            case STATE_LEFT:
+                /* in no colib queue: it runs its own callbacks, or something else holds it (a
+                call, a park, an external awaitable), so ending it would free a frame in use */
+                if (slot)
+                    return ERROR_GENERIC;   /* its callbacks while drive() runs it: our own kill */
+                throw kill_not_colib_owned_t(top);
 
             case STATE_WAITING_IO: {
                 /* an io that completed before the stop is delivered: it did its work */
@@ -6436,8 +6453,11 @@ struct killer_state_t {
                 slot = top;
                 break;
 
-            default:
+            case STATE_WAITING_SEM:
                 break;
+
+            default:
+                return ERROR_GENERIC;       /* a state this kill doesn't know */
         }
         end_chain();
         return ERROR_OK;
```

## The regression test

Five files in `tests/`, landed with the change. Each fails on the unpatched header and passes on the
patched one (both runs are in "How it was checked"). A test that must end the program does it once,
through a `std::set_terminate` handler that checks where it ended and reports the result, the
pattern of 018-022 and 002-006.

| test      | what it pins                                                                   |
|-----------|--------------------------------------------------------------------------------|
| `018-030` | #02: a kill from the UNWAIT callbacks of a target its own kill drives returns  |
|           | ERROR_GENERIC and the drive ends the target (`ELWUELWUX`); a parked target     |
|           | throws to the kill's caller and its owner destroys it once (`ELWUX`); last, a  |
|           | kill from the target's own WAIT_SEM callback ends the program, nothing freed   |
| `018-031` | #41: a kill from an EXIT callback during a `sem_t::clear()` ends the program   |
|           | before the teardown destroyed anything                                         |
| `018-032` | #42: a kill from the task's own SCHED callback ends the program before the     |
|           | task is freed or queued                                                        |
| `018-033` | #46: a throw out of a woken waiter's UNWAIT_SEM callback ends the program      |
|           | inside the callback; its locals are destroyed first, and the terminate handler |
|           | sees the exception                                                             |
| `018-034` | the semaphore join point: a LEAVE signal wakes the waiter at once; a signal    |
|           | and a refusal leave the token in the semaphore; a `clear()` doesn't see the    |
|           | waiter; a `signal_all()` doesn't count it                                      |

The LEAVE, WAIT_IO and YIELD windows of #02 take the same path in `kill()` as the WAIT_SEM one
(`STATE_LEFT`, then a throw caught in `do_generic_modifs()`); a test file ends the program only
once, so they have no file of their own. `COLIB_ENABLE_DEBUG_CHECKS` is on in 018-030 and 018-034.

### `018-030-reproduced_killer_self_window.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test84 - Reproduced Bugs: a kill from the target's own wait callbacks freed the running frame
================================================================================================= */

/* Was an open bug: killer_state_t::kill() threw only for STATE_RUNNING, and destroyed the target
in any other state its switch did not name. While its own suspension callbacks run, the target is
not STATE_RUNNING: do_leave_modifs() sets STATE_LEFT before the LEAVE callbacks, and the semaphore
linked the waiter (STATE_WAITING_SEM) before the WAIT_SEM ones. A kill_fn() called from those
callbacks destroyed the frame whose await_suspend was still running (ASan heap-use-after-free, see
repro/02-killer_window_uaf.md). The same happened to a target that the kill itself was running up
to its next wait, and to a target parked by a modif, freed under the modif that owns it.

The kill now ends only a target that colib owns: one that sits in a colib queue. For a target out
of its body (STATE_LEFT) it throws kill_not_colib_owned_t. A modif callback can't throw, so a kill
from the target's own callbacks ends the program, before anything is freed; from elsewhere the
throw reaches the caller of the kill. Called from the target's callbacks while that kill runs the
target, the kill returns ERROR_GENERIC (a callback of its own kill), and the target dies at its
next wait.

The runs, in order: a kill from the UNWAIT callbacks while the kill runs the target; a kill of a
parked target; then a kill from the victim's WAIT_SEM callbacks, which must end the program inside
that kill, with the victim not destroyed. The recorded callbacks of the first two show every LEAVE
closed by an ENTER and every WAIT by an UNWAIT, and the debug checks agree. */

static const char *test84_file = "018-030-reproduced_killer_self_window.cpp";

static int test84_destruct_cnt = 0;
static bool test84_resumed = false;
static std::string test84_order;

struct test84_marker_t {
    ~test84_marker_t() { test84_destruct_cnt++; }
};

/* records the victim's callbacks: E/L enter and leave, W/U a semaphore wait and its end, R return,
X exit */
static co::modif_pack_t test84_recorder() {
    auto f = co::CO_MODIF_INHERIT_NONE;
    auto rec = [](const char *c) {
        return [c](auto...) { test84_order += c; return co::ERROR_OK; };
    };
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(f, rec("E")));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(f, rec("L")));
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(f, rec("W")));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(f, rec("U")));
    pack.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(f, rec("R")));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(f, rec("X")));
    return pack;
}

static void test84_reset() {
    test84_destruct_cnt = 0;
    test84_resumed = false;
    test84_order.clear();
}

static co::task_t test84_victim(co::sem_p block) {
    test84_marker_t marker;
    co_await block->wait();     /* never signaled */
    test84_resumed = true;      /* must never run */
    co_return co::ERROR_OK;
}

/* --------------------------------------------- the kill from its callbacks while it drives it */

static bool test84_got_token = false;
static std::vector<co::error_e> test84_inner;
static co::error_e test84_outer = co::ERROR_GENERIC;

static co::task_t test84_driven(co::sem_p sem, co::sem_p never) {
    test84_marker_t marker;
    co_await sem->wait();           /* signaled, then killed: the kill runs it from here */
    test84_got_token = true;        /* what its wait received is not lost */
    co_await never->wait();         /* it dies here */
    test84_resumed = true;          /* must never run */
    co_return co::ERROR_OK;
}

static co::task_t test84_driver(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();           /* the victim waits on sem */
    sem->signal();
    test84_outer = kill_fn();
    co_return 0;
}

static int test84_drive_kill() {
    test84_reset();
    test84_got_token = false;
    test84_inner.clear();
    test84_outer = co::ERROR_GENERIC;

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* the victim's UNWAIT callbacks call the kill that runs it */
    auto again_mods = co::modif_pack_t{};
    again_mods.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [kill_fn](co::state_t *, co::sem_t *) -> co::error_e {
                test84_inner.push_back(kill_fn());
                return co::ERROR_OK;
            }));

    auto victim = co::add_modifs(pool.get(), test84_driven(sem, never), killer_mods);
    co::add_modifs(pool.get(), victim, again_mods);
    co::add_modifs(pool.get(), victim, test84_recorder());

    pool->sched(COLIB_REGNAME(victim));
    pool->sched(test84_driver(sem, kill_fn));
    ASSERT_FN(pool->run());

    DBG("drive order: %s", test84_order.c_str());
    ASSERT_FN(CHK_BOOL(test84_outer == co::ERROR_OK));     /* the kill ended it */
    ASSERT_FN(CHK_BOOL(test84_inner.size() == 2));         /* its resume, and its park */
    for (auto err : test84_inner)
        ASSERT_FN(CHK_BOOL(err == co::ERROR_GENERIC));     /* a callback of its own kill */
    ASSERT_FN(CHK_BOOL(test84_got_token));
    ASSERT_FN(CHK_BOOL(!test84_resumed));
    ASSERT_FN(CHK_BOOL(test84_destruct_cnt == 1));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    ASSERT_FN(CHK_BOOL(test84_order == "ELWUELWUX"));
    return 0;
}

/* ------------------------------------------------------------ the kill of a parked target */

static co::state_t *test84_parked = nullptr;
static bool test84_kill_threw = false;

static co::task_t test84_owner(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();           /* the victim parks */
    try {
        kill_fn();
    }
    catch (co::kill_not_colib_owned_t &) {
        test84_kill_threw = true;   /* a modif holds it: the kill can't end it */
    }
    co::destroy_state(test84_parked);   /* its owner ends it, once */
    co_return 0;
}

static int test84_held_kill() {
    test84_reset();
    test84_parked = nullptr;
    test84_kill_threw = false;

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto park_mods = co::modif_pack_t{};
    park_mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *s, co::sem_t *) -> co::error_e {
                test84_parked = s;
                return co::ERROR_SUSPENDED;     /* parked: this modif owns it now */
            }));

    auto victim = co::add_modifs(pool.get(), test84_victim(block), killer_mods);
    co::add_modifs(pool.get(), victim, park_mods);
    co::add_modifs(pool.get(), victim, test84_recorder());

    pool->sched(COLIB_REGNAME(victim));
    pool->sched(test84_owner(kill_fn));
    ASSERT_FN(pool->run());

    DBG("held order: %s", test84_order.c_str());
    ASSERT_FN(CHK_BOOL(test84_kill_threw));
    ASSERT_FN(CHK_BOOL(!test84_resumed));
    ASSERT_FN(CHK_BOOL(test84_destruct_cnt == 1));         /* destroyed by its owner only */
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    ASSERT_FN(CHK_BOOL(test84_order == "ELWUX"));
    return 0;
}

/* --------------------------------------- the kill from its own WAIT_SEM callbacks: it must end
the program, inside that kill, before anything was freed (the last run: it doesn't come back) */

static bool test84_in_kill = false;

static int test84_self_kill() {
    test84_reset();
    std::set_terminate([] {
        bool ok = test84_in_kill && test84_destruct_cnt == 0 && !test84_resumed;
        print_test_result(test84_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto self_kill_mods = co::modif_pack_t{};
    self_kill_mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [kill_fn](co::state_t *, co::sem_t *) -> co::error_e {
                test84_in_kill = true;
                kill_fn();          /* the victim is executing: the program must end here */
                test84_in_kill = false;
                return co::ERROR_OK;
            }));

    auto victim = co::add_modifs(pool.get(), test84_victim(block), killer_mods);
    co::add_modifs(pool.get(), victim, self_kill_mods);

    pool->sched(COLIB_REGNAME(victim));
    pool->run();
    DBG("no terminate: the kill from the WAIT_SEM callback must end the program");
    return -1;
}

int main() {
    int ret = test84_drive_kill();
    if (ret == 0)
        ret = test84_held_kill();
    if (ret == 0)
        ret = test84_self_kill();     /* ends the program when it passes */
    print_test_result(test84_file, ret >= 0);
    return ret;
}
```

### `018-031-reproduced_killer_during_teardown.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test85 - Reproduced Bugs: a kill from a teardown's callbacks freed frames the teardown frees
================================================================================================= */

/* Was an open bug (repro/41): a teardown (sem_t::clear(), pool_t::clear(), destroy_state(), or
another kill) runs the EXIT callbacks of the frames it destroys. A kill called from one of them
found its target among those frames, out of every colib queue (STATE_LEFT), and destroyed them
itself; the teardown then walked into the freed frames (ASan heap-use-after-free). A kill now
throws for a target colib doesn't own, and a modif callback can't throw, so the program ends inside
that kill, before the teardown destroyed anything. Here an inner frame's EXIT callback calls its own
chain's kill while a sem_t::clear() tears the chain down. */

static const char *test85_file = "018-031-reproduced_killer_during_teardown.cpp";
static int test85_destroyed = 0;
static bool test85_in_kill = false;
static std::function<co::error_e(void)> test85_kill;

struct test85_marker_t {
    ~test85_marker_t() { test85_destroyed++; }
};

static co::task_t test85_inner(co::sem_p block) {
    test85_marker_t marker;
    co_await block->wait();         /* never signaled: the clear tears the chain down here */
    co_return 0;
}

static co::task_t test85_outer(co::sem_p block, co::modif_pack_t inner_mods) {
    test85_marker_t marker;
    auto inner = co::add_modifs(co_await co::get_pool(), test85_inner(block), inner_mods);
    co_await inner;
    co_return 0;
}

int test85_kill_during_teardown() {
    std::set_terminate([] {
        bool ok = test85_in_kill && test85_destroyed == 0;  /* inside the kill, nothing freed */
        print_test_result(test85_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    test85_kill = kill_fn;

    auto exit_mods = co::modif_pack_t{};
    exit_mods.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test85_in_kill = true;
                test85_kill();      /* its chain is being torn down: the program must end here */
                test85_in_kill = false;
                return co::ERROR_OK;
            }));

    pool->sched(co::add_modifs(pool.get(), test85_outer(block, exit_mods), killer_mods));
    ASSERT_FN(pool->run());         /* outer calls inner, inner waits on block */
    block->clear(0);                /* tears inner and outer down: inner's EXIT calls the kill */
    DBG("no terminate: the kill from the teardown's EXIT callback must end the program");
    return -1;
}

int main() {
    int ret = test85_kill_during_teardown();
    print_test_result(test85_file, ret >= 0);
    return ret;
}
```

### `018-032-reproduced_killer_sched_window.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>
#include <memory>

/* Test86 - Reproduced Bugs: a kill from the task's own SCHED callback freed the task being queued
================================================================================================= */

/* Was an open bug (repro/42): pool_internal_t::sched() runs the task's SCHED callbacks, then queues
the task. The killer's own SCHED callback binds it first, so a kill from a later SCHED callback of
the task found it out of every colib queue (STATE_LEFT) and destroyed it, and sched() then queued
the freed task (ASan heap-use-after-free). A kill now throws for a target colib doesn't own, and a
modif callback can't throw, so the program ends inside that kill, before the task is freed. The
task's frame is watched through a shared_ptr parameter: the frame's copy is the last owner, so the
weak_ptr expires exactly when the frame is destroyed (the task never runs). */

static const char *test86_file = "018-032-reproduced_killer_sched_window.cpp";
static bool test86_in_kill = false;
static std::weak_ptr<int> test86_frame;

static co::task_t test86_task(std::shared_ptr<int> keep) {
    (void)keep;
    co_return 0;
}

int test86_sched_window() {
    std::set_terminate([] {
        bool ok = test86_in_kill && !test86_frame.expired();    /* inside the kill, not freed */
        print_test_result(test86_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto keep = std::make_shared<int>(0);
    test86_frame = keep;
    auto t = co::add_modifs(pool.get(), test86_task(keep), killer_mods);
    keep.reset();                   /* the frame's copy is the last owner now */

    /* added after the killer's pack, so the killer's own SCHED callback has bound the task */
    auto sched_kill = co::modif_pack_t{};
    sched_kill.push_back(co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [kill_fn](co::state_t *) -> co::error_e {
                test86_in_kill = true;
                kill_fn();          /* the task is being scheduled: the program must end here */
                test86_in_kill = false;
                return co::ERROR_OK;
            }));
    co::add_modifs(pool.get(), t, sched_kill);

    pool->sched(t);
    DBG("no terminate: the kill from the task's own SCHED callback must end the program");
    return -1;
}

int main() {
    int ret = test86_sched_window();
    print_test_result(test86_file, ret >= 0);
    return ret;
}
```

### `018-033-reproduced_callback_throw_terminates.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test87 - Reproduced Bugs: a throw out of a resume's callbacks lost the semaphore token
================================================================================================= */

/* Was an open bug (repro/46): a waiter woken by signal() already holds its token, and a throw out
of one of its UNWAIT_SEM or ENTER callbacks on the resume left before sem_awaiter_t::await_resume()
handed out the unlocker, so the token was lost and a mutex-style semaphore blocked everyone else for
good. A modif callback can't throw now: do_generic_modifs() catches the throw and calls
std::terminate(), so the program ends inside the callback, before the coroutine sees anything. That
also pins how it ends, the same on every compiler: the callback's own locals are destroyed first,
and the terminate handler still sees the exception. */

static const char *test87_file = "018-033-reproduced_callback_throw_terminates.cpp";
static bool test87_local_destroyed = false;
static bool test87_body_caught = false;

struct test87_error_t : public std::exception {};

struct test87_local_t {
    ~test87_local_t() { test87_local_destroyed = true; }
};

static co::task_t test87_waiter(co::sem_p mtx) {
    try {
        co_await mtx->wait();       /* signaled: its UNWAIT_SEM callback throws on the resume */
    }
    catch (...) {
        test87_body_caught = true;  /* the bug's path: the throw reached the body, token lost */
    }
    co_return 0;
}

static co::task_t test87_signaler(co::sem_p mtx) {
    co_await co::yield();           /* the waiter waits on mtx */
    mtx->signal();
    co_return 0;
}

int test87_callback_throw() {
    std::set_terminate([] {
        bool saw = false;
        if (auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            }
            catch (const test87_error_t &) {
                saw = true;
            }
            catch (...) {
            }
        }
        bool ok = saw && test87_local_destroyed && !test87_body_caught;
        print_test_result(test87_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto mtx = co::create_sem(pool, 0);

    auto throw_mods = co::modif_pack_t{};
    throw_mods.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *) -> co::error_e {
                test87_local_t local;
                throw test87_error_t{};
            }));

    pool->sched(co::add_modifs(pool.get(), test87_waiter(mtx), throw_mods));
    pool->sched(test87_signaler(mtx));
    pool->run();
    DBG("no terminate: the throw out of the UNWAIT_SEM callback must end the program");
    return -1;
}

int main() {
    int ret = test87_callback_throw();
    print_test_result(test87_file, ret >= 0);
    return ret;
}
```

### `018-034-reproduced_sem_own_callbacks.cpp`

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

#include <memory>

/* Test88 - Reproduced Bugs: a semaphore waiter's own callbacks acting on its semaphore
================================================================================================= */

/* Were open bugs, found with FIX_02: sem_awaiter_t linked the waiter into the semaphore's wait list
before its WAIT_SEM callbacks ran, so the waiter counted as waiting while it was still running:
- a LEAVE callback that signaled the semaphore lost the wake-up: the count went to one before the
  waiter was linked, and the waiter then waited with a token sitting in the count;
- a WAIT_SEM callback that signaled and then refused the wait spent the token on the waiter (woken
  into the ready queue) while the refusal resumed it at once, so the token was gone and the waiter
  stayed queued as it ran on;
- a WAIT_SEM callback that cleared the semaphore destroyed the waiter inside its own callback (a
  use-after-free).
The waiter now joins the wait list only after its WAIT_SEM callbacks, as if those callbacks ran in
its body right before the wait: a signal from them leaves a token that it takes at once, a refusal
leaves that token in the semaphore, and a clear() doesn't see the waiter. Likewise signal_all()
from them wakes only the coroutines already waiting, so the waiter itself still waits. */

static const char *test88_file = "018-034-reproduced_sem_own_callbacks.cpp";

struct test88_marker_t {
    int *cnt;
    ~test88_marker_t() { (*cnt)++; }
};

/* ------------------------------------------------------ a LEAVE callback signals its semaphore */

static bool test88_a_done = false;

static co::task_t test88_a_waiter(co::sem_p sem) {
    co_await sem->wait();       /* its own LEAVE callback signals sem */
    test88_a_done = true;
    co_return 0;
}

static int test88_leave_signals() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto fired = std::make_shared<bool>(false);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(co::CO_MODIF_INHERIT_NONE,
            [s = sem.get(), fired](co::state_t *) -> co::error_e {
                if (!*fired) {          /* the LEAVE of its wait, not the one of its end */
                    *fired = true;
                    s->signal();
                }
                return co::ERROR_OK;
            }));
    pool->sched(co::add_modifs(pool.get(), test88_a_waiter(sem), mods));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_a_done));         /* it took the token: no lost wake-up */
    ASSERT_FN(CHK_BOOL(!sem->try_dec()));       /* and the token is spent */
    return 0;
}

/* --------------------------------------- a WAIT_SEM callback signals, then refuses the wait */

static int test88_b_runs = 0;

static co::task_t test88_b_waiter(co::sem_p sem) {
    co_await sem->wait();       /* refused by its own callback: it goes on at once */
    test88_b_runs++;
    co_return 0;
}

static int test88_signal_then_refuse() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *s) -> co::error_e {
                s->signal();
                return co::ERROR_USER;  /* refuses the wait */
            }));
    pool->sched(co::add_modifs(pool.get(), test88_b_waiter(sem), mods));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_b_runs == 1));
    ASSERT_FN(CHK_BOOL(sem->try_dec()));        /* the token stayed in the semaphore */
    return 0;
}

/* ------------------------------------------------- a WAIT_SEM callback clears its semaphore */

static int test88_c_destroyed = 0;
static bool test88_c_done = false;

static co::task_t test88_c_waiter(co::sem_p sem) {
    test88_marker_t marker{&test88_c_destroyed};
    co_await sem->wait();       /* its own WAIT_SEM callback clears sem */
    test88_c_done = true;
    co_return 0;
}

static co::task_t test88_c_signaler(co::sem_p sem) {
    co_await co::yield();       /* the waiter waits on sem */
    sem->signal();
    co_return 0;
}

static int test88_clear_from_wait() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *s) -> co::error_e {
                s->clear(0);
                return co::ERROR_OK;
            }));
    pool->sched(co::add_modifs(pool.get(), test88_c_waiter(sem), mods));
    pool->sched(test88_c_signaler(sem));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_c_done));         /* the clear didn't see it: it waited, then woke */
    ASSERT_FN(CHK_BOOL(test88_c_destroyed == 1));
    return 0;
}

/* -------------------------------------- a WAIT_SEM callback calls signal_all() on its semaphore */

static bool test88_d_done = false;
static bool test88_d_waited = false;

static co::task_t test88_d_waiter(co::sem_p sem) {
    co_await sem->wait();       /* its own WAIT_SEM callback calls signal_all() */
    test88_d_done = true;
    co_return 0;
}

static co::task_t test88_d_signaler(co::sem_p sem) {
    co_await co::yield();       /* a waiter woken by that signal_all() would run here */
    test88_d_waited = !test88_d_done;
    sem->signal();
    co_return 0;
}

static int test88_signal_all_from_wait() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto mods = co::modif_pack_t{};
    mods.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *s) -> co::error_e {
                s->signal_all();
                return co::ERROR_OK;
            }));
    pool->sched(co::add_modifs(pool.get(), test88_d_waiter(sem), mods));
    pool->sched(test88_d_signaler(sem));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test88_d_waited));       /* signal_all() didn't count it: it still waited */
    ASSERT_FN(CHK_BOOL(test88_d_done));
    return 0;
}

int main() {
    int ret = test88_leave_signals();
    if (ret == 0)
        ret = test88_signal_then_refuse();
    if (ret == 0)
        ret = test88_clear_from_wait();
    if (ret == 0)
        ret = test88_signal_all_from_wait();
    print_test_result(test88_file, ret >= 0);
    return ret;
}
```

## How it was checked

All runs on Windows 10, MSVC 19.43, built through the tests makefile on scratch copies (so the
real `colib.h` stays free for editing). The patched copy is byte for byte the working tree's
`colib.h` as applied.

**1. The unpatched header fails.** The base is `git show HEAD:colib.h` (9dfe29e), with the tests'
type name set back to `kill_self_t`, built with
`CXX_FLAGS='/EHsc /await:strict /std:c++20 /Zi /FS /I.. /fsanitize=address'`. Tests with several
cases were also built one case at a time:

| run                                   | on HEAD                                                  |
|---------------------------------------|----------------------------------------------------------|
| 018-030, the drive case               | heap-use-after-free, `#0 sem_awaiter_t::await_resume`    |
| 018-030, the parked case              | heap-use-after-free, `#0 destroy_state`                  |
| 018-030, the WAIT_SEM case            | heap-use-after-free, `#0 sem_awaiter_t::await_suspend`   |
| 018-031                               | heap-use-after-free, `#0 destroy_state`                  |
| 018-032                               | heap-use-after-free, `#0 state_list_t::link`             |
| 018-033                               | no terminate: the throw reached the coroutine's body     |
| 018-034, the LEAVE signal             | `[FAILED]`: the wake-up was lost                         |
| 018-034, the signal and refusal       | `[FAILED]`: the token was gone                           |
| 018-034, the `clear()`                | heap-use-after-free, `#0 sem_awaiter_t::await_suspend`   |
| 018-034, the `signal_all()`           | `[FAILED]`: the waiter was woken                         |

**2. The patched header passes.** `make <test>.exe` and a run of each, all exit 0:

```
[PASSED]: 018-030-reproduced_killer_self_window.cpp
[PASSED]: 018-031-reproduced_killer_during_teardown.cpp
[PASSED]: 018-032-reproduced_killer_sched_window.cpp
[PASSED]: 018-033-reproduced_callback_throw_terminates.cpp
[PASSED]: 018-034-reproduced_sem_own_callbacks.cpp
```

018-030, 018-031, 018-032 and 018-033 print their `[PASSED]` from the `std::set_terminate`
handler: each program ended where the test expects, with nothing freed, and 018-033's handler saw
the thrown exception after the callback's local was destroyed.

**3. The whole suite, plain and under ASan.** Scratch copies of the patched header and of
`tests/` (88 files, the five above included); `make`, then `make CXX_FLAGS='/EHsc /await:strict
/std:c++20 /Zi /FS /I.. /fsanitize=address'`:

```
plain  [PASSED: 88]/[FAILED: 0]
asan   [PASSED: 88]/[FAILED: 0]
```

Both runs print the io tests' usual failure logs (`FAILED request: The specified network name is
no longer available.`, `The I/O operation has been aborted ...`): those tests provoke the failures
on purpose, and the base prints the same lines.

**4. The follow-up repros against the patched header.** `repro/41`, `repro/42` and `repro/46`, with
the type renamed and a terminate reporter linked in, ASan builds: all 7 modes of 41, all 4 phases of
42 and all 5 cases of 46 end in `std::terminate`, with no ASan report before it.

**NOT-CHECKED.**

- The epoll and kqueue backends: they don't compile on this machine. No backend-specific line
  changed.
- GCC and Clang: not built here. The new code is a `try`/`catch (...)` around each callback call.

## Interactions

- **#03, `kill()` and `stop_io()` failures.** It adds a return to the `STATE_WAITING_IO` case,
  which this fix leaves as it was. They compose; either order.
- **#07, a woken waiter holds its semaphore.** It mints a hold in `_awake_one()` and takes it in
  `sem_awaiter_t::await_resume()`. This fix adds a second way into that path: a waiter that takes a
  token signaled by its own callbacks resumes on `AWAITER_SUSPEND_LAST` without `_awake_one()`. #07
  must mint its hold there too, as it does for its fast path.
- **#08, an ERROR_SUSPENDED is final.** It changes the same `do_generic_modifs()` loop, on the line
  after this fix's; it rebases on the `try`.
- **#16, a SCHED refusal leaks the task.** A different region (`pool_internal_t::sched`).
- **#18 and #24.** They quote or extend `kill_self_t` (built on HEAD) and follow the rename when
  they are rebased.
- **#19, #25, #26 and #27.** No textual overlap except #27, which rebases on
  `sem_awaiter_t::await_suspend()` (the `next_task()` line is the same).
- **#41, #42 and #46** are closed by this fix (see "The other bugs it closes"); the proposal for
  #46 was moved out of the repo root into `repro/fix/46/`. **#43, #44 and #45** stay open.

## Decisions

The contract decisions this fix made, ruled on by the user on 2026-10-12:

1. **A held target.** A target parked by a modif, inside an external awaitable, or a callee the
   killer lost through `rm_modifs` makes the kill throw `kill_not_colib_owned_t` to its caller,
   instead of being destroyed under whatever holds it. One visible case is `create_timeo` on such a
   task: the timer's kill throws, and `pool->run()` rethrows it.
2. **Callbacks can't throw.** Any exception out of a callback ends the program, a `std::bad_alloc`
   included, through a `catch (...)` that calls `std::terminate()`. So does a kill from a callback
   whose target colib doesn't own, for example a WAIT callback of one coroutine that kills another,
   parked one.
3. **The semaphore's join point.** A signal from the waiter's own LEAVE or WAIT_SEM callbacks hands
   it the token at once, and `signal_all()` or `clear()` called from those callbacks no longer counts
   or destroys it.
4. **The name.** `kill_self_t` became `kill_not_colib_owned_t`, with its `what()` text.
