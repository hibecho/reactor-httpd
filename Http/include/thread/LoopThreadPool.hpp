/**
 * @file LoopThreadPool.hpp
 * @brief
 *
 * 功能设计:
 * 1.从属线程的数量可配置(0个或多个线程)
 * 2.对所有的线程进行管理，本质是管理0个或多个LoopThread对象
 * 3.提供线程分配功能
 *  -当主线程获取一个新连接，需要将新连接挂载到从属线程上进行事件监控及处理事件
 *  -假设有0个从属线程，则直接分配给主线程的Eventloop
 *  -假设有多个从属线程，则采用RR轮转（每个线程依次轮流分配）
 * 4.分配线程
 *   - 没有工作线程：返回 _baseloop。
 *   - 有工作线程：依次选择 _threads[_next]，随后循环更新索引。
 */

#pragma once

#include <memory>
#include <vector>

class LoopThread;
class EventLoop;
class LoopThreadPool
{
  public:
    /*LoopThreadPool构造函数*/
    explicit LoopThreadPool(EventLoop *baseloop);
    ~LoopThreadPool();
    LoopThreadPool(const LoopThreadPool &) = delete;
    LoopThreadPool &operator=(const LoopThreadPool &) = delete;

    // 启动前配置，拒绝负数
    void SetThreadCount(int count);
    // 创建线程，等待各 EventLoop 就绪
    void Create();
    // 启动后分配，返回目标循环。
    EventLoop *NextLoop();
    // 仅在拥有者保证工作线程尚未停止时使用返回的指针。
    std::vector<EventLoop *> Loops();
    void RequestStop();
    void Join();
    void Stop();

  private:
    /*管理线程分配*/
    bool _created = false;
    std::size_t _next = 0;                             // 轮转分配需要记录下一个位置
    int _thread_count = 0;                             // 从属线程的数量
    EventLoop *_baseloop;                              // 主EventLoop，运行在主线程
    std::vector<std::unique_ptr<LoopThread>> _threads; // 保存所有的LoopThread
};
