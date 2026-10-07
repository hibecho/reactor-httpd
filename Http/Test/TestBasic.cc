// 集成测试：EventLoop 驱动的最小 echo 服务器 + 客户端。
//
// 服务器跑在主线程的 EventLoop 上，客户端跑在一个独立的 std::thread 里。二者靠
// std::atomic<bool> stop 与 EventLoop::QueueInLoop 的 eventfd 唤醒协作收尾。
//
// 三条不可动摇的纪律（违反任何一条都会以 use-after-free 或 abort 收场）：
//   1. Channel 没有析构函数，隐式析构**不会**从 Epoller 注销登记。销毁前必须显式
//      Remove()，且必须"先 Remove 再关 fd"——顺序颠倒或干脆跳过，会让复用的 fd 号
//      撞上残留表项，后续 AddEvent 抛 logic_error 直接 terminate。
//   2. 绝不能在一个连接自己的读回调里销毁它：Channel::Handle() 会连续调用读/写/错误/
//      关闭/事件五个回调，提前销毁就是 use-after-free。回收一律经 QueueInLoop 延后到
//      本轮所有 Handle() 返回之后（ExecuteTasks 阶段）。
//   3. accept 出的新 fd 不继承监听套接字的 O_NONBLOCK，必须补 SetNonBlock()，否则首次
//      无数据的 Recv 会把整个事件循环阻塞死。
//
// 背压假设：客户端每个窗口只发 8 KiB，且先读完回显再发下一窗，远低于内核 socket 缓冲，
// 因此服务器不需要输出缓冲与 EPOLLOUT 状态机。若仍然 Send 到 EAGAIN，说明该假设被破坏，
// 此时抛异常响亮失败，而不是静默丢字节污染回显契约。改动 kWindow/kLargeTotal 前请先
// 重新核算这两个量与 SO_SNDBUF/SO_RCVBUF 的关系。

#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"
#include "tcp/Socket.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #expr +              \
                                     " errno=" + std::to_string(errno));                                               \
    } while (false)

static bool WouldBlock()
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

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

// /proc/self/fd 的计数包含 opendir 自身的描述符，前后都算因而相互抵消。
// 只能做同一次运行内的前后相等比较，不能对绝对数值断言。
static int CountOpenFds()
{
    DIR *dir = opendir("/proc/self/fd");
    CHECK(dir != nullptr);
    int count = 0;
    while (readdir(dir) != nullptr)
        ++count;
    closedir(dir);
    return count;
}

namespace
{
    // 看门狗：不用裸 alarm。SIGALRM 的默认动作是直接终止进程，make 只会看到
    // "Alarm clock" 而无法定位是哪条断言挂住。这里只做异步信号安全的 write 与 _exit。
    void OnAlarm(int)
    {
        const char message[] = "TIMEOUT: 10 秒内未完成，疑似阻塞在 LoopOnce() 的 epoll_wait\n";
        const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)ignored;
        _exit(EXIT_FAILURE);
    }

    void ArmWatchdog()
    {
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = OnAlarm;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0; // 不设 SA_RESTART，让被中断的系统调用返回 EINTR
        if (sigaction(SIGALRM, &action, nullptr) != 0)
        {
            std::cerr << "sigaction 失败\n";
            std::exit(EXIT_FAILURE);
        }
        alarm(10);
    }
} // namespace

// accept + echo + 延迟回收。
//
// 成员声明顺序即析构顺序，必须保证 Channel 先于它持有的 fd 被销毁。
class EchoServer
{
  public:
    explicit EchoServer(EventLoop *loop)
        : _loop(loop)
    {
    }

    EchoServer(const EchoServer &) = delete;
    EchoServer &operator=(const EchoServer &) = delete;

    // 异常回退路径也会走到这里，必须幂等
    ~EchoServer()
    {
        Stop();
    }

    void Start(uint16_t port, const std::string &ip)
    {
        CHECK(_listener.CreateServer(port, ip));
        _listen_channel = std::make_unique<Channel>(_listener.GetFd(), _loop);
        _listen_channel->SetReadCallback([this] {
            OnAccept();
        });
        _listen_channel->EnableRead();
        _listener_registered = true;
    }

    void Stop()
    {
        for (auto &entry : _conns)
            entry.second->channel->Remove();
        _conns.clear();

        if (_listener_registered)
        {
            _listen_channel->Remove();
            _listener_registered = false;
        }
        _listen_channel.reset();
    }

