/**
 * @file Channel.cc
 * @brief Channel 类的实现，接口声明见 include/Channel.hpp
 */

#include "Channel.hpp"
#include "EventLoop.hpp"
#include <sys/epoll.h>

Channel::Channel(int fd, EventLoop *loop)
    : _fd(fd)
    , _events(0)
    , _rvents(0)
    , _loop(loop)
{
}

// 析构时自动注销，避免 Epoller 的映射里残留悬垂指针
Channel::~Channel()
{
}

bool Channel::Readable()
{
    return _events & EPOLLIN;
}

bool Channel::Writeable()
{
    return _events & EPOLLOUT;
}

void Channel::EnableRead()
{
    _events |= EPOLLIN;
    Update();
}

void Channel::EnableWrite()
{
    _events |= EPOLLOUT;
    Update();
}

void Channel::DisableRead()
{
    _events &= ~EPOLLIN;
    Update();
}

void Channel::DisableWrite()
{
    _events &= ~EPOLLOUT;
    Update();
}

void Channel::DisableAll()
{
    _events = 0;
    Update();
}

void Channel::Update()
{
    _loop->UpdateEvent(this);
}

void Channel::Remove()
{
    _loop->RemoveEvent(this);
}

int Channel::Getfd() const
{
    return _fd;
}

void Channel::SetREvents(uint32_t revents)
{
    _rvents = revents;
}

uint32_t Channel::GetEvents()
{
    return _events;
}

void Channel::SetReadCallback(const EventCallback &cb)
{
    _read_callback = cb;
}

void Channel::SetWriteCallback(const EventCallback &cb)
{
    _write_callback = cb;
}

void Channel::SetErrorCallback(const EventCallback &cb)
{
    _error_callback = cb;
}

void Channel::SetCloseCallback(const EventCallback &cb)
{
    _close_callback = cb;
}

void Channel::SetEventCallback(const EventCallback &cb)
{
    _event_callback = cb;
}

void Channel::Handle()
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
