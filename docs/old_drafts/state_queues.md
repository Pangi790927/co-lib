# Coroutine state and intrusive queues

This is the fourth step. It goes on top of `killer_diff.md` (sections 3 and 4) and both parts of
`reversed_modifs.md`. Part 2 of that one is required: the states only form a clean sequence once
running (ENTER..LEAVE) and waiting (WAIT..UNWAIT) no longer overlap. Nothing is applied to the
repo.

## What changes

1. **`state_e` in `state_t`:** where the coroutine is. The public, read-only way to read it is
   `get_state()`.

   | `state_e` | Meaning | Set by |
   |---|---|---|
   | `STATE_RUNNING` | executing | ENTER, and the pool popping it to run |
   | `STATE_LEFT` | suspended, not waiting on anything colib knows (a call in progress, a park, an external awaitable) | LEAVE, and leaving a queue |
   | `STATE_READY` | in its pool's ready queue | being linked into it |
   | `STATE_WAITING_SEM` | in a semaphore's wait list | being linked into it |
   | `STATE_WAITING_IO` | waiting on an io | `io_awaiter_t`, once the io is registered |

   The state belongs to each frame, not to the whole chain. A coroutine inside
   `co_await co::sleep_ms(1)` is `LEFT`, since it's calling; the innermost frame, `co::sleep`'s,
   is the one that's `WAITING_IO`.
2. **Intrusive queues:** `state_list_t`. The ready queue and every semaphore's wait list are
   linked through `prev`/`next` in `state_t` itself:
   - **O(1) operations:** `is_ready`, `remove_ready`, `replace_ready` and `erase_waiter` no longer
     search; each is O(1).
   - **No allocation:** the ready queue was a `std::deque` in the pool's memory. The wait list was
     a `std::list` in it too, plus a pool-allocated `shared_ptr` handle per waiter.
   - **At most one queue:** a coroutine is in at most one queue. Linking one that's already linked
     is a colib bug and terminates; that never happened in the suite.
   - **Destroyed means gone from the queue:** `~state_t()` unlinks the node, so destroying a queued
     coroutine can't leave a freed frame in the queue. Before this change, destroying a coroutine
     that sat in the ready queue crashed when the pool later popped it: a probe segfaults on the
     previous step and passes here.
   - **Order is the same as before:** a semaphore still wakes its oldest waiter first, and
     `clear()` still destroys oldest first.
3. **Private links:** `prev`, `next`, `list` and `state` are private in `state_t`. colib's
   internals reach them through `state_list_t` and `state_access_t` (both are friends). Users can
   still name those, since it's one header, the same way a `detail` namespace works. The existing
   public fields are unchanged.
4. **No pimpl pointer in `pool_t` and `sem_t`:** `pool_internal_t` now derives from `pool_t`, and
   `sem_internal_t` from `sem_t`. The public types stay where they are, with the API. The
   internal types are defined where they were, and `create_pool()`/`create_sem()` build the
   derived type.
   - **`get_internal()`** stays, but is now a `static_cast`. That's valid because every `pool_t` /
     `sem_t` is built by those two functions, and their constructors are protected.
   - **Destruction order:** `~pool_t()`'s `clear()` moves to `~pool_internal_t()`, since the base
     destructor runs too late. The pool's allocator stays in the base, so it's built before and
     freed after every member that uses it.
   - **A semaphore outliving its pool:** `sem_t`'s pointer was also the "my pool died" switch, and
     is now `pool = nullptr`. The wait list no longer holds pool memory, so nothing has to be freed
     early. `018-013` and `018-014` still pass.

**What stays:** `pool_t`'s `std::unique_ptr<allocator_memory_t> allocator_memory`.
`COLIB_ALLOCATOR_REPLACE_IMPL_2` (user-provided code) reaches it as `pool->allocator_memory->...`,
so turning it into a value would break that extension point.

**What the killer needed:** three one-line changes. The waiter handle is now the waiting `state_t *`,
so `erase_waiter(*it)` becomes `erase_waiter(it)`. The queue helpers kept their names.

### API changes to know about

- **`sem_waiter_handle_p` is now `state_t *`** (the waiter itself), no longer a `shared_ptr` to a
  list iterator. A WAIT_SEM callback that only takes the parameter still compiles; code that called
  `.get()` on it doesn't.
- **A `COLIB_OS_UNKNOWN` backend's `io_pool_t`** now receives the ready queue as `state_list_t &`
  instead of `std::deque<state_t *, ...> &`. It has `push_back`, `push_front`, `pop_front`,
  `front`, `size` and `empty`, but no iterators. The skeleton comment is updated (hunk 18).
- **`state_t::get_state()`** and **`co::state_e`** are new.

### Not in this step

