/*
MIT License

Copyright (c) 2025, Andrei Pangratie

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef COLIB_H
#define COLIB_H

/*!  DOCUMENTATION
====================================================================================================
====================================================================================================
====================================================================================================

1. Introduction
===============

In C++20 a new feature was added, namely coroutines. This feature allows the creation of a kind of
special function named a coroutine that can have its execution suspended and resumed later on.
Classical functions also do this to a degree, but they only suspend for the duration of the call of
functions that run inside them. Classical functions form a stack of data with their frames, where
coroutines have heap-allocated frames without a clear stack-like order. A classical function is
suspended when it calls another function and resumed when the called function returns. Its frame,
or state if you will, is created when the function is called and destroyed when the function returns.
A coroutine has more suspension points, normal functions can be called but there are two more
suspension points added: co_await and co_yield, in both, the state is left in the coroutine frame
and the execution is continued outside the coroutine. In the case of this library co_await suspends
the current coroutine until the 'called' awaiter is complete and the co_yield suspends the current
coroutine, letting the 'calling' coroutine to continue. The main point of coroutines is that while
a coroutine waits to be resumed another one can be executed, obtaining an asynchronous execution
similar to threads but on a single thread (Those can also be usually run on multiple threads,
but that is not supported by this library), this almost eliminates the need to use synchronization
mechanisms.

Let's consider an example of a coroutine calling another coroutine:

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

At line 11, the coroutine is declared. As you can see, coroutines need to declare their return value
of the type of their handler object, namely colib::task<Type>. That is because the coroutine holds the
return value inside the state of the coroutine, and the user only gets the handler to the coroutine.

At line 15, another awaiter, in this case another coroutine, is awaited with the use of co_await.
This will suspend the get_messages coroutine at that point, letting other coroutines on the system
run if there are any that need to do work, or block until a coroutine gets work to do. Finally,
this coroutine continues to line 16 when a message becomes available. Note that this continuation
will happen if a) there are no things to do or b) if another coroutine awaits something and this
one is the next that waits for execution.

Assuming value is not 0, the coroutine yields at line 18, returning the value but keeping its state.
This state contains the variable value and some other internals of coroutines.

When the message 0 is received, the coroutine returns 0, freeing its internal state. You shouldn't
call the coroutine anymore after this point.

24: colib::task<int32_t> co_main() {
25:     colib::task<int32_t> coro = get_messages();
26:     for (int32_t value = co_await coro; value; value = co_await coro) {
27:         printf("main: %d\n", value);
28:         if (!value)
29:             break;
30:     }
31:     co_return 0;
32: }

The coroutine that calls the above coroutine is co_main. You can observe the creation of the
coroutine at line 25; what looks like a call of the coroutine in fact allocates the coroutine state
and returns the handle that can be further awaited, as you can see in the for loop at line 26.

The coroutine will be called until value is 0, in which case we know that the coroutine has ended
(from its code) and we break from the for loop.

We observe that at line 31 we co_return 0; that is because the co_return is mandatory at the end of
coroutines (as mandated by the language).

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

Now we can look at an example for get_message at line 1. Of course, in a real case, we would await a
message from a socket, for some device, etc., but here we simply wait for a timer of 1 second to
finish.

As for an example of something that can happen between awaits, we can look at co_timer at line 6.
This is another coroutine that prints x and waits 100 ms, 50 times. If you copy and run the message
yourself, you will see that the prints from the co_timer are more frequent and in-between the ones
from co_main.

33: int main() {
34:     colib::pool_p pool = colib::create_pool();
35:     pool->sched(co_main());
36:     pool->sched(co_timer());
37:     pool->run();
38: }

Finally, we can look at main. As you can see, we create the pool at line 34, schedule the main
coroutine and the timer one, and we wait on the pool. The run function won't exit unless there are
no more coroutines to run or, as we will see later on, if a force_awake is called, or if an
error occurs.

2. Library Layout
=================

The library is split in four main sections:
    1. The documentation
    2. Macros and structs/types
    3. Function definitions
    4. Implementation

3. Task
=======

As explained, each coroutine has an internal state. This state remembers the return value of the
function, its local variables, and some other coroutine-specific information. All of these are
remembered inside the coroutine promise. Each coroutine has, inside its promise, a state_t state
that remembers important information for its scheduling within this library. You can obtain this
state by (await get_state()) inside the respective coroutine for which you want to obtain the state
pointer. This state will live for as long as the coroutine lives, but you would usually ignore
its existence. The single instance for which you would use the state is if you are using
modifications (see below).

To each such promise (state, return value, local variables, etc.), the language assigns a handle in
the form of std::coroutine_handle<PromiseType>. These handles are further managed by tasks inside
this library. So, for a coroutine, you will get a task as a handle. The task further specifies the
type of the promise and implicitly the return value of the coroutine, but you don't need to bother
with those details.

A task is also an awaitable. As such, when you await it, it calls or resumes the awaited coroutine,
depending on the state it was left in. The await operation will resume the caller either on a
co_yield (the C++ yield; colib::yield does something else) or on a co_return of the callee. In the
latter case, the awaited coroutine is also destroyed, and further awaits on its task are undefined
behavior.

The task type, as in colib::task<Type>, is the type of the return value of the coroutine.

4. Pool
=======

For a task, the pool is its running space. A task runs on a pool along with other tasks. This pool
can be run only on one thread, i.e., there are no thread synchronization primitives used, except in
the case of COLIB_ENABLE_MULTITHREAD_SCHED.

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

5. Semaphores
=============

Semaphores are created by using the function/coroutine create_sem and are handled by using
sem_p smart pointers. They have a counter that can be increased by signaling them or decreased by
one if the counter is bigger than 0 by using wait. In case the counter is 0 or less than 0, the wait
function blocks until the semaphore is signaled. In this library, semaphores are a bit unusual, as
they can be initialized to a value that is less than 0 so that multiple awaiters can wait for a
task to finish.

6. IO Pool
==========

Inside the pool, there is an Input/Output event pool that implements the operating system-specific
asynchronous mechanism within this library. It offers a way to notify a single function for
multiple requested events to either be ready or completed in conjunction with a state_t *.
In other words, we add pairs of the form (io_desc_t, state_t *) and wait on a function for any of
the operations described by io_desc_t to be completed. We do this in a single place to wait for all
events at once.

On Linux, the epoll_* functions are used, and on Windows, the IO Completion Port mechanism is used.

Of course, all these operations are done internally.

7. Allocator
============

Another internal component of the pool is the allocator. Because many of the internals of coroutines
have the same small memory footprint and are allocated and deallocated many times, an allocator was
implemented that keeps the allocated objects together and also ignores some costs associated with
new or malloc. This allocator can be configured (COLIB_ALLOCATOR_SCALE) to hold more or less memory,
as needed, or ignored completely (COLIB_DISABLE_ALLOCATOR), with malloc being used as an alternative.
If the memory given to the allocator is used up, malloc is used for further allocations.

8. Timers
=========

Another internal component of the pool is the timer_pool_t. This component is responsible
for implementing and managing OS-dependent timers that can run with the IO pool. There are a limited
number of these timers allocated, which limits the maximum number of concurrent sleeps. This number
can be increased by changing COLIB_MAX_TIMER_POOL_SIZE.

9. Modifs
=========

Modifications are callbacks attached to coroutines that are called in specific cases:
on suspend/resume, on call/sched (after a valid state_t is created), on IO wait (on both wait and
finish wait), and on semaphore wait and unwait.

These callbacks can be used to monitor the coroutines, to acquire resources before re-entering a
coroutine, etc. (Internally, these are used for some functions; be aware while replacing existing
ones not to break the library's modifications).

Modifications can be inherited by coroutines in two cases: on call and on sched. More precisely,
each modification can be inherited by a coroutine scheduled from this one or called from this one.
You can modify the modifications for each coroutine using its task to get/add/remove modifications
or awaiters from inside the current coroutine.

10. Debugging
============

Sometimes unwanted behavior can occur. If that happens, it may be debugged using the internal
helpers, those are:
    dbg_enum            - get the description of a library enum code
    dbg_name            - when COLIB_ENABLE_DEBUG_NAMES is true, it can be used to get the name
                          associated with a task, a coroutine handle or a coroutine promise address,
                          those can be registered with COLIB_REGNAME or dbg_register_name
    dbg_create_tracer   - creates a modif_pack_t that can be attached to a coroutine to debug all
                          the coroutine that it calls or schedules
    log_str             - the function that is used to print a logging string (user can change it)
    dbg                 - the function used to log a formatted string
    dbg_format          - the function used to format a string

All those are enabled by COLIB_ENABLE_LOGGING true, else those are disabled.

11. Config Macros
=================

|--------------------------------|------|---------------|------------------------------------------|
| Macro Name                     | Type | Default Value | Description                              |
|================================|======|===============|==========================================|
| COLIB_OS_LINUX                 | BOOL | auto-detect   | If true, the library provided Linux      |
|                                |      |               | implementation will be used to implement |
|                                |      |               | the IO pool and timers.                  |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_OS_WINDOWS               | BOOL | auto-detect   | If true, the library provided Windows    |
|                                |      |               | implementation will be used to implement |
|                                |      |               | the IO pool and timers.                  |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_OS_UNKNOWN               | BOOL | false         | If true, the user provided implementation|
|                                |      |               | will be used to implement the IO pool and|
|                                |      |               | timers. In this case                     |
|                                |      |               | COLIB_OS_UNKNOWN_IO_DESC and             |
|                                |      |               | COLIB_OS_UNKNOWN_IMPLEMENTATION must be  |
|                                |      |               | defined.                                 |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_OS_UNKNOWN_IO_DESC       | CODE | undefined     | This define must be filled with the code |
|                                |      |               | necessary for the struct io_desc_t, use  |
|                                |      |               | the Linux/Windows implementations as     |
|                                |      |               | examples.                                |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_OS_UNKNOWN_IMPLEMENTATION| CODE | undefined     | This define must be filled with the code |
|                                |      |               | necessary for the structs timer_pool_t   |
|                                |      |               | and io_pool_t, use the Linux/Windows     |
|                                |      |               | implementations as examples.             |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_MAX_TIMER_POOL_SIZE      | INT  | 64            | The maximum number of concurrent sleeps. |
|                                |      |               | (Only for Linux)                         |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_MAX_FAST_FD_CACHE        | INT  | 1024          | The maximum file descriptor number to    |
|                                |      |               | hold in a fast access path, the rest will|
|                                |      |               | be held in a map. Only for Linux, on     |
|                                |      |               | Windows all are held in a map.           |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ENABLE_MULTITHREAD_SCHED | BOOL | false         | If true, pool_t::thread_sched can be used|
|                                |      |               | from another thread to schedule a        |
|                                |      |               | coroutine in the same way pool_t::sched  |
|                                |      |               | is used, except, modifications can't be  |
|                                |      |               | added from that schedule point.          |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ENABLE_LOGGING           | BOOL | true          | If true, coroutines will use log_str to  |
|                                |      |               | print/log error strings.                 |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ENABLE_DEBUG_TRACE_ALL   | BOOL | false         | TODO: If true, all coroutines will have a|
|                                |      |               | debug tracer modification that would     |
|                                |      |               | print on the given modif points          |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_DISABLE_ALLOCATOR        | BOOL | false         | If true, the allocator will be disabled  |
|                                |      |               | and malloc will be used instead.         |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ALLOCATOR_SCALE          | INT  | 16            | Scales all memory buckets inside the     |
|                                |      |               | allocator.                               |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ALLOCATOR_REPLACE        | BOOL | false         | If true, COLIB_ALLOCATOR_REPLACE_IMPL_1  |
|                                |      |               | and COLIB_ALLOCATOR_REPLACE_IMPL_2 must  |
|                                |      |               | be defined. As a result, the allocator   |
|                                |      |               | will be replaced with the provided       |
|                                |      |               | implementation.                          |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ALLOCATOR_REPLACE_IMPL_1 | CODE | undefined     | This define must be filled with the code |
|                                |      |               | necessary for the struct                 |
|                                |      |               | allocator_memory_t and alloc,            |
|                                |      |               | dealloc_create functions, use the        |
|                                |      |               | provided implementations as examples.    |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_ALLOCATOR_REPLACE_IMPL_2 | CODE | undefined     | This define must be filled with the code |
|                                |      |               | necessary for the allocate/deallocate    |
|                                |      |               | functions, use the provided              |
|                                |      |               | implementations as examples.             |
|--------------------------------|------|---------------|------------------------------------------|
| COLIB_WIN_ENABLE_SLEEP_AWAKE   | BOOL | false         | Sets the last parameter of the function  |
|                                |      |               | SetWaitableTimer to true or false,       |
|                                |      |               | depending on the value. This function is |
|                                |      |               | used for timers on Windows.              |
|--------------------------------|------|---------------|------------------------------------------|

12. API
=======

12.1 Object types
-----------------

pool_p
    Smart pointer handle to the pool object. When destroyed, the pool is also destroyed. You must
    keep the pool alive while corutines are running and while semaphore exist

sem_p
    Smart pointer handle to the semaphore object. When destroyed, the semaphore is destroyed. It
    is undefined behaviour to destroy the semaphore while a coroutine is waiting on it.

modif_p
    Smart pointer handle to a single modification. Ownership is transfered to the corutine when
    attached.

modif_pack_t
    A vector consisting of modif_p-s. Functions receive those packs togheter to ease use.

task<T>
    The task handle of a coroutine, T is the return type of the respective coroutine.

12.2 Enums and structs
----------------------

struct io_desc_t
    This is the structure that describes an I/O operation, OS-dependent, used internally to
    handle I/O operations.

    On Linux it consists of:
        1. a file descriptor (fd)
        2. epoll event type (events)

    On Windows it consists of:
        1. a file handle (h)
        2. a smart pointer to an internal `io_data_t` structure

    The `io_data_t` structure holds the state of the I/O operation:
        1. the OVERLAPPED structure (check IOCP documentation)
        2. flags - a mostly internal field that would normally be `IO_FLAG_NONE` that holds the
            state type of the I/O operation
        3. recvlen - the byte transfer count
        4. io_request - the actual action to be performed
        5. user pointer to be passed to the `io_request`
        6. a copy of the file handle

The smart pointer must be null for the function `stop_handle` to work.

struct state_t
    This structure, as explained above, is the common type for all coroutines from this library. It
    also holds a user pointer user_ptr that can be used. This pointer can be useful when working
    with modifications.

enum error_e
    Most of the functions from this library return this error type. Warnings or non-errors are
    positive, while errors are negative.

enum run_e
    This is the return value of the pool_t::run.

enum modif_e
    This is the modification type of the modification and it describes the place that this
    modification should be called from.

enum modif_flags_e
    This selects the inheritance mode of the modification; values can be OR-ed together.

12.3 Member functions and vars
------------------------------

pool_t::sched(task<T> task, const modif_pack_t& v) -> void
    Schedules the task with the modifications specified in v to be executed on the pool. That is, it
    adds the task to the ready_queue.

pool_t::run()
    Runs the first coroutine in the ready queue. When this coroutine awaits something, the next
    one will be scheduled. Will keep running until there are no more I/O events to wait for, no more
    timers to sleep on, no more coroutines to run, or force_stop is used. Returns RUN_OK if no error
    occurred during the run. This will block the thread that executed the run.

pool_t::clear()
    Destroys all the coroutines that are attached to this pool, meaning those in the ready queue,
    those waiting for I/O operations, and those waiting for semaphores.

pool_t::stop_io(const io_desc_t&) -> error_e
    Takes as an argument a valid io_desc_t and stops the operation described by the descriptor
    on the respective handle.

pool_t::thread_sched
    Similar to pool_t::sched, can't add modifications with it. Must have
    COLIB_ENABLE_MULTITHREAD_SCHED set to true and can be used from other threads.

pool_t::user_ptr
    This is a pointer that you can use however you want.

sem_t::wait() ~> sem_t::unlocker_t
    Returns an awaiter that can be awaited to decrement the internal counter of the semaphore.

    This awaiter object returns an unlocker that has the `lock` member function doing nothing
    and `unlock` function calling `signal` on the semaphore, meaning it can be used inside a
    `std::lock_guard` object to protect a piece of code using the RAII principle.

sem_t::signal(increment)
    If increment is less than 0, then it will decrease the internal counter with the amount.
    If increment is 0 and the internal counter is less then or equal to 0 then it will awake all the
    waiters, else it does nothing.
    If the increment is bigger than 0 it increases the internal counter and awakes waiters until
    either there are no more waiters or the internal counter is 0.

sem_t::try_dec() -> bool
    Non-blocking; if the semaphore counter is positive, decrements the counter and returns true,
    else returns false.

12.4 Function and coroutines/awaitables
---------------------------------------

Here -> T denotes the returned value T of a function and ~> T the return value T of a coroutine or
awaitable, i.e. it needs co_await to get the value and to execute.

create_pool() -> pool_p
    Creates the pool object, returning the pool_p handle to the pool. Allocates space for the
    allocator and initiates diverese functions of the pool.

get_pool() ~> pool_t *
    Awaitable that returns the pointer of the pool coresponding to the coroutine from which this
    function is called from

get_state() ~> state_t *
    Awaitable that returns the pointer of the state_t of the current coroutine

sched(task<T> task, const modif_pack_t& v) ~> void
    Does the same thing as pool_t::sched, on the running coroutine's pool.

yield() ~> void
    Suspends the current coroutine and moves it to the end of the ready queue within its
    associated pool.

await(Awaitable) ~> task<int>
    Helper coroutine function, given an awaitable, awaits it inside the coroutine await,
    usefull if the awaitable can't be decorated, bacause it isn't a coroutine.

create_timeo(task<T> t, pool, timeo_ms) -> task<std::pair<T, error_e>>
    Schedules the task `t` and a timer that kills the task `t`, if `t` doesn't finish before the
    timer expires in timeo_ms milliseconds. This function returns a coroutine that can be awaited
    to get the return value and error value. If the error value is not ERROR_OK, than the task
    `t` wasn't executed succesfully.

sleep_us ~> void
sleep_ms ~> void
sleep_s  ~> void
sleep    ~> void
    Awaitable coroutines that sleep for the given duration in microseconds, milliseconds, seconds
    or c++ duration. The precision with which this sleep occours is given by the h

create_sem(pool_p, int64_t initial_val)     -> sem_p
create_sem(pool_t *, int64_t initial_val)   -> sem_p
create_sem(int64_t initial_val)             ~> sem_p
    Those functions create a semaphore with the initial value set to initial_value. They need the
    pool and the last variant of this function can deduce it from the coroutine context.

create_killer(pool_t *pool, error_e e) -> std::pair<modif_pack_t, std::function<error_e(void)>>
    Creates a modification pack that can be added to only one coroutine that is associated with
    the given pool. The second parameter e will be the error value of the coroutine. The
    returned function can be called to kill the given coroutine and it's entire call stack (does
    not kill sched stack).

create_future(pool_t *pool, task<T> t) -> task<T>
    Takes a task and adds the requred modifications to it such that the returned object will be
    returned once the return value of the task is available so:
    
    1: auto t = co_task();
    2: auto fut = colib::create_future(t)
    3: co_await colib::sched(t);
    4: // ...
    5: co_await fut; // returns the value of co_task once it has finished executing 

wait_all(task<ret_v>... tasks)
    Wait for all the tasks to finish, the return value can be found in the respective task, killing
    one kills all (sig_killer installed in all). The inheritance is the same as with 'call'.

force_stop(value) ~> errno_e
    Causes the running pool::run to stop, the function will stop at this point, can be resumed with
    another run

wait_event(io_desc) ~> errno_e
    Waits for the described event to be available/finish, depending on the OS. Usefull for 

stop_io(io_desc) -> errno_e
    Stops the given I/O event, described by io_desc, by canceling it's wait and making the awaitable
    give an error.

connect(handle, sockaddr *sa, socklen_t *len/int len)
accept(handle, sockaddr *sa, socklen_t *len)
    Calls system connect/accept using coroutines

read(handle, buffer, len) ~> errno_e
write(handle, buffer, len) ~> errno_e
    Calls the system read/write using coroutines

read_sz(handle, buffer, len) ~> errno_e
write_sz(handle, buffer, len) ~> errno_e
    Same as the sistem calls, just they wait for the exact leght to be sent/received. Those also
    give an error if the connection is closed during the operation.

create_modif<modif_type>(pool, modif_flags_e, cbk) -> modif_p
    Creates a modification that will be executed on the given modif_type, inherited by the rules
    specified inside modif_flags and on the given pool. It will execute the callback cbk at those
    points.

task_modifs(task) -> modif_pack_t
task_modifs()     ~> modif_pack_t
    Given a task or on the running coroutine's task, get the modifications that it has.

add_modifs(pool, task<T>, std::set<modif_p> mods) -> task<T>
add_modifs(std::set<modif_p> mods)                ~> task<T>
    Adds the mods to the specified task (implicit or explicit), given the specified pool (implicit
    or explicit) and returns the task in question.

rm_modifs(task<T>, std::set<modif_p> mods) -> task<T>
rm_modifs(std::set<modif_p> mods)          ~> task<T>
    Removes the modifications from the specified task (implicit or explicit) and returns the task in
    question.

stop_fd(int fd) ~> error_e
    Linux specific, is used to evict an fd from the epoll engine before closing it, you shouldn't
    close a file descriptor before removing it from the pool.

stop_handle(HANDLE h)
    Windows specific, is used to evict an HANDLE h from the iocp engine before closing it,
    you shouldn't close a handle before removing it from the pool.
    
ConnectEx(...) ~> BOOL
WSARecv(...) ~> BOOL
WSARecvMsg(...) ~> BOOL
WSARecvFrom(...) ~> BOOL
WSASend(...) ~> BOOL
WSASendTo(...) ~> BOOL
WSASendMsg(...) ~> BOOL
WriteFile(...) ~> BOOL
WaitCommEvent(...) ~> BOOL
TransactNamedPipe(...) ~> BOOL
ReadFile(...) ~> BOOL
ReadDirectoryChangesW(...) ~> BOOL
LockFileEx(...) ~> BOOL
DeviceIoControl(...) ~> BOOL
ConnectNamedPipe(...) ~> BOOL
AcceptEx(...) ~> BOOL
    All those functions are calling their WinAPI counterpart, but in coroutine context and they
    all are missing the OVERLAPPED pointer because that one is owned by the I/O engine. Some
    of them also offer a parameter named *offset, for functions that need the offset from inside
    the OVERLAPPED structure, that pointer's contents will be copied inside the overlapped structure
    and copied out ov the overlapped structure after the call is done. They require a handle that
    is compatible with iocp and they will attach the handle to the iocp instance. Those are the
    functions listed by msdn to work with iocp (and connect, that is part of an extension)

    HEADER
====================================================================================================
====================================================================================================
====================================================================================================
*/

