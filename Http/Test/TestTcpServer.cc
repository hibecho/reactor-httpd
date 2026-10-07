/**
 * @file TestTcpServer.cc
 * @brief TcpServer 端到端测试：真实回环连接、线程池分配、连接表回收与空闲超时。
 *
 * 结构约束：
 *  - TcpServer 只能在构造它的线程上 Start()。EventLoop 在构造时记录线程 id，
 *    Acceptor 的 SetAcceptCallback 会校验线程，因此主线程负责「构造 + Start()」，
 *    而 Start() 阻塞在主循环上，客户端驱动逻辑只能放在另一个线程里。
 *  - 读取 _conns 这类主循环独占的状态一律通过 QueueInLoop 回到主循环线程，
 *    避免与事件循环并发访问同一容器。
 *
 * 关停通过公共 Stop() 请求，Start() 返回时连接和工作线程已完成收尾。
 */

#include <any>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "base/Buffer.hpp"
#include "base/Logger.hpp"
#include "reactor/EventLoop.hpp"
#include "reactor/TimerQueue.hpp"
#include "tcp/Acceptor.hpp"
#include "tcp/Connection.hpp"
#include "tcp/Socket.hpp"

// 只在测试中访问主循环、线程池与连接表，用于观察线程归属和回收。
#define private public
#include "tcp/TcpServer.hpp"
#include "thread/LoopThread.hpp"
#include "thread/LoopThreadPool.hpp"
#undef private

namespace
{

    // 单个场景的看门狗预算。最长的场景（活动刷新）需要约 7 秒真实等待。
    constexpr unsigned int kStageSeconds = 30;
    constexpr int kClientTimeoutSeconds = 5;
    constexpr int kPollIntervalMs = 5;

    const char *g_stage = "startup";

    // 异步信号安全：只做 write + _exit，并回显当前场景名便于定位挂住的位置。
    void OnAlarm(int)
    {
        const char *prefix = "TestTcpServer timed out at stage: ";
        (void)!write(STDERR_FILENO, prefix, strlen(prefix));
        (void)!write(STDERR_FILENO, g_stage, strlen(g_stage));
        (void)!write(STDERR_FILENO, "\n", 1);
        _exit(2);
    }

    void BeginStage(const char *stage)
    {
        g_stage = stage;
        alarm(kStageSeconds);
        std::cout << "[tcp-server] " << stage << std::endl;
    }

    void EndStage()
    {
        alarm(0);
    }

    std::size_t CountOpenFds()
    {
        DIR *dir = opendir("/proc/self/fd");
        assert(dir != nullptr);
        std::size_t count = 0;
        while (readdir(dir) != nullptr)
            ++count;
        closedir(dir);
        return count;
    }

    uint16_t QueryPort(int listen_fd)
    {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        assert(getsockname(listen_fd, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        return ntohs(address.sin_port);
    }

    // 阻塞式客户端：监听套接字在 TcpServer 构造时已经 listen，
    // 因此即使 Start() 尚未进入事件循环，connect 也能成功。
    int ConnectTo(uint16_t port)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);

        timeval timeout{kClientTimeoutSeconds, 0};
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        return fd;
    }

    void SendAll(int fd, const char *data, std::size_t length)
    {
        std::size_t offset = 0;
        while (offset < length)
        {
            const ssize_t sent = send(fd, data + offset, length - offset, MSG_NOSIGNAL);
            assert(sent > 0);
            offset += static_cast<std::size_t>(sent);
        }
    }

    void RecvExact(int fd, char *data, std::size_t length)
    {
        std::size_t offset = 0;
        while (offset < length)
        {
            const ssize_t received = recv(fd, data + offset, length - offset, 0);
            assert(received > 0);
            offset += static_cast<std::size_t>(received);
        }
    }

    // 服务端主动关闭连接时客户端读到 EOF。
    bool RecvEof(int fd)
    {
        char byte = 0;
        return recv(fd, &byte, 1, 0) == 0;
    }

