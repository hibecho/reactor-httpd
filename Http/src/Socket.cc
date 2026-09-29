/**
 * @file Socket.cc
 * @brief Socket 类的实现，接口声明见 include/Socket.hpp
 */

#include "tcp/Socket.hpp"
#include "base/FdGuard.hpp"
#include "base/Logger.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>

// 描述符由 FdGuard 独占持有，默认构造、移动与析构都直接复用它的语义：
// 未持有时为 -1，移动后源对象自动置为未持有，析构即关闭。
Socket::Socket() = default;

Socket::Socket(int fd)
    : _sockfd(fd)
{
}

Socket::Socket(Socket &&other) noexcept = default;

Socket &Socket::operator=(Socket &&other) noexcept = default;

Socket::~Socket() = default;

// 创建套接字
bool Socket::Create()
{
    if (_sockfd.Valid())
    {
        errno = EALREADY;
        return false;
    }
    // 获取监听套接字
    //  int socket(int domain, int type, int protocol);
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        const int saved_errno = errno;
        LOG_ERROR("Failed to create socket:errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    _sockfd = FdGuard(fd);
    return true;
}

// 绑定地址信息
bool Socket::Bind(const std::string &ip_address, uint16_t port)
{
    // int bind(int sockfd, const struct sockaddr *socket_address,socklen_t
    // addrlen);

    struct sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(port);
    const int result = inet_pton(AF_INET, ip_address.c_str(), &socket_address.sin_addr);
    if (result != 1)
    {
        const int saved_errno = result == 0 ? EINVAL : errno;
        LOG_ERROR("Invalid IPv4 address: errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    socklen_t address_length = sizeof(socket_address);
    if (bind(_sockfd.GetFd(), reinterpret_cast<sockaddr *>(&socket_address), address_length) == -1)
    {
        // 绑定套接字失败
        const int saved_errno = errno;
        LOG_ERROR("Failed to bind socket:errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    return true;
}

// 开始监听
bool Socket::Listen(int backlog)
{
    // int listen(int sockfd, int backlog);
    int ret = listen(_sockfd.GetFd(), backlog);
    if (ret == -1)
    {
        // 监听套接字失败
        const int saved_errno = errno;
        LOG_ERROR("Failed to listen on socket:errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    return true;
}

// 客户端向服务器发起连接
bool Socket::Connect(const std::string &ip_address, uint16_t port)
{
    struct sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(port);
    const int result = inet_pton(AF_INET, ip_address.c_str(), &socket_address.sin_addr);
    if (result != 1)
    {
        const int saved_errno = result == 0 ? EINVAL : errno;
        LOG_ERROR("Invalid IPv4 address: errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    socklen_t address_length = sizeof(socket_address);
    if (connect(_sockfd.GetFd(), reinterpret_cast<sockaddr *>(&socket_address), address_length) == -1)
    {
        // 连接套接字失败
        const int saved_errno = errno;
        LOG_ERROR("Failed to connect socket:errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    return true;
}

// 兼容旧接口名称。
bool Socket::Connet(const std::string &ip_address, uint16_t port)
{
    return Connect(ip_address, port);
}

int Socket::Accept(sockaddr *peer_address, socklen_t *peer_address_length)
{
    while (true)
    {
        int client_fd = accept(_sockfd.GetFd(), peer_address, peer_address_length);
        if (client_fd >= 0)
        {
            return client_fd;
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR)
        {
            continue;
        }
        if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK)
        {
            LOG_ERROR("Failed to accept connection: errno={}", saved_errno);
        }
        errno = saved_errno;
        return -1;
    }
}

ssize_t Socket::Recv(void *buffer, size_t length, int flags)
{
    while (true)
    {
        const ssize_t received = recv(_sockfd.GetFd(), buffer, length, flags);
        if (received >= 0)
        {
            return received;
        }
        const int error = errno;
        if (error == EINTR)
        {
            continue;
        }

        if (error != EAGAIN && error != EWOULDBLOCK)
        {
            LOG_ERROR("Failed to receive data: fd={}, errno={}", _sockfd.GetFd(), error);
        }
        // 日志内部可能调用其他库函数或系统调用，从而改变 errno，因此返回前需要恢复
        errno = error;
        // EAGAIN / EWOULDBLOCK → 不打印错误日志 → 返回 -1
        // 其他非 EINTR 错误   → 打印错误日志   → 返回 -1
        return -1;
    }
}

ssize_t Socket::NonBlockRecv(void *buf, size_t len)
{
    return Recv(buf, len, MSG_DONTWAIT);
}

ssize_t Socket::Send(const void *buf, size_t len, int flag)
{
    while (true)
    {
        ssize_t n = send(_sockfd.GetFd(), buf, len, flag | MSG_NOSIGNAL);
        // 返回实际发送字节数
        if (n >= 0)
        {
            return n;
        }
        const int saved_errno = errno;
        // 当前socket的阻塞等待被信号打断
        if (saved_errno == EINTR)
        {
            continue;
        }
        if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK)
        {
            LOG_ERROR("Failed to send: errno={}", saved_errno);
        }
        errno = saved_errno;
        // EAGAIN / EWOULDBLOCK → 不打印错误日志 → 返回 -1
        // 其他非 EINTR 错误   → 打印错误日志   → 返回 -1
        return -1;
    }
}

ssize_t Socket::NonBlockSend(const void *buf, size_t len)
{
    return Send(buf, len, MSG_DONTWAIT);
}

// 关闭后可重复调用；保留调用前 errno，不对 close 的 EINTR 重试。
void Socket::Close() noexcept
{
    _sockfd.Close();
}

// 创建非阻塞监听套接字，失败释放本次创建的资源并保留 errno。
bool Socket::CreateServer(uint16_t port, const std::string &ip)
{
    if (!Create())
        return false;
    if (!ReuseAddress() || !SetNonBlock() || !Bind(ip, port) || !Listen())
    {
        Close();
        return false;
    }
    return true;
}

// 创建阻塞客户端连接，失败释放本次创建的资源并保留 errno。
bool Socket::CreateClient(uint16_t port, const std::string &ip)
{
    if (!Create())
        return false;
    if (!Connect(ip, port))
    {
        Close();
        return false;
    }
    return true;
}

// 必须在 Bind 前调用；同时开启地址和端口重用。
bool Socket::ReuseAddress()
{
    const int opt = 1;
    if (setsockopt(_sockfd.GetFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1 ||
        setsockopt(_sockfd.GetFd(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1)
    {
        const int saved_errno = errno;
        LOG_ERROR("Failed to enable address reuse: errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    return true;
}

// 保留原有状态标志，增加非阻塞属性。
bool Socket::SetNonBlock()
{
    const int flags = fcntl(_sockfd.GetFd(), F_GETFL, 0);
    if (flags == -1 || fcntl(_sockfd.GetFd(), F_SETFL, flags | O_NONBLOCK) == -1)
    {
        const int saved_errno = errno;
        LOG_ERROR("Failed to set nonblocking mode: errno={}", saved_errno);
        errno = saved_errno;
        return false;
    }
    return true;
}

// 借用描述符，不转移所有权。
int Socket::GetFd() const noexcept
{
    return _sockfd.GetFd();
}
