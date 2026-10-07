// EventLoop 测试：构造、线程归属、任务队列与唤醒、事件优先于任务、
// Channel 事件登记端到端。
//
// 关键前提：LoopOnce() 内部的 epoll_wait 超时是 -1，没有任何事件时会永久阻塞。
// 因此每次调用 LoopOnce() 之前都必须保证存在唤醒源，二者之一：
//   - 有挂起任务：QueueInLoop 与跨线程 RunInLoop 已经写过 eventfd；
//   - 有已登记的 fd 就绪：写入该 fd，使其注册的事件就绪。
// Channel::Remove 之后 fd 不再产生唤醒，此时必须补一个 QueueInLoop 作为独立唤醒源。
//
// ~EventLoop 会关闭 _event_fd，文件末尾的 DestructorReleasesFd 据此断言反复构造析构
// 不会累积描述符；ConstructorFailureReleasesFd 覆盖构造函数中途失败这条路径。

#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"

#include <dirent.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
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
#include <thread>
#include <type_traits>

static_assert(!std::is_copy_constructible<EventLoop>::value, "EventLoop owns its eventfd");
static_assert(!std::is_copy_assignable<EventLoop>::value, "EventLoop owns its eventfd");

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #expr +              \
                                     " errno=" + std::to_string(errno));                                               \
    } while (false)

namespace
{
    // 看门狗：不用裸 alarm(5)。SIGALRM 的默认动作是直接终止进程，make 只会看到
    // "Alarm clock" 而无法定位是哪条断言挂住。这里只做异步信号安全的 write 与 _exit。
    void OnAlarm(int)
    {
        const char message[] = "TIMEOUT: 5 秒内未完成，疑似阻塞在 LoopOnce() 的 epoll_wait\n";
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
        alarm(5);
    }
} // namespace

// 链接期故障注入：武装后 timerfd_create 返回 EMFILE，未武装时透传。
// EventLoop 内部必然构造一个 TimerWheel，因此武装期间构造 EventLoop 会命中
// "成员初始化列表抛异常" 这条路径。
static std::atomic<bool> fail_timerfd_create{false};

