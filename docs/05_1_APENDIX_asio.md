# 05.1 Appendix: Implementing an ASIO Backend

Not a chapter about what exists - a concrete proposal for what a fourth `COLIB_OS_UNKNOWN` backend
built on Asio (`boost::asio` or standalone Asio - identical API for what this needs) could look like,
written to be compared against your own idea, not implemented unprompted. Everything here targets the
exact contract `colib.h` already asks for - see below - verified against `colib.h` as of commit
`fd519a6`.

---

## The exact contract to fill in

`colib.h` already ships a commented-out reference template for exactly this, inside the
`#if COLIB_OS_UNKNOWN` block - this is the actual spec, not a paraphrase:

```cpp
struct io_pool_t {
    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
    : pool{pool}, ready_tasks{ready_tasks}
    {}

    bool is_ok() {}                                              // ctor succeeded?

    // populates ready_tasks. The ONLY point that blocks - if there are no ready tasks, blocks
    // until there are.
    error_e handle_ready() {
        if (ready_tasks.size() > 0) return ERROR_OK;             // never touch the OS if there's
        // ADD CODE HERE                                          // already work queued
    }

    error_e add_waiter(state_t *state, const io_desc_t& io_desc) {}   // register a wait
    error_e force_awake(const io_desc_t& io_desc, error_e retcode) {} // cancel one wait
    error_e clear() {}                                                // awake all
    intptr_t get_internal_handle() {}                                 // internal handle, if any

private:
    pool_t *pool = nullptr;
    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
};

struct timer_pool_t {
    timer_pool_t(pool_t *pool, io_pool_t &io_pool) : pool(pool), io_pool(io_pool) {}

    error_e get_timer(io_desc_t& new_timer) {}                                       // allocate
    error_e set_timer(const io_desc_t& timer, const std::chrono::microseconds&) {}   // arm
    error_e free_timer(const io_desc_t& timer) {}                                    // release

private:
    pool_t *pool = nullptr;
    io_pool_t &io_pool;
};
```

`COLIB_OS_UNKNOWN_IMPLEMENTATION` (this whole pair) splices in as the reference-template `io_pool_t`/
`timer_pool_t` implementation; `COLIB_OS_UNKNOWN_IO_DESC` (your `io_desc_t`) splices in as the
reference-template `io_desc_t`. Everything below fills in
these exact shapes - same constructor signatures, same method names, same "the only blocking call is
`handle_ready()`" rule.

---

## The framing that makes the rest of this fall out cleanly: Asio is IOCP-shaped, not epoll-shaped

`03_execution_model.md`/`05_platforms.md` draw the line between the two existing backends as
readiness-based (epoll: register a watch, syscall happens later, at resume time) vs. completion-based
(IOCP: issuing the op *is* registering the wait, the result is already in hand at resume time). Asio's
`async_*` functions are completion-based too - `socket.async_read_some(buffer, handler)` issues the
real read immediately and the handler fires with the result once it's done. **This backend should
copy the IOCP backend's shape, not epoll's** - `io_desc_t` as a `shared_ptr` to a stashed-closure
struct, `add_waiter` as what actually *issues* the operation (not just arms a readiness watch), and
the wrapper functions building that closure the same way `colib::read()`'s Windows overload does for
Windows.

---

## `io_desc_t` (`COLIB_OS_UNKNOWN_IO_DESC`)

```cpp
struct asio_op_t {
    error_e     result = ERROR_GENERIC;
    size_t      transferred = 0;
    state_t    *state = nullptr;

    std::function<void()> issue;     // calls the real async_* op; set by the wrapper function
    std::function<void()> do_cancel; // calls .cancel() on the underlying asio object
    bool        completed = false;   // guards against a late handler firing after force_awake
};

struct io_desc_t {
    std::shared_ptr<asio_op_t> data;

    bool is_valid() const { return (bool)data; }
    bool operator==(const io_desc_t& oth) const { return data == oth.data; }
};
```

Same shape as Windows' `io_data_t`/`io_desc_t` pair, for the same reason: the
actual operation needs somewhere to stash a completion handler and a cancel function, and a bare
fd-like value (epoll's, kqueue's) has nowhere to put those.

---

## The ownership model this whole appendix had backwards until now

Every sketch above assumed `io_pool_t` **owns** its `asio::io_context` - constructs it privately,
nobody outside this backend ever touches it. That's the wrong model. The right one: **a pool opened on
this backend subcontracts an `asio::io_context` the caller already owns and supplied** - colib's pool
doesn't run its own engine at all here, it borrows turns pumping someone else's (`handle_ready()`
calling `run_one()` on it), and translates whatever completions land on it into `push_ready()` calls
for its own scheduler. This backend becomes a glue layer between Asio and colib, not a
reimplementation of an engine colib already has two of.

That one change resolves open point 1 below outright, and unlocks something the "private io_context"
version never could: **code that has nothing to do with colib - an existing library written directly
against Asio - can keep running exactly as it already does, on the same `io_context`, at the same
time as colib coroutines are scheduled on a pool subcontracting that engine.** Anything with a
reference to the shared `io_context` can construct its own Asio objects and hand them straight into
this backend's `read<Stream>`/`write<Stream>` templates too (they don't care where a `Stream` came
from), without going through any colib-specific construction path at all.

