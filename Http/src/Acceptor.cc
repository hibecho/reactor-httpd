#include "Acceptor.hpp"
#include "Logger.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Socket.hpp"
#include <cassert>
#include <exception>
#include <new>
#include <stdexcept>
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

Acceptor::Acceptor(EventLoop *loop, uint16_t port)
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
    if (!_listener->CreateServer(port))
        throw std::runtime_error("Acceptor: CreateServer failed");

    // 实例化channel对象，进行监听事件的管理
    _channel = std::make_unique<Channel>(_listener->GetFd(), _loop);

    // 设定读事件：Accept，接收新连接
    _channel->SetReadCallback([this] {
        HandleRead();
    });
}

Acceptor::~Acceptor()
{
    assert(_loop->IsInLoopThread());
    // 监听 fd 关闭前先注销事件。
    // 从未 StartAccepting() 的对象没有登记过，注销会抛"未登记"异常并逃出析构函数。
    if (_accepting)
        _channel->Remove();
}

void Acceptor::StartAccepting()
{
    // 注册读事件，允许 EventLoop 调用 HandleRead
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
    while (true)
    {
        const int fd = _listener->Accept();
        if (fd < 0)
        {
            // Socket 已处理 EINTR 和错误日志；资源耗尽时这里不提供退避。
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
