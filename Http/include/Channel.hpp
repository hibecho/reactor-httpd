/**
 * @file Channel.hpp
 * @brief
 *
 * 设计目的:对描述符的事件管理
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
 */

#pragma once
#include <sys/epoll.h>

#include <functional>
#include <memory>

#include "Epoller.hpp"
#include "Logger.hpp"

class Epoller; // 前向声明

class Channel
{
    using EventCallback = std::function<void()>;

  public:
    Channel(int fd, Epoller &epoller)
        : _fd(fd)
        , _events(0)
        , _rvents(0)
        , _ep(epoller)
    {
    }

    // 析构时自动注销，避免 Epoller 的映射里残留悬垂指针
    ~Channel()
    {
        _ep.Detach(this);
    }

    // 持有登记这一独占关系，禁止拷贝：
    // 否则拷贝对象析构时会注销掉原对象在 Epoller 中的登记
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    // 判断是否监控了可读
    bool Readable()
    {
        return _events & EPOLLIN;
    }
    // 判断是否监控了可写
    bool Writeable()
    {
        return _events & EPOLLOUT;
    }
    // 启动读事件监控
    void EnableRead()
    {
        _events |= EPOLLIN;
        Update();
    }
    // 启动写事件监控
    void EnableWrite()
    {
        _events |= EPOLLOUT;
        Update();
    }
    // 关闭读事件监控
    void DisableRead()
    {
        _events &= ~EPOLLIN;
        Update();
    }
    // 关闭写事件监控
    void DisableWrite()
    {
        _events &= ~EPOLLOUT;
        Update();
    }
    // 关闭所有事件监控
    void DisableAll()
    {
        _events = 0;
        Update();
    }

    // 更新事件的监控
    void Update()
    {
        _ep.UpdateEvent(this);
    }
    // 移除事件的监控
    void Remove()
    {
        _ep.DeleteEvent(this);
    }
    // 获取管理事件的文件描述符
    int Getfd() const
    {
        return _fd;
    }
    // 设置已就绪事件
    void SetREvents(uint32_t revents)
    {
        _rvents = revents;
    }
    // 获取事件
    uint32_t GetEvents()
    {
        return _events;
    }

    void SetReadCallback(const EventCallback &cb)
    {
        _read_callback = cb;
    }
    void SetWriteCallback(const EventCallback &cb)
    {
        _write_callback = cb;
    }
    void SetErrorCallback(const EventCallback &cb)
    {
        _error_callback = cb;
    }
    void SetCloseCallback(const EventCallback &cb)
    {
        _close_callback = cb;
    }
    void SetEventCallback(const EventCallback &cb)
    {
        _event_callback = cb;
    }

    void Handle()
    {
        const auto ready = _rvents;
        if ((ready & EPOLLIN) && _read_callback)
            _read_callback();

        if ((ready & EPOLLOUT) && _write_callback)
            _write_callback();

        if ((ready & EPOLLERR) && _error_callback)
            _error_callback();

        if ((ready & EPOLLHUP) && _close_callback)
            _close_callback();

        if (ready != 0 && _event_callback)
            _event_callback();
    }

  private:
    int _fd;
    uint32_t _events;
    uint32_t _rvents;
    Epoller &_ep;                  // 非拥有引用，Epoller 必须比 Channel 活得更久
    EventCallback _read_callback;  // 可读事件触发的回调
    EventCallback _write_callback; // 可写事件触发的回调
    EventCallback _error_callback; // 错误事件触发的回调
    EventCallback _close_callback; // 关闭事件触发的回调
    EventCallback _event_callback; // 任意事件触发的回调
};