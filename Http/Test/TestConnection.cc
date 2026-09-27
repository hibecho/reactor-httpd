#include "base/Buffer.hpp"
#include "reactor/Channel.hpp"
#include "tcp/Connection.hpp"
#include "reactor/EventLoop.hpp"

#include <any>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// 测试链接时包装 Buffer::Write，一次性注入故障，其余调用使用真实实现。
static int write_failure = 0;
extern "C" void __real__ZN6Buffer5WriteEPKvm(Buffer *, const void *, std::size_t);
extern "C" void __wrap__ZN6Buffer5WriteEPKvm(Buffer *buffer, const void *data, std::size_t size)
{
    const int failure = write_failure;
    write_failure = 0;
    if (failure == 1)
        throw std::length_error("injected Buffer::Write failure");
    if (failure == 2)
        throw std::bad_alloc();
    __real__ZN6Buffer5WriteEPKvm(buffer, data, size);
}

namespace
{
// 看门狗：不使用裸 alarm。SIGALRM 的默认动作是直接终止进程，make 只会看到
// "Alarm clock" 而无法定位是哪条断言挂住。这里只做异步信号安全的 write 与 _exit。
// 预算是全套用例的总时长（含两次 1.2 秒的空闲超时等待），留足 sanitizer 下的裕量。
void OnAlarm(int)
{
    const char message[] = "TIMEOUT: 30 秒内未完成，疑似阻塞在 LoopOnce() 的 epoll_wait\n";
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
    DIR *dir = opendir("/proc/self/fd");
    assert(dir != nullptr);
    int count = 0;
    while (readdir(dir) != nullptr)
        ++count;
    closedir(dir);
    return count;
}

struct Peer
{
    int fd;
    ~Peer() { close(fd); }
};

ConnectionPtr MakeConnection(EventLoop &loop, uint64_t id, int &peer)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    peer = fds[1];
    return std::make_shared<Connection>(&loop, id, fds[0]);
}

void Step(EventLoop &loop)
{
    // 保证即使当前没有网络事件，本轮也不会阻塞。
    loop.QueueInLoop([] {});
    loop.LoopOnce();
}

void WriteRequest(int fd, const std::string &data)
{
    assert(send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size()));
}

bool ReadAvailable(int fd, std::string &data)
{
    char buffer[8192];
    for (;;)
    {
        const ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
        if (size > 0)
            data.append(buffer, static_cast<std::size_t>(size));
        else if (size == 0)
            return true;
        else if (errno == EINTR)
            continue;
        else
        {
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            return false;
        }
    }
}

void MessageFailureIsIsolated(bool nonstandard)
{
    EventLoop loop;
    int bad_peer_fd;
    auto bad = MakeConnection(loop, 1, bad_peer_fd);
    Peer bad_peer{bad_peer_fd};
    int good_peer_fd;
    auto good = MakeConnection(loop, 2, good_peer_fd);
    Peer good_peer{good_peer_fd};
    const int bad_fd = bad->GetFd();
    std::weak_ptr<Connection> weak = bad;
    int messages = 0;
    int closed = 0;
    int removed = 0;
    int any_events = 0;
    bool before_release = false;

    bad->SetMessageCallback([&](const ConnectionPtr &, Buffer *) {
        ++messages;
        // 此任务排在异常处理安排的释放任务前，验证释放确实被推迟。
        loop.QueueInLoop([&] {
            assert(fcntl(bad_fd, F_GETFD) >= 0);
            assert(!weak.expired());
            assert(!bad->IsConnected());
            before_release = true;
        });
        if (nonstandard)
            throw 42;
        throw std::runtime_error("message callback failure");
    });
    bad->SetAnyEventCallback([&](const ConnectionPtr &) { ++any_events; });
    bad->SetClosedCallback([&](const ConnectionPtr &conn) {
        ++closed;
        assert(before_release);
        assert(conn->GetFd() == -1);
        errno = 0;
        assert(fcntl(bad_fd, F_GETFD) == -1 && errno == EBADF);
        conn->ShutDown(); // 重入关闭不能重复通知。
        throw std::runtime_error("closed callback failure");
    });
    bad->SetServerClosedCallback([&](const ConnectionPtr &) {
        ++removed;
        assert(closed == 1);
        bad.reset(); // 模拟服务器连接表删除最后一份长期持有。
    });
    good->SetMessageCallback([](const ConnectionPtr &conn, Buffer *buffer) {
        conn->Send(buffer->ReadAsString(buffer->GetReadableSize()));
    });
    bad->Established();
    good->Established();
    WriteRequest(bad_peer.fd, "bad");
    WriteRequest(good_peer.fd, "healthy");
    std::string response;
    for (int i = 0; i < 32 && (removed == 0 || response != "healthy"); ++i)
    {
        Step(loop);
        ReadAvailable(good_peer.fd, response);
    }
    assert(messages == 1 && closed == 1 && removed == 1);
    assert(any_events == 0);
    assert(weak.expired());
    assert(response == "healthy" && good->IsConnected());
    good->ShutDown();
    Step(loop);
    assert(good->GetFd() == -1);
}