#include <array>
#include <chrono>
#include <cinttypes>
#include <coroutine>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <source_location>
#include <stack>
#include <string.h>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifndef COLIB_OS_LINUX
# ifdef __linux__
#  define COLIB_OS_LINUX true
# else
#  define COLIB_OS_LINUX false
# endif
#endif

#ifndef COLIB_OS_WINDOWS
# if (defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)) && !COLIB_OS_LINUX
#  define COLIB_OS_WINDOWS true
# else
#  define COLIB_OS_WINDOWS false
# endif
#endif

#ifndef COLIB_OS_UNKNOWN
# define COLIB_OS_UNKNOWN false
# define COLIB_OS_UNKNOWN_IMPLEMENTATION ;
# define COLIB_OS_UNKNOWN_IO_DESC ;
#endif

#if COLIB_OS_LINUX
# include <fcntl.h>
# include <unistd.h>
# include <sys/epoll.h>
# include <sys/socket.h>
# include <sys/timerfd.h>
#endif

#if COLIB_OS_WINDOWS
# include <winsock2.h>
# include <mswsock.h>
# include <windows.h>
#endif

/*! The version is formated as MAJOR,MINOR,DETAIL, where:
    @MAJOR  - breaking changes to the interface: functions are deleted, functionality is added or
              removed to already existing functions, etc.
    @MINOR  - changes to the interface: functions are added, functionality is added to new
              functons, additional default parameters are added, etc.
    @DETAIL - fixes, implementation fixes, comments, etc. Changes that won't change the way you
              use this library */
#define COLIB_VERSION 0,0,1

#if COLIB_OS_UNKNOWN
/* you should include your needed files before including this file */
#endif

#ifndef COLIB_MAX_TIMER_POOL_SIZE
# define COLIB_MAX_TIMER_POOL_SIZE 64
#endif

#ifndef COLIB_MAX_FAST_FD_CACHE
# define COLIB_MAX_FAST_FD_CACHE 1024
#endif

#ifndef COLIB_ENABLE_MULTITHREAD_SCHED
# define COLIB_ENABLE_MULTITHREAD_SCHED false
#endif

#ifndef COLIB_ENABLE_LOGGING
# define COLIB_ENABLE_LOGGING true
#endif

#if COLIB_ENABLE_LOGGING
# define COLIB_DEBUG(fmt, ...) dbg(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)
#else
# define COLIB_DEBUG(fmt, ...) do {} while (0)
#endif

#ifndef COLIB_ENABLE_DEBUG_NAMES
# define COLIB_ENABLE_DEBUG_NAMES false
#endif

#ifndef COLIB_ENABLE_DEBUG_TRACE_ALL
# define COLIB_ENABLE_DEBUG_TRACE_ALL false
#endif

#ifndef COLIB_DISABLE_ALLOCATOR
# define COLIB_DISABLE_ALLOCATOR false
#endif

#ifndef COLIB_ALLOCATOR_SCALE
# define COLIB_ALLOCATOR_SCALE 16
#endif

#ifndef COLIB_ALLOCATOR_REPLACE
# define COLIB_ALLOCATOR_REPLACE false
# define COLIB_ALLOCATOR_REPLACE_IMPL_1
# define COLIB_ALLOCATOR_REPLACE_IMPL_2
#endif

#ifndef COLIB_WIN_ENABLE_SLEEP_AWAKE
# define COLIB_WIN_ENABLE_SLEEP_AWAKE FALSE
#endif

/* If COLIB_ENABLE_DEBUG_NAMES you can also define COLIB_REGNAME and use it to register a
coroutine's name (a colib::task<T>, std::coroutine_handle or void *) */
#if COLIB_ENABLE_DEBUG_NAMES
# ifndef COLIB_REGNAME
#  define COLIB_REGNAME(thv)   colib::dbg_register_name((thv), "%20s:%5d:%s", __FILE__, __LINE__, #thv)
# endif
#else
# define COLIB_REGNAME(thv)    thv
#endif /* COLIB_ENABLE_DEBUG_NAMES */

namespace colib {

constexpr int MAX_TIMER_POOL_SIZE = COLIB_MAX_TIMER_POOL_SIZE;
constexpr int MAX_FAST_FD_CACHE = COLIB_MAX_FAST_FD_CACHE;

/* coroutine return type, this is the result of creating a coroutine, you can await it and also pass
it around (practically holds a std::coroutine_handle and can be awaited call the coro and to get the
return value) */
template<typename T> struct task;

/* A pool is the shared state between all coroutines. This object holds the epoll handler, timers,
queues, etc. Each corutine has a pointer to this object. You can see this object as an instance or
proxy of epoll/iocp. */
struct pool_t;

/* Modifs, corutine modifications. Those modifications controll the way a corutine behaves when
awaited or when spawning a new corutine from it. Those modifications can be inherited, making all
the corutines in the call sub-tree behave in a similar way. For example: adding a timeouts, for this
example, all the corutines that are on the same call-path(not sched-path) would have a timer, such
that the original call would not exist for more than X time units. (+/- the code between awaits) */
struct modif_t;

/* This is a private table that holds the modifications inside the corutine state */
struct modif_table_t;

/* This is a semaphore working on a pool. It can be awaited to decrement it's count and .signaled()
increments it's count from wherever. More above. */
struct sem_t;

/* Internal state of corutines that is independent of the return value of the corutine. */
struct state_t;

/* used pointers, _p marks a shared_pointer */
using sem_p = std::shared_ptr<sem_t>;
using pool_p = std::shared_ptr<pool_t>;
using modif_p = std::shared_ptr<modif_t>;
using modif_table_p = std::shared_ptr<modif_table_t>;

using modif_pack_t = std::vector<modif_p>;

/* Tasks errors */
enum error_e : int32_t {
    ERROR_YIELDED =  1, /* not really an error, but used to signal that the coro yielded */
    ERROR_OK      =  0,
    ERROR_GENERIC = -1, /* generic error, can use log_str to find the error, or sometimes errno */
    ERROR_TIMEO   = -2, /* the error comes from a modif, namely a timeout */
    ERROR_WAKEUP  = -3, /* the error comes from force awaking the awaiter */
    ERROR_USER    = -4, /* the error comes from a modif, namely an user defined modif, users can
                        use this if they wish to return from modif cbks */
    ERROR_DEPEND  = -5, /* the error comes from a depend modif, i.e. depended function failed */
};

/* Event loop running errors */
enum run_e : int32_t {
    RUN_OK = 0,       /* when the pool stopped because it ran out of things to do */
    RUN_ERRORED = -1, /* comes from epoll errors */
    RUN_ABORTED = -2, /* if a corutine had a stroke (some sort of internal error) */
    RUN_STOPPED = -3, /* can be re-run (comes from force_stop) */
};

enum modif_e : int32_t {
    /* This is called when a task is called (on the task), via 'co_await task' */
    CO_MODIF_CALL_CBK = 0,

    /* This is called on the corutine that is scheduled. Other mods are inherited before this is
    called. The return value of the callback is ignored. */
    CO_MODIF_SCHED_CBK,

    /* This is called on a corutine right before it is destroyed. The return value of the callback
    is ignored */
    CO_MODIF_EXIT_CBK,

    /* This is called on each suspended corutine. The return value of the callback is ignored*/
    CO_MODIF_LEAVE_CBK,

    /* This is called on a resume. The return value of the callback is ignored */
    CO_MODIF_ENTER_CBK,

    /* This is called when a corutine is waiting for an IO (after the leave cbk). If the return
    value is not ERROR_OK, then the wait is aborted. */
    CO_MODIF_WAIT_IO_CBK,

    /* This is called when the io is done and the corutine that awaited it is resumed */
    CO_MODIF_UNWAIT_IO_CBK,

    /* This is similar to wait_io, but on a semaphore */
    CO_MODIF_WAIT_SEM_CBK,

    /* This is similar to unwait_io, but on a semaphore */
    CO_MODIF_UNWAIT_SEM_CBK,

    CO_MODIF_COUNT,
};

enum modif_flags_e : int32_t {
    CO_MODIF_INHERIT_NONE = 0x0,
    CO_MODIF_INHERIT_ON_CALL = 0x1,
    CO_MODIF_INHERIT_ON_SCHED = 0x2,
};


/* all the internal tasks return this, namely error_e but casted to int (a lot of my old code depends
on this and I also find it in theme, as all the linux functions that I use tend to return ints) */
using task_t = task<int>;

/* some forward declarations of internal structures */
struct yield_awaiter_t;
struct sem_awaiter_t;
struct pool_internal_t;
struct sem_internal_t;
struct allocator_memory_t;
struct io_desc_t;

template <typename T>
struct sched_awaiter_t;

/*
There are two things that don't need custom allocating: lowspeed stuff and the corutine promise.
It makes no sense to allocate the corutine promise because:
    1. It is a user defined type so the promise is not going to fit well in our allocator
    2. I expect it be allocated rarelly
    3. It would be a pain to allocate them
    4. a malloc now and then is not such a big deal

For the rest of the code those 4 are not generaly true.

The idea of this allocator is that allocated objects are actually small in size and get allocated/
deallocated fast. The assumption is that there is a direct corellation with the number
num_fds+call_depth, and most of the time there aren't that many file descriptors or depth to
calls (at least from my experience).

So what it does is this: it has 5 bucket levels: 32, 64, 128, 512, 2048 (bytes), with a number of
maximum allocations 16384, 8192, 4096, 1024 and 256 of each and a stack coresponding to each of
them, that is initiated at the start to contain every index from 0 to the max amount of slots in
each bucket. When an allocation occours, an index is poped from the lowest fitting bucket, and
that slot is returned. On a free, if the memory is from a bucket, the index is calculated and pushed
back in that bucket's stack else the normal free is used.

Those bucket levels can be configured, by changing the array bellow (Obs: The buckets must be in
ascending size order).

The improvement from malloc, as a guess, (I used a profiler to see if it does something) is that
    a. the memory is already there, no need to fetch it if not available and no need to check if it
    is available
    b. there are no locks. since we already assume that the code that does the allocations is
    protected either by the absence of multithreading or by the pool's lock
    c. there is no fragmentation, at least until the memory runs out, the pool's memory is localized
*/
/* No shared pointers if not needed: around 80% time spent decrease */
/* Own allocator: around 10%-30% further time decrease (not sure if it was worth the time, but at
least my test go smoother now) */
/* array of {element size, bucket size} */
constexpr std::pair<int, int> allocator_bucket_sizes[] = {
    {32,    COLIB_ALLOCATOR_SCALE * 1024},
    {64,    COLIB_ALLOCATOR_SCALE * 512},
    {128,   COLIB_ALLOCATOR_SCALE * 256},
    {512,   COLIB_ALLOCATOR_SCALE * 64},
    {2048,  COLIB_ALLOCATOR_SCALE * 16}
};
template <typename T>
struct allocator_t {
    constexpr static int common_alignment = 16;

    static_assert(common_alignment % __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 0,
            "ERROR: The allocator assumption is that internal data is aligned to at most 16 bytes");

    using value_type = T;
    allocator_t(pool_t *pool) : pool(pool) {}

    template<typename U>
    allocator_t(const allocator_t <U>& o) noexcept : pool(o.pool) {}

    template<typename U>
    allocator_t &operator = (const allocator_t <U>& o) noexcept { this->pool = o.pool; return *this; }

    T* allocate(size_t n);
    void deallocate(T* _p, std::size_t n) noexcept;

    template <typename U>
    bool operator != (const allocator_t<U>& o) noexcept { return this->pool != o.pool; }

    template <typename U>
    bool operator == (const allocator_t<U>& o) noexcept { return this->pool == o.pool; }

protected:
    template <typename U>
    friend struct allocator_t; /* lonely class */

    pool_t *pool = nullptr; /* this allocator should allways be called on a valid pool */
};
template <typename T> 
struct deallocator_t { /* This class is part of the allocator implementation and is here only
                          because a definition needs it as a full type */
    pool_t *pool = nullptr;
    void operator ()(T *p) { p->~T(); allocator_t<T>{pool}.deallocate(p, 1); }
};

/* having a task be called on two pools or two threads is UB */
struct pool_t {
    /* you use the pointer made by create_pool */
    pool_t(pool_t& sem) = delete;
    pool_t(pool_t&& sem) = delete;
    pool_t &operator = (pool_t& sem) = delete;
    pool_t &operator = (pool_t&& sem) = delete;

    ~pool_t() { clear(); }

    /* Same as colib::sched, but used outside of a corutine, based on the pool */
    template <typename T>
    void sched(task<T> task, const modif_pack_t& v = {}); /* ignores return type */

#if COLIB_ENABLE_MULTITHREAD_SCHED
    template <typename T>
    void thread_sched(task<T> task);
#endif /* COLIB_ENABLE_MULTITHREAD_SCHED */

    /* runs the event loop */
    run_e run();

    /* clears all the corutines that are linked to this pool, so all waiters, all ready tasks, etc.
    This will happen automatically inside the destructor */
    error_e clear();

    /* stops awaiters from waiting on the descriptor (this is the non-awaiting variant) */
    error_e stop_io(const io_desc_t& io_desc);

    /* Better to not touch this function, you need to understand the internals of pool_t to use it */
    pool_internal_t *get_internal();

    int64_t stopval = 0;            /* a value that is set by force_stop(stopval) */
    std::shared_ptr<void> user_ptr; /* the library won't touch this, do whatever with it */

protected:
    template <typename T>
    friend struct allocator_t;

    std::unique_ptr<allocator_memory_t> allocator_memory;

    friend inline std::shared_ptr<pool_t> create_pool();
    pool_t();

private:
    std::unique_ptr<pool_internal_t> internal;
};

struct sem_t {
    struct unlocker_t{ /* compatibility with guard objects ex: std::lock_guard guard(co_await s); */
        sem_t *sem;
        unlocker_t(sem_t *sem) : sem(sem) {}
        void lock() {}
        void unlock() { sem->signal(); }
    };

    /* you use the pointer made by create_sem */
    sem_t(sem_t& sem) = delete;
    sem_t(sem_t&& sem) = delete;
    sem_t &operator = (sem_t& sem) = delete;
    sem_t &operator = (sem_t&& sem) = delete;

    /* If the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire
    call stack) */
    ~sem_t();

    sem_awaiter_t wait(); /* if awaited returns unlocker_t{} */
    error_e signal(int64_t inc = 1); /* returns error if the pool disapeared */
    bool try_dec();

    /* again, beeter don't touch, same as pool */
    sem_internal_t *get_internal();

protected:
    template <typename T, typename ...Args>
    friend inline T *alloc(pool_t *, Args&&...);

    sem_t(pool_t *pool, int64_t val = 0);

private:
    std::unique_ptr<sem_internal_t, deallocator_t<sem_internal_t>> internal;
};


#if COLIB_OS_LINUX

/* This describes the async io op. */
struct io_desc_t {
    int fd = -1;                        /* file descriptor */
    uint32_t events = 0xffff'ffff;      /* epoll events to be waited on the file descriptor */

