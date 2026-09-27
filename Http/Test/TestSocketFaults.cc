#include "tcp/Socket.hpp"
#include "base/Logger.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <vector>

namespace
{
    constexpr int fake_fd = 123456;
    struct Result
    {
        ssize_t value;
        int error;
    };
    std::deque<Result> results;
    std::string failing;
    int fail_error = EIO;
    int calls = 0, closes = 0, set_calls = 0, set_flags = 0, last_flags = 0;
    int checks = 0;
    void check(bool condition, const char *message)
    {
        ++checks;
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }
    void reset(const std::string &operation = "", int error = EIO)
    {
        results.clear();
        failing = operation;
        fail_error = error;
        calls = closes = set_calls = set_flags = last_flags = 0;
    }
    int status(const char *operation)
    {
        if (failing == operation)
        {
            errno = fail_error;
            return -1;
        }
        return 0;
    }
    ssize_t next()
    {
        ++calls;
        check(!results.empty(), "unexpected syscall retry");
        const auto result = results.front();
        results.pop_front();
        errno = result.error;
        return result.value;
    }
}

extern "C"
{
    int __real_close(int);
    int __wrap_socket(int, int, int) { return status("socket") < 0 ? -1 : fake_fd; }
    int __wrap_bind(int, const sockaddr *, socklen_t) { return status("bind"); }
    int __wrap_connect(int, const sockaddr *, socklen_t) { return status("connect"); }
    int __wrap_listen(int, int) { return status("listen"); }
    int __wrap_accept(int, sockaddr *, socklen_t *) { return static_cast<int>(next()); }
    ssize_t __wrap_recv(int, void *, size_t, int flags)
    {
        last_flags = flags;
        return next();
    }
    ssize_t __wrap_send(int, const void *, size_t, int flags)
    {
        last_flags = flags;
        return next();
    }
    int __wrap_setsockopt(int, int, int option, const void *, socklen_t)
    {
        return status(option == SO_REUSEADDR ? "reuseaddr" : "reuseport");
    }
    int __wrap_fcntl(int, int command, ...)
    {
        if (command == F_GETFL)
            return status("getfl") < 0 ? -1 : O_APPEND;
        check(command == F_SETFL, "unexpected fcntl operation");
        va_list args;
        va_start(args, command);
        set_flags = va_arg(args, int);
        va_end(args);
        ++set_calls;
        return status("setfl");
    }
    int __wrap_close(int fd)
    {
        if (fd != fake_fd)
            return __real_close(fd);
        ++closes;
        if (failing == "close")
        {
            errno = fail_error;
            return -1;
        }
        // Deliberately overwrite errno: failed multi-step setup must restore its cause.
        errno = EBADF;
        return 0;
    }
}

