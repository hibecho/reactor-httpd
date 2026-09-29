// TimerQueue 测试：时间轮算法（确定性）与 timerfd 集成（真实秒级）。
//
// 两类驱动：
//   - 确定性用例直接调 TimerAddInLoop / TimerRefreshInLoop / TimerCancelInLoop / Tick，
//     完全不碰 timerfd，毫秒级完成，覆盖到期、回绕、刷新、取消、重复添加、析构；
//   - 真实时间用例经 EventLoop::LoopOnce() 走 timerfd -> Channel -> Tick -> 任务，
//     验证集成链路与 EventLoop 的转发，合计约 10 秒。
//
// 时间轮容量固定 60，于是有两条边界：
//   - 最小延迟 1 个 tick：timeout == 0 被归一为 1，且不会在添加时就地同步回调；
//   - 最大延迟 capacity 个 tick：timeout > 60 被夹紧到 60（按取模处理会让 61 秒变成 1 秒）。
//
// 每个用例都按 `EventLoop loop; TimerWheel wheel(&loop);` 的顺序声明，
// 保证析构顺序是先 wheel 后 loop——~TimerWheel 要经 _loop 摘除 Epoller 上的登记。
//
// 真实时间用例每次 LoopOnce() 必定被唤醒：timerfd 是 1 秒周期的周期定时器，
// 即便没有任何任务到期待处理也每秒产生一次 EPOLLIN，因此不需要额外的唤醒源。
//
// 故障用例 ConstructorFailureReleasesTimerfd 用链接期 --wrap=epoll_ctl 注入登记失败，
// 覆盖构造函数抛出、~TimerWheel 不执行时 timerfd 仍要释放这条路径。

#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"
#include "reactor/TimerQueue.hpp"

#include <dirent.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

static_assert(!std::is_copy_constructible<TimerWheel>::value, "TimerWheel owns its timerfd");
static_assert(!std::is_copy_assignable<TimerWheel>::value, "TimerWheel owns its timerfd");

#define CHECK(expr)                                                                                                      \
    do                                                                                                                   \
    {                                                                                                                    \
        if (!(expr))                                                                                                     \
            throw std::runtime_error(                                                                                    \
                std::string(__func__) + ":" + std::to_string(__LINE__) + " " #expr + " errno=" + std::to_string(errno)); \
    } while (false)

namespace
{
// 看门狗：不用裸 alarm(5)。SIGALRM 的默认动作是直接终止进程，make 只会看到
// "Alarm clock" 而无法定位是哪条断言挂住。这里只做异步信号安全的 write 与 _exit。
// 预算放到 30 秒，因为存在合计约 10 秒的真实时间用例。
void OnAlarm(int)
{
    const char message[] = "TIMEOUT: 看门狗超时，疑似阻塞在 LoopOnce() 的 epoll_wait\n";
    const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(EXIT_FAILURE);
}

void ArmWatchdog()
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = OnAlarm;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // 不设 SA_RESTART，让被中断的系统调用返回 EINTR
    if (sigaction(SIGALRM, &action, nullptr) != 0)
    {
        std::cerr << "sigaction 失败\n";
        std::exit(EXIT_FAILURE);
    }
    alarm(30);
}
} // namespace

// 链接期故障注入：武装后 epoll_ctl 返回 EINVAL，未武装时透传。
// 用于命中 "TimerWheel 构造函数体内登记失败" 这条路径。
static std::atomic<bool> fail_epoll_ctl{false};

extern "C" int __real_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
extern "C" int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    if (fail_epoll_ctl.load())
    {
        errno = EINVAL;
        return -1;
    }
    return __real_epoll_ctl(epfd, op, fd, event);
}

// /proc/self/fd 的计数包含 opendir 自身的描述符，前后都算因而相互抵消，
// 所以只能做前后相等比较，不能对绝对数值断言。
static int CountOpenFds()
{
    DIR *dir = opendir("/proc/self/fd");
    CHECK(dir != nullptr);
    int count = 0;
    while (readdir(dir) != nullptr)
        ++count;
    closedir(dir);
    return count;
}

static void TickN(TimerWheel &wheel, int count)
{
    for (int i = 0; i < count; ++i)
        wheel.Tick();
}

