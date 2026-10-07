#include "base/Buffer.hpp"
#include "reactor/EventLoop.hpp"
#include "tcp/Connection.hpp"
#include "tcp/ResourceLimits.hpp"
#include "tcp/Socket.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// 链接参数：--wrap=write 与 --wrap=_ZN6Buffer5WriteEPKvm。
static std::atomic<bool> fail_wakeup{false};
static std::atomic<int> fail_buffer_write{0};
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (fail_wakeup.load() && size == sizeof(uint64_t))
    {
        char path[64];
        char target[128]{};
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        const auto length = readlink(path, target, sizeof(target) - 1);
        if (length > 0 && std::strcmp(target, "anon_inode:[eventfd]") == 0 && fail_wakeup.exchange(false))
        {
            errno = EIO;
            return -1;
        }
    }
    return __real_write(fd, data, size);
}
extern "C" void __real__ZN6Buffer5WriteEPKvm(Buffer *, const void *, std::size_t);
extern "C" void __wrap__ZN6Buffer5WriteEPKvm(Buffer *buffer, const void *data, std::size_t size)
{
    const int failure = fail_buffer_write.exchange(0);
    if (failure == 1)
        throw std::length_error("injected output append failure");
    if (failure == 2)
        throw std::bad_alloc();
    __real__ZN6Buffer5WriteEPKvm(buffer, data, size);
}

// 全局分配注入只用于普通构建；sanitizer 保留自身分配器，仍运行 Buffer bad_alloc 用例。
#if !defined(__SANITIZE_ADDRESS__)
static thread_local int allocation_countdown = -1;
void *operator new(std::size_t size)
{
    if (allocation_countdown >= 0 && allocation_countdown-- == 0)
        throw std::bad_alloc();
    if (void *value = std::malloc(size == 0 ? 1 : size))
        return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void *value) noexcept
{
    std::free(value);
}
void operator delete[](void *value) noexcept
{
    std::free(value);
}
void operator delete(void *value, std::size_t) noexcept
{
    std::free(value);
}
void operator delete[](void *value, std::size_t) noexcept
{
    std::free(value);
}
#endif