- **The killer still infers** its target's situation from `entered`, `io_desc`, `sem` and
  `is_ready()`. It could read `top->get_state()` instead. That's a follow-up, kept out so this step
  doesn't touch the killer's logic.
- **The debug checks** still keep their own `dbg_check_coro_states` map. Replacing it with checked
  transitions on `state` is a follow-up too.

---

## Verification

Built through the scratch `tests/` makefile, on Windows (MSVC 19.43), with all four steps applied:

| | Result |
|---|---|
| Whole suite (with `012-002` below and the 13 killer/wait/clear tests rebuilt with `COLIB_ENABLE_DEBUG_CHECKS`) | all pass except `018-011` and `018-017`, which fail the same way on today's `colib.h` (`BUGS.md` #5, #6) |
| `012-002` (the new test below) | passes |
| A queued coroutine destroyed, then the pool run (a probe) | the previous step **segfaults**; this one passes |
| The "in two queues at once" `terminate` | never fired in the suite |
| `COLIB_ENABLE_MULTITHREAD_SCHED` | **doesn't compile on today's `colib.h` either**: `<mutex>` and `<atomic>` are never included (`BUGS.md` #7). With those two includes added, `001-001` builds and passes with this step |
| Linux (epoll) and kqueue | **not compiled**. Those backends only change the ready queue's type, but they need a build on Linux |

## The test: `tests/012-002-introspection_state.cpp`

It doesn't turn on `COLIB_ENABLE_DEBUG_CHECKS`. Its second half destroys a queued coroutine
directly with `co::destroy_state()`, which skips the LEAVE the debug checks require before an EXIT.
The point there is that the pool never resumes the freed frame.

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test54 - Coroutine introspection: state_t::get_state()
================================================================================================= */

/* A coroutine's state follows it: READY while queued, RUNNING while it runs, WAITING_SEM on a
semaphore, LEFT while a callee runs - co::sleep_ms() is a callee too, so a sleeping coroutine is
LEFT, and the innermost frame (co::sleep's) is the one WAITING_IO. And a coroutine destroyed while
it is queued leaves the queue by itself: the pool never resumes the freed frame. */

static std::vector<std::string> test54_log;
static co::state_t *test54_watched = nullptr;

static const char *test54_name(co::state_e s) {
    switch (s) {
        case co::STATE_LEFT:        return "LEFT";
        case co::STATE_RUNNING:     return "RUNNING";
        case co::STATE_READY:       return "READY";
        case co::STATE_WAITING_SEM: return "WAITING_SEM";
        case co::STATE_WAITING_IO:  return "WAITING_IO";
    }
    return "?";
}

static void test54_look(const char *when) {
    test54_log.push_back(std::string(when) + ": " + test54_name(test54_watched->get_state()));
}

static co::task_t test54_callee(co::sem_p sem) {
    co_await co::yield();               /* the caller is LEFT meanwhile */
    co_return 0;
}

static co::task_t test54_watched_task(co::sem_p sem) {
    test54_watched = co_await co::get_state();
    test54_look("self");
    co_await sem->wait();
    co_await co::sleep_ms(1);
    co_await test54_callee(sem);
    co_return 0;
}

static co::task_t test54_observer(co::sem_p sem) {
    test54_look("after its start");     /* it waits on sem */
    sem->signal();
    test54_look("signaled");            /* queued */
    co_await co::yield();               /* it runs and sleeps */
    test54_look("sleeping");            /* LEFT: it calls co::sleep_ms() */
    co_await co::sleep_ms(5);           /* it calls the callee, which yields */
    test54_look("calling");
    co_return 0;
}

static bool test54_victim_ran = false;

static co::task_t test54_victim() {
    test54_victim_ran = true;
    co_return 0;
}

int test54_states() {
    {
        auto pool = co::create_pool();
        auto sem = co::create_sem(pool, 0);
        pool->sched(test54_watched_task(sem));
        pool->sched(test54_observer(sem));
        ASSERT_FN(pool->run());

        for (auto &l : test54_log)
            DBG("%s", l.c_str());
        ASSERT_FN(CHK_BOOL(test54_log == std::vector<std::string>({
                "self: RUNNING", "after its start: WAITING_SEM", "signaled: READY",
                "sleeping: LEFT", "calling: LEFT"})));
    }
    {
        /* destroyed while queued: it leaves the queue, the pool never resumes it */
        auto pool = co::create_pool();
        auto victim = test54_victim();
        pool->sched(victim);
        co::state_t *s = &victim.h.promise().state;
        ASSERT_FN(CHK_BOOL(s->get_state() == co::STATE_READY));
        co::destroy_state(s);
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(!test54_victim_ran));
    }
    return 0;
}

