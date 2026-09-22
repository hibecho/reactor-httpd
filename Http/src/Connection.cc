#include "tcp/Connection.hpp"
#include "base/Buffer.hpp"
#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"
#include "base/Logger.hpp"
#include "tcp/Socket.hpp"
#include <cassert>
#include <cerrno>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

Connection::Connection(EventLoop *loop, uint64_t id, int fd)
    : _conn_id(id)
    , _timer_id(id)
    , _sockfd(fd)
    , _status(ConnStatus::CONNECTING)
    , _loop(loop)
    , enable_inactive_release(false)
    , _socket(std::make_unique<Socket>(fd))
    , _channel(std::make_unique<Channel>(fd, loop))
    , _in_buffer(std::make_unique<Buffer>())
    , _out_buffer(std::make_unique<Buffer>())
{
    if (!loop || fd < 0)
        throw std::invalid_argument("Connection requires a loop and valid fd");
    if (!_socket->SetNonBlock())
        throw std::runtime_error("Connection: SetNonBlock failed");
}

Connection::~Connection()
{
    // 正常路径已在 ReleaseInLoop 中注销；这里兜底注销未正常关闭的连接。
    // 所属 EventLoop 必须仍然存活，且析构需发生在所属线程。
    if (_registered)
        _channel->Remove();
}

int Connection::GetFd()
{
    return _socket->GetFd();
}

uint64_t Connection::GetConnetId()
{
    return _conn_id;
}

uint64_t Connection::GetTimerId()
{
    return _timer_id;
}

bool Connection::IsConnected()
{
    return _status == ConnStatus::CONNECTED;
}

void Connection::SetContext(std::any context)
{
    _context = std::move(context);
}

std::any &Connection::GetContext() noexcept
{
    return _context;
}

const std::any &Connection::GetContext() const noexcept
{
    return _context;
}

void Connection::SetConnectedCallback(ConnectedCallback cb)
{
    _connected_callback = std::move(cb);
}

void Connection::SetMessageCallback(MessageCallback cb)
{
    _message_callback = std::move(cb);
}

void Connection::SetClosedCallback(ClosedCallback cb)
{
    _closed_callback = std::move(cb);
}

void Connection::SetServerClosedCallback(ClosedCallback cb)
{
    _server_closed_callback = std::move(cb);
}

void Connection::SetAnyEventCallback(AnyEventCallback cb)
{
    _event_callback = std::move(cb);
}

void Connection::Established()
{
    auto self = shared_from_this();
    _loop->RunInLoop([self] {
        self->EstablishedInLoop();
    });
}

void Connection::EstablishedInLoop()
{
    assert(_loop->IsInLoopThread());
    if (_status != ConnStatus::CONNECTING)
        return;

    // 服务器连接表负责保活；回调不强持有自己，避免循环引用。
    _channel->SetReadCallback([this] {
        HandleRead();
    });
    _channel->SetWriteCallback([this] {
        HandleWrite();
    });
    _channel->SetCloseCallback([this] {
        HandleClose();
    });
    _channel->SetErrorCallback([this] {
        HandleError();
    });
    _channel->SetEventCallback([this] {
        HandleEvent();
    });

    _channel->EnableRead();
    _registered = true;
    _status = ConnStatus::CONNECTED;

    try
    {
        // 回调可在执行中切换协议，保留当前函数对象直到调用返回。
        auto callback = _connected_callback;
        if (callback)
            callback(shared_from_this());
    }
    catch (const std::bad_alloc &)
    {
        throw;
    }
    catch (...)
    {
        LOG_ERROR("Connection {}: {}", "connected callback", "exception; closing connection");
        HandleError();
    }
}

void Connection::HandleRead()
{
    if (_status != ConnStatus::CONNECTED)
        return;
    auto self = shared_from_this();
    char buffer[65536];
    memset(buffer, 0, sizeof(buffer));

    // 在LT模式下，只要缓冲区可读，会一直触发读取事件
    // 无需单向数据读取不完整
    const ssize_t received = _socket->NonBlockRecv(buffer, sizeof(buffer));

    // received > 0 :读取成功
    if (received > 0)
    {
        try
        {
            _in_buffer->Write(buffer, static_cast<std::size_t>(received));
            auto callback = _message_callback;
            if (callback && _in_buffer->GetReadableSize() > 0)
                callback(self, _in_buffer.get());
        }
        catch (const std::bad_alloc &)
        {
            // 内存不足属于上层故障策略；不能假定还能分配清理任务。
            // 缓冲区扩容异常
            throw;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Connection {}: {}", "read/message", e.what());
            HandleError();
        }
        catch (...)
        {
            LOG_ERROR("Connection {}: {}", "read/message", "unknown exception");
            HandleError();
        }
        return;
    }

    // 对端结束发送；先排空待发送数据，再关闭。
    if (received == 0)
    {
        ShutDownInLoop();
        return;
    }

    // received < 0 的情况
    // - 因为缓冲区暂时没有数据可读
    // - 其他异常错误
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK)
        return;
    HandleError();
}

