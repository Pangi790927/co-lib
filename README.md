# Co-Lib

Introduction
===============

In C++20 a new feature was added, namely coroutines. This feature allows the creation of a kind
of special function named a coroutine that can have its execution suspended and resumed later on.
Classical functions also do this to a degree, but they only suspend for the duration of the call 
of functions that run inside them. Classical functions form a stack of data with their frames,
where coroutines have heap-allocated frames without a clear stack-like order. A classical
function is suspended when it calls another function and resumed when the called function
returns. Its frame, or state if you will, is created when the function is called and destroyed
when the function returns. A coroutine has more suspension points, normal functions can be called
but there are two more suspension points added: co_await and co_yield, in both, the state is left
in the coroutine frame and the execution is continued outside the coroutine. In the case of this
library co_await suspends the current coroutine until the 'called' awaiter is complete and the
co_yield suspends the current coroutine, letting the 'calling' coroutine to continue. The main
point of coroutines is that while a coroutine waits to be resumed another one can be executed,
obtaining an asynchronous execution similar to threads but on a single thread (Those can also be
usually run on multiple threads, but that is not supported by this library), this almost
eliminates the need to use synchronization mechanisms.

Let's consider an example of a coroutine calling another coroutine:

```cpp
14: colib::task<int32_t> get_messages() {
15:     int value;
16: 
17:     while (true) {
18:         value = co_await get_message();
19:         if (value == 0)
20:             break;
21:         co_yield value;
22:     }
22:     co_return 0;
23: }
```

At line 11, the coroutine is declared. As you can see, coroutines need to declare their return
value of the type of their handler object, namely colib::task<Type>. That is because the
coroutine holds the return value inside the state of the coroutine, and the user only gets the
handler to the coroutine.

At line 15, another awaiter, in this case another coroutine, is awaited with the use of co_await.
This will suspend the get_messages coroutine at that point, letting other coroutines on the
system run if there are any that need to do work, or block until a coroutine gets work to do.
Finally, this coroutine continues to line 16 when a message becomes available. Note that this
continuation will happen if a) there are no things to do or b) if another coroutine awaits
something and this one is the next that waits for execution.

Assuming value is not 0, the coroutine yields at line 18, returning the value but keeping its
state. This state contains the variable value and some other internals of coroutines.

When the message 0 is received, the coroutine returns 0, freeing its internal state. You
shouldn't call the coroutine anymore after this point.

```cpp
24: colib::task<int32_t> co_main() {
25:     colib::task<int32_t> messages = get_messages();
26:     while (int32_t value = co_await messages) {
27:         printf("main: %d\n", value);
28:         if (!value)
29:             break;
30:     }
31:     co_return 0;
32: }
```

The coroutine that calls the above coroutine is co_main. You can observe the creation of the
coroutine at line 25; what looks like a call of the coroutine in fact allocates the coroutine
state and returns the handle that can be further awaited, as you can see in the for loop at
line 26.

The coroutine will be called until value is 0, in which case we know that the coroutine has ended
(from its code) and we break from the for loop.

We observe that at line 31 we co_return 0; that is because the co_return is mandatory at the end
of coroutines (as mandated by the language).

```cpp
 0: int cnt = 3;
 1: colib::task<int32_t> get_message() {
 2:     co_await colib::sleep_s(1);
 3:     co_return cnt--;
 4: }
 5: 
 6: colib::task<int32_t> co_timer() {
 7:     int x = 50;
 8:     while (x > 0) {
 9:         printf("timer: %d\n", x--);
10:         co_await colib::sleep_ms(100);
11:     }
12:     co_return 0;
13: }
```

Now we can look at an example for get_message at line 1. Of course, in a real case, we would 
await a message from a socket, for some device, etc., but here we simply wait for a timer of 1
second to finish.

As for an example of something that can happen between awaits, we can look at co_timer at line 6.
This is another coroutine that prints x and waits 100 ms, 50 times. If you copy and run the
message yourself, you will see that the prints from the co_timer are more frequent and in-between
the ones from co_main.

```cpp
33: int main() {
34:     colib::pool_p pool = colib::create_pool();
35:     pool->sched(co_main());
36:     pool->sched(co_timer());
37:     pool->run();
38: }
```

Finally, we can look at main. As you can see, we create the pool at line 34, schedule the main
coroutine and the timer one, and we wait on the pool. The run function won't exit unless there
are no more coroutines to run or, as we will see later on, if a force_awake is called, or if an
error occurs.

Library Layout
==============

The library is split in four main sections:
    1. The documentation
    2. Macros and structs/types
    3. Function definitions
    4. Implementation

Task
====

As explained, each coroutine has an internal state. This state remembers the return value of the
function, its local variables, and some other coroutine-specific information. All of these are
remembered inside the coroutine promise. Each coroutine has, inside its promise, a state_t state
that remembers important information for its scheduling within this library. You can obtain this
state by (await get_state()) inside the respective coroutine for which you want to obtain the
state pointer. This state will live for as long as the coroutine lives, but you would usually
ignore its existence. The single instance for which you would use the state is if you are using
modifications (see below).

