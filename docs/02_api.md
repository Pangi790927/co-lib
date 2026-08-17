# 02. API Reference

> Mirrors `colib.h`'s `HEADER` section (~line 376-2176) declaration-for-declaration, in the same
> order the header uses. Doc comments are reproduced **verbatim** from `colib.h` (including its own
> wording/spelling) - this file moves them here, it doesn't rewrite them. Has an ongoing maintenance
> obligation - see this directory's `CLAUDE.md`, "Keeping things in sync": whenever a change to
> `colib.h` touches a declaration's signature or doc comment, update the matching entry here in the
> same pass.
>
> Last verified: 2026-08-15, commit `41fa8ac` (+ uncommitted local changes on top of it) - see
> `progress.md`.

---

## Config Macros

### `COLIB_OS_LINUX`
> If true, the library provided Linux implementation will be used to implement the IO pool and
> timers.

### `COLIB_OS_WINDOWS`
> If true, the library provided Windows implementation will be used to implement the IO pool and
> timers.

### `COLIB_OS_UNIX`
> If true, the library provided UNIX implementation will be used to implement the IO pool and
> timers.

### `COLIB_OS_UNKNOWN`
> If true, the user provided implementation will be used to implement the IO pool and timers. In
> this case COLIB_OS_UNKNOWN_IO_DESC and COLIB_OS_UNKNOWN_IMPLEMENTATION must be defined.

### `COLIB_OS_UNKNOWN_IMPLEMENTATION`
> See COLIB_OS_UNKNOWN

### `COLIB_OS_UNKNOWN_IO_DESC`
> See COLIB_OS_UNKNOWN

### `COLIB_VERSION`
```cpp
#define COLIB_VERSION 0,1,0
```
> The version is formated as MAJOR,MINOR,DETAIL. Only when major and/or minor change a new branch
> will be created from now on.
>
> - `MAJOR` - breaking changes to the interface: functions are deleted, functionality is added or
>   removed to already existing functions, etc.
> - `MINOR` - changes to the interface: functions are added, functionality is added to new
>   functons, additional default parameters are added, etc.
> - `DETAIL` - fixes, implementation fixes, comments, etc. Changes that won't change the way you
>   use this library

### `COLIB_MAX_TIMER_POOL_SIZE`
> The maximum number of concurrent sleeps. (Only for Linux)

Default: `64`.

### `COLIB_MAX_FAST_FD_CACHE`
> The maximum file descriptor number to hold in a fast access path, the rest will be held in a map.
> Only for Linux, on Windows all are held in a map.

Default: `1024`.

### `COLIB_ENABLE_MULTITHREAD_SCHED`
> If true, pool_t::thread_sched can be used from another thread to schedule a coroutine in the same
> way pool_t::sched is used, except, modifications can't be added from that schedule point.

Default: `false`.

### `COLIB_ENABLE_LOGGING`
> If true, coroutines will use log_str to print/log error strings.

Default: `true`.

### `COLIB_DEBUG(fmt, ...)`
> This is a debug macro that can be used to print, using log_file, a formated string. This macro is
> used internally to log diverse errors and warnings. Does nothing if COLIB_ENABLE_LOGGING is false.
>
> - `fmt` The printf format of the formated string
> - `...` The rest of the parameters

### `COLIB_ENABLE_DEBUG_NAMES`
> If true you can also define COLIB_REGNAME and use it to register a coroutine's name (a
> colib::task<T>, std::coroutine_handle or void *)

Default: `false`.

### `COLIB_ENABLE_DEBUG_CHECKS`
> If true, the library will do additional checks on the internal state and on given arguments. If
> any assertion fails, the library will abort.

Default: `false`. *(In `colib.h` this doc comment is itself mistitled `@def COLIB_ENABLE_DEBUG_NAMES`
- a copy/paste artifact from the macro above it - reproduced here as-is per this chapter's
verbatim policy; the macro it actually documents is `COLIB_ENABLE_DEBUG_CHECKS`.)*

### `COLIB_ENABLE_DEBUG_TRACE_ALL`
> If true, all coroutines will have trace their execution as if they had a modification that would
> print on the given modif points

Default: `false`.

### `COLIB_DISABLE_ALLOCATOR`
> If true, the allocator will be disabled and malloc will be used instead.

Default: `false`.

### `COLIB_ALLOCATOR_SCALE`
> Scales all memory buckets inside the allocator.

Default: `16`.

### `COLIB_ALLOCATOR_REPLACE`
> If true, COLIB_ALLOCATOR_REPLACE_IMPL_1 and COLIB_ALLOCATOR_REPLACE_IMPL_2 must be defined. As a
> result, the allocator will be replaced with the provided implementation.

Default: `false`.

### `COLIB_WIN_ENABLE_SLEEP_AWAKE`
> Sets the last parameter of the function SetWaitableTimer to true or false, depending on the
> value. This define is used for timers on Windows.

Default: `FALSE`.

### `COLIB_REGNAME`
> If COLIB_ENABLE_DEBUG_NAMES is true you use COLIB_REGNAME to register coroutines or task names.
> TODO: This library uses it internaly if COLIB_ENABLE_DEBUG_NAMES is true. *)

### `COLIB_LOG_FUNCTION`
> You can define this to some other expression that is compatible with
> `std::function<int(const dbg_string_t&)>` and as a result, that function would be used by the
> library when logging.

---

## Core Types & Enums

### `task<T>` (forward declaration)
```cpp
template<typename T>
struct task;
```
> Coroutine return type, this is the result of creating a coroutine, you can await it and also pass
> it around (practically holds a std::coroutine_handle and can be awaited call the coro and to get
> the return value)
>
> - `T` the return type of the respective coroutine.

### `modif_table_t` (forward declaration)
> This is a private table that holds the modifications inside the corutine state

### Smart pointer aliases
```cpp
using sem_p = std::shared_ptr<sem_t>;
using pool_p = std::shared_ptr<pool_t>;
using modif_p = std::shared_ptr<modif_t>;
using modif_table_p = std::shared_ptr<modif_table_t>;
using modif_pack_t = std::vector<modif_p>;
```
- `sem_p`: *Smart pointer handle to the semaphore object. When destroyed, the semaphore is
  destroyed. It is undefined behaviour to destroy the semaphore while a coroutine is waiting on it.*
- `pool_p`: *Smart pointer handle to the pool object. When destroyed, the pool is also destroyed.
  You must keep the pool alive while corutines are running and while semaphore exist.*
- `modif_p`: *Smart pointer handle to a single modification. Ownership is transfered to the
  corutine when attached.*
- `modif_table_p`: *A pointer used internally to hold the modifications of a coroutine.*
- `modif_pack_t`: *A vector consisting of modif_p-s. Functions receive those packs togheter to ease
  use.*

### `error_e`
```cpp
enum error_e : int32_t {
    ERROR_YIELDED =  1, /*!< not really an error, but used to signal that the coro yielded */
    ERROR_OK      =  0,
    ERROR_GENERIC = -1, /*!< generic error, can use log_str to find the error, or sometimes errno */
    ERROR_TIMEO   = -2, /*!< the error comes from a modif, namely a timeout */
    ERROR_WAKEUP  = -3, /*!< the error comes from force awaking the awaiter */
    ERROR_USER    = -4, /*!< the error comes from a modif, namely an user defined modif, users can
                        use this if they wish to return from modif cbks */
    ERROR_DEPEND  = -5, /*!< the error comes from a depend modif, i.e. depended function failed */
};
```
> Most of the functions from this library return this error type. Warnings or non-errors are
> positive, while errors are negative.

### `run_e`
```cpp
enum run_e : int32_t {
    RUN_OK = 0,       /*!< when the pool stopped because it ran out of things to do */
    RUN_ERRORED = -1, /*!< comes from epoll/iocp or os api errors */
    RUN_ABORTED = -2, /*!< if a corutine had some sort of internal error */
    RUN_STOPPED = -3, /*!< can be re-run (comes from force_stop) */
};
```
> Return type of pool_t::run event loop.