    // 监听端口为 0 时由内核分配，这里读回真实端口
    uint16_t Port() const
    {
        return ntohs(Address(_listener.GetFd()).sin_port);
    }
    int ConnCount() const
    {
        return static_cast<int>(_conns.size());
    }
    int ClosedCount() const
    {
        return _closed.load(std::memory_order_acquire);
    }
    int ReadCalls() const
    {
        return _read_calls;
    }
    int ReadEvents() const
    {
        return _read_events;
    }
    int PeakConns() const
    {
        return _peak_conns;
    }

  private:
    struct Conn
    {
        Socket sock;                      // 接管 fd，析构即 close
        std::unique_ptr<Channel> channel; // 声明在 sock 之后 → 先于 sock 析构
        bool closing = false;
    };

    Conn *Find(int fd)
    {
        auto it = _conns.find(fd);
        return it == _conns.end() ? nullptr : it->second.get();
    }

    void OnAccept()
    {
        for (;;)
        {
            const int fd = _listener.Accept();
            if (fd < 0)
            {
                // 非阻塞监听套接字在无待处理连接时返回 EAGAIN，表示本批收完
                if (WouldBlock())
                    return;
                throw std::runtime_error("EchoServer::OnAccept: accept failed errno=" + std::to_string(errno));
            }

            Socket accepted(fd);
            CHECK(accepted.SetNonBlock()); // accept 出的新 fd 不继承 O_NONBLOCK

            auto conn = std::unique_ptr<Conn>(new Conn());
            conn->sock = std::move(accepted);
            conn->channel = std::make_unique<Channel>(fd, _loop);
            conn->channel->SetReadCallback([this, fd] {
                OnReadable(fd);
            });
            conn->channel->SetErrorCallback([this, fd] {
                RequestClose(fd);
            });
            conn->channel->SetCloseCallback([this, fd] {
                RequestClose(fd);
            });
            conn->channel->EnableRead();

            _conns.emplace(fd, std::move(conn));
            if (ConnCount() > _peak_conns)
                _peak_conns = ConnCount();
        }
    }

    void OnReadable(int fd)
    {
        Conn *conn = Find(fd);
        if (conn == nullptr || conn->closing)
            return;
        ++_read_events;

        std::array<char, 4096> buf{};
        for (;;)
        {
            // 一次就绪内排空到 EAGAIN：单次 Recv 拿到的最多是一个缓冲大小，
            // 大载荷必须靠这个循环才能在本轮读完。
            const ssize_t n = conn->sock.Recv(buf.data(), buf.size());
            ++_read_calls;

            if (n > 0)
            {
                if (!SendAll(fd, *conn, buf.data(), n))
                    return; // SendAll 内部已请求关闭
                continue;
            }
            if (n == 0)
            {
                RequestClose(fd); // 对端关闭：回收本侧连接
                return;
            }
            if (WouldBlock())
                return;       // 本轮已无数据
            RequestClose(fd); // 真实错误
            return;
        }
    }

    // 返回 false 表示已在内部请求关闭，调用方应立即返回
    bool SendAll(int fd, Conn &conn, const char *data, ssize_t len)
    {
        ssize_t sent = 0;
        while (sent < len)
        {
            const ssize_t n = conn.sock.Send(data + sent, static_cast<size_t>(len - sent));
            if (n > 0)
            {
                sent += n;
                continue;
            }
            // 单个 send 短写是正常的，但返回 EAGAIN 说明客户端窗口超过了内核缓冲，
            // 本文件的背压假设已被破坏。静默丢字节会污染回显契约，故响亮失败。
            if (WouldBlock())
                throw std::runtime_error("EchoServer: send would block，8 KiB 窗口假设被破坏");
            RequestClose(fd);
            return false;
        }
        return true;
    }

    // 去重：同一个 Handle() 里读回调与关闭/错误回调可能都请求关闭
    void RequestClose(int fd)
    {
        auto it = _conns.find(fd);
        if (it == _conns.end() || it->second->closing)
            return;
        it->second->closing = true;
        _loop->QueueInLoop([this, fd] {
            CloseConn(fd);
        });
    }

