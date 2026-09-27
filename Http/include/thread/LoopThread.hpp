/**
 * @file LoopThread.hpp
 * @brief
 *
 * 将 EventLoop模块与线程整合
 *  - EventLoop模块与线程是 一一对应的
 *  - EventLoop模块实例化的对象，在构造的时候初始化线程id: _thread_id
 *
 * 设计方案:
 *  1.创建线程
 *  2.在线程中实例化EventLoop模块
 *
 */

#pragma once
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
class EventLoop;

class LoopThread
{
  public:
    LoopThread();
    ~LoopThread();
    // 指针仅在拥有者尚未停止线程时有效；退出后返回 nullptr。
    EventLoop *GetLoop();
    void RequestStop();
    // 由拥有者线程调用；回收后重抛工作线程异常，禁止自 join。
    void Join();
    void Stop();
    LoopThread(const LoopThread &) = delete;
    LoopThread &operator=(const LoopThread &) = delete;

  private:
    void ThreadEntry();

  private:
    /*用于实现_loop获取的同步，避免线程创建了，但_loop还没有进行实例化就进行获取*/
    std::mutex _mutex;           // 互斥锁
    std::condition_variable _cv; // 条件变量，实现同步关系

    /*EventLoop 与 EventThread模块绑定，实现one Loop one Thread*/
    EventLoop *_loop; // EventLoop指针变量，这个对象需要在线程内部实例化
    bool _started = false;
    bool _stop_requested = false;
    std::exception_ptr _error;
    std::thread _thread; // EventLoop对应的线程，必须在最末尾声明
};