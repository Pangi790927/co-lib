# Open: `close_wait()` turns ownership backwards

Status: **open, waiting on a decision**, deferred by the user on 2026-09-24 as "a large one, will
come afterwards". Nothing is changed for it yet.

## What is backwards

```cpp
/* ends the wait `s` is in without resuming it: out of its queue, and its UNWAIT callbacks */
inline void close_wait(state_t *s) {
    state_access_t::unlink(s);
    if (io_desc_t *io = state_access_t::wait_io(s))
        do_unwait_io_modifs(s, *io);
    else if (sem_t *sem = state_access_t::wait_sem(s))
        do_unwait_sem_modifs(s, sem);
    else if (state_access_t::wait_yield(s))
        do_unyield_modifs(s);
}
```

It unlinks the coroutine first and then works out what it waited on from the coroutine's own copy
(`state_t::wait`/`wait_on`). The user's objection: "how are you going to get the io if the owner no
longer believes you have the io? not ok, it turns ownership backwards". In an ownership model the
owner (the semaphore, the io pool, the ready queue) knows what the coroutine waits on and lets go of
it itself.

## Why it is like that

Where a coroutine is linked, and who knows what it waits on:

| Situation                   | Linked in                                 | Who knows the io or semaphore        |
|-----------------------------|-------------------------------------------|--------------------------------------|
| waiting on a semaphore      | that semaphore's wait list                | the semaphore (the list's owner)     |
| waiting on an io            | not a list: epoll keeps it in             | the io pool, only by searching; the  |
|                             | `fd_data_t`, IOCP in `io_data_t`          | awaiter in the frame has the         |
|                             |                                           | `io_desc_t`                          |
| woken, not yet resumed      | the pool's ready queue                    | only the awaiter in its frame        |

Between the wake and the resume nobody who owns the coroutine knows what it waited on: the wake
moves it to the ready queue, and its UNWAIT only runs when it resumes (in its awaiter). That gap is
why `wait_on` exists (8 bytes in `state_t`).

## The direction proposed: whoever wakes it closes its wait

- `sem_internal_t::signal()`, the io pool delivering a completion and `force_awake()`/`stop_io()`
  run the UNWAIT while they still own the coroutine, then hand it to the ready queue.
- A woken coroutine then has no open wait; its awaiter's resume only fires ENTER.
- `close_wait()` only exists for a coroutine still waiting: its owner runs UNWAIT and unlinks it.
- "Did the wake have an effect?" (`woken_with_effect()`) becomes one bit recorded at the wake.

## What blocks it (the decisions needed)

1. **UNWAIT would run in the waker's context** (inside `signal()` called by another coroutine, or
   inside the io pool's event loop). A user UNWAIT callback that signals or kills re-enters loops
   not written for it; each wake loop would first take its waiters out, as `sem_internal_t::clear()`
   does now.
2. **An io wait has no owner pointer** unless io waits use lists, which was agreed to be left alone
   ("let's try to not touch that one if it is not needed"). Without it, a coroutine waiting on an io
   needs its `io_desc_t` from somewhere: the awaiter (the copy again) or a WAIT_IO callback. **This is
   the decision the user has to make first.**
3. **Traces change:** UNWAIT moves from the resume to the wake (still before ENTER).

Related ideas from the user's review (`review-colib.md`): a generic owner-aware `list_head_t`, with
`state_e` derived from the list a coroutine is in, and `wait_on` removed from `state_t`.
