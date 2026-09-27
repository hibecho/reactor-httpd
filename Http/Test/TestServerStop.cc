// 公共接口集成测试：停机排空、截止时间、资源限制与 HTTP 背压恢复。
#include "tcp/TcpServer.hpp"
#include "tcp/ResourceLimits.hpp"
#include "protocol/HttpServer.hpp"
#include "base/Buffer.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <future>
#include <dirent.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static std::atomic<int> fail_eventfd_after{-1};
extern "C" int __real_eventfd(unsigned int, int);
extern "C" int __wrap_eventfd(unsigned int initial, int flags)
{
    const int remaining = fail_eventfd_after.load();
    if (remaining >= 0 && fail_eventfd_after.fetch_sub(1) == 0)
    {
        errno = EMFILE;
        return -1;
    }
    return __real_eventfd(initial, flags);
}

namespace
{
using namespace std::chrono_literals;
void Alarm(int)
{
    constexpr char message[] = "TestServerStop timed out\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(2);
}

template <class T> T Await(std::future<T> &future)
{
    assert(future.wait_for(8s) == std::future_status::ready);
    return future.get();
}

int Connect(uint16_t port, int receive_buffer = 0)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    timeval timeout{8, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    if (receive_buffer)
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    return fd;
}

void Send(int fd, const std::string &data)
{
    std::size_t offset = 0;
    while (offset < data.size())
    {
        ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        offset += static_cast<std::size_t>(n);
    }
}

std::string ReadBytes(int fd, std::size_t count)
{
    std::string data(count, '\0');
    std::size_t offset = 0;
    while (offset < count)
    {
        ssize_t n = recv(fd, data.data() + offset, count - offset, 0);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        offset += static_cast<std::size_t>(n);
    }
    return data;
}

void Eof(int fd, bool reset_allowed = false)
{
    char byte;
    ssize_t n;
    do { n = recv(fd, &byte, 1, 0); } while (n < 0 && errno == EINTR);
    assert(n == 0 || (reset_allowed && n < 0 && errno == ECONNRESET));
}

std::string ResponseBody(int fd)
{
    std::string header;
    while (header.size() < 4 || header.compare(header.size() - 4, 4, "\r\n\r\n") != 0)
    {
        header += ReadBytes(fd, 1);
        assert(header.size() < 65536);
    }
    assert(header.rfind("HTTP/1.1 200 ", 0) == 0);
    std::string lower = header;
    for (char &c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    auto position = lower.find("\r\ncontent-length:");
    assert(position != std::string::npos);
    return ReadBytes(fd, std::stoull(lower.substr(position + 17)));
}

void StopBeforeStart()
{
    TcpServer server(0);
    server.Stop();
    server.Stop();
    bool rejected = false;
    try { server.Start(); }
    catch (const std::logic_error &) { rejected = true; }
    assert(rejected);
}

void DrainAccepted(int workers)
{
    TcpServer server(0);
    server.SetThreadCount(workers);
    const std::string payload(512 * 1024, 'd');
    std::promise<bool> accepted;
    auto ready = accepted.get_future();
    std::atomic<int> closed{0};
    server.SetConnectedCallback([&](const ConnectionPtr &conn) {
        accepted.set_value(conn->Send(payload));
    });
    server.SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    const auto port = server.GetPort();
    std::thread client([&] {
        int fd = Connect(port);
        assert(Await(ready));
        server.Stop();
        server.Stop();
        assert(ReadBytes(fd, payload.size()) == payload);
        Eof(fd);
        close(fd);
    });
    server.Start();
    client.join();
    assert(closed == 1);
    server.Stop();
}

void DeadlineAndTimers()
{
    TcpServer server(0);
    server.SetThreadCount(2);
    server.SetShutdownGrace(1800ms);
    server.EnableInactiveRelease(1);
    ResourceLimits limits;
    limits.output_low = 1024 * 1024;
    limits.output_high = 2 * 1024 * 1024;
    limits.max_output = 32 * 1024 * 1024;
    limits.max_total_output = 64 * 1024 * 1024;
    server.SetResourceLimits(limits);
    std::atomic<int> timers{0};
    server.RunAfter([&] { ++timers; }, 1);
    std::promise<bool> accepted;
    auto ready = accepted.get_future();
    server.SetConnectedCallback([&](const ConnectionPtr &conn) {
        accepted.set_value(conn->Send(std::string(16 * 1024 * 1024, 's')));
    });
    std::promise<std::chrono::steady_clock::time_point> stopping;
    auto stop_time = stopping.get_future();
    std::promise<void> stopped;
    auto finished = stopped.get_future();
    const auto port = server.GetPort();
    std::thread client([&] {
        int fd = Connect(port, 4096);
        assert(Await(ready));
        stopping.set_value(std::chrono::steady_clock::now());
        server.Stop();
        // 客户端不读取、不关闭；由服务端截止时间强制结束排空。
        Await(finished);
        close(fd);
    });
    server.Start();
    auto elapsed = std::chrono::steady_clock::now() - Await(stop_time);
    stopped.set_value();
    client.join();
    assert(elapsed >= 1500ms && elapsed < 5s);
    assert(timers == 0);
}

void ConnectionLimit()
{
    TcpServer server(0);
    ResourceLimits limits;
    limits.max_connections = 1;
    server.SetResourceLimits(limits);
    std::promise<void> connected;
    auto ready = connected.get_future();
    std::atomic<int> count{0};
    server.SetConnectedCallback([&](const ConnectionPtr &conn) {
        if (++count == 1) connected.set_value();
        conn->Send("ready");
    });
    const auto port = server.GetPort();
    std::thread client([&] {
        int first = Connect(port);
        Await(ready);
        assert(ReadBytes(first, 5) == "ready");
        int excess = Connect(port);
        Eof(excess, true);
        close(excess);
        assert(shutdown(first, SHUT_WR) == 0);
        Eof(first);
        close(first);
        // 定时任务的登记和执行都发生在主循环，确保先前移除通知已处理。
        std::promise<void> reclaimed;
        auto space = reclaimed.get_future();
        server.RunAfter([&] { reclaimed.set_value(); }, 1);
        Await(space);
        int replacement = Connect(port);
        assert(ReadBytes(replacement, 5) == "ready");
        server.Stop();
        Eof(replacement);
        close(replacement);
    });
    server.Start();
    client.join();
    assert(count == 2);
}

void LargeBody()
{
    HttpServer server(0);
    server.SetThreadCount(2);
    const std::string payload(512 * 1024, 'u');
    std::atomic<int> calls{0};
    server.AddRoute("POST", "/upload", [&](const HttpRequest &request, HttpResponse &response) {
        assert(request.GetBody() == payload);
        ++calls;
        response.SetBody("uploaded");
    });
    const auto port = server.GetPort();
    std::thread client([&] {
        int fd = Connect(port);
        Send(fd, "POST /upload HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " +
                 std::to_string(payload.size()) + "\r\n\r\n");
        // 总正文超过默认未消费输入限额，分块输入应持续被解析器消费。
        for (std::size_t offset = 0; offset < payload.size(); offset += 8192)
            Send(fd, payload.substr(offset, 8192));
        assert(ResponseBody(fd) == "uploaded");
        Eof(fd);
        close(fd);
        server.Stop();
    });
    server.Start();
    client.join();
    assert(calls == 1);
}

void PipelineResume()
{
    HttpServer server(0);
    server.SetThreadCount(2);
    ResourceLimits limits;
    limits.output_low = 16 * 1024;
    limits.output_high = 64 * 1024;
    limits.max_output = 1024 * 1024;
    limits.max_total_output = 8 * 1024 * 1024;
    server.SetResourceLimits(limits);
    std::atomic<int> calls{0};
    server.AddRoute("GET", "/sequence", [&](const HttpRequest &, HttpResponse &response) {
        int index = calls.fetch_add(1);
        response.SetBody(std::to_string(index) + ":" + std::string(128 * 1024, 'p'));
    });
    const auto port = server.GetPort();
    std::thread client([&] {
        int fd = Connect(port);
        std::string requests;
        for (int i = 0; i < 12; ++i)
            requests += "GET /sequence HTTP/1.1\r\nHost: localhost\r\n" +
                        std::string(i == 11 ? "Connection: close\r\n" : "") + "\r\n";
        Send(fd, requests);
        for (int i = 0; i < 12; ++i)
            assert(ResponseBody(fd) == std::to_string(i) + ":" + std::string(128 * 1024, 'p'));
        Eof(fd);
        close(fd);
        server.Stop();
    });
    server.Start();
    client.join();
    assert(calls == 12);
}

template <class Exception, class Function> void Rejects(Function function)
{
    bool rejected = false;
    try { function(); }
    catch (const Exception &) { rejected = true; }
    assert(rejected);
}

void ForeignStopBeforeStart()
{
    uint16_t port;
    {
        TcpServer server(0);
        port = server.GetPort();
        std::thread stopper([&] { server.Stop(); server.Stop(); });
        stopper.join();
        Rejects<std::logic_error>([&] { server.Start(); });
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0);
    assert(errno == ECONNREFUSED);
    close(fd);
}

void ConfigurationContract()
{
    TcpServer server(0);
    Rejects<std::invalid_argument>([&] { server.SetThreadCount(-1); });
    Rejects<std::invalid_argument>([&] { server.SetShutdownGrace(-1ms); });
    for (int which = 0; which < 6; ++which)
    {
        ResourceLimits limits;
        switch (which)
        {
        case 0: limits.max_connections = 0; break;
        case 1: limits.max_input = 0; break;
        case 2: limits.output_low = 0; break;
        case 3: limits.output_low = limits.output_high; break;
        case 4: limits.output_high = limits.max_output + 1; break;
        case 5: limits.max_output = limits.max_total_output + 1; break;
        }
        Rejects<std::invalid_argument>([&] { server.SetResourceLimits(limits); });
    }
    // 零工作线程：回调位于构造线程，拒绝原因必须是状态冻结而不是线程不符。
    int callbacks = 0;
    auto assert_frozen = [&] {
        Rejects<std::logic_error>([&] { server.SetThreadCount(1); });
        Rejects<std::logic_error>([&] { server.SetResourceLimits(ResourceLimits{}); });
        Rejects<std::logic_error>([&] { server.SetShutdownGrace(1ms); });
        Rejects<std::logic_error>([&] { server.EnableInactiveRelease(1); });
        Rejects<std::logic_error>([&] { server.Start(); });
    };
    server.SetConnectedCallback([&](const ConnectionPtr &) {
        ++callbacks;
        assert_frozen();
        server.Stop();
    });
    const auto port = server.GetPort();
    std::thread client([&] {
        int fd = Connect(port);
        Eof(fd);
        close(fd);
    });
    server.Start();
    client.join();
    assert(callbacks == 1);
    assert_frozen();
}

void TimerExceptionCleanup()
{
    TcpServer server(0);
    server.SetThreadCount(2);
    server.RunAfter([] { throw std::runtime_error("timer failure marker"); }, 1);
    bool propagated = false;
    try { server.Start(); }
    catch (const std::runtime_error &error)
    {
        propagated = std::string(error.what()) == "timer failure marker";
    }
    assert(propagated);
    server.Stop();
    Rejects<std::logic_error>([&] { server.Start(); });
}

int OpenFds()
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != nullptr);
    int count = 0;
    while (readdir(directory)) ++count;
    closedir(directory);
    return count;
}

