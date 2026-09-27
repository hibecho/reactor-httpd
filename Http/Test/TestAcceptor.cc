#include "base/Logger.hpp"
// Acceptor 测试。
//
// 监听事件的登记发生在 StartAccepting()，构造函数只创建监听套接字与 Channel。
// 因此每个需要 accept 的用例都必须显式调用 StartAccepting()——这与
// TcpServer::Start()「先设置 accept 回调，再开始接收」的用法一致。
// 只有 DestructorUnregistersChannel 这类用例同样必须调用它，否则「从未登记」
// 会让析构注销的断言失去意义。

#include "tcp/Acceptor.hpp"
#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <new>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <sys/timerfd.h>

namespace
{
// 看门狗：不使用裸 alarm。SIGALRM 的默认动作是直接终止进程，make 只会看到
// "Alarm clock" 而无法定位是哪条断言挂住。这里只做异步信号安全的 write 与 _exit。
// 全部用例都是同步的、无 sleep，30 秒只作为「accept 循环没有停在 EAGAIN」这类
// 挂死的兜底。
void OnAlarm(int)
{
    const char message[] = "TIMEOUT: 30 秒内未完成，疑似 HandleRead 未在 EAGAIN/EMFILE 处停止\n";
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
        std::cerr << "sigaction failed\n";
        std::exit(EXIT_FAILURE);
    }
    alarm(30);
}

// /proc/self/fd 的计数包含 opendir 自身的描述符，前后都算因而相互抵消，
// 所以只能做前后相等比较，不能对绝对数值断言。
int CountOpenFds()
{
    // 等待后台日志完成，避免时间格式化等临时文件访问干扰 fd 快照。
    Logger::Instance().Flush();
    DIR *dir = opendir("/proc/self/fd");
    assert(dir != nullptr);
    int count = 0;
    while (readdir(dir) != nullptr)
        ++count;
    closedir(dir);
    return count;
}

// 临时压低进程描述符上限用的 RAII。只有真的耗尽描述符，accept 才会返回
// EMFILE——伪造返回值覆盖不到 Socket::Accept 保留 errno 并记录日志的那条路径，
// 也验证不了「释放描述符后恢复」本身。
// 上限是进程级状态，任何退出路径都必须恢复：留下一个低上限会让后续用例以难以
// 定位的方式失败，所以恢复失败直接终止，不留给调用方继续跑的机会。
struct FdLimitGuard
{
    struct rlimit original;

    explicit FdLimitGuard(rlim_t soft)
    {
        if (getrlimit(RLIMIT_NOFILE, &original) != 0)
        {
            std::cerr << "getrlimit failed\n";
            std::exit(EXIT_FAILURE);
        }
        struct rlimit limited = original;
        limited.rlim_cur = soft;
        if (setrlimit(RLIMIT_NOFILE, &limited) != 0)
        {
            std::cerr << "setrlimit failed\n";
            std::exit(EXIT_FAILURE);
        }
    }

    ~FdLimitGuard()
    {
        if (setrlimit(RLIMIT_NOFILE, &original) != 0)
        {
            const char message[] = "FATAL: 恢复 RLIMIT_NOFILE 失败，后续用例结果不可信\n";
            const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
            (void)ignored;
            _exit(EXIT_FAILURE);
        }
    }

    FdLimitGuard(const FdLimitGuard &) = delete;
    FdLimitGuard &operator=(const FdLimitGuard &) = delete;
};

// 填满描述符表直到 EMFILE，返回实际填充数（由调用方负责关闭）。
// 返回值等于 cap 说明上限没被压到能触发耗尽的程度，用例前提不成立。
int FillFdsUntilEmfile(int *fds, int cap)
{
    int filled = 0;
    while (filled < cap)
    {
        const int fd = open("/dev/null", O_RDONLY);
        if (fd < 0)
        {
            assert(errno == EMFILE);
            break;
        }
        fds[filled++] = fd;
    }
    return filled;
}

