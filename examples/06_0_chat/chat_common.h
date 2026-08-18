#ifndef CHAT_COMMON_H
#define CHAT_COMMON_H

#include "../../colib.h"

#include <string>
#include <vector>
#include <functional>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>

#if COLIB_OS_LINUX || COLIB_OS_UNIX
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#elif COLIB_OS_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "Ws2_32.lib")
#endif

/* Chat example - shared helpers
=================================================================================================
Self-contained on purpose (see examples/CLAUDE.md) - no dependency on tests/tests_common.h, so this
directory can be copied out of the repo on its own. Everything here exists to keep chat_server.cpp/
chat_client.cpp focused on the coroutine patterns docs/06_0_example_chat.md actually walks through,
not on socket/error-handling boilerplate. */

namespace chat {

namespace co = colib;

/*! One connection's OS-level handle type - a plain fd on Linux/Unix, a SOCKET on Windows. Sockets
and read_sz()/write_sz() don't use the same C++ type on both backends (colib.h's Windows I/O
functions take HANDLE, not SOCKET - see as_io_handle() below), so every place in this example that
needs "the connection" spells it as conn_handle_t rather than assuming int. */
#if COLIB_OS_LINUX || COLIB_OS_UNIX
using conn_handle_t = int;
constexpr conn_handle_t INVALID_CONN = -1;
#elif COLIB_OS_WINDOWS
using conn_handle_t = SOCKET;
constexpr conn_handle_t INVALID_CONN = INVALID_SOCKET;
#endif

inline bool valid_conn(conn_handle_t h) {
#if COLIB_OS_LINUX || COLIB_OS_UNIX
    return h >= 0;
#elif COLIB_OS_WINDOWS
    return h != INVALID_SOCKET;
#endif
}

/*! colib.h's Windows read()/write()/read_sz()/write_sz() take a HANDLE, not a SOCKET - the two are
interchangeable in practice (tests/005-001-io.cpp does the same cast), this just names the cast so
call sites read as "the handle I read/write on" instead of a bare reinterpret. On Linux this is the
identity function - fd is already the right type for colib::read()/write(). */
#if COLIB_OS_LINUX || COLIB_OS_UNIX
inline conn_handle_t as_io_handle(conn_handle_t h) { return h; }
#elif COLIB_OS_WINDOWS
inline HANDLE as_io_handle(conn_handle_t h) { return (HANDLE)h; }
#endif

inline void close_conn(conn_handle_t h) {
#if COLIB_OS_LINUX || COLIB_OS_UNIX
    close(h);
#elif COLIB_OS_WINDOWS
    shutdown(h, SD_BOTH);
    closesocket(h);
#endif
}

/*! Last-OS-error accessor, unified across platforms only enough for a one-line diagnostic print -
this example doesn't need FormatMessage-level decoding (see tests_common.h's ASSERT_FN for the
fuller version if that's ever wanted here). */
inline long last_os_error() {
#if COLIB_OS_LINUX || COLIB_OS_UNIX
    return errno;
#elif COLIB_OS_WINDOWS
    return (long)GetLastError();
#endif
}

/*! Minimal RAII cleanup helper, same idiom as tests/tests_common.h's FnScope (duplicated rather
than shared - see examples/CLAUDE.md on why this directory doesn't depend on tests/). */
struct FnScope {
    using fn_t = std::function<void(void)>;
    std::vector<fn_t> fns;

    FnScope(fn_t fn) { fns.push_back(std::move(fn)); }
    FnScope() {}
    ~FnScope() { for (auto &f : fns) f(); }
    void disable() { fns.clear(); }
};

/*! Peels one space-delimited token off the front of `line`: `word` gets everything up to the first
space, `rest` gets everything after it (or "" if there was no space at all). Used to parse both the
wire protocol ("MSG general hello there" -> ("MSG", "general hello there") -> ("general", "hello
there")) and local `/commands` ("join general" -> ("join", "general")) - one small function instead
of a general tokenizer, since every command here needs at most two fields (a keyword, then "the
rest of the line" verbatim). */
inline void split_first_word(const std::string &line, std::string &word, std::string &rest) {
    size_t sp = line.find(' ');
    if (sp == std::string::npos) {
        word = line;
        rest.clear();
        return;
    }
    word = line.substr(0, sp);
    rest = line.substr(sp + 1);
}

/*! Creates, binds, and listens on a TCP socket bound to 0.0.0.0:`port`. Returns INVALID_CONN on
failure (check with valid_conn()). colib.h does not wrap socket()/bind()/listen() itself - see
docs/05_platforms.md - so this is plain OS-level setup, identical in spirit to tests/005-001-io.cpp's
own test8_server()/server() socket setup. */
inline conn_handle_t create_listen_socket(uint16_t port) {
    conn_handle_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (!valid_conn(s))
        return s;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

#if COLIB_OS_LINUX || COLIB_OS_UNIX
    int enable = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close_conn(s);
        return INVALID_CONN;
    }
    if (listen(s, 16) != 0) {
        close_conn(s);
        return INVALID_CONN;
    }
    return s;
}