    bool is_valid() { return fd > 0; }
};

#endif /* COLIB_OS_LINUX */
#if COLIB_OS_WINDOWS

struct io_data_t {
    enum io_flag_e : int32_t {
        IO_FLAG_NONE = 0,
        IO_FLAG_TIMER = 1,
        IO_FLAG_ADDED = 2,
        IO_FLAG_TIMER_RUN = 4,
    };

    OVERLAPPED overlapped = {0};                    /* must be the first member of this struct */
    io_flag_e flags = io_flag_e{0};
    state_t *state = nullptr;                       /* state of the task */
    DWORD recvlen = 0;

    std::function<error_e(void *)> io_request;      /* function to be called inside add_waiter */
    void *ptr = nullptr;                            /* can be context for io_request or timer */
    HANDLE h = NULL;                                /* same as bellow */
};

struct io_desc_t {
    std::shared_ptr<io_data_t> data = nullptr;
    HANDLE h = NULL;

    bool is_valid() { return h != NULL; }
};

#endif /* COLIB_OS_WINDOWS */
#if COLIB_OS_UNKNOWN

/* This describes the async io op. */
COLIB_OS_UNKNOWN_IO_DESC

#endif /* COLIB_OS_UNKNOWN */

/* This is the state struct that each corutine has */
struct state_t {
    error_e err = ERROR_OK;                 /* holds the error return in diverse cases */
    pool_t *pool = nullptr;                 /* the pool of this coro */
    modif_table_p modif_table;              /* we allocate a table only if there are mods */

    state_t *caller_state = nullptr;        /* this holds the caller's state, and with it the
                                            return path */ 

    std::coroutine_handle<void> self;       /* the coro's self handle */

    std::exception_ptr exception = nullptr; /* the exception that must be propagated */

    std::shared_ptr<void> user_ptr;         /* this is a pointer that the user can use for whatever
                                            he feels like. This library will not touch this pointer */
};

/* This is mostly internal */
using sem_waiter_handle_p =
        std::shared_ptr<                        /* if this pointer is not available, the waiter was
                                                   evicted from the waiters list */
            std::list<                          /* List with the semaphore waiters */
                std::pair<
                    state_t *,                  /* The waiting corutine state */
                    std::shared_ptr<void>       /* Where the shared_ptr is actually stored */
                >,
                allocator_t<std::
                    pair<
                        state_t *,
                        std::shared_ptr<void>
                    >
                >                               /* profiling shows the default is slow */
            >::iterator                         /* iterator in the respective list */
        >;

/* A modif is a callback for a specifi stage in the corutine's flow */
struct modif_t {
    using variant_t = std::variant<
        std::function<error_e(state_t *)>,              /* call_cbk */
        std::function<error_e(state_t *)>,              /* sched_cbk */
        std::function<error_e(state_t *)>,              /* exit_cbk */
        std::function<error_e(state_t *)>,              /* leave_cbk */
        std::function<error_e(state_t *)>,              /* enter_cbk */
        std::function<error_e(state_t *, io_desc_t&)>,  /* wait_io_cbk */
        std::function<error_e(state_t *, io_desc_t&)>,  /* unwait_io_cbk */

         /* wait_sem_cbk - OBS: the std::shared_ptr<void> part can be ignored, it's internal */
        std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,

         /* unwait_sem_cbk */
        std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>
    >;

    variant_t cbk;
    modif_e type = CO_MODIF_COUNT;
    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
};

/* Pool & Sched functions:
------------------------------------------------------------------------------------------------  */

inline std::shared_ptr<pool_t> create_pool();

inline task<pool_t *> get_pool();
inline task<state_t *> get_state();

/* This does not stop the curent corutine, it only schedules the task, but does not yet run it. */
template <typename T>
inline sched_awaiter_t<T> sched(task<T> to_sched, const modif_pack_t& v = {});

/* This stops the current coroutine from running and places it at the end of the ready queue. */
inline yield_awaiter_t yield();

/* Modifications
------------------------------------------------------------------------------------------------  */

template <modif_e type, typename Cbk>
inline modif_p create_modif(pool_t *pool, modif_flags_e flags, Cbk&& cbk);

template <modif_e type, typename Cbk>
inline modif_p create_modif(pool_p  pool, modif_flags_e flags, Cbk&& cbk);

/* This returns the internal vec that holds the different modifications of the task. Changing them
here is ill-defined */
template <typename T>
inline std::vector<modif_p> task_modifs(task<T> t);

/* This adds a modifier to the task and returns the task. Duplicates will be ignored */
template <typename T>
inline task<T> add_modifs(pool_t *pool, task<T> t, const std::set<modif_p>& mods); /* not awaitable */

/* Removes modifier from the vector */
template <typename T>
inline task<T> rm_modifs(task<T> t, const std::set<modif_p>& mods); /* not awaitable */

/* same as above, but adds them for the current task */
inline task<std::vector<modif_p>> task_modifs();
inline task_t add_modifs(const std::set<modif_p>& mods); 
inline task_t rm_modifs(const std::set<modif_p>& mods);

/* calls an awaitable inside a task_t, this is done to be able to use modifs on awaitables */
template <typename Awaiter>
inline task_t await(Awaiter&& awaiter);

/* Timing
------------------------------------------------------------------------------------------------  */

/* adds a timeout modification to the task, i.e. the corutine will be stopped from executing if the
given time passes (uses create_killer) */
template <typename T>
inline task<std::pair<T, error_e>> create_timeo(
        task<T> t, pool_t *pool, const std::chrono::microseconds& timeo);

/* sleep functions */
inline task_t sleep_us(uint64_t timeo_us);
inline task_t sleep_ms(uint64_t timeo_ms);
inline task_t sleep_s(uint64_t timeo_s);
inline task_t sleep(const std::chrono::microseconds& us);

/* Flow Controll:
------------------------------------------------------------------------------------------------  */

/* Creates semaphores, those need the pool to exist */
inline sem_p create_sem(pool_t *pool, int64_t val);
inline sem_p create_sem(pool_p  pool, int64_t val);
inline task<sem_p> create_sem(int64_t val);

inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e);

template <typename T>
inline task<T> create_future(pool_t *pool, task<T> t); /* not awaitable */

template <typename ...ret_v>
inline task<std::tuple<ret_v>...> wait_all(task<ret_v>... tasks);

inline task_t force_stop(int64_t stopval = 0);

/* EPOLL:
------------------------------------------------------------------------------------------------  */

inline task_t wait_event(const io_desc_t& io_desc);

/* closing a file descriptor while it is managed by the coroutine pool will break the entire system
so you must use stop_fd on the file descriptor before you close it. This makes sure the fd is
awakened and ejected from the system before closing it. For example:

    co_await colib::stop_fd(fd);
    close(fd);
*/
inline task_t stop_io(const io_desc_t& io_desc);

#if COLIB_OS_LINUX

/* Linux Specific:
------------------------------------------------------------------------------------------------  */

inline task_t stop_fd(int fd);

inline task_t        connect(int fd, sockaddr *sa, socklen_t *len);
inline task_t        accept(int fd, sockaddr *sa, socklen_t *len);
inline task<ssize_t> read(int fd, void *buff, size_t len);
inline task<ssize_t> write(int fd, const void *buff, size_t len);
inline task_t        read_sz(int fd, void *buff, size_t len);
inline task_t        write_sz(int fd, const void *buff, size_t len);

#endif /* COLIB_OS_LINUX */

/* Windows Specific:
------------------------------------------------------------------------------------------------  */

#if COLIB_OS_WINDOWS

/* similar to stop_io, but stops all the waiters on a handle */
inline task_t stop_handle(HANDLE h);

/* Those functions are the same as their Windows API equivalent, the difference is that they don't
expose the overlapped structure, which is used by the coro library. They require a handle that
is compatible with iocp and they will attach the handle to the iocp instance. Those are the
functions listed by msdn to work with iocp (and connect, that is part of an extension) */
inline task<BOOL> ConnectEx(SOCKET  s,
                            const   sockaddr *name,
                            int     namelen,
                            PVOID   lpSendBuffer,
                            DWORD   dwSendDataLength,
                            LPDWORD lpdwBytesSent);

inline task<BOOL> AcceptEx(SOCKET   sListenSocket,
                           SOCKET   sAcceptSocket,
                           PVOID    lpOutputBuffer,
                           DWORD    dwReceiveDataLength,
                           DWORD    dwLocalAddressLength,
                           DWORD    dwRemoteAddressLength,
                           LPDWORD  lpdwBytesReceived);

inline task<BOOL> ConnectNamedPipe(HANDLE hNamedPipe);

inline task<BOOL> DeviceIoControl(HANDLE    hDevice,
                                  DWORD     dwIoControlCode,
                                  LPVOID    lpInBuffer,
                                  DWORD     nInBufferSize,
                                  LPVOID    lpOutBuffer,
                                  DWORD     nOutBufferSize,
                                  LPDWORD   lpBytesReturned);

inline task<BOOL> LockFileEx(HANDLE     hFile,
                             DWORD      dwFlags,
                             DWORD      dwReserved,
                             DWORD      nNumberOfBytesToLockLow,
                             DWORD      nNumberOfBytesToLockHigh,
                             uint64_t   *offset);

inline task<BOOL> ReadDirectoryChangesW(HANDLE                              hDirectory,
                                        LPVOID                              lpBuffer,
                                        DWORD                               nBufferLength,
                                        BOOL                                bWatchSubtree,
                                        DWORD                               dwNotifyFilter,
                                        LPDWORD                             lpBytesReturned,
                                        LPOVERLAPPED_COMPLETION_ROUTINE     lpCompletionRoutine);

inline task<BOOL> ReadFile(HANDLE   hFile,
                           LPVOID   lpBuffer,
                           DWORD    nNumberOfBytesToRead,
                           LPDWORD  lpNumberOfBytesRead);

inline task<BOOL> TransactNamedPipe(HANDLE  hNamedPipe,
                                    LPVOID  lpInBuffer,
                                    DWORD   nInBufferSize,
                                    LPVOID  lpOutBuffer,
                                    DWORD   nOutBufferSize,
                                    LPDWORD lpBytesRead);

inline task<BOOL> WaitCommEvent(HANDLE  hFile,
                                LPDWORD lpEvtMask);

inline task<BOOL> WriteFile(HANDLE  hFile,
                            LPCVOID lpBuffer,
                            DWORD   nNumberOfBytesToWrite,
                            LPDWORD lpNumberOfBytesWritten,
                            uint64_t *offset);

inline task<BOOL> WSASendMsg(SOCKET                             Handle,
                            LPWSAMSG                            lpMsg,
                            DWORD                               dwFlags,
                            LPDWORD                             lpNumberOfBytesSent,
                            LPWSAOVERLAPPED_COMPLETION_ROUTINE  lpCompletionRoutine);

inline task<BOOL> WSASendTo(SOCKET                             s,
                            LPWSABUF                           lpBuffers,
                            DWORD                              dwBufferCount,
                            LPDWORD                            lpNumberOfBytesSent,
                            DWORD                              dwFlags,
                            const sockaddr                     *lpTo,
                            int                                iTolen,
                            LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

inline task<BOOL> WSASend(SOCKET s,
                          LPWSABUF                           lpBuffers,
                          DWORD                              dwBufferCount,
                          LPDWORD                            lpNumberOfBytesSent,
                          DWORD                              dwFlags,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

inline task<BOOL> WSARecvFrom(SOCKET                             s,
                              LPWSABUF                           lpBuffers,
                              DWORD                              dwBufferCount,
                              LPDWORD                            lpNumberOfBytesRecvd,
                              LPDWORD                            lpFlags,
                              sockaddr                           *lpFrom,
                              LPINT                              lpFromlen,
                              LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

inline task<BOOL> WSARecvMsg(SOCKET                             s,
                             LPWSAMSG                           lpMsg,
                             LPDWORD                            lpdwNumberOfBytesRecvd,
                             LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

inline task<BOOL> WSARecv(SOCKET                             s,
                          LPWSABUF                           lpBuffers,
                          DWORD                              dwBufferCount,
                          LPDWORD                            lpNumberOfBytesRecvd,
                          LPDWORD                            lpFlags,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

/* Addaptations for windows */
inline task_t        connect(SOCKET s, const sockaddr *sa, uint32_t len);
inline task<SOCKET>  accept(SOCKET s, sockaddr *sa, uint32_t *len);
inline task<SSIZE_T> read(HANDLE h, void *buff, size_t len);
inline task<SSIZE_T> write(HANDLE h, const void *buff, size_t len, uint64_t *offset = nullptr);
inline task_t        read_sz(HANDLE h, void *buff, size_t len);
inline task_t        write_sz(HANDLE h, const void *buff, size_t len, uint64_t *offset = nullptr);


#endif /* COLIB_OS_WINDOWS */

/* Unknown Specific:
------------------------------------------------------------------------------------------------  */

#if COLIB_OS_UNKNOWN

/* You implement your own */

#endif /* COLIB_OS_UNKNOWN */

/* Debug Interfaces:
------------------------------------------------------------------------------------------------  */

/* why would I replace the old string with the new one based on the allocator? because I need to
know that the library allocates only through the allocator, so debugging would interfere with
that. */
using dbg_string_t = std::basic_string<char, std::char_traits<char>, allocator_t<char>>;

template <typename... Args>
inline void dbg(const char *file, const char *func, int line, const char *fmt, Args&&... args);

/* Registers a name for a given task */
template <typename T, typename ...Args>
inline task<T> dbg_register_name(task<T> t, const char *fmt, Args&&...);

template <typename P, typename ...Args>
inline std::coroutine_handle<P> dbg_register_name(std::coroutine_handle<P> h,
        const char *fmt, Args&&...);

template <typename ...Args>
inline void * dbg_register_name(void *addr, const char *fmt, Args&&...);

/* creates a modifier that traces things from corutines, mostly a convenience function, it also
uses the log_str function, or does nothing else if it isn't defined. If you don't like the
verbosity, be free to null any callback you don't care about. */
inline modif_pack_t dbg_create_tracer(pool_t *pool);

/* Obtains the name given to the respective task, handle or address */
template <typename T>
inline dbg_string_t dbg_name(task<T> t);

template <typename P>
inline dbg_string_t dbg_name(std::coroutine_handle<P> h);

inline dbg_string_t dbg_name(void *v);

/* Obtains a string from the given enum */
inline dbg_string_t dbg_enum(error_e code);
inline dbg_string_t dbg_enum(run_e code);

#if COLIB_OS_LINUX
inline dbg_string_t dbg_epoll_events(uint32_t events);
#endif /* COLIB_OS_LINUX */

/* formats a string using the C snprintf, similar in functionality to a combination of
snprintf+std::format, in the version of g++ that I'm using std::format is not available  */
template <typename... Args>
inline dbg_string_t dbg_format(const char *fmt, Args&& ...args);

/* calls log_str to save the log string */
#if COLIB_ENABLE_LOGGING
inline std::function<int(const dbg_string_t&)> log_str =
        [](const dbg_string_t& msg){ return printf("%s", msg.c_str()); };
#endif /* COLIB_ENABLE_LOGGING */

/* IMPLEMENTATION 
=================================================================================================
=================================================================================================
================================================================================================= */

/* Helpers
------------------------------------------------------------------------------------------------- */

template <typename P>
using handle = std::coroutine_handle<P>;

template <typename T, typename K>
constexpr auto has(T&& data_struct, K&& key) {
    return std::forward<T>(data_struct).find(std::forward<K>(key))
            != std::forward<T>(data_struct).end();
}

using sem_wait_list_t = std::list<std::pair<state_t *, std::shared_ptr<void>>,
        allocator_t<std::pair<state_t *,std::shared_ptr<void>>>>;
using sem_wait_list_it = sem_wait_list_t::iterator;

inline void destroy_state(state_t *curr) {
    while (curr) {
        state_t *next = curr->caller_state;
        curr->self.destroy();
        curr = next;
    }
}

struct FnScope {
    std::function<void(void)> fn;
    FnScope(std::function<void(void)> fn) : fn(fn) {}
    ~FnScope() {
        if (fn)
            fn();
    }
    void precall() {
        fn();
        fn = {};
    }
};

/* Allocator
------------------------------------------------------------------------------------------------- */

#if COLIB_ALLOCATOR_REPLACE

COLIB_ALLOCATOR_REPLACE_IMPL_1

#else /* COLIB_ALLOCATOR_REPLACE */

struct allocator_memory_t {
    constexpr static int buckets_cnt =
            sizeof(allocator_bucket_sizes)/sizeof(allocator_bucket_sizes[0]);

    template <size_t obj_sz, size_t cnt>
    struct bucket_t {
        static_assert(obj_sz);
        using bucket_object_t = char[obj_sz];

        alignas(allocator_t<int>::common_alignment) bucket_object_t objects[cnt];
        int free_stack[cnt];
        int stack_head = 0;

        static constexpr int floorlog2(int x) {
            return x == 1 ? 0 : 1 + floorlog2(x >> 1);
        }

        void init() {
            stack_head = cnt - 1;
            for (int i = 0; i < cnt; i++)
                free_stack[i] = cnt - i - 1;
        }

        void *alloc(size_t bytes, bool &invalidated) {
            if (bytes > obj_sz)
                return NULL;
            else
                invalidated = true; /* shouldn't check the other buckets if the size was good, but
                                       the allocation failed for other reasons */
            if (!stack_head)
                return NULL;

            void *ret = &objects[free_stack[stack_head]];
            stack_head--;
            return ret;
        }

        void free(void *ptr) {
            auto p = (char *)ptr;
            auto start = (char *)&objects;
            auto stop = start + sizeof(objects);

            if (p < start || p >= stop)
                return ;

            int index = ((p - start) >> floorlog2(obj_sz));
            stack_head++;
            free_stack[stack_head] = index;
        }
    };

    template <size_t ...I>
    struct buckets_type_helper {
        using type = std::tuple<bucket_t<
                allocator_bucket_sizes[I].first, allocator_bucket_sizes[I].second>...>;

        buckets_type_helper(std::index_sequence<I...>) {}
    };

    template <size_t N>
    using buckets_type = decltype(buckets_type_helper(std::make_index_sequence<N>{}))::type;

    allocator_memory_t() {
        std::apply([](auto&&... args) { ((args.init()), ...); }, buckets);
    }

    void *alloc(size_t bytes) {
        void *ret = NULL;
        bool invalidated = false;
        std::apply([&](auto&&... args) {
            ((ret = (ret || invalidated) ? ret : args.alloc(bytes, invalidated)), ...);
        }, buckets);
        return ret;
    }

    void free(void *ptr) {
        std::apply([&](auto&&... args) { ((args.free(ptr)), ...); }, buckets);
    }

    /* There are some objectives for this buckets array:
        1. The buckets must all stay togheter (localized in memory)
        2. The buckets must all be constructed automatically from allocator_bucket_sizes
    */
    buckets_type<buckets_cnt> buckets;
};

/* allocator_t<T>::allocate/deallocate are declared under pool_internal_t */

template <typename T, typename ...Args>
inline T *alloc(pool_t *pool, Args&&... args) {
    return new(allocator_t<T>{pool}.allocate(1)) T(std::forward<Args>(args)...);
}

template <typename T>
inline auto dealloc_create(pool_t *pool) {
    return deallocator_t<T>{ .pool = pool };
}

#endif /* COLIB_ALLOCATOR_REPLACE */

/* Modifs Part
------------------------------------------------------------------------------------------------- */

struct modif_table_t {
    modif_table_t(pool_t *pool)
    : table{
            /* :( */
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}}
    } {}

    std::array<std::vector<modif_p, allocator_t<modif_p>>, CO_MODIF_COUNT> table;
};

inline size_t get_modif_table_sz(modif_table_p ptable) {
    if (!ptable)
        return 0;
    size_t ret = 0;
    for (auto &t : ptable->table)
        ret += t.size();
    return ret;
}

inline modif_table_p create_modif_table(pool_t *pool, const std::vector<modif_p>& to_add) {
    modif_table_p ret = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
            dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});

    std::unordered_set<modif_p, std::hash<modif_p>, std::equal_to<modif_p>,
            allocator_t<modif_p>> existing(allocator_t<modif_p>{pool});

    for (auto &m : to_add) {
        if (m && !has(existing, m)) {
            existing.insert(m); /* we silently eliminate duplicates, as per description */
            ret->table[m->type].push_back(m);
        }
    }
    if (get_modif_table_sz(ret) == 0)
        return nullptr;
    return ret;
}

inline void inherit_modifs(state_t *state, modif_table_p parent_table, modif_flags_e inherit_place) {
    auto pool = state->pool;
    if (!parent_table)
        return ;
    modif_table_p new_table = state->modif_table;

    std::unordered_set<modif_p, std::hash<modif_p>, std::equal_to<modif_p>,
            allocator_t<modif_p>> existing(allocator_t<modif_p>{pool});

    if (new_table) {
        /* should be rare */
        for (auto &cbk_table : new_table->table)
            for (auto &modif : cbk_table)
                existing.insert(modif);
    }
    if (!new_table) {
        new_table = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
                dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});
    }
    for (auto& cbk_table : parent_table->table)
        for (auto& modif : cbk_table)
            if (modif && !has(existing, modif) && (modif->flags & inherit_place))
                new_table->table[modif->type].push_back(modif);
    if (get_modif_table_sz(new_table) != 0)
        state->modif_table = new_table;   
}