void Step(EventLoop &loop)
{
    // 保证即使当前没有网络事件，本轮也不会阻塞。
    loop.QueueInLoop([] {});
    loop.LoopOnce();
}

// 阻塞式连到回环地址上的指定端口，返回由调用者负责关闭的描述符。
int ConnectTo(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(fd >= 0);

    struct sockaddr_in address
    {
    };
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(connect(fd, reinterpret_cast<struct sockaddr *>(&address), static_cast<socklen_t>(sizeof(address))) == 0);
    return fd;
}

struct Client
{
    int fd;
    ~Client() { close(fd); }
};

uint16_t AddressPort(int fd)
{
    struct sockaddr_in address
    {
    };
    socklen_t length = static_cast<socklen_t>(sizeof(address));
    assert(getsockname(fd, reinterpret_cast<struct sockaddr *>(&address), &length) == 0);
    return ntohs(address.sin_port);
}

// 构造失败必须抛错且不留描述符泄漏。
void ConstructorRejectsNullLoop()
{
    const int baseline = CountOpenFds();
    bool rejected = false;
    try
    {
        Acceptor acceptor(nullptr, 0);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    assert(rejected);
    assert(CountOpenFds() == baseline);
}

// 端口已被占用时 CreateServer 失败，构造必须抛错。
// 占用方使用裸 socket + bind 且不设 SO_REUSEPORT：Socket::ReuseAddress 会同时打开
// SO_REUSEADDR 与 SO_REUSEPORT，而 SO_REUSEPORT 要求参与绑定的所有套接字都设置它，
// 否则 bind 返回 EADDRINUSE。若占用方也开了 SO_REUSEPORT，本用例会静默失效。
void ConstructorRejectsOccupiedPort()
{
    EventLoop loop;

    const int occupant = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(occupant >= 0);
    struct sockaddr_in address
    {
    };
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(bind(occupant, reinterpret_cast<struct sockaddr *>(&address), static_cast<socklen_t>(sizeof(address))) == 0);

    const uint16_t port = AddressPort(occupant);
    assert(port != 0);

    const int baseline = CountOpenFds();
    bool rejected = false;
    bool rejected_too_late = false;
    try
    {
        Acceptor acceptor(&loop, port);
    }
    catch (const std::system_error &)
    {
        // system_error 派生自 runtime_error，只用 catch (runtime_error) 接不住区别：
        // 忽略 CreateServer 返回值时，构造会带着已置 -1 的描述符继续走到 EnableRead，
        // 在内核 epoll_ctl 处才抛 system_error，那样「在 CreateServer 处拒绝」的
        // 契约并没有被验证。必须把它单独分出来。
        rejected_too_late = true;
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    assert(rejected && !rejected_too_late);
    // 失败时监听描述符已随构造回退释放。
    assert(CountOpenFds() == baseline);

    close(occupant);
}

// 原始缺陷的回归钉：先 Create() 再 CreateServer() 会让 CreateServer 内部
// 那句 if (!Create()) return false 因 EALREADY 必然失败，绑定与监听被整体跳过，
// 于是端口为 0、SO_ACCEPTCONN 为 0。
void ListeningSocketIsConfigured()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    const uint16_t port = AddressPort(acceptor.GetListenFd());

    const int fd = acceptor.GetListenFd();
    assert(fd >= 0);
    assert(port != 0);

    int accepting = -1;
    socklen_t option_length = static_cast<socklen_t>(sizeof(accepting));
    assert(getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &option_length) == 0);
    assert(accepting == 1);

    // CreateServer 内部会调用 SetNonBlock，监听描述符必须是非阻塞的。
    const int flags = fcntl(fd, F_GETFL);
    assert(flags != -1 && (flags & O_NONBLOCK) != 0);

    // 端口确实可连。
    Client client{ConnectTo(port)};
}

