/**
 * @file TcpServer.hpp
 * @brief
 *
 * 对象管理:
 *  - Acceptor对象，创建一个监听套接字
 *  - EventLoop对象,基于baseloop对象，实现对监听套接字的事件监控
 *  - 实现对所有新连接的管理：std::unordermap<uint64_t,PtrConnection> _conns
 *  - LoopThreadPool对象，创建loop线程池，对新建连接进行事件监控及处理
 *
 * 功能设计:
 * - 设置从属线程池的数量
 * - 启动服务器
 * - 设置各种回调函数，用户设置给TcpServer，TcpServer设置给获取的新连接
 * - 是否启动非活跃连接定时销毁任务
 * - 添加定时任务功能
 *
 * 流程设计:
 * 1.在TcpServer中实例化一个Acceptor，以及一个EventLoop对象(baseloop)
 * 2.将Acceptor挂载到baseloop
 * 3.一旦Acceptor对象就绪了可读事件，则执行读事件回调函数获取新连接
 * 4.对新连接，创建一个Connection对象，并对Connection设置功能回调函数
 * 5.是否启动Connection的非活跃连接超时销毁规则
 * 6.将新连接对于的Connection挂到LoopThreadPool从属线程对应的EventLoop中进行事件监控
 * 7.一旦Connection对应的连接就绪了可读事件，则这时候进行读时间回调函数，读取数据，读取完毕后调用TcpServer设置的消息回调
 *
 *
 * 完整流程图:
 *    客户端发起连接
 *           ↓
 *     Acceptor 接收连接 ← baseloop 监控监听套接字
 *           ↓
 *     创建 Connection，设置回调和超时规则
 *           ↓
 *     分配给线程池中的某个 EventLoop
 *           ↓
 *     客户端发送数据
 *           ↓
 *     Connection 读取数据，调用用户设置的消息回调
 *
 */

#pragma once

#include "reactor/TimerQueue.hpp"
#include "tcp/Connection.hpp"
#include "tcp/ResourceLimits.hpp"
#include <chrono>
#include <exception>
#include <csignal>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <stdint.h>
#include <unordered_map>
class Channel;
class Acceptor;
class EventLoop;
class LoopThreadPool;

class TcpServer
{
    // 连接类事件：建立成功
    using ConnectedCallback = std::function<void(const ConnectionPtr &)>;
    // 连接类事件：建立关闭
    using ClosedCallback = std::function<void(const ConnectionPtr &)>;
    // 数据到达：业务层解析输入缓冲区
    using MessageCallback = std::function<void(const ConnectionPtr &, Buffer *)>;
    // 任意事件：刷新活跃度
    using AnyEventCallback = std::function<void(const ConnectionPtr &)>;

  public:
    /*TcpServer的构造函数*/
    explicit TcpServer(uint16_t port);
    ~TcpServer();
    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    // 设置线程数量
    void SetThreadCount(int count);
    // 设置是否启用超时销毁
    void EnableInactiveRelease(int timeout);
    // 可跨线程提交；delay 为 1..60 个 tick，返回本服务器内唯一的任务 ID。
    // 添加和取消均排队执行；服务器及主循环须保持存活并继续处理队列。
    uint64_t RunAfter(task_t task, int delay);
    // 仅传入本服务器 RunAfter 返回的 ID；重复取消或任务已结束时无操作。
    // 异步取消，不能中断已经开始执行的回调。
    void CancelTask(uint64_t timer_id);
    // 启动服务器
    void Start();
    // 异步请求；Start 返回时清理和线程回收完成。启动前停止后不可重启。
    // 调用者须保证对象活到 Start 返回；不能在运行中析构。
    void Stop();
    void SetResourceLimits(const ResourceLimits &limits);
    void SetShutdownGrace(std::chrono::milliseconds grace);
    void EnableSignalStop();
    uint16_t GetPort() const noexcept { return _port; }

    // 设置连接回调
    void SetConnectedCallback(ConnectedCallback cb);
    // 设置消息处理回调
    void SetMessageCallback(MessageCallback cb);
    // 设置关闭回调
    void SetClosedCallback(ClosedCallback cb);
    // 设置任意事件回调
    void SetAnyEventCallback(AnyEventCallback cb);

  private:
    // 为新连接构造 Connection；连接 ID 必须由 AllocateId() 分配。
    void NewConnection(int fd);
    // 从管理Connection的_conns进行移除连接
    void RemoveConnection(const ConnectionPtr &conn);
    void RemoveConnectionInLoop(uint64_t id);
    uint64_t AllocateId(); // 连接和普通任务统一取号，不回收 ID。
    void RunAfterInLoop(uint64_t id, task_t task, int delay);
    void EnsureConfigurable() const;
    void StopInLoop();
    void MaybeFinishStop();
    void WorkerCleanupComplete();
    void ForceCloseConnections();
    void HandleException(std::exception_ptr error) noexcept;
    void SetupStopEvents();
    void CleanupStopEvents() noexcept;


  private:
    /*基本属性*/
    uint16_t _port;                                     // 服务器所需要的端口
    std::atomic<uint64_t> _next_id{0};                  // 连接与普通任务统一分配 ID
    bool _enable_inactive_release = false;              // 默认取消定时销毁
    int _inactive_timeout = 0;                          // 0 表示未启用，正数表示超时时间
    std::unique_ptr<EventLoop> _baseloop;               // 主线程的EventLoop对象，负责监听事件的处理
    std::unique_ptr<Acceptor> _acceptor;                // 监听套接字管理的对象
    std::unique_ptr<LoopThreadPool> _pool;              // 这是从属EventLoop线程池
    std::unordered_map<uint64_t, ConnectionPtr> _conns; // 保存管理所有连接对应的shared_ptr对象

    enum class State { Created, Running, Stopping, Stopped };
    // 状态及业务提交与 Connection::Send 共用额度锁，定义统一接受边界。
    std::shared_ptr<OutputBudget> _budget = std::make_shared<OutputBudget>();
    State _state = State::Created;
    std::chrono::milliseconds _shutdown_grace{5000};
    std::unordered_set<uint64_t> _business_timers;
    std::exception_ptr _runtime_error; // 由额度锁保护
    bool _stop_started = false;
    bool _cleanup_barrier = false;
    std::size_t _cleanup_waiting = 0;
    int _deadline_fd = -1;
    std::unique_ptr<Channel> _deadline_channel;
    bool _deadline_registered = false;
    bool _signal_stop = false;
    int _signal_fd = -1;
    std::unique_ptr<Channel> _signal_channel;
    bool _signal_registered = false;
    bool _signal_mask_saved = false;
    sigset_t _old_signal_mask{};

    /*设置事件回调*/
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;
};