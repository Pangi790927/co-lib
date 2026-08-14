# colib.h Architecture Analysis

This document captures design-level patterns not covered by `readme.md`'s component-by-component
API reference. For what each type/function does, see `readme.md`; for allocator, I/O pool, and timer
pool details, see `readme.md`'s "Core Concepts" section (#6-#8) — not repeated here.

---

## Layers

- **Public API** (colib.h lines ~29-2153): types (`task<T>`, `pool_t`, `sem_t`, `state_t`, `modif_t`,
  `io_desc_t`), smart pointers (`pool_p`, `sem_p`, `modif_p`), enums (`error_e`, `run_e`, `modif_e`,
  `modif_flags_e`), free functions (`create_pool`, `sched`, `run`, `clear`, `sleep_*`, ...).
- **Implementation** (lines ~2154-6867): allocator, modif tables, internal pool/sem structs, awaiter
  types, platform-specific epoll/IOCP/kqueue code.

## Key Design Patterns

- **RAII**: `FnScope` for cleanup callbacks, `pool_p`/`sem_p`/`modif_p` shared-pointer lifetime
  management.
- **Coroutine Promise**: `task<T>`'s promise type carries a `state_t`, manages the coroutine handle,
  and captures the return value.
- **Awaiter**: custom awaiters per operation (`yield_awaiter_t`, `sem_awaiter_t`, `sched_awaiter_t<T>`,
  etc.) rather than one generic awaitable.
- **Callback/Modification**: `modif_t` wraps a `std::variant` of callback signatures, one per
  `modif_e` lifecycle event, with `modif_flags_e` controlling inheritance to awaited (`ON_CALL`) or
  scheduled (`ON_SCHED`) coroutines.

## Coroutine Lifecycle

```
Created -> Scheduled -> Running -> {Suspended (co_await) | Yielded | Blocked (I/O, sem)} -> Resumed
        -> {Completed (co_return) | Error (exception) | Destroyed (cleared)}
```

## Error Codes (error_e)
```cpp
enum error_e : int32_t {
    ERROR_YIELDED =  1,  // Not really an error, signals yield
    ERROR_OK      =  0,  // Success
    ERROR_GENERIC = -1,
    ERROR_TIMEO   = -2,
    ERROR_WAKEUP  = -3,  // Force wakeup
    ERROR_USER    = -4,
    ERROR_DEPEND  = -5,
};
```
Propagates from: coroutine `co_return`, modification callback return, I/O syscall failures,
`create_timeo` expiry, `pool->run()`'s `run_e` result.

## Threading Model

- **Single-threaded (default)**: all coroutines run on one thread; `pool->run()` blocks the caller;
  execution order is deterministic.
- **Multi-threaded** (`COLIB_ENABLE_MULTITHREAD_SCHED`): `pool->thread_sched()` may be called from
  other threads to hand work to the pool, but coroutine execution itself stays single-threaded/
  cooperative — no parallel coroutine bodies. Modifications cannot be added via `thread_sched`.

## Object Lifetimes

- `pool_t`: created by `create_pool()`, destroyed when the last `pool_p` is destroyed.
- `sem_t`: created by `create_sem()`, destroyed when the last `sem_p` is destroyed.
- `state_t`: created when a coroutine is scheduled, destroyed when the coroutine completes.
- `modif_t`: created by `create_modif()`, destroyed when the last `modif_p` is destroyed.

---

*Last Updated: 2026-07-26*
