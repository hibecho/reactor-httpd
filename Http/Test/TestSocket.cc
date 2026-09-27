#include "tcp/Socket.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

static_assert(!std::is_copy_constructible<Socket>::value, "Socket owns its fd");
static_assert(!std::is_copy_assignable<Socket>::value, "Socket owns its fd");
static_assert(std::is_nothrow_move_constructible<Socket>::value, "safe move");
static_assert(std::is_nothrow_move_assignable<Socket>::value, "safe move");

#define CHECK(expr)                                                                                                      \
    do                                                                                                                   \
    {                                                                                                                    \
        if (!(expr))                                                                                                     \
            throw std::runtime_error(                                                                                    \
                std::string(__func__) + ":" + std::to_string(__LINE__) + " " #expr + " errno=" + std::to_string(errno)); \
    } while (false)

static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
static sockaddr_in Address(int fd)
{
    sockaddr_in a{};
    socklen_t n = sizeof(a);
    CHECK(getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n) == 0);
    return a;
}
static void Timeout(int fd)
{
    timeval t{2, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)) == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t)) == 0);
}
struct Pair
{
    Socket listener, client, server;
    Pair()
    {
        CHECK(listener.Create());
        CHECK(listener.Bind("127.0.0.1", 0));
        CHECK(Address(listener.GetFd()).sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(listener.Listen());
        Timeout(listener.GetFd());
        CHECK(client.CreateClient(ntohs(Address(listener.GetFd()).sin_port), "127.0.0.1"));
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        server = Socket(listener.Accept(reinterpret_cast<sockaddr *>(&peer), &size));
        CHECK(server.GetFd() >= 0);
        CHECK(size == sizeof(peer));
        CHECK(peer.sin_family == AF_INET);
        CHECK(peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(peer.sin_port == Address(client.GetFd()).sin_port);
        Timeout(client.GetFd());
        Timeout(server.GetFd());
    }
};

static void Ownership()
{
    Socket s;
    CHECK(s.GetFd() == -1);
    s.Close();
    s.Close();
    CHECK(s.Create());
    int fd = s.GetFd();
    CHECK(!s.Create());
    CHECK(errno == EALREADY);
    CHECK(s.GetFd() == fd);
    CHECK(fcntl(fd, F_GETFD) >= 0);
    Socket moved(std::move(s));
    CHECK(s.GetFd() == -1);
    CHECK(moved.GetFd() == fd);
    Socket target;
    CHECK(target.Create());
    int old = target.GetFd();
    target = std::move(moved);
    CHECK(moved.GetFd() == -1);
    CHECK(target.GetFd() == fd);
    CHECK(fcntl(old, F_GETFD) == -1 && errno == EBADF);
    Socket &alias = target;
    target = std::move(alias);
    CHECK(target.GetFd() == fd);
    target.Close();
    target.Close();
    CHECK(target.GetFd() == -1);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    {
        Socket scoped;
        CHECK(scoped.Create());
        fd = scoped.GetFd();
    }
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

static void OptionsAndServer()
{
    Socket server;
    CHECK(server.CreateServer(0, "127.0.0.1"));
    CHECK(Address(server.GetFd()).sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    CHECK(ntohs(Address(server.GetFd()).sin_port) != 0);
    CHECK(fcntl(server.GetFd(), F_GETFL) & O_NONBLOCK);
    for (int option : {SO_REUSEADDR, SO_REUSEPORT, SO_ACCEPTCONN})
    {
        int enabled = 0;
        socklen_t n = sizeof(enabled);
        CHECK(getsockopt(server.GetFd(), SOL_SOCKET, option, &enabled, &n) == 0);
        CHECK(enabled == 1);
    }
    CHECK(server.Accept() == -1);
    CHECK(WouldBlock());
    Socket client;
    CHECK(client.Create());
    CHECK(client.Connet("127.0.0.1", ntohs(Address(server.GetFd()).sin_port)));
    Socket accepted(server.Accept());
    CHECK(accepted.GetFd() >= 0);
    CHECK(!(fcntl(accepted.GetFd(), F_GETFL) & O_NONBLOCK));
    int before = fcntl(accepted.GetFd(), F_GETFL);
    CHECK(accepted.SetNonBlock());
    CHECK(accepted.SetNonBlock());
    CHECK(fcntl(accepted.GetFd(), F_GETFL) == (before | O_NONBLOCK));
    CHECK(accepted.ReuseAddress());
    Socket default_server;
    CHECK(default_server.CreateServer(0));
    CHECK(Address(default_server.GetFd()).sin_addr.s_addr == htonl(INADDR_ANY));
}

static void DataTransfer()
{
    Pair p;
    const std::array<char, 7> data{{'a', '\0', 'b', '\xff', 'c', '\0', 'd'}};
    std::array<char, 32> out{};
    CHECK(p.client.Send(data.data(), data.size()) == ssize_t(data.size()));
    CHECK(p.server.Recv(out.data(), data.size(), MSG_PEEK | MSG_WAITALL) == ssize_t(data.size()));
    CHECK(std::memcmp(out.data(), data.data(), data.size()) == 0);
    CHECK(p.server.Recv(out.data(), 2, MSG_WAITALL) == 2);
    CHECK(p.server.Recv(out.data() + 2, data.size() - 2, MSG_WAITALL) == ssize_t(data.size() - 2));
    CHECK(std::memcmp(out.data(), data.data(), data.size()) == 0);
    CHECK(p.server.NonBlockSend(data.data(), data.size()) == ssize_t(data.size()));
    CHECK(p.client.Recv(out.data(), data.size(), MSG_WAITALL) == ssize_t(data.size()));
    CHECK(std::memcmp(out.data(), data.data(), data.size()) == 0);
    CHECK(p.client.Send(data.data(), 0) == 0);
    // Zero-length receive is tested with queued data, avoiding an empty blocking socket.
    CHECK(p.client.Send(data.data(), 1) == 1);
    CHECK(p.server.Recv(out.data(), 0) == 0);
    CHECK(p.server.Recv(out.data(), 1) == 1);
    CHECK(p.server.NonBlockRecv(out.data(), out.size()) == -1);
    CHECK(WouldBlock());
    CHECK(!(fcntl(p.server.GetFd(), F_GETFL) & O_NONBLOCK));
    CHECK(shutdown(p.client.GetFd(), SHUT_WR) == 0);
    CHECK(p.server.Recv(out.data(), out.size()) == 0);
    CHECK(p.server.Send(data.data(), 1) == 1);
    CHECK(p.client.Recv(out.data(), 1) == 1);
}

static void Backpressure()
{
    Pair p;
    int small = 4096;
    CHECK(setsockopt(p.client.GetFd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    CHECK(setsockopt(p.server.GetFd(), SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
    std::vector<char> data(1024 * 1024, 'x');
    bool partial = false, blocked = false;
    for (int i = 0; i < 1024; ++i)
    {
        ssize_t n = p.client.NonBlockSend(data.data(), data.size());
        if (n < 0)
        {
            CHECK(WouldBlock());
            blocked = true;
            break;
        }
        CHECK(n > 0);
        partial |= size_t(n) < data.size();
    }
    CHECK(partial);
    CHECK(blocked);
    CHECK(!(fcntl(p.client.GetFd(), F_GETFL) & O_NONBLOCK));
    CHECK(p.client.SetNonBlock());
    // The full send buffer must never block even with the ordinary interface.
    ssize_t n = p.client.Send(data.data(), data.size());
    CHECK(n >= 0 || WouldBlock());
}

static void Failures()
{
    Socket invalid;
    char data = 0;
    CHECK(!invalid.Bind("127.0.0.1", 0));
    CHECK(errno == EBADF);
    CHECK(!invalid.Listen());
    CHECK(errno == EBADF);
    CHECK(!invalid.Connect("127.0.0.1", 1));
    CHECK(errno == EBADF);
    CHECK(invalid.Accept() == -1);
    CHECK(errno == EBADF);
    CHECK(invalid.Recv(&data, 1) == -1);
    CHECK(errno == EBADF);
    CHECK(invalid.Send(&data, 1) == -1);
    CHECK(errno == EBADF);
    CHECK(invalid.NonBlockRecv(&data, 1) == -1);
    CHECK(errno == EBADF);
    CHECK(invalid.NonBlockSend(&data, 1) == -1);
    CHECK(errno == EBADF);
    CHECK(!invalid.SetNonBlock());
    CHECK(errno == EBADF);
    CHECK(!invalid.ReuseAddress());
    CHECK(errno == EBADF);
    Socket s;
    CHECK(s.Create());
    for (const char *ip : {"", "not-an-ip", "256.1.2.3", "::1"})
    {
        CHECK(!s.Bind(ip, 0));
        CHECK(errno == EINVAL);
        CHECK(!s.Connect(ip, 1));
        CHECK(errno == EINVAL);
    }
    Socket bad_server;
    CHECK(!bad_server.CreateServer(0, "bad"));
    CHECK(errno == EINVAL);
    CHECK(bad_server.GetFd() == -1);
    Socket bad_client;
    CHECK(!bad_client.CreateClient(1, "bad"));
    CHECK(errno == EINVAL);
    CHECK(bad_client.GetFd() == -1);
    Socket occupied;
    CHECK(occupied.Create());
    CHECK(occupied.Bind("127.0.0.1", 0));
    uint16_t port = ntohs(Address(occupied.GetFd()).sin_port);
    Socket conflict;
    CHECK(!conflict.CreateServer(port, "127.0.0.1"));
    CHECK(errno == EADDRINUSE);
    CHECK(conflict.GetFd() == -1);
    Socket refused;
    CHECK(!refused.CreateClient(port, "127.0.0.1"));
    CHECK(errno == ECONNREFUSED);
    CHECK(refused.GetFd() == -1);
    int fd = occupied.GetFd();
    CHECK(!occupied.CreateServer(0));
    CHECK(errno == EALREADY);
    CHECK(occupied.GetFd() == fd);
    CHECK(!occupied.CreateClient(port, "127.0.0.1"));
    CHECK(errno == EALREADY);
    CHECK(occupied.GetFd() == fd);
}

static void NoSigpipe()
{
    CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Socket sender(fds[0]);
    CHECK(close(fds[1]) == 0);
    const char byte = 'x';
    CHECK(sender.Send(&byte, 1) == -1);
    CHECK(errno == EPIPE);
    CHECK(sender.Send(&byte, 1, MSG_DONTWAIT) == -1);
    CHECK(errno == EPIPE);
    CHECK(sender.NonBlockSend(&byte, 1) == -1);
    CHECK(errno == EPIPE);
}

static volatile sig_atomic_t signal_count = 0;
static void OnSignal(int) { ++signal_count; }
static void InterruptedCalls()
{
    struct sigaction action{};
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, nullptr) == 0);
    Pair p;
    pid_t parent = getpid();
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0)
    {
        usleep(50000);
        kill(parent, SIGUSR1);
        usleep(50000);
        const char c = 'z';
        _exit(send(p.client.GetFd(), &c, 1, MSG_NOSIGNAL) == 1 ? 0 : 1);
    }
    char c = 0;
    CHECK(p.server.Recv(&c, 1) == 1);
    CHECK(c == 'z');
    CHECK(signal_count > 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    Socket listener;
    CHECK(listener.Create());
    CHECK(listener.Bind("127.0.0.1", 0));
    CHECK(listener.Listen());
    Timeout(listener.GetFd());
    uint16_t port = ntohs(Address(listener.GetFd()).sin_port);
    signal_count = 0;
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
    {
        usleep(50000);
        kill(parent, SIGUSR1);
        usleep(50000);
        Socket client;
        _exit(client.CreateClient(port, "127.0.0.1") ? 0 : 1);
    }
    Socket accepted(listener.Accept());
    CHECK(accepted.GetFd() >= 0);
    CHECK(signal_count > 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main()
{
    const std::pair<const char *, void (*)()> tests[] = {
        {"ownership", Ownership}, {"options_and_server", OptionsAndServer}, {"data_transfer", DataTransfer}, {"backpressure", Backpressure}, {"failures", Failures}, {"no_sigpipe", NoSigpipe}, {"interrupted_calls", InterruptedCalls}};
    int failures = 0;
    for (const auto &test : tests)
    {
        std::cout.flush();
        std::cerr.flush();
        pid_t pid = fork();
        if (pid == -1)
        {
            std::cerr << "fork failed\n";
            return 1;
        }
        if (pid == 0)
        {
            alarm(10);
            try
            {
                test.second();
                std::cout.flush();
                std::cerr.flush();
                _exit(0);
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
                _exit(1);
            }
        }
        int status = 0;
        pid_t result;
        do
        {
            result = waitpid(pid, &status, 0);
        } while (result == -1 && errno == EINTR);
        bool ok = result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::cout << (ok ? "PASS " : "FAIL ") << test.first << '\n';
        if (!ok)
        {
            ++failures;
            if (WIFSIGNALED(status))
                std::cerr << "signal=" << WTERMSIG(status) << '\n';
        }
    }
    return failures ? 1 : 0;
}