    // 只在 ExecuteTasks 阶段被调用，此时本轮所有 Channel::Handle() 均已返回，
    // 销毁 Channel 不会让同一轮里排在后面的回调踩空。
    void CloseConn(int fd)
    {
        auto it = _conns.find(fd);
        if (it == _conns.end())
            return; // 已被 Stop() 或先前的重复任务收走

        // 铁律：先 Remove 摘除登记，再让 Socket 析构关闭 fd
        it->second->channel->Remove();
        _conns.erase(it);
        _closed.fetch_add(1, std::memory_order_release);
    }

    EventLoop *_loop;
    Socket _listener; // 声明在 _listen_channel 之前 → 后于它析构
    std::unique_ptr<Channel> _listen_channel;
    std::unordered_map<int, std::unique_ptr<Conn>> _conns;
    bool _listener_registered = false;

    std::atomic<int> _closed{0}; // 唯一被客户端线程读取的量
    int _read_calls = 0;         // 以下三个只被 loop 线程触碰，故用普通 int
    int _read_events = 0;
    int _peak_conns = 0;
};

namespace
{
    constexpr std::size_t kWindow = 8 * 1024;
    constexpr std::size_t kLargeTotal = 64 * 1024;
    constexpr int kConcurrentClients = 8;
    constexpr int kSequentialRounds = 64;

    Socket Connect(uint16_t port)
    {
        Socket sock;
        CHECK(sock.CreateClient(port, "127.0.0.1"));
        Timeout(sock.GetFd()); // 让阻塞收发有界，回显丢失时报 EAGAIN 而不是挂死
        return sock;
    }

    void SendAllFd(Socket &sock, const char *data, std::size_t len)
    {
        std::size_t sent = 0;
        while (sent < len)
        {
            const ssize_t n = sock.Send(data + sent, len - sent);
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            throw std::runtime_error("client: send failed errno=" + std::to_string(errno));
        }
    }

    // 不能用 MSG_WAITALL：SO_RCVTIMEO 到期时会返回部分结果，反而掩盖超时
    void RecvExactly(Socket &sock, char *out, std::size_t len)
    {
        std::size_t got = 0;
        while (got < len)
        {
            const ssize_t n = sock.Recv(out + got, len - got);
            if (n > 0)
            {
                got += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0)
                throw std::runtime_error("client: recv 遇到对端提前关闭");
            throw std::runtime_error("client: recv failed errno=" + std::to_string(errno));
        }
    }

