#include "tcp/Connection.hpp"
#include "reactor/TimerQueue.hpp"
#include "reactor/EventLoop.hpp"
#include <atomic>
#include <memory>
#include <unordered_map>
// 只在测试中访问主循环和 ID 边界，启停通过公共接口。
#define private public
#include "tcp/TcpServer.hpp"
#undef private
#include <cassert>
#include <csignal>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

static void OnAlarm(int)
{
    constexpr char message[] = "TcpServer timer test timed out\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(2);
}

int main()
{
    std::signal(SIGALRM, OnAlarm);
    alarm(10);
    TcpServer server(0);
    for (int delay : {-1, 0, 61})
    {
        bool rejected = false;
        try { server.RunAfter([] {}, delay); }
        catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
    bool rejected = false;
    try { server.RunAfter({}, 1); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);

    int canceled_calls = 0;
    int live_calls = 0;
    // 工作线程添加，主循环线程在登记尚未执行时取消。
    uint64_t canceled_id = 0;
    std::thread producer([&] {
        canceled_id = server.RunAfter([&] { ++canceled_calls; }, 1);
    });
    producer.join();
    server.CancelTask(canceled_id);
    server.CancelTask(canceled_id);
    server.CancelTask(0); // 不存在的 ID。

    constexpr int workers = 4;
    constexpr int per_worker = 100;
    std::vector<uint64_t> ids[workers];
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i)
        threads.emplace_back([&, i] {
            for (int j = 0; j < per_worker; ++j)
            {
                auto id = server.RunAfter([&] { ++canceled_calls; }, 1);
                ids[i].push_back(id);
                server.CancelTask(id);
            }
        });
    for (auto &thread : threads) thread.join();
    std::set<uint64_t> unique{canceled_id};
    for (const auto &group : ids)
        for (auto id : group) assert(unique.insert(id).second);
    // 模拟新连接从同一分配器取号，连接超时任务直接使用连接 ID。
    const auto conn_id = server.AllocateId();
    assert(unique.insert(conn_id).second);
    int connection_timeouts = 0;
    server._baseloop->TimerAdd(conn_id, 1, [&] { ++connection_timeouts; });
    const auto live_id = server.RunAfter([&] { ++live_calls; }, 1);
    assert(unique.insert(live_id).second);
    server.RunAfter([&] {
        server.CancelTask(live_id); // 已执行任务的取消无副作用。
        server.Stop();
    }, 2);

    server._next_id.store(std::numeric_limits<uint64_t>::max() - 1);
    assert(server.AllocateId() == std::numeric_limits<uint64_t>::max());
    for (int i = 0; i < 2; ++i)
    {
        bool exhausted = false;
        try { server.RunAfter([] {}, 1); }
        catch (const std::overflow_error &) { exhausted = true; }
        assert(exhausted);
    }
    server.Start();
    assert(canceled_calls == 0);
    assert(live_calls == 1);
    assert(connection_timeouts == 1);
    alarm(0);
    std::cout << "TcpServer timer tests passed\n";
}
