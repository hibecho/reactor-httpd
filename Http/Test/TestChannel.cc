// Channel 契约测试。
//
// Channel 不再直接持有 Epoller：Enable*/Disable*/Update/Remove 一律经 EventLoop* 转发。
// 因此本文件按是否需要 EventLoop 分成两类：
//   - 只验证事件派发的用例（Handle/SetREvents/Readable/回调设置）用
//     Channel(fd, nullptr)，显式表达"与 loop 无关"；
//   - 涉及 Enable*/Disable*/Update/Remove 的用例绑定共享的 EventLoop。
//
// 纪律：凡被 Enable* 登记过的 Channel，离开作用域前必须显式 Remove()。DisableAll
// 只把监听置 0，登记仍在；而 ~Channel 不注销（见下方已知缺陷），残留项会让复用同号
// 描述符的后续用例抛 logic_error。
//
// 已知缺陷（本次不修，见 tasks/todo.md）：~Channel 没有声明，隐式析构不会从 Epoller
// 注销登记，而 Epoller.hpp 里提到 Detach 的那句注释宣称会注销（该 Detach 本身也已
// 不存在）。文件末尾的 DestructorDoesNotDetach 用一条确定性探测把该行为固定下来。

#include "reactor/Channel.hpp"
#include "reactor/EventLoop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

// 持有登记这一独占关系，拷贝构造出的对象析构时会注销原对象的登记
static_assert(!std::is_copy_constructible<Channel>::value, "Channel must not be copy constructible");
static_assert(!std::is_copy_assignable<Channel>::value, "Channel must not be copy assignable");

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

static void DrainEventfd(int fd)
{
    eventfd_t value = 0;
    assert(read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)));
}

// 事件开关的状态机与幂等性。这些调用会真实落到 epoll，fd 必须是有效描述符。
static void ToggleStateMachine(EventLoop &loop)
{
    const int fd = MakeEventfd();
    {
        Channel channel(fd, &loop);
        assert(!channel.Readable() && !channel.Writeable());

        channel.EnableRead();
        channel.EnableRead();
        assert(channel.Readable() && !channel.Writeable());

        channel.EnableWrite();
        channel.EnableWrite();
        assert(channel.Readable() && channel.Writeable());

        channel.DisableRead();
        channel.DisableRead();
        assert(!channel.Readable() && channel.Writeable());

        channel.EnableRead();
        channel.DisableWrite();
        channel.DisableWrite();
        assert(channel.Readable() && !channel.Writeable());

        channel.EnableWrite();
        channel.DisableAll();
        assert(!channel.Readable() && !channel.Writeable());

        // DisableAll 只把监听置 0，对象仍在 Epoller 中登记，显式注销应当成功
        channel.Remove();
    }
    assert(close(fd) == 0);
}

// 回调派发：用 SetREvents 直接注入就绪事件，不依赖内核，也不需要 EventLoop
static void HandleDispatchesAllEventCombinations()
{
    const int fd = MakeEventfd();
    Channel channel(fd, nullptr);
    const uint32_t events[] = {EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLHUP};
    const char labels[] = {'R', 'W', 'E', 'C'};
    std::string calls;
    channel.SetReadCallback([&calls] { calls += 'R'; });
    channel.SetWriteCallback([&calls] { calls += 'W'; });
    channel.SetErrorCallback([&calls] { calls += 'E'; });
    channel.SetCloseCallback([&calls] { calls += 'C'; });
    channel.SetEventCallback([&calls] { calls += 'A'; });

    for (unsigned mask = 0; mask < 16; ++mask)
    {
        uint32_t ready = 0;
        std::string expected;
        for (unsigned i = 0; i < 4; ++i)
            if (mask & (1u << i))
            {
                ready |= events[i];
                expected += labels[i];
            }
        if (ready)
            expected += 'A';
        calls.clear();
        channel.SetREvents(ready);
        channel.Handle();
        assert(calls == expected);
    }
    assert(close(fd) == 0);
}

// 监听与就绪是两回事：只监听不触发回调
static void ReadyAndInterestAreSeparate(EventLoop &loop)
{
    const int fd = MakeEventfd();
    {
        Channel channel(fd, &loop);
        std::string calls;
        channel.SetEventCallback([&calls] { calls += 'A'; });
        channel.EnableRead();
        channel.EnableWrite();
        channel.SetREvents(0);
        channel.Handle();
        assert(calls.empty()); // Monitoring alone must not trigger callbacks.
        channel.Remove();
    }
    assert(close(fd) == 0);
}

// 回调替换与清空：清空后该事件不再派发
static void CallbackReplacementAndClearing()
{
    const int fd = MakeEventfd();
    Channel channel(fd, nullptr);
    std::string calls;
    channel.SetEventCallback([&calls] { calls += 'A'; });
    channel.SetReadCallback([&calls] { calls += 'N'; });
    channel.SetREvents(EPOLLIN);
    channel.Handle();
    assert(calls == "NA");

    channel.SetReadCallback({});
    calls.clear();
    channel.Handle();
    assert(calls == "A");
    assert(close(fd) == 0);
}