void AcceptsSingleConnection()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();

    int count = 0;
    int accepted_fd = -1;
    acceptor.SetAcceptCallback([&](int fd) {
        ++count;
        accepted_fd = fd;
    });

    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};
    Step(loop);

    assert(count == 1);
    assert(accepted_fd >= 0);

    // 契约定格：Acceptor 不改动新描述符的状态标志。Linux 的 accept 不继承监听
    // 套接字的 O_NONBLOCK，需要非阻塞由回调方自行设置（Connection 构造函数已完成）。
    const int flags = fcntl(accepted_fd, F_GETFL);
    assert(flags != -1 && (flags & O_NONBLOCK) == 0);

    char byte = 'x';
    assert(send(accepted_fd, &byte, 1, MSG_NOSIGNAL) == 1);

    close(accepted_fd);
}

// 低于单轮预算的连接应在一次 LoopOnce() 内全部处理。
void AcceptsAllPendingConnections()
{
    const int kClients = 8;

    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    const uint16_t port = AddressPort(acceptor.GetListenFd());

    int count = 0;
    int fds[kClients];
    for (int i = 0; i < kClients; ++i)
        fds[i] = -1;
    acceptor.SetAcceptCallback([&](int fd) {
        assert(count < kClients);
        fds[count] = fd;
        ++count;
    });

    Client clients[kClients] = {{ConnectTo(port)}, {ConnectTo(port)},
                                {ConnectTo(port)}, {ConnectTo(port)},
                                {ConnectTo(port)}, {ConnectTo(port)},
                                {ConnectTo(port)}, {ConnectTo(port)}};

    Step(loop);

    assert(count == kClients);
    for (int i = 0; i < kClients; ++i)
    {
        assert(fds[i] >= 0);
        assert(fcntl(fds[i], F_GETFL) != -1);
        for (int j = 0; j < i; ++j)
            assert(fds[i] != fds[j]); // 每个连接拿到互不相同的描述符
    }

    for (int i = 0; i < kClients; ++i)
        close(fds[i]);
}

// 无可读连接时不得触发回调，也不得因 EAGAIN 处理不当而空转。
// 若把 fd < 0 分支写成 continue，LT 模式下 HandleRead 永不返回，由看门狗兜住。
void IdleReadDoesNotInvokeCallback()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();

    int count = 0;
    acceptor.SetAcceptCallback([&](int fd) {
        ++count;
        close(fd);
    });

    Step(loop);
    assert(count == 0);

    const int baseline = CountOpenFds();
    Step(loop);
    assert(count == 0);
    assert(CountOpenFds() == baseline);
}

// 未设置回调时，accept 到的描述符必须被释放而不是泄漏。
// 回调方不接管就没人会关它，LT 模式下不处理还会反复触发同一条通知。
void EmptyCallbackClosesAcceptedFd()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();

    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};
    const int baseline = CountOpenFds();

    Step(loop);

    assert(CountOpenFds() == baseline);
}

// 回调抛出的普通异常不得逃出事件分发，监听端必须继续工作。
void CallbackExceptionIsContained()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    const uint16_t port = AddressPort(acceptor.GetListenFd());

    int count = 0;
    acceptor.SetAcceptCallback([&](int fd) {
        ++count;
        close(fd);
        throw std::runtime_error("injected accept callback failure");
    });

    Client first{ConnectTo(port)};
    Step(loop); // 不得向外抛异常
    assert(count == 1);

    Client second{ConnectTo(port)};
    Step(loop);
    assert(count == 2);
}

// 与 Connection 的约定一致：内存耗尽向上传播，不被静默吞掉。
void CallbackBadAllocPropagates()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();

    int count = 0;
    acceptor.SetAcceptCallback([&](int fd) {
        ++count;
        close(fd);
        throw std::bad_alloc();
    });

    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};

    bool propagated = false;
    try
    {
        Step(loop);
    }
    catch (const std::bad_alloc &)
    {
        propagated = true;
    }
    assert(propagated);
    assert(count == 1);
}