void PeerEofDrainsResponse()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 3, peer_fd);
    Peer peer{peer_fd};
    const int send_buffer = 4096;
    assert(setsockopt(conn->GetFd(), SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    const std::string expected(128 * 1024, 'x');
    int closed = 0;
    conn->SetMessageCallback([&](const ConnectionPtr &current, Buffer *buffer) {
        assert(buffer->ReadAsString(buffer->GetReadableSize()) == "request");
        current->Send(expected);
    });
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->Established();
    WriteRequest(peer.fd, "request");
    assert(shutdown(peer.fd, SHUT_WR) == 0);
    std::string response;
    bool eof = false;
    for (int i = 0; i < 512 && !eof; ++i)
    {
        Step(loop);
        eof = ReadAvailable(peer.fd, response);
    }
    assert(eof && response == expected);
    assert(closed == 1 && conn->GetFd() == -1);
}

void AllocationFailurePropagates(bool buffer_failure)
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 4, peer_fd);
    Peer peer{peer_fd};
    const int fd = conn->GetFd();
    int closed = 0;
    int messages = 0;
    conn->SetMessageCallback([&](const ConnectionPtr &, Buffer *) {
        ++messages;
        throw std::bad_alloc();
    });
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->Established();
    WriteRequest(peer.fd, "request");
    if (buffer_failure)
        write_failure = 2;
    bool caught = false;
    try
    {
        Step(loop);
    }
    catch (const std::bad_alloc &)
    {
        caught = true;
    }
    assert(caught && closed == 0 && fcntl(fd, F_GETFD) >= 0);
    assert(messages == (buffer_failure ? 0 : 1));
    // 注入异常不等于真实内存耗尽；由上层明确决定关闭并执行清理。
    conn->ShutDown();
    assert(fcntl(fd, F_GETFD) >= 0);
    Step(loop);
    assert(closed == 1 && conn->GetFd() == -1);
    errno = 0;
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}
void BufferWriteFailureClosesConnection()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 5, peer_fd);
    Peer peer{peer_fd};
    int messages = 0;
    int closed = 0;
    int removed = 0;
    conn->SetMessageCallback([&](const ConnectionPtr &, Buffer *) { ++messages; });
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) { ++removed; });
    conn->Established();
    WriteRequest(peer.fd, "already read from socket");
    write_failure = 1;
    Step(loop);
    assert(write_failure == 0);
    assert(messages == 0 && closed == 1 && removed == 1);
    assert(conn->GetFd() == -1 && !conn->IsConnected());
}

void CancelIdleTimeoutAndEnableAgain()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 6, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->Established();
    conn->EnableInactiveRelease(1);
    std::thread worker([conn] {
        conn->CancelInactiveRelease();
        conn->CancelInactiveRelease();
    });
    worker.join();
    Step(loop); // 先执行取消任务，再等待原定时器到期。
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Step(loop);
    assert(conn->IsConnected() && closed == 0);
    conn->EnableInactiveRelease(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Step(loop);
    assert(!conn->IsConnected() && conn->GetFd() == -1 && closed == 1);
    conn->CancelInactiveRelease();
}

void QueuedProtocolSwitchOwnsArguments()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 7, peer_fd);
    Peer peer{peer_fd};
    int established = 0, messages = 0, events = 0, closed = 0, removed = 0;
    conn->SetContext(std::string("old"));
    conn->SetConnectedCallback([&](const ConnectionPtr &) { ++established; });
    conn->SetMessageCallback([](const ConnectionPtr &, Buffer *) { assert(false); });
    conn->SetClosedCallback([](const ConnectionPtr &) { assert(false); });
    conn->SetAnyEventCallback([](const ConnectionPtr &) { assert(false); });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) { ++removed; });
    conn->Established();
    std::thread worker([&] {
        conn->SwitchProtocol(
            std::string(4096, 'p'),
            [&](const ConnectionPtr &) { ++established; },
            [&, tag = std::string(4096, 'm')](const ConnectionPtr &current, Buffer *buffer) {
                assert(tag == std::string(4096, 'm'));
                assert(std::any_cast<const std::string &>(current->GetContext()) == std::string(4096, 'p'));
                assert(buffer->ReadAsString(buffer->GetReadableSize()) == "new protocol");
                ++messages;
            },
            [&](const ConnectionPtr &) { ++closed; },
            [&](const ConnectionPtr &) { ++events; });
    });
    worker.join(); // 临时参数生命周期结束后，才执行排队的切换任务。
    assert(std::any_cast<const std::string &>(conn->GetContext()) == "old");
    Step(loop);
    assert(std::any_cast<const std::string &>(conn->GetContext()) == std::string(4096, 'p'));
    assert(established == 1);
    WriteRequest(peer.fd, "new protocol");
    Step(loop);
    assert(messages == 1 && events > 0);
    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && removed == 1);
}

