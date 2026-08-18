#define COLIB_ENABLE_DEBUG_NAMES true
/* See chat_server.cpp's identical #define for why - stop_read()'s force-cancel is expected here
too (both the socket and the stdin bridge use it during shutdown). */
#define COLIB_ENABLE_LOGGING false

#include "chat_common.h"

#include <set>
#include <memory>
#include <cstdlib>

/* Chat example - client
=================================================================================================
Walked through in docs/06_0_example_chat.md. Two coroutines share one colib::pool_t for the whole
session: one only ever reads the server socket and prints what arrives, the other only ever reads
what the user types and parses it. Neither is aware of the other except through a couple of global
variables and the stop_read() handshake at the bottom - this is the "many coroutines, one pool, no
threads" story docs/01_introduction.md/docs/03_execution_model.md already promise, shown end to end
against a real socket and (see chat_common.h's async_stdin_t) a real, not-actually-async console. */

namespace co = colib;
using namespace chat;

static constexpr uint16_t DEFAULT_PORT = 4242;

/* Global, file-scope state for the whole client session - same style tests/005-001-io.cpp itself
uses for shared test state (test8_server_fd, test8_pass_cnt, ...). A production client would likely
wrap this in a class; keeping it flat here keeps the coroutine control flow front and center, which
is what this chapter is actually about. */
static std::shared_ptr<line_conn_t> g_conn;      /* the server connection */
static std::unique_ptr<async_stdin_t> g_stdin;   /* the keyboard, bridged - see chat_common.h */
static std::string g_active_channel;             /* "" = none joined yet */
static std::set<std::string> g_joined;
static bool g_quit = false;

static co::task_t print_line(const std::string &s) {
    printf("%s\n", s.c_str());
    fflush(stdout);
    co_return 0;
}

/*! Reads and displays whatever the server sends, for as long as the connection lasts. */
static co::task_t socket_reader_task() {
    while (!g_quit) {
        std::string line;
        int ret = co_await g_conn->read_line(line);
        if (ret < 0) {
            if (!g_quit)
                co_await print_line("[disconnected from server]");
            break;
        }

        std::string cmd, rest;
        split_first_word(line, cmd, rest);
        if (cmd == "MSG") {
            std::string channel, sender_and_text;
            split_first_word(rest, channel, sender_and_text);
            std::string sender, text;
            split_first_word(sender_and_text, sender, text);
            /* A message for a channel that isn't the active one still gets printed - just marked,
            the way an IRC/Discord-style client flags unread activity in a background channel
            instead of hiding it outright. */
            const char *marker = (channel == g_active_channel) ? "" : "*";
            co_await print_line("[" + channel + "]" + marker + " " + sender + ": " + text);
        } else if (cmd == "INFO") {
            co_await print_line("-- " + rest);
        } else if (!line.empty()) {
            co_await print_line("?? " + line);
        }
    }

    /* The socket side is done (server hung up, or we're quitting) - wake the stdin side if it's
    the one still blocked on a read, so the whole process can actually exit instead of waiting for
    one more keypress. See chat_common.h's stop_read() and docs/06_0_example_chat.md's shutdown
    section for why this handshake is needed at all. */
    g_quit = true;
    if (g_stdin)
        co_await g_stdin->stop_read();
    co_return 0;
}