namespace
{
    void Timeout(int)
    {
        constexpr char message[] = "TestSendLimits timeout\n";
        (void)__real_write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(2);
    }
    void Step(EventLoop &loop)
    {
        loop.QueueInLoop([] {
        });
        loop.LoopOnce();
    }
    std::shared_ptr<OutputBudget> Budget(std::size_t maximum = 512 * 1024, std::size_t total = 1024 * 1024)
    {
        auto budget = std::make_shared<OutputBudget>();
        budget->limits.output_low = 1024;
        budget->limits.output_high = 2048;
        budget->limits.max_output = maximum;
        budget->limits.max_total_output = total;
        budget->limits.Validate();
        return budget;
    }
    std::size_t Used(const std::shared_ptr<OutputBudget> &budget)
    {
        std::lock_guard<std::mutex> lock(budget->mutex);
        return budget->used;
    }
    struct Peer
    {
        int fd = -1;
        ~Peer()
        {
            if (fd >= 0)
                close(fd);
        }
    };
    ConnectionPtr Connect(EventLoop &loop, const std::shared_ptr<OutputBudget> &budget, Peer &peer, uint64_t id = 1)
    {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
        peer.fd = fds[1];
        Socket owned(fds[0]);
        auto connection = std::make_shared<Connection>(&loop, id, std::move(owned), budget);
        int send_size = 4096;
        assert(setsockopt(connection->GetFd(), SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size)) == 0);
        connection->Established();
        return connection;
    }
    bool Read(int fd, std::string &result)
    {
        char buffer[8192];
        for (;;)
        {
            const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0)
                result.append(buffer, static_cast<std::size_t>(count));
            else if (count == 0)
                return true;
            else if (errno != EINTR)
            {
                assert(errno == EAGAIN || errno == EWOULDBLOCK);
                return false;
            }
        }
    }
    void Close(EventLoop &loop, const ConnectionPtr &connection)
    {
        connection->ForceClose();
        for (int i = 0; i < 4; ++i)
            Step(loop);
        assert(connection->GetFd() == -1);
        assert(connection->PendingOutput() == 0);
    }
    void StopBudget(const std::shared_ptr<OutputBudget> &budget)
    {
        std::lock_guard<std::mutex> lock(budget->mutex);
        budget->stopping.store(true);
    }

    void InputLimitBoundary()
    {
        EventLoop loop;
        auto budget = Budget();
        budget->limits.max_input = 4;
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        int messages = 0;
        connection->SetMessageCallback([&](const ConnectionPtr &, Buffer *input) {
            ++messages;
            assert(input->GetReadableSize() <= 4);
        });
        assert(send(peer.fd, "abcd", 4, MSG_NOSIGNAL) == 4);
        Step(loop);
        assert(messages == 1 && connection->IsConnected());
        assert(send(peer.fd, "e", 1, MSG_NOSIGNAL) == 1);
        Step(loop);
        Step(loop);
        assert(connection->GetFd() == -1 && messages == 1);
        assert(Used(budget) == 0);
    }

    void CleanupDrainsCurrentBatchAndDescendants()
    {
        EventLoop loop;
        std::string order;
        loop.QueueInLoop([&] {
            order += 'a';
            loop.DrainPendingTasks();
            // 清理屏障不能越过已经取出的同批次后续任务及其派生任务。
            assert(order == "abcd");
            loop.Quit();
        });
        loop.QueueInLoop([&] {
            order += 'b';
            loop.QueueInLoop([&] {
                order += 'c';
                loop.QueueInLoop([&] {
                    order += 'd';
                });
            });
        });
        loop.Loop();
        assert(order == "abcd");
    }

    void SingleLimitIncludesQueuedData()
    {
        EventLoop loop;
        auto budget = Budget(4096, 8192);
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        bool accepted = false;
        bool overflow = true;
        std::thread sender([&] {
            accepted = connection->Send(std::string(4096, 'a'));
            overflow = connection->Send("b");
        });
        sender.join();
        assert(accepted && !overflow);
        assert(Used(budget) == 4096);
        assert(connection->PendingOutput() == 4096);
        for (int i = 0; i < 4; ++i)
            Step(loop);
        assert(connection->GetFd() == -1);
        assert(Used(budget) == 0);
        assert(connection->PendingOutput() == 0);
    }

    void GlobalLimitCompetingConnections()
    {
        EventLoop loop;
        auto budget = Budget(4096, 4096);
        Peer peers[2];
        ConnectionPtr connections[2] = {Connect(loop, budget, peers[0], 1), Connect(loop, budget, peers[1], 2)};
        std::mutex mutex;
        std::condition_variable condition;
        int ready = 0;
        bool go = false;
        bool accepted[2] = {};
        auto send = [&](int index) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++ready;
                condition.notify_all();
                condition.wait(lock, [&] {
                    return go;
                });
            }
            accepted[index] = connections[index]->Send(std::string(4096, 'a' + index));
        };
        std::thread first(send, 0);
        std::thread second(send, 1);
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] {
                return ready == 2;
            });
            go = true;
            condition.notify_all();
        }
        first.join();
        second.join();
        assert(accepted[0] != accepted[1]);
        assert(Used(budget) == 4096);
        assert(connections[0]->PendingOutput() + connections[1]->PendingOutput() == 4096);
        Close(loop, connections[0]);
        Close(loop, connections[1]);
        assert(Used(budget) == 0);
    }

    void AcceptedBeforeStopDrains()
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        int closed = 0;
        connection->SetClosedCallback([&](const ConnectionPtr &) {
            ++closed;
        });
        const std::string payload(64 * 1024, 'q');
        bool accepted = false;
        std::thread sender([&] {
            accepted = connection->Send(payload);
        });
        sender.join();
        assert(accepted && connection->PendingOutput() == payload.size());
        StopBudget(budget);
        assert(!connection->Send("after stop"));
        connection->ShutDown();
        std::string received;
        bool eof = false;
        for (int i = 0; i < 1024 && !eof; ++i)
        {
            Step(loop);
            eof = Read(peer.fd, received);
        }
        assert(eof && received == payload && closed == 1);
        assert(Used(budget) == 0 && connection->PendingOutput() == 0);
        connection->ShutDown();
        Step(loop);
        assert(closed == 1);
    }

    void StopBeforeSubmissionRejects()
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        StopBudget(budget);
        bool accepted = true;
        std::thread sender([&] {
            accepted = connection->Send("late");
        });
        sender.join();
        assert(!accepted && Used(budget) == 0);
        Close(loop, connection);
        std::string received;
        assert(Read(peer.fd, received) && received.empty());
    }

    void PartialWriteThenCloseReturnsRemainder()
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        const std::string payload(256 * 1024, 'p');
        assert(connection->Send(payload));
        for (int i = 0; i < 8 && connection->PendingOutput() == payload.size(); ++i)
            Step(loop);
        const auto remaining = connection->PendingOutput();
        assert(remaining > 0 && remaining < payload.size());
        assert(Used(budget) == remaining);
        Close(loop, connection);
        assert(Used(budget) == 0);
        std::string received;
        assert(Read(peer.fd, received));
        assert(received == payload.substr(0, payload.size() - remaining));
    }

    void CommittedWakeFailureKeepsOwnership()
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        bool accepted = false;
        std::thread sender([&] {
            fail_wakeup.store(true);
            accepted = connection->Send("committed once");
        });
        sender.join();
        assert(!fail_wakeup.load() && accepted);
        assert(Used(budget) == std::strlen("committed once"));
        // 不用 Step 添加额外唤醒；循环自己的定时器必须能推进已提交任务。
        loop.LoopOnce();
        std::string received;
        for (int i = 0; i < 8 && received != "committed once"; ++i)
        {
            Step(loop);
            Read(peer.fd, received);
        }
        assert(received == "committed once");
        assert(Used(budget) == 0);
        Close(loop, connection);
    }

    void BufferFailureReturnsReservation(int failure)
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        bool accepted = false;
        std::thread sender([&] {
            accepted = connection->Send("append fails");
        });
        sender.join();
        assert(accepted && Used(budget) != 0);
        fail_buffer_write.store(failure);
        try
        {
            Step(loop);
        }
        catch (const std::bad_alloc &)
        {
            assert(failure == 2);
        }
        catch (const std::length_error &)
        {
            assert(failure == 1);
        }
        assert(fail_buffer_write.load() == 0);
        assert(Used(budget) == 0 && connection->PendingOutput() == 0);
        Close(loop, connection);
        std::string received;
        assert(Read(peer.fd, received) && received.empty());
    }

    void SynchronousAppendFailureCloses()
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        fail_buffer_write.store(1);
        bool failed = false;
        try
        {
            connection->Send("synchronous failure");
        }
        catch (const std::length_error &)
        {
            failed = true;
        }
        assert(failed && fail_buffer_write.load() == 0);
        assert(Used(budget) == 0);
        assert(!connection->Send("must not retry"));
        Close(loop, connection);
    }

    void DiscardedTaskReturnsReservation()
    {
        auto budget = Budget();
        auto account = std::make_shared<SendAccount>(budget);
        {
            EventLoop loop;
            auto reservation = std::make_shared<SendReservation>(account);
            {
                std::lock_guard<std::mutex> lock(budget->mutex);
                budget->used = account->pending = reservation->bytes = 123;
            }
            loop.QueueInLoop([reservation] {
            });
            reservation.reset();
            assert(Used(budget) == 123);
            // 不执行任务，让 EventLoop 析构丢弃尚未执行的任务。
        }
        assert(Used(budget) == 0 && account->pending == 0);
    }

