# 06.0 Example: a multi-channel chat server and client

A runnable tutorial, not a reference chapter - the actual code lives in `examples/06_0_chat/`
(`chat_common.h`, `chat_server.cpp`, `chat_client.cpp`), verified by actually building and running it
(two real client processes against a real server, on this Windows dev box - see "Running it" below
for the exact commands). This chapter walks through *why* it's built the way it is, not just *what*
it does; read it next to the actual source, not instead of it.

The goal, in the user's own framing: a chat app where you can be in more than one channel at once and
switch between them with `/commands`, the way an IRC or Discord client lets you. What follows is one
`chat_server` process that many `chat_client` processes connect to - a hub relay, not a peer-to-peer
mesh (every client that's connected to the same channel is effectively connected to every other
member of it; nobody needs to also run their own accept loop). That's a deliberate reading of "each
chat app should be able to have a chat connection with all the other chats," made explicitly here so
it's easy to redirect this design if a true peer-to-peer mesh was actually wanted - and it's the
better teaching example regardless: the server's "accept in a loop, dispatch a coroutine per client"
pattern is the clearest possible demonstration of what coroutines actually buy you over one thread
per connection, which a mesh wouldn't add anything to.

A real, captured transcript (two `chat_client`s against one `chat_server`, commands sent a few hundred
milliseconds apart so the timing is visible):

```
alice's terminal:                          bob's terminal:
------------------                          ----------------
Connected. /join <channel> to start...      Connected. /join <channel> to start...
-- welcome, guest1 - /join a channel...     -- welcome, guest2 - /join a channel...
> /nick alice                               > /nick bob
-- nick set to alice                        -- nick set to bob
> /join general                             
-- active channel is now general            > /join general
[general] *: alice joined general           -- active channel is now general
[general] *: bob joined general             [general] *: bob joined general
> hello everyone                            [general] alice: hello everyone
                                             > hi alice
[general] bob: hi alice
> /quit                                     > /quit
```

Everything past this point assumes you've read `docs/03_execution_model.md` (call vs. sched, the
scheduler) and `docs/04_lifetimes.md` (coroutine/pool lifetimes) - this chapter shows those ideas
doing real work, it doesn't re-explain them.

---

## The protocol