**Correction to my first pass: this *does* fit entirely inside `COLIB_OS_UNKNOWN_IMPLEMENTATION`, no
upstream `colib.h` change needed - I reached for a new `create_pool()` overload when there's already an
existing, purpose-built hook for exactly this.** `pool_t` already has a public
`std::shared_ptr<void> user_ptr` field with a doc comment that could almost be describing
this use case directly: *"a pointer that you can use however you want. The library won't touch it,
except, of course, when destructing the pool."* `io_pool_t`'s constructor still can't receive the
`io_context` as a constructor argument (it runs *inside* `pool_t()`, before `create_pool()` has
returned anything the caller could set `user_ptr` on yet) - but it doesn't need to. `io_pool_t` keeps
`colib.h`'s exact unmodified `io_pool_t(pool_t*, deque&)` signature, stores nothing about the
`io_context` at construction at all, and every method that needs it fetches it from `pool->user_ptr`
on demand instead:

**One more piece needed before the code below makes sense: `user_ptr` has to hold more than just the
`io_context`.** A naturally-firing completion handler only closes over `asio_op_t` - it has no way
back to *this* `io_pool_t` instance to update anything, because `pool_internal_t::io_pool` (colib.h:
3954) is a private member with no accessor. Rather than needing a new colib.h hook for that too,
`user_ptr` holds a small shared struct instead of a bare `io_context&` - reachable identically from
`io_pool_t` (via `pool->user_ptr`) and from a detached completion handler (via `asio_op_t::pool`,
stashed in `add_waiter`, then `->user_ptr`):

```cpp
struct asio_engine_t {
    asio::io_context &ctx;
    size_t outstanding = 0;   // this pool's own pending-op count - NOT the shared engine's global
                              // work count, which includes ops belonging to unrelated code too
};
// caller, after create_pool(): pool->user_ptr = std::make_shared<asio_engine_t>(my_io_context);

struct io_pool_t {
    io_pool_t(pool_t *pool, std::deque<state_t*, allocator_t<state_t*>> &ready_tasks)
    : pool(pool), ready_tasks(ready_tasks) {}   // matches colib.h's template exactly, unmodified

    asio_engine_t &engine() { return *std::static_pointer_cast<asio_engine_t>(pool->user_ptr); }

    bool is_ok() { return (bool)pool->user_ptr; }   // was the engine ever supplied?

    error_e handle_ready() {
        if (ready_tasks.size() > 0)
            return ERROR_OK;
        auto &eng = engine();
        // Loop, not a single call: one run_one() only guarantees ONE handler ran, from anyone
        // sharing the engine - could easily be someone else's, leaving ready_tasks still empty
        // even though this pool has real outstanding ops of its own. Keep going until either
        // something becomes ready, or this pool's own count says there's truly nothing left to
        // wait for (ctx.run_one() returning 0 - the shared engine has zero work for ANYONE left -
        // is the only other legitimate exit, and implies eng.outstanding was already 0 too).
        while (ready_tasks.empty() && eng.outstanding > 0) {
            if (eng.ctx.run_one() == 0)
                break;
        }
        return ERROR_OK;
    }

    error_e add_waiter(state_t *state, const io_desc_t& io_desc) {
        io_desc.data->state = state;
        io_desc.data->pool = pool;      // the missing reachability link - see "wrapper functions" note
        ++engine().outstanding;
        io_desc.data->issue();          // actually issues the real async_* op
        return ERROR_OK;
    }

    error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
        auto &data = io_desc.data;
        if (data->completed) return ERROR_OK;   // already resolved naturally, nothing to do
        data->completed = true;
        data->result = retcode;
        --engine().outstanding;                  // no longer pending, however this resolves
        if (data->do_cancel) data->do_cancel();  // best-effort: suppresses the real op's own
                                                  // completion handler from firing later and
                                                  // touching a coroutine that's about to be
                                                  // resumed/destroyed by force_awake's caller
        pool->get_internal()->push_ready(data->state); // see "the cancel()-is-async pitfall" below
                                                  // for why this can't just wait for cancel()'s handler
        return ERROR_OK;
    }

    error_e clear() {
        // ctx.stop() would be the WRONG move - it's borrowed, not owned, and stopping it would yank
        // the engine out from under any non-colib code sharing it too. There's also nothing for this
        // function to actively do: pool_internal_t::clear() already walks every one of *this pool's*
        // waiters itself and calls force_awake() on each (see open point 3 below, still true) - by
        // the time this runs, every colib-originated pending op (and this pool's outstanding count)
        // has already been resolved individually. Anything else on the shared engine isn't this
        // pool's to touch.
        return ERROR_OK;
    }

    intptr_t get_internal_handle() { return 0; }  // Asio has no single portable "the handle" the
                                                    // way epoll_fd/IOCP HANDLE are - low value here

private:
    pool_t *pool;
    std::deque<state_t*, allocator_t<state_t*>> &ready_tasks;
};
```