template <modif_e cbk_id, typename ...Args>
inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
    if (auto modif_table = state->modif_table) {
        for (auto &modif : modif_table->table[cbk_id]) {
            error_e ret = std::get<cbk_id>(modif->cbk)(state, args...);
            if (ret != ERROR_OK) {
                /* If any callback returned an error, we stop their execution and return the error. */
                return ret;
            }
        }
    }
    return ERROR_OK;
}

inline error_e do_sched_modifs(state_t *state, modif_table_p parent_table) {
    inherit_modifs(state, parent_table, CO_MODIF_INHERIT_ON_SCHED);
    return do_generic_modifs<CO_MODIF_SCHED_CBK>(state);
}

inline error_e do_call_modifs(state_t *state, modif_table_p parent_table) {
    inherit_modifs(state, parent_table, CO_MODIF_INHERIT_ON_CALL);
    return do_generic_modifs<CO_MODIF_CALL_CBK>(state);
}

inline error_e do_leave_modifs(state_t *state) {
    return do_generic_modifs<CO_MODIF_LEAVE_CBK>(state);
}

inline error_e do_entry_modifs(state_t *state) {
    return do_generic_modifs<CO_MODIF_ENTER_CBK>(state);
}

inline error_e do_exit_modifs(state_t *state) {
    return do_generic_modifs<CO_MODIF_EXIT_CBK>(state);
}

inline error_e do_wait_io_modifs(state_t *state, io_desc_t &io_desc) {
    return do_generic_modifs<CO_MODIF_WAIT_IO_CBK>(state, io_desc);
}

inline error_e do_unwait_io_modifs(state_t *state, io_desc_t &io_desc) {
    return do_generic_modifs<CO_MODIF_UNWAIT_IO_CBK>(state, io_desc);
}

inline error_e do_wait_sem_modifs(state_t *state, sem_t *sem, sem_waiter_handle_p _it) {
    return do_generic_modifs<CO_MODIF_WAIT_SEM_CBK>(state, sem, _it);
}

inline error_e do_unwait_sem_modifs(state_t *state, sem_t *sem, sem_waiter_handle_p _it) {
    return do_generic_modifs<CO_MODIF_UNWAIT_SEM_CBK>(state, sem, _it);
}

/* considering you may want to create a modif at runtime this seems to be the best way */
template <modif_e type_id, typename Cbk>
inline modif_p create_modif(pool_t *pool, modif_flags_e flags, Cbk&& cbk) {
    modif_p ret = modif_p(alloc<modif_t>(pool, modif_t{
        .type = type_id,
        .flags = flags,
    }), dealloc_create<modif_t>(pool), allocator_t<int>{pool});
    
    ret->cbk = modif_t::variant_t(std::in_place_index_t<type_id>{}, std::forward<Cbk>(cbk));
    return ret;
}

template <modif_e type_id, typename Cbk>
inline modif_p create_modif(pool_p pool, modif_flags_e flags, Cbk&& cbk) {
    return create_modif<type_id>(pool.get(), flags, std::forward<Cbk>(cbk));
}
/* The Task
------------------------------------------------------------------------------------------------- */

template <typename T>
struct task_state_t;

/* So, a corutine handle points to a corutine, so a task. A task should be able to be awaited,
hence this task object can A. hold the corutine and B. be awaited to get the corutine return value*/
template<typename T>
struct task {
    using promise_type = task_state_t<T>;
    using handle_t = handle<task_state_t<T>>;

    task() : h(std::noop_coroutine()) {}
    task(handle_t h) : h(h) {}
    task(handle<void> h) : h(handle_t::from_address(h.address())) {}

    /* this is here because we would like to know if a corutine died without ever beeing called */
    task(task &oth) : h(oth.h) { oth.ever_called = true; }
    task(task &&oth) : h(std::move(oth.h)) { oth.ever_called = true; }
    task& operator = (task &oth) { oth.ever_called = true; h = oth.h; return *this; }
    task& operator = (task &&oth) { oth.ever_called = true; h = std::move(oth.h); return *this; }

    bool await_ready() noexcept { return false; }

    /* Those need to be implemented bellow the task_state */
    template <typename P>
    handle<void> await_suspend(handle<P> caller) noexcept;
    T            await_resume();
    pool_t       *get_pool();
    state_t      *get_state();

    error_e get_err() { return ERROR_OK; };

    bool ever_called = false;
    handle_t h;
};

/* has to know about pool, will be implemented bellow the pool, but it is required here and to be
accesible to others that need to clean the corutine (for example in modifs, if the tasks need
killing) */
inline handle<void> final_awaiter_cleanup(state_t *ending_task_state);
inline handle<void> cpp_yield_awaiter(state_t *yielding_task_state);

template <typename T>
struct task_state_t {
    struct final_awaiter_t {
        bool await_ready() noexcept  { return false; }
        void await_resume() noexcept {}
        
        handle<void> await_suspend(handle<task_state_t<T>> ending_task) noexcept {
            return final_awaiter_cleanup(&ending_task.promise().state);
        }
    };

    struct cpp_yield_awaiter_t {
        bool await_ready() noexcept  { return false; }
        void await_resume() noexcept {}
        
        handle<void> await_suspend(handle<task_state_t<T>> yielding_task) noexcept {
            return cpp_yield_awaiter(&yielding_task.promise().state);
        }
    };

    task<T> get_return_object() {
        typename task<T>::handle_t h = task<T>::handle_t::from_promise(*this);
        state.self = h;
        return task<T>{h};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    final_awaiter_t     final_suspend() noexcept   { return final_awaiter_t{}; }
    void                unhandled_exception()      { state.exception = std::current_exception(); }

    /* probably slower than most generators, but if you need it it's here */
    template <typename R>
    cpp_yield_awaiter_t yield_value(R&& ret) {
        this->ret.template emplace<T>(std::forward<R>(ret));
        return {};
    }

    template <typename R>
    void return_value(R&& ret) {
        this->ret.template emplace<T>(std::forward<R>(ret));
    }

    state_t state;
    std::exception_ptr exception;
    std::variant<std::monostate, T> ret;
};

template <typename T>
template <typename P>
inline handle<void> task<T>::await_suspend(handle<P> caller) noexcept {
    do_leave_modifs(&caller.promise().state);
    ever_called = true;

    h.promise().state.caller_state = &caller.promise().state;
    h.promise().state.pool = caller.promise().state.pool;

    if (do_call_modifs(&this->h.promise().state, caller.promise().state.modif_table) != ERROR_OK) {
        do_entry_modifs(&caller.promise().state);
        return caller;
    }

    return h;
}

template <typename T>
inline T task<T>::await_resume() {
    ever_called = true;
    do_entry_modifs(h.promise().state.caller_state);

    /* propagate coroutine exception in called context */
    std::exception_ptr exc_ptr = h.promise().state.exception;
    if (exc_ptr) {
        h.destroy();
        std::rethrow_exception(exc_ptr);
    }

    auto ret = std::get<T>(h.promise().ret);
    if (h.promise().state.err != ERROR_YIELDED) {
        h.destroy();
    }

    return ret;
}

template <typename T>
inline pool_t *task<T>::get_pool() {
    return h.promise().state.pool;
}

template <typename T>
inline state_t *task<T>::get_state() {
    return &h.promise().state;
}

/* The Pool & Epoll engine
------------------------------------------------------------------------------------------------- */

#if COLIB_OS_LINUX

/* This is a component of the pool_internal that is somehow prepared to be replaced in caseyou need
it to be.To change the waiting mechanism you want to change this struct and io_desc_t and otherwise
this whole library should be ignorant to the system async mechanisms until the endpoint functions
like accept, connect, write, etc. */
struct io_pool_t {
    struct fd_data_t {
        struct waiter_t {
            uint32_t mask = 0; /* individual waiter mask */
            state_t *state;
        };
        int fd;
        uint32_t mask = 0; /* all the masks in one */
        std::vector<waiter_t, allocator_t<waiter_t>> waiters;
    };

    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
    :       pool{pool},
            fd_data_slow(allocator_t<int>{pool}),
            ret_evs(allocator_t<int>{pool}),
            ready_tasks{ready_tasks}
    {
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            COLIB_DEBUG("FAILED epoll_create1 err:%s[%d] -> ret:%d",
                    strerror(errno), errno, epoll_fd);
        }
    }

    ~io_pool_t() {
        close(epoll_fd);
    }

    bool is_ok() {
        return epoll_fd >= 0;
    }

    error_e handle_ready() {
        if (ready_tasks.size() > 0) {
            /* we don't do anything if there are tasks ready, we only start interogating the system
            about ready io events if we don't have corutines to serve */
            return ERROR_OK;
        }

        if (ret_evs.size() == 0) {
            /* we don't have what to wait on, there are no registered awaiters */
            return ERROR_OK;
        }

        /* Now that we know we have no corutines ready we know we have to wait for some sort of io
        event to happen(timers, file io, network io, etc.). */
        int num_evs;
        do {
            num_evs = epoll_wait(epoll_fd, ret_evs.data(), ret_evs.size(), -1);
        }
        while (num_evs < 0 && errno == EINTR);
        if (num_evs < 0) {
            COLIB_DEBUG("FAILED epoll_wait: epoll_fd[%d] err:%s[%d] -> ret:%d",
                    epoll_fd, strerror(errno), errno, num_evs);
            return ERROR_GENERIC;
        }

        /* New events arrived, it means that we can take those and push them into the ready_tasks */
        for (int i = 0; i < num_evs; i++) {
            int fd = ret_evs[i].data.fd;
            fd_data_t *data = get_data(fd);
            if (!data) {
                COLIB_DEBUG("FAILED: fd[%d] doesn't have associated data", fd);
                return ERROR_GENERIC;
            }

            uint32_t events = ret_evs[i].events;
            if (~data->mask & events) {
                COLIB_DEBUG("WARNING: unexpected events[%s] on fd[%d] with mask[%s]",
                        dbg_epoll_events(events).c_str(), fd, dbg_epoll_events(data->mask).c_str());
            }

            uint32_t remove_mask = 0;
            for (auto &w : data->waiters) {
                if (w.mask & events) {
                    w.state->err = ERROR_OK;
                    ready_tasks.push_back(w.state);
                    remove_mask |= w.mask;
                }
            }
            error_e ret;
            if ((ret = remove_waiter(io_desc_t{ .fd = fd, .events = remove_mask })) != ERROR_OK) {
                COLIB_DEBUG("FAILED to remove awaiter fd[%d] events%s",
                        fd, dbg_epoll_events(remove_mask).c_str());
                return ret;
            }
        }

        return ERROR_OK;
    }

    error_e add_waiter(state_t *state, const io_desc_t& io_desc) {
        if (!io_desc.events) {
            COLIB_DEBUG("FAILED Can't await zero events fd[%d]", io_desc.fd);
            return ERROR_GENERIC;
        }
        fd_data_t *data = nullptr;
        if (data = get_data(io_desc.fd)) {
            if (data->mask & io_desc.events) {
                COLIB_DEBUG("FAILED Can't wait on same events twice: fd[%d] existing%s attempt%s",
                        io_desc.fd,
                        dbg_epoll_events(data->mask).c_str(),
                        dbg_epoll_events(io_desc.events).c_str());
                return ERROR_GENERIC;
            }
            struct epoll_event ev = {};
            ev.events = io_desc.events | data->mask;
            ev.data.fd = io_desc.fd;
            int ret = 0;
            if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, io_desc.fd, &ev)) < 0) {
                COLIB_DEBUG("FAILED epoll_ctl EPOLL_CTL_MOD fd[%d] events%s err:%s[%d] -> ret:%d",
                        io_desc.fd, dbg_epoll_events(ev.events).c_str(), strerror(errno), ret);
                return ERROR_GENERIC;
            }
            data->mask = ev.events;
            data->waiters.push_back(fd_data_t::waiter_t{
                .mask = io_desc.events,
                .state = state,
            });
            state->err = ERROR_GENERIC;
            return ERROR_OK;
        }
        else {
            struct epoll_event ev = {};
            ev.events = io_desc.events;
            ev.data.fd = io_desc.fd;
            int ret;
            if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, io_desc.fd, &ev)) < 0) {
                COLIB_DEBUG("FAILED epoll_ctl EPOLL_CTL_ADD fd[%d] events%s err:%s[%d] -> ret:%d",
                        io_desc.fd, dbg_epoll_events(ev.events).c_str(), strerror(errno), ret);
                return ERROR_GENERIC;
            }
            data = alloc<fd_data_t>(pool, fd_data_t{
                .fd = io_desc.fd,
                .mask = ev.events,
                .waiters = std::vector<fd_data_t::waiter_t, allocator_t<fd_data_t::waiter_t>>{pool},
            });
            set_data(io_desc.fd, data);
            data->waiters.push_back(fd_data_t::waiter_t{
                .mask = io_desc.events,
                .state = state,
            });
            ret_evs.push_back(epoll_event{});
            state->err = ERROR_GENERIC;
            return ERROR_OK;
        }
    }

    error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
        auto data = get_data(io_desc.fd);
        if (!data || !(data->mask & io_desc.events)) {
            return ERROR_OK;
        }

        uint32_t remove_mask = 0;
        for (auto &w : data->waiters) {
            if ((w.mask & io_desc.events)) {
                remove_mask |= w.mask;
                w.state->err = retcode;
                ready_tasks.push_back(w.state);
            }
        }
        error_e ret;
        if ((ret = remove_waiter(io_desc_t{ .fd = io_desc.fd, .events = remove_mask })) != ERROR_OK) {
            COLIB_DEBUG("FAILED remove_waiter in force_awake fd[%d] events%s",
                    io_desc.fd, dbg_epoll_events(remove_mask).c_str());
            return ret;
        }
        return ERROR_OK;
    }

    error_e clear() {
        for (int i = 0; i < MAX_FAST_FD_CACHE; i++) {
            if (auto *data = fd_data_fast[i]) {
                for (auto &w : data->waiters) {
                    destroy_state(w.state);
                }
                if (remove_waiter(io_desc_t{ .fd = i, .events = 0xffff'ffff }) != ERROR_OK) {
                    COLIB_DEBUG("FAILED to remove awaiter: fd:%d", i);
                    return ERROR_GENERIC;
                }
            }
            fd_data_fast[i] = nullptr;
        }
        for (auto &[fd, data] : fd_data_slow) {
            if (data) {
                for (auto &w : data->waiters) {
                    destroy_state(w.state);
                }
                if (remove_waiter(io_desc_t{ .fd = fd, .events = 0xffff'ffff }) != ERROR_OK) {
                    COLIB_DEBUG("FAILED to remove awaiter: fd:%d", fd);
                    return ERROR_GENERIC;
                }
            }
        }
        fd_data_slow.clear();
        return ERROR_OK;
    }

private:
    error_e remove_waiter(const io_desc_t& io_desc) {
        auto data = get_data(io_desc.fd);
        if (!data) {
            COLIB_DEBUG("FAILED attempted to remove an inexisting waiter fd[%d]", io_desc.fd);
            return ERROR_GENERIC;
        }
        if ((data->mask & ~io_desc.events) != 0) {
            struct epoll_event ev;
            ev.events = data->mask & ~io_desc.events;
            ev.data.fd = io_desc.fd;
            data->mask = ev.events;
            int ret;
            if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, io_desc.fd, &ev)) < 0) {
                COLIB_DEBUG("FAILED epoll_ctl EPOLL_CTL_MOD fd[%d] events%s err:%s[%d] -> ret:%d",
                        io_desc.fd, dbg_epoll_events(ev.events).c_str(), strerror(errno), ret);
                return ERROR_GENERIC;
            }
            data->waiters.erase(
                std::remove_if(
                    data->waiters.begin(),
                    data->waiters.end(),
                    [events = io_desc.events](const fd_data_t::waiter_t& m) {
                        return events & m.mask;
                    }
                ),
                data->waiters.end()
            );
        }
        else {
            set_data(io_desc.fd, nullptr);
            int ret;
            if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, io_desc.fd, NULL)) < 0) {
                COLIB_DEBUG("FAILED epoll_ctl EPOLL_CTL_DEL fd[%d] err:%s[%d] -> ret:%d",
                        io_desc.fd, strerror(errno), ret);
                return ERROR_GENERIC;
            }
            ret_evs.pop_back();
        }
        return ERROR_OK;
    }

    fd_data_t *get_data(int fd) {
        if (fd < 0)
            return nullptr;
        if (fd >= MAX_FAST_FD_CACHE)
            return has(fd_data_slow, fd) ? fd_data_slow[fd] : nullptr;
        else
            return fd_data_fast[fd];
    }

    void set_data(int fd, fd_data_t *data) {
        /* for some reason, that I forgot, but there was a reason, I think, i preffered explicit
        allocation/deallocation, if you find it, the reason, consider sharing it with me. */
        if (auto p = get_data(fd))
            deallocator_t<fd_data_t>{pool}(p);
        if (fd < 0)
            return ;
        if (fd >= MAX_FAST_FD_CACHE) {
            if (!data)
                fd_data_slow.erase(fd);
            else
                fd_data_slow[fd] = data;
        }
        else {
            fd_data_fast[fd] = data;
        }
    }
    /* usually fds are at most 1024, so an array of ints is 4kb and will be way faster to access
    than a map */
    pool_t *pool = nullptr;
    fd_data_t *fd_data_fast[MAX_FAST_FD_CACHE] = {nullptr,};
    std::map<int, fd_data_t *, std::less<int>, allocator_t<std::pair<const int, fd_data_t *>>>
            fd_data_slow;

    std::vector<struct epoll_event, allocator_t<struct epoll_event>> ret_evs;

    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
    int epoll_fd = -1;
};

