/**
 * @file EventLoop.cc
 * @brief
 *
 * 创建 eventfd 失败 -> 抛出异常 ->停止 EventLoop 构造，不再创建后面的 Channel
 */

#include "reactor/EventLoop.hpp"
#include "base/Buffer.hpp"
#include "base/Logger.hpp"
#include "reactor/Channel.hpp"
#include "reactor/Epoller.hpp"
#include "reactor/TimerQueue.hpp"
#include "tcp/Socket.hpp"
#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

EventLoop::EventLoop()
    : _thread_id(std::this_thread::get_id())
    , _event_fd(CreateEventfd())
    , _event_channel(std::make_unique<Channel>(_event_fd, this))
    , _ep(std::make_unique<Epoller>())
    , _tw(std::make_unique<TimerWheel>(this))
{
    // 给_event_channel注册回调方法
    // 1.设置读事件回调
    // 2.设置可读事件
    _event_channel->SetReadCallback([this]() {
        HandleEventfd();
    });
    _event_channel->EnableRead();
}

EventLoop::~EventLoop()
{
    if (close(_event_fd) < 0)
        LOG_ERROR("Failed to close event fd");
}

void EventLoop::Loop()
{
    while (!_quit.load())
    {
        LoopOnce();
    }
}

// 处理一轮：等待一次就绪事件 → 分发 → 执行一批任务
// 不读取也不重置停止标志：停止判定只由 Loop() 的循环条件负责。
void EventLoop::LoopOnce()
{
    // 1.等待事件
    std::vector<Channel *> active;
    try { _ep->WaitEvent(active); }
    catch (...)
    {
        if (!_exception_handler) throw;
        _exception_handler(std::current_exception());
        ExecuteTasks(); // 故障停机仍需处理连接注销和关闭任务。
        return;
    }
    // 2.处理就绪事件
    for (Channel *channel : active)
    {
        try { channel->Handle(); }
        catch (...)
        {
            if (!_exception_handler) throw;
            _exception_handler(std::current_exception());
        }
    }
    // 3.执行一批任务
    ExecuteTasks();
}

void EventLoop::Quit()
{
    _quit.store(true);
    WeakupEventfd();
}

void EventLoop::RunInLoop(Task task)
{
    if (IsInLoopThread())
    {
        task();
    }
    else
    {
        QueueInLoop(std::move(task));
    }
}

// 来自其他线程，都放入队列，稍后执行
void EventLoop::QueueInLoop(Task task)
{
    // 线程安全的放入任务
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _tasks.push(std::move(task));
    }
    // 发送唤醒通知，任务队列中任务已经就绪
    WeakupEventfd();
}

void EventLoop::SetExceptionHandler(std::function<void(std::exception_ptr)> handler)
{
    if (!IsInLoopThread())
        throw std::logic_error("SetExceptionHandler requires loop thread");
    _exception_handler = std::move(handler);
}

void EventLoop::QueueInLoopCommitted(Task task)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _tasks.push(std::move(task));
    }
    try { WeakupEventfd(); }
    catch (...) { LOG_ERROR("task committed; eventfd wake failed, awaiting timerfd"); }
}

void EventLoop::DrainPendingTasks()
{
    if (!IsInLoopThread())
        throw std::logic_error("DrainPendingTasks requires loop thread");
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_tasks.empty() && _active_tasks.empty())
                return;
        }
        ExecuteTasks();
    }
}

bool EventLoop::IsInLoopThread() const
{
    return _thread_id == std::this_thread::get_id();
}

// 取出一批任务，解锁后执行
void EventLoop::ExecuteTasks()
{
    // 线程安全的取出任务
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_active_tasks.empty())
            _active_tasks.swap(_tasks);
    }

    // 执行任务
    while (!_active_tasks.empty())
    {
        Task task = std::move(_active_tasks.front());
        _active_tasks.pop();
        try { task(); }
        catch (...)
        {
            if (!_exception_handler) throw;
            _exception_handler(std::current_exception());
        }
    }
}

// 添加/修改描述符的事件
void EventLoop::UpdateEvent(Channel *channel)
{
    _ep->UpdateEvent(channel);
}

// 移除描述符的事件
void EventLoop::RemoveEvent(Channel *channel)
{
    _ep->RemoveEvent(channel);
}

// 创建eventfd
int EventLoop::CreateEventfd() const
{
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
    {
        const int error = errno;
        LOG_ERROR("创建 eventfd 失败");
        throw std::system_error(error, std::generic_category(), "创建 eventfd 失败");
    }
    return efd;
}

// 写 eventfd
void EventLoop::WeakupEventfd()
{
    const uint64_t cnt = 1;

    while (true)
    {
        const ssize_t ret = write(_event_fd, &cnt, sizeof(cnt));

        if (ret == static_cast<ssize_t>(sizeof(cnt)))
            return;

        if (ret == -1)
        {
            const int error = errno;

            if (error == EINTR)
                continue;

            if (error == EAGAIN || error == EWOULDBLOCK)
                return;
            LOG_ERROR("写 eventfd 失败");
            throw std::system_error(error, std::generic_category(), "写 eventfd 失败");
        }
        LOG_ERROR("写 eventfd 返回了异常字节数");
        // eventfd 正常写入应当完整写入 8 字节
        throw std::runtime_error("写 eventfd 返回了异常字节数");
    }
}

// 读 eventfd，消费通知
void EventLoop::HandleEventfd()
{
    uint64_t cnt = 0;
    while (true)
    {
        const ssize_t ret = read(_event_fd, &cnt, sizeof(cnt));

        if (ret == static_cast<ssize_t>(sizeof(cnt)))
            return;

        if (ret == -1)
        {
            const int error = errno;

            if (error == EINTR)
                continue;

            if (error == EAGAIN || error == EWOULDBLOCK)
                return;
            LOG_ERROR("读 eventfd 失败");
            throw std::system_error(error, std::generic_category(), "读 eventfd 失败");
        }
        LOG_ERROR("读 eventfd 返回了异常字节数");
        throw std::runtime_error("读 eventfd 返回了异常字节数");
    }
}

void EventLoop::TimerAdd(uint64_t id, uint32_t timeout, task_t cb)
{
    _tw->TimerAdd(id, timeout, cb);
}
void EventLoop::TimerRefresh(uint64_t id)
{
    _tw->TimerRefresh(id);
}
void EventLoop::TimerCancel(uint64_t id)
{
    _tw->TimerCancel(id);
}