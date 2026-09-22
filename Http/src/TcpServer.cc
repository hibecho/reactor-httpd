#include "TcpServer.hpp"
#include "Acceptor.hpp"
#include "Connection.hpp"
#include "EventLoop.hpp"
#include "LoopThreadPool.hpp"
#include "TimerQueue.hpp"
#include <assert.h>
#include <limits>
#include <stdexcept>
#include <utility>

TcpServer::TcpServer(uint16_t port)
    : _port(port)
    , _baseloop(std::make_unique<EventLoop>())
    , _acceptor(std::make_unique<Acceptor>(_baseloop.get(), _port))
    , _pool(std::make_unique<LoopThreadPool>(_baseloop.get()))
{
}

TcpServer::~TcpServer() = default;

// 设置线程数量
void TcpServer::SetThreadCount(int count)
{
    _pool->SetThreadCount(count);
}

uint64_t TcpServer::AllocateId()
{
    uint64_t current = _next_id.load(std::memory_order_relaxed);
    for (;;)
    {
        if (current == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("server ID exhausted");
        if (_next_id.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return current + 1;
    }
}

// 启动服务器
void TcpServer::Start()
{
    // 创建从属线程
    _pool->Create();
    // 为监听套接字注册回调方法
    _acceptor->SetAcceptCallback([this](int fd) {
        NewConnection(fd);
    });
    // 开启接收连接的事件监控
    _acceptor->StartAccepting();
    // 执行事件路由
    _baseloop->Loop();
}

// 为新连接构造一个Connection对象进行管理
void TcpServer::NewConnection(int fd)
{
    // 为每个连接分配id
    const uint64_t conn_id = AllocateId();
    ConnectionPtr conn = std::make_shared<Connection>(_pool->NextLoop(), conn_id, fd);

    // 设置回调事件
    conn->SetConnectedCallback(_connected_callback);
    conn->SetMessageCallback(_message_callback);
    conn->SetAnyEventCallback(_event_callback);
    conn->SetClosedCallback(_closed_callback);
    conn->SetServerClosedCallback([this](const ConnectionPtr &closed_conn) {
        RemoveConnection(closed_conn);
    });

    // 放入哈希表由主EventLoop对所有连接的管理
    _conns.emplace(conn_id, conn);

    // 是否启动非活跃连接销毁
    if (_enable_inactive_release)
        conn->EnableInactiveRelease(_inactive_timeout);

    conn->Established();
}

// 从管理Connection的_conns进行移除连接
void TcpServer::RemoveConnection(const ConnectionPtr &conn)
{
    const uint64_t id = conn->GetConnetId();
    _baseloop->RunInLoop([this, id]() {
        RemoveConnectionInLoop(id);
    });
}

void TcpServer::RemoveConnectionInLoop(uint64_t id)
{
    assert(_baseloop->IsInLoopThread());
    auto it = _conns.find(id);
    if (it != _conns.end())
    {
        _conns.erase(id);
    }
}

// 设置连接回调
void TcpServer::SetConnectedCallback(ConnectedCallback cb)
{
    _connected_callback = std::move(cb);
}

// 设置消息处理回调
void TcpServer::SetMessageCallback(MessageCallback cb)
{
    _message_callback = std::move(cb);
}

// 设置关闭回调
void TcpServer::SetClosedCallback(ClosedCallback cb)
{
    _closed_callback = std::move(cb);
}

// 设置任意事件回调
void TcpServer::SetAnyEventCallback(AnyEventCallback cb)
{
    _event_callback = std::move(cb);
}

// 设置是否启用超时销毁
void TcpServer::EnableInactiveRelease(int timeout)
{
    _inactive_timeout = timeout;
    _enable_inactive_release = true;
}

// 设置定时任务
// delay：延迟时间限定在[0,60]之间，单位为s。
uint64_t TcpServer::RunAfter(task_t task, int delay)
{
    if (!task)
        throw std::invalid_argument("RunAfter requires a task");

    if (delay <= 0 || delay > 60)
        throw std::invalid_argument("delay must be in [1, 60] ticks");
    const uint64_t id = AllocateId();
    _baseloop->QueueInLoop([this, id, task = std::move(task), delay]() mutable {
        RunAfterInLoop(id, std::move(task), delay);
    });
    return id;
}

void TcpServer::RunAfterInLoop(uint64_t id, task_t task, int delay)
{
    _baseloop->TimerAdd(id, static_cast<uint32_t>(delay), std::move(task));
}

void TcpServer::CancelTask(uint64_t timer_id)
{
    // 始终排队，避免主循环线程取消时越过尚未执行的添加请求。
    _baseloop->QueueInLoop([this, timer_id]() {
        _baseloop->TimerCancel(timer_id);
    });
}