void SwitchInsideMessagePreservesCallbackAndUnreadBytes()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 8, peer_fd);
    Peer peer{peer_fd};
    int old_messages = 0, new_messages = 0;
    conn->SetMessageCallback(
        [&, captured = std::string(8192, 's')](const ConnectionPtr &current, Buffer *buffer) {
            ++old_messages;
            assert(buffer->ReadAsString(3) == "old");
            current->SwitchProtocol(
                std::string("new"), {},
                [&](const ConnectionPtr &, Buffer *next) {
                    ++new_messages;
                    assert(next->ReadAsString(next->GetReadableSize()) == "retained+next");
                }, {}, {});
            // 切换立即生效，但不能销毁当前正在执行的回调对象。
            assert(std::any_cast<const std::string &>(current->GetContext()) == "new");
            assert(captured == std::string(8192, 's'));
            assert(buffer->GetReadableSize() == 8);
        });
    conn->Established();
    WriteRequest(peer.fd, "oldretained");
    Step(loop);
    assert(old_messages == 1 && new_messages == 0);
    WriteRequest(peer.fd, "+next");
    Step(loop);
    assert(old_messages == 1 && new_messages == 1);
    conn->ShutDown();
    Step(loop);
    assert(conn->GetFd() == -1);
}

void ConstructorRejectsInvalidArguments()
{
    EventLoop loop;
    const int fds_before = CountOpenFds();

    {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
        Peer peer{fds[1]};

        // loop 为空：在参数校验阶段就被拒绝。
        bool null_loop = false;
        try
        {
            std::make_shared<Connection>(nullptr, 10, fds[0]);
        }
        catch (const std::invalid_argument &)
        {
            null_loop = true;
        }
        assert(null_loop);

        // 构造失败时已构造完成的成员会析构，描述符不会泄漏。
        errno = 0;
        assert(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);
    }

    // fd 为负：同样在校验阶段被拒绝，此时没有需要回收的描述符。
    bool bad_fd = false;
    try
    {
        std::make_shared<Connection>(&loop, 11, -1);
    }
    catch (const std::invalid_argument &)
    {
        bad_fd = true;
    }
    assert(bad_fd);

    assert(CountOpenFds() == fds_before);
}

void ConstructorPropagatesNonBlockFailure()
{
    EventLoop loop;
    const int fds_before = CountOpenFds();

    // O_PATH 描述符由本用例真实持有，生命周期干净：F_GETFL 可用而 F_SETFL 返回
    // EBADF，正好命中 SetNonBlock 的失败条件。先把前提固定下来，
    // 避免内核行为变化后用例静默失去检测力。
    const int path_fd = open("/", O_PATH | O_CLOEXEC);
    assert(path_fd >= 0);
    assert(fcntl(path_fd, F_GETFL, 0) != -1);
    errno = 0;
    assert(fcntl(path_fd, F_SETFL, O_NONBLOCK) == -1 && errno == EBADF);

    bool rejected = false;
    try
    {
        std::make_shared<Connection>(&loop, 12, path_fd);
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    assert(rejected);

    // 构造失败后 Socket 成员析构已关闭该描述符，此处不能再关一次。
    errno = 0;
    assert(fcntl(path_fd, F_GETFD) == -1 && errno == EBADF);
    assert(CountOpenFds() == fds_before);
}

void EnableInactiveReleaseRejectsZeroTimeout()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 13, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->Established();

    // 参数校验发生在 shared_from_this 与 RunInLoop 之前，
    // 抛错时不可能已经登记定时任务，因此无需等待即可断言。
    bool rejected = false;
    try
    {
        conn->EnableInactiveRelease(0);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    assert(rejected);
    assert(conn->IsConnected() && closed == 0);

    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && conn->GetFd() == -1);
}

