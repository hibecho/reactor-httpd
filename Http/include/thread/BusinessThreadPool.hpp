/**
 * @file BusinessThreadPool.hpp
 * @brief 业务线程池：执行可能长时间阻塞的 handler，使事件循环不被占住。
 *
 * 与 LoopThreadPool 正交：那个管 IO 事件循环（每个 loop 一个线程，连接绑定其上、不迁移），
 * 这个管业务回调。两者必须分开，原因不只是「别的请求排队」——
 *
 * 事件循环线程一旦被 handler 占住，连它自己刚写的数据都发不出去：Connection::Send 只把
 * 数据写进输出缓冲并 EnableWrite()，真正的 send() 发生在 EPOLLOUT 回调里，而 EPOLLOUT
 * 要等循环回到 epoll_wait 才会被处理。所以阻塞的 handler 会让流式响应退化成「攒完一次性发」，
 * 无论写代码时调了多少次 Write。
 *
 * 任务队列满时 Submit 直接返回 false（由调用方决定回 503 还是别的），不阻塞事件循环——
 * 在事件循环线程上等待业务线程腾出位置，等于把刚解决的问题原样搬回来。
 */
#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class BusinessThreadPool
{
  public:
    explicit BusinessThreadPool(std::size_t thread_count);
    ~BusinessThreadPool();

    BusinessThreadPool(const BusinessThreadPool &) = delete;
    BusinessThreadPool &operator=(const BusinessThreadPool &) = delete;

    // 投递任务；队列已满或已停止时返回 false。
    bool Submit(std::function<void()> task);

    // 停止并回收全部工作线程。可重复调用；析构时也会调用。
    void Stop();

  private:
    void Worker();

    std::vector<std::thread> _workers;
    std::queue<std::function<void()>> _tasks;
    std::mutex _mutex;
    std::condition_variable _ready;
    std::size_t _capacity;
    bool _stopping = false;
};