#if !defined(__SANITIZE_ADDRESS__)
    void SubmissionAllocationFailureDoesNotLeak()
    {
        // 第一次分配建立额度凭证；第二次分配构造跨线程任务，此时额度已预留。
        for (int failure = 0; failure < 2; ++failure)
        {
            EventLoop loop;
            auto budget = Budget();
            Peer peer;
            auto connection = Connect(loop, budget, peer);
            bool rejected = false;
            bool injected = false;
            std::thread sender([&] {
                std::string payload(4096, 'f');
                allocation_countdown = failure;
                try
                {
                    rejected = !connection->Send(std::move(payload));
                }
                catch (const std::bad_alloc &)
                {
                    rejected = true;
                }
                injected = allocation_countdown == -1;
                allocation_countdown = -1;
            });
            sender.join();
            assert(injected && rejected);
            assert(Used(budget) == 0 && connection->PendingOutput() == 0);
            Close(loop, connection);
        }
    }
#endif

    void HighWaterAllowsWholeResponseAndResumesCachedInput(bool shutdown)
    {
        EventLoop loop;
        auto budget = Budget();
        Peer peer;
        auto connection = Connect(loop, budget, peer);
        const std::string payload(64 * 1024, 'h');
        int messages = 0;
        connection->SetMessageCallback([&](const ConnectionPtr &current, Buffer *input) {
            while (input->GetReadableSize() != 0 && current->CanProcessInput())
            {
                input->ReadAsString(1);
                ++messages;
                assert(current->Send(messages == 1 ? payload : "tail"));
            }
        });
        assert(send(peer.fd, "ab", 2, MSG_NOSIGNAL) == 2);
        Step(loop);
        assert(messages == 1);
        assert(!connection->CanProcessInput());
        assert(connection->PendingOutput() > budget->limits.output_high);
        if (shutdown)
            connection->ShutDown();
        const std::string expected = shutdown ? payload : payload + "tail";
        std::string received;
        for (int i = 0; i < 1024 && received.size() < expected.size(); ++i)
        {
            Read(peer.fd, received);
            Step(loop);
        }
        Read(peer.fd, received);
        assert(received == expected);
        assert(messages == (shutdown ? 1 : 2));
        assert(Used(budget) == 0);
        Close(loop, connection);
    }
} // namespace

int main()
{
    struct sigaction action{};
    action.sa_handler = Timeout;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM, &action, nullptr) == 0);
    alarm(30);
    InputLimitBoundary();
    CleanupDrainsCurrentBatchAndDescendants();
    SingleLimitIncludesQueuedData();
    GlobalLimitCompetingConnections();
    AcceptedBeforeStopDrains();
    StopBeforeSubmissionRejects();
    PartialWriteThenCloseReturnsRemainder();
    CommittedWakeFailureKeepsOwnership();
    BufferFailureReturnsReservation(1);
    BufferFailureReturnsReservation(2);
    SynchronousAppendFailureCloses();
    DiscardedTaskReturnsReservation();
#if !defined(__SANITIZE_ADDRESS__)
    SubmissionAllocationFailureDoesNotLeak();
#endif
    HighWaterAllowsWholeResponseAndResumesCachedInput(false);
    HighWaterAllowsWholeResponseAndResumesCachedInput(true);
    alarm(0);
    std::cout << "Send limits tests passed\n";
}