// 参数类型从 uint32_t 回退到 int 的回归用例。在 uint32_t 下 -1 会回绕成
// 4294967295，timeout <= 0 无感通过，再被 TimerWheel::NormalizeTimeout 静默
// 钳位到容量 60。上面只测 0 的用例恰好落在唯一仍然有效的路径上，抓不到它。
void EnableInactiveReleaseRejectsNegativeTimeout()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 30, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->Established();

    bool rejected = false;
    try
    {
        conn->EnableInactiveRelease(-1);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    assert(rejected);
    assert(conn->IsConnected() && closed == 0);

    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && conn->GetFd() == -1);
}

void EstablishedIsIdempotent()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 14, peer_fd);
    Peer peer{peer_fd};
    int established = 0;
    conn->SetConnectedCallback([&](const ConnectionPtr &) { ++established; });

    conn->Established();
    assert(established == 1);

    // 状态已不是 CONNECTING，重复建立不再通知。
    conn->Established();
    Step(loop);
    assert(established == 1 && conn->IsConnected());

    // 关闭之后再建立同样被拒绝，且不能把状态改回已连接。
    conn->ShutDown();
    Step(loop);
    assert(!conn->IsConnected() && conn->GetFd() == -1);
    conn->Established();
    Step(loop);
    assert(established == 1 && !conn->IsConnected());
}

void ConnectedCallbackFailureClosesConnection()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 15, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    int removed = 0;
    conn->SetConnectedCallback([&](const ConnectionPtr &) {
        throw std::runtime_error("connected callback failure");
    });
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) { ++removed; });

    // 异常在 EstablishedInLoop 内部被捕获并安排释放，本轮结束前执行。
    conn->Established();
    Step(loop);
    assert(closed == 1 && removed == 1);
    assert(conn->GetFd() == -1 && !conn->IsConnected());
}

void ConnectedCallbackBadAllocPropagates()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 16, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    const int fd = conn->GetFd();
    conn->SetConnectedCallback([&](const ConnectionPtr &) { throw std::bad_alloc(); });
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });

    bool caught = false;
    try
    {
        conn->Established();
    }
    catch (const std::bad_alloc &)
    {
        caught = true;
    }
    // bad_alloc 原样向上传播，清理交由上层决定。
    assert(caught && closed == 0);
    assert(fcntl(fd, F_GETFD) >= 0);

    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && conn->GetFd() == -1);
}

void AnyEventCallbackFailureClosesConnection()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 17, peer_fd);
    Peer peer{peer_fd};
    int messages = 0;
    int events = 0;
    int closed = 0;
    int removed = 0;
    conn->SetMessageCallback([&](const ConnectionPtr &, Buffer *) { ++messages; });
    conn->SetAnyEventCallback([&](const ConnectionPtr &) {
        ++events;
        throw std::runtime_error("event callback failure");
    });
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) { ++removed; });
    conn->Established();
    WriteRequest(peer.fd, "event");

    // 异常由 HandleEvent 内部捕获，不会穿透 LoopOnce()；此处刻意不包 try/catch，
    // 一旦该捕获被破坏，异常逃逸本身就是可定位的失败。
    Step(loop);
    // 抛错后已安排释放，后续事件被 HandleEvent 的守卫拦下。
    assert(messages == 1 && events == 1);
    assert(closed == 1 && removed == 1);
    assert(conn->GetFd() == -1 && !conn->IsConnected());
}

void SendBeforeEstablishedIsDropped()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 18, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });

    // 尚未建立连接：发送必须被丢弃。刻意在 Send 之后才建立，
    // 否则写回调尚未安装，即使该守卫被删除也观察不到数据外发。
    conn->Send("before established");
    conn->Established();
    Step(loop);
    std::string received;
    assert(!ReadAvailable(peer.fd, received) && received.empty());

    // 已经释放：发送同样不得产生副作用。
    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && conn->GetFd() == -1);
    conn->Send("after release");
    Step(loop);
    received.clear();
    assert(ReadAvailable(peer.fd, received) && received.empty());
    assert(closed == 1);
}

void DestructorUnregistersUnclosedConnection()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 19, peer_fd);
    Peer peer{peer_fd};
    conn->Established();
    const int old_fd = conn->GetFd();
    std::weak_ptr<Connection> weak = conn;

    // 不调用 ShutDown，直接放弃最后一份长期持有：析构必须兜底注销登记并关闭描述符。
    conn.reset();
    assert(weak.expired());

    std::string received;
    assert(ReadAvailable(peer.fd, received) && received.empty());

    // 让 old_fd 这个号码重新成为有效描述符，再用同一个号码登记新的 Channel。
    // 若析构漏了注销，残留表项会让这次登记失败——这是与地址无关的探测：
    // 无论残留表项存的是失效指针还是恰好被复用地址的新指针，EnableRead 都不会成功。
    const int probe_src = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(probe_src >= 0);
    if (probe_src != old_fd)
    {
        assert(dup2(probe_src, old_fd) == old_fd);
        close(probe_src);
    }

    Channel probe(old_fd, &loop);
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
    close(old_fd);
}