/* same as above, this is a part of pool_internal_t and it stays here for potential later rewriting.
The idea is that timers need to be created by some sort of system provided mechanism and waiting
on it should be compatibile with the io_pool_t and as such, the timer should return a io_desc_t */
struct timer_pool_t {
    timer_pool_t(pool_t *, io_pool_t &) {}

    error_e get_timer(io_desc_t& new_timer) {
        if (stack_head > 0) {
            new_timer.fd = timer_stack[stack_head];
            new_timer.events = EPOLLIN;
            stack_head--;
            return ERROR_OK;
        }

        int timer_fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC);
        if (timer_fd < 0) {
            COLIB_DEBUG("FAILED to allocate new timer err:%s[%d] -> ret: %d",
                    strerror(errno), errno, timer_fd);
            return ERROR_GENERIC;
        }

        new_timer.fd = timer_fd;
        new_timer.events = EPOLLIN;

        return ERROR_OK;
    }

    /* This function arms the timer and it will be triggered once the timer expires */
    /* not necesary to be a member function on Linux, but I feel it is better to put it here for
    future use */
    error_e set_timer(const io_desc_t& timer, const std::chrono::microseconds& time_us) {
        int64_t t = time_us.count();
        itimerspec its = {};
        its.it_value.tv_nsec = (t % 1000'000) * 1000ULL;
        its.it_value.tv_sec = t / 1000'000ULL;
        int ret = 0;
        if ((ret = timerfd_settime(timer.fd, 0, &its, NULL)) < 0) {
            COLIB_DEBUG("FAILED to set expiration date fd[%d] err:%s[%d] -> ret: %d",
                    timer.fd, strerror(errno), errno, ret);
            return ERROR_GENERIC;
        }
        return ERROR_OK;
    }

    error_e free_timer(io_desc_t& timer) {
        if (stack_head + 1 < MAX_TIMER_POOL_SIZE) {
            stack_head++;
            timer_stack[stack_head] = timer.fd;
        }
        else {
            close(timer.fd);
        }
        return ERROR_OK;
    }

    ~timer_pool_t() {
        while (stack_head >= 0) {
            close(timer_stack[stack_head]);
            stack_head--;
        }
    }
private:
    int timer_stack[MAX_TIMER_POOL_SIZE];
    int stack_head = -1;
};

#endif /* COLIB_OS_LINUX */
#if COLIB_OS_WINDOWS

inline std::string get_last_error() {
    char num_buff[64] = {0};
    LPVOID lpMsgBuf;
    DWORD dw = GetLastError();
    sprintf(num_buff, "0x%x", dw);

    if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR) &lpMsgBuf, 0, NULL) == 0)
    {
        return std::string("UNKNOWN_WINDOWS_ERROR [") + num_buff + std::string("]");
    }
    else {
        auto ret = (LPCSTR)lpMsgBuf + std::string("[") + num_buff + std::string("]");
        LocalFree(lpMsgBuf);
        return ret;
    }
}

struct io_pool_t;

struct io_pool_t {
    using ptr_type = std::shared_ptr<io_data_t>;
    using set_val_type = std::set<ptr_type>::value_type;
    using set_type = std::set<ptr_type, std::less<set_val_type>, allocator_t<set_val_type>>;
    using map_val_type = std::map<HANDLE, set_type>::value_type;

    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
    : pool{pool}, ready_tasks{ready_tasks}, handles{allocator_t<map_val_type>{pool}}
    {
        iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 1);
        if (!iocp) {
            COLIB_DEBUG("FAILED CreateIoCompletionPort err:%s", get_last_error().c_str());
        }
    }

    ~io_pool_t() {
        clear();
        CloseHandle(iocp);
    }

    bool is_ok() {
        return iocp;
    }

    error_e handle_ready() {
        if (ready_tasks.size() > 0) {
            /* we don't do anything if there are tasks ready, we only start interogating the system
            about ready io events if we don't have corutines to serve */
            return ERROR_OK;
        }

        if (handles.size() == 0) {
            /* we don't have what to wait on, there are no registered awaiters */
            return ERROR_OK;
        }

        /* Now that we know we have no corutines ready we know we have to wait for some sort of io
        event to happen(timers, file io, network io, etc.). */
        OVERLAPPED_ENTRY entry; 
        ULONG cnt;
        while (true) {
            bool ret = GetQueuedCompletionStatusEx(iocp, &entry, 1, &cnt, INFINITE, TRUE);
            if (!ret) {
                if (GetLastError() == WAIT_IO_COMPLETION) {
                    /* Ok, this was signaled by the timer calback, or whatever apc */
                    continue;
                }
                COLIB_DEBUG("Failed GetQueuedCompletionStatus: %s", get_last_error().c_str());
                /* here and in linux impl, good place for warning exception? */
                return ERROR_GENERIC;
            }
            if (cnt == 0) {
                COLIB_DEBUG("WHY?");
                return ERROR_GENERIC;
            }
            break;
        }

        /* awake the waiter (mark it's state->error and push it onto the ready tasks) */
        io_data_t *data = (io_data_t *)entry.lpOverlapped;
        data->state->err = ERROR_OK;
        data->recvlen = entry.dwNumberOfBytesTransferred;

        awake_io(data);

        return ERROR_OK;
    }

    /* The order should be: add_waiter, do the request, suspend */
    error_e add_waiter(state_t *state, const io_desc_t& io_desc) {
        if (!io_desc.data) {
            COLIB_DEBUG("FAILED Can't await null data");
            return ERROR_GENERIC;
        }
        std::shared_ptr<io_data_t> data = io_desc.data;
        if (data->flags & io_data_t::IO_FLAG_ADDED) {
            COLIB_DEBUG("FAILED Can't add awaiter twice");
            return ERROR_GENERIC;
        }

        /* there are two things that must happen here:
            1. the overlapped structure needs to be linked to the state structure
            2. the handle of the io thinghy must be aquired by this pool */

        data->state = state;
        state->err = ERROR_GENERIC;
        if (data->flags & io_data_t::IO_FLAG_TIMER) {
            /* nothing more to do */
        }
        else {
            /* This can fail if the handle was already associated, case in which we don't care */
            CreateIoCompletionPort(data->h, iocp, 0, 0);
        }

        error_e err = data->io_request(data->ptr);
        if (err != ERROR_OK) {
            COLIB_DEBUG("Failed the io_request: %s", get_last_error().c_str());
            return err;
        }
        data->flags = io_data_t::io_flag_e(data->flags | io_data_t::IO_FLAG_ADDED);

        enqueue_data(data);

        return ERROR_OK;
    }

    /* the state (singular) that is waiting for io_desc must be awakened */
    error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
        auto awake_data = [this, retcode](std::shared_ptr<io_data_t> data) -> error_e {
            if ((data->flags & io_data_t::IO_FLAG_TIMER) &&
                    (data->flags & io_data_t::IO_FLAG_TIMER_RUN))
            {
                if (!CancelWaitableTimer(data->h)) {
                    COLIB_DEBUG("Failed to stop timer: %s", get_last_error().c_str());
                    return ERROR_GENERIC;
                }
                if (!CloseHandle(data->h)) {
                    COLIB_DEBUG("Failed CloseHandle: %s", get_last_error().c_str());
                    return ERROR_GENERIC;
                }
                data->h = NULL;
                data->flags = io_data_t::io_flag_e(data->flags & ~io_data_t::IO_FLAG_TIMER_RUN);
            }
            else if (!CancelIoEx(data->h, &data->overlapped)) {
                COLIB_DEBUG("Failed to cancel io: %s", get_last_error().c_str());
                return ERROR_GENERIC;
            }

            data->state->err = retcode;
            awake_io(data.get());

            return ERROR_OK;
        };
        if (!io_desc.data) {
            if (!has(handles, io_desc.h))
                return ERROR_OK;
            std::vector<std::shared_ptr<io_data_t>> datas;
            auto it = handles.find(io_desc.h);
            for (auto &data : it->second) {
                datas.push_back(data);
            }
            for (auto data : datas) {
                error_e err;
                if ((err = awake_data(data)) != ERROR_OK)
                    return err;
            }
        }
        else {
            return awake_data(io_desc.data);
        }

        return ERROR_OK;
    }

    error_e clear() {
        for (auto &[_, datas] : handles) {
            for (auto &data : datas) {
                if ((data->flags & io_data_t::IO_FLAG_TIMER) &&
                        (data->flags & io_data_t::IO_FLAG_TIMER_RUN))
                {
                    if (!CancelWaitableTimer(data->h)) {
                        COLIB_DEBUG("Failed to stop timer: %s", get_last_error().c_str());
                        return ERROR_GENERIC;
                    }
                    if (!CloseHandle(data->h)) {
                        COLIB_DEBUG("Failed CloseHandle: %s", get_last_error().c_str());
                        return ERROR_GENERIC;
                    }
                    data->flags = io_data_t::io_flag_e(data->flags & ~io_data_t::IO_FLAG_TIMER_RUN);
                    data->h = NULL;
                }
                else if (!CancelIoEx(data->h, &data->overlapped)) {
                    COLIB_DEBUG("Failed to cancel io: %s", get_last_error().c_str());
                    return ERROR_GENERIC;
                }
                destroy_state(data->state);
            }
        }
        handles.clear();
        return ERROR_OK;
    }

    void awake_io(io_data_t *data) {
        dequeue_data(data);
        ready_tasks.push_back(data->state);
    }

    HANDLE get_iocp() {
        return iocp;
    }

private:
    void dequeue_data(io_data_t *data) {
        /* WARNING Be aware to not ever copy this pointer around */
        std::shared_ptr<io_data_t> only_key_to_set(data, [](io_data_t*){});
        if (!has(handles, data->h))
            return ;
        if (!has(handles.find(data->h)->second, only_key_to_set))
            return ;
        handles.find(data->h)->second.erase(only_key_to_set);
        if (handles.find(data->h)->second.size() == 0)
            handles.erase(data->h);
    }

    void enqueue_data(std::shared_ptr<io_data_t> data) {
        if (has(handles, data->h))
            handles.find(data->h)->second.insert(data);
        else
            handles.insert(map_val_type{data->h, allocator_t<set_val_type>{pool}})
                    .first->second.insert(data);
    }

    pool_t *pool = nullptr;
    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
    HANDLE iocp = nullptr;

    std::map<HANDLE, set_type, std::less<HANDLE>, allocator_t<map_val_type>> handles;

    int waiter_cnt = 0;
};

struct timer_pool_t {
    timer_pool_t(pool_t *pool, io_pool_t &io_pool) : pool(pool), io_pool(io_pool) {}

    /* initialize the io_desc_t class with a timer awaitable, not yet triggering the timer */
    error_e get_timer(io_desc_t& new_timer) {
        new_timer = io_desc_t {
            .data = std::shared_ptr<io_data_t>(alloc<io_data_t>(pool),
                    dealloc_create<io_data_t>(pool), allocator_t<int>{pool}),
            .h = CreateWaitableTimer(NULL, FALSE, NULL)
        };

        if (!new_timer.h) {
            COLIB_DEBUG("Failed to create timer: %s", get_last_error().c_str());
            new_timer.h = nullptr;
            return ERROR_GENERIC;
        }
        
        *new_timer.data = io_data_t {
            .flags = io_data_t::IO_FLAG_TIMER,
            .state = nullptr,
            .io_request = [](void *) -> error_e { return ERROR_OK; },
            .ptr = nullptr,
            .h = new_timer.h
        };

        return ERROR_OK;
    }

    /* start the respective timer with the time_us duration */
    error_e set_timer(const io_desc_t& timer, const std::chrono::microseconds& time_us) {
        if (!timer.data) {
            COLIB_DEBUG("Invalid timer descriptor!");
            return ERROR_GENERIC;
        }

        timer.data->ptr = (void *)&io_pool;

        LARGE_INTEGER due_time = {0};
        uint64_t us = time_us.count();

        due_time.LowPart  = (DWORD) ((us * -10) & 0xFFFFFFFF);
        due_time.HighPart = (LONG)  ((us * -10) >> 32);

        bool res = SetWaitableTimer(timer.h, &due_time, 0, [](void *ptr, DWORD, DWORD) {
                /* This function will be called when the timer expires, awakening the task and
                my understanding is that this will happen somewhere in the same thread of this pool
                so this is ok (OBS: This can happen in some random user's SleepEx) */

                io_data_t *data = (io_data_t *)ptr;
                io_pool_t *io_pool = (io_pool_t *)data->ptr;

                if (!data || !io_pool) {
                    COLIB_DEBUG("Sanity check, this shouldn't happen");
                    return ;
                }
                if (!PostQueuedCompletionStatus(io_pool->get_iocp(), 0, 0, &data->overlapped)) {
                    COLIB_DEBUG("Failed post: %s", get_last_error().c_str());
                    return ;
                }
            },
            timer.data.get(),
            (COLIB_WIN_ENABLE_SLEEP_AWAKE) ? TRUE : FALSE
        );
        if (!res) {
            COLIB_DEBUG("Couldn't start the timer");
            return ERROR_OK;
        }
        timer.data->flags = io_data_t::io_flag_e{timer.data->flags | io_data_t::IO_FLAG_TIMER_RUN};

        return ERROR_OK;
    }

    error_e free_timer(io_desc_t& timer) {
        if (timer.data->flags & io_data_t::IO_FLAG_TIMER_RUN) {
            if (!CancelWaitableTimer(timer.data->h)) {
                COLIB_DEBUG("Failed to stop timer: %s", get_last_error().c_str());
                return ERROR_GENERIC;
            }
            if (!CloseHandle(timer.data->h)) {
                COLIB_DEBUG("Failed CloseHandle: %s", get_last_error().c_str());
                return ERROR_GENERIC;
            }
        }
        timer.data = nullptr;
        timer.h = NULL;
        return ERROR_OK;
    }

private:
    size_t timer_id = 0;
    pool_t *pool = nullptr;
    io_pool_t &io_pool;
};

#endif /* COLIB_OS_WINDOWS */

#if COLIB_OS_UNKNOWN

COLIB_OS_UNKNOWN_IMPLEMENTATION

/*
// Those two structs need implemented:

struct io_pool_t {
    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks) {}

    // returns true if the constructor was succesfull
    bool is_ok() {}

    // populates ready_tasks with, well, tasks that are ready. This is the only point that blocks,
    // i.e. if there are no ready tasks, this blocks till there are
    error_e handle_ready() {}

    // the task with state "state" will wait until the event "io_desc" is ready, this function adds
    // this waiter inside this pool
    error_e add_waiter(state_t *state, const io_desc_t& io_desc) {}

    // the state (singular) that is waiting for io_desc must be awakened
    error_e force_awake(const io_desc_t& io_desc, error_e retcode) {}

    // awakes all
    error_e clear() {}
};

struct timer_pool_t {
    // initialize the io_desc_t class with a timer awaitable, not yet triggering the timer
    error_e get_timer(io_desc_t& new_timer) {}

    // start the respective timer with the time_us duration
    error_e set_timer(const io_desc_t& timer, const std::chrono::microseconds& time_us) {}

    // free the respective timer, this should happen only when there are no tasks waiting for this
    // timer (for awaking timers the function force_awake is used)
    error_e free_timer(const io_desc_t& timer) {}
};

*/

#endif /* COLIB_OS_UNKNOWN */

struct pool_internal_t {
    pool_internal_t(pool_t *_pool)
    :   pool(_pool),
        ready_tasks{allocator_t<state_t *>{_pool}},
        io_pool{_pool, ready_tasks},
        sem_pool{allocator_t<sem_t *>{_pool}},
        timer_pool(_pool, io_pool)
    {}

    template <typename T>
    void sched(task<T> task, modif_table_p parent_table) {
        /* first we give our new task the pool */
        task.h.promise().state.pool = pool;

        /* second we call our callbacks on it because it is now scheduled */
        if (do_sched_modifs(&task.h.promise().state, parent_table) != ERROR_OK) {
            return ;
        }

        /* third, we add the task to the pool */
        ready_tasks.push_back(&task.h.promise().state);
    }

#if COLIB_ENABLE_MULTITHREAD_SCHED
    template <typename T>
    void thread_sched(task<T> task) {
        std::lock_guard guard(lock);
        thread_pushed_new_tasks = true;
        ready_thread_tasks.push_back(&task.h.promise().state);
    }
#endif /* COLIB_ENABLE_MULTITHREAD_SCHED */

    run_e run() {
        if (!io_pool.is_ok()) {
            COLIB_DEBUG("the io pool is not working, check previous logs");
            return RUN_ERRORED;
        }

        dbg_register_name(task_t{std::noop_coroutine()}, "std::noop_coroutine");

        state_t *state = next_task_state();
        if (state == nullptr)
            return RUN_OK;

        ret_val = RUN_ABORTED;
        do_entry_modifs(state);
        state->self.resume();

        if (posted_exception) {
            auto pe = posted_exception;
            posted_exception = nullptr;
            std::rethrow_exception(pe);
        }

        return ret_val;
    }

    void set_exception(std::exception_ptr exc) {
        posted_exception = exc;
    }

    void push_ready(state_t *state) {
        ready_tasks.push_back(state);
    }

    void push_ready_front(state_t *state) {
        ready_tasks.push_front(state);
    }

    bool remove_ready(state_t *state) {
        for (auto it = ready_tasks.begin(); it != ready_tasks.end(); it++) {
            if (*it == state) {
                ready_tasks.erase(it);
                return true;
            }
        }
        return false;
    }

    state_t *next_task_state() {
#if COLIB_ENABLE_MULTITHREAD_SCHED
        /* First move the tasks comming from another thread, that is if there are any */
        if (thread_pushed_new_tasks) {
            std::lock_guard guard(lock);
            for (auto &s : ready_thread_tasks)
                ready_tasks.push_back(s);
            ready_thread_tasks.clear();
            thread_pushed_new_tasks = false;
        }
#endif /* COLIB_ENABLE_MULTITHREAD_SCHED */

        if (io_pool.handle_ready() != ERROR_OK) {
            COLIB_DEBUG("Failed io pool");
            ret_val = RUN_ERRORED;
            return nullptr;
        }

        if (!ready_tasks.empty()) {
            auto ret = ready_tasks.front();
            ready_tasks.pop_front();
            return ret;
        }

        /* we have nothing else to do, we resume the pool_t::run */
        ret_val = RUN_OK;
        return nullptr;
    }

