#include "reactor/EventLoop.hpp"
#include <cassert>
#include <csignal>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>

// 仅测试线程访问。让退出请求发生在 Loop 已检查停止标志之后。
static std::promise<void> *wait_entered = nullptr;
extern "C" int __real_epoll_wait(int, epoll_event *, int, int);
extern "C" int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout)
{
    if (wait_entered)
    {
        auto *entered = wait_entered;
        wait_entered = nullptr;
        entered->set_value();
        // timerfd 首次在一秒后就绪，不能让它掩盖 eventfd 唤醒缺失。
        int result = __real_epoll_wait(fd, events, count, 200);
        if (result == 0)
            throw std::runtime_error("Quit did not wake epoll_wait");
        return result;
    }
    return __real_epoll_wait(fd, events, count, timeout);
}

static void OnAlarm(int)
{
    constexpr char message[] = "EventLoop Quit test timed out\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(2);
}

static void QuitBeforeLoop()
{
    EventLoop loop;
    bool called = false;
    loop.QueueInLoop([&]() {
        called = true;
    });
    loop.Quit();
    loop.Quit();
    loop.Loop();
    assert(!called);
    loop.Loop(); // 停止标志不可在 Loop 开头重置。
    assert(!called);
}

static void FinishCurrentBatch()
{
    EventLoop loop;
    int completed = 0;
    bool later = false;
    loop.QueueInLoop([&]() {
        ++completed;
        loop.Quit();
        loop.QueueInLoop([&]() {
            later = true;
        });
    });
    loop.QueueInLoop([&]() {
        ++completed;
    });
    loop.Loop();
    assert(completed == 2);
    assert(!later);
}

static void CrossThreadQuit()
{
    EventLoop loop;
    std::promise<void> entered;
    auto ready = entered.get_future();
    wait_entered = &entered;
    std::exception_ptr worker_error;
    std::thread worker([&]() {
        ready.wait();
        try
        {
            loop.Quit();
        }
        catch (...)
        {
            worker_error = std::current_exception();
        }
    });
    std::exception_ptr loop_error;
    try
    {
        loop.Loop();
    }
    catch (...)
    {
        loop_error = std::current_exception();
    }
    worker.join(); // 对象销毁之前，确保 Quit 已返回。
    if (worker_error)
        std::rethrow_exception(worker_error);
    if (loop_error)
        std::rethrow_exception(loop_error);
}

int main()
{
    std::signal(SIGALRM, OnAlarm);
    alarm(10);
    try
    {
        QuitBeforeLoop();
        FinishCurrentBatch();
        CrossThreadQuit();
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    alarm(0);
    std::cout << "EventLoop Quit tests passed\n";
}