void SetAcceptCallbackReplacesPreviousCallback()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();

    int first = 0;
    int second = 0;
    acceptor.SetAcceptCallback([&](int fd) {
        ++first;
        close(fd);
    });
    acceptor.SetAcceptCallback([&](int fd) {
        ++second;
        close(fd);
    });

    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};
    Step(loop);

    assert(first == 0 && second == 1);
}

// 析构必须注销 Channel 登记。用「同号描述符重新登记是否成功」探测，与地址无关：
// 残留表项会让 UpdateEvent 走到 ModifyEvent，由 RequireRegistered 的身份比较抛
// logic_error，或由内核 EPOLL_CTL_MOD 返回 ENOENT 抛 system_error——两条路径都会
// 让下面的断言失败。不能用「端口不可连」代替，那只能证明描述符已关闭。
void DestructorUnregistersChannel()
{
    EventLoop loop;
    int listen_fd = -1;
    {
        Acceptor acceptor(&loop, 0);
        acceptor.StartAccepting();
        listen_fd = acceptor.GetListenFd();
        assert(listen_fd >= 0);
    }

    // 监听描述符已随 ~Acceptor 关闭，号码可被复用。
    errno = 0;
    assert(fcntl(listen_fd, F_GETFD) == -1 && errno == EBADF);

    const int source = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(source >= 0);
    if (source != listen_fd)
    {
        assert(dup2(source, listen_fd) == listen_fd);
        close(source);
    }

    Channel probe(listen_fd, &loop);
    bool registered = true;
    try
    {
        probe.EnableRead();
    }
    catch (...)
    {
        registered = false;
    }
    assert(registered);

    probe.Remove();
    close(listen_fd);
}

// 实际耗尽描述符，验证暂停监听和定时恢复。资源释放后用 LoopOnce 等待
// 独立退避定时器，不依赖 sleep 猜测恢复时刻；看门狗兜住遗漏恢复的缺陷。
void AcceptEmfileThenRecovers(bool stop_during_backoff = false)
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();

    int count = 0;
    int accepted_fd = -1;
    acceptor.SetAcceptCallback([&](int fd) {
        ++count;
        accepted_fd = fd;
    });

    // EventLoop、Acceptor 和这次 connect 都要占用描述符，必须都在压低上限之前完成。
    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};

    const int baseline = CountOpenFds();

    int filler[64];
    int filled = 0;
    std::chrono::steady_clock::time_point paused_at;
    {
        // 略高于当前用量：留出的余量刚好让填充循环能证明「还开得动，然后开不动」。
        FdLimitGuard guard(static_cast<rlim_t>(baseline + 2));
        filled = FillFdsUntilEmfile(filler, 64);
        // 必须是被 EMFILE 挡下而不是数组先满，否则这个用例什么也没验证。
        assert(filled > 0 && filled < 64);

        // 连接已经躺在内核的 accept 队列里，但此刻无处安放新的描述符。
        paused_at = std::chrono::steady_clock::now();
        Step(loop);
        assert(count == 0);
    }

    for (int i = 0; i < filled; ++i)
        close(filler[i]);

    if (stop_during_backoff)
    {
        acceptor.StopAccepting();
        assert(acceptor.GetListenFd() == -1);
        // 用独立定时事件驱动超过退避期，确认 Stop 后旧恢复事件不会重新启动监听。
        const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        assert(timer >= 0);
        Channel timer_channel(timer, &loop);
        bool elapsed = false;
        timer_channel.SetReadCallback([&] {
            uint64_t ticks = 0;
            assert(read(timer, &ticks, sizeof(ticks)) == sizeof(ticks));
            elapsed = true;
        });
        timer_channel.EnableRead();
        itimerspec delay{};
        delay.it_value.tv_nsec = 150 * 1000 * 1000;
        assert(timerfd_settime(timer, 0, &delay, nullptr) == 0);
        while (!elapsed) loop.LoopOnce();
        timer_channel.Remove();
        close(timer);
        assert(count == 0);
        assert(CountOpenFds() == baseline - 2); // 监听和退避 timer 均已关闭。
        return;
    }

    // Start 幂等，不能绕过当前退避；实际恢复由 timerfd 触发。
    acceptor.StartAccepting();
    while (count == 0) loop.LoopOnce();
    assert(std::chrono::steady_clock::now() - paused_at >= std::chrono::milliseconds(100));
    assert(count == 1);
    assert(accepted_fd >= 0);

    // 拿到的必须是可用的描述符，而不只是一个非负整数。
    char byte = 'y';
    assert(send(accepted_fd, &byte, 1, MSG_NOSIGNAL) == 1);
    close(accepted_fd);

    assert(CountOpenFds() == baseline);
}
} // namespace