void Connection::Send(std::string data)
{
    auto self = shared_from_this();
    _loop->RunInLoop([self, data = std::move(data)]() mutable {
        self->SendInLoop(std::move(data));
    });
}

void Connection::SendInLoop(std::string data)
{
    /*防御性编程*/
    assert(_loop->IsInLoopThread());
    if (_status != ConnStatus::CONNECTED)
        return;

    // 1.数据进入输出缓冲区
    _out_buffer->WriteString(data);

    // 2.开启写监控
    if (_out_buffer->GetReadableSize() != 0)
        _channel->EnableWrite();
}

void Connection::HandleWrite()
{
    // 防御性编程
    if (_release_pending || _released)
        return;

    // 获取可以发送的数据
    const std::size_t size = _out_buffer->GetReadableSize();
    if (size != 0)
    {
        // 在LT模式下，只要缓冲区有数据会一直通知
        // 无需担心数据一次发送不完全
        const ssize_t sent = _socket->NonBlockSend(_out_buffer->GetReadPosition(), size);

        // sent < 0
        // - 因为 EAGAIN 和 EWOULDBLOCK，暂时不可写导致sent=-1
        // - 因为 其他错误，导致sent =-1
        if (sent < 0)
        {
            // 情况一：因为 EAGAIN 和 EWOULDBLOCK，暂时不可写导致sent=-1
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK)
                return; // 暂时不可写，保留数据，等待下次可写事件。

            // 情况二：因为 其他错误，导致sent =-1
            HandleError(); // 其他发送错误，安排关闭连接。
            return;
        }

        // 正常情况下不会发生，提前判断了可以发送的数据不为0
        if (sent == 0)
        {
            // 非空发送没有取得进展，按异常关闭。
            HandleError();
            return;
        }

        // sent > 0：消费本次成功发送的数据，剩余数据留待后续发送。
        _out_buffer->MoveReadOffset(static_cast<std::size_t>(sent));
    }

    // 缓冲区无数据，关闭写事件
    // 防止在LT模式下，只要内核发送缓冲区非空，会一直触发写事件导致空转CPU
    if (_out_buffer->GetReadableSize() == 0)
    {
        // 数据发完全，且当前处于正在关闭状态，调用关闭
        // - 输出缓冲区为空。
        // - 连接已经请求关闭。
        _channel->DisableWrite();
        if (_status == ConnStatus::DISCONNETING)
            HandleClose();
    }
}

void Connection::ShutDown()
{
    // 这里没有立即关闭套接字，而是保证后续操作在连接所属的线程执行：
    //- 当前就是所属线程：立即执行 ShutDownInLoop()。
    //- 当前是其他线程：任务进入所属 EventLoop 的队列。
    auto self = shared_from_this();
    _loop->RunInLoop([self] {
        self->ShutDownInLoop();
    });
}

void Connection::ShutDownInLoop()
{
    /*防御编程*/
    assert(_loop->IsInLoopThread());
    if (_release_pending || _released)
        return;

    // 1.将状态设置为正在关闭，还没有完成关闭。
    _status = ConnStatus::DISCONNETING;

    // 2.取消读事件监控，不再继续读取请求。
    if (_registered)
        _channel->DisableRead();

    // 3.判断输出缓冲区是否还有数据
    //   如果还有数据需要等待缓冲区数据发送完后，在HandWirte函数接口中进行关闭。
    if (_out_buffer->GetReadableSize() == 0)
        HandleClose();
}

void Connection::HandleClose()
{
    // 正常关闭最终也走同一个延迟释放入口。
    // HandleError() 实际承担了“安排释放”的公共职责。
    HandleError();
}

void Connection::HandleError()
{
    // 如果已经安排了释放，或者已经释放完成，就直接返回，不再重复处理。
    assert(_loop->IsInLoopThread());
    if (_release_pending || _released)
        return;

    // 1. QueueInLoop 而非 RunInLoop：Channel::Handle 还可能继续分发本轮事件。
    //    延迟实际清理：等本轮事件分发结束，才执行 ReleaseInLoop()。
    auto self = shared_from_this();
    _loop->QueueInLoop([self] {
        self->ReleaseInLoop();
    });

    // 2. 安排延迟释放
    _release_pending = true;

    // 3.设置状态为已关闭
    _status = ConnStatus::DISCONNECTED;
}