    handle<void> next_task() {
        state_t *ns = next_task_state();
        return ns ? ns->self : std::noop_coroutine();
    }

    template <typename P>
    error_e wait_io(handle<P> h, const io_desc_t& io_desc) {
        /* add the file descriptor inside epoll and schedule the corutine for waiting on it */
        return io_pool.add_waiter(&h.promise().state, io_desc);
    }

    error_e stop_io(const io_desc_t& io_desc, error_e retcode) {
        /* stop the io and set it's return code as retcode */
        return io_pool.force_awake(io_desc, retcode);
    }

    error_e get_timer(io_desc_t& new_timer) {
        /* returns a timer object, referenced by the io descriptor new_timer, gaining ownership over
        it */
        return timer_pool.get_timer(new_timer);
    }

    error_e set_timer(const io_desc_t& timer, const std::chrono::microseconds& time_us) {
        /* arms the timer referenced by the descriptor timer to be awakened after time_us
        microseconds from now */
        return timer_pool.set_timer(timer, time_us);
    }

    error_e free_timer(io_desc_t& timer) {
        /* releases the ownership of the timer back to the pool */
        return timer_pool.free_timer(timer);
    }

    void add_sem(sem_t *s) {
        sem_pool.insert(s);
    }

    void rm_sem(sem_t *s) {
        sem_pool.erase(s);
    }

    /* This function needs the semaphore definition so it is implemented bellow the semaphore */
    error_e clear();

    run_e ret_val;

private:
    pool_t *pool;
    std::deque<state_t *, allocator_t<state_t *>> ready_tasks;
    io_pool_t io_pool;
    timer_pool_t timer_pool;

    std::exception_ptr posted_exception = nullptr;

    /* bookkeeping for end of life destruction */
    std::set<sem_t *, std::less<sem_t *>, allocator_t<sem_t *>> sem_pool;

#if COLIB_ENABLE_MULTITHREAD_SCHED
    std::mutex lock;
    std::atomic<bool> thread_pushed_new_tasks = false;
    std::vector<state_t *> ready_thread_tasks;
#endif /* COLIB_ENABLE_MULTITHREAD_SCHED */
};

#if COLIB_ALLOCATOR_REPLACE

COLIB_ALLOCATOR_REPLACE_IMPL_2

#else /* COLIB_ALLOCATOR_REPLACE */

/* Allocate/deallocate need the definition of pool_internal_t */
template <typename T>
inline T* allocator_t<T>::allocate(size_t n) {
    T *ret = nullptr;
    if (!COLIB_DISABLE_ALLOCATOR && pool && !std::is_same_v<char, T>)
        ret = static_cast<T*>(pool->allocator_memory->alloc(n * sizeof(T)));
    if (!ret)
        ret = static_cast<T*>(std::malloc(n * sizeof(T)));
    return ret;
}
template <typename T>
inline void allocator_t<T>::deallocate(T* _p, std::size_t) noexcept {
    if (!pool) {
        std::free(_p);
        return ;
    }

    char *p = (char *)_p;
    char *start = (char *)pool->allocator_memory.get();
    char *stop = start + sizeof(*pool->allocator_memory);

    if (p < start || p >= stop) {
        std::free(p);
        return ;
    }

    pool->allocator_memory->free(p);
}

#endif /* COLIB_ALLOCATOR_REPLACE */

inline handle<void> cpp_yield_awaiter(state_t *yielding_task_state) {
    /* bassicaly the same as bellow, except we don't destroy the corutine */
    do_leave_modifs(yielding_task_state);

    yielding_task_state->err = ERROR_YIELDED;
    state_t *caller_state = yielding_task_state->caller_state;

    /* from the point of view of the corutine modifications we are exiting here, this keeps the
    call stack proper */
    do_exit_modifs(yielding_task_state);

    if (caller_state) {
        return caller_state->self;
    }

    return yielding_task_state->pool->get_internal()->next_task();
}

inline handle<void> final_awaiter_cleanup(state_t *ending_task_state) {
    do_leave_modifs(ending_task_state);
    /* not sure if I should do something with the return value ... */
    state_t *caller_state = ending_task_state->caller_state;
    do_exit_modifs(ending_task_state);

    if (caller_state) {
        return caller_state->self;
    }
    auto pool = ending_task_state->pool;
    /* If the task that we are final_awaiting has no caller, then it is the moment to destroy it,
    no one needs it's return value. Else it will be destroyed by the caller. */
    if (ending_task_state->exception) {
        pool->get_internal()->set_exception(ending_task_state->exception);
        ending_task_state->self.destroy();
        return std::noop_coroutine();
    }
    ending_task_state->self.destroy();
    return pool->get_internal()->next_task();
}

inline pool_t::pool_t() {
    allocator_memory = std::make_unique<allocator_memory_t>();
    internal = std::make_unique<pool_internal_t>(this);
}

template <typename T>
inline void pool_t::sched(task<T> task, const modif_pack_t& v) {
    internal->sched(task, create_modif_table(this, v));
}

#if COLIB_ENABLE_MULTITHREAD_SCHED
template <typename T>
inline void pool_t::thread_sched(task<T> task) {
    internal->sched(task);
}
#endif /* COLIB_ENABLE_MULTITHREAD_SCHED */

inline run_e pool_t::run() {
    return internal->run();
}

/*  OBS: if clear is called, it is called from outside of the pool, else this is UB
    There are 4 places where tasks can reside:
        1. the io_pool - waiting on some sort of event
        2. a semaphore queue - waiting on a semaphore to signal
        3. the ready queue - waiting to be rescheduled
        4. call stacks - waiting for a call to finish

    Tasks can be created by either:
        a. sched
        b. call

    So on clear we will destroy the corutines in the following order:
        *. whenever a corutine is terminated, also terminate the caller (state_t *caller_state)
        1. terminate all corutines waiting on the io_pool
        2. terminate all the corutines waiting on semaphores
        3. terminate all corutines waiting in the ready queue, in the ready order, terminating
 */
inline error_e pool_t::clear() {
    return get_internal()->clear();
}

inline pool_internal_t *pool_t::get_internal() {
    return internal.get();
}

inline error_e pool_t::stop_io(const io_desc_t& io_desc) {
    return get_internal()->stop_io(io_desc, ERROR_WAKEUP);
}

inline std::shared_ptr<pool_t> create_pool() {
    return std::shared_ptr<pool_t>(new pool_t{});
}

/* Awaiters
------------------------------------------------------------------------------------------------- */

struct yield_awaiter_t {
    yield_awaiter_t() {}
    yield_awaiter_t(const yield_awaiter_t &oth) = delete;
    yield_awaiter_t &operator = (const yield_awaiter_t &oth) = delete;
    yield_awaiter_t(yield_awaiter_t &&oth) = delete;
    yield_awaiter_t &operator = (yield_awaiter_t &&oth) = delete;

    bool await_ready() { return false; }

    template <typename P>
    inline handle<void> await_suspend(handle<P> h) {
        auto pool = h.promise().state.pool;
        state = &h.promise().state;
        do_leave_modifs(&h.promise().state);
        pool->get_internal()->push_ready(&h.promise().state);
        return pool->get_internal()->next_task();
    }

    void await_resume() {
        do_entry_modifs(state);
    }

    state_t *state;
};

template <typename T>
struct sched_awaiter_t {
    sched_awaiter_t(task<T> t, std::vector<modif_p> v) : t(t), v(v) {}
    sched_awaiter_t(const sched_awaiter_t &oth) = delete;
    sched_awaiter_t &operator = (const sched_awaiter_t &oth) = delete;
    sched_awaiter_t(sched_awaiter_t &&oth) = delete;
    sched_awaiter_t &operator = (sched_awaiter_t &&oth) = delete;

    bool await_ready() { return false;}
    void await_resume() {}

    template <typename P>
    bool await_suspend(handle<P> h) {
        auto pool = h.promise().state.pool;
        pool->get_internal()->sched(t, create_modif_table(pool, v));
        return false;
    }

    task<T> t;
    std::vector<modif_p> v;
};

struct get_pool_awaiter_t {
    get_pool_awaiter_t() {}
    get_pool_awaiter_t(const get_pool_awaiter_t &oth) = delete;
    get_pool_awaiter_t &operator = (const get_pool_awaiter_t &oth) = delete;
    get_pool_awaiter_t(get_pool_awaiter_t &&oth) = delete;
    get_pool_awaiter_t &operator = (get_pool_awaiter_t &&oth) = delete;

    template <typename P>
    bool await_suspend(handle<P> h) {
        pool = h.promise().state.pool;
        return false;
    }

    bool    await_ready()              { return false; };
    pool_t *await_resume()             { return pool; }

    pool_t *pool;
};


struct get_state_awaiter_t {
    get_state_awaiter_t() {}
    get_state_awaiter_t(const get_state_awaiter_t &oth) = delete;
    get_state_awaiter_t &operator = (const get_state_awaiter_t &oth) = delete;
    get_state_awaiter_t(get_state_awaiter_t &&oth) = delete;
    get_state_awaiter_t &operator = (get_state_awaiter_t &&oth) = delete;

    template <typename P>
    bool await_suspend(handle<P> h) {
        state = &h.promise().state;
        return false;
    }

    bool     await_ready()              { return false; };
    state_t *await_resume()             { return state; }

    state_t *state;
};

/* This is the object with which you wait for a file descriptor, you create it with a valid
wait_cond and fd and co_await on it. The resulting integer must be positive to not be an error */
struct io_awaiter_t {
    io_desc_t io_desc;

    io_awaiter_t(const io_desc_t& io_desc) : io_desc(io_desc) {}
    io_awaiter_t(const io_awaiter_t &oth) = delete;
    io_awaiter_t &operator = (const io_awaiter_t &oth) = delete;
    io_awaiter_t(io_awaiter_t &&oth) = delete;
    io_awaiter_t &operator = (io_awaiter_t &&oth) = delete;

    bool await_ready() { return false; };

    template <typename P>
    handle<void> await_suspend(handle<P> h) {
        auto pool = h.promise().state.pool;
        state = &h.promise().state;
        /* in case we can't schedule the fd we log the failure and return the same coro */
        do_leave_modifs(state);
        if ((ret_err = do_wait_io_modifs(state, io_desc)) != ERROR_OK) {
            do_entry_modifs(state);
            return h;
        }
        ret_err = pool->get_internal()->wait_io(h, io_desc);
        if (ret_err != ERROR_OK) {
            COLIB_DEBUG("Failed to register wait: %s on: %s",
                    dbg_enum(ret_err).c_str(), dbg_name(h).c_str());
            do_entry_modifs(state);
            return h;
        }
        return pool->get_internal()->next_task();
    }

    error_e await_resume() {
        do_unwait_io_modifs(state, io_desc);
        do_entry_modifs(state);
        if (ret_err != ERROR_OK)
            return ret_err;
        else
            return state->err;
    }

private:
    /* The state of the corutine that called us. We know it will exist at least as much as the
    suspension */
    state_t *state;
    error_e ret_err = ERROR_OK;
};

/* Semaphore
------------------------------------------------------------------------------------------------- */

struct sem_internal_t {
    using sem_aloc = allocator_t<std::pair<std::coroutine_handle<void>, std::shared_ptr<void>>>;

    sem_internal_t(pool_t *pool, int64_t val) : pool(pool), val(val),
            waiting_on_sem(sem_aloc{pool}) {}

    bool await_ready() {
        if (val > 0) {
            val--;
            return true;
        }
        return false;
    }

    bool try_dec() {
        return await_ready();
    }

    error_e signal(int64_t inc = 1) {
        auto old = val;
        if (inc == 0 && val < 0) {
            val = 0;
            inc = (int64_t)waiting_on_sem.size();
        }
        val += inc;

        while (val > 0 && waiting_on_sem.size()) {
            error_e ret = _awake_one();
            if (ret != ERROR_OK)
                return ret;
            val--;
        }
        return ERROR_OK;
    }

    error_e clear() {
        while (waiting_on_sem.size()) {
            auto to_awake = waiting_on_sem.back();
            destroy_state(to_awake.first);
            waiting_on_sem.pop_back();
        }
        return ERROR_OK;
    }

    /* be carefull with this one */
    void erase_waiter(sem_wait_list_it it) {
        waiting_on_sem.erase(it);
    }

protected:
    friend sem_awaiter_t;
    friend sem_t;
    friend inline sem_p create_sem(pool_t *pool, int64_t val);

    sem_waiter_handle_p push_waiter(state_t *state) {
        auto p = std::shared_ptr<sem_wait_list_it>(
                alloc<sem_wait_list_it>(pool), dealloc_create<sem_wait_list_it>(pool),
                allocator_t<int>{pool});
        waiting_on_sem.push_front({state, p});
        *p = waiting_on_sem.begin();
        return p;
    }


    pool_t *get_pool() {
        return pool;
    }

private:
    error_e _awake_one() {
        auto to_awake = waiting_on_sem.back();
        pool->get_internal()->push_ready(to_awake.first);
        waiting_on_sem.pop_back();
        return ERROR_OK;
    }

    sem_wait_list_t waiting_on_sem;
    int64_t val;
    pool_t *pool;
};

inline error_e pool_internal_t::clear() {
    if (io_pool.clear() != ERROR_OK) {
        COLIB_DEBUG("FAILED to clear events waiting for io");
        return ERROR_GENERIC;
    }
    for (auto &s : sem_pool) {
        if (s->get_internal()->clear() != ERROR_OK) {
            COLIB_DEBUG("FAILED to clear events waiting on one of the semaphores");
            return ERROR_GENERIC; 
        }
    }
    for (auto &state : ready_tasks) {
        destroy_state(state);
    }
    ready_tasks.clear();
    return ERROR_OK;
}

struct sem_awaiter_t {
    sem_awaiter_t(sem_t *sem) : sem(sem) {}
    sem_awaiter_t(const sem_awaiter_t &oth) = delete;
    sem_awaiter_t &operator = (const sem_awaiter_t &oth) = delete;
    sem_awaiter_t(sem_awaiter_t &&oth) = delete;
    sem_awaiter_t &operator = (sem_awaiter_t &&oth) = delete;

    template <typename P>
    handle<void> await_suspend(handle<P> to_suspend) {
        state = &to_suspend.promise().state;

        auto pool = sem->get_internal()->get_pool();
        psem_it = sem->get_internal()->push_waiter(state);

        do_leave_modifs(state);
        if (do_wait_sem_modifs(state, sem, psem_it) != ERROR_OK) {
            sem->get_internal()->erase_waiter(*psem_it);
            do_entry_modifs(state);
            return to_suspend;
        }

        triggered = true;
        return pool->get_internal()->next_task();
    }

    sem_t::unlocker_t await_resume() {
        if (triggered) {
            do_unwait_sem_modifs(state, sem, psem_it);
            do_entry_modifs(state);
        }
        triggered = false;
        return sem_t::unlocker_t(sem);
    }

    bool await_ready() {
        return sem->get_internal()->await_ready();
    }

    state_t *state = nullptr;
    sem_t *sem = nullptr;
    sem_waiter_handle_p psem_it;
    bool triggered = false;
};

inline sem_t::sem_t(pool_t *pool, int64_t val)
: internal(std::unique_ptr<sem_internal_t, deallocator_t<sem_internal_t>>(
        alloc<sem_internal_t>(pool, pool, val), dealloc_create<sem_internal_t>(pool)))
{
    get_internal()->pool->get_internal()->add_sem(this);
}

inline sem_t::~sem_t() {
    get_internal()->clear();
    get_internal()->pool->get_internal()->rm_sem(this);
}

inline sem_awaiter_t sem_t::wait() { return sem_awaiter_t(this); }
inline error_e       sem_t::signal(int64_t inc) { return internal->signal(inc); }
inline bool          sem_t::try_dec() { return internal->try_dec(); }

inline sem_internal_t *sem_t::get_internal() {
    return internal.get();
}

inline sem_p create_sem(pool_t *pool, int64_t val) {
    auto ret = std::shared_ptr<sem_t>(
            alloc<sem_t>(pool, pool, val), dealloc_create<sem_t>(pool), allocator_t<int>{pool}); 
    return ret;
}
inline sem_p create_sem(pool_p pool, int64_t val) {
    return create_sem(pool.get(), val);
}

inline task<sem_p> create_sem(int64_t val) {
    co_return create_sem(co_await get_pool(), val);
}

/* Functions
------------------------------------------------------------------------------------------------  */

template <typename ...ret_v>
inline task<std::tuple<ret_v...>> wait_all(task<ret_v>... tasks) {
    auto pool = co_await get_pool();

    std::tuple<task<ret_v>...> futures = {create_future(pool, tasks)...};

    (co_await sched(tasks), ...);

    co_return co_await std::apply([](auto&&... futures) -> task<std::tuple<ret_v...>> {
        co_return std::tuple<ret_v...>{co_await futures...};
    }, futures);
}

template <typename T>
inline sched_awaiter_t<T> sched(task<T> to_sched, const modif_pack_t& v) {
    return sched_awaiter_t(to_sched, v);
}

template <typename Awaiter>
inline task_t await(Awaiter&& awaiter) {
    co_await std::forward<Awaiter>(awaiter);
    co_return ERROR_OK;
}

inline yield_awaiter_t yield() {
    return yield_awaiter_t{};
}

inline task<pool_t *> get_pool() {
    get_pool_awaiter_t awaiter;
    co_return co_await awaiter;
}

inline task<state_t *> get_state() {
    get_state_awaiter_t awaiter;
    co_return co_await awaiter;
}

inline task_t stop_io(const io_desc_t& io_desc) {
    co_return (co_await get_pool())->get_internal()->stop_io(io_desc, ERROR_WAKEUP);
}

inline task_t force_stop(int64_t stopval) {
    struct stop_awaiter_t {
        stop_awaiter_t(int64_t stopval) : stopval(stopval) {}

        bool await_ready() { return false; };

        handle<void> await_suspend(handle<task_state_t<int>> h) {
            state = &h.promise().state;
            do_leave_modifs(state);
            state->pool->get_internal()->push_ready_front(state);
            state->pool->get_internal()->ret_val = RUN_STOPPED;
            state->pool->stopval = stopval;
            return std::noop_coroutine();
        }

        error_e await_resume() {
            do_entry_modifs(state);
            return ERROR_OK; /* no errors to be had here */
        }

        int64_t stopval = 0;
        state_t *state = nullptr;
    };
    /* this interrupts the pool, if you continue it, it will continue from this place */
    co_return (co_await stop_awaiter_t{stopval});
}