namespace
{
struct CopyFailure
{
    int *mode;
    int *calls;
    CopyFailure(int *m, int *c) : mode(m), calls(c) {}
    CopyFailure(const CopyFailure &other) : mode(other.mode), calls(other.calls)
    {
        const int failure = *mode;
        *mode = 0;
        if (failure == 1) throw std::runtime_error("copy failure");
        if (failure == 2) throw std::bad_alloc();
    }
    void operator()(int fd) const { ++*calls; close(fd); }
};

void CopyFailureClosesFd(int failure)
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    const uint16_t port = AddressPort(acceptor.GetListenFd());
    int mode = 0;
    int calls = 0;
    acceptor.SetAcceptCallback(CopyFailure(&mode, &calls));
    Client first{ConnectTo(port)};
    Client second{ConnectTo(port)};
    const int baseline = CountOpenFds();
    mode = failure;
    bool propagated = false;
    try { Step(loop); }
    catch (const std::bad_alloc &) { propagated = true; }
    assert(propagated == (failure == 2));
    assert(CountOpenFds() == baseline);
    if (failure == 2)
    {
        assert(calls == 0);
        Step(loop);
    }
    assert(calls == 1);
    assert(CountOpenFds() == baseline);
}

void CallbackChangesApplyToNextConnection(bool clear)
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    const uint16_t port = AddressPort(acceptor.GetListenFd());
    int first_calls = 0;
    int next_calls = 0;
    acceptor.SetAcceptCallback([&, token = std::make_shared<int>(42)](int fd) {
        ++first_calls;
        close(fd);
        if (clear) acceptor.SetAcceptCallback({});
        else acceptor.SetAcceptCallback([&](int next) { ++next_calls; close(next); });
        assert(*token == 42);
    });
    Client first{ConnectTo(port)};
    Client second{ConnectTo(port)};
    const int baseline = CountOpenFds();
    Step(loop);
    assert(first_calls == 1);
    assert(next_calls == (clear ? 0 : 1));
    assert(CountOpenFds() == baseline);
}

void CallbackExceptionDoesNotCloseReusedFd()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    int replacement = -1;
    acceptor.SetAcceptCallback([&](int fd) {
        close(fd);
        const int source = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        assert(source >= 0);
        if (source != fd) { assert(dup2(source, fd) == fd); close(source); }
        replacement = fd;
        throw std::runtime_error("after fd reuse");
    });
    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};
    Step(loop);
    assert(replacement >= 0 && fcntl(replacement, F_GETFD) >= 0);
    close(replacement);
}

