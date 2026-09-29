/**
 * @file Channel.hpp
 * @brief
 *
 * 设计目的:对描述符的事件管理
 *
 * 所有权:Channel 只借用描述符，从不拥有它。fd 的生命周期由构造时传入方负责
 * （EventLoop 的 eventfd、TimerWheel 的 timerfd、Socket 的套接字等），因此 _fd
 * 用裸 int 而不是 FdGuard 表示——FdGuard 会在析构时关闭描述符，那样一来每个
 * 构造点都会出现两个所有者，重复关闭并可能误关已被内核复用给其它连接的同号描述符。
 *
 * 接口设计:
 * 1.事件管理
 *   - 描述符是否可读
 *   - 描述符是否可写
 *   - 对描述符监控可读
 *   - 对描述符监控可写
 *   - 解除可读事件监控
 *   - 解除可写事件监控
 *   - 解除所有事件监控
 *
 * 2.事件触发后的处理管理
 *   -需要处理的事件: 可读、可写、挂断、错误、任意
 *   -事件处理的回调函数
 *
 * 声明与实现分离，实现见 src/Channel.cc
 */

#pragma once
#include <cstdint>
#include <functional>

class EventLoop; // 前向声明：Channel 只持有其指针并转发事件登记

class Channel
{
    using EventCallback = std::function<void()>;

  public:
    /*Channel的构造函数*/
    // 只借用 fd，不拥有：传入方须保证它在 Channel 存活期间有效，Channel 不关闭它。
    Channel(int fd, EventLoop *loop);
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    // 判断是否监控了可读
    bool Readable();
    // 判断是否监控了可写
    bool Writeable();
    // 启动读事件监控
    void EnableRead();
    // 启动写事件监控
    void EnableWrite();
    // 关闭读事件监控
    void DisableRead();
    // 关闭写事件监控
    void DisableWrite();
    // 关闭所有事件监控
    void DisableAll();
    // 更新事件的监控
    void Update();
    // 移除事件的监控
    void Remove();
    // 获取管理事件的文件描述符
    int Getfd() const;
    // 设置已就绪事件
    void SetREvents(uint32_t revents);
    // 获取事件
    uint32_t GetEvents();

    /*设置事件回调*/
    void SetReadCallback(EventCallback cb);
    void SetWriteCallback(EventCallback cb);
    void SetErrorCallback(EventCallback cb);
    void SetCloseCallback(EventCallback cb);
    void SetEventCallback(EventCallback cb);

    void Handle();

  private:
    int _fd;                       // 借用的描述符：所有权属于传入方，本类不关闭它
    uint32_t _events;              // 设置的事件
    uint32_t _rvents;              // 激活的事件
    EventLoop *_loop;              // 非拥有，_loop 必须比 Channel 活得更久
    EventCallback _read_callback;  // 可读事件触发的回调
    EventCallback _write_callback; // 可写事件触发的回调
    EventCallback _error_callback; // 错误事件触发的回调
    EventCallback _close_callback; // 关闭事件触发的回调
    EventCallback _event_callback; // 任意事件触发的回调
};