inline task_t sleep(const std::chrono::microseconds& timeo) {
    auto pool = co_await get_pool();
    io_desc_t timer;
    error_e err;
    if ((err = pool->get_internal()->get_timer(timer)) != ERROR_OK) {
        COLIB_DEBUG("FAILED to get a timer %s", dbg_enum(err).c_str());
        co_return err;
    }
    if ((err = pool->get_internal()->set_timer(timer, timeo)) != ERROR_OK) {
        COLIB_DEBUG("FAILED to set a timer %s", dbg_enum(err).c_str());
        error_e err_err;
        if ((err_err = pool->get_internal()->free_timer(timer)) != ERROR_OK) {
            COLIB_DEBUG("FAILED to free(on error: %s) the timer %s",
                    dbg_enum(err).c_str(), dbg_enum(err_err).c_str());
        }
        co_return err;
    }

    FnScope scope([pool, &timer] {
        /* this function can be called on a kill, it needs to be able to be called inside the
        destructor */
        error_e err_err;
        if ((err_err = pool->get_internal()->free_timer(timer)) != ERROR_OK) {
            COLIB_DEBUG("FAILED to free(on error: %s) the timer %s",
                    dbg_enum(err_err).c_str());
        }
    });
    io_awaiter_t awaiter(timer);
    if ((err = co_await awaiter) != ERROR_OK) {
        COLIB_DEBUG("FAILED waiting on timer: %s", dbg_enum(err).c_str());
        scope.precall();
        co_return err;
    }

    scope.precall();
    co_return ERROR_OK;
}

inline task_t sleep_us(uint64_t timeo_us) {
    co_return co_await sleep(std::chrono::microseconds(timeo_us));
}

inline task_t sleep_ms(uint64_t timeo_ms) {
    co_return co_await sleep(std::chrono::milliseconds(timeo_ms));
}

inline task_t sleep_s(uint64_t timeo_s) {
    co_return co_await sleep(std::chrono::seconds(timeo_s));
}

inline task_t wait_event(const io_desc_t& io_desc) {
    io_awaiter_t awaiter(io_desc);
    co_return (co_await awaiter);
}

#if COLIB_OS_LINUX

inline task_t stop_fd(int fd) {
    /* gets the fd out of the poll, awakening all it's waiters with error_e ERROR_WAKEUP */
    co_return (co_await stop_io(io_desc_t{.fd = fd}));
}

inline task_t connect(int fd, sockaddr *sa, socklen_t len) {
    /* connect is special, first we take it and make it non-blocking for the initial connection */
    int flags;
    if ((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        COLIB_DEBUG("FAILED to get old flags for fd[%d] %s[%d]",
                fd, strerror(errno), errno);
        co_return ERROR_GENERIC;
    }
    bool need_nonblock = (flags & O_NONBLOCK) == 0;
    if (need_nonblock && (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)) {
        COLIB_DEBUG("FAILED to toggle on the O_NONBLOCK flag on fd[%d] %s[%d]",
                fd, strerror(errno), errno);
        co_return ERROR_GENERIC;
    }

    int res = ::connect(fd, sa, len);

    if (need_nonblock && (flags = fcntl(fd, F_GETFL, 0) < 0)) {
        COLIB_DEBUG("FAILED to get the new flags for the fd[%d] %s[%d]",
                fd, strerror(errno), errno);
        co_return ERROR_GENERIC;
    }
    if (need_nonblock && (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)) {
        COLIB_DEBUG("FAILED to toggle off the O_NONBLOCK flag on fd[%d] %s [%d]",
                fd, strerror(errno), errno);
        co_return ERROR_GENERIC;
    }

    /* now that our intention to connect was sent to the os we first check if the connection was
    not done */
    if (res < 0 && errno != EINPROGRESS) {
        co_return res;
    }

    if (res == 0)
        co_return 0;

    /* if not, then we need to create an awaiter */
    io_awaiter_t awaiter{io_desc_t{
        .fd = fd,
        .events = EPOLLOUT,
    }};
    error_e err = co_await awaiter;

    if (err != ERROR_OK) {
        COLIB_DEBUG("Failed waiting operation on %d co_err: %s errno: %s[%d]",
                fd, dbg_enum(err).c_str(), strerror(errno), errno);
        co_return err;
    }

    /* now that we where signaled back by the os, we can check that we are connected and return: */
    int result;
    socklen_t result_len = sizeof(result);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
        COLIB_DEBUG("FAILED getsockopt: fd: %d err: %s[%d]", fd, strerror(errno), errno);
        co_return ERROR_GENERIC;
    }

    if (result != 0) {
        COLIB_DEBUG("FAILED connect: fd: %d err: %s[%d]", fd, strerror(errno), errno);
        co_return result;
    }

    co_return ERROR_OK;
}

inline task_t accept(int fd, sockaddr *sa, socklen_t *len) {
    io_awaiter_t awaiter(io_desc_t{
        .fd = fd,
        .events = EPOLLIN | EPOLLHUP,
    });
    error_e err = co_await awaiter;
    if (err != ERROR_OK) {
        co_return err;
    }
    /* OBS: those functions are a bit special, they return the result of the operation, not only
    the enums listed in the lib */
    co_return ::accept(fd, sa, len); 
}

inline task<ssize_t> read(int fd, void *buff, size_t len) {
    io_awaiter_t awaiter(io_desc_t{
        .fd = fd,
        .events = EPOLLIN,
    });
    error_e err = co_await awaiter;
    if (err != ERROR_OK) {
        co_return err;
    }
    /* OBS: those functions are a bit special, they return the result of the operation, not only
    the enums listed in the lib */
    co_return ::read(fd, buff, len);
}

inline task<ssize_t> write(int fd, const void *buff, size_t len) {
    io_awaiter_t awaiter(io_desc_t{
        .fd = fd,
        .events = EPOLLOUT,
    });
    error_e err = co_await awaiter;
    if (err != ERROR_OK) {
        co_return err;
    }
    /* OBS: those functions are a bit special, they return the result of the operation, not only
    the enums listed in the lib */
    co_return ::write(fd, buff, len);
}

inline task_t read_sz(int fd, void *buff, size_t len) {
    ssize_t original_len = len;
    while (true) {
        if (!len)
            break ;
        ssize_t ret = co_await read(fd, buff, len);
        if (ret == 0) {
            COLIB_DEBUG("Read failed, peer is closed, fd: %d", fd);
            co_return ERROR_GENERIC;
        }
        else if (ret < 0) {
            COLIB_DEBUG("Failed read, fd: %d", fd);
            co_return ERROR_GENERIC;
        }
        else {
            len -= ret;
            buff = (char *)buff + ret;
        }
    }
    co_return ERROR_OK;
}

inline task_t write_sz(int fd, const void *buff, size_t len) {
    ssize_t original_len = len;
    while (true) {
        if (!len)
            break ;
        ssize_t ret = co_await write(fd, buff, len);
        if (ret < 0) {
            COLIB_DEBUG("Failed write, fd: %d", fd);
            co_return ERROR_GENERIC;
        }
        else {
            len -= ret;
            buff = (char *)buff + ret;
        }
    }
    co_return ERROR_OK;
}

#endif /* COLIB_OS_LINUX */ 

#if COLIB_OS_WINDOWS

inline LPFN_CONNECTEX   _connect_ex = NULL;
inline LPFN_ACCEPTEX    _accept_ex = NULL;  
inline LPFN_WSASENDMSG  _wsa_send_msg = NULL;
inline LPFN_WSARECVMSG  _wsa_recv_msg = NULL;

inline io_desc_t create_io_desc(pool_t *pool) {
    io_desc_t new_desc = io_desc_t {
        .data = std::shared_ptr<io_data_t>(alloc<io_data_t>(pool),
                dealloc_create<io_data_t>(pool), allocator_t<int>{pool}),
        .h = NULL
    };
    
    *new_desc.data = io_data_t {
        .flags = io_data_t::IO_FLAG_NONE,
        .state = nullptr,
        .ptr = nullptr,
        .h = new_desc.h
    };

    return new_desc;
}

template <typename Fn>
inline error_e load_win_fn(GUID guid, Fn& fn) {
    SOCKET sock;
    DWORD dwBytes;
    int ret;

    /* Dummy socket needed for WSAIoctl */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        COLIB_DEBUG("Failed to create dumy socket: %s", get_last_error().c_str());
        return ERROR_GENERIC;
    }

    ret = WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
                  &guid, sizeof(guid),
                  &fn, sizeof(fn),
                  &dwBytes, NULL, NULL);
    if (ret != 0) {
        COLIB_DEBUG("Failed to load extension function: %s", get_last_error().c_str());
        return ERROR_GENERIC;
    }

    ret = closesocket(sock);
    if (ret != 0) {
        COLIB_DEBUG("Failed to close dumy socket: %s", get_last_error().c_str());
        return ERROR_GENERIC;
    }

    return ERROR_OK;
}

inline error_e handle_done_req(io_data_t *data, error_e err, DWORD *len, uint64_t *offset) {
    if (err != ERROR_OK) {
        CloseHandle(data->overlapped.hEvent);
        COLIB_DEBUG("FAILED: %s", get_last_error().c_str());
        return ERROR_GENERIC;
    }
    if (len)
        *len = data->recvlen;
    if (!CloseHandle(data->overlapped.hEvent)) {
        COLIB_DEBUG("Failed to close event: %s", get_last_error().c_str());
        return ERROR_GENERIC;
    }
    if (offset) {
        *offset = 0;
        *offset |= data->overlapped.Offset;
        *offset |= (uint64_t(data->overlapped.OffsetHigh) << 32);
    }
    return ERROR_OK;
}

inline task_t stop_handle(HANDLE h) {
    co_return co_await stop_io(io_desc_t{ .data = nullptr, .h = h });
}

/* Those functions are the same as their Windows API equivalent, the difference is that they don't
expose the overlapped structure, which is used by the coro library. They require a handle that
is compatible with iocp and they will attach the handle to the iocp instance. Those are the
functions listed by msdn to work with iocp (and connect, that is part of an extension) */
inline task<BOOL> ConnectEx(SOCKET  s, const sockaddr *name, int namelen, PVOID lpSendBuffer,
        DWORD dwSendDataLength, LPDWORD lpdwBytesSent)
{
    if (!_connect_ex && load_win_fn(WSAID_CONNECTEX, _connect_ex) != ERROR_OK) {
        COLIB_DEBUG("Can't load extension");
        co_return false;
    }

    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{s, name, namelen, lpSendBuffer, dwSendDataLength, lpdwBytesSent,
            &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)s;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(_connect_ex, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpdwBytesSent, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> AcceptEx( SOCKET sListenSocket, SOCKET sAcceptSocket, PVOID lpOutputBuffer,
        DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength,
        LPDWORD lpdwBytesReceived)
{
    if (!_accept_ex && load_win_fn(WSAID_ACCEPTEX, _accept_ex) != ERROR_OK) {
        COLIB_DEBUG("Can't load extension");
        co_return false;
    }

    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{sListenSocket, sAcceptSocket, lpOutputBuffer, dwReceiveDataLength,
            dwLocalAddressLength, dwRemoteAddressLength, lpdwBytesReceived, &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->h = desc.h = (HANDLE)sListenSocket;
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(_accept_ex, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpdwBytesReceived, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> ConnectNamedPipe(HANDLE hNamedPipe) {
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hNamedPipe, &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->h = desc.h = hNamedPipe;
    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::ConnectNamedPipe, *params);
        if (!ret && (GetLastError() != ERROR_IO_PENDING || GetLastError() == ERROR_PIPE_CONNECTED))
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, NULL, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer,
            DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer,
            nOutBufferSize, lpBytesReturned, &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->h = desc.h = hDevice;
    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::DeviceIoControl, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpBytesReturned, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> LockFileEx(HANDLE hFile, DWORD dwFlags, DWORD dwReserved,
        DWORD nNumberOfBytesToLockLow, DWORD nNumberOfBytesToLockHigh, uint64_t *offset)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hFile, dwFlags, dwReserved, nNumberOfBytesToLockLow,
            nNumberOfBytesToLockHigh, &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->h = desc.h = hFile;
    if (offset) {
        desc.data->overlapped.Offset = (*offset) & 0xffffffff;
        desc.data->overlapped.OffsetHigh = (*offset) >> 32;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::LockFileEx, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, NULL, offset) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> ReadDirectoryChangesW(HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength,
        BOOL bWatchSubtree, DWORD dwNotifyFilter, LPDWORD lpBytesReturned,
        LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hDirectory, lpBuffer, nBufferLength, bWatchSubtree, dwNotifyFilter,
            lpBytesReturned, &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->h = desc.h = hDirectory;
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::ReadDirectoryChangesW, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpBytesReturned, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
        LPDWORD lpNumberOfBytesRead)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,
            &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->h = desc.h = hFile;
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::ReadFile, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesRead, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> TransactNamedPipe(HANDLE hNamedPipe, LPVOID lpInBuffer, DWORD nInBufferSize,
        LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesRead)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hNamedPipe, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize,
            lpBytesRead, &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->h = desc.h = hNamedPipe;
    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::TransactNamedPipe, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpBytesRead, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;

}

inline task<BOOL> WaitCommEvent(HANDLE  hFile, LPDWORD lpEvtMask) {
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hFile, lpEvtMask, &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->h = desc.h = hFile;
    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::WaitCommEvent, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, NULL, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WriteFile(HANDLE  hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
        LPDWORD lpNumberOfBytesWritten, uint64_t *offset)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten,
            &desc.data->overlapped};
    using params_t = decltype(params);

    desc.data->h = desc.h = hFile;
    if (offset) {
        desc.data->overlapped.Offset = (*offset) & 0xffffffff;
        desc.data->overlapped.OffsetHigh = (*offset) >> 32;
    }
    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::WriteFile, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesWritten, offset) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WSASendMsg(SOCKET handle, LPWSAMSG lpMsg, DWORD dwFlags,
        LPDWORD lpNumberOfBytesSent, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    if (!_wsa_send_msg && load_win_fn(WSAID_WSASENDMSG, _wsa_send_msg) != ERROR_OK) {
        COLIB_DEBUG("Can't load extension");
        co_return false;
    }
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{handle, lpMsg, dwFlags, lpNumberOfBytesSent,
            &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)handle;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(_wsa_send_msg, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesSent, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
        LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const sockaddr *lpTo, int iTolen,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo,
            iTolen, &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)s;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::WSASendTo, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesSent, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
        LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
            &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)s;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::WSASend, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesSent, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
        LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, sockaddr *lpFrom, LPINT lpFromlen,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom,
            lpFromlen, &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)s;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::WSARecvFrom, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesRecvd, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WSARecvMsg(SOCKET s, LPWSAMSG lpMsg, LPDWORD lpdwNumberOfBytesRecvd,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    if (!_wsa_recv_msg && load_win_fn(WSAID_WSARECVMSG, _wsa_recv_msg) != ERROR_OK) {
        COLIB_DEBUG("Can't load extension");
        co_return false;
    }

    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{s, lpMsg, lpdwNumberOfBytesRecvd,
            &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)s;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(_wsa_recv_msg, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpdwNumberOfBytesRecvd, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

inline task<BOOL> WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
        LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    auto desc = create_io_desc(co_await get_pool());

    auto params = std::tuple{s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
            &desc.data->overlapped, lpCompletionRoutine};
    using params_t = decltype(params);

    desc.data->h = desc.h = (HANDLE)s;
    desc.data->overlapped.hEvent = WSACreateEvent();
    if (!desc.data->overlapped.hEvent) {
        COLIB_DEBUG("Failed to create event: %s", get_last_error().c_str());
        co_return false;
    }
    desc.data->io_request = +[](void *ptr) -> error_e {
        params_t *params = (params_t *)ptr;
        bool ret = std::apply(::WSARecv, *params);
        if (!ret && GetLastError() != ERROR_IO_PENDING)
            return ERROR_GENERIC;
        return ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    error_e ret = co_await io_awaiter_t(desc);
    if (handle_done_req(desc.data.get(), ret, lpNumberOfBytesRecvd, NULL) != ERROR_OK) {
        COLIB_DEBUG("FAILED request: %s", get_last_error().c_str());
        co_return false;
    }
    co_return true;
}

/* Those are here to increase the compatibility with the linux functions  */
inline task_t connect(SOCKET s, const sockaddr *sa, uint32_t len) {
    struct sockaddr_in tmp_addr;
    ZeroMemory(&tmp_addr, sizeof(tmp_addr));
    tmp_addr.sin_family = AF_INET;
    tmp_addr.sin_addr.s_addr = INADDR_ANY;
    tmp_addr.sin_port = 0;

    /* ConnectEx requires the socket to be initially bound */
    int rc = bind(s, (SOCKADDR*) &tmp_addr, sizeof(tmp_addr));
    if (rc != 0) {
        COLIB_DEBUG("Failed first bind: %s", get_last_error().c_str());
        co_return ERROR_GENERIC;
    }

    BOOL ok = co_await ConnectEx(s, sa, len, NULL, 0, NULL);
    co_return ok ? ERROR_OK : ERROR_GENERIC;
}

inline task<SOCKET> accept(SOCKET s, sockaddr *sa, uint32_t *len) {
    SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (client_sock == INVALID_SOCKET) {
        COLIB_DEBUG("Failed to construct client sock: %s", get_last_error().c_str());
        co_return INVALID_SOCKET;
    }

    std::vector<uint8_t> addr_buff((*len + 16) * 2);
    DWORD rlen = 0;

    BOOL ok = co_await AcceptEx(s, client_sock, &addr_buff[0], 0, *len + 16, *len + 16, &rlen);
    if (!ok) {
        COLIB_DEBUG("Failed accept: %s", get_last_error().c_str());
        closesocket(client_sock);
        co_return INVALID_SOCKET;
    }

    memcpy(sa, &addr_buff[0], *len);
    co_return client_sock;
}

inline task<SSIZE_T> read(HANDLE h, void *buff, size_t len) {
    DWORD nread = 0;
    BOOL ok = co_await ReadFile(h, buff, (DWORD)len, &nread);
    if (!ok) {
        COLIB_DEBUG("Failed read");
        co_return ERROR_GENERIC;
    }
    co_return nread;
}

inline task<SSIZE_T> write(HANDLE h, const void *buff, size_t len, uint64_t *offset) {
    DWORD nwrite = 0;
    BOOL ok = co_await WriteFile(h, buff, (DWORD)len, &nwrite, offset);
    if (!ok) {
        COLIB_DEBUG("Failed write");
        co_return ERROR_GENERIC;
    }
    co_return nwrite;
}

inline task_t read_sz(HANDLE h, void *buff, size_t len) {
    SSIZE_T original_len = len;
    while (true) {
        if (!len)
            break ;
        SSIZE_T ret = co_await read(h, buff, len);
        if (ret == 0) {
            COLIB_DEBUG("Read failed, peer is closed");
            co_return ERROR_GENERIC;
        }
        else if (ret < 0) {
            COLIB_DEBUG("Failed read");
            co_return ERROR_GENERIC;
        }
        else {
            len -= ret;
            buff = (char *)buff + ret;
        }
    }
    co_return ERROR_OK;
}

inline task_t write_sz(HANDLE h, const void *buff, size_t len, uint64_t *offset) {
    SSIZE_T original_len = len;
    while (true) {
        if (!len)
            break ;
        SSIZE_T ret = co_await write(h, buff, len, offset);
        if (ret < 0) {
            COLIB_DEBUG("Failed write");
            co_return ERROR_GENERIC;
        }
        else {
            len -= ret;
            buff = (char *)buff + ret;
        }
    }
    co_return ERROR_OK;
}

#endif /* COLIB_OS_WINDOWS */

#if COLIB_OS_UNKNOWN
/* you implement your own */
#endif /* COLIB_OS_UNKNOWN */

struct task_modifs_getter_t {
    modif_table_p table;

    bool          await_ready() noexcept { return false; }
    modif_table_p await_resume() noexcept { return table; }

    template <typename P>
    bool await_suspend(handle<P> h) noexcept {
        table = h.promise().state.modif_table;
        return false;
    }
};

inline std::vector<modif_p> get_task_modifs(modif_table_p table) {
    std::vector<modif_p> ret;
    if (!table)
        return ret;
    for (auto &cbk_table : table->table) {
        for (auto &modif : cbk_table)
            ret.push_back(modif);
    }
    return ret;
}

template <typename T>
inline std::vector<modif_p> task_modifs(task<T> t) {
    auto mt = t.h.promise().state.modif_table;
    return get_task_modifs(mt);
}

inline task<std::vector<modif_p>> task_modifs() {
    auto table = co_await task_modifs_getter_t{};
    co_return get_task_modifs(table);
}

inline void add_modifs_to_table(modif_table_p table, std::set<modif_p> mods) {
    if (!table)
        return;
    for (auto &cbk_table : table->table)
        for (auto &modif : cbk_table)
            if (has(mods, modif))
                mods.erase(modif);
    for (auto &mod : mods)
        if (mod)
            table->table[mod->type].push_back(mod);
}

inline void rm_modifs_from_table(modif_table_p table, const std::set<modif_p>& mods) {
    if (!table)
        return;
    for (auto &cbk_table : table->table) {
        cbk_table.erase(std::remove_if(cbk_table.begin(), cbk_table.end(),
                [&mods](modif_p m) { return has(mods, m); }), cbk_table.end());
    }
}

template <typename T>
inline task<T> add_modifs(pool_t *pool, task<T> t, const std::set<modif_p>& mods) {
    auto &table = t.h.promise().state.modif_table;
    if (!table)
        table = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
                dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});
    add_modifs_to_table(table, mods);
    if (get_modif_table_sz(table) == 0)
        table = nullptr;
    return t;
}

template <typename T>
inline task<T> rm_modifs(task<T> t, const std::set<modif_p>& mods) {
    auto &table = t.h.promise().state.modif_table;
    rm_modifs_from_table(table, mods);
    if (get_modif_table_sz(table) == 0)
        table = nullptr;
    return t;
}

inline task_t add_modifs(const std::set<modif_p>& mods) {
    auto &table = (co_await get_state())->modif_table;
    auto pool = co_await get_pool();
    if (!table)
        table = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
                dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});
    add_modifs_to_table(table, mods);
    if (get_modif_table_sz(table) == 0)
        table = nullptr;
    co_return ERROR_OK;
}