static co::task_t handle_command(const std::string &line) {
    if (line[0] == '/') {
        std::string cmd, rest;
        split_first_word(line.substr(1), cmd, rest);

        if (cmd == "join") {
            if (rest.empty()) {
                co_await print_line("-- /join needs a channel name");
            } else {
                co_await g_conn->write_line("JOIN " + rest);
                g_joined.insert(rest);
                if (g_active_channel.empty()) {
                    g_active_channel = rest;
                    co_await print_line("-- active channel is now " + rest);
                }
            }
        } else if (cmd == "leave") {
            if (!g_joined.count(rest)) {
                co_await print_line("-- not joined to " + rest);
            } else {
                co_await g_conn->write_line("LEAVE " + rest);
                g_joined.erase(rest);
                if (g_active_channel == rest) {
                    g_active_channel = g_joined.empty() ? "" : *g_joined.begin();
                    co_await print_line(g_active_channel.empty()
                            ? "-- no active channel now, /join one"
                            : "-- active channel is now " + g_active_channel);
                }
            }
        } else if (cmd == "switch" || cmd == "ch") {
            if (!g_joined.count(rest))
                co_await print_line("-- not joined to " + rest + " yet, /join it first");
            else {
                g_active_channel = rest;
                co_await print_line("-- active channel is now " + rest);
            }
        } else if (cmd == "list") {
            co_await g_conn->write_line("LIST");
        } else if (cmd == "nick") {
            if (rest.empty())
                co_await print_line("-- /nick needs a name");
            else
                co_await g_conn->write_line("NICK " + rest);
        } else if (cmd == "quit") {
            g_quit = true;
        } else {
            co_await print_line("-- unknown command: /" + cmd
                    + " (try /join, /leave, /switch, /list, /nick, /quit)");
        }
    } else if (g_active_channel.empty()) {
        co_await print_line("-- no active channel, /join one first");
    } else {
        co_await g_conn->write_line("MSG " + g_active_channel + " " + line);
    }
    co_return 0;
}

/*! Reads and parses whatever the user types, for as long as the session lasts. */
static co::task_t stdin_reader_task() {
    co_await print_line("Connected. /join <channel> to start, /switch <channel> to change the "
            "active one, /quit to exit.");

    while (!g_quit) {
        std::string line;
        int ret = co_await g_stdin->read_line(line);
        if (ret < 0)
            break;
        if (!g_quit && !line.empty())
            co_await handle_command(line);
    }

    /* Mirror of socket_reader_task()'s handshake above - if the user typed /quit, the socket side
    is very likely still blocked on read_line(), waiting for a server message that isn't coming. */
    g_quit = true;
    co_await g_conn->stop_read();
    co_return 0;
}

static co::task_t client_main(std::string host, uint16_t port) {
    conn_handle_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!valid_conn(sock)) {
        printf("[client] failed to create socket (os error %ld)\n", last_os_error());
        co_return co::ERROR_GENERIC;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    /* A few retries, same connect-then-back-off pattern tests/005-001-io.cpp's test8_client() uses
    - the server may simply not have called listen() yet if both processes were just started
    together. */
    int attempts = 20;
    bool connected = false;
    while (attempts-- > 0) {
#if COLIB_OS_LINUX || COLIB_OS_UNIX
        int ret = co_await co::connect(sock, (struct sockaddr *)&addr, sizeof(addr));
#elif COLIB_OS_WINDOWS
        int ret = co_await co::connect(sock, (sockaddr*)&addr, sizeof(addr));
#endif
        if (ret >= 0) {
            connected = true;
            break;
        }
        co_await co::sleep_ms(100);
    }
    if (!connected) {
        printf("[client] could not connect to %s:%d\n", host.c_str(), port);
        close_conn(sock);
        co_return co::ERROR_GENERIC;
    }

    g_conn = std::make_shared<line_conn_t>(sock);
    g_stdin = std::make_unique<async_stdin_t>();
    co_await g_stdin->connect();

    co_await co::sched(COLIB_REGNAME(socket_reader_task()));
    co_await co::sched(COLIB_REGNAME(stdin_reader_task()));

    /* client_main() itself is done the moment both are scheduled - it doesn't need to wait for
    them the way this coroutine's own caller (main(), via pool->run()) does, since the pool as a
    whole only finishes once every scheduled coroutine has. */
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

    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : DEFAULT_PORT;

    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(client_main(host, port)));
    co::run_e ret = pool->run();

    if (g_conn)
        close_conn(g_conn->handle);

    return ret == co::RUN_OK ? 0 : -1;
}