int main() {
    int ret = test54_states();
    print_test_result("012-002-introspection_state.cpp", ret >= 0);
    return ret;
}
```

---

## The diff

The hunks are in file order, so together they make the whole patch (`--- a/colib.h` /
`+++ b/colib.h`, 41 hunks).

### 1. Declarations: `pool_t` and `sem_t` lose their pointer, `state_e`, `state_t`, `sem_waiter_handle_p`

The public classes keep their API. `~pool_t()` and `~sem_t()` go away, and their work moves to the derived types. `state_t` gets `get_state()` and the private node. The debug-check record of a semaphore wait now holds the waiting state.

```diff
--- a/colib.h
+++ b/colib.h
@@ -649,6 +649,7 @@ struct pool_t;
 struct modif_t;
 struct sem_t;
 struct state_t;
+struct state_list_t;
 
 /*! This is a private table that holds the modifications inside the corutine state */
 struct modif_table_t;
@@ -875,8 +876,6 @@ struct pool_t {
     pool_t &operator = (pool_t& sem) = delete;
     pool_t &operator = (pool_t&& sem) = delete;
 
-    ~pool_t() { clear(); }
-
     /*! Schedules the task with the modifications specified in v to be executed on the pool.
      * That is, it adds the task to the ready_queue.
      * 
@@ -941,10 +940,8 @@ protected:
     std::unique_ptr<allocator_memory_t> allocator_memory;
 
     friend inline std::shared_ptr<pool_t> create_pool();
+    friend struct pool_internal_t;
     pool_t();
-
-private:
-    std::unique_ptr<pool_internal_t> internal;
 };
 
 /*! This is a semaphore working on a pool. It can be awaited to decrement it's count and .signale()
@@ -970,9 +967,6 @@ struct sem_t {
     sem_t &operator = (sem_t& sem) = delete;
     sem_t &operator = (sem_t&& sem) = delete;
 
-    /* If the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire
-    call stack) */
-    ~sem_t();
 
     /*! This awaiter object returns an unlocker that has the `lock` member function doing nothing
      * and `unlock` function calling `signal` on the semaphore, meaning it can be used inside a
@@ -1009,7 +1003,7 @@ struct sem_t {
     /*! Again, beeter don't touch, same as pool. This is public only to ease the writing of the
      * implementation. @{ */
     sem_internal_t *get_internal();
-    void invalidate_self() { internal = nullptr; }
+    void invalidate_self();
     /*! @} */
 
 protected:
@@ -1017,11 +1011,9 @@ protected:
     friend inline T *alloc(pool_t *, Args&&...);
 
     friend inline sem_p create_sem(pool_t *pool, int64_t val);
+    friend struct sem_internal_t;
 
-    sem_t(pool_t *pool, int64_t val = 0);
-
-private:
-    std::unique_ptr<sem_internal_t> internal;
+    sem_t() {}
 };
 
 
@@ -1098,6 +1090,15 @@ COLIB_OS_UNKNOWN_IO_DESC
 
 #endif /* COLIB_OS_UNKNOWN */
 
+/*! Where a coroutine is, see state_t::get_state() */
+enum state_e : int32_t {
+    STATE_LEFT = 0,     /*!< suspended, not waiting on anything colib knows (a call, a park) */
+    STATE_RUNNING,      /*!< executing */
+    STATE_READY,        /*!< in its pool's ready queue */
+    STATE_WAITING_SEM,  /*!< in a semaphore's wait list */
+    STATE_WAITING_IO,   /*!< waiting on an io */
+};
+
 /*! Internal state of corutines that is independent of the return value of the corutine.
  * This structure, as explained above, is the common type for all coroutines from this library.
  * It also holds a user pointer user_ptr that can be used. This pointer can be useful when
@@ -1117,26 +1118,22 @@ struct state_t {
     std::shared_ptr<void> user_ptr;         /*!< this is a pointer that the user can use for whatever
                                             he feels like. This library will not touch this pointer */
 
-    ~state_t();                             /*!< Only used on debug */
+    state_e get_state() const { return state; }
+
+    ~state_t();                             /*!< Unlinks it from the queue it is in */
+
+private:
+    friend struct state_list_t;
+    friend struct state_access_t;
+
+    state_e state = STATE_LEFT;
+    state_t *prev = nullptr;
+    state_t *next = nullptr;
+    state_list_t *list = nullptr;           /* the queue it is linked in, if any */
 };
 