/*! One TCP connection, buffered for line-oriented text. The wire protocol (see chat_server.cpp/
chat_client.cpp) is flat newline-delimited text with no length prefix, so every reader needs to
accumulate bytes until a '\n' shows up and hand back one line at a time, keeping whatever's left over
for the next call - read_line() is that accumulation loop, kept in one place so neither the server
nor the client repeats it. */
struct line_conn_t {
    conn_handle_t handle;
    std::string buf;

    line_conn_t(conn_handle_t h) : handle(h) {}

    /*! Reads until a full line is available, or the connection ends. Returns colib::ERROR_OK (0)
    with `out` set to the line (no trailing '\n'/'\r') on success, a negative error_e otherwise -
    either the peer closed/reset the connection, or (see stop_read() below) this read was force-
    woken on purpose during shutdown. Either way, the caller's job is just "loop while this returns
    >= 0". */
    co::task<int> read_line(std::string &out) {
        while (true) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) {
                out = buf.substr(0, nl);
                if (!out.empty() && out.back() == '\r')
                    out.pop_back();
                buf.erase(0, nl + 1);
                co_return co::ERROR_OK;
            }

            char chunk[1024];
            auto n = co_await co::read(as_io_handle(handle), chunk, sizeof(chunk));
            if (n <= 0)
                co_return (int)co::ERROR_GENERIC;
            buf.append(chunk, (size_t)n);
        }
    }

    /*! Appends the framing '\n' and writes the whole line out. */
    co::task_t write_line(const std::string &line) {
        std::string framed = line;
        framed.push_back('\n');
        co_return co_await co::write_sz(as_io_handle(handle), framed.data(), framed.size());
    }

    /*! Force-wakes a pending read_line() on this connection with an error, the same colib.h
    stop_fd()/stop_handle() mechanism tests/005-001-io.cpp uses to unblock a pending accept() during
    teardown (see docs/04_lifetimes.md's teardown-ordering note) - here used so one of the client's
    two coroutines can tell the other "we're shutting down" instead of leaving it blocked forever on
    a read that will never naturally complete. Safe to call even if nothing is currently waiting. */
    co::task_t stop_read() {
#if COLIB_OS_LINUX || COLIB_OS_UNIX
        co_return co_await co::stop_fd(handle);
#elif COLIB_OS_WINDOWS
        co_return co_await co::stop_handle(as_io_handle(handle));
#endif
    }
};

/*! Strips a leading UTF-8 byte-order-mark (EF BB BF) from `s`, if present, in place. Whatever
originally supplied stdin's bytes - a redirected file saved by an editor that writes one (common on
Windows; Notepad's default UTF-8 save does this), or in this example's own testing, .NET's
Process.StandardInput StreamWriter, which prepends one to the very first write - can hand the very
first line a BOM before any real text, which silently breaks a leading-character check like this
example's own `line[0] == '/'` command-detection (found by actually running this example against a
harness that does exactly that - a real, not hypothetical, source of the BOM). Only ever needs
checking on the first line of a stream, since a BOM (if present at all) only ever appears once, right
at the start. */
inline void strip_utf8_bom(std::string &s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB
            && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
}

#if COLIB_OS_LINUX || COLIB_OS_UNIX

/*! Async stdin, Linux/Unix: a tty fd is just another epoll-able fd - no bridging needed, only a
flip to non-blocking mode (colib's epoll read path assumes non-blocking fds, the same requirement
any socket already has to meet). See the Windows branch below for why this isn't this simple
everywhere - this struct exists so chat_client.cpp can treat "read a line the user typed" the same
way on both platforms: `co_await stdin_bridge.read_line(line)`. */
struct async_stdin_t {
    line_conn_t conn{0};
    bool first_line = true;

    async_stdin_t() {
        int flags = fcntl(0, F_GETFL, 0);
        fcntl(0, F_SETFL, flags | O_NONBLOCK);
    }

    /* No handshake needed here - fd 0 is already readable the moment the process exists. Exists
    only so chat_client.cpp can call `co_await g_stdin->connect()` uniformly on both platforms
    instead of special-casing Windows' real handshake below. */
    co::task_t connect() { co_return co::ERROR_OK; }

    co::task<int> read_line(std::string &out) {
        int ret = co_await conn.read_line(out);
        if (ret == co::ERROR_OK && first_line) {
            first_line = false;
            strip_utf8_bom(out);
        }
        co_return ret;
    }

    co::task_t stop_read() { co_return co_await conn.stop_read(); }
};

#elif COLIB_OS_WINDOWS

