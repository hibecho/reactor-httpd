#include "Acceptor.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Logger.hpp"
#include "Socket.hpp"
#include <cassert>
#include <exception>
#include <new>
#include <stdexcept>
#include <unistd.h>
#include <utility>

Acceptor::Acceptor(EventLoop *loop, uint16_t port)
    : _listener(std::make_unique<Socket>())
    , _loop(loop)
{
    if (!_loop)
        throw std::invalid_argument("Acceptor requires a loop");
    if (!_loop->IsInLoopThread())
        throw std::logic_error("Acceptor requires the loop thread");

    // 创建、配置并启动监听。
    if (!_listener->CreateServer(port))
        throw std::runtime_error("Acceptor: CreateServer failed");

    _channel = std::make_unique<Channel>(_listener->GetFd(), _loop);
    _channel->SetReadCallback([this] { HandleRead(); });
    _channel->EnableRead();
}

Acceptor::~Acceptor()
{
    assert(_loop->IsInLoopThread());
    // 监听 fd 关闭前先注销事件。
    _channel->Remove();
}

void Acceptor::SetAcceptCallback(AcceptCallback cb)
{
    if (!_loop->IsInLoopThread())
        throw std::logic_error("Acceptor callback requires the loop thread");
    _accept_callback = std::move(cb);
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

        DispatchAcceptedFd(fd);
    }
}

void Acceptor::DispatchAcceptedFd(int fd)
{
    bool handed_off = false;
    try
    {
        // 保留副本，允许回调替换自身。
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
        throw;
    }
    catch (const std::exception &e)
    {
        if (!handed_off)
            close(fd);
        LOG_ERROR("Acceptor accept callback: {}", e.what());
    }
    catch (...)
    {
        if (!handed_off)
            close(fd);
        LOG_ERROR("Acceptor accept callback: {}", "unknown exception");
    }
}