**This closes open point 2 below as a side effect, not just open point 1** - `asio_op_t::pool` (set
once, in `add_waiter`) is exactly the "path back to `pool_internal_t` from inside a completion handler"
that every `push_ready_from(data)` placeholder on this page needed. `push_ready_from(data)` throughout
this appendix should now read as
`data->pool->get_internal()->push_ready(data->state); --std::static_pointer_cast<asio_engine_t>
(data->pool->user_ptr)->outstanding;` - both the push and the same outstanding-count decrement
`force_awake` does, since a naturally-completing op needs the identical bookkeeping update as a
force-awoken one, just triggered from the other direction. `asio_op_t` needs a `pool_t *pool = nullptr;`
field added to what was shown earlier for this to compile.

Wrapper functions reach the engine the same way - `co_await get_pool()` gets the `pool_t*`, then
`std::static_pointer_cast<asio_engine_t>(pool->user_ptr)->ctx`. Worth a real accessor
(`pool_t::get_asio_engine()` or similar) wrapping that cast once, rather than every call site repeating
it against a field whose contents are purely a caller-side convention (`user_ptr` is `void`-typed on
purpose - `colib.h` enforces nothing about what's actually in it) - but functionally, no upstream
signature change is needed for any of this to work.

**The `force_awake`/cancel()-is-async pitfall, worth calling out explicitly:** `asio::*::cancel()`
doesn't synchronously invoke the completion handler - it schedules the handler to run (with
`operation_aborted`) on a *later* `io_context::run_one()`/`poll()` call, same as `CancelIoEx` on
Windows. If `force_awake` only called `cancel()` and waited for that handler to eventually
`push_ready`, a killer's unwind (`04_lifetimes.md`) would need another full trip through
`handle_ready()` before the coroutine is actually awake - and worse, by the time that handler *does*
fire, the coroutine it would resume may already have been destroyed by the killer. That's why
`force_awake` above pushes ready **and** sets `data->completed = true` *itself*, synchronously, and
`cancel()` is just cleanup to stop the real op's own handler from later touching `data` - the
`completed` flag is what makes that handler a safe no-op if it does still fire afterward.

---

## `timer_pool_t`

```cpp
struct timer_pool_t {
    timer_pool_t(pool_t *pool, io_pool_t &io_pool) : pool(pool), io_pool(io_pool) {}

    error_e get_timer(io_desc_t& new_timer) {
        new_timer.data = std::make_shared<asio_op_t>();
        new_timer.data->timer = std::make_unique<asio::steady_timer>(io_pool.engine().ctx);
        return ERROR_OK;
    }

    error_e set_timer(const io_desc_t& timer, const std::chrono::microseconds& time_us) {
        auto data = timer.data;
        data->timer->expires_after(time_us);
        data->issue = [data]() {
            data->timer->async_wait([data](const std::error_code &ec) {
                if (data->completed) return;      // already force_awake'd - see io_pool_t above
                data->completed = true;
                data->result = ec ? ERROR_WAKEUP : ERROR_OK;
                /* push_ready(data->state) via whatever the pool access path ends up being */
            });
        };
        data->do_cancel = [data]() { data->timer->cancel(); };
        return ERROR_OK;   // arming itself happens through the same add_waiter() path as any
    }                       // other wait - set_timer prepares the closures, add_waiter fires them

    error_e free_timer(const io_desc_t& timer) { return ERROR_OK; }  // shared_ptr handles it

private:
    pool_t *pool;
    io_pool_t &io_pool;
};
```

`asio_op_t` needs a `std::unique_ptr<asio::steady_timer> timer` member alongside the fields shown
earlier for this to compile - omitted above for brevity, but it's the one place this backend's
`io_desc_t` payload needs to be a strict superset of "just an I/O op," carrying either an I/O object
or a timer depending on which `get_timer`/wrapper-function path created it.

---

## Wrapper functions (`connect`/`read`/`write`/...)

These are what actually build the `issue`/`do_cancel` closures - `add_waiter` above only *invokes*
`issue`, it never knows what kind of operation it's driving. Shape, mirroring Windows' `colib::read()`
overload - but see the correction right after: `read`/`write` specifically should **not**
be hardcoded to `asio::ip::tcp::socket`.

**`read`/`write` should be generic over any Asio async-I/O-capable object, constrained with a C++20
`requires` clause, not one function per device type.** Checked against Asio's own reference: it
documents `AsyncReadStream`/`AsyncWriteStream` as named requirements (`a.async_read_some(buffers, tok)`,
`a.get_executor()`, etc.) in the traditional generic-programming sense, but **does not** ship them as
real C++20 `concept`s - so colib defining its own is exactly the missing piece, not duplicated effort:

```cpp
template <typename T>
concept AsyncReadStream = requires(T &t, asio::mutable_buffer buf,
        std::function<void(std::error_code, size_t)> handler) {
    { t.async_read_some(buf, handler) };
    { t.cancel() };
};

template <typename T>
concept AsyncWriteStream = requires(T &t, asio::const_buffer buf,
        std::function<void(std::error_code, size_t)> handler) {
    { t.async_write_some(buf, handler) };
    { t.cancel() };
};

template <AsyncReadStream Stream>
inline task<ssize_t> read(Stream &s, void *buf, size_t len) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    desc.data->issue = [&s, buf, len, data = desc.data]() {
        s.async_read_some(asio::buffer(buf, len), [data](std::error_code ec, size_t n) {
            if (data->completed) return;
            data->completed = true;
            // Asio signals EOF as ec == asio::error::eof; POSIX (and colib's own read()) signals
            // it as a 0-byte return with NO error. Collapsing every ec to ERROR_GENERIC would
            // silently turn ordinary EOF into a reported failure - has to be special-cased.
            data->result = (!ec || ec == asio::error::eof) ? ERROR_OK
                          : (ec == asio::error::operation_aborted) ? ERROR_WAKEUP
                          : ERROR_GENERIC;
            data->transferred = n;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&s]() { s.cancel(); };

    error_e err = co_await io_awaiter_t(desc);
    co_return err == ERROR_OK ? (ssize_t)desc.data->transferred : -1;
}
// write<Stream> is the symmetric AsyncWriteStream-constrained twin
```

