# Backend implementation: a mock example

A design sketch, **not compiled**. Nothing in `colib.h` supports this yet. It shows how an io
backend (the `io_desc_t` / `io_pool_t` / `timer_pool_t` part of colib) would be written from
outside once the header is restructured as discussed on 2026-09-24. The backend here is a mock:
in-process channels and a fake clock, useful for deterministic tests.

## The rules this design keeps

- **The user never edits a colib file**, and includes `colib.h` exactly once.
- **No code in macros.** `COLIB_OS_UNKNOWN_IO_DESC` / `COLIB_OS_UNKNOWN_IMPLEMENTATION` go away.
  The only defines are flags.
- **The backend's data is held by value** inside colib's own types, so there's no pointer to an
  `_internal_t`, and access is as fast as today.
- **The rest of `colib.h` is agnostic.** It calls the interface's member functions and never looks
  inside the backend's data.

## The files

| File | Owner | Contents |
|---|---|---|
| `colib_fwd.h` | colib | What a backend's data may need before `colib.h`: forward declarations (`state_t`, `pool_t`), `error_e`, `state_e`, and the class layouts of `state_list_t` and `allocator_t`, whose member functions are declared there and defined, still `inline`, in `colib.h` |
| `mock_pre.h` | backend | The backend's **data types**, complete: `io_desc_data_t`, `io_pool_data_t`, `timer_pool_data_t`. They only use what `colib_fwd.h` gives |
| `colib.h` | colib | The interface classes (`io_desc_t`, `io_pool_t`, `timer_pool_t`), which hold the data types by value, with member functions **declared only**, and the whole generic core |
| `mock_after.h` | backend | The **definitions**: the interface's member functions, and the backend's own io functions (here `mock_read`, `mock_write`) |
| `backend.h` | user | Stitches the three together; the only file the user's code includes |

The built-in backends (Linux, Windows, kqueue) are the same kind of pre/after pair, living inside
`colib.h` as sections. When no custom backend is given, `colib.h` uses its built-in pre section
near the top and its built-in after section at the bottom. So a normal user still writes one
`#include "colib.h"`.

```cpp
/* backend.h - the user's file */
#pragma once

#define COLIB_OS_UNKNOWN true       /* a flag: a backend is provided, don't use a built-in one */
#include "mock_pre.h"
#include "colib.h"
#include "mock_after.h"
```

---

## `colib_fwd.h` (shipped by colib, shown shortened)

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace colib {

struct state_t;
struct pool_t;
struct io_desc_t;       /* the interface classes, defined in colib.h: a backend's data may */
struct io_pool_t;       /* point at them */

enum error_e : int32_t { /* ERROR_FINISHED ... ERROR_DEPEND, as in colib.h today */ };
enum state_e : int32_t { STATE_LEFT, STATE_RUNNING, STATE_READY, STATE_WAITING_SEM,
                         STATE_WAITING_IO };

/* the layout is complete here, so a backend may hold one by value; the bodies are in colib.h */
struct state_list_t {
    state_list_t(state_e kind);
    void push_back(state_t *s);
    state_t *pop_front();
    bool empty() const;
    /* ... */
private:
    state_e kind;
    state_t *head = nullptr;
    state_t *tail = nullptr;
    size_t cnt = 0;
};

/* the same for the pool's allocator: only a pool_t * inside, the members defined in colib.h */
template <typename T>
struct allocator_t {
    using value_type = T;
    allocator_t(pool_t *pool) : pool(pool) {}
    template <typename U> allocator_t(const allocator_t<U> &oth) : pool(oth.pool) {}
    T *allocate(size_t n);
    void deallocate(T *p, size_t n);
    pool_t *pool;
};

} /* namespace colib */
```

## What `colib.h` declares for a backend (shown shortened)

These are today's `io_pool_t`/`timer_pool_t` functions, plus `completed()`. The backend provides the
bodies.

```cpp
namespace colib {

struct io_desc_t {
    io_desc_data_t data;                            /* the backend's, by value */
    bool same(const io_desc_t &oth) const;          /* the only comparison colib uses */
};

struct io_pool_t {
    io_pool_t(pool_t *pool, state_list_t &ready_tasks);
    ~io_pool_t();

    bool is_ok();
    error_e handle_ready();     /* nothing ready: wait for events and queue their waiters */
    error_e add_waiter(state_t *state, io_desc_t &io_desc);
    error_e force_awake(io_desc_t &io_desc, error_e retcode);
    bool completed(const io_desc_t &io_desc);   /* the io already did its work before the resume */
    error_e clear();            /* destroys every waiter */
    intptr_t get_internal_handle();

