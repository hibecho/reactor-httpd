#include "thread/LoopThread.hpp"
#include "reactor/EventLoop.hpp"
#include <memory>
#include <stdexcept>

LoopThread::LoopThread()
    : _loop(nullptr), _thread([this] { ThreadEntry(); })
{}

LoopThread::~LoopThread()
{
    // 对象属于控制线程，不能由正在使用 this 的工作线程销毁。
    if (_thread.joinable() && _thread.get_id() == std::this_thread::get_id())
        std::terminate();
    try { RequestStop(); } catch (...) {}
    if (_thread.joinable())
        _thread.join();
}

EventLoop *LoopThread::GetLoop()
{
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [this] { return _started; });
    if (_error)
        std::rethrow_exception(_error);
    return _loop;
}

void LoopThread::RequestStop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _stop_requested = true;
    // 和退出时清空指针使用同一把锁，保证调用期间对象仍然存在。
    if (_loop)
        _loop->Quit();
}

void LoopThread::Join()
{
    if (_thread.joinable())
    {
        if (_thread.get_id() == std::this_thread::get_id())
            throw std::logic_error("LoopThread cannot join itself");
        _thread.join();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_error)
        std::rethrow_exception(_error);
}

void LoopThread::Stop()
{
    std::exception_ptr error;
    try { RequestStop(); } catch (...) { error = std::current_exception(); }
    Join();
    if (error)
        std::rethrow_exception(error);
}

void LoopThread::ThreadEntry()
{
    // 保留所有权直到指针在锁内失效，异常退出也不会暴露悬空指针。
    std::unique_ptr<EventLoop> loop;
    try
    {
        loop = std::make_unique<EventLoop>();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stop_requested)
                loop->Quit();
            _loop = loop.get();
            _started = true;
        }
        _cv.notify_all();
        loop->Loop();
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _error = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _loop = nullptr;
        _started = true;
    }
    _cv.notify_all();
}