// 创建一个非阻塞 eventfd，用作独立于定时器的描述符
static int MakeEventfd()
{
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

// 到期槽位 = 当前刻度 + timeout，第 timeout 次 Tick 触发，且只触发一次
static void ExpireAfterTimeout()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 3, [&fired]() { ++fired; });

    TickN(wheel, 2);
    CHECK(fired == 0);
    TickN(wheel, 1);
    CHECK(fired == 1);

    // 再走满一圈：任务已从表盘与索引中清除，不得第二次触发
    TickN(wheel, 60);
    CHECK(fired == 1);
}

// timeout == 0 归一为 1 个 tick：下一个 tick 到期，而不是绕满整圈（60 tick）
static void TimeoutZeroFiresOnNextTick()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 0, [&fired]() { ++fired; });

    // 添加时不就地同步回调，最小精度就是一个 tick
    CHECK(fired == 0);
    wheel.Tick();
    CHECK(fired == 1);
}

// timeout == capacity 是合法的最大延迟：绕满一整圈才到期
static void TimeoutAtCapacityFiresOnFullRevolution()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 60, [&fired]() { ++fired; });

    TickN(wheel, 59);
    CHECK(fired == 0);
    TickN(wheel, 1);
    CHECK(fired == 1);
}

// timeout > capacity 夹紧到 capacity。若按取模处理，timeout=61 会落在第 1 槽，
// 一次 Tick 后就触发——想要 61 秒却 1 秒就响。
static void TimeoutAboveCapacityIsClamped()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 61, [&fired]() { ++fired; });

    TickN(wheel, 1);
    CHECK(fired == 0);
    TickN(wheel, 58);
    CHECK(fired == 0);
    TickN(wheel, 1);
    CHECK(fired == 1);
}

// 槽位取模回绕：刻度已过 58 时添加 timeout=2，落点回绕到第 0 槽
static void IndexWrapsAroundWheel()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    TickN(wheel, 58); // _tick == 58
    wheel.TimerAddInLoop(1, 2, [&fired]() { ++fired; });

    TickN(wheel, 1); // _tick == 59，未到
    CHECK(fired == 0);
    TickN(wheel, 1); // _tick == 0，到期
    CHECK(fired == 1);
}

// 取消后即便槽位到期，回调也不得执行（任务被销毁时跳过回调）
static void CancelPreventsCallback()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 2, [&fired]() { ++fired; });
    wheel.TimerCancelInLoop(1);

    TickN(wheel, 4);
    CHECK(fired == 0);
}

// 未知 ID 的取消与刷新都是空操作，不得影响其它任务
static void UnknownIdIsNoop()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 1, [&fired]() { ++fired; });

    wheel.TimerCancelInLoop(999);
    wheel.TimerRefreshInLoop(999);

    wheel.Tick();
    CHECK(fired == 1);
}

// 刷新从当前刻度重新计时：原定第 3 次 Tick 到期，刷新后顺延到第 4 次
static void RefreshReschedulesFromCurrentTick()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int fired = 0;
    wheel.TimerAddInLoop(1, 3, [&fired]() { ++fired; });

    wheel.Tick();                // _tick == 1
    wheel.TimerRefreshInLoop(1); // 落点重算为 (1 + 3) % 60 == 4

    TickN(wheel, 2); // _tick == 3，原定到期点
    CHECK(fired == 0);
    TickN(wheel, 1); // _tick == 4
    CHECK(fired == 1);

    // 旧槽位只是引用计数--，不得留下第二份引用导致再次触发
    TickN(wheel, 60);
    CHECK(fired == 1);
}

// 到期后索引中的记录已清除：同一 ID 重新添加能正常触发，刷新/取消也不崩
static void ReAddSameIdAfterExpiry()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int first = 0;
    int second = 0;
    wheel.TimerAddInLoop(1, 1, [&first]() { ++first; });
    wheel.Tick();
    CHECK(first == 1);

    wheel.TimerRefreshInLoop(1); // 已到期并移除：直接返回
    wheel.TimerCancelInLoop(1);

    wheel.TimerAddInLoop(1, 1, [&second]() { ++second; });
    wheel.Tick();
    CHECK(second == 1);
}

