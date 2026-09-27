#include <atomic>
#include <cassert>
#include <cerrno>
#include <future>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include "thread/LoopThread.hpp"
#include "thread/LoopThreadPool.hpp"
#include "reactor/EventLoop.hpp"

static std::atomic<int> fail_after{-1};
extern "C" int __real_eventfd(unsigned int, int);
extern "C" int __wrap_eventfd(unsigned int init, int flags)
{
    int remaining = fail_after.load();
    if (remaining >= 0 && fail_after.fetch_sub(1) == 0)
    {
        errno = EMFILE;
        return -1;
    }
    return __real_eventfd(init, flags);
}

int main()
{
    // 普通析构会停止并回收尚在等待事件的线程。
    for (int i = 0; i < 20; ++i)
    {
        LoopThread thread;
        assert(thread.GetLoop());
    }
    {
        LoopThread thread;
        thread.RequestStop(); // 允许 EventLoop 尚未构造完成。
        thread.Join();
        thread.Stop();
        assert(thread.GetLoop() == nullptr);
    }
    {
        LoopThread thread;
        std::promise<bool> rejected;
        auto result = rejected.get_future();
        thread.GetLoop()->QueueInLoop([&] {
            try { thread.Join(); rejected.set_value(false); }
            catch (const std::logic_error &) { rejected.set_value(true); }
        });
        assert(result.get());
        thread.Stop();
    }
    {
        LoopThread thread;
        thread.GetLoop()->QueueInLoop([] { throw std::runtime_error("worker failure"); });
        bool propagated = false;
        try { thread.Join(); }
        catch (const std::runtime_error &) { propagated = true; }
        assert(propagated);
    }
    {
        fail_after = 0;
        LoopThread thread;
        bool propagated = false;
        try { thread.GetLoop(); }
        catch (const std::system_error &) { propagated = true; }
        assert(propagated);
        fail_after = -1;
    }
    EventLoop base;
    {
        LoopThreadPool pool(&base);
        pool.Create();
        assert(pool.NextLoop() == &base);
        assert(pool.Loops().empty());
        pool.Stop();
    }
    {
        LoopThreadPool pool(&base);
        pool.SetThreadCount(2);
        fail_after = 1;
        bool propagated = false;
        try { pool.Create(); }
        catch (const std::system_error &) { propagated = true; }
        assert(propagated);
        fail_after = -1;
        pool.Create(); // 失败后允许重新创建，先前 worker 已回收。
        auto first = pool.NextLoop();
        auto second = pool.NextLoop();
        assert(first && second && first != second);
        assert(pool.NextLoop() == first);
        assert(pool.Loops().size() == 2);
        bool rejected = false;
        try { pool.SetThreadCount(1); }
        catch (const std::logic_error &) { rejected = true; }
        assert(rejected);
        pool.Stop();
        assert(pool.Loops().empty());
        pool.Stop();
    }
}