void WrongThreadIsRejected()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    int calls = 0;
    acceptor.SetAcceptCallback([&](int fd) { ++calls; close(fd); });
    const int baseline = CountOpenFds();
    bool constructor_rejected = false;
    bool setter_rejected = false;
    bool start_rejected = false;
    bool stop_rejected = false;
    std::thread worker([&] {
        try { Acceptor wrong(&loop, 0); }
        catch (const std::logic_error &) { constructor_rejected = true; }
        try { acceptor.SetAcceptCallback({}); }
        catch (const std::logic_error &) { setter_rejected = true; }
        try { acceptor.StartAccepting(); }
        catch (const std::logic_error &) { start_rejected = true; }
        try { acceptor.StopAccepting(); }
        catch (const std::logic_error &) { stop_rejected = true; }
    });
    worker.join();
    assert(constructor_rejected && setter_rejected && start_rejected && stop_rejected);
    assert(CountOpenFds() == baseline);
    Client client{ConnectTo(AddressPort(acceptor.GetListenFd()))};
    Step(loop);
    assert(calls == 1);
}
// 停止可以发生在 accept 回调内部；同轮其余待接入连接不能继续交给业务。
void StopDuringAccept()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    const uint16_t port = AddressPort(acceptor.GetListenFd());
    int calls = 0;
    acceptor.SetAcceptCallback([&](int fd) {
        close(fd);
        ++calls;
        acceptor.StopAccepting();
    });
    Client first{ConnectTo(port)};
    Client second{ConnectTo(port)};
    Step(loop);
    assert(calls == 1);
    assert(acceptor.GetListenFd() == -1);
    acceptor.StopAccepting();
    Step(loop);
    assert(calls == 1);
    bool rejected = false;
    try { acceptor.StartAccepting(); }
    catch (const std::logic_error &) { rejected = true; }
    assert(rejected);
}

void StopBeforeStart()
{
    const int baseline = CountOpenFds();
    {
        EventLoop loop;
        Acceptor acceptor(&loop, 0);
        acceptor.StopAccepting();
        acceptor.StopAccepting();
        assert(acceptor.GetListenFd() == -1);
        Step(loop);
    }
    assert(CountOpenFds() == baseline);
}

void AcceptBudgetAllowsStopTask()
{
    EventLoop loop;
    Acceptor acceptor(&loop, 0);
    acceptor.StartAccepting();
    const uint16_t port = AddressPort(acceptor.GetListenFd());
    std::vector<int> clients;
    for (int i = 0; i < 128; ++i) clients.push_back(ConnectTo(port));
    int calls = 0;
    acceptor.SetAcceptCallback([&](int fd) { ++calls; close(fd); });
    loop.QueueInLoop([&] { acceptor.StopAccepting(); });
    loop.LoopOnce();
    assert(calls == 64);
    assert(acceptor.GetListenFd() == -1);
    Step(loop);
    assert(calls == 64);
    for (int fd : clients) close(fd);
}

} // namespace

int main()
{
    ArmWatchdog();
    try
    {
        ConstructorRejectsNullLoop();
        CopyFailureClosesFd(1);
        CopyFailureClosesFd(2);
        CallbackChangesApplyToNextConnection(false);
        CallbackChangesApplyToNextConnection(true);
        CallbackExceptionDoesNotCloseReusedFd();
        WrongThreadIsRejected();
        ConstructorRejectsOccupiedPort();
        ListeningSocketIsConfigured();
        AcceptsSingleConnection();
        AcceptsAllPendingConnections();
        IdleReadDoesNotInvokeCallback();
        EmptyCallbackClosesAcceptedFd();
        CallbackExceptionIsContained();
        CallbackBadAllocPropagates();
        SetAcceptCallbackReplacesPreviousCallback();
        DestructorUnregistersChannel();
        AcceptEmfileThenRecovers();
        AcceptEmfileThenRecovers(true);
        StopDuringAccept();
        StopBeforeStart();
        AcceptBudgetAllowsStopTask();

        std::cout << "Acceptor tests passed: 构造失败路径、监听套接字配置、单连接与批量 accept、"
                     "空闲不触发与 EAGAIN 终止、空回调释放描述符、回调异常隔离、bad_alloc 传播、"
                     "回调替换、析构注销登记、EMFILE 退避恢复、退避中停止、接入预算与停止。\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Acceptor tests failed: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
