#include "tcp/TcpServer.hpp"
#include "base/Logger.hpp"
#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"
#include "tcp/Acceptor.hpp"
#include "tcp/Socket.hpp"
#include "thread/LoopThreadPool.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <exception>
#include <limits>
#include <pthread.h>
#include <stdexcept>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>

TcpServer::TcpServer(uint16_t port)
    : _port(port)
    , _baseloop(std::make_unique<EventLoop>())
    , _acceptor(std::make_unique<Acceptor>(_baseloop.get(), port))
    , _pool(std::make_unique<LoopThreadPool>(_baseloop.get()))
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (getsockname(_acceptor->GetListenFd(), reinterpret_cast<sockaddr *>(&address), &length) < 0)
        throw std::system_error(errno, std::generic_category(), "getsockname");
    _port = ntohs(address.sin_port);
}

TcpServer::~TcpServer()
{
    // 生命周期要求析构在构造线程，且 Start 已经返回。
    assert(_baseloop->IsInLoopThread());
    try
    {
        Stop();
        _baseloop->DrainPendingTasks();
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("server destructor cleanup: {}", error.what());
    }
    CleanupStopEvents();
}

void TcpServer::EnsureConfigurable() const
{
    if (!_baseloop->IsInLoopThread())
        throw std::logic_error("TcpServer must be configured on its constructing thread");
    std::lock_guard<std::mutex> lock(_budget->mutex);
    if (_state != State::Created)
        throw std::logic_error("TcpServer configuration is frozen");
}

void TcpServer::SetThreadCount(int count)
{
    EnsureConfigurable();
    _pool->SetThreadCount(count);
}

void TcpServer::SetResourceLimits(const ResourceLimits &limits)
{
    EnsureConfigurable();
    limits.Validate();
    std::lock_guard<std::mutex> lock(_budget->mutex);
    _budget->limits = limits;
}

void TcpServer::SetShutdownGrace(std::chrono::milliseconds grace)
{
    EnsureConfigurable();
    if (grace.count() < 0)
        throw std::invalid_argument("shutdown grace must be nonnegative");
    _shutdown_grace = grace;
}

