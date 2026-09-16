/**
 * @file Epoller.hpp
 * @brief
 *
 * 设计目的: 对epool的封装
 *
 * 接口设计:
 *  - 添加描述符的事件监控
 *  - 修改描述符的事件监控
 *  - 移除描述符的事件监控
 *
 * 封装思想:
 *  - 管理epoll的操作句柄
 *  - struct epoll_event 结构数组，监控所有的活跃事件
 *  - hash表管理 "描述符" 与 "Channel"对象
 *
 * 逻辑流程:
 *  描述符就绪 -> 在hash表定位到Channel -> 通过Channel处理监控的事件
 */

#pragma once

#include <sys/epoll.h>

#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

class Channel;
class Epoller
{
    // 1024 是一次获取事件的数量上限，不是最多监听 1024 个连接
    static constexpr std::size_t MAX_EVENTS_PER_WAIT = 1024;

  private:
    // 检查非空参数及已登记对象的身份
    void RequireRegistered(Channel *channel) const;

    // 对epoll_ctl的直接操作
    void Control(Channel *channel, int op);

  public:
    Epoller();

    // 释放持有的 epoll 句柄
    ~Epoller();

    // 拥有 epoll 句柄这一独占资源，禁止拷贝：
    // 拷贝会使两个对象持有同一描述符，析构时重复 close 并释放他人的句柄
    Epoller(const Epoller &) = delete;
    Epoller &operator=(const Epoller &) = delete;

    // 添加监听；重复登记抛出异常，失败时不保留新映射
    void AddEvent(Channel *channel);
    // 修改已登记对象的监听；失败时保留映射
    void ModifyEvent(Channel *channel);
    // 移除事件的监控；描述符已被 close 时内核已摘除登记，此时一并清理映射
    void DeleteEvent(Channel *channel);
    // 幂等注销：未登记或对象身份不符时静默返回，内核注销失败也清理映射。
    // 不抛异常，供 Channel 析构调用。
    void Detach(Channel *channel) noexcept;
    // 事件存在则更新，不存在则创建
    void UpdateEvent(Channel *channel);
    // 获取已就绪描述符的Channel对象
    void WaitEvent(std::vector<Channel *> &active);

  private:
    int _epfd;
    std::array<struct epoll_event, MAX_EVENTS_PER_WAIT> _events{};
    // 非拥有指针：Epoller 删除映射时只需要 erase，不能 delete 这个指针。
    // 显式 DeleteEvent 与 Channel 析构触发的 Detach 共同保证不残留悬垂指针
    std::unordered_map<int, Channel *> _channels;
};
