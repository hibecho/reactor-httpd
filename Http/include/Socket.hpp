/**
 * @file Socket.hpp
 * @brief
 * 1.提供的功能:对套接字进行封装
 *
 * 2接口设计
 *  - 创建套接字
 *  - 绑定地址信息
 *  - 开始监听
 *  - 向服务器发起连接
 *  - 获取新连接
 *  - 接收数据
 *  - 关闭套接字
 *  - 创建一个服务端连接
 *  - 创建一个客户端连接
 *  - 设置套接字的选项：开启地址端口的复用
 *  - 设置套接字的阻塞属性: 设置为非阻塞
 *
 */

#pragma once
#include <cerrno>
#include <string>
#include <fcntl.h>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "Logger.hpp"
#define MAX_LISTEN_SIZE 1024

class Socket
{
public:
    Socket() : _sockfd(-1) {}
    // 接管 fd 的所有权，调用者不应再关闭它。
    explicit Socket(int fd) : _sockfd(fd) {}
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : _sockfd(other._sockfd)
    {
        other._sockfd = -1;
    }
    Socket &operator=(Socket &&other) noexcept
    {
        if (this != &other)
        {
            Close();
            _sockfd = other._sockfd;
            other._sockfd = -1;
        }
        return *this;
    }
    ~Socket() { Close(); }

    // 创建套接字
    bool Create()
    {
        if (_sockfd != -1)
        {
            errno = EALREADY;
            return false;
        }
        // int socket(int domain, int type, int protocol);
        _sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_sockfd == -1)
        {
            const int saved_errno = errno;
            LOG_ERROR("Failed to create socket:errno={}", saved_errno);
            errno = saved_errno;
            return false;
        }
        return true;
    }

    // 绑定地址信息
    bool Bind(const std::string &ip_address, uint16_t port)
    {
        // int bind(int sockfd, const struct sockaddr *socket_address,socklen_t addrlen);

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
        if (bind(_sockfd, reinterpret_cast<sockaddr *>(&socket_address), address_length) == -1)
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
    bool Listen(int backlog = MAX_LISTEN_SIZE)
    {
        // int listen(int sockfd, int backlog);
        int ret = listen(_sockfd, backlog);
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
    bool Connect(const std::string &ip_address, uint16_t port)
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
        if (connect(_sockfd, reinterpret_cast<sockaddr *>(&socket_address), address_length) == -1)
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
    bool Connet(const std::string &ip_address, uint16_t port)
    {
        return Connect(ip_address, port);
    }

    // 获取新连接；成功返回描述符（由调用者负责关闭），失败返回 -1 并保留 errno。
    // Linux accept 返回的新描述符不继承监听套接字的 O_NONBLOCK。
    // 获取对端地址时，两个参数均须有效；*peer_address_length 输入缓冲区大小，
    // 输出实际地址长度。不需要地址时可直接调用 Accept()。
    // 非阻塞模式下 EAGAIN/EWOULDBLOCK 表示暂无连接，不记录错误日志。
    int Accept(sockaddr *peer_address = nullptr, socklen_t *peer_address_length = nullptr)
    {
        while (true)
        {
            int client_fd = accept(_sockfd, peer_address, peer_address_length);
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

    // 接收一次可用数据，返回实际字节数（可能小于 len）。
    // len > 0 时返回 0 表示对端发送方向有序关闭；len == 0 不能用于判断断连。
    // -1 表示失败，errno 为 EAGAIN/EWOULDBLOCK 时应等待可读后再试。
    ssize_t Recv(void *buf, size_t len, int flag = 0)
    {
        while (true)
        {
            ssize_t n = recv(_sockfd, buf, len, flag);
            if (n >= 0)
            {
                return n;
            }
            const int saved_errno = errno;
            if (saved_errno == EINTR)
            {
                continue;
            }
            if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK)
            {
                LOG_ERROR("Failed to recv: errno={}", saved_errno);
            }
            errno = saved_errno;
            return -1;
        }
    }
    ssize_t NonBlockRecv(void *buf, size_t len)
    {
        return Recv(buf, len, MSG_DONTWAIT);
    }

    // 发送一次数据，返回实际字节数；调用者负责后续发送剩余部分。
    // -1 表示失败，errno 为 EAGAIN/EWOULDBLOCK 时应等待可写后再试。
    // 抑制本次调用的 SIGPIPE，发送失败通过返回值和 errno 报告。
    ssize_t Send(const void *buf, size_t len, int flag = 0)
    {
        while (true)
        {
            ssize_t n = send(_sockfd, buf, len, flag | MSG_NOSIGNAL);
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
            return -1;
        }
    }

    ssize_t NonBlockSend(const void *buf, size_t len)
    {
        return Send(buf, len, MSG_DONTWAIT);
    }

    // 关闭后可重复调用；保留调用前 errno，不对 close 的 EINTR 重试。
    void Close() noexcept
    {
        if (_sockfd != -1)
        {
            const int saved_errno = errno;
            const int fd = _sockfd;
            _sockfd = -1;
            close(fd);
            errno = saved_errno;
        }
    }

    // 创建非阻塞监听套接字，失败释放本次创建的资源并保留 errno。
    bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0")
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
    bool CreateClient(uint16_t port, const std::string &ip)
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
    bool ReuseAddress()
    {
        const int opt = 1;
        if (setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1 ||
            setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1)
        {
            const int saved_errno = errno;
            LOG_ERROR("Failed to enable address reuse: errno={}", saved_errno);
            errno = saved_errno;
            return false;
        }
        return true;
    }

    // 保留原有状态标志，增加非阻塞属性。
    bool SetNonBlock()
    {
        const int flags = fcntl(_sockfd, F_GETFL, 0);
        if (flags == -1 || fcntl(_sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            const int saved_errno = errno;
            LOG_ERROR("Failed to set nonblocking mode: errno={}", saved_errno);
            errno = saved_errno;
            return false;
        }
        return true;
    }

    // 借用描述符，不转移所有权。
    int GetFd() const noexcept { return _sockfd; }

private:
    int _sockfd;
};