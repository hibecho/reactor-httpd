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
 * 声明与实现分离，实现见 src/Socket.cc
 */

#pragma once
#include "base/FdGuard.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_LISTEN_SIZE 1024

class Socket
{
  public:
    /*Socket构造函数*/
    Socket();
    explicit Socket(int fd);
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;
    ~Socket();

    // 创建套接字
    bool Create();
    // 绑定地址信息
    bool Bind(const std::string &ip_address, uint16_t port);
    // 开始监听
    bool Listen(int backlog = MAX_LISTEN_SIZE);
    // 客户端向服务器发起连接
    bool Connect(const std::string &ip_address, uint16_t port);
    // 兼容旧接口名称。
    bool Connet(const std::string &ip_address, uint16_t port);

    // 获取新连接；成功返回描述符（由调用者负责关闭），失败返回 -1 并保留 errno。
    // Linux accept 返回的新描述符不继承监听套接字的 O_NONBLOCK。
    // 获取对端地址时，两个参数均须有效；*peer_address_length 输入缓冲区大小，
    // 输出实际地址长度。不需要地址时可直接调用 Accept()。
    // 非阻塞模式下 EAGAIN/EWOULDBLOCK 表示暂无连接，不记录错误日志。
    int Accept(sockaddr *peer_address = nullptr, socklen_t *peer_address_length = nullptr);

    // 接收一次可用数据，返回实际字节数（可能小于 len）。
    // len > 0 时返回 0 表示对端发送方向有序关闭；len == 0 不能用于判断断连。
    // -1 表示失败，errno 为 EAGAIN/EWOULDBLOCK 时应等待可读后再试。
    ssize_t Recv(void *buf, size_t len, int flag = 0);

    // 非阻塞接收
    ssize_t NonBlockRecv(void *buf, size_t len);

    // 发送一次数据，返回实际字节数；调用者负责后续发送剩余部分。
    // -1 表示失败，errno 为 EAGAIN/EWOULDBLOCK 时应等待可写后再试。
    // 抑制本次调用的 SIGPIPE，发送失败通过返回值和 errno 报告。
    ssize_t Send(const void *buf, size_t len, int flag = 0);

    // 非阻塞发送
    ssize_t NonBlockSend(const void *buf, size_t len);

    // 关闭后可重复调用；保留调用前 errno，不对 close 的 EINTR 重试。
    void Close() noexcept;

    // 创建非阻塞监听套接字，失败释放本次创建的资源并保留 errno。
    bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0");

    // 创建阻塞客户端连接，失败释放本次创建的资源并保留 errno。
    bool CreateClient(uint16_t port, const std::string &ip);

    // 必须在 Bind 前调用；同时开启地址和端口重用。
    bool ReuseAddress();

    // 保留原有状态标志，增加非阻塞属性。
    bool SetNonBlock();

    // 借用描述符，不转移所有权。
    int GetFd() const noexcept;

  private:
    FdGuard _sockfd; // 独占持有，析构即关闭
};