    // 服务器侧的回收发生在主线程的 LoopOnce() 里，客户端只能限时等待
    void WaitClosed(const EchoServer &server, int expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (server.ClosedCount() < expected)
        {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("等待服务器回收连接超时，期望 " + std::to_string(expected) + " 实得 " +
                                         std::to_string(server.ClosedCount()));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // S1 并发独立回显：每个连接一份内容不同的载荷，回显串台必然被逐字节比对抓到
    void ConcurrentEcho(uint16_t port)
    {
        std::vector<Socket> clients;
        clients.reserve(kConcurrentClients);
        for (int i = 0; i < kConcurrentClients; ++i)
            clients.push_back(Connect(port));

        std::array<char, 256> payload{};
        for (int i = 0; i < kConcurrentClients; ++i)
        {
            payload.fill(static_cast<char>('A' + i));
            SendAllFd(clients[i], payload.data(), payload.size());
        }

        std::array<char, 256> echo{};
        for (int i = 0; i < kConcurrentClients; ++i)
        {
            payload.fill(static_cast<char>('A' + i)); // 重建期望值
            echo.fill(0);
            RecvExactly(clients[i], echo.data(), echo.size());
            CHECK(echo == payload);
        }
    } // 8 个连接在此析构关闭

    // S2 大载荷：分窗口发送，每窗先读完回显。服务器读缓冲只有 4096，
    // 因此只有读回调真的排空到 EAGAIN 才能在本轮读完。
    void LargeEcho(uint16_t port)
    {
        Socket client = Connect(port);
        std::array<char, kWindow> window{};
        std::array<char, kWindow> echo{};

        for (std::size_t base = 0; base < kLargeTotal; base += kWindow)
        {
            for (std::size_t i = 0; i < window.size(); ++i)
                window[i] = static_cast<char>((base + i) % 251);
            SendAllFd(client, window.data(), window.size());

            echo.fill(0);
            RecvExactly(client, echo.data(), echo.size());
            CHECK(echo == window);
        }
    }

    // S4 连续 accept 与 fd 号复用：上一连接关闭后其 fd 号会被下一个复用，
    // 这是"先 Remove 再 close"纪律的最强回归——漏掉 Remove 或顺序颠倒，
    // 复用的 fd 号撞上残留表项会让 AddEvent 抛 logic_error 并 terminate。
    void SequentialEcho(uint16_t port)
    {
        for (int i = 0; i < kSequentialRounds; ++i)
        {
            Socket client = Connect(port);
            std::array<char, 32> payload{};
            payload.fill(static_cast<char>('a' + (i % 26)));
            SendAllFd(client, payload.data(), payload.size());

            std::array<char, 32> echo{};
            RecvExactly(client, echo.data(), echo.size());
            CHECK(echo == payload);
        }
    }

    // S5 空连接：连上立刻关闭、不发数据
    void EmptyConnection(uint16_t port)
    {
        Socket client = Connect(port);
    }

    void RunClientScenarios(uint16_t port, const EchoServer &server)
    {
        int expected_closed = 0;

        ConcurrentEcho(port);
        expected_closed += kConcurrentClients;
        WaitClosed(server, expected_closed);

        LargeEcho(port);
        ++expected_closed;
        WaitClosed(server, expected_closed);

        SequentialEcho(port);
        expected_closed += kSequentialRounds;
        WaitClosed(server, expected_closed);

        EmptyConnection(port);
        ++expected_closed;
        WaitClosed(server, expected_closed);
    }

    void RunIntegration()
    {
        EventLoop loop; // 最先声明 → 最后析构
        EchoServer server(&loop);
        server.Start(0, "127.0.0.1"); // 端口交给内核分配
        const uint16_t port = server.Port();

        std::atomic<bool> stop{false};
        std::exception_ptr client_error;

        std::thread client([&] {
            try
            {
                RunClientScenarios(port, server);
            }
            catch (...)
            {
                client_error = std::current_exception();
            }
            // 先置位再唤醒。eventfd 是水平触发的计数器，写进去的计数在被消费前一直可读，
            // 因此无论主线程此刻是还没进入 LoopOnce() 还是已经阻塞在 epoll_wait，
            // 这一次唤醒都不会丢失，两种时序都收敛。
            stop.store(true, std::memory_order_release);
            loop.QueueInLoop([] {
            });
        });

        // 守卫声明在 loop 之后 → 先于 loop 析构，保证异常回退路径上也一定 join。
        // 主线程若在 LoopOnce() 内因断言失败抛出，栈回退会先析构守卫，
        // 此时 loop 仍然存活，不存在子线程还在用一个已死 loop 的路径。
        struct JoinGuard
        {
            std::thread &thread;
            ~JoinGuard()
            {
                if (thread.joinable())
                    thread.join();
            }
        } guard{client};

        while (!stop.load(std::memory_order_acquire))
            loop.LoopOnce();

        client.join(); // join 之后子线程不可能再触碰 loop

        // 以下三个量都是 loop 线程写的，与读它们的线程相同，天然无竞争
        CHECK(server.ConnCount() == 0);
        CHECK(server.PeakConns() == kConcurrentClients);
        // 读回调若只 Recv 一次就返回，两个计数会严格相等；排空到 EAGAIN 才会大于
        CHECK(server.ReadCalls() > server.ReadEvents());

        server.Stop();

        if (client_error)
            std::rethrow_exception(client_error);
    }

    // 取基线前先跑一次完整的建连/拆除，让一次性的惰性分配先发生，避免误报。
    // 这里刻意不进入 LoopOnce()：目的只是让相关系统调用路径都跑过一遍。
    int WarmupAndBaseline()
    {
        {
            EventLoop loop;
            Socket listener;
            CHECK(listener.CreateServer(0, "127.0.0.1"));
            Socket client;
            CHECK(client.CreateClient(ntohs(Address(listener.GetFd()).sin_port), "127.0.0.1"));
            Socket accepted(listener.Accept());
            CHECK(accepted.GetFd() >= 0);
        }
        return CountOpenFds();
    }
} // namespace

int main()
{
    try
    {
        ArmWatchdog();

        const int baseline = WarmupAndBaseline();
        RunIntegration();
        CHECK(CountOpenFds() == baseline);

        std::cout << "TestBasic passed: 8 路并发独立回显、64 KiB 大载荷多轮读取、"
                     "对端关闭后的连接回收、64 轮 fd 号复用、空连接、fd 记账\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