inline task_t rm_modifs(const std::set<modif_p>& mods) {
    auto table = co_await task_modifs_getter_t{};
    rm_modifs_from_table(table, mods);
    if (get_modif_table_sz(table) == 0)
        table = nullptr;
    co_return ERROR_OK;
}

template <typename T>
inline task<T> create_future(pool_t *pool, task<T> t) {
    using data_t = std::pair<T, bool>;

    auto sem = create_sem(pool, 0);
    auto data = std::shared_ptr<data_t>(alloc<data_t>(pool),
            dealloc_create<data_t>(pool), allocator_t<int>{pool});

    data->second = false;

    /* ! this std::function will not be used using our allocator */
    auto exit_func = [sem, data](state_t *state) -> error_e {
        typename task<T>::handle_t h = task<T>::handle_t::from_address(state->self.address());
        data->first = std::get<T>(h.promise().ret);
        data->second = true;
        sem->signal();
        return ERROR_OK;
    };

    add_modifs(pool, t, std::set<modif_p>{
            create_modif<CO_MODIF_EXIT_CBK>(pool, CO_MODIF_INHERIT_NONE, exit_func)});

    return [](sem_p sem, std::shared_ptr<data_t> data) -> task<T> {
        if (!data->second) {
            co_await sem->wait();
        }
        co_return data->first;
    }(sem, data);
}

template <typename T>
inline task<std::pair<T, error_e>> create_timeo(
        task<T> t, pool_t *pool, const std::chrono::microseconds& timeo)
{
    struct timer_state_t {
        std::function<error_e(void)> timer_elapsed_sig;
        std::function<error_e(void)> timer_sig;
        sem_p sem;
        uint64_t duration;
        error_e tstate_err;
        task<T> t;
        T ret;
    };

    auto tstate = std::shared_ptr<timer_state_t>(alloc<timer_state_t>(pool),
            dealloc_create<timer_state_t>(pool), allocator_t<int>{pool});

    auto [timer_elapsed_killer, timer_elapsed_sig] = create_killer(pool, ERROR_WAKEUP);
    auto [timer_killer, timer_sig] = create_killer(pool, ERROR_TIMEO);

    tstate->sem = create_sem(pool, 0);
    tstate->timer_elapsed_sig = timer_elapsed_sig;
    tstate->timer_sig = timer_sig;
    tstate->duration = timeo.count();
    tstate->tstate_err = ERROR_OK;
    tstate->t = t;

    auto exec_coro = [](std::shared_ptr<timer_state_t> tstate) -> task_t {
        tstate->ret = co_await tstate->t;
        tstate->timer_sig();
        tstate->sem->signal();
        tstate->tstate_err = ERROR_OK;
        co_return ERROR_OK;
    }(tstate);

    auto timer_coro = [](std::shared_ptr<timer_state_t> tstate) -> task_t {
        error_e err;
        if ((err = co_await sleep_us(tstate->duration)) != ERROR_OK) {
            tstate->tstate_err = err;
            tstate->timer_elapsed_sig();
            co_return ERROR_GENERIC;
        }
        tstate->tstate_err = ERROR_TIMEO;
        tstate->timer_elapsed_sig();
        tstate->sem->signal();
        co_return ERROR_OK;
    }(tstate);

    add_modifs(pool, timer_coro, timer_killer);
    add_modifs(pool, exec_coro, timer_elapsed_killer);

    pool->sched(timer_coro);
    pool->sched(exec_coro);

    return [](std::shared_ptr<timer_state_t> tstate) -> task<std::pair<T, error_e>>{
        co_await tstate->sem->wait();
        co_return {tstate->ret, tstate->tstate_err};
    }(tstate);
}

/* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
/* this is inherited by-call and it must kill all coros in the call path and also stop all waiters
(io and sem) */
/* CAUTION: This will be similar to an exception thrown on the active await and the call stack */
inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e) {
    struct kill_state_t {
        std::stack<state_t *> call_stack;
        io_desc_t io_desc;
        sem_t *sem = nullptr;
        sem_waiter_handle_p it;
    };

    auto kstate = std::shared_ptr<kill_state_t>(alloc<kill_state_t>(pool),
            dealloc_create<kill_state_t>(pool), allocator_t<int>{pool});

    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;

    /* ! those std::functions will not be used using our allocator */
    auto sig_kill = [kstate, e]() -> error_e {
        /* If there is no call stack we have nothing to awake */
        if (kstate->call_stack.size() == 0)
            return ERROR_GENERIC;

        /* the top of the stack holds a pointer to the pool */
        auto pool = kstate->call_stack.top()->pool;

        /* if we are waiting for something in the top level we will first try to see if the last
        pushed state is ready for resuming, case in which we remove it from the ready queue. Else
        we remove it from the io or semaphore waiting queue, whichever was holding the awaiter. */
        if ((kstate->sem || kstate->io_desc.is_valid()) &&
                pool->get_internal()->remove_ready(kstate->call_stack.top()))
        {
            kstate->sem = nullptr;
            kstate->io_desc = io_desc_t{};
        }
        else if (kstate->sem) {
            kstate->sem->get_internal()->erase_waiter(*kstate->it);
            kstate->sem = nullptr;
        }
        else if (kstate->io_desc.is_valid()) {
            pool->get_internal()->stop_io(kstate->io_desc, ERROR_WAKEUP);
            kstate->io_desc = io_desc_t{};
        }

        /* Now we unwind the call stack, removing all except the last one. The last one will be
        removed by it's caller */
        while (kstate->call_stack.size() > 1) {
            kstate->call_stack.top()->self.destroy();
            kstate->call_stack.pop();
        }

        /* Finally we prepare the root of the trace and schedule it's caller */
        kstate->call_stack.top()->err = e;
        pool->get_internal()->push_ready(kstate->call_stack.top());
        kstate->call_stack.pop();
        return ERROR_OK;
    };

    modif_pack_t pack;
    pack.push_back(create_modif<CO_MODIF_CALL_CBK>(pool, flags,
        [kstate](state_t *s) -> error_e {
            kstate->call_stack.push(s);
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_EXIT_CBK>(pool, flags,
        [kstate](state_t *s) -> error_e {
            kstate->call_stack.pop();
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_WAIT_IO_CBK>(pool, flags,
        [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
            kstate->io_desc = io_desc;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_UNWAIT_IO_CBK>(pool, flags,
        [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
            kstate->io_desc = io_desc_t{};
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(pool, flags,
        [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
            kstate->sem = sem;
            kstate->it = it;
            return ERROR_OK;
        }
    ));
    pack.push_back(create_modif<CO_MODIF_UNWAIT_SEM_CBK>(pool, flags,
        [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
            kstate->sem = nullptr;
            return ERROR_OK;
        }
    ));

    return {pack, sig_kill};
}

/* Debug stuff
------------------------------------------------------------------------------------------------- */

template <typename T, typename ...Args>
inline task<T> dbg_register_name(task<T> t, const char *fmt, Args&&... args) {
    dbg_register_name(t.h.address(), fmt, std::forward<Args>(args)...);
    return t;
}

template <typename P, typename ...Args>
inline handle<P> dbg_register_name(handle<P> h, const char *fmt, Args&&... args) {
    dbg_register_name(h.address(), fmt, std::forward<Args>(args)...);
    return h;
}

template <typename T>
inline dbg_string_t dbg_name(task<T> t) {
    return dbg_name(t.h.address());
}

template <typename P>
inline dbg_string_t dbg_name(handle<P> h) {
    return dbg_name(h.address());
}

inline dbg_string_t dbg_enum(error_e code) {
    switch (code) {
        case ERROR_YIELDED: return dbg_string_t{"ERROR_YIELDED",   allocator_t<char>{nullptr}};
        case ERROR_OK:      return dbg_string_t{"ERROR_OK",        allocator_t<char>{nullptr}};
        case ERROR_GENERIC: return dbg_string_t{"ERROR_GENERIC",   allocator_t<char>{nullptr}};
        case ERROR_TIMEO:   return dbg_string_t{"ERROR_TIMEO",     allocator_t<char>{nullptr}};
        case ERROR_WAKEUP:  return dbg_string_t{"ERROR_WAKEUP",    allocator_t<char>{nullptr}};
        case ERROR_USER:    return dbg_string_t{"ERROR_USER",      allocator_t<char>{nullptr}};
        case ERROR_DEPEND:  return dbg_string_t{"ERROR_DEPEND",    allocator_t<char>{nullptr}};
        default:            return dbg_string_t{"[ERROR_UNKNOWN]", allocator_t<char>{nullptr}};
    }
}

inline dbg_string_t dbg_enum(run_e code) {
    dbg_string_t ret{"", allocator_t<char>{nullptr}};
    switch (code) {
        case RUN_OK:      return dbg_string_t{"RUN_OK"       , allocator_t<char>{nullptr}};      
        case RUN_ERRORED: return dbg_string_t{"RUN_ERRORED"  , allocator_t<char>{nullptr}}; 
        case RUN_ABORTED: return dbg_string_t{"RUN_ABORTED"  , allocator_t<char>{nullptr}}; 
        case RUN_STOPPED: return dbg_string_t{"RUN_STOPPED"  , allocator_t<char>{nullptr}};
        default:          return dbg_string_t{"[RUN_UNKNOWN]", allocator_t<char>{nullptr}};
    }
}

#if COLIB_OS_LINUX
inline dbg_string_t dbg_epoll_events(uint32_t events) {
    dbg_string_t ret{"[", allocator_t<char>{nullptr}};
    if (events & EPOLLIN)    ret += "EPOLLIN|";
    if (events & EPOLLOUT)   ret += "EPOLLOUT|";
    if (events & EPOLLRDHUP) ret += "EPOLLRDHUP|";
    if (events & EPOLLPRI)   ret += "EPOLLPRI|";
    if (events & EPOLLERR)   ret += "EPOLLERR|";
    if (events & EPOLLHUP)   ret += "EPOLLHUP|";
    if (ret.size() > 1)
        ret[ret.size() - 1] = ']';
    else
        ret += "]";
    return ret;
}
#endif /* COLIB_OS_LINUX */

inline modif_pack_t dbg_create_tracer(pool_t *pool) {
    modif_flags_e flags = modif_flags_e(CO_MODIF_INHERIT_ON_CALL | CO_MODIF_INHERIT_ON_SCHED);
    modif_pack_t mods;

    mods.push_back(create_modif<CO_MODIF_CALL_CBK>(pool, flags, [&](state_t *s) -> error_e {
        COLIB_DEBUG(">  CALL: %s", dbg_name(s->self).c_str());
        return ERROR_OK;
    }));
    mods.push_back(create_modif<CO_MODIF_SCHED_CBK>(pool, flags, [&] (state_t *s) -> error_e {
        COLIB_DEBUG("> SCHED: %s", dbg_name(s->self).c_str());
        return ERROR_OK;
    }));
    mods.push_back(create_modif<CO_MODIF_EXIT_CBK>(pool, flags, [&] (state_t *s) -> error_e {
        COLIB_DEBUG(">  EXIT: %s", dbg_name(s->self).c_str());
        return ERROR_OK;
    }));
    mods.push_back(create_modif<CO_MODIF_LEAVE_CBK>(pool, flags, [&] (state_t *s) -> error_e {
        COLIB_DEBUG("> LEAVE: %s", dbg_name(s->self).c_str());
        return ERROR_OK;
    }));
    mods.push_back(create_modif<CO_MODIF_ENTER_CBK>(pool, flags, [&] (state_t *s) -> error_e {
        COLIB_DEBUG("> ENTRY: %s", dbg_name(s->self).c_str());
        return ERROR_OK;
    }));
    mods.push_back(create_modif<CO_MODIF_WAIT_IO_CBK>(pool, flags,
        [&] (state_t *s, io_desc_t &io_desc) -> error_e {
            COLIB_DEBUG(">  WAIT: %s", dbg_name(s->self).c_str());
            return ERROR_OK;
        })
    );
    mods.push_back(create_modif<CO_MODIF_UNWAIT_IO_CBK>(pool, flags,
        [&] (state_t *s, io_desc_t &io_desc) -> error_e {
            COLIB_DEBUG(">UNWAIT: %s", dbg_name(s->self).c_str());
            return ERROR_OK;
        }
    ));
    mods.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(pool, flags,
        [&] (state_t *s, sem_t *sem, sem_waiter_handle_p _it) -> error_e {
            COLIB_DEBUG(">   SEM: %s", dbg_name(s->self).c_str());
            return ERROR_OK;
        }
    ));
    mods.push_back(create_modif<CO_MODIF_UNWAIT_SEM_CBK>(pool, flags,
        [&] (state_t *s, sem_t *sem, sem_waiter_handle_p _it) -> error_e {
            COLIB_DEBUG("> UNSEM: %s", dbg_name(s->self).c_str());
            return ERROR_OK;
        }
    ));
    return mods;
}

template <typename... Args>
inline dbg_string_t dbg_format(const char *fmt, Args&& ...args) {
    std::vector<char, allocator_t<char>> buff(allocator_t<char>{nullptr});

    int cnt = snprintf(NULL, 0, fmt, std::forward<Args>(args)...);
    if (cnt <= 0)
        return dbg_string_t{"[failed snprintf1]", allocator_t<char>{nullptr}};

    buff.resize(cnt + 1);
    if (snprintf(buff.data(), cnt + 1, fmt, std::forward<Args>(args)...) < 0)
        return dbg_string_t{"[failed snprintf2]", allocator_t<char>{nullptr}};

    return dbg_string_t{buff.data(), allocator_t<char>{nullptr}};
}

inline uint64_t dbg_get_time() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

#if COLIB_ENABLE_DEBUG_NAMES

using dbg_val_type = std::map<void *, std::pair<dbg_string_t, int>>::value_type;
inline std::map<void *,std::pair<dbg_string_t, int>, std::less<void *>, allocator_t<dbg_val_type>>
        dbg_names{allocator_t<dbg_val_type> {nullptr}};

template <typename ...Args>
inline void *dbg_register_name(void *addr, const char *fmt, Args&&... args) {
    if (!has(dbg_names, addr))
        dbg_names.insert({addr, {dbg_format(fmt, std::forward<Args>(args)...), 1}});
    else
        dbg_names.find(addr)->second.second++;
    return addr;
}

inline dbg_string_t dbg_name(void *v) {
    if (!has(dbg_names, v))
        dbg_register_name(v, "%p", v);
    return dbg_format("%s[%" PRIu64 "]", dbg_names.find(v)->second.first.c_str(),
            dbg_names.find(v)->second.second);
}

#else /* COLIB_ENABLE_DEBUG_NAMES */

template <typename ...Args>
inline void *dbg_register_name(void *addr, const char *fmt, Args&&... args) { return addr; }

inline dbg_string_t dbg_name(void *v) { return dbg_string_t{"", allocator_t<char>{nullptr}}; };

#endif /* COLIB_ENABLE_DEBUG_NAMES */

#if COLIB_ENABLE_LOGGING

inline void dbg_raw(const dbg_string_t& msg, const char *file, const char *func, int line) {
    if (log_str) {
        log_str(dbg_format("[%" PRIu64 "] %s:%4d %s() :> %s\n", dbg_get_time(),
                file, line, func, msg.c_str()));
    }
}

template <typename... Args>
inline void dbg(const char *file, const char *func, int line, const char *fmt, Args&&... args) {
    dbg_raw(dbg_format(fmt, std::forward<Args>(args)...), file, func, line);
}

#else /* COLIB_ENABLE_LOGGING */

template <typename... Args> /* no logging -> do nothing */
inline void dbg(const char *, const char *, int, const char *fmt, Args&&...) {}

#endif /* COLIB_ENABLE_LOGGING */

/* The end
------------------------------------------------------------------------------------------------- */

}

#endif /* COLIB_H */
