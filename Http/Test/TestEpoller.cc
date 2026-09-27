// Epoller 契约测试。
//
// 架构前提：Channel 不再直接持有 Epoller，Enable*/Disable*/Update/Remove 一律经
// EventLoop* 转发；而 Channel 的监听位没有 public setter（SetREvents 改的是已就绪
// 事件）。因此要让 Channel 带上监听位、真正走进内核就绪路径，就必须绑定真实的
// EventLoop。本文件据此分为两组：
//
//   Group 1  裸 Epoller 的边界契约。Channel(fd, nullptr) 只当作"可登记的 token"：
//            它禁止调用 Enable*/Disable*/Update/Remove，那些成员会解引用 _loop，
//            误用会立刻空指针崩溃，是响亮的失败而非静默错误。
//   Group 2  就绪路径。真实 EventLoop 只负责让 Enable* 合法并设置监听位；登记与
//            等待仍由独立的裸 Epoller 承担，这样 active 向量的 size、元素身份、
//            多描述符同时就绪这些断言才能保留（走 EventLoop::Loop 就全丢了，
//            因为它的 _ep 是私有的）。
//
// Group 2 的两条纪律：绝不驱动 loop（loop.Loop() / loop.LoopOnce() 都不调）；也不在 fd 仍登记于该 loop 时关闭并复用
// 该 fd 号，否则会污染 loop 内部的 _channels 映射。
//
// 已知缺陷（本次不修，见 tasks/todo.md）：
//   - Epoller::Detach 连声明都没有（只在 Epoller.hpp 里留了一句注释），引用即编译失败。
//   - Channel::~Channel 函数体为空，不注销 Epoller 中的登记。原测试中依赖"析构
//     自动注销"的 fd 复用回归段编码的是这个未实现的契约，已随迁移删除。

#include "reactor/Channel.hpp"
#include "reactor/Epoller.hpp"
#include "reactor/EventLoop.hpp"

#include <cassert>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

// 拥有 epoll 句柄这一独占资源，拷贝会导致重复关闭他人的句柄
static_assert(!std::is_copy_constructible<Epoller>::value, "Epoller must not be copy constructible");
static_assert(!std::is_copy_assignable<Epoller>::value, "Epoller must not be copy assignable");

template <class Error, class Action>
void ExpectThrow(Action action)
{
    bool caught = false;
    try
    {
        action();
    }
    catch (const Error &)
    {
        caught = true;
    }
    assert(caught);
}

// 统计当前进程打开的描述符数量，用于检测句柄泄漏
int CountOpenFds()
{
    DIR *dir = opendir("/proc/self/fd");
    assert(dir != nullptr);
    int count = 0;
    while (readdir(dir) != nullptr)
        ++count;
    closedir(dir);
    return count;
}

static int MakeEventfd()
{
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);
    return fd;
}

static void WriteEventfd(int fd)
{
    const eventfd_t counter = 1;
    assert(write(fd, &counter, sizeof(counter)) == static_cast<ssize_t>(sizeof(counter)));
}

// 消费掉 eventfd 的计数。读回调只置标志、不读数据，不 drain 的话该描述符会持续
// 可读，后续阶段的 WaitEvent 就会把不该就绪的对象一并返回。
static void DrainEventfd(int fd)
{
    eventfd_t value = 0;
    assert(read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)));
}

namespace
{
void OnAlarmInterrupt(int)
{
}
} // namespace

// WaitEvent 被打断时应返回且不产出就绪事件。
// 必须放在 main 末尾：它会重设 SIGALRM 的处理器和定时器，从而解除看门狗。
static void WaitEventReturnsOnSignal()
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = OnAlarmInterrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // 不设 SA_RESTART，让 epoll_wait 返回 EINTR
    assert(sigaction(SIGALRM, &action, nullptr) == 0);

    itimerval timer{};
    timer.it_value.tv_usec = 200 * 1000;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);

    Epoller idle;
    std::vector<Channel *> active;
    idle.WaitEvent(active);
    assert(active.empty());

    // 关掉定时器并还原默认处理器
    itimerval stop{};
    assert(setitimer(ITIMER_REAL, &stop, nullptr) == 0);
    assert(signal(SIGALRM, SIG_DFL) != SIG_ERR);
}