-/*! This is mostly internal. Internal pointer to an iterator inside the semaphore awaiter queue. It
- * will be given as a parameter inside the callback of a modifier, */
-using sem_waiter_handle_t = std::list<                          /* List with the semaphore waiters */
-    std::pair<
-        state_t *,                  /* The waiting corutine state */
-        std::shared_ptr<void>       /* Where the shared_ptr is actually stored */
-    >,
-    allocator_t<std::
-        pair<
-            state_t *,
-            std::shared_ptr<void>
-        >
-    >                               /* profiling shows the default is slow */
->::iterator;                        /* iterator in the respective list */
-using sem_waiter_handle_p =
-        std::shared_ptr<sem_waiter_handle_t>;   /* if this pointer is not available, the waiter was
-                                                   evicted from the waiters list */
+/*! The waiter in a semaphore's wait list, given as a parameter to the wait_sem modif callback */
+using sem_waiter_handle_p = state_t *;
             
 
 /*! Modifs, corutine modifications. Those modifications controll the way a corutine behaves when
@@ -1157,7 +1154,7 @@ struct modif_t {
         std::function<error_e(state_t *, io_desc_t&)>,  /* wait_io_cbk */
         std::function<error_e(state_t *, io_desc_t&)>,  /* unwait_io_cbk */
 
-         /* wait_sem_cbk - OBS: the std::shared_ptr<void> part can be ignored, it's internal */
+         /* wait_sem_cbk */
         std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,
 
          /* unwait_sem_cbk - No handle here, as the semaphore is no longer in the waiting list */
@@ -2282,7 +2279,7 @@ struct dbg_check_state_t {
     io_desc_t *io = nullptr; /* it's ok, to compare ptrs, else we would copy and incr the
                                 ref of a pointer on windows that would alter the behaviour */
     sem_t *sem = nullptr;
-    sem_waiter_handle_t *sem_it = nullptr;
+    state_t *sem_it = nullptr;
 };
 
 inline std::map<pool_t *,
```

### 2. `state_list_t`, `state_access_t`, and ENTER/LEAVE setting the state

The queue sits where `sem_wait_list_t` was, ahead of everything that uses it.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2312,9 +2309,70 @@ constexpr auto has(T&& data_struct, K&&
             != std::forward<T>(data_struct).end();
 }
 
-using sem_wait_list_t = std::list<std::pair<state_t *, std::shared_ptr<void>>,
-        allocator_t<std::pair<state_t *,std::shared_ptr<void>>>>;
-using sem_wait_list_it = sem_wait_list_t::iterator;
+/* A queue of coroutines, linked through their own state_t: no allocation, a coroutine is in at
+most one queue, and it leaves it when destroyed. `kind` is the state of the ones linked in it. */
+struct state_list_t {
+    state_list_t(state_e kind) : kind(kind) {}
+    state_list_t(const state_list_t &) = delete;
+    state_list_t &operator = (const state_list_t &) = delete;
+    ~state_list_t() { while (head) unlink(head); }
+
+    void push_back(state_t *s) { link(s, tail, nullptr); }
+    void push_front(state_t *s) { link(s, nullptr, head); }
+    state_t *front() { return head; }
+    state_t *pop_front() {
+        state_t *s = head;
+        if (s)
+            unlink(s);
+        return s;
+    }
+    bool has(state_t *s) const { return s->list == this; }
+    bool empty() const { return !head; }
+    size_t size() const { return cnt; }
+
+    void unlink(state_t *s) {
+        (s->prev ? s->prev->next : head) = s->next;
+        (s->next ? s->next->prev : tail) = s->prev;
+        s->prev = s->next = nullptr;
+        s->list = nullptr;
+        s->state = STATE_LEFT;
+        cnt--;
+    }
+
+    /* puts `with` in the place of `s` */
+    void replace(state_t *s, state_t *with) {
+        state_t *after = s->prev;
+        unlink(s);
+        link(with, after, after ? after->next : head);
+    }
+
+private:
+    void link(state_t *s, state_t *prev, state_t *next) {
+        if (s->list)
+            std::terminate();   /* in two queues at once: a colib bug */
+        s->prev = prev;
+        s->next = next;
+        (prev ? prev->next : head) = s;
+        (next ? next->prev : tail) = s;
+        s->list = this;
+        s->state = kind;
+        cnt++;
+    }
+
+    state_e kind;
+    state_t *head = nullptr;
+    state_t *tail = nullptr;
+    size_t cnt = 0;
+};
+
+/* colib's own access to the private parts of state_t */
+struct state_access_t {
+    static void set(state_t *s, state_e state) { s->state = state; }
+    static void unlink(state_t *s) {
+        if (s->list)
+            s->list->unlink(s);
+    }
+};
 
 /* Those are needed for destroy_state, internally and to call it */
 inline error_e do_leave_modifs(state_t *state);
@@ -2558,12 +2616,14 @@ inline error_e do_call_modifs(state_t *s
 inline error_e do_leave_modifs(state_t *state) {
     COLIB_DEBUG_TRACE("     LEAVE: %s state: %p", dbg_name(state->self).c_str(), state);
     COLIB_DEBUG_CHECK_LEAVE(state);
+    state_access_t::set(state, STATE_LEFT);
     return do_generic_modifs<CO_MODIF_LEAVE_CBK>(state);
 }
 
 inline error_e do_entry_modifs(state_t *state) {
     COLIB_DEBUG_TRACE("     ENTER: %s state: %p", dbg_name(state->self).c_str(), state);
     COLIB_DEBUG_CHECK_ENTER(state);
+    state_access_t::set(state, STATE_RUNNING);
     return do_generic_modifs<CO_MODIF_ENTER_CBK>(state);
 }
 
```

