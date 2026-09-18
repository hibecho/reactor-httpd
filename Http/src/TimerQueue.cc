#include "TimerQueue.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Logger.hpp"
#include <cerrno>
#include <stdexcept>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace
{
// 时间轮只有整 tick 精度:0 归一为 1（下一个 tick 到期），
// 超过容量的夹紧到容量（而不是取模，取模会把"61 秒后"变成"1 秒后"）。
uint32_t NormalizeTimeout(uint32_t timeout, uint32_t capacity)
{
    if (timeout == 0)
        return 1;
    return timeout > capacity ? capacity : timeout;
}
} // namespace

TimerTask::TimerTask(uint64_t id, uint32_t timeout, const task_t &cb)
    : _id(id)
    , _timeout(timeout)
    , _task_cb(cb)
    , _canceled(false)
{}

void TimerTask::SetRelease(ReleaseFunc release)
{
    _release = std::move(release);
}

uint32_t TimerTask::GetTimeout()
{
    return _timeout;
}

void TimerTask::Cancel()
{
    _canceled = true;
}

// ~TimerTask的调用逻辑:
// tick 指向该槽位 -> 主动释放shared_ptr-> 触发~TimerTask -> 执行回调方法
TimerTask::~TimerTask()
{
    // 任务没有被取消
    if (!_canceled)
        _task_cb();
    // 清理 _timers 中的记录
    _release();
}

TimerWheel::TimerWheel(EventLoop *loop)
    : _loop(loop)
    , _timerfd(CreateTimerfd())
    , _timer_channel(std::make_unique<Channel>(_timerfd, _loop))
    , _tick(0)
    , _capacity(60)
    , _wheel(_capacity)
{
    _timer_channel->SetReadCallback([this]() {
        const uint64_t times = ReadTimerfd();
        // timerfd 记录的是"未读的到期次数"。进程被挂起、或某个回调耗时超过一个周期后
        // 会累积多次到期，只推进一格会让整个时间轮永久落后于真实时间。
        for (uint64_t i = 0; i < times; ++i)
            Tick();
    });
    _timer_channel->EnableRead();
}

TimerWheel::~TimerWheel()
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
            ptrtask->Cancel();
        }
    }
    _wheel.clear();

    // Channel 不会在析构时自动注销登记，而 _timer_channel 的 unique_ptr 要等到
    // 析构函数体结束才释放，所以必须在这里显式摘除；
    // 否则 Epoller 的映射会残留一个悬垂指针，
    // 该 fd 号被复用后新的 Channel 会在登记时被判身份不符。
    // 构造函数末尾的 EnableRead() 保证了它已登记，因此这里不会抛。
    _timer_channel->Remove();

    // 关闭 timerfd，否则每次构造都泄漏一个描述符
    if (close(_timerfd) < 0)
        LOG_ERROR("Failed to close timer fd");
}

void TimerWheel::TimerAdd(uint64_t id, uint32_t timeout, task_t cb)
{
    _loop->RunInLoop([this, id, timeout, cb]() { TimerAddInLoop(id, timeout, cb); });
}

