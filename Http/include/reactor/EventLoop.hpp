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
 * 这三步构成一轮，即 LoopOnce()；Loop() 反复执行它直到 Quit()。
 *
 *
 * 成员包含:
 *  -使用Poller模块
 *  -事件监控
 *  -互斥锁
 *  -任务队列
 *
 */

#pragma once

#include "base/FdGuard.hpp"
#include "reactor/TimerQueue.hpp"
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/eventfd.h>
#include <thread>
class Buffer;
class Channel;
class Epoller;
class Logger;
class Socket;

class EventLoop
{
    using Task = std::function<void()>;

  public:
    /*构造函数*/
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    /*EventLoop模块的接口*/

    // 持续事件循环：反复执行 LoopOnce()，直到 Quit() 置位。
    // 必须由所属线程调用；Quit 后不能重新启动（不重置停止标志）。
    void Loop();
    // 处理一轮：等待一次就绪事件 → 分发 → 执行一批任务，然后返回。
    // 不读取也不重置停止标志，停止判定由 Loop() 的循环条件负责。
    // 没有就绪事件且 eventfd 未被写过时会阻塞在 epoll_wait（超时为 -1）。
    // 必须由所属线程调用。
    void LoopOnce();
    // 可跨线程调用：请求退出并唤醒，完成当前轮后返回。
    // 调用期间对象必须存活；唤醒失败会抛异常。
    // 上层须先停止提交任务并完成连接清理，不保证排空后来入队的任务。
    void Quit();

    // 当前就是所属线程：立即执行
    // 其他线程：入队并唤醒
    void RunInLoop(Task task);
    // 来自其他线程，都放入队列，稍后执行
    void QueueInLoop(Task task);
    // 入队前失败抛异常；入队后唤醒失败只记日志，周期 timerfd 保证继续推进。
    // 返回即表示已提交，不可因唤醒失败重试同一发送。
    void QueueInLoopCommitted(Task task);
    // 所属线程关闭外部业务提交入口后，排空已接受任务及其派生清理。
    void DrainPendingTasks();
    // 所属线程设置；服务器用它安排故障停机。未设置时异常仍向调用方传播。
    void SetExceptionHandler(std::function<void(std::exception_ptr)> handler);
    // 判断是否为所属线程
    bool IsInLoopThread() const;

    /* Epoller模块的上层接口*/
    //  添加/修改描述符的事件
    void UpdateEvent(Channel *channel);
    // 移除描述符的事件
    void RemoveEvent(Channel *channel);

    /*TiemrQueue模块的上层接口*/
    // 添加定时任务
    void TimerAdd(uint64_t id, uint32_t timeout, task_t cb);
    // 刷新定时任务
    void TimerRefresh(uint64_t id);
    // 取消定时任务
    void TimerCancel(uint64_t id);

  private:
    int CreateEventfd() const; // 创建eventfd
    void WeakupEventfd();      // 写 eventfd
    void HandleEventfd();      // 读 eventfd，消费通知
    void ExecuteTasks();       // 取出一批任务，解锁后执行

  private:
    std::function<void(std::exception_ptr)> _exception_handler;
    std::atomic<bool> _quit{false};

    /*EventLoop模块：维护EventLoop的任务队列*/
    std::thread::id _thread_id;              // 线程id: 判断操作是否发生在所属线程
    FdGuard _event_fd;                       // _event_fd: 通知任务队列的文件描述符，由成员自己负责关闭
    std::unique_ptr<Channel> _event_channel; // _event_channel: 用于管理通知任务队列的文件描述符
    std::queue<Task> _active_tasks;          // 所属线程当前批次，清理屏障也必须排空
    std::queue<Task> _tasks;                 // 任务队列
    std::mutex _mutex;                       // 保证任务队列线程安全的互斥锁

    /* Epoller模块 */
    std::unique_ptr<Epoller> _ep; // 找出哪些文件描述符已经就绪

    /*TimerQueue模块*/
    std::unique_ptr<TimerWheel> _tw; // 管理定时任务
};