void Connection::ReleaseInLoop()
{

    assert(_loop->IsInLoopThread());
    if (_released)
        return;

    auto self = shared_from_this();
    // 1.注销 Channel
    if (_registered)
    {
        _channel->Remove(); // 必须先注销，再关闭 fd。
        _registered = false;
    }

    // 2.取消已启用的定时任务
    if (enable_inactive_release)
    {
        _loop->TimerCancel(_timer_id);
        enable_inactive_release = false;
    }

    // 3.关闭 Socket
    _socket->Close();
    _sockfd = -1;

    // 4.清空输入、输出缓冲区
    _in_buffer->Clear();
    _out_buffer->Clear();

    // 5.设置 _released = true
    _status = ConnStatus::DISCONNECTED;
    _released = true; // 通知前标记完成，回调重入不会再次释放。

    // 6.移出回调：通知中重新设置回调也不会销毁正在执行的函数对象。
    auto closed = std::move(_closed_callback);
    auto server_closed = std::move(_server_closed_callback);

    std::exception_ptr failure;
    std::exception_ptr server_failure;
    try
    {
        if (closed)
            closed(self);
    }
    catch (...)
    {
        failure = std::current_exception();
    }

    try
    {
        if (server_closed)
            server_closed(self);
    }
    catch (...)
    {
        server_failure = std::current_exception();
    }

    // 两个回调都得到执行机会后，再报告或传播异常。
    for (const auto &error : {failure, server_failure})
    {
        if (!error)
            continue;
        try
        {
            std::rethrow_exception(error);
        }
        catch (const std::bad_alloc &)
        {
            throw;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Connection {}: {}", "close callback", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Connection {}: {}", "close callback", "unknown exception");
        }
    }
}

void Connection::HandleEvent()
{
    if (_release_pending || _released)
        return;
    try
    {
        // 连接长时间没有活动，就自动关闭；
        // 只要有活动，就重新计算等待时间。
        if (enable_inactive_release)
            _loop->TimerRefresh(_timer_id);

        auto callback = _event_callback;
        if (callback)
            callback(shared_from_this());
    }
    catch (const std::bad_alloc &)
    {
        throw;
    }
    catch (...)
    {
        LOG_ERROR("Connection {}: {}", "event callback", "exception; closing connection");
        HandleError();
    }
}

void Connection::EnableInactiveRelease(int timeout)
{
    if (timeout <= 0)
        throw std::invalid_argument("空闲超时时间必须大于 0");

    auto self = shared_from_this();

    // 上面的校验已经排除非正值，这里的转换不会回绕。
    const uint32_t normalized = static_cast<uint32_t>(timeout);

    _loop->RunInLoop([self, normalized] {
        self->EnableInactiveReleaseInLoop(normalized);
    });
}

void Connection::EnableInactiveReleaseInLoop(uint32_t timeout)
{
    assert(_loop->IsInLoopThread());

    if (_release_pending || _released)
        return;

    std::weak_ptr<Connection> weak = shared_from_this();

    _loop->TimerAdd(_timer_id, timeout, [weak]() noexcept {
        ConnectionPtr conn = weak.lock();
        if (!conn)
            return; // 连接对象已经不存在。
        try
        {
            // 超时了：安排延迟释放，不再等待输出发送完。
            conn->HandleError();
        }
        catch (...)
        {
            // 当前时间轮在 TimerTask 析构中执行回调，
            // 不能让异常逃出析构函数。
            LOG_ERROR("Connection {}: {}", "idle timeout", "无法安排连接释放");
            std::terminate();
        }
    });

    enable_inactive_release = true;
}

void Connection::CancelInactiveRelease()
{
    auto self = shared_from_this();
    _loop->RunInLoop([self] {
        self->CancelInactiveReleaseInLoop();
    });
}

void Connection::CancelInactiveReleaseInLoop()
{
    assert(_loop->IsInLoopThread());
    if (!enable_inactive_release)
        return;

    // 必须取消任务本身，仅停止刷新仍会导致原任务到期关闭连接。
    _loop->TimerCancel(_timer_id);
    enable_inactive_release = false;
}

void Connection::SwitchProtocol(std::any context, ConnectedCallback conn, MessageCallback msg, ClosedCallback closed,
                                AnyEventCallback event)
{
    auto self = shared_from_this();
    // 任务自己拥有参数，调用方的局部变量和临时对象可以先销毁。
    _loop->RunInLoop([self, context = std::move(context), conn = std::move(conn), msg = std::move(msg),
                      closed = std::move(closed), event = std::move(event)]() mutable {
        self->SwitchProtocolInLoop(std::move(context), std::move(conn), std::move(msg), std::move(closed),
                                   std::move(event));
    });
}

void Connection::SwitchProtocolInLoop(std::any context, ConnectedCallback conn, MessageCallback msg,
                                      ClosedCallback closed, AnyEventCallback event)
{
    assert(_loop->IsInLoopThread());
    if (_status != ConnStatus::CONNECTING && _status != ConnStatus::CONNECTED)
        return;

    // 参数准备完成后再移动替换；这些移动赋值不抛异常。
    // 当前正在执行的回调由调用处的局部副本保护。
    _context = std::move(context);
    _connected_callback = std::move(conn);
    _message_callback = std::move(msg);
    _closed_callback = std::move(closed);
    _event_callback = std::move(event);
}