    template <typename Predicate> bool WaitFor(Predicate predicate, int timeout_ms = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        }
        return predicate();
    }

    // 在主循环线程内执行并等待结果；主循环必须在运行。
    template <typename Function> void RunInLoopSync(EventLoop *loop, Function function)
    {
        std::promise<void> done;
        std::future<void> settled = done.get_future();
        loop->QueueInLoop([&] {
            function();
            done.set_value();
        });
        assert(settled.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    // _conns 由主循环独占，只能在主循环线程里读。
    std::size_t ConnCount(TcpServer &server)
    {
        std::size_t count = 0;
        RunInLoopSync(server._baseloop.get(), [&] {
            count = server._conns.size();
        });
        return count;
    }

    // 由服务器负责连接回收与工作线程退出，不预先要求连接表为空。
    void StopServer(TcpServer &server)
    {
        server.Stop();
    }

    // 单线程（不启动工作线程）：回调链路、回显与线程归属。
    void ScenarioSingleThreadEcho()
    {
        BeginStage("single-threaded echo and callback thread");

        TcpServer server(0);
        server.SetThreadCount(0);
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        const std::thread::id base_thread = std::this_thread::get_id();
        std::mutex mutex;
        std::vector<std::thread::id> callback_threads;
        std::atomic<int> connected{0};
        std::atomic<int> messages{0};
        std::atomic<int> closed{0};

        const char payload[] = "hello-tcp-server";
        const std::size_t payload_size = sizeof(payload) - 1;

        server.SetConnectedCallback([&](const ConnectionPtr &) {
            std::lock_guard<std::mutex> lock(mutex);
            callback_threads.push_back(std::this_thread::get_id());
            connected.fetch_add(1);
        });
        server.SetAnyEventCallback([&](const ConnectionPtr &) {
            std::lock_guard<std::mutex> lock(mutex);
            callback_threads.push_back(std::this_thread::get_id());
        });
        server.SetMessageCallback([&](const ConnectionPtr &conn, Buffer *buffer) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                callback_threads.push_back(std::this_thread::get_id());
            }
            messages.fetch_add(1);
            conn->Send(buffer->ReadAsString(buffer->GetReadableSize()));
        });
        server.SetClosedCallback([&](const ConnectionPtr &) {
            std::lock_guard<std::mutex> lock(mutex);
            callback_threads.push_back(std::this_thread::get_id());
            closed.fetch_add(1);
        });

        std::thread driver([&] {
            const int client = ConnectTo(port);
            SendAll(client, payload, payload_size);

            char echoed[sizeof(payload)] = {};
            RecvExact(client, echoed, payload_size);
            assert(memcmp(echoed, payload, payload_size) == 0);

            assert(WaitFor([&] {
                return connected.load() == 1 && messages.load() >= 1;
            }));

            close(client);
            assert(WaitFor([&] {
                return closed.load() == 1;
            }));
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        {
            std::lock_guard<std::mutex> lock(mutex);
            assert(!callback_threads.empty());
            // 未启动工作线程时，全部回调都应在主循环线程上执行。
            for (const std::thread::id &id : callback_threads)
                assert(id == base_thread);
        }
        EndStage();
    }

    // 线程池：3 个工作线程轮流接管 6 条连接。
    void ScenarioThreadPoolDistribution()
    {
        BeginStage("worker thread pool distribution");

        constexpr int kClients = 6;
        constexpr std::size_t kWorkers = 3;

        TcpServer server(0);
        server.SetThreadCount(static_cast<int>(kWorkers));
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        const std::thread::id base_thread = std::this_thread::get_id();
        std::mutex mutex;
        std::set<std::thread::id> handler_threads;
        std::atomic<int> connected{0};
        std::atomic<int> messages{0};
        std::atomic<int> closed{0};

        server.SetConnectedCallback([&](const ConnectionPtr &) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                handler_threads.insert(std::this_thread::get_id());
            }
            connected.fetch_add(1);
        });
        server.SetMessageCallback([&](const ConnectionPtr &conn, Buffer *buffer) {
            messages.fetch_add(1);
            conn->Send(buffer->ReadAsString(buffer->GetReadableSize()));
        });
        server.SetClosedCallback([&](const ConnectionPtr &) {
            closed.fetch_add(1);
        });

        std::thread driver([&] {
            std::vector<int> clients;
            for (int i = 0; i < kClients; ++i)
                clients.push_back(ConnectTo(port));
            assert(WaitFor([&] {
                return connected.load() == kClients;
            }));

            std::vector<std::string> expected;
            for (int i = 0; i < kClients; ++i)
            {
                expected.push_back("client-" + std::to_string(i));
                SendAll(clients[static_cast<std::size_t>(i)], expected.back().data(), expected.back().size());
            }
            for (int i = 0; i < kClients; ++i)
            {
                std::string echoed(expected[static_cast<std::size_t>(i)].size(), '\0');
                RecvExact(clients[static_cast<std::size_t>(i)], echoed.data(), echoed.size());
                assert(echoed == expected[static_cast<std::size_t>(i)]);
            }
            assert(WaitFor([&] {
                return messages.load() >= kClients;
            }));

            for (int fd : clients)
                close(fd);
            assert(WaitFor([&] {
                return closed.load() == kClients;
            }));
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        {
            std::lock_guard<std::mutex> lock(mutex);
            // 轮转分配：6 条连接恰好覆盖 3 个工作线程，且主循环不参与处理。
            assert(handler_threads.size() == kWorkers);
            assert(handler_threads.count(base_thread) == 0);
        }
        EndStage();
    }

    // 空闲超时：没有任何数据往来的连接被服务器主动关闭。
    void ScenarioInactiveRelease()
    {
        BeginStage("inactive connection release");

        TcpServer server(0);
        server.SetThreadCount(1);
        server.EnableInactiveRelease(1);
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        std::atomic<int> closed{0};
        server.SetClosedCallback([&](const ConnectionPtr &) {
            closed.fetch_add(1);
        });

        std::thread driver([&] {
            const int client = ConnectTo(port);
            assert(RecvEof(client));
            assert(WaitFor([&] {
                return closed.load() == 1;
            }));
            close(client);
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        EndStage();
    }

    // 空闲计时会被数据往来刷新：持续收发时连接不会被关闭，停止后才会超时。
    void ScenarioActivityRefreshesTimeout()
    {
        BeginStage("activity refreshes the idle timer");

        TcpServer server(0);
        server.SetThreadCount(1);
        server.EnableInactiveRelease(2);
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        std::atomic<int> closed{0};
        server.SetClosedCallback([&](const ConnectionPtr &) {
            closed.fetch_add(1);
        });
        // 回显用于证明连接仍然可用（而不只是没被关闭）。
        server.SetMessageCallback([&](const ConnectionPtr &conn, Buffer *buffer) {
            conn->Send(buffer->ReadAsString(buffer->GetReadableSize()));
        });

        std::thread driver([&] {
            const int client = ConnectTo(port);
            const auto start = std::chrono::steady_clock::now();

            // 空闲超时为 2 个 tick，这里每 700 毫秒往返一次，跨越多个超时周期。
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
            {
                SendAll(client, "x", 1);
                char echoed = 0;
                RecvExact(client, &echoed, 1);
                assert(echoed == 'x');
                assert(closed.load() == 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
            }
            assert(closed.load() == 0);

            // 停止活动后，连接最终按空闲超时被关闭。
            assert(RecvEof(client));
            assert(WaitFor([&] {
                return closed.load() == 1;
            }));
            close(client);
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        EndStage();
    }

    // 未设置任何业务回调：数据只进缓冲区，连接仍然建立并回收。
    void ScenarioWithoutCallbacks()
    {
        BeginStage("lifecycle without user callbacks");

        TcpServer server(0);
        server.SetThreadCount(0);
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        std::thread driver([&] {
            const int client = ConnectTo(port);
            SendAll(client, "ignored-by-server", 17);
            assert(WaitFor([&] {
                return ConnCount(server) == 1;
            }));

            close(client);
            assert(WaitFor([&] {
                return ConnCount(server) == 0;
            }));
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        EndStage();
    }

    // 大负载回显：跨多次读事件与多次写事件，验证数据不被截断或乱序。
    void ScenarioLargePayloadEcho()
    {
        BeginStage("large payload echo");

        constexpr std::size_t kPayloadSize = 256 * 1024;

        TcpServer server(0);
        server.SetThreadCount(1);
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        std::atomic<int> messages{0};
        server.SetMessageCallback([&](const ConnectionPtr &conn, Buffer *buffer) {
            messages.fetch_add(1);
            conn->Send(buffer->ReadAsString(buffer->GetReadableSize()));
        });

        std::thread driver([&] {
            const int client = ConnectTo(port);

            std::string payload(kPayloadSize, '\0');
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>('a' + (i % 26));

            SendAll(client, payload.data(), payload.size());
            std::string echoed(payload.size(), '\0');
            RecvExact(client, echoed.data(), echoed.size());
            assert(echoed == payload);
            assert(messages.load() >= 1);

            close(client);
            assert(WaitFor([&] {
                return ConnCount(server) == 0;
            }));
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        EndStage();
    }

    // 参数校验：负数线程数在启动前被拒绝，且不影响后续配置与启动。
    void ScenarioRejectsNegativeThreadCount()
    {
        BeginStage("thread count validation");

        TcpServer server(0);
        bool rejected = false;
        try
        {
            server.SetThreadCount(-1);
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        assert(rejected);

        // 被拒绝后仍能正常配置并启动：单线程处理一条回显连接。
        server.SetThreadCount(0);
        const uint16_t port = QueryPort(server._acceptor->GetListenFd());

        server.SetMessageCallback([&](const ConnectionPtr &conn, Buffer *buffer) {
            conn->Send(buffer->ReadAsString(buffer->GetReadableSize()));
        });

        std::thread driver([&] {
            const int client = ConnectTo(port);
            SendAll(client, "ok", 2);
            char echoed[2] = {};
            RecvExact(client, echoed, 2);
            assert(memcmp(echoed, "ok", 2) == 0);

            close(client);
            StopServer(server);
        });

        server.Start();
        driver.join();

        assert(server._conns.empty());
        EndStage();
    }

} // namespace

int main()
{
    std::signal(SIGALRM, OnAlarm);
    std::signal(SIGPIPE, SIG_IGN);
    alarm(kStageSeconds);

    const std::size_t baseline_fds = CountOpenFds();

    ScenarioSingleThreadEcho();
    ScenarioThreadPoolDistribution();
    ScenarioInactiveRelease();
    ScenarioActivityRefreshesTimeout();
    ScenarioWithoutCallbacks();
    ScenarioLargePayloadEcho();
    ScenarioRejectsNegativeThreadCount();

    // 全部服务器已停止：fd 数应回到基准，服务器不留下描述符泄漏。
    assert(CountOpenFds() == baseline_fds);
    std::cout << "TcpServer end-to-end tests passed\n";
    return 0;
}