    io_pool_data_t data;                            /* the backend's, by value */
};

struct timer_pool_t {
    timer_pool_t(pool_t *pool, io_pool_t &io_pool);

    error_e get_timer(io_desc_t &new_timer);
    error_e set_timer(const io_desc_t &timer, const std::chrono::microseconds &time_us);
    error_e free_timer(io_desc_t &timer);

    timer_pool_data_t data;
};

/* pool_internal_t holds them by value, as today:
       io_pool_t io_pool;
       timer_pool_t timer_pool;          */

} /* namespace colib */
```

---

## `mock_pre.h`: the data types

The mock is readiness-based, like epoll. A read waits until its channel has bytes, and the bytes
are read after the resume, so `completed()` is always false. Timers run on a mock clock: when
nothing else can progress, the clock jumps to the next timer. Nothing ever really blocks.

```cpp
#pragma once
#include <vector>
#include "colib_fwd.h"

namespace colib {

struct mock_channel_t;          /* the user-facing object, defined in mock_after.h */

struct io_desc_data_t {
    enum kind_e : int32_t { MOCK_READ, MOCK_TIMER };

    kind_e kind = MOCK_READ;
    mock_channel_t *chan = nullptr;     /* reads: the channel */
    uint64_t due_us = 0;                /* timers: when, on the mock clock */
};

struct mock_waiter_t {
    io_desc_t *desc;            /* lives in the waiting coroutine's frame for the whole wait */
    state_t *state;
};

struct io_pool_data_t {
    pool_t *pool = nullptr;
    state_list_t *ready = nullptr;                      /* the pool's ready queue */
    std::vector<mock_waiter_t, allocator_t<mock_waiter_t>> waiting;    /* the pool's memory */
    uint64_t now_us = 0;                                /* the mock clock */
};

struct timer_pool_data_t {
    io_pool_t *io = nullptr;
};

} /* namespace colib */
```

`mock_waiter_t` names `io_desc_t`, which is only defined in `colib.h`. That's fine, because it's
only held through a pointer. A backend only needs complete types for what it holds by value.

## `mock_after.h`: the definitions

```cpp
#pragma once
#include <deque>
#include <algorithm>