extern "C" int __real_timerfd_create(clockid_t clockid, int flags);
extern "C" int __wrap_timerfd_create(clockid_t clockid, int flags)
{
    if (fail_timerfd_create.load())
    {
        errno = EMFILE;
        return -1;
    }
    return __real_timerfd_create(clockid, flags);
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

template <class Action> static bool ThrowsLogicError(Action action)
{
    try
    {
        action();
    }
    catch (const std::logic_error &)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
    return false;
}

// 创建一个非阻塞 eventfd，用作被 EventLoop 管理的连接描述符
static int MakeEventfd()
{
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

// 写入 eventfd 使其变为可读
static void Notify(int fd)
{
    const uint64_t one = 1;
    CHECK(write(fd, &one, sizeof(one)) == static_cast<ssize_t>(sizeof(one)));
}

// 构造期间 _event_channel->EnableRead() 会经 Channel::Update 转发到
// _ep->UpdateEvent；_ep 若未初始化，这里就会解引用空指针。
static void ConstructAndThreadIdentity()
{
    EventLoop loop;
    CHECK(loop.IsInLoopThread());

    std::atomic<bool> same_from_worker(true);
    std::thread worker([&loop, &same_from_worker]() {
        same_from_worker.store(loop.IsInLoopThread());
    });
    worker.join();
    CHECK(!same_from_worker.load());
}

// 同线程调用必须立即执行，不能等到 LoopOnce()
static void RunInLoopSameThread()
{
    EventLoop loop;
    std::string order;
    loop.RunInLoop([&order]() {
        order += 'A';
    });
    loop.RunInLoop([&order]() {
        order += 'B';
    });
    CHECK(order == "AB");
}

// 入队不等于执行；QueueInLoop 写入 eventfd，LoopOnce() 必定被唤醒
static void QueueInLoopDefersUntilLoop()
{
    EventLoop loop;
    bool ran = false;
    loop.QueueInLoop([&ran]() {
        ran = true;
    });
    CHECK(!ran);

    loop.LoopOnce();
    CHECK(ran);
}

// 跨线程 RunInLoop 走队列 + eventfd 唤醒，且任务在所属线程执行
static void CrossThreadRunInLoop()
{
    EventLoop loop;
    const std::thread::id loop_thread = std::this_thread::get_id();
    std::atomic<bool> ran(false);
    std::thread::id task_thread{};

    std::thread worker([&loop, &ran, &task_thread]() {
        loop.RunInLoop([&ran, &task_thread]() {
            task_thread = std::this_thread::get_id();
            ran.store(true);
        });
    });

    // worker 无论快慢都能唤醒：先写 eventfd 则 Loop 立即返回，后写则 Loop 阻塞等待
    loop.LoopOnce();
    worker.join();

    CHECK(ran.load());
    CHECK(task_thread == loop_thread);
}

// 一次 LoopOnce() 执行同一批入队的全部任务
static void BatchQueueInLoop()
{
    EventLoop loop;
    const int count = 8;
    int executed = 0;
    for (int i = 0; i < count; ++i)
        loop.QueueInLoop([&executed, i]() {
            executed += i + 1;
        });
    loop.LoopOnce();
    CHECK(executed == count * (count + 1) / 2);
}

// ExecuteTasks 先把队列整体换出再执行，本轮新入队的任务留到下一轮
static void TaskQueueSwapSemantics()
{
    EventLoop loop;
    bool second_ran = false;
    loop.QueueInLoop([&loop, &second_ran]() {
        loop.QueueInLoop([&second_ran]() {
            second_ran = true;
        });
    });

    loop.LoopOnce();
    CHECK(!second_ran);

    // 内层 QueueInLoop 又写过 eventfd，下一次 LoopOnce() 必定被唤醒
    loop.LoopOnce();
    CHECK(second_ran);
}

// 一轮先处理就绪事件，再执行任务队列
static void EventsBeforeTasks()
{
    EventLoop loop;
    const int fd = MakeEventfd();
    Channel channel(fd, &loop);
    std::string order;
    channel.SetReadCallback([&order]() {
        order += 'C';
    });
    channel.EnableRead();

    loop.QueueInLoop([&order]() {
        order += 'T';
    });
    Notify(fd);

    loop.LoopOnce();
    CHECK(order == "CT");

    channel.Remove();
    CHECK(close(fd) == 0);
}

// 监听位变化经 Channel::Update 转发到 Epoller 的 Modify 路径
static void ModifyPath()
{
    EventLoop loop;
    const int fd = MakeEventfd();
    Channel channel(fd, &loop);
    bool readable = false;
    bool writable = false;
    channel.SetReadCallback([&readable]() {
        readable = true;
    });
    channel.SetWriteCallback([&writable]() {
        writable = true;
    });
    channel.EnableRead();

    // 改为只监听可写：eventfd 恒可写，无需写入即可就绪
    channel.DisableRead();
    channel.EnableWrite();

    loop.LoopOnce();
    CHECK(writable);
    CHECK(!readable);

    channel.Remove();
    CHECK(close(fd) == 0);
}

// Channel::Remove 必须真的摘除登记，否则 fd 就绪时回调仍会触发
static void RemovePath()
{
    EventLoop loop;
    const int fd = MakeEventfd();
    Channel channel(fd, &loop);
    bool called = false;
    channel.SetReadCallback([&called]() {
        called = true;
    });
    channel.SetEventCallback([&called]() {
        called = true;
    });
    channel.EnableRead();

    channel.Remove();

    // fd 仍可读但已从 Epoller 摘除，必须补一个任务作为独立唤醒源
    Notify(fd);
    bool woke = false;
    loop.QueueInLoop([&woke]() {
        woke = true;
    });

    loop.LoopOnce();
    CHECK(woke);
    CHECK(!called);

    CHECK(close(fd) == 0);
}

// 二次 Remove 走到 Epoller::RequireRegistered 的未登记分支
static void RemoveIsIdentityChecked()
{
    EventLoop loop;
    const int fd = MakeEventfd();
    Channel channel(fd, &loop);
    channel.EnableRead();
    channel.Remove();
    CHECK(ThrowsLogicError([&channel]() {
        channel.Remove();
    }));

    CHECK(close(fd) == 0);
}

// ~EventLoop 关闭 _event_fd，反复构造析构不应累积描述符。
// 这条断言在 ~EventLoop 缺失时会失败（每次构造泄漏一个 eventfd）。
static void DestructorReleasesFd()
{
    const int before = CountOpenFds();
    for (int i = 0; i < 64; ++i)
    {
        EventLoop loop;
        CHECK(loop.IsInLoopThread());
    }
    CHECK(CountOpenFds() == before);
}

// TimerWheel 是 EventLoop 的成员，在初始化列表里构造。它抛异常时异常来自初始化列表，
// ~EventLoop 不会执行——只有已构造完成的成员会析构，eventfd 必须由成员自己负责关闭。
// 注入 timerfd_create 失败即命中这条路径。
static void ConstructorFailureReleasesFd()
{
    const int before = CountOpenFds();

    fail_timerfd_create.store(true);
    bool threw = false;
    try
    {
        EventLoop loop;
    }
    catch (const std::system_error &)
    {
        threw = true;
    }
    fail_timerfd_create.store(false);

    CHECK(threw);
    CHECK(CountOpenFds() == before);
}

int main()
{
    try
    {
        ArmWatchdog();
        ConstructAndThreadIdentity();
        RunInLoopSameThread();
        QueueInLoopDefersUntilLoop();
        CrossThreadRunInLoop();
        BatchQueueInLoop();
        TaskQueueSwapSemantics();
        EventsBeforeTasks();
        ModifyPath();
        RemovePath();
        RemoveIsIdentityChecked();
        DestructorReleasesFd();
        ConstructorFailureReleasesFd();
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }

    std::cout << "EventLoop tests passed: 构造与线程归属、同线程立即执行、入队延迟执行、"
                 "跨线程唤醒、批量执行、队列换出语义、事件优先于任务、"
                 "Channel 登记/修改/移除的转发链路、析构释放 eventfd、"
                 "构造中途失败时不泄漏 eventfd。\n";
    return 0;
}