### `modif_e`
```cpp
enum modif_e : int32_t {
    CO_MODIF_CALL_CBK = 0,
    CO_MODIF_SCHED_CBK,
    CO_MODIF_EXIT_CBK,
    CO_MODIF_LEAVE_CBK,
    CO_MODIF_ENTER_CBK,
    CO_MODIF_WAIT_IO_CBK,
    CO_MODIF_UNWAIT_IO_CBK,
    CO_MODIF_WAIT_SEM_CBK,
    CO_MODIF_UNWAIT_SEM_CBK,
    CO_MODIF_COUNT,
};
```
> This is the modification type of the modification and it describes the place that this
> modification should be called from.

Per-value comments, verbatim:

- `CO_MODIF_CALL_CBK`: *This is called when a task is called (on the task), via 'co_await task'*
- `CO_MODIF_SCHED_CBK`: *This is called on the corutine that is scheduled. Other mods are inherited
  before this is called. The return value of the callback is ignored.*
- `CO_MODIF_EXIT_CBK`: *This is called on a corutine right before it is destroyed. The return value
  of the callback is ignored*
- `CO_MODIF_LEAVE_CBK`: *This is called on each suspended corutine. The return value of the
  callback is ignored*
- `CO_MODIF_ENTER_CBK`: *This is called on a resume. The return value of the callback is ignored*
- `CO_MODIF_WAIT_IO_CBK`: *This is called when a corutine is waiting for an IO (after the leave
  cbk). If the return value is not ERROR_OK, then the wait is aborted.*
  > **Flagged - see `tests/BUGS.md` #1.** This comment's claimed ordering ("after the leave cbk")
  > is the specific thing under dispute there: the actual, verified order is `WAIT_IO` *before*
  > `LEAVE`, not after. Reproduced here verbatim per this chapter's policy, not silently corrected -
  > treat the comment's ordering claim as unresolved/incorrect until `BUGS.md` #1 is closed one way
  > or the other.
- `CO_MODIF_UNWAIT_IO_CBK`: *This is called when the io is done and the corutine that awaited it is
  resumed*