void StartupFailureRestoresResources()
{
    const int before = OpenFds();
    sigset_t original, after;
    assert(pthread_sigmask(SIG_SETMASK, nullptr, &original) == 0);
    {
        TcpServer server(0);
        server.SetThreadCount(2);
        server.EnableSignalStop();
        // 第一个工作循环成功，第二个失败：必须回收已启动的线程。
        fail_eventfd_after.store(1);
        Rejects<std::system_error>([&] { server.Start(); });
        assert(fail_eventfd_after.load() == -1);
        assert(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
        for (int signal : {SIGINT, SIGTERM})
            assert(sigismember(&original, signal) == sigismember(&after, signal));
        server.Stop();
        Rejects<std::logic_error>([&] { server.Start(); });
    }
    assert(OpenFds() == before);
}

void EmptyServerStop()
{
    for (int workers : {0, 2})
    {
        TcpServer server(0);
        server.SetThreadCount(workers);
        server.RunAfter([&] { server.Stop(); server.Stop(); }, 1);
        server.Start();
    }
}

void WorkerExceptionCleanup()
{
    TcpServer server(0);
    server.SetThreadCount(2);
    std::atomic<int> closed{0};
    server.SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    server.SetConnectedCallback([](const ConnectionPtr &) { throw std::bad_alloc(); });
    const auto port = server.GetPort();
    std::thread client([&] {
        int fd = Connect(port);
        Eof(fd);
        close(fd);
    });
    bool propagated = false;
    try { server.Start(); }
    catch (const std::bad_alloc &) { propagated = true; }
    client.join();
    assert(propagated);
    assert(closed == 1);
    server.Stop();
}
} // namespace

int main()
{
    std::signal(SIGALRM, Alarm);
    alarm(45);
    const int fd_baseline = OpenFds();
    StartupFailureRestoresResources();
    EmptyServerStop();
    StopBeforeStart();
    ForeignStopBeforeStart();
    ConfigurationContract();
    TimerExceptionCleanup();
    WorkerExceptionCleanup();
    DrainAccepted(0);
    DrainAccepted(3);
    DeadlineAndTimers();
    ConnectionLimit();
    LargeBody();
    PipelineResume();
    assert(OpenFds() == fd_baseline);
    alarm(0);
    std::cout << "Server shutdown and resource tests passed\n";
}