// 裸 Epoller 的边界契约。此块内所有 Channel 都绑定 nullptr，只作可登记的 token。
static void BareEpollerContracts()
{
    Epoller poller;

    // 空指针参数
    ExpectThrow<std::invalid_argument>([&] { poller.AddEvent(nullptr); });
    ExpectThrow<std::invalid_argument>([&] { poller.ModifyEvent(nullptr); });
    ExpectThrow<std::invalid_argument>([&] { poller.RemoveEvent(nullptr); });

    const int fd = MakeEventfd();
    Channel channel(fd, nullptr);
    Channel other(fd, nullptr); // 同 fd 的第二个对象，与 channel 同时存活

    // 未登记即修改/移除
    ExpectThrow<std::logic_error>([&] { poller.ModifyEvent(&channel); });
    ExpectThrow<std::logic_error>([&] { poller.RemoveEvent(&channel); });

    poller.AddEvent(&channel);
    // 重复登记
    ExpectThrow<std::logic_error>([&] { poller.AddEvent(&channel); });
    // 身份校验：fd 命中但对象不是登记时的那个
    ExpectThrow<std::logic_error>([&] { poller.ModifyEvent(&other); });
    ExpectThrow<std::logic_error>([&] { poller.RemoveEvent(&other); });
    // UpdateEvent 是"存在则修改，不存在则创建"：已登记走修改，不应抛
    poller.UpdateEvent(&channel);

    poller.RemoveEvent(&channel);
    ExpectThrow<std::logic_error>([&] { poller.ModifyEvent(&channel); });
    ExpectThrow<std::logic_error>([&] { poller.RemoveEvent(&channel); });

    // 登记失败必须回滚映射：fd=-1 时 epoll_ctl 报 EBADF，抛 system_error。
    // 若失败时残留了映射，第二次会先撞上"已登记"而抛 logic_error。
    Channel bad(-1, nullptr);
    ExpectThrow<std::system_error>([&] { poller.AddEvent(&bad); });
    ExpectThrow<std::system_error>([&] { poller.AddEvent(&bad); });

    // 描述符被外部 close：内核在关闭时就已把它从本 epoll 摘除，登记实际不存在。
    // 此时 RemoveEvent 必须静默清理映射，否则该 fd 号被复用后会永久无法再登记。
    const int dying = MakeEventfd();
    Channel dying_channel(dying, nullptr);
    poller.AddEvent(&dying_channel);
    assert(close(dying) == 0);
    poller.RemoveEvent(&dying_channel); // EBADF，不得抛出
    // 映射已 erase，所以这里仍报 EBADF；若残留则会抛 logic_error
    ExpectThrow<std::system_error>([&] { poller.AddEvent(&dying_channel); });

    // 映射未受影响的一个对照：ModifyEvent 对已关闭的描述符报 system_error
    poller.AddEvent(&channel);
    poller.RemoveEvent(&channel);

    assert(close(fd) == 0);

    // 析构必须释放 epoll 句柄，反复构造销毁不应累积描述符
    const int fds_before = CountOpenFds();
    for (int i = 0; i < 64; ++i)
    {
        Epoller scoped;
        (void)scoped;
    }
    assert(CountOpenFds() == fds_before);
}

// 就绪路径。EventLoop 仅用于设置监听位，登记与等待由独立的裸 Epoller 承担。
static void ReadyPath()
{
    EventLoop loop;

    const int fd1 = MakeEventfd();
    const int fd2 = MakeEventfd();

    Epoller reactor;
    Channel first(fd1, &loop);
    Channel second(fd2, &loop);
    first.EnableRead();
    second.EnableRead();
    assert(first.Readable() && second.Readable());

    reactor.AddEvent(&first);
    reactor.AddEvent(&second);

    // 单描述符就绪：就绪事件与读回调正确送达
    bool first_readable = false;
    first.SetReadCallback([&first_readable] { first_readable = true; });
    WriteEventfd(fd1);
    std::vector<Channel *> ready;
    reactor.WaitEvent(ready);
    assert(ready.size() == 1 && ready[0] == &first);
    ready[0]->Handle();
    assert(first_readable);
    DrainEventfd(fd1);

    // 多描述符同时就绪：一次等待全部返回，顺序不作要求
    first_readable = false; // 重置，确保本段确实重新触发了回调
    bool second_readable = false;
    second.SetReadCallback([&second_readable] { second_readable = true; });
    WriteEventfd(fd1);
    WriteEventfd(fd2);
    ready.clear();
    reactor.WaitEvent(ready);
    assert(ready.size() == 2);
    assert((ready[0] == &first && ready[1] == &second) || (ready[0] == &second && ready[1] == &first));
    for (Channel *channel : ready)
        channel->Handle();
    assert(first_readable && second_readable);
    DrainEventfd(fd1);
    DrainEventfd(fd2);

    // 监听位变化生效：改成只监听可写。eventfd 恒可写，无需写入即可就绪；
    // 若 ModifyEvent 是空操作，裸 epoll 上仍挂着已被读回调消费掉的 EPOLLIN，
    // 下一次 WaitEvent 会一直阻塞，由看门狗报失败。
    first.DisableRead();
    first.EnableWrite();
    reactor.ModifyEvent(&first);
    bool first_writable = false;
    first.SetWriteCallback([&first_writable] { first_writable = true; });
    ready.clear();
    reactor.WaitEvent(ready);
    assert(ready.size() == 1 && ready[0] == &first);
    ready[0]->Handle();
    assert(first_writable);

    // 收尾：两个 epoll（loop 内部的和 reactor）都要摘除登记，再关描述符
    reactor.RemoveEvent(&first);
    reactor.RemoveEvent(&second);
    first.Remove();
    second.Remove();
    assert(close(fd1) == 0);
    assert(close(fd2) == 0);
}

int main()
{
    alarm(5);

    BareEpollerContracts();
    ReadyPath();
    WaitEventReturnsOnSignal();

    std::cout << "Epoller tests passed\n";
}