Flat, newline-delimited text, no length prefix - deliberately the simplest thing that could work, so
the chapter's attention stays on the coroutine patterns rather than on wire-format design. Every
reader (client or server) accumulates bytes until a `\n` shows up and hands back one line at a time;
`chat_common.h`'s `line_conn_t::read_line()` is that accumulation loop, written once and shared by
both sides - a small lesson of its own (a byte stream doesn't hand you whole lines for free, no
matter which side of the connection you're on).

| Direction | Command | Meaning |
|---|---|---|
| client → server | `NICK <name>` | set/change display name |
| client → server | `JOIN <channel>` | join (server creates the channel on first join) |
| client → server | `LEAVE <channel>` | leave |
| client → server | `MSG <channel> <text...>` | send `text` (rest of the line) to `channel` |
| client → server | `LIST` | ask for known channels + member counts |
| server → client | `MSG <channel> <sender> <text...>` | a relayed message |
| server → client | `INFO <text...>` | a system notice (welcome, joined/left, errors, list results) |

`chat_common.h`'s `split_first_word()` peels one token off the front of a line and hands back "the
rest" verbatim - used for both this wire protocol and the client's local `/commands` below, since
neither ever needs more than "a keyword, then the rest of the line."

## The client's `/commands`

| Command | Effect |
|---|---|
| `/join <channel>` | joins; becomes the active channel if it's the first one joined |
| `/leave <channel>` | leaves; if it was active, the active channel becomes another joined one, or none |
| `/switch <channel>` (alias `/ch`) | purely local - changes which channel plain text targets |
| `/list` | asks the server for known channels |
| `/nick <name>` | renames |
| `/quit` | disconnects and exits |
| *(anything else)* | sent as a message to the active channel, or a local error if there isn't one |

A message for a channel that isn't currently active still gets printed, just marked with a leading
`*` (`socket_reader_task()` in `chat_client.cpp`) - the same "flag activity in a background channel
instead of hiding it" idea IRC/Discord clients use, kept as a one-line marker here rather than a real
multi-pane UI (out of scope for a first tutorial - see the end of this chapter).

---

## The server

### One coroutine per client

```cpp
static co::task_t server_main(uint16_t port) {
    conn_handle_t listen_sock = create_listen_socket(port);
    ...
    while (true) {
        conn_handle_t client = co_await co::accept(listen_sock, ...);
        if (!valid_conn(client))
            break;
        co_await co::sched(handle_client(client));
    }
}
```

`colib.h` doesn't wrap `socket()`/`bind()`/`listen()` itself (see `docs/05_platforms.md`) -
`create_listen_socket()` in `chat_common.h` is plain OS-level setup, identical in spirit to
`tests/005-001-io.cpp`'s own server socket setup. What's actually worth looking at is the loop: every
`co_await co::accept(...)` looks exactly like a blocking `accept()` call, and every one of the
(potentially hundreds of) `handle_client(client)` coroutines it dispatches looks exactly like a
blocking per-connection handler - `while (true) { line = co_await c->conn.read_line(line); ... }` -
but none of them actually block a thread, and there is exactly one OS thread running the whole
server. This is `01_introduction.md`'s "many coroutines, one pool, no threads" claim, now handling
real, concurrent, independent TCP connections instead of a toy example.

`co::sched(handle_client(client))` is fire-and-forget: `server_main()` doesn't wait for a client to
finish before accepting the next one, it just hands the new coroutine to the pool and loops straight
back to `accept()`.

### Why `c` in `handle_client()` doesn't need a lifetime worry

```cpp
static co::task_t handle_client(conn_handle_t handle) {
    auto c = std::make_unique<client_t>(handle, "guest" + std::to_string(g_next_guest++));
    FnScope scope([&] { leave_all(*c); close_conn(c->handle); });
    ...
    while (true) {
        ret = co_await c->conn.read_line(line);
        ...
    }
}
```

`tests/005-001-io.cpp`'s `test8_io_pipe` comment documents a real pitfall: when a *lambda* is also a
coroutine, its frame stores a pointer back to the closure object, not a copy of what it captured - a
fire-and-forget lambda coroutine can easily end up referencing already-destroyed captured state.
`handle_client` sidesteps this entirely by not being a lambda at all: `c` is a genuine local variable
of a real coroutine function, and a coroutine's own locals *do* live in its frame for exactly as long
as the coroutine itself does, correctly surviving every `co_await` inside it. `g_channels` ends up
holding raw `client_t*` pointers into these per-connection frames - safe as long as `leave_all()`
(via the `FnScope` above) always runs before the frame that owns the pointee is destroyed, which it
does: `FnScope`'s destructor fires as the coroutine returns, before its frame is torn down.

### The channel map needs no lock - but it does need a snapshot

```cpp
static std::map<std::string, std::set<client_t*>> g_channels;
```

No lock, for the reason every shared `colib.h` pool structure is safe by default: only one coroutine
is ever actually *running* at a time (`docs/03_execution_model.md`). But "single-threaded" is not the
same guarantee as "nothing else can happen while I'm mid-loop" - `broadcast()` learned this the hard
way while this example was being written:

```cpp
static co::task_t broadcast(const std::string &channel, const std::string &sender,
        const std::string &text, client_t *exclude = nullptr) {
    auto it = g_channels.find(channel);
    if (it == g_channels.end())
        co_return co::ERROR_OK;

    std::vector<client_t*> members(it->second.begin(), it->second.end());   /* snapshot first */

    std::string line = "MSG " + channel + " " + sender + " " + text;
    for (auto *member : members) {
        if (member == exclude)
            continue;
        co_await member->conn.write_line(line);
    }
    co_return co::ERROR_OK;
}
```

If this loop iterated `g_channels[channel]` directly instead of a snapshot, every `write_line()` call
inside it is a genuine suspend point - and while this coroutine is suspended, some *other* client's
coroutine (one joining, leaving, or disconnecting from the very channel being broadcast to) gets to
run and can mutate that same `std::set` out from under the live iteration. That's undefined behavior
(iterator invalidation), and it happens with a single OS thread and zero locks - the danger isn't
concurrency, it's **reentrancy across a suspend point**, a related but distinct hazard from the kind
of invariant a genuinely-concurrent design has to work out (two threads never touching overlapping
coroutine state, a different problem this single-threaded example never has to solve at all).
Snapshotting the member list before the first `co_await` in the loop is
the fix, and it generalizes: any loop that awaits something while walking shared mutable state needs
to ask "could a suspend point in here let something else invalidate what I'm iterating?" - single vs.
multi-threaded doesn't change the question, only how the "something else" gets scheduled.

---

## The client

Two coroutines, scheduled independently onto one pool, sharing only a handful of global variables
(`chat_client.cpp` keeps them flat, file-scope, the same style `tests/005-001-io.cpp` itself uses for
shared test state - a real client would likely wrap this in a class):

```cpp
auto pool = co::create_pool();
pool->sched(COLIB_REGNAME(client_main(host, port)));
co::run_e ret = pool->run();
```

`client_main()` connects, then schedules two more coroutines and returns immediately - it doesn't
wait for them, because the *pool* is what waits: `pool->run()` doesn't return until every scheduled
coroutine (not just the first one) has finished.

```cpp
co_await co::sched(COLIB_REGNAME(socket_reader_task()));
co_await co::sched(COLIB_REGNAME(stdin_reader_task()));
```

`socket_reader_task()` does nothing but `co_await g_conn->read_line(line)` in a loop and print
whatever arrives. `stdin_reader_task()` does nothing but `co_await g_stdin->read_line(line)` in a
loop and parse whatever the user typed. Neither one is aware the other exists, except through the
handful of `g_*` globals and the shutdown handshake below - this *is* `01_introduction.md`'s "many
coroutines, one pool" story, now applied to two genuinely different, genuinely concurrent I/O sources
(a socket and, per the next section, something that only pretends to be one) on the same thread.

### Shutting down two independent loops together

Either loop can decide the session is over first - the server can hang up (`socket_reader_task`
notices) or the user can type `/quit` (`stdin_reader_task` notices) - and whichever one notices needs
to wake the *other*, which is otherwise sitting in a `co_await read_line()` that has no reason to
return on its own:

```cpp
g_quit = true;
if (g_stdin)
    co_await g_stdin->stop_read();     /* from socket_reader_task(), on server hangup */
```
```cpp
g_quit = true;
co_await g_conn->stop_read();          /* from stdin_reader_task(), on /quit */
```

`stop_read()` (`chat_common.h`) is `co::stop_fd()`/`co::stop_handle()` under the hood - the same
force-wake mechanism `tests/005-001-io.cpp`'s `test8_client` uses to unblock a pending `accept()`
during its own teardown, and the same "pre-cancel before close" ordering `docs/04_lifetimes.md`
describes. It's safe to call even when nothing is currently waiting, which is why both sides can call
it unconditionally rather than tracking whether the other loop happens to be blocked at that exact
moment.

---

## The stdin bridge

This is the chapter's other real lesson, and it exists because of a genuine platform constraint, not
a stylistic choice: `colib.h`'s Windows/IOCP backend requires every handle it waits on to have been
opened with `FILE_FLAG_OVERLAPPED` (`io_pool_t::add_waiter` associates the handle with the IOCP and
issues a real overlapped `ReadFile`). `GetStdHandle(STD_INPUT_HANDLE)` does not support overlapped
I/O at all - `colib::read()` can never be pointed at the console directly on Windows. Linux's epoll
backend has no such restriction; a tty fd is just another epoll-able fd once it's set non-blocking.

`chat_common.h`'s `async_stdin_t` is two different implementations behind one interface
(`co_await stdin.read_line(line)`, `co_await stdin.connect()`, `co_await stdin.stop_read()`):

- **Linux:** a thin shim - flip fd 0 to non-blocking, read it directly.
- **Windows:** a plain `std::thread` does ordinary *blocking* `std::getline(std::cin, ...)` reads and
  forwards each completed line into one end of a named pipe created *with*
  `FILE_FLAG_OVERLAPPED`; the pool side then treats the *other* end exactly like any other
  colib.h-managed handle.

This is worth naming as a general technique, not filing away as a stdin-specific workaround:
**anything that's inherently blocking and has no native overlapped-I/O support can be bridged into a
colib.h pool the same way** - a background thread that only ever makes blocking calls, feeding results
through a pipe the pool can `co_await` normally. Legacy blocking libraries and synchronous hardware
APIs are the same shape of problem as "read the keyboard."

Two real bugs in this bridge, both found by actually running the example rather than by inspection -
worth recording exactly because a reading-only pass wouldn't have caught either:

- **A pipe from `CreateNamedPipeA` starts in a "listening" state.** The very first `read_line()` call
  failed with `ERROR_PIPE_LISTENING` before the connection had actually been completed.
  `co::ConnectNamedPipe()` (already exercised by `tests/005-001-io.cpp`'s `test8_io_pipe`) is
  `colib.h`'s async wrapper for the fix - `async_stdin_t::connect()` must be awaited once, right
  after construction, before the first read.
- **The first line off a bridged stdin can carry a UTF-8 byte-order-mark (`EF BB BF`)** that silently
  defeats a leading-character check like this client's own `line[0] == '/'` command detection - found
  via this example's own test harness (.NET's `Process.StandardInput` prepends one), but just as real
  from a BOM'd text file redirected into stdin on either platform (Notepad's default UTF-8 save does
  this on Windows). `chat_common.h`'s `strip_utf8_bom()` strips it from the first line only, since a
  BOM (if present at all) only ever appears once, right at the very start of a stream.