void ShutDownBeforeEstablishedCloses()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 20, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    int removed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) { ++removed; });

    // 从未 Established：Channel 未登记，关闭路径的两处 _registered 判断都走假分支。
    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && removed == 1);
    assert(conn->GetFd() == -1 && !conn->IsConnected());

    std::string received;
    assert(ReadAvailable(peer.fd, received) && received.empty());
}

void CloseCallbackFailuresAreAggregated()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 21, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    int removed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) {
        ++closed;
        throw std::runtime_error("closed callback failure");
    });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) {
        ++removed;
        throw std::runtime_error("server closed callback failure");
    });
    conn->Established();
    conn->ShutDown();

    // 两个通知都得到执行，普通异常只记录不外抛。
    Step(loop);
    assert(closed == 1 && removed == 1);
    assert(conn->GetFd() == -1);
}

void ServerClosedBadAllocPropagates()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 22, peer_fd);
    Peer peer{peer_fd};
    int closed = 0;
    int removed = 0;
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->SetServerClosedCallback([&](const ConnectionPtr &) {
        ++removed;
        throw std::bad_alloc();
    });
    conn->Established();
    conn->ShutDown();

    bool caught = false;
    try
    {
        Step(loop);
    }
    catch (const std::bad_alloc &)
    {
        caught = true;
    }
    // bad_alloc 会重抛，但两个通知都已执行，资源也已清理完毕。
    assert(caught && closed == 1 && removed == 1);
    assert(conn->GetFd() == -1);
}

void SwitchProtocolRespectsLifecycle()
{
    EventLoop loop;
    int peer_fd;
    auto conn = MakeConnection(loop, 23, peer_fd);
    Peer peer{peer_fd};
    int new_messages = 0;
    int closed = 0;

    // 1.CONNECTING 阶段允许切换：建立连接后应使用新的消息回调。
    // 切换会整体替换四个业务回调，因此关闭回调必须在切换之后再注册。
    conn->SwitchProtocol(std::string("pending"), {}, [&](const ConnectionPtr &, Buffer *) { ++new_messages; },
                         {}, {});
    conn->SetClosedCallback([&](const ConnectionPtr &) { ++closed; });
    conn->Established();
    WriteRequest(peer.fd, "hello");
    Step(loop);
    assert(new_messages == 1);

    // 2.释放之后切换必须被忽略，上下文不能被改写。
    conn->ShutDown();
    Step(loop);
    assert(closed == 1 && conn->GetFd() == -1);

    conn->SwitchProtocol(std::string("after release"), {}, [&](const ConnectionPtr &, Buffer *) { ++new_messages; },
                         {}, {});
    Step(loop);
    assert(std::any_cast<const std::string &>(conn->GetContext()) == "pending");
    assert(new_messages == 1);
}
} // namespace

int main()
{
    ArmWatchdog();
    try
    {
        MessageFailureIsIsolated(false);
        MessageFailureIsIsolated(true);
        PeerEofDrainsResponse();
        BufferWriteFailureClosesConnection();
        AllocationFailurePropagates(false);
        AllocationFailurePropagates(true);
        CancelIdleTimeoutAndEnableAgain();
        QueuedProtocolSwitchOwnsArguments();
        SwitchInsideMessagePreservesCallbackAndUnreadBytes();

        ConstructorRejectsInvalidArguments();
        ConstructorPropagatesNonBlockFailure();
        EnableInactiveReleaseRejectsZeroTimeout();
        EnableInactiveReleaseRejectsNegativeTimeout();
        EstablishedIsIdempotent();
        ConnectedCallbackFailureClosesConnection();
        ConnectedCallbackBadAllocPropagates();
        AnyEventCallbackFailureClosesConnection();
        SendBeforeEstablishedIsDropped();
        DestructorUnregistersUnclosedConnection();
        ShutDownBeforeEstablishedCloses();
        CloseCallbackFailuresAreAggregated();
        ServerClosedBadAllocPropagates();
        SwitchProtocolRespectsLifecycle();

        std::cout << "Connection tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        // 异常逃逸型变异的主要表现：不让它变成 std::terminate，
        // 否则失败既不可定位，也无法与断言失败区分。
        std::cerr << "Connection tests failed: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