To each such promise (state, return value, local variables, etc.), the language assigns a handle
in the form of std::coroutine_handle<PromiseType>. These handles are further managed by tasks
inside this library. So, for a coroutine, you will get a task as a handle. The task further
specifies the type of the promise and implicitly the return value of the coroutine, but you don't
need to bother with those details.

A task is also an awaitable. As such, when you await it, it calls or resumes the awaited
coroutine, depending on the state it was left in. The await operation will resume the caller
either on a co_yield (the C++ yield; colib::yield does something else) or on a co_return of the
callee. In the latter case, the awaited coroutine is also destroyed, and further awaits on its
task are undefined behavior.

The task type, as in colib::task<Type>, is the type of the return value of the coroutine.

Pool
====

For a task, the pool is its running space. A task runs on a pool along with other tasks. This
pool can be run only on one thread, i.e., there are no thread synchronization primitives used,
except in the case of COLIB_ENABLE_MULTITHREAD_SCHED.

The pool remembers the coroutines that are ready and resumes them when the currently running
coroutine yields to wait for some event (as in colib::yield). The pool also holds the allocator
used internally and the io_pool and timers, which are explained below and are responsible for
managing the asynchronous waits in the library.

A task remembers the pool it was scheduled on while either (co_await colib::sched(task)) or
pool_t::sched(task) are used on the respective task.

There are many instances where there are two variants of a function: one where the function has
the pool as an argument and another where that argument is omitted, but the function is in fact a
coroutine that needs to be awaited. Inside a coroutine, using await on a function, the pool is
deduced automatically from the running coroutine.

From inside a running coroutine, you can use (co_await colib::get_pool()) to get the pool of the
running coroutine.

Semaphores
==========

Semaphores are created by using the function/coroutine create_sem and are handled by using
sem_p smart pointers. They have a counter that can be increased by signaling them or decreased by
one if the counter is bigger than 0 by using wait. In case the counter is 0 or less than 0, the
wait function blocks until the semaphore is signaled. In this library, semaphores are a bit
unusual, as they can be initialized to a value that is less than 0 so that multiple awaiters can
wait for a task to finish.

IO Pool
==========

Inside the pool, there is an Input/Output event pool that implements the operating
system-specific asynchronous mechanism within this library. It offers a way to notify a single
function for multiple requested events to either be ready or completed in conjunction with a
state_t \*. In other words, we add pairs of the form (io_desc_t, state_t \*) and wait on a function
for any of the operations described by io_desc_t to be completed. We do this in a single place to
wait for all events at once.

On Linux, the epoll_\* functions are used, and on Windows, the IO Completion Port mechanism is
used.

Of course, all these operations are done internally.

Allocator
=========

Another internal component of the pool is the allocator. Because many of the internals of
coroutines have the same small memory footprint and are allocated and deallocated many times, an
allocator was implemented that keeps the allocated objects together and also ignores some costs
associated with new or malloc. This allocator can be configured (COLIB_ALLOCATOR_SCALE) to hold
more or less memory, as needed, or ignored completely (COLIB_DISABLE_ALLOCATOR), with malloc
being used as an alternative. If the memory given to the allocator is used up, malloc is used for
further allocations.

Timers
======

Another internal component of the pool is the timer_pool_t. This component is responsible
for implementing and managing OS-dependent timers that can run with the IO pool. There are a
limited number of these timers allocated, which limits the maximum number of concurrent sleeps.
This number can be increased by changing COLIB_MAX_TIMER_POOL_SIZE.

Modifs
======

Modifications are callbacks attached to coroutines that are called in specific cases:
on suspend/resume, on call/sched (after a valid state_t is created), on IO wait (on both wait and
finish wait), and on semaphore wait and unwait.