// 向_wheel中添加定时任务
// id:为每个任务生成的编号
// timeout:触发定时任务的时间
void TimerWheel::TimerAddInLoop(uint64_t id, uint32_t timeout, task_t cb)
{
    // 归一化只做在入口，TimerTask 存的就是归一后的值，
    // TimerRefreshInLoop 用 GetTimeout() 重算槽位时天然一致。
    const uint32_t delay = NormalizeTimeout(timeout, _capacity);

    // 同 id 重复添加视为重启：先取消并摘除旧记录。
    // 旧任务此刻仍挂在自己的槽位上（要到该槽到期才会析构），取消后它到期时
    // 会静默销毁、不触发回调；这里先摘记录，是为了让新记录从写下的一刻起就归新任务所有。
    auto old = _timers.find(id);
    if (old != _timers.end())
    {
        // 将weak_ptr -> shared_ptr,只增加引用计数，不进行任务调用
        PtrTask task = old->second.lock();
        // 任务不为空则取消任务
        if (task)
        {
            task->Cancel();
        }
        // 摘掉旧记录
        _timers.erase(old);
    }
    // 添加新任务
    PtrTask pt = std::make_shared<TimerTask>(id, delay, cb);
    // 隐式转换：PtrTask-> WeakTask
    //
    const WeakTask self(pt);

    // 移除_timers中观察的指定 ID 任务，但只在记录仍属于 self 时移除
    // 防止因添加新任务更新，而删除了_timer中的新任务记录
    pt->SetRelease([this, id, self]() { RemoveTimerIfOwned(id, self); });

    // 根据时钟滴答位置，添加定时任务
    const auto pos = (_tick + delay) % _capacity;
    _wheel[pos].push_back(pt);
    _timers[id] = self;
} // 出作用域后，pt销毁，仅有_wheel[pos]管理任务

void TimerWheel::TimerRefresh(uint64_t id)
{
    _loop->RunInLoop([this, id]() { TimerRefreshInLoop(id); });
}

// 刷新/延迟_wheel中的任务
// 设计思路: 把同一个任务的智能指针，再放入新的时间槽，旧槽中的指针保留即可
// - 旧槽释放，只会进行引用计数--
// - 新槽释放才会进行回调任务
void TimerWheel::TimerRefreshInLoop(uint64_t id)
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

void TimerWheel::TimerCancel(uint64_t id)
{
    _loop->RunInLoop([this, id]() { TimerCancelInLoop(id); });
}

// 取消任务
void TimerWheel::TimerCancelInLoop(uint64_t id)
{
    // 任务没有找到则提前返回
    auto it = _timers.find(id);
    if (it == _timers.end())
    {
        return;
    }
    // 将WeakPtr变成PtrTask
    PtrTask task = it->second.lock();
    if (!task)
    {
        // 任务已经销毁，提前返回
        return;
    }
    task->Cancel();
}

// 每秒中执行一次
void TimerWheel::Tick()
{
    _tick = (_tick + 1) % _capacity;
    // 每秒执行一次定时任务
    std::vector<PtrTask> expired;
    expired.swap(_wheel[_tick]);
    // 主动将槽清空，假设这是任务最后一个 shared_ptr，执行析构调用任务
    expired.clear();
}

// RemoveTimerIfOwned:移除_timers中观察的指定 ID 任务，但只在记录仍属于 self 时移除
void TimerWheel::RemoveTimerIfOwned(uint64_t id, const WeakTask &self)
{
    auto it = _timers.find(id);
    if (it == _timers.end())
    {
        return;
    }

    // 本函数只在 ~TimerTask 中被调用，此刻 self 的强引用计数已经是 0：
    // weak_ptr::lock() 必然返回空，拿不到裸指针来比身份，只能比控制块。
    // owner_before 双向都不成立，才说明两条记录指向同一个对象。
    // 相当于 a < b || b < a   // 表示 a != b
    if (it->second.owner_before(self) || self.owner_before(it->second))
    {
        return; // 该 id 已被更晚添加的任务接管，不能删
    }
    _timers.erase(it);
}

int TimerWheel::CreateTimerfd()
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

uint64_t TimerWheel::ReadTimerfd()
{
    uint64_t times = 0;
    while (true)
    {
        const ssize_t ret = read(_timerfd, &times, sizeof(times));

        if (ret == static_cast<ssize_t>(sizeof(times)))
            return times;

        if (ret == -1)
        {
            const int error = errno;

            if (error == EINTR)
                continue;

            if (error == EAGAIN || error == EWOULDBLOCK)
                return 0;
            LOG_ERROR("读 timerfd 失败");
            throw std::system_error(error, std::generic_category(), "读 timerfd 失败");
        }
        LOG_ERROR("读 timerfd 返回了异常字节数");
        throw std::runtime_error("读 timerfd 返回了异常字节数");
    }
}