// 空回调：任何事件组合都不得抛 bad_function_call
static void EmptyCallbacks()
{
    Channel empty(-1, nullptr);
    empty.SetREvents(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
    empty.Handle();
}

// 一轮派发使用本轮开始时的就绪快照，回调中途改写不影响本轮
static void EventSnapshot()
{
    const int fd = MakeEventfd();
    Channel channel(fd, nullptr);
    std::string calls;
    channel.SetReadCallback([&calls, &channel] {
        calls += 'R';
        channel.SetREvents(0);
    });
    channel.SetWriteCallback([&calls] { calls += 'W'; });
    channel.SetEventCallback([&calls] { calls += 'A'; });

    channel.SetREvents(EPOLLIN | EPOLLOUT);
    channel.Handle();
    assert(calls == "RWA"); // The current dispatch uses an event snapshot.

    calls.clear();
    channel.Handle();
    assert(calls.empty());
    assert(close(fd) == 0);
}

// 同 fd 的两个对象：第二个登记时因身份不符被拒
static void SameFdIdentityChecked(EventLoop &loop)
{
    const int fd = MakeEventfd();
    Channel first(fd, &loop);
    first.EnableRead();
    Channel second(fd, &loop); // 两者同时存活，地址必不相同
    assert(ThrowsLogicError([&second] { second.EnableRead(); }));
    first.Remove();
    assert(close(fd) == 0);
}

// 转发链路探针 1：Update -> Add 生效，描述符就绪时回调被触发
static void UpdateRegistersChannel(EventLoop &loop)
{
    const int fd = MakeEventfd();
    {
        Channel channel(fd, &loop);
        bool readable = false;
        channel.SetReadCallback([&readable] { readable = true; });
        channel.EnableRead();

        WriteEventfd(fd);
        loop.LoopOnce();
        assert(readable);

        DrainEventfd(fd);
        channel.Remove();
    }
    assert(close(fd) == 0);
}

// 转发链路探针 2：Update -> Modify 生效，改成只监听可写后读回调不再触发
static void UpdateModifiesChannel(EventLoop &loop)
{
    const int fd = MakeEventfd();
    {
        Channel channel(fd, &loop);
        bool readable = false;
        bool writable = false;
        channel.SetReadCallback([&readable] { readable = true; });
        channel.SetWriteCallback([&writable] { writable = true; });
        channel.EnableRead();

        // eventfd 恒可写，无需写入即可就绪
        channel.DisableRead();
        channel.EnableWrite();

        loop.LoopOnce();
        assert(writable);
        assert(!readable);

        channel.Remove();
    }
    assert(close(fd) == 0);
}

// 转发链路探针 3：Remove 生效，登记摘除后描述符就绪也不再回调
static void RemoveUnregistersChannel(EventLoop &loop)
{
    const int fd = MakeEventfd();
    {
        Channel channel(fd, &loop);
        bool called = false;
        channel.SetReadCallback([&called] { called = true; });
        channel.SetEventCallback([&called] { called = true; });
        channel.EnableRead();
        channel.Remove();

        // 描述符仍可读，但已从 Epoller 摘除；必须补一个任务作为独立唤醒源，
        // 否则 LoopOnce() 会永久阻塞在 epoll_wait
        WriteEventfd(fd);
        bool woke = false;
        loop.QueueInLoop([&woke] { woke = true; });

        loop.LoopOnce();
        assert(woke);
        assert(!called); // 若 Remove 未生效，这里会被置为 true

        DrainEventfd(fd);
    }
    assert(close(fd) == 0);
}

// 转发链路探针 4：二次 Remove 走到 Epoller::RequireRegistered 的未登记分支
static void RemoveIsIdentityChecked(EventLoop &loop)
{
    const int fd = MakeEventfd();
    {
        Channel channel(fd, &loop);
        channel.EnableRead();
        channel.Remove();
        assert(ThrowsLogicError([&channel] { channel.Remove(); }));
    }
    assert(close(fd) == 0);
}

// 已知缺陷 3 的确定性探测：~Channel 不注销登记。
// 先分配 b 再释放 a，保证两者同时存活过、地址必然不同，于是 Epoller 的身份比较
// 确定走到 mismatch，不依赖栈/堆地址复用（这正是 tasks/lessons.md 记下的教训）。
// 该比较只读取映射中已失效的指针值，不解引用被销毁的对象。
// 修复缺陷 3 后应把这里的断言反转为"b->EnableRead() 不抛且登记成功"。
static void DestructorDoesNotDetach(EventLoop &loop)
{
    const int fd = MakeEventfd();
    std::unique_ptr<Channel> a(new Channel(fd, &loop));
    a->EnableRead();
    std::unique_ptr<Channel> b(new Channel(fd, &loop));
    a.reset(); // 期望注销，实际未发生

    // a 的登记仍在映射里，b 想登记同一 fd 时撞上身份不符
    assert(ThrowsLogicError([&b] { b->EnableRead(); }));

    assert(close(fd) == 0);
}

int main()
{
    alarm(5);

    EventLoop loop;

    ToggleStateMachine(loop);
    HandleDispatchesAllEventCombinations();
    ReadyAndInterestAreSeparate(loop);
    CallbackReplacementAndClearing();
    EmptyCallbacks();
    EventSnapshot();
    SameFdIdentityChecked(loop);
    UpdateRegistersChannel(loop);
    UpdateModifiesChannel(loop);
    RemoveUnregistersChannel(loop);
    RemoveIsIdentityChecked(loop);

    // 必须放在最后：它刻意留下一条悬垂登记，之后不得再复用该描述符号
    DestructorDoesNotDetach(loop);

    std::cout << "Channel tests passed: event toggles, 16 event combinations, "
                 "ready/interest separation, callback replacement/clearing, "
                 "empty callbacks, event snapshot, same-fd identity check, "
                 "loop forwarding (add/modify/remove), and the known "
                 "destructor-does-not-detach defect.\n";
}
