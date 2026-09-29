/**
 * @file FdGuard.hpp
 * @brief
 *
 * 设计目的:独占持有文件描述符，析构即关闭；可移动、不可复制。
 *
 * 解决的问题:构造函数中途抛异常时，只有已完整构造的成员会析构，类自己的析构函数
 * 不会执行。因此「先创建裸 fd，再构造 Channel、Epoller、容器等其它成员」的类，
 * 一旦后续成员构造失败，裸 fd 就会泄漏——它没有析构函数，没人替它 close。
 * 把 fd 交给本类型持有后，这个清理时机由成员自身的析构保证，与构造函数是否跑完无关。
 *
 * 未持有时值为 -1（默认构造、Close() 之后、Release() 之后），此时析构不做任何事。
 *
 * 本文件不按仓库「声明与实现分离」的惯例拆分：成员函数都是单行 noexcept 操作，
 * GetFd() 又位于 write/read 热路径上，拆到 .cc 只会把它变成跨编译单元调用。
 * 也因此本类型不依赖 base/Logger，保持零耦合。
 */

#pragma once
#include <cerrno>
#include <unistd.h>

class FdGuard
{
  public:
    FdGuard() noexcept = default;
    // 接管一个已打开的描述符；不检查有效性，传入 -1 等价于默认构造
    explicit FdGuard(int fd) noexcept
        : _fd(fd)
    {
    }

    // 析构函数
    ~FdGuard()
    {
        Close();
    }

    // 拷贝构造 和 赋值重载默认删除
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;

    // 移动构造
    FdGuard(FdGuard &&other) noexcept
        : _fd(other.Release())
    {
    }
    // 移动赋值
    FdGuard &operator=(FdGuard &&other) noexcept
    {
        if (this != &other)
        {
            Close();
            _fd = other.Release();
        }
        return *this;
    }

    // 返回持有的描述符，未持有时为 -1
    int GetFd() const noexcept
    {
        return _fd;
    }

    bool Valid() const noexcept
    {
        return _fd >= 0;
    }

    // 放弃所有权，返回裸描述符交由调用方关闭；本对象转为未持有
    int Release() noexcept
    {
        const int fd = _fd;
        _fd = -1;
        return fd;
    }

    // 关闭后可重复调用；保留调用前 errno，不对 close 的 EINTR 重试。
    // 与 Socket::Close 同契约，全仓库只有这一种关闭语义。
    void Close() noexcept
    {
        if (_fd < 0)
            return;

        const int saved_errno = errno;
        const int closing = _fd;
        // 先置为未持有再关闭：close 失败也不会留下会被二次关闭的陈旧值
        _fd = -1;
        close(closing);
        errno = saved_errno;
    }

  private:
    int _fd = -1;
};
