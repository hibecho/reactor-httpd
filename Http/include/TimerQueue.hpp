#pragma once

#include "EventLoop.hpp"
#include <cerrno>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <sys/timerfd.h>
#include <system_error>
#include <unordered_map>
#include <vector>

using task_t = std::function<void()>;
using ReleaseFunc = std::function<void()>;
class TimerTask
{
  public:
    TimerTask(uint64_t id, uint32_t timeout, const task_t &cb)
        : _id(id)
        , _timeout(timeout)
        , _task_cb(cb)
        , _canceled(false)
    {
    }

    void SetRelease(const ReleaseFunc release)
    {
        _release = std::move(release);
    }

    uint32_t GetTimeout()
    {
        return _timeout;
    }

    void Concel()
    {
        _canceled = true;
    }

    ~TimerTask()
    {
        // tick 指向该槽位 -> 主动释放shared_ptr -> 触发TimerTask析构函数 -> 执行回调方法
        if (!_canceled)
            _task_cb();
        // 清理 _timers 中的记录
        _release();
    }

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
    TimerWheel(EventLoop *loop)
        : _loop(loop)
        , _timerfd(CreateTimerfd())
        , _timer_channel(std::make_unique<Channel>(_timerfd, _loop))
        , _tick(0)
        , _capacity(60)
        , _wheel(_capacity)
    {
        _timer_channel->SetReadCallback([this]() {
            ReadTimefd();
            Tick();
        });
        _timer_channel->EnableRead();
    }

    ~TimerWheel()
    {
        // 前提：已停止事件分发，不再有回调访问本对象。
        // 进入 TimerWheel 析构函数体，两个容器都有效
        // → 取消所有任务，避免销毁时执行业务回调
        // → 清空 _wheel，触发 TimerTask 析构
        // → _release() 访问仍有效的 _timers
        // → 析构函数体结束
        // → 自动析构 _timers 和已清空的 _wheel

        for (auto &slot : _wheel)
        {
            for (auto &ptrtask : slot)
            {
                ptrtask->Concel();
            }
        }
        _wheel.clear();
    }

    void TimerAdd(uint64_t id, uint32_t timeout, task_t cb)
    {
        _loop->RunInLoop([this, id, timeout, cb]() { TimerAddInLoop(id, timeout, cb); });
    }

    // 向_wheel中添加定时任务
    // id:为每个任务生成的编号
    // timeout:每次tick触发的时间
    void TimerAddInLoop(uint64_t id, uint32_t timeout, task_t cb)
    {
        PtrTask pt = std::make_shared<TimerTask>(id, timeout, cb);
        pt->SetRelease([this, id]() { RemoveTimer(id); });

        // 根据时钟滴答位置，添加定时任务
        const auto pos = (_tick + timeout) % _capacity;
        _wheel[pos].push_back(pt);
        _timers[id] = WeakTask(pt); // 隐式转换：PtrTask-> WeakTask
    } // 出作用域后，pt销毁，仅有_wheel[pos]管理任务

    void TimerRefresh(uint64_t id)
    {
        _loop->RunInLoop([this, id]() { TimerRefreshInLoop(id); });
    }

    // 刷新/延迟_wheel中的任务
    // 设计思路: 把同一个任务的智能指针，再放入新的时间槽，旧槽中的指针保留即可
    // - 旧槽释放，只会进行引用计数--
    // - 新槽释放才会进行回调任务
    void TimerRefreshInLoop(uint64_t id)
    {
        // 1.根据ID查找任务
        auto it = _timers.find(id);
        if (it == _timers.end())
        {
            return;
        }

        // 2.将弱指针提升为共享指针
        PtrTask task = it->second.lock();
        if (!task)
        {
            // 任务已经销毁，提前返回
            return;
        }

        // 3.从当前刻度重新计算到期槽位
        const auto pos = (_tick + task->GetTimeout()) % _capacity;

        // 4. 新槽位也持有同一个任务
        _wheel[pos].push_back(task);
    }

    void TimerConcel(uint64_t id)
    {
        _loop->RunInLoop([this, id]() { TimerConcelInLoop(id); });
    }

    // 取消任务
    void TimerConcelInLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
        {
            return;
        }
        PtrTask task = it->second.lock();
        if (!task)
        {
            // 任务已经销毁，提前返回
            return;
        }
        task->Concel();
    }

    // 每秒中执行一次
    void Tick()
    {
        _tick = (_tick + 1) % _capacity;
        // 每秒执行一次定时任务
        std::vector<PtrTask> expired;
        expired.swap(_wheel[_tick]);
        // 主动将槽清空，假设这是任务最后一个 shared_ptr，执行析构调用任务
        expired.clear();
    }

  private:
    // RemoveTimer:移除_timers中观察的指定 ID 任务
    void RemoveTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it != _timers.end())
        {
            _timers.erase(id);
        }
    }
    int CreateTimerfd()
    {
        // CLOCK_MONOTONIC:它从系统启动开始计时，只会单调递增，不受用户修改系统时间、NTP 调整时间的影响
        int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (timerfd < 0)
        {
            const int saved_errno = errno;
            LOG_ERROR("Failed to timerfd_create: errno={}", saved_errno);
            throw std::system_error(saved_errno, std::generic_category(), "timerfd_create failed");
        }

        struct itimerspec itime;
        // 第一次设定的超时时间
        itime.it_value.tv_sec = 1;
        itime.it_value.tv_nsec = 0;
        // 第一次以后设定的超时时间。
        itime.it_interval.tv_sec = 1;
        itime.it_interval.tv_nsec = 0;
        // 将定时设定到内核
        timerfd_settime(timerfd, 0, &itime, nullptr);
        return timerfd;
    }
    void ReadTimefd()
    {
        uint64_t times;
        while (true)
        {
            const ssize_t ret = read(_timerfd, &times, sizeof(times));

            if (ret == static_cast<ssize_t>(sizeof(times)))
                return;

            if (ret == -1)
            {
                const int error = errno;

                if (error == EINTR)
                    continue;

                if (error == EAGAIN || error == EWOULDBLOCK)
                    return;
                LOG_ERROR("读 timerfd 失败");
                throw std::system_error(error, std::generic_category(), "读 timerfd 失败");
            }
            LOG_ERROR("读 timerfd 返回了异常字节数");
            throw std::runtime_error("读 timerfd 返回了异常字节数");
        }
    }

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