- `CO_MODIF_WAIT_SEM_CBK`: *This is similar to wait_io, but on a semaphore* (inherits the same
  ordering flag as `CO_MODIF_WAIT_IO_CBK` above, per `BUGS.md` #1)
- `CO_MODIF_UNWAIT_SEM_CBK`: *This is similar to unwait_io, but on a semaphore*

### `modif_flags_e`
```cpp
enum modif_flags_e : int32_t {
    CO_MODIF_INHERIT_NONE = 0x0,    /*!< Not inherited */
    CO_MODIF_INHERIT_ON_CALL = 0x1, /*!< Inherited on call */
    CO_MODIF_INHERIT_ON_SCHED = 0x2,/*!< Inherited on sched */
};
```
> Type of modifier inheriting policy. (Can be or-ed togheter)

### `task_t`
```cpp
using task_t = task<int>;
```
> all the internal tasks return this, namely error_e but casted to int (a lot of my old code
> depends on this and I also find it in theme, as all the linux functions that I use tend to return
> ints)

### Forward-declared internal structures
```cpp
struct yield_awaiter_t;
struct sem_awaiter_t;
struct pool_internal_t;
struct sem_internal_t;
struct get_state_awaiter_t;
struct get_pool_awaiter_t;
struct allocator_memory_t;
struct io_desc_t;

template <typename T>
struct sched_awaiter_t;
```
No doc comment in `colib.h` (`///@cond`-wrapped, excluded from Doxygen output).

---

## Allocator

### `allocator_bucket_sizes`
```cpp
constexpr std::pair<int, int> allocator_bucket_sizes[] = {
    {32,    COLIB_ALLOCATOR_SCALE * 1024},
    {64,    COLIB_ALLOCATOR_SCALE * 512},
    {128,   COLIB_ALLOCATOR_SCALE * 256},
    {512,   COLIB_ALLOCATOR_SCALE * 64},
    {2048,  COLIB_ALLOCATOR_SCALE * 16}
};
```
> Array of {element size, bucket size} for the custom allocator

### `allocator_t<T>`
> Custom allocator for this library. Holds a small portion of memory for fast use/reuse by the
> library.
>
> There are two things that don't need custom allocating: lowspeed stuff and the corutine promise.
> It makes no sense to allocate the corutine promise because:
>
> 1. It is a user defined type so the promise is not going to fit well in our allocator
> 2. I expect it be allocated rarelly
> 3. It would be a pain to allocate them
> 4. a malloc now and then is not such a big deal
>
> For the rest of the code those 4 are not generaly true.
>
> The idea of this allocator is that allocated objects are actually small in size and get
> allocated/deallocated fast. The assumption is that there is a direct corellation with the number
> num_fds+call_depth, and most of the time there aren't that many file descriptors or depth to
> calls (at least from my experience).
>
> So what it does is this: it has 5 bucket levels: 32, 64, 128, 512, 2048 (bytes), with a number of
> maximum allocations 16384, 8192, 4096, 1024 and 256 of each and a stack coresponding to each of
> them, that is initiated at the start to contain every index from 0 to the max amount of slots in
> each bucket. When an allocation occours, an index is poped from the lowest fitting bucket, and
> that slot is returned. On a free, if the memory is from a bucket, the index is calculated and
> pushed back in that bucket's stack else the normal free is used.
>
> Those bucket levels can be configured, by changing the array bellow (Obs: The buckets must be in
> ascending size order).
>
> The improvement from malloc, as a guess, (I used a profiler to see if it does something) is that
>
> 1. the memory is already there, no need to fetch it if not available and no need to check if it
>    is available
> 2. there are no locks. since we already assume that the code that does the allocations is
>    protected either by the absence of multithreading or by the pool's lock
> 3. there is no fragmentation, at least until the memory runs out, the pool's memory is localized

### `deallocator_t<T>`
> This class is part of the allocator implementation and is here only because a definition needs it
> as a full type

---

## `pool_t`

> A pool is the shared state between all coroutines. This object holds the epoll/iocp handler,
> timers, queues, etc. Each corutine has a pointer to this object. You can see this object as an
> instance or proxy of epoll/iocp.
>
> OBS: having a task be called on two pools is undefined behaviour.
> OBS: You should use the pointer made by create_pool, you shoudln't construct this yourself.

### `pool_t::sched`
```cpp
template <typename T>
void sched(task<T> task, const modif_pack_t& v = {}); /* ignores return type */
```
> Schedules the task with the modifications specified in v to be executed on the pool. That is, it
> adds the task to the ready_queue.
>
> - `task` The task of the coroutine to be added
> - `v` The modifications to be added to the task

### `pool_t::thread_sched` *(only when `COLIB_ENABLE_MULTITHREAD_SCHED`)*
```cpp
template <typename T>
void thread_sched(task<T> task);
```
> Similar to pool_t::sched, can't add modifications with it. Must have
> COLIB_ENABLE_MULTITHREAD_SCHED set to true and can be used from other threads.
>
> - `task` The task of the coroutine that is to be scheduled.

### `pool_t::run`
```cpp
run_e run();
```
> Runs the first coroutine in the ready queue. When this coroutine awaits something, the next one
> will be scheduled. Will keep running until there are no more I/O events to wait for, no more
> timers to sleep on, no more coroutines to run, or force_stop is used. This will block the thread
> that executed the run.
>
> Returns: Returns RUN_OK if no error occurred during the run, else the return code.

### `pool_t::clear`
```cpp
error_e clear();
```
> Destroys all the coroutines that are attached to this pool, meaning those in the ready queue,
> those waiting for I/O operations, and those waiting for semaphores. This will happen automatically
> inside the destructor.
>
> Returns: ERROR_OK if not an error, else the error code.

### `pool_t::stop_io`
```cpp
error_e stop_io(const io_desc_t& io_desc);
```
> Takes as an argument a valid io_desc_t and stops the operation described by the descriptor on the
> respective handle.
>
> - `io_desc` the descriptor of the io event to be stopped
>
> Returns: ERROR_OK if not an error, else the error code.

### `pool_t::get_internal_handle`
```cpp
intptr_t get_internal_handle();
```
> Get the internal handle of the pool, that can be the file descriptor of the epoll, the file
> descriptor of the kqueue or the handle of the iocp. Not really sure why you need it, but if you
> do, this is how can access it.

### `pool_t::get_internal`
```cpp
pool_internal_t *get_internal();
```
> Better to not touch this function, you need to understand the internals of pool_t to use it. It
> is public only to simplify the implementation.

### `pool_t::stopval`
```cpp
int64_t stopval = 0;
```
> A value that is set by force_stop(stopval). You can also set it if you need it, only force_stop
> modifies it

### `pool_t::user_ptr`
```cpp
std::shared_ptr<void> user_ptr;
```
> This is a pointer that you can use however you want. The library won't touch it, except, of
> course, when destructing the pool.

### `create_pool()`
```cpp
inline pool_p create_pool();
```
> Creates the pool object. Allocates space for the allocator and initiates diverese functions of
> the pool (epoll, iocp, timers, etc.).
>
> Returns: The pool_p handle to the pool

---

## `sem_t`

> This is a semaphore working on a pool. It can be awaited to decrement it's count and .signale()
> increments it's count from wherever. More above.

*(constructor is private/protected - use `create_sem`; if the semaphore dies while waiters wait,
they will all be forcefully destroyed - their entire call stack.)*

### `sem_t::unlocker_t`
> compatibility layer with guard objects ex: std::lock_guard guard(co_await u);

- `unlocker_t::lock()`: *Does nothing, the lock was already done by the co_await*
- `unlocker_t::unlock()`: *Signals the semaphore that returned the unlocker_t*

### `sem_t::wait`
```cpp
sem_awaiter_t wait(); /* if awaited returns unlocker_t{} */
```
> This awaiter object returns an unlocker that has the `lock` member function doing nothing and
> `unlock` function calling `signal` on the semaphore, meaning it can be used inside a
> `std::lock_guard` object to protect a piece of code using the RAII principle.
>
> Returns: An awaiter that can be awaited to decrement the internal counter of the semaphore. the
> await will result in an object of the type unlocker_t, that can be used inside a guard or
> ignored.

### `sem_t::signal`
```cpp
error_e signal(int64_t inc = 1); /* returns error if the pool disapeared */
```
> This function modifies the internal counter and awakes coroutines that are waiting on this
> semaphore as such:
> - If increment is less than 0, then it will decrease the internal counter with the amount.
> - If increment is 0 it does nothing.
> - If the increment is bigger than 0 it increases the internal counter and awakes waiters until
>   either there are no more waiters or the internal counter is 0.
>
> To wake every waiter regardless of the counter, use signal_all(). To reset a negative counter
> back to 0, use clear(0).

### `sem_t::signal_all`
```cpp
error_e signal_all();
```
> This signals all waiting coroutines to wake up. Uses the above signal function with the number of
> awaiters as the parameter and returns what it returs

### `sem_t::try_dec`
```cpp
bool try_dec();
```
> Non-blocking; If the semaphore counter is positive, decrements the counter and returns true, else
> returns false.

### `sem_t::clear`
```cpp
error_e clear(int64_t val);
```
> Destroys every coroutine currently waiting on this semaphore (its stack unwinds via destructors;
> it never resumes past `co_await wait()`), then resets the counter to `val`.

### `sem_t::get_internal` / `sem_t::invalidate_self`
```cpp
sem_internal_t *get_internal();
void invalidate_self() { internal = nullptr; }
```
> Again, beeter don't touch, same as pool. This is public only to ease the writing of the
> implementation.

### `create_sem()`
```cpp
inline sem_p create_sem(pool_t *pool, int64_t val);
inline sem_p create_sem(pool_p  pool, int64_t val);
inline task<sem_p> create_sem(int64_t val);
```
> Create a semaphore with the initial value set to `val`.
>
> - `pool` The pool (or pool_p, for the second overload) on which to create this semaphore.
> - `val` The initial value of the semaphore, can be negative
>
> Returns: a smart pointer that handles the semaphore internals (the third, coroutine-form
> overload: **Coroutine** that resolves to: A smart pointer that handles the semaphore internals.)

---

## `io_desc_t` (platform-specific)

**Linux** (`COLIB_OS_LINUX`):
```cpp
struct io_desc_t {
    int fd = -1;                        /*!< file descriptor */
    uint32_t events = 0xffff'ffff;      /*!< epoll events to be waited on the file descriptor */

    bool is_valid() { return fd >= 0; }
    bool operator == (const io_desc_t &oth) { return oth.fd == fd && oth.events == events; }
};
```
> This is the structure that describes an I/O operation, OS-dependent, used internally to handle
> I/O operations.

**Windows** (`COLIB_OS_WINDOWS`):
```cpp
struct io_data_t {
    enum io_flag_e : int32_t {
        IO_FLAG_NONE = 0,
        IO_FLAG_TIMER = 1,
        IO_FLAG_ADDED = 2,
        IO_FLAG_TIMER_RUN = 4,
    };

    OVERLAPPED overlapped = {0};                    /*!< must be the first member of this struct
                                                         (check IOCP documentation) */
    io_flag_e flags = io_flag_e{0};                 /*!< a mostly internal field that would normally
                                                         be `IO_FLAG_NONE` that holds the state type
                                                         of the I/O operation */
    state_t *state = nullptr;                       /*!< state of the task */
    DWORD recvlen = 0;                              /*!< the byte transfer count */

    std::function<error_e(void *)> io_request;      /*!< function to be called inside add_waiter,
                                                         for example: the ReadFile request */
    void *ptr = nullptr;                            /*!< can be context for io_request or timer
                                                         info */
    HANDLE h = NULL;                                /*!< same as in `io_desc_t` */
};
```
> Holds the state of the I/O operation

```cpp
struct io_desc_t {
    std::shared_ptr<io_data_t> data = nullptr;  /*!< internal `io_data_t` structure */
    HANDLE h = NULL;                            /*!< file/io device handle */

    bool is_valid() { return h != NULL; }
    bool operator == (const io_desc_t &oth) { return oth.h == h && oth.data == data; }
};
```
> This is the structure that describes an I/O operation, OS-dependent, used internally to handle
> I/O operations.
>
> The smart pointer `data` must be null for the function `stop_handle` to work.

**Unix/kqueue** (`COLIB_OS_UNIX`):
```cpp
struct io_desc_t {
    uintptr_t ident = (uintptr_t)-1;    /*!< file descriptor most of the time */
    short filter = -1;                  /*!< the filter of the event */
    unsigned int fflags = 0;            /*!< this is passed directly to the kevent on add/wait */
    intptr_t data = 0;                  /*!< this is passed directly to the kevent on add/wait */

    bool is_valid() { return ident != (uintptr_t)-1; }
};
```
> This is the structure that describes an I/O operation, OS-dependent, used internally to handle
> I/O operations.

**Unknown** (`COLIB_OS_UNKNOWN`): `COLIB_OS_UNKNOWN_IO_DESC` in full, preceded by the comment
`/* This describes the async io op. */`

---

## `state_t`

```cpp
struct state_t {
    error_e err = ERROR_OK;                 /*!< holds the error return in diverse cases */
    pool_t *pool = nullptr;                 /*!< the pool of this coro */
    modif_table_p modif_table;              /*!< we allocate a table only if there are mods */

    state_t *caller_state = nullptr;        /*!< this holds the caller's state, and with it the
                                            return path */

    std::coroutine_handle<void> self;       /*!< the coro's self handle */

    std::exception_ptr exception = nullptr; /*!< the exception that must be propagated */

    std::shared_ptr<void> user_ptr;         /*!< this is a pointer that the user can use for whatever
                                            he feels like. This library will not touch this pointer */

    ~state_t();                             /*!< Only used on debug */
};
```
> Internal state of corutines that is independent of the return value of the corutine. This
> structure, as explained above, is the common type for all coroutines from this library. It also
> holds a user pointer user_ptr that can be used. This pointer can be useful when working with
> modifications.

### `sem_waiter_handle_t` / `sem_waiter_handle_p`
```cpp
using sem_waiter_handle_t = std::list<
    std::pair<
        state_t *,
        std::shared_ptr<void>
    >,
    allocator_t<std::pair<state_t *, std::shared_ptr<void>>>
>::iterator;
using sem_waiter_handle_p = std::shared_ptr<sem_waiter_handle_t>;
```
> This is mostly internal. Internal pointer to an iterator inside the semaphore awaiter queue. It
> will be given as a parameter inside the callback of a modifier

`sem_waiter_handle_p`: *if this pointer is not available, the waiter was evicted from the waiters
list*

---

## `modif_t`

```cpp
struct modif_t {
    using variant_t = std::variant<
        std::function<error_e(state_t *)>,              /* call_cbk */
        std::function<error_e(state_t *)>,              /* sched_cbk */
        std::function<error_e(state_t *)>,              /* exit_cbk */
        std::function<error_e(state_t *)>,              /* leave_cbk */
        std::function<error_e(state_t *)>,              /* enter_cbk */
        std::function<error_e(state_t *, io_desc_t&)>,  /* wait_io_cbk */
        std::function<error_e(state_t *, io_desc_t&)>,  /* unwait_io_cbk */
        std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,  /* wait_sem_cbk - OBS: the
                                            std::shared_ptr<void> part can be ignored, it's internal */
        std::function<error_e(state_t *, sem_t *)>       /* unwait_sem_cbk - No handle here, as the
                                            semaphore is no longer in the waiting list */
    >;

    variant_t cbk;
    modif_e type = CO_MODIF_COUNT;
    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
};
```
> Modifs, corutine modifications. Those modifications controll the way a corutine behaves when
> awaited or when spawning a new corutine from it. Those modifications can be inherited, making all
> the corutines in the call sub-tree behave in a similar way. For example: adding a timeouts, for
> this example, all the corutines that are on the same call-path(not sched-path) would have a
> timer, such that the original call would not exist for more than X time units. (+/- the code
> between awaits).
>
> You should use create_modif to create an object of this type.

`modif_t::cbk`: *This is the callback that will be called on the location specified by type. It
must be placed inside the correct variant slot.*

---

## Pool & Sched functions

### `get_pool()`
```cpp
inline get_pool_awaiter_t get_pool();
```
> Returns: **Awaitable** that resolves to: The pointer of the pool coresponding to the coroutine
> from which this function is called from.

### `get_state()`
```cpp
inline get_state_awaiter_t get_state();
```
> Returns: **Awaitable** that resolves to: The pointer of the state_t of the current coroutine.

### `sched()`
```cpp
template <typename T>
inline sched_awaiter_t<T> sched(task<T> to_sched, const modif_pack_t& v = {});
```
> Does the same thing as pool_t::sched, on the running coroutine's pool. This does not stop the
> curent corutine, it only schedules the task, but does not yet run it.
>
> - `to_sched` The coroutine's task that is to be scheduled.
> - `v` The modifications that should be added to the coroutine.
>
> Returns: **Awaitable** that resolves to: executing the sched.

### `yield()`
```cpp
inline yield_awaiter_t yield();
```
> Suspends the current coroutine and moves it to the end of the ready queue within its associated
> pool.
>
> Returns: **Awaitable** that resolves to: executing the yield.

---

## Externals

*Task Initialization group - functions for manually initializing coroutine tasks. Normally, tasks
are initialized and scheduled using the `sched` functions. However, in rare cases (e.g., resuming
the coroutine engine from inside a callback), you may need to create a task that runs without being
scheduled.*

Example usage, verbatim from `colib.h`:
```cpp
void callback(user_ctx_t *ctx) {
    colib::pool_t *pool = ctx->pool;
    // OBS: we do not propagate modifications, for simple call chains, this is ok, but if you
    // need them you may want to add them.
    auto continuation = [&]() -> co::task_t {
        struct stop_awaitable_t : public std::suspend_always {
            std::coroutine_handle<void> await_suspend(std::coroutine_handle<>) {
                co_return std::noop_coroutine();
            }
        };

        co_await dependency_awaiter();
        co_await stop_awaitable_t{};
        co_return 0;
    }();
    colib::external_init_task(state, pool, {});
    continuation.h.resume(); // Manually start the coroutine
    continuation.h.destroy(); // This coroutine is our responsability
}
```

### `external_init_task()`
```cpp
template <typename T>
inline state_t *external_init_task(task<T> task, pool_t *pool);
inline state_t *external_init_task(state_t *state, pool_t *pool);
```
> - `T` The return type of the coroutine task.
> - `task`/`state` The coroutine task/state to initialize.
> - `pool` The pool the task will run on. (Again, a task can't change it's pool)
>
> Returns: `state_t*` Pointer to the initialized task state.

Separately, on user-implemented awaitables in general:

> Using an user-implemented awaitable to suspend and resume a coroutine
>
> You can create your own awaitables that suspend coroutines without relying on the library's IO
> system. The awaitable must implement:
> - await_ready()
> - await_resume()
> - await_suspend()
>
> Here is a typical pattern for await_suspend with this library:
>
> ```
> await_suspend(colib_coro) {
>     // Step 1: Register suspension
>     colib::state_t *to_resume = colib::external_on_suspend(colib_coro);
>
>     // Step 2: Do your custom work here (non-blocking is recommended)
>
>     // Step 3: Decide how to resume
>     // Option 1: Resume your own coroutine
>     return own_coro;
>
>     // Option 2: Resume the suspended coroutine directly
>     return colib::external_on_resume(to_resume);
>
>     // Option 3: Continue another coroutine from the library
>     return colib::external_wait_next_task(state->pool);
> }
> ```
>
> Notes:
> - Always ensure the suspended coroutine is eventually resumed, otherwise the library may stall.
> - Avoid blocking in await_suspend; blocking will pause all library coroutines.
> - Lifetimes: do not let the pool or coroutine state be destroyed before resuming.

### `external_on_suspend()`
```cpp
template <typename P>
inline state_t *external_on_suspend(std::coroutine_handle<P> colib_coro);
```
> Registers that a coroutine is being suspended by an external awaitable (e.g., waiting for a
> condition or custom event outside the library's IO system). This function allows the library to
> track suspended coroutines properly.
>
> Usage:
> - Call this in your awaitable's await_suspend before performing any custom suspension logic.
> - Returns a raw pointer to the coroutine's state, which must later be resumed.
>
> - `P` Coroutine promise type
> - `colib_coro` The coroutine handle being suspended
>
> Returns: Pointer to the internal coroutine state, used for later resumption

### `external_on_resume()`
```cpp
inline std::coroutine_handle<void> external_on_resume(state_t *state);
```
> Resumes a coroutine that was suspended using external_on_suspend. This ensures the library keeps
> track of active/inactive coroutines properly.
>
> Usage:
> - Call this when you want to resume the coroutine.
> - Returns a coroutine handle that should be awaited or scheduled next.
>
> - `state` The state of the coroutine to resume
>
> Returns: Coroutine handle ready for execution

### `external_sched_resume()`
```cpp
inline void external_sched_resume(state_t *state);
```
> Schedules a suspended coroutine to be resumed later by the library's scheduler.
>
> Usage:
> - Call this when you want to mark a coroutine as ready to continue, but you still don't want to
>   resume the library.
> - This and external_on_resume are mutually exclusive, only one call can be made per each received
>   state_t*.
> - Useful when implementing custom awaitables that integrate with the library's scheduling system.
> - The task will be added inside the ready_tasks of the state's pool, so resuming the pool will
>   eventually execute the scheduled task and the provided await_resume will be called
>
> - `state` Pointer to the suspended coroutine's state

### `external_has_next_task()`
```cpp
inline bool external_has_next_task(pool_t *pool);
```
> Checks whether the library has any ready-to-run tasks.
>
> Usage:
> - Use this before calling external_wait_next_task if you want to avoid blocking.
> - Returns true if calling external_wait_next_task will immediately return a task; false otherwise.
>
> - `pool` The pool for which to query the ready state
>
> Returns: True if a task is available for immediate execution

### `external_wait_next_task()`
```cpp
inline std::coroutine_handle<void> external_wait_next_task(pool_t *pool);
```
> Retrieves the next coroutine that has work to perform. If no task is ready, this function blocks
> until a coroutine becomes runnable (e.g., an IO operation completes or another external awaitable
> resumes a coroutine).
>
> Usage:
> - Typically used in external awaitables to continue processing library-managed coroutines.
>
> - `pool` The pool from which to fetch the next ready coroutine (obs: each valid state_t has to
>   have a valid pool_t*)
>
> Returns: Coroutine handle for the next task to run

---

## Modifications

### `create_modif()`
```cpp
template <modif_e type, typename Cbk>
inline modif_p create_modif(modif_flags_e flags, Cbk&& cbk);
```
> Creates a modification that will be executed on the given modif_type, inherited by the rules
> specified inside modif_flags and on the given pool. It will execute the callback cbk at those
> points.
>
> - `type` The location from which this modification will be called from.
> - `flags` The inherit flags of this modification.
> - `cbk` The callback that will be called.
>
> Returns: A smart pointer to the modification.

*(`colib.h` declares this identical signature+comment twice in a row - reproduced faithfully; looks
like a copy/paste duplicate in the header itself, not two distinct overloads.)*

### `task_modifs()` (task-argument form)
```cpp
template <typename T>
inline std::vector<modif_p> task_modifs(task<T> t);
```
> Get the modifications that a coroutine has.
>
> - `t` The task of the coroutine
>
> Returns: A vector that holds the different modifications of the task.

### `add_modifs()` / `rm_modifs()` (task-argument forms)
```cpp
template <typename T>
inline task<T> add_modifs(pool_t *pool, task<T> t, const modif_pack_t& mods);
template <typename T>
inline task<T> rm_modifs(task<T> t, const modif_pack_t& mods);
```
> Adds modifiers to the task 't'. This uses a set because the modifiers need to be unique.
> (`rm_modifs`: Removes modifiers from a coroutine. This uses a set because the modifiers need to
> be unique.)
>
> - `pool` The pool that is common in between the coroutine and the modifications.
> - `t` The task of the coroutine.
> - `mods` The modifications to be added / removed.
>
> Returns: The task t is returned for convenience.

### `task_modifs()` / `add_modifs()` / `rm_modifs()` (coroutine forms)
```cpp
inline task<std::vector<modif_p>> task_modifs();
inline task_t add_modifs(const modif_pack_t& mods);
inline task_t rm_modifs(const modif_pack_t& mods);
```
> Get/Adds/Removes the modifications/modifiers that/to/from the current coroutine has. This uses a
> set because the modifiers need to be unique (add/rm forms).
>
> **Warning:** Must be co_await-ed directly by the coroutine whose modifs are being read/added to/
> removed from: it resolves to whichever coroutine is co_await-ing it, not to itself. Scheduling
> this task via pool->sched()/co::sched() instead of co_await-ing it is undefined behavior - there
> is no "current coroutine"/"current task" for it to refer to in that case.
>
> - `mods` The modifications to be added / removed. *(add/rm forms only)*
>
> Returns: **Coroutine** that resolves to: A vector that holds the different modifications of the
> task / the adding of the modifiers / the removing of the modifiers.

### `await()`
```cpp
template <typename Awaiter>
inline task_t await(Awaiter&& awaiter);
```
> Helper coroutine function, given an awaitable, awaits it inside the coroutine await, usefull if
> the awaitable can't be decorated with modifiers, bacause it isn't a coroutine.
>
> - `awaiter` The awaiter that is to be co_awaited.
>
> Returns: **Coroutine** that resolves to: The awaiter being co_await-ed and **further** resolves
> to the success value, on success, ERROR_OK.

---

## Timing

### `create_timeo()`
```cpp
template <typename T>
inline task<std::pair<T, error_e>> create_timeo(
        task<T> t, pool_t *pool, const std::chrono::microseconds& timeo);
```
> Schedules the task `t` and a timer that kills the task `t`, if `t` doesn't finish before the
> timer expires in timeo_ms milliseconds. This function returns a coroutine that can be awaited to
> get the return value and error value. If the error value is not ERROR_OK, than the task `t`
> wasn't executed succesfully. This function will schedule the coroutine pointed by `t`
>
> - `t` The coroutine that is to be scheduled
> - `pool` The pool on which to schedule the task `t`
> - `timeo` The timeout after which the task `t` will be destroyed if it didn't complete the
>   execution.
>
> Returns: A coroutine that will resolve to the return type T and an error_e that signals if an
> error occoured(for example the timeout). T must be default constructible.

### `sleep_us()` / `sleep_ms()` / `sleep_s()` / `sleep()`
```cpp
inline task_t sleep_us(uint64_t timeo_us);
inline task_t sleep_ms(uint64_t timeo_ms);
inline task_t sleep_s(uint64_t timeo_s);
inline task_t sleep(const std::chrono::microseconds& us);
```
> Awaitable coroutine that sleep for the given duration in microseconds/milliseconds/seconds (or a
> c++ duration, for `sleep`). The precision with which this sleep occours is given by the hardware.
>
> A 0-duration sleep resolves immediately without ever suspending or touching the timer subsystem -
> same on every platform, deliberately, rather than inheriting whatever a 0 due-time happens to mean
> to the underlying OS timer API (which differs: `timerfd_settime` disarms on Linux, while a 0
> `SetWaitableTimer` due-time reads as an absolute FILETIME in the deep past on Windows).
>
> - `timeo_us`/`timeo_ms`/`timeo_s`/`us` Time duration in the respective unit.
>
> Returns: **Coroutine** that resolves to: executing the sleep

---

## Flow Controll

### `create_killer()`
```cpp
inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e);
```
> Creates a modification pack that can be added to only one coroutine that is associated with the
> given pool. The second parameter e will be the error value of the coroutine. The returned
> function can be called to kill the given coroutine and it's entire call stack (does not kill
> sched stack).
>
> **Warning:** A killer is single-target and single-use, permanently: attach its modif_pack_t to
> exactly one coroutine, call the returned function at most once. Once that coroutine has been
> killed (or has otherwise exited), the same killer cannot be re-attached to a different coroutine
> to kill it too - the returned function will unconditionally report "nothing to kill" from then
> on, even if you did attach it elsewhere. This isn't an arbitrary restriction: the killer tracks
> its target's call stack as one flat, untagged stack, so attaching the same killer to more than
> one coroutine (whether at once or one after another) has no well-defined way to tell those
> coroutines' frames apart. Calling the returned function reentrantly - from within a callback that
> runs as a side effect of the kill it already triggered - is caught and rejected the same way
> (also reported as "nothing to kill"), rather than corrupting the in-progress unwind.
>
> - `pool` The pool on which to bind this killer
> - `e` The error value that will be set inside the killed coroutine on kill
>
> Returns: A pair containing the modification pack that is to be attached to the target coroutine
> and a function that is to be called when the user wants to kill the target coroutine.

### `create_future()`
```cpp
template <typename T>
inline task<T> create_future(pool_t *pool, task<T> t); /* not awaitable */
```
> Takes a task and adds the requred modifications to it such that the returned object will be
> returned once the return value of the task is available.
>
> - `pool` The pool on which the task `t` will be scheduled.
> - `t` The task of the coroutine that will be scheduled.
>
> Returns: The task of a coroutine that can be awaited to wait for the return value
>
> Example:
> ```cpp
> 1: auto t = co_task();
> 2: auto fut = colib::create_future(t)
> 3: co_await colib::sched(t);
> 4: // ...
> 5: co_await fut; // returns the value of co_task once it has finished executing
> ```

### `wait_all()`
```cpp
template <typename ...ret_v>
inline task<std::tuple<ret_v>...> wait_all(task<ret_v>... tasks);
```
> Wait for all the tasks to finish, the return value can be found in the respective task, killing
> one kills all (sig_killer installed in all). The inheritance is the same as with 'call'.
>
> - `tasks` The tasks to be awaited
>
> Returns: **Coroutine** that resolves to: The waiting of all the tasks, that **further** results
> in their return values.

### `force_stop()`
```cpp
inline task_t force_stop(int64_t stopval = 0);
```
> Causes the running pool::run to stop, the function will stop at this point, can be resumed with
> another run call.
>
> - `stopval` The return value of the current run call.
>
> Returns: **Coroutine** that resolves to: the execution of the stop and **further** results in
> ERROR_OK in the coroutine that called the stop, this would happen on the next call of the
> function pool_t::run

---

## Cross-platform I/O

### `wait_event()`
```cpp
inline task_t wait_event(const io_desc_t& io_desc);
```
> Waits for the described event to be available/finish, depending on the OS. Usefull if you have an
> event that supports the curent type of async engine (epoll/iocp) but is not implemented in this
> library.
>
> - `io_desc` The descriptor of the event. On Windows this must have a valid event state.
>
> Returns: **Coroutine** that resolves to: the awaiting of the desired event and **further**
> resolves to ERROR_OK if the event completed successfully.

### `stop_io()`
```cpp
inline task_t stop_io(const io_desc_t& io_desc);
```
> Stops the given I/O event, described by io_desc, by canceling it's wait and making the awaitable
> return an error. This does not close the file descriptor mentioned in `io_desc`, in fact a call
> to stop_io is necesary if the descriptor is awaited by the pool, else the entire event queue will
> error out.
>
> - `io_desc` The event that needs to be closed. On Windows you can either stop a specific event or
>   all the events on a descriptor, while on Linux you can be more granular with your events.
>   Either way, to stop all events on Windows, use a nullptr for the io state pointer.
>
> Returns: **Coroutine** that resolves to: the stopping of the event when awaited and **further**
> resolves to the status of the event, ERROR_OK, if the wait was successfull.

---

## Linux && UNIX Specific (`COLIB_OS_LINUX || COLIB_OS_UNIX`)

### `stop_fd()`
```cpp
inline task_t stop_fd(int fd);
```
> Linux specific, is used to evict an fd from the epoll engine before closing it, you shouldn't
> close a file descriptor before removing it from the pool.
>
> - `fd` The file descriptor to close
>
> Returns: **Coroutine** that resolves to: The action being performed, **further** resolves to an
> eventual error or ERROR_OK on success.

### `connect()`
```cpp
inline task_t connect(int fd, sockaddr *sa, socklen_t *len);
```
> Linux specific, calls system's connect using coroutines. Check out `man connect`.
>
> - `fd` - file descriptor, same as ::connect
> - `sa` - socket address, same as ::connect
> - `len` - size of socket address, same as ::connect
>
> Returns: **Coroutine** that resolves to: the execution of the function and **further** resolves
> to the success value of the function, i.e. ERROR_OK for success.

### `accept()`
```cpp
inline task_t accept(int fd, sockaddr *sa, socklen_t *len);
```
> Linux specific, calls system's accept using coroutines. Check out `man accept`.
>
> - `fd` - file descriptor, same as ::accept
> - `sa` - socket address, same as ::accept
> - `len` - size of socket address, same as ::accept
>
> Returns: **Coroutine** that resolves to: the execution of the function and **further** resolves
> to the success value of the function, i.e. ERROR_OK for success.

### `read()`
```cpp
inline task<ssize_t> read(int fd, void *buff, size_t len);
```
> Linux specific, calls system's read using coroutines. Check out `man read`.
>
> - `fd` - file descriptor, same as ::read
> - `buff` - read buffer, same as ::read
> - `len` - size of the buffer, same as ::read
>
> Returns: **Coroutine** that resolves to: the execution of the function and **further** resolves
> to the return value of the function.

### `write()`
```cpp
inline task<ssize_t> write(int fd, const void *buff, size_t len);
```
> Linux specific, Calls system's write using coroutines. Check out `man 2 write`.
>
> - `fd` - file descriptor, same as ::write
> - `buff` - write buffer, same as ::write
> - `len` - size of the buffer, same as ::write
>
> Returns: **Coroutine** that resolves to: the execution of the function and **further** resolves
> to the return value of the function.

### `read_sz()`
```cpp
inline task_t read_sz(int fd, void *buff, size_t len);
```
> Linux specific, same as the sistem call read, just that it waits for the exact length to be
> received. This function also gives an error if the connection is closed during the operation.
> See `read` for details.
>
> - `fd` - the file descriptor
> - `buff` - to read into buffer
> - `len` - size of the buffer
>
> Returns: **Coroutine** that resolves to: the execution of the function and **further** resolves
> to the success value of the function, i.e. ERROR_OK for success.

### `write_sz()`
```cpp
inline task_t write_sz(int fd, const void *buff, size_t len);
```
> Linux specific, same as the sistem call write, just that it waits for the exact length to be
> written. This function also gives an error if the connection is closed during the operation. See
> `write` for details.
>
> - `fd` - the file descriptor
> - `buff` - to write from buffer
> - `len` - size of the buffer
>
> Returns: **Coroutine** that resolves to: the execution of the function and **further** resolves
> to the success value of the function, i.e. ERROR_OK for success.

---

## Windows Specific (`COLIB_OS_WINDOWS`)

### `stop_handle()`
```cpp
inline task_t stop_handle(HANDLE h);
```
> Windows specific, is used to evict a HANDLE h from the iocp engine before closing it, you
> shouldn't close a handle before removing it from the pool.
>
> - `h` The handle to be evicted.
>
> Returns: **Coroutine** that resolves to: The action being performed, **further** resolves to an
> eventual error or ERROR_OK on success.

*Those functions are the same as their Windows API equivalent, the difference is that they don't
expose the overlapped structure, which is used by the coro library. They require a handle that is
compatible with iocp and they will attach the handle to the iocp instance. Those are the functions
listed by msdn to work with iocp (and connect, that is part of an extension)*

Each of the following has the same lead-in comment, verbatim: *"This function is calling it's WinAPI
(or extension) counterpart, but in coroutine context and it is missing the OVERLAPPED pointer
because that one is owned by the I/O engine. It requires a handle that is compatible with iocp,
usually be the flag FILE_FLAG_OVERLAPPED, that handle will be attached to the iocp instance."* and
the same return doc: *"**Coroutine** that resolves to: the execution of the function and **further**
resolves to TRUE if the execution was successfull, FALSE otherwise"* - not repeated per-entry below.

### `ConnectEx()`
```cpp
inline task<BOOL> ConnectEx(SOCKET  s,
                            const   sockaddr *name,
                            int     namelen,
                            PVOID   lpSendBuffer,
                            DWORD   dwSendDataLength,
                            LPDWORD lpdwBytesSent);
```
> - `s` handle of the socket, same as ::ConnectEx
> - `name` Same as ::ConnectEx
> - `namelen` Same as ::ConnectEx
> - `lpSendBuffer` Same as ::ConnectEx
> - `dwSendDataLength` Same as ::ConnectEx
> - `lpdwBytesSent` Same as ::ConnectEx when the call is blocking

### `AcceptEx()`
```cpp
inline task<BOOL> AcceptEx(SOCKET   sListenSocket,
                           SOCKET   sAcceptSocket,
                           PVOID    lpOutputBuffer,
                           DWORD    dwReceiveDataLength,
                           DWORD    dwLocalAddressLength,
                           DWORD    dwRemoteAddressLength,
                           LPDWORD  lpdwBytesReceived);
```
> - `sListenSocket` Socket handle, Same as ::AcceptEx
> - `sAcceptSocket` Same as ::AcceptEx
> - `lpOutputBuffer` Same as ::AcceptEx
> - `dwReceiveDataLength` Same as ::AcceptEx
> - `dwLocalAddressLength` Same as ::AcceptEx
> - `dwRemoteAddressLength` Same as ::AcceptEx
> - `lpdwBytesReceived` Same as ::AcceptEx when the call is blocking

### `ConnectNamedPipe()`
```cpp
inline task<BOOL> ConnectNamedPipe(HANDLE hNamedPipe);
```
> - `hNamedPipe` Handle of the pipe, same as ::ConnectNamedPipe

### `DeviceIoControl()`
```cpp
inline task<BOOL> DeviceIoControl(HANDLE    hDevice,
                                  DWORD     dwIoControlCode,
                                  LPVOID    lpInBuffer,
                                  DWORD     nInBufferSize,
                                  LPVOID    lpOutBuffer,
                                  DWORD     nOutBufferSize,
                                  LPDWORD   lpBytesReturned);
```
> - `hDevice` Device handle, Same as ::DeviceIoControl
> - `dwIoControlCode` Same as ::DeviceIoControl
> - `lpInBuffer` Same as ::DeviceIoControl
> - `nInBufferSize` Same as ::DeviceIoControl
> - `lpOutBuffer` Same as ::DeviceIoControl
> - `nOutBufferSize` Same as ::DeviceIoControl
> - `lpBytesReturned` Same as ::DeviceIoControl when the call is blocking

### `LockFileEx()`
```cpp
inline task<BOOL> LockFileEx(HANDLE     hFile,
                             DWORD      dwFlags,
                             DWORD      dwReserved,
                             DWORD      nNumberOfBytesToLockLow,
                             DWORD      nNumberOfBytesToLockHigh,
                             uint64_t   *offset);
```
> - `hFile` File handle, same as ::LockFileEx
> - `dwFlags` Same as ::LockFileEx
> - `dwReserved` Same as ::LockFileEx
> - `nNumberOfBytesToLockLow` Same as ::LockFileEx
> - `nNumberOfBytesToLockHigh` Same as ::LockFileEx
> - `offset` This functions needs the offset from inside the OVERLAPPED structure, this pointer's
>   contents will be copied inside the overlapped structure and copied out of the overlapped
>   structure after the call is done.

### `ReadDirectoryChangesW()`
```cpp
inline task<BOOL> ReadDirectoryChangesW(HANDLE                              hDirectory,
                                        LPVOID                              lpBuffer,
                                        DWORD                               nBufferLength,
                                        BOOL                                bWatchSubtree,
                                        DWORD                               dwNotifyFilter,
                                        LPDWORD                             lpBytesReturned,
                                        LPOVERLAPPED_COMPLETION_ROUTINE     lpCompletionRoutine);
```
> - `hDirectory` Directory handle, same as ::ReadDirectoryChangesW
> - `lpBuffer` Same as ::ReadDirectoryChangesW
> - `nBufferLength` Same as ::ReadDirectoryChangesW
> - `bWatchSubtree` Same as ::ReadDirectoryChangesW
> - `dwNotifyFilter` Same as ::ReadDirectoryChangesW
> - `lpBytesReturned` Same as ::ReadDirectoryChangesW when the call is blocking
> - `lpCompletionRoutine` Same as ::ReadDirectoryChangesW

### `ReadFile()`
```cpp
inline task<BOOL> ReadFile(HANDLE   hFile,
                           LPVOID   lpBuffer,
                           DWORD    nNumberOfBytesToRead,
                           LPDWORD  lpNumberOfBytesRead,
                           uint64_t *offset);
```
> - `hFile` File handle, Same as ::ReadFile
> - `lpBuffer` Same as ::ReadFile
> - `nNumberOfBytesToRead` Same as ::ReadFile
> - `lpNumberOfBytesRead` Same as ::ReadFile when the call is blocking

### `TransactNamedPipe()`
```cpp
inline task<BOOL> TransactNamedPipe(HANDLE  hNamedPipe,
                                    LPVOID  lpInBuffer,
                                    DWORD   nInBufferSize,
                                    LPVOID  lpOutBuffer,
                                    DWORD   nOutBufferSize,
                                    LPDWORD lpBytesRead);
```
> - `hNamedPipe` Pipe handle, same as ::TransactNamedPipe
> - `lpInBuffer` Same as ::TransactNamedPipe
> - `nInBufferSize` Same as ::TransactNamedPipe
> - `lpOutBuffer` Same as ::TransactNamedPipe
> - `nOutBufferSize` Same as ::TransactNamedPipe
> - `lpBytesRead` Same as ::TransactNamedPipe when the call is blocking

### `WaitCommEvent()`
```cpp
inline task<BOOL> WaitCommEvent(HANDLE  hFile,
                                LPDWORD lpEvtMask);
```
> - `hFile` Device handle, Same as ::WaitCommEvent
> - `lpEvtMask` Same as ::WaitCommEvent

### `WriteFile()`
```cpp
inline task<BOOL> WriteFile(HANDLE  hFile,
                            LPCVOID lpBuffer,
                            DWORD   nNumberOfBytesToWrite,
                            LPDWORD lpNumberOfBytesWritten,
                            uint64_t *offset);
```
> - `hFile` File handle, same as ::WriteFile
> - `lpBuffer` Same as ::WriteFile
> - `nNumberOfBytesToWrite` Same as ::WriteFile
> - `lpNumberOfBytesWritten` Same as ::WriteFile when the call is blocking
> - `offset` This functions needs the offset from inside the OVERLAPPED structure, this pointer's
>   contents will be copied inside the overlapped structure and copied out of the overlapped
>   structure after the call is done.

### `WSASendMsg()`
```cpp
inline task<BOOL> WSASendMsg(SOCKET                             s,
                            LPWSAMSG                            lpMsg,
                            DWORD                               dwFlags,
                            LPDWORD                             lpNumberOfBytesSent,
                            LPWSAOVERLAPPED_COMPLETION_ROUTINE  lpCompletionRoutine);
```
> - `s` Socket handle, same as ::WSASendMsg
> - `lpMsg` Same as ::WSASendMsg
> - `dwFlags` Same as ::WSASendMsg
> - `lpNumberOfBytesSent` Same as ::WSASendMsg when the call is blocking
> - `lpCompletionRoutine` Same as ::WSASendMsg

### `WSASendTo()`
```cpp
inline task<BOOL> WSASendTo(SOCKET                             s,
                            LPWSABUF                           lpBuffers,
                            DWORD                              dwBufferCount,
                            LPDWORD                            lpNumberOfBytesSent,
                            DWORD                              dwFlags,
                            const sockaddr                     *lpTo,
                            int                                iTolen,
                            LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```
> - `s` Socket handle, same as ::WSASendTo
> - `lpBuffers` Same as ::WSASendTo
> - `dwBufferCount` Same as ::WSASendTo
> - `lpNumberOfBytesSent` Same as ::WSASendTo when the call is blocking
> - `dwFlags` Same as ::WSASendTo
> - `lpTo` Same as ::WSASendTo
> - `iTolen` Same as ::WSASendTo
> - `lpCompletionRoutine` Same as ::WSASendTo

### `WSASend()`
```cpp
inline task<BOOL> WSASend(SOCKET                             s,
                          LPWSABUF                           lpBuffers,
                          DWORD                              dwBufferCount,
                          LPDWORD                            lpNumberOfBytesSent,
                          DWORD                              dwFlags,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```
> - `s` Socket handle, same as ::WSASend
> - `lpBuffers` Same as ::WSASend
> - `dwBufferCount` Same as ::WSASend
> - `lpNumberOfBytesSent` Same as ::WSASend when the call is blocking
> - `dwFlags` Same as ::WSASend
> - `lpCompletionRoutine` Same as ::WSASend

### `WSARecvFrom()`
```cpp
inline task<BOOL> WSARecvFrom(SOCKET                             s,
                              LPWSABUF                           lpBuffers,
                              DWORD                              dwBufferCount,
                              LPDWORD                            lpNumberOfBytesRecvd,
                              LPDWORD                            lpFlags,
                              sockaddr                           *lpFrom,
                              LPINT                              lpFromlen,
                              LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```
> - `s` Socket handle, same as ::WSARecvFrom
> - `lpBuffers` Same as ::WSARecvFrom
> - `dwBufferCount` Same as ::WSARecvFrom
> - `lpNumberOfBytesRecvd` Same as ::WSARecvFrom when the call is blocking
> - `lpFlags` Same as ::WSARecvFrom
> - `lpFrom` Same as ::WSARecvFrom
> - `lpFromlen` Same as ::WSARecvFrom
> - `lpCompletionRoutine` Same as ::WSARecvFrom

### `WSARecvMsg()`
```cpp
inline task<BOOL> WSARecvMsg(SOCKET                             s,
                             LPWSAMSG                           lpMsg,
                             LPDWORD                            lpdwNumberOfBytesRecvd,
                             LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```
> - `s` Socket handle, same as ::WSARecvMsg
> - `lpMsg` Same as ::WSARecvMsg
> - `lpdwNumberOfBytesRecvd` Same as ::WSARecvMsg when the call is blocking
> - `lpCompletionRoutine` Same as ::WSARecvMsg

### `WSARecv()`
```cpp
inline task<BOOL> WSARecv(SOCKET                             s,
                          LPWSABUF                           lpBuffers,
                          DWORD                              dwBufferCount,
                          LPDWORD                            lpNumberOfBytesRecvd,
                          LPDWORD                            lpFlags,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```
> - `s` - Socket handle, same as ::WSARecv
> - `lpBuffers` - Same as ::WSARecv
> - `dwBufferCount` - Same as ::WSARecv
> - `lpNumberOfBytesRecvd` - Same as ::WSARecv when the call is blocking
> - `lpFlags` - Same as ::WSARecv
> - `lpCompletionRoutine` - Same as ::WSARecv

### Adaptations for Windows
```cpp
inline task_t        connect(SOCKET s, const sockaddr *sa, uint32_t len);
inline task<SOCKET>  accept(SOCKET s, sockaddr *sa, uint32_t *len);
inline task<SSIZE_T> read(HANDLE h, void *buff, size_t len, uint64_t *offset = nullptr);
inline task<SSIZE_T> write(HANDLE h, const void *buff, size_t len, uint64_t *offset = nullptr);
inline task_t        read_sz(HANDLE h, void *buff, size_t len, uint64_t *offset = nullptr);
inline task_t        write_sz(HANDLE h, const void *buff, size_t len, uint64_t *offset = nullptr);
```
- `connect`: *This is the ported version of the Linux colib::connect, it is the same, but it takes
  a socket handle as a parameter and uses colib::ConnectEx internally.*
- `accept`: *This is the ported version of the Linux colib::accept, it is the same, but it takes a
  socket handle as a parameter and uses colib::AcceptEx internally.*
- `read`: *This is the ported version of the Linux colib::read, it is the same, but it takes a
  socket handle as a parameter and uses colib::ReadFile internally.*
- `write`: *This is the ported version of the Linux colib::write, it is the same, but it takes a
  socket handle as a parameter and uses colib::WriteFile internally. It also takes an offset, just
  like WriteFile.*
- `read_sz`: *This is the ported version of the Linux colib::read_sz, it is the same, but it takes
  a socket handle as a parameter and uses the Windows version of colib::read internally.*
- `write_sz`: *This is the ported version of the Linux colib::write_sz, it is the same, but it
  takes a socket handle as a parameter and uses the Windows version of colib::write internally. It
  also takes an offset, just like WriteFile.*

---

## Unknown Specific (`COLIB_OS_UNKNOWN`)

> You implement your own

No further declarations - entirely user-supplied.

---

## Debug Interfaces

*Why replace the old string with the new one based on the allocator? because I need to know that
the library allocates only through the allocator, so debugging would interfere with that.*

### `dbg_string_t`
```cpp
using dbg_string_t = std::basic_string<char, std::char_traits<char>, allocator_t<char>>;
```
No standalone doc comment (documented by the preceding block quoted above).

### `dbg()`
```cpp
template <typename... Args>
inline void dbg(const char *file, const char *func, int line, const char *fmt, Args&&... args);
```
> logs a formated string, used by COLIB_DEBUG

### `dbg_register_name()`
```cpp
template <typename T, typename ...Args>
inline task<T> dbg_register_name(task<T> t, const char *fmt, Args&&...);
template <typename P, typename ...Args>
inline std::coroutine_handle<P> dbg_register_name(std::coroutine_handle<P> h,
        const char *fmt, Args&&...);
template <typename ...Args>
inline void * dbg_register_name(void *addr, const char *fmt, Args&&...);
```
> Registers a name for a given task

### `dbg_create_tracer()`
```cpp
inline modif_pack_t dbg_create_tracer(pool_t *pool);
```
> creates a modifier that traces the path of corutines, mostly a convenience function, it also uses
> the log_str function, or does nothing else if it isn't defined. If you don't like the verbosity,
> be free to null any callback you don't care about.

### `dbg_name()`
```cpp
template <typename T>
inline dbg_string_t dbg_name(task<T> t);
template <typename P>
inline dbg_string_t dbg_name(std::coroutine_handle<P> h);
inline dbg_string_t dbg_name(void *v);
```
> Obtains the name given to the respective task / Obtains a string from the given coroutine handle
> / Obtains a string from the given coroutine address

### `dbg_enum()`
```cpp
inline dbg_string_t dbg_enum(error_e code);
inline dbg_string_t dbg_enum(run_e code);
```
> Obtains a string from the given enum

### `dbg_to_str()`
```cpp
inline dbg_string_t dbg_to_str(const io_desc_t &desc);
```
> Optains a string version of the given object

*(Note: currently unimplemented in `colib.h` - the definition's body is just `return
"NOT_IMPLEMENTED_TO_STR"` with a bare `/* TODO: */`, colib.h ~line 6151-6155.)*

### `dbg_epoll_events()` *(`COLIB_OS_LINUX` only)*
```cpp
inline dbg_string_t dbg_epoll_events(uint32_t events);
```
> Obtains a string from the given epoll event

### `dbg_kqueue_filter()` *(`COLIB_OS_UNIX` only)*
```cpp
inline dbg_string_t dbg_kqueue_filter(short filter);
```
> Obtain a string from the given kqueue filter

### `dbg_format()`
```cpp
template <typename... Args>
inline dbg_string_t dbg_format(const char *fmt, Args&& ...args);
```
> formats a string using the C snprintf, similar in functionality to a combination of
> snprintf+std::format, in the version of g++ that I'm using std::format is not available

### `log_str` *(when `COLIB_ENABLE_LOGGING`)*
```cpp
inline std::function<int(const dbg_string_t&)> log_str = COLIB_LOG_FUNCTION;
```
No standalone doc comment - see `COLIB_LOG_FUNCTION` above; the trailing comment `/* calls log_str
to save the log string */` precedes the `#if COLIB_ENABLE_LOGGING` guard, not the declaration
itself.