namespace colib {

/* the backend's own public object: an in-process byte channel */
struct mock_channel_t {
    std::deque<char> bytes;
    bool closed = false;
};

inline bool io_desc_t::same(const io_desc_t &oth) const {
    return data.kind == oth.data.kind && data.chan == oth.data.chan;
}

/* ---------------------------------------------------------------- io_pool_t */

inline io_pool_t::io_pool_t(pool_t *pool, state_list_t &ready_tasks)
: data{ .pool = pool, .ready = &ready_tasks,
        .waiting{ allocator_t<mock_waiter_t>{pool} } } {}

inline io_pool_t::~io_pool_t() { clear(); }

inline bool io_pool_t::is_ok() { return true; }

inline error_e io_pool_t::add_waiter(state_t *state, io_desc_t &io_desc) {
    state->err = ERROR_GENERIC;             /* set for real when it's woken, as in every backend */
    data.waiting.push_back({ &io_desc, state });
    return ERROR_OK;
}

/* wakes one waiter: out of `waiting`, into the ready queue */
inline void mock_wake(io_pool_data_t &data, size_t i, error_e err) {
    state_t *s = data.waiting[i].state;
    data.waiting.erase(data.waiting.begin() + i);
    s->err = err;
    data.ready->push_back(s);
}

inline error_e io_pool_t::handle_ready() {
    if (!data.ready->empty())
        return ERROR_OK;
    /* 1. every read whose channel has bytes (or was closed) is ready */
    bool woke = false;
    for (size_t i = 0; i < data.waiting.size(); ) {
        io_desc_data_t &d = data.waiting[i].desc->data;
        if (d.kind == io_desc_data_t::MOCK_READ && (!d.chan->bytes.empty() || d.chan->closed)) {
            mock_wake(data, i, ERROR_OK);
            woke = true;
        }
        else
            i++;
    }
    if (woke)
        return ERROR_OK;
    /* 2. nothing else can happen: the mock clock jumps to the earliest timer, which fires */
    size_t best = data.waiting.size();
    for (size_t i = 0; i < data.waiting.size(); i++) {
        io_desc_data_t &d = data.waiting[i].desc->data;
        if (d.kind == io_desc_data_t::MOCK_TIMER &&
                (best == data.waiting.size() || d.due_us < data.waiting[best].desc->data.due_us))
            best = i;
    }
    if (best != data.waiting.size()) {
        data.now_us = std::max(data.now_us, data.waiting[best].desc->data.due_us);
        mock_wake(data, best, ERROR_OK);
    }
    return ERROR_OK;        /* nothing queued: the pool has nothing left to do, run() returns */
}

inline error_e io_pool_t::force_awake(io_desc_t &io_desc, error_e retcode) {
    for (size_t i = 0; i < data.waiting.size(); i++) {
        if (data.waiting[i].desc->same(io_desc)) {
            mock_wake(data, i, retcode);
            return ERROR_OK;
        }
    }
    return ERROR_OK;
}

/* readiness only: a woken read hasn't read anything yet */
inline bool io_pool_t::completed(const io_desc_t &) { return false; }

inline error_e io_pool_t::clear() {
    while (!data.waiting.empty()) {
        mock_waiter_t w = data.waiting.back();
        data.waiting.pop_back();
        do_unwait_io_modifs(w.state, *w.desc);     /* it already left, only its wait is closed */
        destroy_state(w.state);
    }
    return ERROR_OK;
}

inline intptr_t io_pool_t::get_internal_handle() { return 0; }

/* ---------------------------------------------------------------- timer_pool_t */

inline timer_pool_t::timer_pool_t(pool_t *, io_pool_t &io_pool) : data{ .io = &io_pool } {}

inline error_e timer_pool_t::get_timer(io_desc_t &new_timer) {
    new_timer.data = io_desc_data_t{ .kind = io_desc_data_t::MOCK_TIMER };
    return ERROR_OK;
}

inline error_e timer_pool_t::set_timer(const io_desc_t &timer,
        const std::chrono::microseconds &time_us)
{
    const_cast<io_desc_t &>(timer).data.due_us = data.io->data.now_us + time_us.count();
    return ERROR_OK;
}

inline error_e timer_pool_t::free_timer(io_desc_t &timer) {
    timer.data = io_desc_data_t{};
    return ERROR_OK;
}

/* ---------------------------------------------------------------- the backend's io functions */

/* waits until the channel has bytes (or is closed), then reads what's there */
inline task<int64_t> mock_read(mock_channel_t &chan, char *buff, size_t len) {
    io_desc_t desc{ .data{ .kind = io_desc_data_t::MOCK_READ, .chan = &chan } };
    error_e err = co_await io_awaiter_t(desc);
    if (err != ERROR_OK)
        co_return err;
    size_t n = std::min(len, chan.bytes.size());
    std::copy_n(chan.bytes.begin(), n, buff);
    chan.bytes.erase(chan.bytes.begin(), chan.bytes.begin() + n);
    co_return (int64_t)n;           /* 0: closed and empty */
}

/* not a coroutine: writing never waits, it only makes the readers ready */
inline void mock_write(mock_channel_t &chan, const char *buff, size_t len) {
    chan.bytes.insert(chan.bytes.end(), buff, buff + len);
}

inline void mock_close(mock_channel_t &chan) { chan.closed = true; }

} /* namespace colib */
```

---

## Using it

With the mock clock, a timeout test is deterministic: it never sleeps for real and never depends on
the machine's load.

```cpp
#include "backend.h"

co::task_t reader(co::mock_channel_t &chan, co::pool_t *pool) {
    char buff[16];
    /* nothing was written yet: the timer (mock clock) fires first */
    auto [n1, err1] = co_await co::create_timeo(co::mock_read(chan, buff, sizeof(buff)),
            pool, std::chrono::microseconds(5000));
    /* err1 == ERROR_TIMEO */

    co::mock_write(chan, "hi", 2);
    auto [n2, err2] = co_await co::create_timeo(co::mock_read(chan, buff, sizeof(buff)),
            pool, std::chrono::microseconds(5000));
    /* err2 == ERROR_OK, n2 == 2 */
    co_return 0;
}

int main() {
    auto pool = co::create_pool();
    co::mock_channel_t chan;
    pool->sched(reader(chan, pool.get()));
    pool->run();
}
```

---

## Notes

- **One backend per program.** Two `.cpp` files of the same program built with different backends
  would give `io_pool_t` two different layouts: an ODR violation, as with mismatched `COLIB_OS_*`
  defines today.
- **Everything in the after file is `inline`**, because `backend.h` is included from several
  `.cpp` files.
- **A missing member function is a link error** that names it (for example
  `unresolved external io_pool_t::completed`).
- **`completed()` is the backend's answer to the killer:** did a woken io already do its work
  before its coroutine resumed? The mock and epoll/kqueue always say no, since they're
  readiness-based. IOCP says yes for a request that moved its data.
- **Open:** whether the backend's public io functions (`mock_read`, and `read`/`accept` for the
  built-in backends) should live in a namespace of their own, and how `COLIB_ENABLE_DEBUG_CHECKS`'
  `dbg_to_str(io_desc)` is provided. Most likely it becomes one more declared member,
  `io_desc_t::to_str()`, defined by the backend.
