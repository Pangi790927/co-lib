# Co-Lib

### Introduction

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
the current coroutine until the *called* awaiter is complete and the co_yield suspends the current
coroutine, letting the *calling* coroutine to continue. The main point of coroutines is that while
a coroutine waits to be resumed another one can be executed, obtaining an asynchronous execution
similar to threads but on a single thread (Those can also be usually run on multiple threads,
but that is not supported by this library), this almost eliminates the need to use synchronization
mechanisms.

Let's consider an example of a coroutine calling another coroutine:

```cpp
/*14:*/ colib::task<int32_t> get_messages() {
/*15:*/     int value;
/*16:*/ 
/*17:*/     while (true) {
/*18:*/         value = co_await get_message();
/*19:*/         if (value == 0)
/*20:*/             break;
/*21:*/         co_yield value;
/*22:*/     }
/*22:*/     co_return 0;
/*23:*/ }
```
At *line 11*, the coroutine is declared. As you can see, coroutines need to declare their return value
of the type of their handler object, namely `colib::task<Type>`. That is because the coroutine holds the
return value inside the state of the coroutine, and the user only gets the handler to the coroutine.

At *line 15*, another awaiter, in this case another coroutine, is awaited with the use of `co_await`.
This will suspend the get_messages coroutine at that point, letting other coroutines on the system
run if there are any that need to do work, or block until a coroutine gets work to do. Finally,
this coroutine continues to *line 16* when a message becomes available. Note that this continuation
will happen if a) there are no things to do or b) if another coroutine awaits something and this
one is the next that waits for execution.

Assuming value is not 0, the coroutine yields at *line 18*, returning the value but keeping its state.
This state contains the variable value and some other internals of coroutines.

When the message 0 is received, the coroutine returns 0, freeing its internal state. You shouldn't
call the coroutine anymore after this point.

```cpp
/*24:*/ colib::task<int32_t> co_main() {
/*25:*/     colib::task<int32_t> messages = get_messages();
/*26:*/     while (int32_t value = co_await messages) {
/*27:*/         printf("main: %d\n", value);
/*28:*/         if (!value)
/*29:*/             break;
/*30:*/     }
/*31:*/     co_return 0;
/*32:*/ }
```

The coroutine that calls the above coroutine is `co_main`. You can observe the creation of the
coroutine at *line 25*; what looks like a call of the coroutine in fact allocates the coroutine state
and returns the handle that can be further awaited, as you can see in the for loop at *line 26*.

The coroutine will be called until value is 0, in which case we know that the coroutine has ended
(from its code) and we break from the for loop.

We observe that at *line 31* we `co_return 0;` that is because the `co_return` is mandatory at the end of
coroutines (as mandated by the language).

```cpp
#include "colib.h"

/* 0:*/ int cnt = 3;
/* 1:*/ colib::task<int32_t> get_message() {
/* 2:*/     co_await colib::sleep_s(1);
/* 3:*/     co_return cnt--;
/* 4:*/ }
/* 5:*/ 
/* 6:*/ colib::task<int32_t> co_timer() {
/* 7:*/     int x = 50;
/* 8:*/     while (x > 0) {
/* 9:*/         printf("timer: %d\n", x--);
/*10:*/         co_await colib::sleep_ms(100);
/*11:*/     }
/*12:*/     co_return 0;
/*13:*/ }
```

Now we can look at an example for `get_message` at *line 1*. Of course, in a real case, we would await a
message from a socket, for some device, etc., but here we simply wait for a timer of 1 second to
finish.

As for an example of something that can happen between awaits, we can look at co_timer at line 6.
This is another coroutine that prints x and waits 100 ms, 50 times. If you copy and run the message
yourself, you will see that the prints from the co_timer are more frequent and in-between the ones
from co_main.

```cpp
/*33:*/ int main() {
/*34:*/     colib::pool_p pool = colib::create_pool();
/*35:*/     pool->sched(co_main());
/*36:*/     pool->sched(co_timer());
/*37:*/     pool->run();
/*38:*/ }
```

Finally, we can look at main. As you can see, we create the pool at *line 34*, schedule the main
coroutine and the timer one, and we wait on the pool. The run function won't exit unless there are
no more coroutines to run or if a `force_awake` is called, or if an error occurs.

### Organization

- `colib.h` - Single-header implementation of the coroutine library.
- `tests.cpp` - Contains the tests for the library.
- `LICENSE` - The MIT license.
- `makefile` - The makefile used to build the tests.
- `a.out` or `tests.exe` - The resulting test executables.