void TcpServer::EnableSignalStop()
{
    EnsureConfigurable();
    _signal_stop = true;
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

void TcpServer::HandleException(std::exception_ptr error) noexcept
{
    {
        std::lock_guard<std::mutex> lock(_budget->mutex);
        if (!_runtime_error)
            _runtime_error = error;
    }
    try
    {
        Stop();
    }
    catch (...)
    {
        // 内存/队列持续失败时无法再保证清理任务可投递，不能让 worker 悄悄退出。
        LOG_ERROR("server cannot submit mandatory shutdown after a loop exception");
        std::terminate();
    }
}

void TcpServer::SetupStopEvents()
{
    const int deadline_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (deadline_fd < 0)
        throw std::system_error(errno, std::generic_category(), "shutdown timerfd_create");
    // 从这一行起 fd 由成员负责关闭，后续任何一步抛异常都不会泄漏。
    _deadline_fd = FdGuard(deadline_fd);
    _deadline_channel = std::make_unique<Channel>(deadline_fd, _baseloop.get());
    _deadline_channel->SetReadCallback([this] {
        uint64_t expirations;
        while (read(_deadline_fd.GetFd(), &expirations, sizeof(expirations)) < 0 && errno == EINTR)
        {
        }
        ForceCloseConnections();
    });
    _deadline_channel->EnableRead();
    _deadline_registered = true;
    if (!_signal_stop)
        return;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    const int error = pthread_sigmask(SIG_BLOCK, &mask, &_old_signal_mask);
    if (error != 0)
        throw std::system_error(error, std::generic_category(), "pthread_sigmask");
    _signal_mask_saved = true;
    const int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0)
        throw std::system_error(errno, std::generic_category(), "signalfd");
    _signal_fd = FdGuard(signal_fd);
    _signal_channel = std::make_unique<Channel>(signal_fd, _baseloop.get());
    _signal_channel->SetReadCallback([this] {
        signalfd_siginfo info;
        for (;;)
        {
            const auto count = read(_signal_fd.GetFd(), &info, sizeof(info));
            if (count == sizeof(info))
            {
                Stop();
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
    });
    _signal_channel->EnableRead();
    _signal_registered = true;
}

void TcpServer::CleanupStopEvents() noexcept
{
    try
    {
        if (_deadline_registered)
            _deadline_channel->Remove();
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("shutdown channel cleanup: {}", error.what());
    }
    _deadline_registered = false;
    _deadline_channel.reset();
    _deadline_fd.Close(); // 幂等，CleanupStopEvents 允许被多次进入
    try
    {
        if (_signal_registered)
            _signal_channel->Remove();
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("signal channel cleanup: {}", error.what());
    }
    _signal_registered = false;
    _signal_channel.reset();
    if (_signal_fd.Valid())
    {
        // 先排空内核里未读的信号再关闭，顺序不能调换。
        signalfd_siginfo info;
        while (read(_signal_fd.GetFd(), &info, sizeof(info)) == sizeof(info))
        {
        }
        _signal_fd.Close();
    }
    if (_signal_mask_saved)
    {
        const int error = pthread_sigmask(SIG_SETMASK, &_old_signal_mask, nullptr);
        if (error)
            LOG_ERROR("restore signal mask failed: {}", error);
        _signal_mask_saved = false;
    }
}

void TcpServer::Start()
{
    if (!_baseloop->IsInLoopThread())
        throw std::logic_error("TcpServer must start on its constructing thread");
    bool cannot_start;
    bool prestart_cleanup;
    {
        std::lock_guard<std::mutex> lock(_budget->mutex);
        cannot_start = _state != State::Created;
        prestart_cleanup = _state == State::Stopped && !_stop_started;
        if (!cannot_start)
            _state = State::Running;
    }
    if (cannot_start)
    {
        if (prestart_cleanup)
            _baseloop->DrainPendingTasks();
        throw std::logic_error("TcpServer cannot restart or start after Stop");
    }
    std::exception_ptr failure;
    try
    {
        // 创建工作线程前阻塞信号，使所有 worker 继承掩码。
        auto on_exception = [this](std::exception_ptr error) {
            HandleException(error);
        };
        _baseloop->SetExceptionHandler(on_exception);
        SetupStopEvents();
        _pool->Create();
        for (auto *loop : _pool->Loops())
        {
            std::function<void(std::exception_ptr)> handler = on_exception;
            loop->QueueInLoopCommitted([loop, handler = std::move(handler)]() mutable {
                loop->SetExceptionHandler(std::move(handler));
            });
        }
        _acceptor->SetAcceptCallback([this](int fd) {
            NewConnection(fd);
        });
        if (!_budget->stopping.load())
            _acceptor->StartAccepting();
        _baseloop->Loop();
    }
    catch (...)
    {
        failure = std::current_exception();
        {
            std::lock_guard<std::mutex> lock(_budget->mutex);
            _budget->stopping.store(true);
            _state = State::Stopping;
        }
        // 在拥有者循环仍存活时完成连接清理，再回收工作线程。
        _shutdown_grace = std::chrono::milliseconds(0);
        StopInLoop();
        ForceCloseConnections();
        _baseloop->DrainPendingTasks();
        if (!_conns.empty() || _cleanup_waiting != 0)
            _baseloop->Loop();
    }
    // 正常退出必须由停止屏障触发；工作线程在连接全部解除登记后才退出。
    try
    {
        _pool->Stop();
    }
    catch (...)
    {
        if (!failure)
            failure = std::current_exception();
    }
    _baseloop->DrainPendingTasks();
    CleanupStopEvents();
    {
        std::lock_guard<std::mutex> lock(_budget->mutex);
        _budget->stopping.store(true);
        _state = State::Stopped;
        if (!failure)
            failure = _runtime_error;
    }
    if (failure)
        std::rethrow_exception(failure);
}

void TcpServer::Stop()
{
    bool before_start = false;
    {
        std::lock_guard<std::mutex> lock(_budget->mutex);
        if (_state == State::Stopping || _state == State::Stopped)
            return;
        before_start = _state == State::Created;
        // 先保证任务已提交再关闭入口；同锁下 Send 不会插入两步之间。
        _baseloop->QueueInLoopCommitted([this] {
            StopInLoop();
        });
        _budget->stopping.store(true);
        _state = before_start ? State::Stopped : State::Stopping;
    }
    if (before_start && _baseloop->IsInLoopThread())
        _baseloop->DrainPendingTasks();
}

void TcpServer::StopInLoop()
{
    assert(_baseloop->IsInLoopThread());
    if (_stop_started)
        return;
    _stop_started = true;
    _acceptor->StopAccepting();
    for (auto id : _business_timers)
        _baseloop->TimerCancel(id);
    _business_timers.clear();
    for (auto &entry : _conns)
    {
        entry.second->CancelInactiveRelease();
        entry.second->ShutDown();
    }
    if (!_conns.empty())
    {
        itimerspec specification{};
        specification.it_value.tv_sec = _shutdown_grace.count() / 1000;
        specification.it_value.tv_nsec = (_shutdown_grace.count() % 1000) * 1000000;
        if (_shutdown_grace.count() == 0)
            specification.it_value.tv_nsec = 1;
        if (!_deadline_fd.Valid() || timerfd_settime(_deadline_fd.GetFd(), 0, &specification, nullptr) < 0)
            ForceCloseConnections();
    }
    MaybeFinishStop();
}

void TcpServer::ForceCloseConnections()
{
    for (auto &entry : _conns)
        entry.second->ForceClose();
    MaybeFinishStop();
}

void TcpServer::MaybeFinishStop()
{
    if (!_stop_started || !_conns.empty() || _cleanup_barrier)
        return;
    _cleanup_barrier = true;
    auto loops = _pool->Loops();
    _cleanup_waiting = loops.size();
    for (auto *loop : loops)
    {
        loop->QueueInLoopCommitted([this, loop] {
            loop->DrainPendingTasks();
            _baseloop->QueueInLoopCommitted([this] {
                WorkerCleanupComplete();
            });
        });
    }
    if (_cleanup_waiting == 0)
        _baseloop->QueueInLoopCommitted([this] {
            _baseloop->DrainPendingTasks();
            _baseloop->Quit();
        });
}

void TcpServer::WorkerCleanupComplete()
{
    assert(_cleanup_waiting != 0);
    if (--_cleanup_waiting == 0)
        _baseloop->QueueInLoopCommitted([this] {
            _baseloop->DrainPendingTasks();
            _baseloop->Quit();
        });
}

void TcpServer::NewConnection(int fd)
{
    Socket owner(fd);
    if (_budget->stopping.load() || _conns.size() >= _budget->limits.max_connections)
        return;
    const uint64_t id = AllocateId();
    auto conn = std::make_shared<Connection>(_pool->NextLoop(), id, std::move(owner), _budget);
    conn->SetConnectedCallback(_connected_callback);
    conn->SetMessageCallback(_message_callback);
    conn->SetAnyEventCallback(_event_callback);
    conn->SetClosedCallback(_closed_callback);
    conn->SetServerClosedCallback([this](const ConnectionPtr &closed) {
        RemoveConnection(closed);
    });
    _conns.emplace(id, conn);
    try
    {
        if (_enable_inactive_release)
            conn->EnableInactiveRelease(_inactive_timeout);
        conn->Established();
    }
    catch (...)
    {
        // 已经排队的建立/定时任务仍持有连接，关闭也必须在其所属循环执行。
        conn->ForceClose();
        throw;
    }
}

void TcpServer::RemoveConnection(const ConnectionPtr &conn)
{
    const auto id = conn->GetConnetId();
    _baseloop->QueueInLoopCommitted([this, id] {
        RemoveConnectionInLoop(id);
    });
}

void TcpServer::RemoveConnectionInLoop(uint64_t id)
{
    assert(_baseloop->IsInLoopThread());
    _conns.erase(id);
    MaybeFinishStop();
}

void TcpServer::SetConnectedCallback(ConnectedCallback cb)
{
    EnsureConfigurable();
    _connected_callback = std::move(cb);
}
void TcpServer::SetMessageCallback(MessageCallback cb)
{
    EnsureConfigurable();
    _message_callback = std::move(cb);
}
void TcpServer::SetClosedCallback(ClosedCallback cb)
{
    EnsureConfigurable();
    _closed_callback = std::move(cb);
}
void TcpServer::SetAnyEventCallback(AnyEventCallback cb)
{
    EnsureConfigurable();
    _event_callback = std::move(cb);
}
void TcpServer::EnableInactiveRelease(int timeout)
{
    EnsureConfigurable();
    if (timeout < 1 || timeout > 60)
        throw std::invalid_argument("inactivity timeout must be 1..60");
    _inactive_timeout = timeout;
    _enable_inactive_release = true;
}

uint64_t TcpServer::RunAfter(task_t task, int delay)
{
    if (!task)
        throw std::invalid_argument("RunAfter requires a task");
    if (delay <= 0 || delay > 60)
        throw std::invalid_argument("delay must be in [1, 60] ticks");
    std::lock_guard<std::mutex> lock(_budget->mutex);
    if (_budget->stopping.load())
        throw std::logic_error("server is stopping");
    const auto id = AllocateId();
    _baseloop->QueueInLoopCommitted([this, id, task = std::move(task), delay]() mutable {
        RunAfterInLoop(id, std::move(task), delay);
    });
    return id;
}

void TcpServer::RunAfterInLoop(uint64_t id, task_t task, int delay)
{
    if (_budget->stopping.load())
        return;
    _business_timers.insert(id);
    try
    {
        _baseloop->TimerAdd(id, static_cast<uint32_t>(delay), [this, id, task = std::move(task)] {
            _business_timers.erase(id);
            // 进入回调即为开始执行边界；停止不尝试中断同步业务代码。
            if (!_budget->stopping.load())
            {
                // TimerTask 在 noexcept 析构中触发回调，必须在此拦截业务异常。
                try
                {
                    task();
                }
                catch (...)
                {
                    HandleException(std::current_exception());
                }
            }
        });
    }
    catch (...)
    {
        _business_timers.erase(id);
        throw;
    }
}

void TcpServer::CancelTask(uint64_t id)
{
    std::lock_guard<std::mutex> lock(_budget->mutex);
    if (_budget->stopping.load())
        return;
    _baseloop->QueueInLoopCommitted([this, id] {
        _baseloop->TimerCancel(id);
        _business_timers.erase(id);
    });
}