/*! Async stdin, Windows: NOT as simple as flipping a flag. colib.h's IOCP backend requires every
handle it waits on to have been opened with FILE_FLAG_OVERLAPPED (io_pool_t::add_waiter associates
the handle with the IOCP and issues a real overlapped ReadFile) - GetStdHandle(STD_INPUT_HANDLE)
does not support overlapped I/O at all, so colib::read() can never be pointed at the console
directly here, unlike Linux.

The bridge: a plain std::thread does ordinary *blocking* console reads (std::getline) and forwards
each completed line into one end of a Windows named pipe created WITH FILE_FLAG_OVERLAPPED; the pool
side then treats the *other* end exactly like any other colib.h-managed handle - an ordinary
read_line() call, no special-casing anywhere else in this example. This is a general technique, not
a stdin-specific hack: anything that's inherently blocking and has no native overlapped-I/O support
(a legacy blocking library call, a synchronous hardware API, ...) can be bridged into a colib.h pool
the same way - a thread that only ever does blocking calls, feeding results through a pipe the pool
can await normally. */
struct async_stdin_t {
    std::string pipe_name;
    HANDLE pipe_read;
    std::thread feeder;
    /* Reinterpreting a pipe HANDLE as a conn_handle_t (== SOCKET on Windows, both are pointer-sized)
    is intentional, not a type mix-up: line_conn_t only ever calls as_io_handle() to get a HANDLE
    back out for read_sz()/write_sz()/stop_handle(), and never calls anything socket-specific on it -
    so it works unchanged for a named pipe. Nothing here ever calls close_conn() on it (that would be
    wrong for a pipe) - this destructor closes pipe_read directly instead. */
    line_conn_t conn;
    bool first_line = true;

    async_stdin_t()
    : pipe_name("\\\\.\\pipe\\colib_chat_stdin_" + std::to_string(GetCurrentProcessId())),
      pipe_read(CreateNamedPipeA(pipe_name.c_str(),
              PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
              1, 4096, 4096, 0, NULL)),
      conn((conn_handle_t)(intptr_t)pipe_read)
    {
        feeder = std::thread(feed, pipe_name);
    }

    /*! Must be awaited once before the first read_line() call. A pipe created by CreateNamedPipeA
    starts in a "listening" state - colib.h's own ReadFile-based read() fails with
    ERROR_PIPE_LISTENING if issued before something completes the connection (found by actually
    running this example: the very first read attempt failed exactly this way). co::ConnectNamedPipe
    is colib.h's async wrapper for the fix - same pattern tests/005-001-io.cpp's test8_io_pipe uses
    (schedule the client-side connector first, then await ConnectNamedPipe on the server side; per
    MSDN, if the client already connected by the time this runs, it just returns success
    immediately, so starting the feeder thread first in the constructor above is fine, not a race). */
    co::task_t connect() {
        co_return co_await co::ConnectNamedPipe(pipe_read);
    }

    /* strip_utf8_bom() call mirrors the Linux branch above - see that struct's read_line() and
    strip_utf8_bom()'s own doc comment for why (found via this exact bridge: the feeder thread below
    forwards whatever raw bytes std::getline(std::cin, ...) hands it, BOM included if the input
    source had one, since neither std::cin nor a byte-forwarding thread has any reason to know what
    a BOM is). */
    co::task<int> read_line(std::string &out) {
        int ret = co_await conn.read_line(out);
        if (ret == co::ERROR_OK && first_line) {
            first_line = false;
            strip_utf8_bom(out);
        }
        co_return ret;
    }

    co::task_t stop_read() { co_return co_await conn.stop_read(); }

    ~async_stdin_t() {
        /* Cleanly interrupting a blocking std::getline(std::cin) call has no portable, simple
        answer (closing stdin from another thread doesn't reliably unblock it) - acceptable for a
        first tutorial: the process is exiting either way once we get here, so the thread is
        detached rather than joined, and the OS reclaims it at process exit. A later chapter could
        do better (e.g. ReadConsoleInput polled against a cancellation flag). */
        feeder.detach();
        CloseHandle(pipe_read);
    }

private:
    /* Runs on the feeder thread, never touches the pool - just blocking console reads and a
    blocking (non-overlapped) write into the pipe's write end, which is exactly what a plain
    background thread is for. */
    static void feed(std::string pipe_name) {
        HANDLE pipe_write = CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, NULL,
                OPEN_EXISTING, 0, NULL);
        if (pipe_write == INVALID_HANDLE_VALUE)
            return;

        std::string line;
        while (std::getline(std::cin, line)) {
            line.push_back('\n');
            DWORD written = 0;
            if (!WriteFile(pipe_write, line.data(), (DWORD)line.size(), &written, NULL))
                break;
        }
        CloseHandle(pipe_write);
    }
};

#endif

} /* namespace chat */

#endif /* CHAT_COMMON_H */
