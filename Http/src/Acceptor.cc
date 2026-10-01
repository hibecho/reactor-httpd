#include "tcp/Acceptor.hpp"
#include "base/FdGuard.hpp"
#include "base/Logger.hpp"
#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"
#include "tcp/Socket.hpp"
#include <cassert>
#include <cerrno>
#include <exception>
#include <new>
#include <stdexcept>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>

/**
 * @brief Construct a new Acceptor:: Acceptor object
 *
 * @param loop
 * @param port
 *
 * 流程阶段

   监听套接字可读
        ↓
    Acceptor::HandleRead()
        ↓
    accept() 得到新连接 fd
        ↓
    DispatchAcceptedFd(fd)
        ↓
    执行这里注册的 lambda
        ↓
    TcpServer::NewConnection(fd)
        ↓
    创建 Connection、设置回调、保存并启动连接
 */

Acceptor::Acceptor(EventLoop *loop, uint16_t port, const std::string &ip)
    : _listener(std::make_unique<Socket>())
    , _loop(loop)
{
    // 异常处理
    if (!_loop)
        throw std::invalid_argument("Acceptor requires a loop");
    if (!_loop->IsInLoopThread())
        throw std::logic_error("Acceptor requires the loop thread");

    // 创建 socket + 绑定端口 + listen
    // 内部已经设定sockfd
    if (!_listener->CreateServer(port, ip))
        throw std::runtime_error("Acceptor: CreateServer failed");

    // 实例化channel对象，进行监听事件的管理
    _channel = std::make_unique<Channel>(_listener->GetFd(), _loop);

    // 设定读事件：Accept，接收新连接
    _channel->SetReadCallback([this] {
        HandleRead();
    });

    const int retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (retry_fd < 0)
        throw std::system_error(errno, std::generic_category(), "Acceptor timerfd_create");
    // 从这一行起 fd 由成员负责关闭：后面任何一步抛异常都不会泄漏，不需要手写 try/catch。
    _retry_fd = FdGuard(retry_fd);
    _retry_channel = std::make_unique<Channel>(retry_fd, _loop);
    _retry_channel->SetReadCallback([this] {
        HandleRetry();
    });
}

Acceptor::~Acceptor()
{
    assert(_loop->IsInLoopThread());
    StopAccepting();
}

void Acceptor::StartAccepting()
{
    if (!_loop->IsInLoopThread())
        throw std::logic_error("Acceptor start requires the loop thread");
    if (_stopped)
        throw std::logic_error("Acceptor cannot restart after stop");
    // 已启动但正在退避时也不可绕过定时恢复。
    if (_registered)
        return;
    _retry_channel->EnableRead();
    _retry_registered = true;
    try
    {
        _channel->EnableRead();
        _registered = true;
        _accepting = true;
    }
    catch (...)
    {
        _retry_channel->Remove();
        _retry_registered = false;
        throw;
    }
}

void Acceptor::StopAccepting()
{
    if (!_loop->IsInLoopThread())
        throw std::logic_error("Acceptor stop requires the loop thread");
    _stopped = true;
    _accepting = false;
    if (_registered)
    {
        _channel->Remove();
        _registered = false;
    }
    if (_retry_registered)
    {
        _retry_channel->Remove();
        _retry_registered = false;
    }
    _listener->Close();
    // Close 可重复调用，StopAccepting 允许被多次进入。
    _retry_fd.Close();
}

void Acceptor::PauseAccepting()
{
    _channel->DisableRead();
    _accepting = false;
    itimerspec delay{};
    delay.it_value.tv_nsec = 100 * 1000 * 1000; // 100 ms，使用独立的单调时钟。
    if (timerfd_settime(_retry_fd.GetFd(), 0, &delay, nullptr) < 0)
    {
        const int error = errno;
        StopAccepting();
        throw std::system_error(error, std::generic_category(), "Acceptor timerfd_settime");
    }
}

void Acceptor::HandleRetry()
{
    // Stop 可能由同轮排在前面的事件发起；此时 fd 已关闭，不能再读或重新登记。
    if (_stopped)
        return;
    uint64_t expirations = 0;
    ssize_t count;
    do
    {
        count = read(_retry_fd.GetFd(), &expirations, sizeof(expirations));
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (count != static_cast<ssize_t>(sizeof(expirations)))
        throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Acceptor timerfd read");
    _channel->EnableRead();
    _accepting = true;
}

int Acceptor::GetListenFd() const noexcept
{
    return _listener->GetFd();
}

void Acceptor::HandleRead()
{
    assert(_loop->IsInLoopThread());
    // LT 会在下一轮继续报告未处理连接；有限预算让停止任务和定时事件得到执行机会。
    constexpr unsigned kAcceptBudget = 64;
    for (unsigned accepted = 0; _accepting && accepted < kAcceptBudget; ++accepted)
    {
        const int fd = _listener->Accept();
        if (fd < 0)
        {
            // Socket 已处理 EINTR、保留 errno；资源耗尽时禁读避免 LT 忙轮询。
            if (errno == EMFILE || errno == ENFILE)
                PauseAccepting();
            return;
        }
        // 执行_accept_callback回调函数
        DispatchAcceptedFd(fd);
    }
}

void Acceptor::SetAcceptCallback(AcceptCallback cb)
{
    if (!_loop->IsInLoopThread())
        throw std::logic_error("Acceptor callback requires the loop thread");
    _accept_callback = std::move(cb);
}

/**
 * @brief
 *  将取出的新连接fd进行执行_accept_callback回调，在回调函数中分配Connection
 * @param fd
 *
 * 执行流程：
 *
 *  准备回调
 *       ↓
 *   没有回调 → 关闭 fd
 *       ↓
 *   有回调 → 交出 fd，执行回调
 *       ↓
 *   出现异常 → 只清理自己仍负责的资源
 *
 */
void Acceptor::DispatchAcceptedFd(int fd)
{
    // handed_off 标记管理责任是否已经交出去
    // 调用回调之前：Acceptor 负责 fd
    // 调用回调开始：回调接管 fd
    bool handed_off = false;
    try
    {
        // 为了支持回调执行期间重新设置或清空 _accept_callback
        // 回调内部可能再次调用：  acceptor.SetAcceptCallback(...);

        auto callback = _accept_callback;

        if (!callback)
        {
            close(fd);
            return;
        }

        // 从调用开始由回调管理 fd，异常时不能重复关闭。
        handed_off = true;
        callback(fd);
    }
    catch (const std::bad_alloc &)
    {
        if (!handed_off)
            close(fd);
        LOG_ERROR("bad_alloc");
        throw;
    }
    catch (const std::exception &e)
    {
        // 清理尚未交出的 fd，记录具体错误
        if (!handed_off)
            close(fd);
        LOG_ERROR("Acceptor accept callback: {}", e.what());
    }
    catch (...)
    {
        // 清理尚未交出的 fd，记录未知错误
        if (!handed_off)
            close(fd);
        LOG_ERROR("Acceptor accept callback: {}", "unknown exception");
    }
}
