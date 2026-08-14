#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test8 - IO
================================================================================================= */

static void test8_set_msg(uint32_t uints[4]) {
    uints[0] = 2;
    uints[1] = uint32_t(-15);
    uints[2] = 0xbadc0ffe;
    uints[3] = 41;
}

static int test8_chk_msg(uint32_t uints[4]) {
    if (!(uints[0] == 2 && uints[1] == uint32_t(-15) && uints[2] == 0xbadc0ffe && uints[3] == 41)) {
        DBG("WRONG MESSAGE: %u %d 0x%x %u", uints[0], uints[1], uints[2], uints[3]);
        return -1;
    }
    return 0;
}

#if COLIB_OS_LINUX || COLIB_OS_UNIX

int test8_server_fd;
int test8_pass_cnt = 0;
int test8_client_done = 0;
const char *test8_local_ip = "127.0.0.1";
const int test8_port = 3000;

co::task_t test8_server_conn(int fd) {
    uint32_t uints[4] = {0};
    FnScope scope([fd]{ close(fd); });

    ASSERT_COFN(co_await co::read_sz(fd, uints, sizeof(uints)));
    test8_set_msg(uints);

    ASSERT_COFN(co_await co::write_sz(fd, uints, sizeof(uints)));
    co_await co::sleep_ms(10);
    ASSERT_COFN(co_await co::write_sz(fd, uints, sizeof(uints[0]) * 3));
    co_await co::sleep_ms(10);
    ASSERT_COFN(co_await co::write_sz(fd, &uints[3], sizeof(uints[3]) * 1));

    test8_pass_cnt++;
    co_return 0;
}

co::task_t test8_server() {
    ASSERT_COFN(test8_server_fd = socket(AF_INET, SOCK_STREAM, 0));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(test8_port);
    addr.sin_addr.s_addr = inet_addr(test8_local_ip);

    const int enable = 1;
    ASSERT_COFN(setsockopt(test8_server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)));

    ASSERT_COFN(bind(test8_server_fd, (const struct sockaddr *)&addr,
            sizeof(struct sockaddr_in)));
    ASSERT_COFN(listen(test8_server_fd, 2));

    while (true) {
        int fd;
        fd = co_await co::accept(test8_server_fd, NULL, NULL);
        if (fd == co::ERROR_WAKEUP)
            break;
        ASSERT_COFN(fd);

        co_await co::sched(test8_server_conn(fd));
    }

    ASSERT_COFN(shutdown(test8_server_fd, SHUT_RDWR));
    close(test8_server_fd);

    test8_pass_cnt++;
    co_return 0;
}

co::task_t test8_client() {
    int fd;

    co::pool_t *pool = co_await co::get_pool();
    FnScope scope([pool]{
        test8_client_done++;
        if (test8_client_done == 3) {
#if COLIB_OS_LINUX
            pool->stop_io(co::io_desc_t{.fd = test8_server_fd});
#elif COLIB_OS_UNIX
            pool->stop_io(co::io_desc_t{.ident = (uintptr_t)test8_server_fd});
#endif
        }
    });

    ASSERT_COFN(fd = socket(AF_INET, SOCK_STREAM, 0));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(test8_port);
    addr.sin_addr.s_addr = inet_addr(test8_local_ip);

    int retest = 3;
    while (--retest >= 0) {
        int ret = co_await co::connect(fd, (struct sockaddr *)&addr,
                sizeof(struct sockaddr_in));
        if (ret < 0) {
            co_await co::sleep_ms(50);
            DBG("Failed connect...");
        }
        else
            break;
    }
    ASSERT_COFN(retest);

    uint32_t uints1[4] = {0};
    test8_set_msg(uints1);
    ASSERT_COFN(test8_chk_msg(uints1));
    ASSERT_COFN(co_await co::write_sz(fd, uints1, sizeof(uints1)));

    uint32_t uints2[4] = {0};
    ASSERT_COFN(co_await co::read_sz(fd, uints2, sizeof(uints2)));
    ASSERT_COFN(test8_chk_msg(uints2));

    uint32_t uints3[4] = {0};
    ASSERT_COFN(co_await co::read_sz(fd, uints3, sizeof(uints3[0]) * 2));
    co_await co::sleep_ms(10);
    ASSERT_COFN(co_await co::read_sz(fd, &uints3[2], sizeof(uints3[2]) * 2));
    co_await co::sleep_ms(10);
    ASSERT_COFN(test8_chk_msg(uints3));

    test8_pass_cnt++;
    co_return 0;
}

