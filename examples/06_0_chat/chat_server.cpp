#define COLIB_ENABLE_DEBUG_NAMES true
/* stop_fd()/stop_handle() (used in chat_common.h's line_conn_t::stop_read()) force-cancel a pending
read by design - colib.h's own COLIB_ENABLE_LOGGING (on by default) unconditionally logs that
cancellation as a "FAILED"/timed-out request, even though it's expected, intentional shutdown
behavior, not a real error. Disabled here so this example's terminal output stays about the chat
protocol, not colib.h's internals - see docs/06_0_example_chat.md's shutdown section. */
#define COLIB_ENABLE_LOGGING false

#include "chat_common.h"

#include <map>
#include <set>
#include <memory>
#include <cstdlib>

/* Chat example - server
=================================================================================================
Walked through in docs/06_0_example_chat.md. One process, one colib::pool_t, one accept loop that
dispatches a fresh coroutine per connected client - the "why coroutines" demonstration this chapter
is built around: dozens of clients can each sit in their own co_await read_line() loop, looking for
all the world like blocking per-connection code, without a single OS thread per connection. */

namespace co = colib;
using namespace chat;

static constexpr uint16_t DEFAULT_PORT = 4242;

struct client_t {
    conn_handle_t handle;
    std::string nick;
    std::set<std::string> channels;
    line_conn_t conn;

    client_t(conn_handle_t h, std::string n) : handle(h), nick(std::move(n)), conn(h) {}
};

/* Channel membership - a plain, unsynchronized map/set, safe for the same reason every shared
colib.h pool structure is safe by default: everything here only ever runs on the one thread that
calls pool->run() (see docs/03_execution_model.md) - no lock needed, not because there's no shared
mutable state, but because only one coroutine is ever actually running at a time. */
static std::map<std::string, std::set<client_t*>> g_channels;
static int g_next_guest = 1;

static co::task_t send_info(client_t &c, const std::string &text) {
    co_return co_await c.conn.write_line("INFO " + text);
}

/*! Relays `text` from `sender` to every member of `channel` except `exclude` (the sender itself,
for MSG - server-side echo would otherwise show a client its own message twice). */
static co::task_t broadcast(const std::string &channel, const std::string &sender,
        const std::string &text, client_t *exclude = nullptr) {
    auto it = g_channels.find(channel);
    if (it == g_channels.end())
        co_return co::ERROR_OK;

    /* Snapshot the member list before awaiting anything below - g_channels[channel] is shared
    mutable state, and every write_line() call is a genuine suspend point where another coroutine
    (a client joining/leaving/disconnecting) gets to run and could mutate the very std::set this
    loop would otherwise be iterating live. That's undefined behavior (iterator invalidation) even
    though this whole program is single-threaded - the danger here isn't concurrency, it's
    reentrancy across a suspend point, which is exactly the kind of bug single-threaded cooperative
    scheduling doesn't automatically rule out (only genuinely parallel resumption does; see
    docs/00_1_research_multithreading_pool.md's "call-chain-disjointness" discussion for the
    related, harder version of this same class of bug). */
    std::vector<client_t*> members(it->second.begin(), it->second.end());

    std::string line = "MSG " + channel + " " + sender + " " + text;
    for (auto *member : members) {
        if (member == exclude)
            continue;
        co_await member->conn.write_line(line);
    }
    co_return co::ERROR_OK;
}

static void leave_all(client_t &c) {
    for (auto &ch : c.channels) {
        auto it = g_channels.find(ch);
        if (it == g_channels.end())
            continue;
        it->second.erase(&c);
        if (it->second.empty())
            g_channels.erase(it);
    }
}

static co::task_t handle_line(client_t &c, const std::string &line) {
    std::string cmd, rest;
    split_first_word(line, cmd, rest);

    if (cmd == "NICK") {
        if (rest.empty()) {
            co_await send_info(c, "NICK needs a name");
        } else {
            c.nick = rest;
            co_await send_info(c, "nick set to " + c.nick);
        }
    } else if (cmd == "JOIN") {
        if (rest.empty()) {
            co_await send_info(c, "JOIN needs a channel name");
        } else {
            g_channels[rest].insert(&c);
            c.channels.insert(rest);
            co_await broadcast(rest, "*", c.nick + " joined " + rest);
        }
    } else if (cmd == "LEAVE") {
        if (rest.empty() || !c.channels.count(rest)) {
            co_await send_info(c, "not in that channel");
        } else {
            g_channels[rest].erase(&c);
            c.channels.erase(rest);
            co_await broadcast(rest, "*", c.nick + " left " + rest);
        }
    } else if (cmd == "MSG") {
        std::string channel, text;
        split_first_word(rest, channel, text);
        if (!c.channels.count(channel))
            co_await send_info(c, "not in " + channel + " - /join it first");
        else
            co_await broadcast(channel, c.nick, text, &c);
    } else if (cmd == "LIST") {
        std::string reply = "channels:";
        for (auto &[name, members] : g_channels)
            reply += " " + name + "(" + std::to_string(members.size()) + ")";
        if (g_channels.empty())
            reply += " (none yet)";
        co_await send_info(c, reply);
    } else {
        co_await send_info(c, "unknown command: " + cmd);
    }
    co_return 0;
}

/*! One of these per connected client, dispatched fire-and-forget from server_main()'s accept loop
below. `c` is a genuine local variable of this coroutine, not a captured lambda member - it lives in
this coroutine's own frame for exactly as long as this coroutine does, safely surviving every
co_await inside it (contrast with the lambda-capture pitfall tests/005-001-io.cpp's test8_io_pipe
comment documents; a real coroutine's own locals don't have that problem, only a *lambda*'s captured
members do). */
static co::task_t handle_client(conn_handle_t handle) {
    auto c = std::make_unique<client_t>(handle, "guest" + std::to_string(g_next_guest++));
    FnScope scope([&] {
        leave_all(*c);
        close_conn(c->handle);
    });

    printf("[server] %s connected\n", c->nick.c_str());
    co_await send_info(*c, "welcome, " + c->nick + " - /join a channel to start chatting");

    while (true) {
        std::string line;
        int ret = co_await c->conn.read_line(line);
        if (ret < 0)
            break;
        if (!line.empty())
            co_await handle_line(*c, line);
    }

    printf("[server] %s disconnected\n", c->nick.c_str());
    co_return 0;
}

static co::task_t server_main(uint16_t port) {
    conn_handle_t listen_sock = create_listen_socket(port);
    if (!valid_conn(listen_sock)) {
        printf("[server] failed to listen on port %d (os error %ld)\n", port, last_os_error());
        co_return co::ERROR_GENERIC;
    }
    FnScope scope([listen_sock] { close_conn(listen_sock); });

    printf("[server] listening on port %d\n", port);

    while (true) {
#if COLIB_OS_LINUX || COLIB_OS_UNIX
        conn_handle_t client = co_await co::accept(listen_sock, NULL, NULL);
#elif COLIB_OS_WINDOWS
        struct sockaddr_in client_addr;
        uint32_t addr_len = sizeof(client_addr);
        conn_handle_t client = co_await co::accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
#endif
        if (!valid_conn(client)) {
            printf("[server] accept failed (os error %ld), stopping\n", last_os_error());
            break;
        }
        co_await co::sched(handle_client(client));
    }
    co_return 0;
}

int main(int argc, char **argv) {
#if COLIB_OS_WINDOWS
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return -1;
    }
#endif

    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : DEFAULT_PORT;

    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(server_main(port)));
    co::run_e ret = pool->run();

    return ret == co::RUN_OK ? 0 : -1;
}