---

## Running it

```
cd examples/06_0_chat
make                          # builds chat_server(.exe) and chat_client(.exe)

./chat_server                 # one terminal
./chat_client                 # one terminal per user, defaults to 127.0.0.1:4242
./chat_client <host> <port>   # to point at a different server
```

Then, in a client:

```
> /nick alice
> /join general
> hello everyone
> /join random
> /switch random
> /quit
```

`chat_server` keeps running until killed (`Ctrl+C`) - there's no `/shutdown` command in this first
tutorial. A client disconnecting uncleanly (killed, not `/quit`) is handled the same as any other
`read_line()` failure: the server's `FnScope` cleans up that client's channel memberships and closes
its socket, and every other client is unaffected - verified by actually killing a connected client
mid-session and confirming the server kept running and the remaining client kept working.

## What's out of scope here

Stated plainly rather than silently absent: authentication, message history / reconnect persistence,
private whispers/DMs, rate limiting, TLS, and a real multi-pane terminal UI (this client just prints a
scrolling log with a `*` marker for background-channel activity, not a curses-style split view).
There's also no server-side "user disconnected" broadcast to a channel's other members - an abrupt
disconnect is handled safely (see above) but silently, which a later chapter could improve using the
same `broadcast()` this one already has. Any of these could become a later `06_N` chapter; none of
them are planned or scoped here (the chapter list past `06_0` is the user's to plan, same as every
other chapter in this directory - see `docs/CLAUDE.md`).