// 同 id 重复添加视为重启：旧任务被取消，只有新任务触发
static void ReAddSameIdReplacesOldTask()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int old_fired = 0;
    int new_fired = 0;
    wheel.TimerAddInLoop(1, 1, [&old_fired]() { ++old_fired; }); // 原定第 1 次 Tick 触发
    wheel.TimerAddInLoop(1, 3, [&new_fired]() { ++new_fired; }); // 重启，改到第 3 次

    TickN(wheel, 1); // 旧任务的原定到期点
    CHECK(old_fired == 0);
    TickN(wheel, 2);
    CHECK(new_fired == 1);
    CHECK(old_fired == 0);
}

// 旧任务被替换后，它析构时执行的清理不得删掉新任务的登记。
// 这条用例专门盯住索引错删：两个旧任务都在 _tick == 1 销毁并各调用一次 _release，
// 若那次清理按 id 无条件删除，下面对新任务的取消与刷新就会查不到而静默失效。
static void IndexSurvivesOldTaskCleanup()
{
    EventLoop loop;
    TimerWheel wheel(&loop);
    int new1_fired = 0;
    int new2_fired = 0;

    // id=1 用来验证替换后取消仍然有效
    wheel.TimerAddInLoop(1, 1, []() {});                           // 旧任务，落点 slot 1
    wheel.TimerAddInLoop(1, 5, [&new1_fired]() { ++new1_fired; }); // 新任务，落点 slot 5

    // id=2 用来验证替换后刷新仍然有效
    wheel.TimerAddInLoop(2, 1, []() {});                           // 旧任务，落点 slot 1
    wheel.TimerAddInLoop(2, 5, [&new2_fired]() { ++new2_fired; }); // 新任务，落点 slot 5

    wheel.Tick(); // _tick == 1：两个旧任务在此销毁，各自执行一次清理

    wheel.TimerCancelInLoop(1);  // 必须仍能找到 id=1 的新任务
    wheel.TimerRefreshInLoop(2); // 必须仍能找到 id=2 的新任务，重算为 (1 + 5) % 60 == 6

    TickN(wheel, 4); // _tick == 5：新任务的原定到期点
    CHECK(new1_fired == 0); // 取消生效
    CHECK(new2_fired == 0); // 刷新已把它顺延走

    TickN(wheel, 1); // _tick == 6：刷新后的到期点
    CHECK(new2_fired == 1);
    CHECK(new1_fired == 0);
}

// 析构时取消所有待触发任务：表盘清空触发 TimerTask 析构，但回调不得执行
static void DestructorCancelsPendingTasks()
{
    EventLoop loop;
    int fired = 0;
    {
        TimerWheel wheel(&loop);
        wheel.TimerAddInLoop(1, 3, [&fired]() { ++fired; });
        wheel.Tick();
    } // ~TimerWheel

    CHECK(fired == 0);
}

// ~TimerWheel 必须关闭 timerfd。反复构造析构不应累积描述符，
// 每次循环都让任务真的触发一次，确保走的是完整的添加-到期-清理路径。
static void DestructorReleasesTimerfd()
{
    EventLoop loop;
    const int before = CountOpenFds();

    for (int i = 0; i < 64; ++i)
    {
        TimerWheel wheel(&loop);
        int fired = 0;
        wheel.TimerAddInLoop(1, 1, [&fired]() { ++fired; });
        wheel.Tick();
        CHECK(fired == 1);
    }

    CHECK(CountOpenFds() == before);
}

// TimerWheel 构造函数体内登记 timerfd 失败会抛异常，此时 ~TimerWheel 不会执行，
// timerfd 必须由成员自己负责关闭。
// 注入 epoll_ctl 失败即命中这条路径（EnableRead -> EventLoop::UpdateEvent -> Epoller::AddEvent）。
static void ConstructorFailureReleasesTimerfd()
{
    EventLoop loop; // 先正常构造，武装只覆盖随后的 TimerWheel 构造
    const int before = CountOpenFds();

    fail_epoll_ctl.store(true);
    bool threw = false;
    try
    {
        TimerWheel wheel(&loop);
    }
    catch (const std::system_error &)
    {
        threw = true;
    }
    fail_epoll_ctl.store(false);

    CHECK(threw);
    CHECK(CountOpenFds() == before);
}

// ~TimerWheel 必须把 timerfd 的 Channel 从 Epoller 摘除。
// 析构释放的 fd 号会被内核复用给下一个 timerfd；若残留陈旧登记，
// 新 Channel 登记时会因身份不符而被 Epoller 拒绝。
static void DestructorUnregistersChannel()
{
    EventLoop loop;

    for (int i = 0; i < 4; ++i)
    {
        TimerWheel wheel(&loop); // 反复复用上一轮析构释放的 fd 号
        wheel.Tick();
    }

    // 事件登记能力仍然正常
    const int fd = MakeEventfd();
    Channel channel(fd, &loop);
    channel.EnableRead();
    channel.Remove();
    CHECK(close(fd) == 0);
}