int test8_io() {
    auto pool = co::create_pool();
    pool->sched(test8_server());
    pool->sched(test8_client());
    pool->sched(test8_client());
    pool->sched(test8_client());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test8_pass_cnt == 7));
    return 0;
}

#elif COLIB_OS_WINDOWS

int test8_io_connect_accept_ex_cnt = 0;

co::task_t test8_io_connect_accept_ex() {
    auto client = []() -> co::task_t {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_COFN(CHK_BOOL(sock != INVALID_SOCKET));

        /* ConnectEx requires the socket to be initially bound? */
        struct sockaddr_in addr;
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;

        int rc = bind(sock, (SOCKADDR*) &addr, sizeof(addr));
        ASSERT_COFN(CHK_BOOL(rc == 0));

        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(27015);

        BOOL ok = co_await co::ConnectEx(sock, (SOCKADDR*) &addr, sizeof(addr), NULL, 0, NULL);
        ASSERT_COFN(CHK_BOOL(ok));

        rc = shutdown(sock, SD_BOTH);
        closesocket(sock);

        test8_io_connect_accept_ex_cnt++;

        co_return 0;
    };
    auto client_conn = [](SOCKET sock) -> co::task_t {
        FnScope scope_client_sock([&]{
            shutdown(sock, SD_BOTH);
            closesocket(sock);
        });

        test8_io_connect_accept_ex_cnt++;

        co_return 0;
    };
    auto server = [&client_conn]() -> co::task_t {
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_COFN(CHK_BOOL(server_sock != INVALID_SOCKET));
        FnScope scope_server_sock([&]{ closesocket(server_sock); });

        struct sockaddr_in server_addr;
        ZeroMemory(&server_addr, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        server_addr.sin_port = htons(27015);

        int rc = bind(server_sock, (SOCKADDR*) &server_addr, sizeof(server_addr));
        ASSERT_COFN(CHK_BOOL(rc == 0));

        ASSERT_COFN(CHK_BOOL(listen(server_sock, 100) != SOCKET_ERROR));

        int i = 3;
        while (i --> 0) {
            SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ASSERT_COFN(CHK_BOOL(client_sock != INVALID_SOCKET));
            FnScope scope_client_sock([&]{ closesocket(client_sock); });

            char addr_buff[(sizeof (sockaddr_in) + 16) * 2];
            DWORD recved = 0;

            ASSERT_COFN(CHK_BOOL(co_await co::AcceptEx(server_sock, client_sock, addr_buff, 0,
                    sizeof (sockaddr_in) + 16, sizeof (sockaddr_in) + 16, &recved)));

            scope_client_sock.disable();
            co_await co::sched(client_conn(client_sock));
        }

        test8_io_connect_accept_ex_cnt++;

        co_return 0;
    };
    co_await co::sched(server());
    co_await co::sched(client());
    co_await co::sched(client());
    co_await co::sched(client());
    co_return 0;
}

int test8_io_connect_accept_cnt = 0;

co::task_t test8_io_connect_accept() {
    auto client = []() -> co::task_t {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_COFN(CHK_BOOL(sock != INVALID_SOCKET));
        FnScope scope_sock([&]{
            shutdown(sock, SD_BOTH);
            closesocket(sock);
        });

        struct sockaddr_in addr;
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(27016);

        ASSERT_COFN(co_await co::connect(sock, (SOCKADDR*) &addr, sizeof(addr)));

        uint32_t uints1[4] = {0};
        test8_set_msg(uints1);
        ASSERT_COFN(test8_chk_msg(uints1));
        ASSERT_COFN(co_await co::write_sz((HANDLE)sock, uints1, sizeof(uints1)));

        uint32_t uints2[4] = {0};
        ASSERT_COFN(co_await co::read_sz((HANDLE)sock, uints2, sizeof(uints2)));
        ASSERT_COFN(test8_chk_msg(uints2));

        uint32_t uints3[4] = {0};
        ASSERT_COFN(co_await co::read_sz((HANDLE)sock, uints3, sizeof(uints3[0]) * 2));
        co_await co::sleep_ms(10);
        ASSERT_COFN(co_await co::read_sz((HANDLE)sock, &uints3[2], sizeof(uints3[2]) * 2));
        co_await co::sleep_ms(10);
        ASSERT_COFN(test8_chk_msg(uints3));

        test8_io_connect_accept_cnt++;
        co_return 0;
    };
    auto client_conn = [](SOCKET sock) -> co::task_t {
        FnScope scope_client_sock([&]{
            shutdown(sock, SD_BOTH);
            closesocket(sock);
        });

        uint32_t uints[4] = {0};

        ASSERT_COFN(co_await co::read_sz((HANDLE)sock, uints, sizeof(uints)));
        test8_set_msg(uints);

        ASSERT_COFN(co_await co::write_sz((HANDLE)sock, uints, sizeof(uints)));
        co_await co::sleep_ms(10);
        ASSERT_COFN(co_await co::write_sz((HANDLE)sock, uints, sizeof(uints[0]) * 3));
        co_await co::sleep_ms(10);
        ASSERT_COFN(co_await co::write_sz((HANDLE)sock, &uints[3], sizeof(uints[3]) * 1));

        test8_io_connect_accept_cnt++;
        co_return 0;
    };
    auto server = [&client_conn]() -> co::task_t {
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_COFN(CHK_BOOL(server_sock != INVALID_SOCKET));
        FnScope scope_server_sock([&]{ closesocket(server_sock); });

        struct sockaddr_in server_addr;
        ZeroMemory(&server_addr, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        server_addr.sin_port = htons(27016);

        int rc = bind(server_sock, (SOCKADDR*) &server_addr, sizeof(server_addr));
        ASSERT_COFN(CHK_BOOL(rc == 0));

        ASSERT_COFN(CHK_BOOL(listen(server_sock, 100) != SOCKET_ERROR));

        int i = 3;
        while (i --> 0) {
            struct sockaddr_in client_addr;

            uint32_t addr_len = sizeof(client_addr);
            SOCKET client_sock = co_await co::accept(
                    server_sock, (SOCKADDR *)&client_addr, &addr_len);
            ASSERT_COFN(CHK_BOOL(client_sock != INVALID_SOCKET));

            co_await co::sched(client_conn(client_sock));
        }

        test8_io_connect_accept_cnt++;
        co_return 0;
    };
    co_await co::sched(server());
    co_await co::sched(client());
    co_await co::sched(client());
    co_await co::sched(client());
    co_return 0;
}

co::task_t test8_io_send_recv() {
    /* TODO: WSASend */
    /* TODO: WSASendTo */
    /* TODO: WSASendMsg */
    /* TODO: WSARecv */
    /* TODO: WSARecvFrom */
    /* TODO: WSARecvMsg */
    co_return 0;
}

int test8_io_pipe_cnt = 0;

co::task_t test8_io_pipe() {
    const char *pipe_name = "\\.\\pipe\\TestCoroNamedPipe";
    const int buff_sz = 1024;

    char message[] = "This is the pipe message";
    char message2[] = "This is the returning message";

    HANDLE pipe = CreateNamedPipeA(
            pipe_name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | PIPE_READMODE_MESSAGE,
            PIPE_TYPE_MESSAGE,
            PIPE_UNLIMITED_INSTANCES,
            buff_sz,
            buff_sz,
            NMPWAIT_USE_DEFAULT_WAIT,
            NULL);
    ASSERT_COFN(CHK_PTR(pipe));
    FnScope pipe_scope([pipe]{ CloseHandle(pipe); });

    auto pipe_client = [&pipe_name, &message, &message2]() -> co::task_t {
        HANDLE client_pipe = CreateFileA(
                pipe_name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                NULL);
        ASSERT_COFN(CHK_PTR(client_pipe));
        FnScope pipe_scope([client_pipe]{ CloseHandle(client_pipe); });

        DWORD flags = PIPE_READMODE_MESSAGE;
        ASSERT_COFN(CHK_BOOL(SetNamedPipeHandleState(client_pipe, &flags, NULL, NULL)));

        char buff[1024] = {0};
        DWORD recved = 0;
        ASSERT_COFN(CHK_BOOL(co_await co::TransactNamedPipe(client_pipe,
                message2, sizeof(message2), buff, sizeof(message), &recved)));

        ASSERT_COFN(CHK_BOOL(recved == sizeof(message)));
        ASSERT_COFN(CHK_BOOL(memcmp(buff, message, recved) == 0));

        test8_io_pipe_cnt++;
        co_return 0;
    };

    co_await co::sched(pipe_client());
    
    ASSERT_COFN(CHK_BOOL(co_await co::ConnectNamedPipe(pipe)));

    char buff[1024] = {0};
    DWORD recved = 0;
    ASSERT_COFN(CHK_BOOL(co_await co::ReadFile(pipe, buff, sizeof(message2), &recved, NULL)));

    DWORD sent = 0;
    ASSERT_COFN(CHK_BOOL(co_await co::WriteFile(pipe, message, sizeof(message), &sent, NULL)));

    ASSERT_COFN(CHK_BOOL(sent == sizeof(message)));
    ASSERT_COFN(CHK_BOOL(recved == sizeof(message2)));
    ASSERT_COFN(CHK_BOOL(memcmp(buff, message2, recved) == 0));
    
    test8_io_pipe_cnt++;

    co_return 0;
}

int test8_io_device_cnt = 0;
co::task_t test8_io_device() {
    HANDLE dev = CreateFileA(
            "\\.\\PhysicalDrive0",
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);
    ASSERT_COFN(CHK_PTR(dev));

    DWORD junk;
    DISK_GEOMETRY geometry;
    ASSERT_COFN(CHK_BOOL(co_await co::DeviceIoControl(
            dev,
            IOCTL_DISK_GET_DRIVE_GEOMETRY,
            NULL,
            0,
            &geometry,
            sizeof(geometry),
            &junk)));

    uint64_t disk_sz = 0;

    DBG("Cylinders       = %I64d", geometry.Cylinders);
    DBG("Tracks/cylinder = %ld",   (ULONG) geometry.TracksPerCylinder);
    DBG("Sectors/track   = %ld",   (ULONG) geometry.SectorsPerTrack);
    DBG("Bytes/sector    = %ld",   (ULONG) geometry.BytesPerSector);

    disk_sz = geometry.Cylinders.QuadPart * (ULONG)geometry.TracksPerCylinder *
               (ULONG)geometry.SectorsPerTrack * (ULONG)geometry.BytesPerSector;
    DBG("Disk size       = %I64d (Bytes) = %.2f (Gb)", 
            disk_sz, (double) disk_sz / (1024 * 1024 * 1024));
    co_return 0;
}

int test8_io_lock_file_cnt;

co::task_t test8_io_lock_file() {
    HANDLE file = CreateFileA(
            "./test8_io_lock_file",
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_ALWAYS,
            FILE_FLAG_OVERLAPPED,
            NULL);
    ASSERT_COFN(CHK_PTR(file));
    FnScope file_scope([file]{ CloseHandle(file); });

    char buff[1024] = {0};
    ASSERT_COFN(CHK_BOOL(co_await co::WriteFile(file, buff, sizeof(buff), nullptr, nullptr)));
    ASSERT_COFN(CHK_BOOL(co_await co::ReadFile(file, buff, sizeof(buff), nullptr, nullptr)));

    ASSERT_COFN(CHK_BOOL(co_await co::LockFileEx(
            file,
            LOCKFILE_EXCLUSIVE_LOCK,
            0,
            1024,
            0,
            nullptr)));

    /* TODO: Don't really know how to further check this function, I need a different proc? */

    test8_io_lock_file_cnt++;
    co_return 0;
}

int test8_io_dir_changes_cnt = 0;
co::task_t test8_io_dir_changes() {
    HANDLE dir = CreateFile("./", GENERIC_READ,
            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    ASSERT_COFN(CHK_PTR(dir));
    FnScope dir_scope([dir]{ CloseHandle(dir); });

    auto add_rm_file = []() -> co::task_t {
        const char *filename = "./test8_io_dir_changes";
        HANDLE file = CreateFileA(
                filename,
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_ALWAYS,
                FILE_FLAG_OVERLAPPED,
                NULL);
        ASSERT_COFN(CHK_PTR(file));
        CloseHandle(file);
        ASSERT_COFN(remove(filename));

        test8_io_dir_changes_cnt++;
        co_return 0;
    };

    /* OBS: the CreateFile inside add_rm_file will be called after ReadDirectoryChangesW,
    that is because co::sched does not change the running coroutine */
    co_await co::sched(add_rm_file());

    char buff[1024*10];
    DWORD nread;
    uint32_t action = 0;
    while (true) {
        ASSERT_COFN(CHK_BOOL(co_await co::ReadDirectoryChangesW(dir, buff, sizeof(buff), FALSE,
                FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME, &nread, nullptr)));
        ASSERT_COFN(CHK_BOOL(nread != 0));

        DWORD off = 0;
        do {
            auto fi = (FILE_NOTIFY_INFORMATION *)(buff + off);
            switch (fi->Action) {
                case FILE_ACTION_ADDED:   action |= 1; break;
                case FILE_ACTION_REMOVED: action |= 2; break;
                default: DBG("Unknown action");
            }
            off = fi->NextEntryOffset;
        } while (off);

        if (action == 3)
            break;
    }

    test8_io_dir_changes_cnt++;
    co_return 0;
}

co::task_t test8_io_comm_event() {
    /* TODO: no idea how to test this one (WaitCommEvent), do I need some some usb dev? */
    co_return 0;
}

int test8_io() {
    auto pool = co::create_pool();
    pool->sched(test8_io_connect_accept_ex());
    pool->sched(test8_io_connect_accept());
    pool->sched(test8_io_comm_event());
    pool->sched(test8_io_dir_changes());
    pool->sched(test8_io_lock_file());
    pool->sched(test8_io_device());
    pool->sched(test8_io_pipe());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test8_io_connect_accept_cnt == 7));
    ASSERT_FN(CHK_BOOL(test8_io_connect_accept_ex_cnt == 7));
    ASSERT_FN(CHK_BOOL(test8_io_pipe_cnt == 2));
    ASSERT_FN(CHK_BOOL(test8_io_lock_file_cnt == 1));
    ASSERT_FN(CHK_BOOL(test8_io_dir_changes_cnt == 2));
    return 0;
}

#else

int test8_io() {
    /* IO tests are platform-specific, skip on unsupported platforms */
    DBG("IO tests skipped on this platform");
    return 0;
}

#endif

int main() {
    int ret = test8_io();
    print_test_result("005-001-io.cpp", ret >= 0);
    return ret;
}
