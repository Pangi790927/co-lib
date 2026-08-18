# 00. Contents

The `colib.h` documentation, in reading order. `01`–`05` are the reference chapters - read them in
order the first time through; `05.1` and `06.0` build on them rather than continuing the sequence, so
they're marked apart below.

## Reference

1. **[Introduction](01_introduction.md)** - what a coroutine is, why this library exists, and a tour
   of every major piece (`task`, `pool`, semaphores, the IO pool, the allocator, timers,
   modifications, debugging) at a glance.
2. **[API Reference](02_api.md)** - every public declaration in `colib.h`, with its doc comment,
   organized the way the header itself is organized.
3. **[Execution Model](03_execution_model.md)** - `call` vs. `sched`, what the scheduler actually
   does with a ready coroutine, and how semaphores fit into that flow.
4. **[Lifetimes](04_lifetimes.md)** - when a coroutine frame is actually destroyed, how a killer
   unwinds a call chain, and the lifetimes of the pool, semaphores, and modifications.
5. **[Platforms](05_platforms.md)** - the epoll, IOCP, and (work-in-progress) kqueue backends;
   `io_desc_t`'s three shapes; what's genuinely symmetric across them and what isn't.

## Beyond the reference chapters

- **[05.1 Appendix: an Asio Backend](05_1_APENDIX_asio.md)** - a design proposal, not existing
  behavior: what a fourth, Asio-based backend would look like, filling in `colib.h`'s own
  `COLIB_OS_UNKNOWN` reference template. Not decided, not implemented.
- **[06.0 Example: a Chat Server and Client](06_0_example_chat.md)** - the first of a tutorial
  series (`06_*`) that teaches the library through real, runnable example applications rather than
  prose alone. The code it walks through lives in `examples/06_0_chat/`.

## Not chapters

`CLAUDE.md`, `progress.md`, `TODO.md`, and `understanding.md` in this directory are working/tracking
documents, not user-facing content - see `CLAUDE.md` for what each one is for.
