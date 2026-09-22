/**
 * @file TimerQueue.hpp
 * @brief
 *
 * 设计目的:用时间轮管理定时任务，由 timerfd 驱动，每 1 秒推进一格（tick）。
 *
 * 时间精度与范围（容量固定为 60）:
 *  - 最小延迟 1 个 tick:timeout == 0 会被归一为 1，不会在添加时就地同步回调；
 *  - 最大延迟 capacity 个 tick:timeout 超过 capacity 会被夹紧到 capacity，
 *    而不是按 capacity 取模（取模会让 timeout=61 变成 1 秒后就触发）。
 *  - 时间精度范围为[1,60]
 *
 * 重复添加语义:同一个 id 再次 TimerAdd 视为重启——旧任务被取消，只有新任务触发。
 * 只想在原有任务上重新计时、不换回调时，用 TimerRefresh 更省（不分配新任务对象）。
 *
 * 声明与实现分离，实现见 src/TimerQueue.cc
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class EventLoop; // 前向声明：TimerWheel 只持有其指针并转发任务投递
class Channel;   // 前向声明：unique_ptr 成员，析构定义在 .cc 中，此处无需完整类型

using task_t = std::function<void()>;
using ReleaseFunc = std::function<void()>;

class TimerTask
{
  public:
    TimerTask(uint64_t id, uint32_t timeout, const task_t &cb);
    ~TimerTask();

    void SetRelease(ReleaseFunc release);
    uint32_t GetTimeout();
    void Cancel();

  private:
    uint64_t _id;         // 定时器任务的对象ID
    uint32_t _timeout;    // 定时任务的超时对象
    task_t _task_cb;      // 定时器对象要执行的任务
    ReleaseFunc _release; // 用于移除TimerWheel中的定时器对象信息
    bool _canceled;       // 定时任务是否被取消
};

class TimerWheel
{
    using WeakTask = std::weak_ptr<TimerTask>;
    using PtrTask = std::shared_ptr<TimerTask>;

  public:
    /*TimerWheel构造函数*/
    TimerWheel(EventLoop *loop);
    ~TimerWheel();
    TimerWheel(const TimerWheel &) = delete;
    TimerWheel &operator=(const TimerWheel &) = delete;

    void TimerAdd(uint64_t id, uint32_t timeout, task_t cb);
    void TimerAddInLoop(uint64_t id, uint32_t timeout, task_t cb);
    void TimerRefresh(uint64_t id);
    void TimerRefreshInLoop(uint64_t id);
    void TimerCancel(uint64_t id);
    void TimerCancelInLoop(uint64_t id);
    void Tick();

  private:
    // 仅当 _timers 中该 id 仍然指向 self 时移除记录，
    // 避免旧任务析构时错删被同 id 新任务接管的登记
    void RemoveTimerIfOwned(uint64_t id, const WeakTask &self);
    // 创建Timerfd文件描述符
    int CreateTimerfd();
    // 读 timerfd，返回本次读到的到期次数（调用方据此推进同等数量的 tick）
    uint64_t ReadTimerfd();

  private:
    // Eventloop模块监控定时事件
    EventLoop *_loop;                        // EventLoop管理定时器
    int _timerfd;                            // 定时器描述符：读取计时器，进行定时任务
    std::unique_ptr<Channel> _timer_channel; // 管理定时器描述符

    // 定时器模块
    uint32_t _tick;     // 当前的秒针，tick走到哪里就释放哪里的PtrTask，释放哪里相当于执行哪里的任务
    uint32_t _capacity; // 表盘的最大数量 --最大延迟时间
    std::vector<std::vector<PtrTask>> _wheel;       // 任务表盘
    std::unordered_map<uint64_t, WeakTask> _timers; // 观察查找并取消任务,让挂进表盘的定时任务失去"将来被触发"的资格。
};