**Every `ec ? ERROR_GENERIC : ERROR_OK` in every sketch on this page - including the ones already
shown for `io_pool_t`/`timer_pool_t` above, and every verb further down - has this same bug and needs
the same two-part fix: check `asio::error::eof` (only meaningful on read-shaped ops, where it means
"success, 0 bytes" not "failure"), and check `asio::error::operation_aborted` and map it to
`ERROR_WAKEUP`, not `ERROR_GENERIC` - that's specifically the code epoll/IOCP's own `force_awake`
paths use (colib.h's `error_e` doc comment: "the error comes from force awaking the awaiter"), and a
cancelled Asio op's handler firing with `operation_aborted` is exactly that case. Shown once here in
full rather than repeated at every call site below for length.

One `read<Stream>`/`write<Stream>` pair, satisfied equally by `asio::ip::tcp::socket`,
`asio::posix::stream_descriptor`, `asio::windows::stream_handle`, `asio::serial_port`,
`asio::readable_pipe`/`writable_pipe` - every device type from the inventory above whose actual
transfer primitive is `async_read_some`/`async_write_some` - instead of one near-identical overload
per type. `connect`/`accept` stay concrete (`async_connect`/`async_accept` aren't part of this
concept - a resolver or acceptor isn't a stream), but `read_sz`/`write_sz` (loop until full length or
error) and `read_until` from the verb list below both compose on top of `read<Stream>`'s same
`AsyncReadStream`-constrained shape for free, rather than needing their own per-type versions either.

**Where the same trick does and doesn't extend to the rest of the verb table:** `async_wait` is
*tempting* to genericize the same way (it's broadly available too), but its completion signature
actually differs by type - socket/acceptor/posix-descriptor's `async_wait` takes a `wait_type` enum
and completes with just `void(error_code)`; `basic_waitable_timer`'s takes no enum at all;
`basic_signal_set`'s completes with `void(error_code, int signal_number)`. A single
`AsyncWaitable` concept could still cover the socket/acceptor/descriptor family, but timer and
signal_set would need their own. `async_send`/`async_receive`/`async_send_to`/`async_receive_from`
stay socket/datagram-specific by nature - posix descriptors and pipes never had flags or peer
addresses to begin with, so there's no broader concept to reach for there. `async_resolve` and
`async_read_at`/`async_write_at` are each their own single device type, not families - no genericizing
to do.

---

## How many Asio operations would this backend actually need to wrap?

Verified against the Asio 1.30 reference (think-async.com), not recalled from memory - the point of
asking this before starting is that Asio's surface is *much* wider than what `colib.h`'s existing
Linux/Windows backends wrap today, and it's worth seeing the real shape of that gap before committing
to "wrap everything."

**The core async "verbs" - about 9 distinct primitives, reused across every I/O object type:**

| Verb | What it does | Which object types have it |
|---|---|---|
| `async_connect` | outbound connection | stream/datagram sockets, + a free-function overload that resolves *and* connects |
| `async_accept` | inbound connection | acceptor |
| `async_read_some` / `async_write_some` | partial transfer, whatever's ready right now | stream sockets, posix descriptors, Windows stream handles, serial ports, pipes |
| `async_send` / `async_receive` | like read_some/write_some, but with protocol-level flags (`MSG_*`) | stream/datagram sockets |
| `async_send_to` / `async_receive_from` | like send/receive, but with an explicit peer endpoint | datagram (UDP) sockets only |
| `async_wait` | readiness-only, no I/O performed | sockets, acceptor, timers, signal_set, posix descriptors, serial ports - the closest thing to epoll's own model, available almost everywhere |
| `async_resolve` | DNS/service name resolution | resolver only - **colib currently has no equivalent at all**, callers pass already-resolved `sockaddr`s |
| `asio::async_read` / `asio::async_write` (free functions) | loop until the buffer's full or an error - exactly `read_sz`/`write_sz`'s job | any type with `async_read_some`/`async_write_some` |
| `async_read_at` / `async_write_at` / `async_read_until` (free functions) | offset-based transfer; delimiter-based transfer (e.g. line protocols) | random-access files; any stream - **no colib equivalent at all** |

**The "device" types that carry those verbs - about a dozen distinct classes**, each exposing some
subset of the verbs above: `basic_stream_socket` (TCP + Unix-domain stream sockets share this
template), `basic_datagram_socket` (UDP + Unix-domain datagram), `basic_raw_socket`,
`basic_seq_packet_socket`, `basic_socket_acceptor`, `basic_waitable_timer` (steady/system/
high-resolution are all this template with a different clock), `basic_signal_set`, `ip::basic_resolver`,
`posix::basic_stream_descriptor` (wraps an arbitrary Unix fd - pipes, eventfds, anything - for
`async_read_some`/`async_write_some`/`async_wait`), `windows::basic_stream_handle`/
`basic_random_access_handle`/`basic_object_handle` (the Windows-side equivalent, wraps a `HANDLE`),
`basic_serial_port`, and the newer `basic_readable_pipe`/`basic_writable_pipe`/`basic_stream_file`/
`basic_random_access_file` (cross-platform pipe and file I/O, no `HANDLE`/fd juggling needed at the
call site).

**Against what `colib.h` wraps today** (`05_platforms.md`'s breakdown): `connect`, `accept`, `read`,
`write`, `read_sz`, `write_sz`, `stop_fd`/`stop_handle`, plus the Windows-only extras (named pipes,
`DeviceIoControl`, file locking, `WaitCommEvent`, the raw `WSA*` family). That's a name-for-name match
against maybe 5 of Asio's ~9 verbs, on essentially one device type (a generic stream socket/handle) -
**the rest (`async_send_to`/`async_receive_from` for real UDP peer-addressing, `async_resolve`,
`async_read_until`, signal sets, serial ports, the newer pipe/file types) would be net-new surface
this library doesn't expose in any backend today, not a reimplementation of something that already
exists.** Worth deciding up front whether this backend's job is "reach exact parity with today's
`connect`/`accept`/`read`/`write` surface, now portable through one engine" (a bounded, finishable
scope) or "also expose what Asio can do that the current backends can't" (open-ended, and arguably a
separate, later decision from *whether to have an Asio backend at all*).

**That "one device type" framing gets less true once `read`/`write` are written generically** (see
`AsyncReadStream`/`AsyncWriteStream` below, in "Wrapper functions") - a single `requires`-constrained
pair picks up sockets, posix descriptors, Windows handles, serial ports, and pipes at once, for the
cost of writing it once. The verbs that stay genuinely per-device-type are `connect`/`accept`
(different objects entirely - resolver/acceptor, not a stream), `send`/`receive`/`send_to`/
`receive_from` (socket/datagram-only, nothing to genericize against), and `resolve`/`read_at`/
`write_at` (each a single device type, not a family). So the real shape of the "parity" vs.
"everything" decision above is narrower than "9 verbs × 12 types" suggests - closer to "a handful of
generic functions plus half a dozen genuinely type-specific ones."

---

## Every verb from the table above, imagined as a colib wrapper function

All nine, for the "open-ended" scope above - so the actual shape of that extra surface is visible
instead of just named. Every one follows the same skeleton as `read` earlier: build an `io_desc_t`,
fill `issue`/`do_cancel`, `co_await io_awaiter_t(desc)`. Error-checking/using-declarations trimmed for
length; the point is the shape of each, not production-ready code.

**`async_send`/`async_receive` - like `read`/`write`, but flag-aware (`MSG_*`-equivalent):**
```cpp
inline task<ssize_t> send(asio::ip::tcp::socket &sock, const void *buf, size_t len, int flags = 0) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    desc.data->issue = [&sock, buf, len, flags, data = desc.data]() {
        sock.async_send(asio::buffer(buf, len), to_asio_flags(flags),
                [data](std::error_code ec, size_t n) {
            if (data->completed) return;
            data->completed = true;
            data->result = ec ? ERROR_GENERIC : ERROR_OK;
            data->transferred = n;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&sock]() { sock.cancel(); };
    error_e err = co_await io_awaiter_t(desc);
    co_return err == ERROR_OK ? (ssize_t)desc.data->transferred : -1;
}
// receive() is the same shape, async_receive() instead of async_send()
```
Needs a small `to_asio_flags(int)` mapping colib/POSIX `MSG_*` values onto `asio::socket_base::message_flags` - a real but bounded piece of glue, not a design question.

**`async_send_to`/`async_receive_from` - UDP, peer-addressed (colib has no equivalent shape today):**
```cpp
inline task<ssize_t> send_to(asio::ip::udp::socket &sock, const void *buf, size_t len,
        const asio::ip::udp::endpoint &to) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    desc.data->issue = [&sock, buf, len, to, data = desc.data]() {
        sock.async_send_to(asio::buffer(buf, len), to, [data](std::error_code ec, size_t n) {
            if (data->completed) return;
            data->completed = true;
            data->result = ec ? ERROR_GENERIC : ERROR_OK;
            data->transferred = n;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&sock]() { sock.cancel(); };
    error_e err = co_await io_awaiter_t(desc);
    co_return err == ERROR_OK ? (ssize_t)desc.data->transferred : -1;
}

inline task<ssize_t> receive_from(asio::ip::udp::socket &sock, void *buf, size_t len,
        asio::ip::udp::endpoint &from /* out param */) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    desc.data->issue = [&sock, buf, len, &from, data = desc.data]() {
        sock.async_receive_from(asio::buffer(buf, len), from, [data](std::error_code ec, size_t n) {
            if (data->completed) return;
            data->completed = true;
            data->result = ec ? ERROR_GENERIC : ERROR_OK;
            data->transferred = n;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&sock]() { sock.cancel(); };
    error_e err = co_await io_awaiter_t(desc);
    co_return err == ERROR_OK ? (ssize_t)desc.data->transferred : -1;
}
```
Every existing colib I/O function assumes a connected fd/handle with an implicit peer - `send_to`/
`receive_from` are the first ones that need an endpoint threaded through as an explicit extra
parameter (in, for `send_to`) or out-parameter (for `receive_from`). Not hard, just a genuinely
different call shape from `read(fd, buf, len)`.

**`async_wait` - readiness only, no I/O performed (closest thing to colib's existing `wait_event`):**
```cpp
inline task_t wait_readable(asio::ip::tcp::socket &sock) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    desc.data->issue = [&sock, data = desc.data]() {
        sock.async_wait(asio::ip::tcp::socket::wait_read, [data](std::error_code ec) {
            if (data->completed) return;
            data->completed = true;
            data->result = ec ? ERROR_GENERIC : ERROR_OK;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&sock]() { sock.cancel(); };
    co_return co_await io_awaiter_t(desc);
}
```
**Correction to something I understated earlier: this does *not* actually give this backend a working
`wait_event` for free, and that's a real regression worth stating plainly, not glossing over.**
`wait_event(const io_desc_t&)` is exactly two lines - `io_awaiter_t
awaiter(io_desc); co_return co_await awaiter;` - because on epoll/IOCP, an `io_desc_t` *is* the
complete, self-contained description of the wait (a bare `{fd, events}` struct, or a pre-built
`OVERLAPPED`-carrying handle) - the caller builds one directly with no help from any wrapper function,
and `wait_event` just awaits it. This backend's `io_desc_t` is a `shared_ptr<asio_op_t>` whose
usefulness depends entirely on `issue`/`do_cancel` already being populated with real Asio calls bound
to a specific typed device object - there's no way for a caller to hand-build one the way they'd
hand-build `io_desc_t{.fd = fd, .events = EPOLLIN}`. So `wait_event` on this backend either stays
technically present but practically useless (an empty `io_desc_t` has nothing to issue), or the
"arbitrary event, no named wrapper needed" escape hatch this library has on every other backend simply
doesn't exist here - every wait has to go through a purpose-built wrapper function instead. Worth
weighing as a real cost of this backend's shape, not a footnote.

Left concrete to `asio::ip::tcp::socket` here rather than templated, unlike `read`/`write`/`read_until`
above - an `AsyncWaitable` concept covering socket/acceptor/posix-descriptor's shared
`async_wait(wait_type, handler)` shape is plausible, but `basic_waitable_timer`'s `async_wait` takes no
`wait_type` and `basic_signal_set`'s completes with an extra `int signal_number`, so it wouldn't extend
as cleanly or as far as `AsyncReadStream`/`AsyncWriteStream` do - worth a closer look before committing
to it.

**`async_resolve` - DNS/service resolution (no colib equivalent - callers currently pass a resolved
`sockaddr` in, resolution itself was never this library's job before):**
```cpp
inline task<std::vector<asio::ip::tcp::endpoint>> resolve(asio::ip::tcp::resolver &resolver,
        const std::string &host, const std::string &service) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    auto results = std::make_shared<asio::ip::tcp::resolver::results_type>();
    desc.data->issue = [&resolver, host, service, results, data = desc.data]() {
        resolver.async_resolve(host, service,
                [data, results](std::error_code ec, asio::ip::tcp::resolver::results_type r) {
            if (data->completed) return;
            data->completed = true;
            data->result = ec ? ERROR_GENERIC : ERROR_OK;
            *results = r;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&resolver]() { resolver.cancel(); };
    error_e err = co_await io_awaiter_t(desc);
    std::vector<asio::ip::tcp::endpoint> out;
    if (err == ERROR_OK)
        for (auto &e : *results) out.push_back(e.endpoint());
    co_return out;
}
```
The first one whose result isn't a byte count - it returns a small collection instead. `task<T>`
already supports that fine (same as `wait_all`'s `std::tuple<...>`), it's just a different flavor of
result than every byte-oriented function above.

**`async_read_at`/`async_write_at` - explicit-offset transfer, no shared file-position state:**
```cpp
inline task<ssize_t> read_at(asio::random_access_file &file, uint64_t offset, void *buf, size_t len) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    desc.data->issue = [&file, offset, buf, len, data = desc.data]() {
        asio::async_read_at(file, offset, asio::buffer(buf, len),
                [data](std::error_code ec, size_t n) {
            if (data->completed) return;
            data->completed = true;
            data->result = (!ec || ec == asio::error::eof) ? ERROR_OK : ERROR_GENERIC;
            data->transferred = n;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&file]() { file.cancel(); };
    error_e err = co_await io_awaiter_t(desc);
    co_return err == ERROR_OK ? (ssize_t)desc.data->transferred : -1;
}
// write_at() is the same shape, asio::async_write_at() instead
```
Genuinely new capability, not a portability wrapper around something that already exists elsewhere in
the library - colib's current `read`/`write` (both platforms) always operate against an implicit
stream position (the fd's own offset, or a socket's stream position); nothing today exposes an
explicit-offset "read from *here*, regardless of what position the handle is otherwise at" operation
as a named, callable wrapper.

**`async_read_until` - delimiter-based, needs its own buffer (breaks the "caller owns the buffer"
convention every other colib I/O function follows). Reuses `AsyncReadStream` from above, same as
`read`/`write` - `async_read_until` only requires the same `async_read_some`-shaped object, it's not
its own device-type family:**
```cpp
template <AsyncReadStream Stream>
inline task<std::string> read_until(Stream &s, char delim) {
    io_desc_t desc;
    desc.data = std::make_shared<asio_op_t>();
    auto sbuf = std::make_shared<asio::streambuf>();   // <- owned here, not passed in by the caller
    desc.data->issue = [&s, delim, sbuf, data = desc.data]() {
        asio::async_read_until(s, *sbuf, delim, [data](std::error_code ec, size_t) {
            if (data->completed) return;
            data->completed = true;
            data->result = ec ? ERROR_GENERIC : ERROR_OK;
            push_ready_from(data);
        });
    };
    desc.data->do_cancel = [&s]() { s.cancel(); };
    error_e err = co_await io_awaiter_t(desc);
    if (err != ERROR_OK) co_return std::string{};
    std::istream is(sbuf.get());
    std::string line;
    std::getline(is, line, delim);
    co_return line;
}
```
Every other function on this page writes into a caller-supplied `buf`/`len`, matching
`read(fd, buf, len)`'s existing shape exactly. This one can't: `async_read_until` may read *past* the
delimiter looking for it and has to keep the leftover bytes somewhere for the next call, so the
`streambuf` has to be owned across calls, not handed in per-call. This is the one operation in the
whole set that doesn't fit colib's existing I/O function shape at all - worth deciding whether it's
in scope for a first version, or a deliberately-deferred "not yet" the way UDP/resolve/serial/signals
already are for the existing backends.

---

## Open design points - not settled here, worth comparing against your own idea specifically

1. **Resolved, with working code, in `io_pool_t` above.** The diagnosis: `handle_ready()`'s original
   single `run_one()` call implicitly assumed a *private* `io_context` where every handler that could
   ever fire belonged to this pool - true there, false on a *shared* one, where a single `run_one()`
   call is only guaranteed to run **one** handler, from **anyone**. If it happened to service some
   unrelated library's completion instead of this pool's own, `ready_tasks` stays empty,
   `next_task_state()` returns `nullptr`, and `pool_internal_t::run()` returns
   `RUN_OK` - *while this pool still has live, unresolved waiters* - not an inefficiency, a broken
   contract (`03_execution_model.md`: `run()` only returns when there's truly nothing left). Fixed by
   giving `io_pool_t` its own `outstanding` count (via the shared `asio_engine_t` in `user_ptr`, since
   `io_pool_t` needed a bigger `user_ptr` payload anyway - see below) and looping `run_one()` in
   `handle_ready()` until either `ready_tasks` gains an entry or this pool's own count hits zero,
   instead of calling it once.
2. **Resolved as a side effect of fixing point 1, not separately.** Every `push_ready_from(data)`
   placeholder needed a path back to `pool_internal_t` from inside a completion handler that only
   closed over `asio_op_t` - the same `asio_op_t::pool` field the `outstanding` counter's decrement
   needed for the exact same reason. One field, two problems solved together; see the `io_pool_t` code
   and the paragraph right after it above for the resolved shape.
3. **Superseded by the `clear()` fix above (it's now a no-op, not a `stop()` call) - but the underlying
   question is still worth checking, not just assumed away.** `io_pool_t::clear()` now relies entirely
   on `pool_internal_t::clear()` having already walked every one of this pool's waiters and called
   `force_awake()` on each before/around calling this - worth confirming that ordering against
   `04_lifetimes.md`'s documented `clear()` sequence precisely (I/O waiters → semaphore waiters → ready
   queue) rather than assuming "probably fine" the way the original version of this point did.
4. **Threading: one `io_context` per pool, driven *only* from inside `handle_ready()`.** Never call
   `io_context::run()` (as opposed to `run_one()`) or run it on a separate thread - colib's `ready_tasks`
   and this backend's own state are touched without locking, on the assumption that only the pool's
   own thread ever calls in (same constraint `COLIB_ENABLE_MULTITHREAD_SCHED` already documents for
   the rest of the library). Handing the `io_context` to Asio's own thread pool would break that
   invariant silently. **The flip side worth naming, not just the constraint:** Asio's `io_context` is
   explicitly designed to have `run()` called from multiple threads concurrently and dispatch handlers
   safely across them - a capability epoll/IOCP's backends don't get for free the way this one could.
   Whether that's ever worth exploiting depends on whether `COLIB_ENABLE_MULTITHREAD_SCHED`'s existing
   single-point-of-entry (`thread_sched`, still funnels into the one pool thread) is a constraint this
   library actually wants to relax someday - a much bigger question than this appendix, flagged here
   only because this backend is the first place the capability would even be sitting there unused.
5. **Correction, not a settled point: I was wrong that `stop_fd`/`stop_handle` aren't needed - I'd
   conflated "does closing the object cancel the op" with "is the synchronous evict-and-wake mechanism
   still needed," and it is.** Traced the actual chain: `stop_fd(fd)` is just
   `co_await stop_io(io_desc_t{.fd = fd})`; `stop_io` is already fully cross-platform,
   not per-backend, and calls straight into whatever `io_pool_t::force_awake()` the active backend
   provides. This backend's `force_awake` already **is** that mechanism - synchronous `push_ready`,
   not waiting on `cancel()`'s async handler, exactly so a caller can rely on the waiter being woken
   *before* proceeding to tear anything down. Nothing about that need goes away here.
   **What's real is that Asio's `cancel()`-is-async gap makes this *more* load-bearing here, not
   less:** every `issue`/completion-handler lambda in this appendix captures its `Stream&` by
   reference (`[&s, ...]`). If a caller destroys or closes that object while an op is still
   outstanding *without* going through `stop_io` first, the completion handler is left holding a
   dangling reference, and/or the coroutine waiting on it stays parked indefinitely, since nothing
   synchronously woke it the way `force_awake` does - the exact same shaped hazard `stop_fd` exists to
   prevent on epoll (`close(fd)` before `EPOLL_CTL_DEL` corrupts the fd table), just triggered by
   object lifetime instead of fd reuse.
   **What does change is the caller-facing shape, not the mechanism:** there's no bare fd/`HANDLE` to
   build a `stop_fd(int)`/`stop_handle(HANDLE)`-style function from - `io_desc_t` here is a
   `shared_ptr<asio_op_t>`, so cancelling one means having the exact `io_desc_t` that call produced,
   not just "the fd" the way `io_desc_t{.fd = fd}` can be reconstructed from nothing but a bare `int`.

   **That's not just a naming difference - it's a real, unresolved gap, and I overclaimed a clean
   fix a moment ago by glossing over it.** `create_killer`'s cancellation path is fine regardless of
   backend: its `WAIT_IO` modif callback captures a direct pointer to the exact `io_desc_t` in flight
   (`kstate->io_desc = &io_desc;`) at the moment the wait starts, no lookup
   needed. But `stop_fd(fd)`/`stop_handle(h)` being callable with *just* a bare fd/`HANDLE` - from
   code that never held onto anything from the original `read`/`write` call - works on epoll/IOCP only
   because **those backends maintain their own internal map from fd/handle to the currently-waiting
   `state_t`** (epoll's `fd_data_t` map, keyed by fd). None of the sketches on this page build an
   equivalent map from `Stream&`/socket identity to its current `asio_op_t` - without one, there's no
   way to cancel-by-bare-object the way `stop_fd`/`stop_handle` let you cancel-by-bare-fd today.
   Genuinely open: whether this backend needs that same lookup table (real, non-trivial extra state to
   maintain), or whether it's acceptable for this backend to only support cancellation via a killer
   (which doesn't need it) or via a caller who explicitly threads the `io_desc_t` through themselves -
   a real capability gap against the existing backends' public API, not settled by anything above.
6. **`get_internal_handle()` - confirmed as real public API, not just internal plumbing, and this
   backend likely can't support it.** Traced the call chain: `pool_t::get_internal_handle()`
   (forwarding through `pool_internal_t`/`io_pool_t`) is public, and nothing *inside*
   `colib.h` itself calls it - every other backend returns its raw `epoll_fd`/IOCP `HANDLE`, which
   only matters for a user who wants to integrate colib's engine with some *other* event loop
   externally. Asio's `io_context` doesn't portably expose whatever raw OS handle its own internal
   reactor happens to be using (epoll on Linux, IOCP on Windows) - so this backend's
   `get_internal_handle()` would have to honestly return `0`/unsupported, a real capability loss for
   whoever was relying on that escape hatch, not just an unimplemented stub.

---

## Not covered at all - flagging honestly rather than implying this is a complete guide

- **Build/test integration.** `tests/CLAUDE.md`'s conventions have `make unix`/`make unix_kqueue` as
  the pattern a new backend follows - this appendix never sketches the equivalent (`make asio`?), what
  compiler/linker flags an Asio build needs, or how `COLIB_OS_UNKNOWN`/`COLIB_OS_UNKNOWN_IO_DESC`/
  `COLIB_OS_UNKNOWN_IMPLEMENTATION` would actually get `#define`d before `#include "colib.h"` in a real
  test file. Standalone Asio is header-only like `colib.h` itself, which keeps "just take the file"
  mostly true; Boost.Asio is not, and pulls in the rest of Boost's build story - worth deciding which
  one explicitly rather than treating them as interchangeable, which this whole appendix has done so
  far by just saying "Asio."
- **The reactor-double-layering tradeoff - a real cost this appendix hasn't weighed against the
  portability benefit.** On Linux, Asio's own default reactor is itself epoll-based; on Windows, IOCP.
  This backend would concretely be "epoll wrapped by Asio wrapped by colib" on Linux - an extra
  abstraction layer over what colib's *existing* Linux backend already does directly and thinly. The
  case for this backend is portability and less code to maintain across platforms, not raw performance
  over the existing epoll/IOCP backends - worth stating as the actual tradeoff being made, not leaving
  implicit.
- **Debug/tracing support** (`dbg_to_str(io_desc_t)`, interaction with `COLIB_ENABLE_DEBUG_NAMES`/
  `dbg_create_tracer`) - `05_platforms.md` calls out kqueue's `dbg_to_str` as an unimplemented stub;
  this appendix hasn't sketched what a working one looks like for `asio_op_t` either.
- **No worked end-to-end example** - every sketch on this page is a single function in isolation; there
  isn't a full "here's a coroutine using this backend to handle a TCP connection start to finish" the
  way `01_introduction.md`'s worked examples give for the rest of the library.
