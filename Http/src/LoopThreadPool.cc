#include "thread/LoopThreadPool.hpp"
#include "reactor/EventLoop.hpp"
#include "base/Logger.hpp"
#include "thread/LoopThread.hpp"

LoopThreadPool::LoopThreadPool(EventLoop *baseloop)
    : _baseloop(baseloop)
{}

LoopThreadPool::~LoopThreadPool() = default;

// 启动前配置，拒绝负数
void LoopThreadPool::SetThreadCount(int count)
{
    if (count < 0)
    {
        LOG_ERROR("线程数量不能为负数");
        throw std::invalid_argument("线程数量不能为负数");
    }
    _thread_count = count;
}

// 创建线程，等待各 EventLoop 就绪
void LoopThreadPool::Create()
{
    if (_thread_count > 0)
    {
        _threads.resize(_thread_count);
        for (int i = 0; i < _thread_count; i++)
        {
            _threads[i] = std::make_unique<LoopThread>();
        }
    }
}

// 启动后分配，返回目标循环。
EventLoop *LoopThreadPool::NextLoop()
{
    if (_threads.empty())
        return _baseloop;
    EventLoop *loop = _threads[_next]->GetLoop();
    _next = (_next + 1) % _threads.size();
    return loop;
}
