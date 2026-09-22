#include "thread/LoopThread.hpp"
#include "reactor/EventLoop.hpp"

LoopThread::LoopThread()
    : _loop(nullptr)
    , _thread([this]() {
        ThreadEntry();
    })
{
}

LoopThread::~LoopThread()
{
}

/**
 * @brief
 *  GetLoop调用线程:获取EventLoop
 *  ThreadEntry:给EventLoop赋值
 * 时序图：
 *
 * 调用线程                          工作线程
 *  获得锁
 *  检查：_loop 尚未就绪              尝试获得锁，暂时被阻塞
 *  wait 原子地释放锁并进入等待
 *                                   获得锁
 *                                   更新 _loop
 *                                   释放锁
 *                                   发送通知
 *   被唤醒，重新获得锁
 *   再次检查：_loop 已就绪
 */

// 调用线程:获取EventLoop
EventLoop *LoopThread::GetLoop()
{
    EventLoop *loop = nullptr;
    {
        // 安全地读取 _loop，避免和工作线程的写入产生数据竞争。
        std::unique_lock<std::mutex> lock(_mutex);
        // 保证了读取 _loop时非空
        _cv.wait(lock, [this]() {
            return _loop != nullptr;
        });

        loop = _loop;
    }
    return loop;
}

// 工作线程:给EventLoop赋值
void LoopThread::ThreadEntry()
{
    // 必须在工作线程创建，使 EventLoop 记录正确的线程 ID。
    EventLoop loop;

    // 进行互斥的对loop进行赋值
    {
        std::lock_guard<std::mutex> locl(_mutex);
        _loop = &loop;
    }

    // 状态更新完成后，通知等待者
    _cv.notify_all();

    // 不持有 _mutex 运行事件循环。
    loop.Loop();
}