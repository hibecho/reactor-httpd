/**
 * @file EventLoop.hpp
 * @brief
 *
 * 功能作用:
 *   -让一个线程统一处理它负责的连接事件；
 *   -其他线程如果要操作这些连接，就把操作作为任务交给这个线程执行。
 *
 * 关键点:这个模块与线程一一对应
 *
 * 一个 EventLoop 可以管理很多连接，并不是一个连接一个线程
 * 如何保证一个连接的所有操作都在eventloop模块
 *
 * 解决方案:
 *  -给eventloop模块添加一个任务队列
 *  -对连接的所有操作都进行一次封装，将对连接的操作并不直接执行，而是当作任务添加到任务队列中
 *
 * eventloop的处理流程
 *  1.在线程中对描述符进行事件监控
 *  2.有描述符就绪则对描述符进行事件处理 (如何保证处理回调函数中的操作都在线程中)
 *  3.就绪事件处理完，这时候在对任务队列的所有任务进行依次执行
 *
 *
 * 成员包含:
 *  -使用Poller模块
 *  -事件监控
 *  -互斥锁
 *  -任务队列
 *
 * 接口设计:
 *
 *
 */

#pragma once
#include "Buffer.hpp"
#include "Channel.hpp"
#include "Epoller.hpp"
#include "Logger.hpp"
#include "Socket.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/eventfd.h>
class EventLoop
{
    using Task = std::function<void()>;

  public:
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // 必须由所属线程调用
    void Loop();
    // 当前就是所属线程：立即执行
    // 其他线程：入队并唤醒
    void RunInLoop(Task task);
    // 来自其他线程，都放入队列，稍后执行
    void QueueInLoop(Task task);
    bool IsInLoopThread() const;
    void UpdateEvent(Channel *channel); // 添加/修改描述符的事件
    void RemoveEvent(Channel *channel); // 移除描述符的事件

  private:
    void WeakupEventfd(); // 写 eventfd
    void HandleEventfd(); // 读 eventfd，消费通知
    void ExecuteTasks();  // 取出一批任务，解锁后执行
    int CreateEventfd() const;

  private:
    std::thread::id _thread_id;              // 线程id: 判断操作是否发生在所属线程
    int _event_fd;                           // _event_fd: 通知任务队列的文件描述符
    std::unique_ptr<Channel> _event_channel; // _event_channel: 用于管理通知任务队列的文件描述符
    std::unique_ptr<Epoller> _ep;            // 找出哪些文件描述符已经就绪
    std::queue<Task> _tasks;                 // 任务队列
    std::mutex _mutex;                       // 保证任务队列线程安全的互斥锁
};