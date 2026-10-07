#include "thread/LoopThreadPool.hpp"
#include "thread/LoopThread.hpp"
#include <exception>
#include <stdexcept>

LoopThreadPool::LoopThreadPool(EventLoop *baseloop)
    : _baseloop(baseloop)
{
}

LoopThreadPool::~LoopThreadPool()
{
    try
    {
        RequestStop();
    }
    catch (...)
    {
    }
    // 每个 LoopThread 析构都会 join，且不会抛出工作线程异常。
}

void LoopThreadPool::SetThreadCount(int count)
{
    if (count < 0)
        throw std::invalid_argument("线程数量不能为负数");
    if (_created)
        throw std::logic_error("线程池启动后不能配置线程数量");
    _thread_count = count;
}

void LoopThreadPool::Create()
{
    if (_created)
        throw std::logic_error("线程池不能重复创建");
    std::vector<std::unique_ptr<LoopThread>> threads;
    threads.reserve(static_cast<std::size_t>(_thread_count));
    for (int i = 0; i < _thread_count; ++i)
    {
        threads.push_back(std::make_unique<LoopThread>());
        if (!threads.back()->GetLoop())
            throw std::runtime_error("工作线程在启动期间退出");
    }
    _threads.swap(threads);
    _next = 0;
    _created = true;
}

EventLoop *LoopThreadPool::NextLoop()
{
    if (_threads.empty())
        return _baseloop;
    EventLoop *loop = _threads[_next]->GetLoop();
    if (!loop)
        throw std::runtime_error("工作线程已停止");
    _next = (_next + 1) % _threads.size();
    return loop;
}

void LoopThreadPool::RequestStop()
{
    std::exception_ptr error;
    for (auto &thread : _threads)
    {
        try
        {
            thread->RequestStop();
        }
        catch (...)
        {
            if (!error)
                error = std::current_exception();
        }
    }
    if (error)
        std::rethrow_exception(error);
}

void LoopThreadPool::Join()
{
    std::exception_ptr error;
    for (auto &thread : _threads)
    {
        try
        {
            thread->Join();
        }
        catch (...)
        {
            if (!error)
                error = std::current_exception();
        }
    }
    if (error)
        std::rethrow_exception(error);
}

void LoopThreadPool::Stop()
{
    std::exception_ptr error;
    try
    {
        RequestStop();
    }
    catch (...)
    {
        error = std::current_exception();
    }
    Join();
    if (error)
        std::rethrow_exception(error);
}

std::vector<EventLoop *> LoopThreadPool::Loops()
{
    std::vector<EventLoop *> loops;
    loops.reserve(_threads.size());
    for (auto &thread : _threads)
    {
        if (auto *loop = thread->GetLoop())
            loops.push_back(loop);
    }
    return loops;
}