### 3. The io pools take the ready queue as a `state_list_t`

The same one-line change in every backend, and in the `COLIB_OS_UNKNOWN` skeleton comment.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2789,7 +2849,7 @@ inline state_t *task<T>::get_state() {
 #if COLIB_OS_UNIX
 
 struct io_pool_t {
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     : pool{pool}, ready_tasks{ready_tasks}
     {
 #ifndef KQUEUE_CLOEXEC
@@ -2860,7 +2920,7 @@ struct io_pool_t {
 private:
     int kq = -1;
     pool_t *pool = nullptr;
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
 };
 
 struct timer_pool_t {
@@ -2896,7 +2956,7 @@ struct io_pool_t {
         std::vector<waiter_t, allocator_t<waiter_t>> waiters;
     };
 
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     :       pool{pool},
             fd_data_slow(allocator_t<int>{pool}),
             ret_evs(allocator_t<int>{pool}),
@@ -3178,7 +3238,7 @@ private:
 
     std::vector<struct epoll_event, allocator_t<struct epoll_event>> ret_evs;
 
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
     int epoll_fd = -1;
 };
 
@@ -3297,7 +3357,7 @@ struct io_pool_t {
     using set_type = std::set<ptr_type, std::less<set_val_type>, allocator_t<set_val_type>>;
     using map_val_type = std::map<HANDLE, set_type>::value_type;
 
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     : pool{pool}, ready_tasks{ready_tasks}, handles{allocator_t<map_val_type>{pool}}
     {
         iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 1);
@@ -3617,7 +3677,7 @@ private:
     }
 
     pool_t *pool = nullptr;
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
     HANDLE iocp = nullptr;
 
     std::map<HANDLE, set_type, std::less<HANDLE>, allocator_t<map_val_type>> handles;
@@ -3726,7 +3786,7 @@ COLIB_OS_UNKNOWN_IMPLEMENTATION
 // Those two structs need implemented:
 
 struct io_pool_t {
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     : pool{pool}, ready_tasks{ready_tasks}
     {}
 
@@ -3759,7 +3819,7 @@ struct io_pool_t {
 
 private:
     pool_t *pool = nullptr;
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
 };
 
 struct timer_pool_t {
```

### 4. `pool_internal_t` derives from `pool_t`, and the queue operations

`next_task_state()` pops, then marks the popped state `RUNNING`. The placeholder check stays. `clear()` pops before it destroys.

```diff
--- a/colib.h
+++ b/colib.h
@@ -3784,15 +3844,18 @@ private:
 
 #endif /* COLIB_OS_UNKNOWN */
 
-struct pool_internal_t {
-    pool_internal_t(pool_t *_pool)
-    :   pool(_pool),
-        ready_tasks{allocator_t<state_t *>{_pool}},
-        io_pool{_pool, ready_tasks},
-        timer_pool(_pool, io_pool),
-        sem_pool{allocator_t<sem_t *>{_pool}}
+/* The pool itself: pool_t is its public part, create_pool() makes one of these */
+struct pool_internal_t : public pool_t {
+    pool_internal_t()
+    :   pool(this),
+        ready_tasks{STATE_READY},
+        io_pool{this, ready_tasks},
+        timer_pool(this, io_pool),
+        sem_pool{allocator_t<sem_t *>{this}}
     {}
 
+    ~pool_internal_t() { clear(); }
+
     template <typename T>
     void sched(task<T> task, const modif_pack_t& own_modifs, modif_table_p parent_table) {
         /* first we give our new task the pool */
@@ -3903,28 +3966,23 @@ struct pool_internal_t {
     }
 
     bool is_ready(state_t *state) {
-        return std::find(ready_tasks.begin(), ready_tasks.end(), state) != ready_tasks.end();
+        return ready_tasks.has(state);
     }
 
     /* puts `with` in the place of `state` in the ready queue */
     bool replace_ready(state_t *state, state_t *with) {
-        auto it = std::find(ready_tasks.begin(), ready_tasks.end(), state);
-        if (it == ready_tasks.end())
+        if (!ready_tasks.has(state))
             return false;
-        *it = with;
+        ready_tasks.replace(state, with);
         return true;
     }
 
     bool remove_ready(state_t *state) {
         COLIB_DEBUG_TRACE_SCOPE("state: %p", state);
-
-        auto it = std::remove(ready_tasks.begin(), ready_tasks.end(), state);
-        if (it != ready_tasks.end()) {
-            ready_tasks.erase(it, ready_tasks.end());
-            COLIB_DEBUG_TRACE("returned true");
-            return true;
-        }
-        return false;
+        if (!ready_tasks.has(state))
+            return false;
+        ready_tasks.unlink(state);
+        return true;
     }
 
     bool has_next_task_state() {
@@ -3969,10 +4027,10 @@ struct pool_internal_t {
         }
 
         if (!ready_tasks.empty()) {
-            auto ret = ready_tasks.front();
+            auto ret = ready_tasks.pop_front();
             if (!ret->self)
                 std::terminate();   /* a killer's placeholder: the scheduler ran inside a kill */
-            ready_tasks.pop_front();
+            state_access_t::set(ret, STATE_RUNNING);
             COLIB_DEBUG_TRACE("next_state: %p", ret);
             return ret;
         }
@@ -4034,7 +4092,7 @@ struct pool_internal_t {
 
 private:
     pool_t *pool;
-    std::deque<state_t *, allocator_t<state_t *>> ready_tasks;
+    state_list_t ready_tasks;
     io_pool_t io_pool;
     timer_pool_t timer_pool;
 
@@ -4139,23 +4197,22 @@ inline handle<void> final_awaiter_cleanu
 
 inline pool_t::pool_t() {
     allocator_memory = std::make_unique<allocator_memory_t>();
-    internal = std::make_unique<pool_internal_t>(this);
 }
 
 template <typename T>
 inline void pool_t::sched(task<T> task, const modif_pack_t& v) {
-    internal->sched(task, v, nullptr);
+    get_internal()->sched(task, v, nullptr);
 }
 
 #if COLIB_ENABLE_MULTITHREAD_SCHED
 template <typename T>
 inline void pool_t::thread_sched(task<T> task) {
-    internal->thread_sched(task);
+    get_internal()->thread_sched(task);
 }
 #endif /* COLIB_ENABLE_MULTITHREAD_SCHED */
 
 inline run_e pool_t::run() {
-    return internal->run();
+    return get_internal()->run();
 }
 
 /*  OBS: if clear is called, it is called from outside of the pool, else this is UB
@@ -4181,11 +4238,11 @@ inline error_e pool_t::clear() {
 }
 
 inline intptr_t pool_t::get_internal_handle() {
-    return internal->get_internal_handle();
+    return get_internal()->get_internal_handle();
 }
 
 inline pool_internal_t *pool_t::get_internal() {
-    return internal.get();
+    return static_cast<pool_internal_t *>(this);
 }
 
 inline error_e pool_t::stop_io(const io_desc_t& io_desc) {
@@ -4193,7 +4250,7 @@ inline error_e pool_t::stop_io(const io_
 }
 
 inline std::shared_ptr<pool_t> create_pool() {
-    return std::shared_ptr<pool_t>(new pool_t{});
+    return std::shared_ptr<pool_t>(new pool_internal_t{});
 }
 
 /* External Part
```

### 5. `io_awaiter_t`: `WAITING_IO`

```diff
--- a/colib.h
+++ b/colib.h
@@ -4385,6 +4442,7 @@ struct io_awaiter_t {
             do_entry_modifs(state);
             return h;
         }
+        state_access_t::set(state, STATE_WAITING_IO);
         triggered = true;
         return pool->get_internal()->next_task();
     }
```

### 6. `sem_internal_t` derives from `sem_t`, its wait list, and `sem_awaiter_t`

The awaiter links the waiter after LEAVE, so the state goes `LEFT` then `WAITING_SEM`.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4413,11 +4471,29 @@ private:
 /* Semaphore
 ------------------------------------------------------------------------------------------------- */
 
-struct sem_internal_t {
-    using sem_aloc = allocator_t<std::pair<std::coroutine_handle<void>, std::shared_ptr<void>>>;
+/* The semaphore itself: sem_t is its public part, create_sem() makes one of these */
+struct sem_internal_t : public sem_t {
+    sem_internal_t(pool_t *pool, int64_t val)
+    : pool(pool), val(val), waiting_on_sem(STATE_WAITING_SEM)
+    {
+        pool->get_internal()->add_sem(this);
+    }
+
+    /* If the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire
+    call stack) */
+    ~sem_internal_t() {
+        COLIB_DEBUG_TRACE_SCOPE("this: %p", this);
+        if (!pool) { /* we where already handled by the pool */
+            COLIB_DEBUG_TRACE("already handled");
+            return ;
+        }
+        clear(0);
+        pool->get_internal()->rm_sem(this);
+    }
 
-    sem_internal_t(pool_t *pool, int64_t val, sem_t *selfptr)
-    : pool(pool), val(val), waiting_on_sem(sem_aloc{pool}), selfptr(selfptr) {}
+    /* the pool died: nothing of it may be touched anymore */
+    void invalidate() { pool = nullptr; }
+    bool valid() const { return pool; }
 
     bool await_ready() {
         if (val > 0) {
@@ -4451,19 +4527,17 @@ struct sem_internal_t {
 
     error_e clear(int64_t val = 0) {
         COLIB_DEBUG_TRACE_SCOPE("sem_t::clear");
-        while (waiting_on_sem.size()) {
-            auto to_awake = waiting_on_sem.back();
-            waiting_on_sem.pop_back();
-            do_unwait_sem_modifs(to_awake.first, selfptr);
-            destroy_state(to_awake.first);
+        while (state_t *to_awake = waiting_on_sem.pop_front()) {
+            do_unwait_sem_modifs(to_awake, this);
+            destroy_state(to_awake);
         }
         this->val = val;
         return ERROR_OK;
     }
 
-    /* be carefull with this one */
-    void erase_waiter(sem_wait_list_it it) {
-        waiting_on_sem.erase(it);
+    void erase_waiter(state_t *waiter) {
+        if (waiting_on_sem.has(waiter))
+            waiting_on_sem.unlink(waiter);
     }
 
 protected:
@@ -4472,12 +4546,8 @@ protected:
     friend inline sem_p create_sem(pool_t *pool, int64_t val);
 
     sem_waiter_handle_p push_waiter(state_t *state) {
-        auto p = std::shared_ptr<sem_wait_list_it>(
-                alloc<sem_wait_list_it>(pool), dealloc_create<sem_wait_list_it>(pool),
-                allocator_t<int>{pool});
-        waiting_on_sem.push_front({state, p});
-        *p = waiting_on_sem.begin();
-        return p;
+        waiting_on_sem.push_back(state);
+        return state;
     }
 
 
@@ -4487,16 +4557,13 @@ protected:
 
 private:
     error_e _awake_one() {
-        auto to_awake = waiting_on_sem.back();
-        pool->get_internal()->push_ready(to_awake.first);
-        waiting_on_sem.pop_back();
+        pool->get_internal()->push_ready(waiting_on_sem.pop_front());
         return ERROR_OK;
     }
 
     pool_t *pool = nullptr;
     int64_t val;
-    sem_wait_list_t waiting_on_sem;
-    sem_t *selfptr = nullptr;
+    state_list_t waiting_on_sem;
 };
 
 inline error_e pool_internal_t::clear() {
@@ -4518,10 +4585,8 @@ inline error_e pool_internal_t::clear()
         sem_pool.erase(s);
     }
     COLIB_DEBUG_TRACE("Cleaning wait queue");
-    for (auto &state : ready_tasks) {
+    while (state_t *state = ready_tasks.pop_front())
         destroy_state(state);
-    }
-    ready_tasks.clear();
     return ERROR_OK;
 }
 
@@ -4545,12 +4610,12 @@ struct sem_awaiter_t {
         state = &to_suspend.promise().state;
 
         auto pool = sem->get_internal()->get_pool();
-        psem_it = sem->get_internal()->push_waiter(state);
 
         do_leave_modifs(state);
+        psem_it = sem->get_internal()->push_waiter(state);
         error_e err = do_wait_sem_modifs(state, sem, psem_it);
         if (err != ERROR_OK) {
-            sem->get_internal()->erase_waiter(*psem_it);
+            sem->get_internal()->erase_waiter(psem_it);
             do_unwait_sem_modifs(state, sem);
             if (err == ERROR_SUSPENDED)
                 return std::noop_coroutine();   /* parked by a modif */
@@ -4587,46 +4652,34 @@ struct sem_awaiter_t {
 
     state_t *state = nullptr;
     sem_t *sem = nullptr;
-    sem_waiter_handle_p psem_it;
+    sem_waiter_handle_p psem_it = nullptr;
     await_state_e await_state = AWAITER_NOT_CALLED;
 };
 
-inline sem_t::sem_t(pool_t *pool, int64_t val)
-: internal(std::make_unique<sem_internal_t>(pool, val, this))
-{
-    get_internal()->pool->get_internal()->add_sem(this);
-}
-
-inline sem_t::~sem_t() {
-    COLIB_DEBUG_TRACE_SCOPE("this: %p", this);
-    if (!internal) { /* we where already handled by the pool */
-        COLIB_DEBUG_TRACE("already handled");
-        return ;
-    }
-    get_internal()->clear(0);
-    get_internal()->pool->get_internal()->rm_sem(this);
-}
-
 inline sem_awaiter_t sem_t::wait() { return sem_awaiter_t(this); }
 /* A semaphore that outlived its pool was invalidated by pool_t::clear() and has no internals left:
 it does nothing and says so, as signal()'s comment promises, instead of dereferencing them. Only
 wait() is left unguarded, since every coroutine that could wait died in the same clear().
 2026-09-23 04:52 */
 inline error_e sem_t::signal(int64_t inc) {
-    return internal ? internal->signal(inc) : ERROR_GENERIC;
+    return get_internal()->valid() ? get_internal()->signal(inc) : ERROR_GENERIC;
 }
 inline error_e sem_t::signal_all() {
-    return internal ? internal->signal_all() : ERROR_GENERIC;
+    return get_internal()->valid() ? get_internal()->signal_all() : ERROR_GENERIC;
 }
 inline bool sem_t::try_dec() {
-    return internal ? internal->try_dec() : false;
+    return get_internal()->valid() ? get_internal()->try_dec() : false;
 }
 inline error_e sem_t::clear(int64_t val) {
-    return internal ? internal->clear(val) : ERROR_GENERIC;
+    return get_internal()->valid() ? get_internal()->clear(val) : ERROR_GENERIC;
 }
 
 inline sem_internal_t *sem_t::get_internal() {
-    return internal.get();
+    return static_cast<sem_internal_t *>(this);
+}
+
+inline void sem_t::invalidate_self() {
+    get_internal()->invalidate();
 }
 
 inline sem_p create_sem(pool_t *pool, int64_t val) {
@@ -4634,7 +4687,7 @@ inline sem_p create_sem(pool_t *pool, in
 	because they may survive outside of the pool, and we would not have how
 	to de-allocate them anymore, so semaphores need to be allocated with the
 	global allocator */
-    return std::shared_ptr<sem_t>(new sem_t(pool, val));
+    return std::shared_ptr<sem_t>(new sem_internal_t(pool, val));
 }
 inline sem_p create_sem(pool_p pool, int64_t val) {
     return create_sem(pool.get(), val);
```

### 7. The killer

The waiter handle is the waiting state.

```diff
--- a/colib.h
+++ b/colib.h
@@ -6033,7 +6086,7 @@ struct killer_state_t : std::enable_shar
     std::stack<state_t *> call_stack;   /* top() is the innermost frame */
     io_desc_t *io_desc = nullptr;       /* the io the top waits on */
     sem_t *sem = nullptr;               /* the semaphore the top waits on */
-    sem_waiter_handle_p it;             /* the top's place in the semaphore's wait list */
+    sem_waiter_handle_p it = nullptr;   /* the top's place in the semaphore's wait list */
 
     bool entered = false;   /* the top is executing, or was scheduled and never started */
     bool dying = false;     /* the next wait parks */
@@ -6099,7 +6152,7 @@ inline error_e killer_state_t::kill() {
 
     if (sem) {
         /* waiting on a semaphore, no token was taken */
-        sem->get_internal()->erase_waiter(*it);
+        sem->get_internal()->erase_waiter(it);
         drop(top);
         return ERROR_OK;
     }
@@ -6282,7 +6335,7 @@ inline std::pair<modif_pack_t, std::func
     pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
         [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
             COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p it-ptr: %p",
-                    kstate.get(), s, sem, it.get());
+                    kstate.get(), s, sem, it);
             if (kstate->dying)
                 return kstate->park(s);
             kstate->sem = sem;
```

### 8. `~state_t()` unlinks, and the debug check

```diff
--- a/colib.h
+++ b/colib.h
@@ -6533,13 +6586,16 @@ inline dbg_string_t dbg_name(void *v) {
 
 inline state_t::~state_t() {
     COLIB_DEBUG_TRACE("ENDING STATE: %p", this);
+    state_access_t::unlink(this);
     if (this->self)
         dbg_names.erase(this->self.address());
 }
 
 #else /* COLIB_ENABLE_DEBUG_NAMES */
 
-inline state_t::~state_t() {}
+inline state_t::~state_t() {
+    state_access_t::unlink(this);
+}
 
 template <typename ...Args>
 inline void *dbg_register_name(void *addr, const char *, Args&&... args) { return addr; }
@@ -6731,7 +6787,7 @@ inline void dbg_check_modif_wait_sem(sta
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
     dbg_state.sem = sem;
-    dbg_state.sem_it = it.get();
+    dbg_state.sem_it = it;
 }
 
 inline void dbg_check_modif_unwait_sem(state_t *s, sem_t *sem) {
```