// 真实时间：timerfd -> Channel -> Tick -> 任务
static void LoopDrivenExpiry()
{
    EventLoop loop;
    int fired1 = 0;
    int fired2 = 0;
    loop.TimerAdd(1, 1, [&fired1]() { ++fired1; });
    loop.TimerAdd(2, 2, [&fired2]() { ++fired2; });

    loop.LoopOnce(); //约 1 秒
    CHECK(fired1 == 1);
    CHECK(fired2 == 0);

    loop.LoopOnce(); //约 1 秒
    CHECK(fired2 == 1);
    CHECK(fired1 == 1); // 只触发一次
}

// 真实时间：EventLoop::TimerCancel 必须转发到取消。若转发成刷新，任务照旧触发。
static void LoopDrivenCancel()
{
    EventLoop loop;
    int fired = 0;
    loop.TimerAdd(1, 1, [&fired]() { ++fired; });
    loop.TimerCancel(1);

    loop.LoopOnce(); //约 1 秒
    loop.LoopOnce(); //约 1 秒
    CHECK(fired == 0);
}

// 真实时间：EventLoop::TimerRefresh 必须转发到刷新。若转发成取消，任务永不触发。
static void LoopDrivenRefresh()
{
    EventLoop loop;
    int fired = 0;
    loop.TimerAdd(1, 2, [&fired]() { ++fired; }); // 原定第 2 秒到期

    loop.LoopOnce(); //第 1 秒
    CHECK(fired == 0);
    loop.TimerRefresh(1); // 从当前刻度重新计时，顺延到第 3 秒

    loop.LoopOnce(); //第 2 秒，原定到期时刻
    CHECK(fired == 0);
    loop.LoopOnce(); //第 3 秒
    CHECK(fired == 1);
}

// 真实时间：进程落后多个周期后，一次读入必须补齐对应数量的 tick。
// 若只推进一格，时间轮会永久落后于真实时间。
static void CatchUpAfterStall()
{
    EventLoop loop;
    int fired = 0;
    loop.TimerAdd(1, 3, [&fired]() { ++fired; });

    // 不调用 LoopOnce()，让 timerfd 累积至少 3 次到期。
    // 0.2 秒余量用于避开"第 3 次到期恰好卡在睡眠结束瞬间"的边界抖动。
    struct timespec stall;
    stall.tv_sec = 3;
    stall.tv_nsec = 200000000L;
    CHECK(nanosleep(&stall, nullptr) == 0);
    CHECK(fired == 0); // 没有事件分发，回调不可能跑

    loop.LoopOnce(); //一次读入 3 次到期，推进 3 格
    CHECK(fired == 1);
}

int main()
{
    try
    {
        ArmWatchdog();
        ExpireAfterTimeout();
        TimeoutZeroFiresOnNextTick();
        TimeoutAtCapacityFiresOnFullRevolution();
        TimeoutAboveCapacityIsClamped();
        IndexWrapsAroundWheel();
        CancelPreventsCallback();
        UnknownIdIsNoop();
        RefreshReschedulesFromCurrentTick();
        ReAddSameIdAfterExpiry();
        ReAddSameIdReplacesOldTask();
        IndexSurvivesOldTaskCleanup();
        DestructorCancelsPendingTasks();
        DestructorReleasesTimerfd();
        ConstructorFailureReleasesTimerfd();
        DestructorUnregistersChannel();
        LoopDrivenExpiry();
        LoopDrivenCancel();
        LoopDrivenRefresh();
        CatchUpAfterStall();
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }

    std::cout << "TimerQueue tests passed: 到期槽位、timeout 上下界归一、刻度回绕、"
                 "取消、刷新重算、重复添加（到期后重加与按 id 重启）、旧任务清理后索引仍可用、"
                 "析构取消待触发任务、析构释放 timerfd、构造中途失败时不泄漏 timerfd、"
                 "析构摘除 Epoller 登记、timerfd 集成、Cancel/Refresh 转发、落后补偿。\n";
    return 0;
}
