#include "thread/BusinessThreadPool.hpp"

#include <stdexcept>
#include <utility>

namespace
{
// 队列上限：够吸收突发，又不至于在业务全线阻塞时把内存吃光。
constexpr std::size_t kCapacity = 1024;
} // namespace

BusinessThreadPool::BusinessThreadPool(std::size_t thread_count)
    : _capacity(kCapacity)
{
    if (thread_count == 0)
        throw std::invalid_argument("BusinessThreadPool: thread count must be positive");

    _workers.reserve(thread_count);
    try
    {
        for (std::size_t index = 0; index < thread_count; ++index)
            _workers.emplace_back([this] { Worker(); });
    }
    catch (...)
    {
        // 起了一半就失败：先把已起的收回来，否则它们会悬在这里等一个永远不会来的任务
        Stop();
        throw;
    }
}

BusinessThreadPool::~BusinessThreadPool()
{
    Stop();
}

bool BusinessThreadPool::Submit(std::function<void()> task)
{
    if (!task)
        return false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping || _tasks.size() >= _capacity)
            return false;
        _tasks.push(std::move(task));
    }
    _ready.notify_one();
    return true;
}

void BusinessThreadPool::Stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping)
        {
            // 已经停过：这里只负责 join（可能上次是被析构路径之外的调用打断的）
        }
        else
        {
            _stopping = true;
        }
    }
    _ready.notify_all();

    for (auto &worker : _workers)
    {
        if (worker.joinable())
            worker.join();
    }
    _workers.clear();
}

void BusinessThreadPool::Worker()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _ready.wait(lock, [this] { return _stopping || !_tasks.empty(); });

            // 停止时把队列里已有的任务做完再退出：半途丢弃会让客户端拿不到任何响应
            if (_tasks.empty())
                return;
            task = std::move(_tasks.front());
            _tasks.pop();
        }
        task();
    }
}
