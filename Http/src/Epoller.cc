#include "Epoller.hpp"
#include "Channel.hpp"
#include "Logger.hpp"
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

Epoller::Epoller()
{
    // 参数被忽略
    _epfd = epoll_create(1);
    if (_epfd < 0)
    {
        LOG_ERROR("Failed to epoll_create");
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "epoll_create failed");
    }
}

Epoller::~Epoller()
{
    // 构造失败会抛异常，因此存活对象的句柄必然有效
    // 内核会自动摘除该 epoll 上登记的描述符，无需逐个注销
    if (close(_epfd) < 0)
        LOG_ERROR("Failed to close epoll fd");
}

// 修改和删除必须作用于已登记的同一个对象
void Epoller::RequireRegistered(Channel *channel) const
{
    if (channel == nullptr)
        throw std::invalid_argument("Epoller: null Channel");

    auto it = _channels.find(channel->Getfd());
    if (it == _channels.end() || it->second != channel)
        throw std::logic_error("Epoller: Channel is not registered");
}

// 仅封装内核操作，不修改 Channel 映射
void Epoller::Control(Channel *channel, int op)
{
    const int fd = channel->Getfd();
    epoll_event ev{};
    ev.events = channel->GetEvents();
    ev.data.fd = fd;
    if (epoll_ctl(_epfd, op, fd, &ev) < 0)
    {
        const int error = errno;
        LOG_ERROR("epoll_ctl failed, op={}", std::to_string(op), ", fd={}", std::to_string(fd));
        throw std::system_error(error, std::generic_category(),
                                "epoll_ctl failed, op=" + std::to_string(op) + ", fd=" + std::to_string(fd));
    }
}

void Epoller::AddEvent(Channel *channel)
{
    if (channel == nullptr)
        throw std::invalid_argument("Epoller::AddEvent: null Channel");

    // 先完成可能分配内存的登记，避免内核添加成功后映射分配失败
    auto result = _channels.emplace(channel->Getfd(), channel);
    if (!result.second)
        throw std::logic_error("Epoller::AddEvent: fd already registered");

    try
    {
        Control(channel, EPOLL_CTL_ADD);
    }
    catch (...)
    {
        _channels.erase(result.first);
        throw;
    }
}

void Epoller::ModifyEvent(Channel *channel)
{
    RequireRegistered(channel);
    Control(channel, EPOLL_CTL_MOD);
}

void Epoller::UpdateEvent(Channel *channel)
{
    if (channel == nullptr)
        return;
    auto it = _channels.find(channel->Getfd());
    if (it == _channels.end())
    {
        AddEvent(channel);
    }
    else
    {
        ModifyEvent(channel);
    }
}

void Epoller::RemoveEvent(Channel *channel)
{
    RequireRegistered(channel);

    try
    {
        Control(channel, EPOLL_CTL_DEL);
    }
    catch (const std::system_error &error)
    {
        // EBADF 说明描述符已被外部 close，内核在关闭时就会把它从本 epoll 摘除，登记其实已经不存在了。
        // 映射留着只会让这个 fd 号后续无法再被登记：
        // 内核会把该号码复用给新描述符，新 Channel 找到残留表项后因身份不符而抛异常。
        // 其它错误（如 fd 仍有效但不可轮询）保留映射，交由调用方处理。
        if (error.code().value() != EBADF)
            throw;
    }

    _channels.erase(channel->Getfd());
}

void Epoller::WaitEvent(std::vector<Channel *> &active)
{
    // 本次调用覆盖上次结果；clear 不会缩减 capacity
    active.clear();

    // int epoll_wait(int epfd, struct epoll_event *events,int maxevents, int timeout);
    int nfds = epoll_wait(_epfd, _events.data(), static_cast<int>(_events.size()), -1);

    // 异常处理
    if (nfds < 0)
    {
        const int error = errno;

        // 被信号打断，交回外层事件循环决定是否继续等待
        if (error == EINTR)
            return;
        LOG_ERROR("epoll_wait failed");
        throw std::system_error(error, std::generic_category(), "epoll_wait failed");
    }

    for (int i = 0; i < nfds; i++)
    {
        int rfd = _events[i].data.fd;
        uint32_t revents = _events[i].events;
        // 异常处理
        auto it = _channels.find(rfd);
        if (it == _channels.end())
        {
            LOG_ERROR("Epoller::WaitEvent: Channel not found, fd={}", std::to_string(rfd));
            throw std::logic_error("Epoller::WaitEvent: Channel not found, fd=" + std::to_string(rfd));
        }
        Channel *channel = it->second;
        // 将就绪事件设定到chennel中，由channel对象管理已就绪事件
        channel->SetREvents(revents);
        // 将已就绪事件的channel放入active中
        active.push_back(channel);
    }
}