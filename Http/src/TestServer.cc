#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Socket.hpp"
#include <cerrno>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

// 简单 TCP 回显：收到什么就原样发回，使用 nc 127.0.0.1 8080 连接。
struct Connection
{
    Socket socket;
    Channel channel;
    std::string output;
    std::size_t sent = 0;
    bool closing = false;

    Connection(int fd, EventLoop &loop)
        : socket(fd)
        , channel(fd, &loop)
    {
    }
};

static bool WouldBlock()
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

int main()
{
    try
    {
        EventLoop loop;
        Socket listener;
        if (!listener.CreateServer(8080))
            throw std::runtime_error("监听 8080 端口失败");

        std::unordered_map<int, std::unique_ptr<Connection>> connections;
        Channel listen_channel(listener.GetFd(), &loop);

        // Handle() 可能还要执行其他回调，必须等本轮事件分发结束再销毁连接。
        auto close_connection = [&](Connection *conn) {
            if (conn->closing)
                return;
            conn->closing = true;
            loop.QueueInLoop([&, fd = conn->socket.GetFd()] {
                connections.at(fd)->channel.Remove();
                connections.erase(fd); // Socket 析构关闭 fd
            });
        };

        auto flush = [&](Connection *conn) {
            if (conn->closing)
                return;
            while (conn->sent < conn->output.size())
            {
                const ssize_t n = conn->socket.Send(conn->output.data() + conn->sent, conn->output.size() - conn->sent);
                if (n > 0)
                {
                    conn->sent += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && WouldBlock())
                {
                    conn->channel.EnableWrite(); // 等待可写后继续发送剩余数据
                    return;
                }
                close_connection(conn);
                return;
            }
            conn->output.clear();
            conn->sent = 0;
            conn->channel.DisableWrite();
            conn->channel.EnableRead();
        };

        listen_channel.SetReadCallback([&] {
            for (;;)
            {
                const int fd = listener.Accept();
                if (fd < 0)
                {
                    if (WouldBlock())
                        return;
                    throw std::runtime_error("accept 失败");
                }
                auto owner = std::make_unique<Connection>(fd, loop);
                Connection *conn = owner.get();
                if (!conn->socket.SetNonBlock())
                    throw std::runtime_error("设置连接非阻塞失败");
                conn->channel.SetReadCallback([&, conn] {
                    if (conn->closing || !conn->output.empty())
                        return;
                    char buffer[4096];
                    const ssize_t n = conn->socket.Recv(buffer, sizeof(buffer));
                    if (n > 0)
                    {
                        conn->output.assign(buffer, static_cast<std::size_t>(n));
                        // LT 模式下每次读一块即可；先发完这一块，再接收后续数据。
                        conn->channel.DisableRead();
                        flush(conn);
                    }
                    else if (n == 0 || !WouldBlock())
                    {
                        close_connection(conn);
                    }
                });
                conn->channel.SetWriteCallback([&, conn] { flush(conn); });
                conn->channel.SetErrorCallback([&, conn] { close_connection(conn); });
                conn->channel.SetCloseCallback([&, conn] { close_connection(conn); });
                connections.emplace(fd, std::move(owner));
                conn->channel.EnableRead();
            }
        });
        listen_channel.EnableRead();

        std::cout << "TCP 回显服务已启动：0.0.0.0:8080，按 Ctrl+C 退出" << std::endl;
        while (true)
            loop.Loop(); // 当前 Loop() 只处理一轮事件
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