int main()
{
    Logger::Instance().Shutdown();
    {
        Socket socket(fake_fd);
        for (int operation = 0; operation < 3; ++operation)
        {
            char data[16]{};
            auto invoke = [&]() -> ssize_t
            {
                if (operation == 0)
                    return socket.Accept();
                if (operation == 1)
                    return socket.Recv(data, sizeof(data));
                return socket.Send(data, sizeof(data));
            };
            reset();
            results = {{-1, EINTR}, {-1, EINTR}, {3, 0}};
            check(invoke() == 3 && calls == 3, "EINTR retries to success");
            reset();
            results = {{0, 0}};
            check(invoke() == 0 && calls == 1, "zero is a successful result");
            reset();
            results = {{2, 0}};
            check(invoke() == 2 && calls == 1, "short result is not retried");
            for (int error : {EAGAIN, EWOULDBLOCK, ECONNRESET, EBADF})
            {
                reset();
                results = {{-1, error}};
                check(invoke() == -1 && errno == error && calls == 1,
                      "failure preserves errno without retry");
            }
        }
        char data[16]{};
        reset();
        results = {{1, 0}};
        check(socket.NonBlockRecv(data, sizeof(data)) == 1 &&
                  (last_flags & MSG_DONTWAIT),
              "nonblocking receive flag");
        reset();
        results = {{1, 0}};
        const char *payload = "test";
        check(socket.NonBlockSend(payload, 4) == 1 &&
                  (last_flags & MSG_DONTWAIT) && (last_flags & MSG_NOSIGNAL),
              "nonblocking send suppresses SIGPIPE");
        reset();
        results = {{1, 0}};
        check(socket.Send(payload, 4, MSG_MORE) == 1 &&
                  (last_flags & MSG_MORE) && (last_flags & MSG_NOSIGNAL),
              "caller send flags retained with SIGPIPE suppression");
        reset("getfl", EPERM);
        check(!socket.SetNonBlock() && errno == EPERM && set_calls == 0,
              "failed F_GETFL does not attempt F_SETFL");
        reset("setfl", EPERM);
        check(!socket.SetNonBlock() && errno == EPERM && set_calls == 1,
              "failed F_SETFL is reported");
        reset();
        check(socket.SetNonBlock() && set_calls == 1 &&
                  set_flags == (O_APPEND | O_NONBLOCK),
              "existing file flags preserved");
        reset();
        check(!socket.Create() && errno == EALREADY && socket.GetFd() == fake_fd,
              "duplicate Create retains owned descriptor");
    }
    for (const auto &operation : {"socket", "getfl", "setfl", "reuseaddr",
                                  "reuseport", "bind", "listen"})
    {
        reset(operation, EACCES);
        {
            Socket server;
            check(!server.CreateServer(8080, "127.0.0.1") && errno == EACCES,
                  "server setup reports original failure");
            check(server.GetFd() == -1, "failed server setup leaves no descriptor");
            check(closes == (failing == "socket" ? 0 : 1), "server failure closes exactly once");
        }
        check(closes == (failing == "socket" ? 0 : 1), "server destructor does not close twice");
    }
    for (const auto &operation : {"socket", "connect"})
    {
        reset(operation, ECONNREFUSED);
        Socket client;
        check(!client.CreateClient(8080, "127.0.0.1") && errno == ECONNREFUSED,
              "client setup preserves original failure");
        check(client.GetFd() == -1 && closes == (failing == "socket" ? 0 : 1),
              "failed client setup cleans ownership");
    }
    reset();
    {
        Socket server;
        check(server.CreateServer(8080, "127.0.0.1"), "server successful setup");
    }
    check(closes == 1, "successful server closes on destruction");
    reset();
    {
        Socket client;
        check(client.CreateClient(8080, "127.0.0.1"), "client successful setup");
    }
    check(closes == 1, "successful client closes on destruction");
    for (bool server : {false, true})
    {
        reset();
        Socket socket;
        const bool success = server ? socket.CreateServer(8080, "invalid-ip")
                                    : socket.CreateClient(8080, "invalid-ip");
        check(!success && errno == EINVAL && socket.GetFd() == -1 && closes == 1,
              "invalid address cleans up newly created descriptor");
    }
    for (bool server : {false, true})
    {
        reset();
        Socket socket(fake_fd);
        const bool success = server ? socket.CreateServer(8080)
                                    : socket.CreateClient(8080, "127.0.0.1");
        check(!success && errno == EALREADY && socket.GetFd() == fake_fd && closes == 0,
              "duplicate composite creation retains original descriptor");
    }
    for (int error : {EINTR, EIO, EBADF})
    {
        reset("close", error);
        {
            Socket socket(fake_fd);
            errno = EACCES;
            socket.Close();
            check(socket.GetFd() == -1 && closes == 1 && errno == EACCES,
                  "close failure releases ownership without retry and preserves errno");
            socket.Close();
            check(closes == 1, "repeated Close does not retry failed close");
        }
        check(closes == 1, "destructor does not retry failed close");
    }
    std::cout << "Socket fault injection: " << checks << " checks passed\n";
}