These callbacks can be used to monitor the coroutines, to acquire resources before re-entering a
coroutine, etc. (Internally, these are used for some functions; be aware while replacing existing
ones not to break the library's modifications).

Modifications can be inherited by coroutines in two cases: on call and on sched. More precisely,
each modification can be inherited by a coroutine scheduled from this one or called from this
one. You can modify the modifications for each coroutine using its task to get/add/remove
modifications or awaiters from inside the current coroutine.

Debugging
=========

Sometimes unwanted behavior can occur. If that happens, it may be debugged using the internal
helpers, those are:
  - dbg_enum            - get the description of a library enum code
  - dbg_name            - when COLIB_ENABLE_DEBUG_NAMES is true, it can be used to get the name
                          associated with a task, a coroutine handle or a coroutine promise
                          address, those can be registered with COLIB_REGNAME or
                          dbg_register_name
  - dbg_create_tracer   - creates a modif_pack_t that can be attached to a coroutine to debug all
                          the coroutine that it calls or schedules
  - log_str             - the function that is used to print a logging string (user can change
                          it)
  - dbg                 - the function used to log a formatted string
  - dbg_format          - the function used to format a string

All those are enabled by COLIB_ENABLE_LOGGING true, else those are disabled.

Config Macros
=============

| Macro Name                     | Type | Default    | Description                              |
|--------------------------------|------|------------|------------------------------------------|
| COLIB_OS_LINUX                 | BOOL | auto-detect| If true, the library provided Linux      |
|                                |      |            | implementation will be used to implement |
|                                |      |            | the IO pool and timers.                  |
| COLIB_OS_WINDOWS               | BOOL | auto-detect| If true, the library provided Windows    |
|                                |      |            | implementation will be used to implement |
|                                |      |            | the IO pool and timers.                  |
| COLIB_OS_UNKNOWN               | BOOL | false      | If true, the user provided implementation|
|                                |      |            | will be used to implement the IO pool and|
|                                |      |            | timers. In this case                     |
|                                |      |            | COLIB_OS_UNKNOWN_IO_DESC and             |
|                                |      |            | COLIB_OS_UNKNOWN_IMPLEMENTATION must be  |
|                                |      |            | defined.                                 |
| COLIB_OS_UNKNOWN_IO_DESC       | CODE | undefined  | This define must be filled with the code |
|                                |      |            | necessary for the struct io_desc_t, use  |
|                                |      |            | the Linux/Windows implementations as     |
|                                |      |            | examples.                                |
| COLIB_OS_UNKNOWN_IMPLEMENTATION| CODE | undefined  | This define must be filled with the code |
|                                |      |            | necessary for the structs timer_pool_t   |
|                                |      |            | and io_pool_t, use the Linux/Windows     |
|                                |      |            | implementations as examples.             |
| COLIB_MAX_TIMER_POOL_SIZE      | INT  | 64         | The maximum number of concurrent sleeps. |
|                                |      |            | (Only for Linux)                         |
| COLIB_MAX_FAST_FD_CACHE        | INT  | 1024       | The maximum file descriptor number to    |
|                                |      |            | hold in a fast access path, the rest will|
|                                |      |            | be held in a map. Only for Linux, on     |
|                                |      |            | Windows all are held in a map.           |
| COLIB_ENABLE_MULTITHREAD_SCHED | BOOL | false      | If true, pool_t::thread_sched can be used|
|                                |      |            | from another thread to schedule a        |
|                                |      |            | coroutine in the same way pool_t::sched  |
|                                |      |            | is used, except, modifications can't be  |
|                                |      |            | added from that schedule point.          |
| COLIB_ENABLE_LOGGING           | BOOL | true       | If true, coroutines will use log_str to  |
|                                |      |            | print/log error strings.                 |
| COLIB_ENABLE_DEBUG_TRACE_ALL   | BOOL | false      | TODO: If true, all coroutines will have a|
|                                |      |            | debug tracer modification that would     |
|                                |      |            | print on the given modif points          |
| COLIB_DISABLE_ALLOCATOR        | BOOL | false      | If true, the allocator will be disabled  |
|                                |      |            | and malloc will be used instead.         |
| COLIB_ALLOCATOR_SCALE          | INT  | 16         | Scales all memory buckets inside the     |
|                                |      |            | allocator.                               |
| COLIB_ALLOCATOR_REPLACE        | BOOL | false      | If true, COLIB_ALLOCATOR_REPLACE_IMPL_1  |
|                                |      |            | and COLIB_ALLOCATOR_REPLACE_IMPL_2 must  |
|                                |      |            | be defined. As a result, the allocator   |
|                                |      |            | will be replaced with the provided       |
|                                |      |            | implementation.                          |
| COLIB_ALLOCATOR_REPLACE_IMPL_1 | CODE | undefined  | This define must be filled with the code |
|                                |      |            | necessary for the struct                 |
|                                |      |            | allocator_memory_t and alloc,            |
|                                |      |            | dealloc_create functions, use the        |
|                                |      |            | provided implementations as examples.    |
| COLIB_ALLOCATOR_REPLACE_IMPL_2 | CODE | undefined  | This define must be filled with the code |
|                                |      |            | necessary for the allocate/deallocate    |
|                                |      |            | functions, use the provided              |
|                                |      |            | implementations as examples.             |
| COLIB_WIN_ENABLE_SLEEP_AWAKE   | BOOL | false      | Sets the last parameter of the function  |
|                                |      |            | SetWaitableTimer to true or false,       |
|                                |      |            | depending on the value. This define is   |
|                                |      |            | used for timers on Windows.              |
| COLIB_ENABLE_DEBUG_NAMES       | BOOL | false      | If true you can also define COLIB_REGNAME|
|                                |      |            | and use it to register a coroutine's name|
|                                |      |            | (a colib::task<T>, std::coroutine_handle |
|                                |      |            | or void *). COLIB_REGNAME is auto-defined|
|                                |      |            | to use colib::dbg_register_name.         |

### Organization

- `colib.h` - Single-header implementation of the coroutine library.
- `tests.cpp` - Contains the tests for the library.
- `LICENSE` - The MIT license.
- `makefile` - The makefile used to build the tests.
- `a.out` or `tests.exe` - The resulting test executables.
