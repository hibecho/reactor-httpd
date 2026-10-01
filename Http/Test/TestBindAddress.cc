// 监听地址：构造时传的 ip 必须真的被用上。
//
// 值得单独测，是因为「绑得比要求的更宽」不会让任何功能失败——传了 127.0.0.1 却被
// 绑到 0.0.0.0，本地照常连通、测试照常全绿，只是服务悄悄暴露在所有网卡上。
#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

// 标准库头必须在这里全部包含完：#define private public 之后再展开它们，
// 会把 std 内部类的私有成员也一并改掉，直接编译失败
#include <algorithm>
#include <any>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define private public
#include "protocol/HttpServer.hpp"
#include "tcp/Acceptor.hpp"
#include "tcp/TcpServer.hpp"
#undef private

namespace
{

// 从监听套接字上读回实际绑定的地址
std::string BoundAddress(HttpServer &server)
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const int fd = server._server._acceptor->GetListenFd();
    assert(fd >= 0);
    assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0);

    char text[INET_ADDRSTRLEN] = {0};
    assert(inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) != nullptr);
    return std::string(text);
}

} // namespace

int main()
{
    // 不传 ip：沿用 0.0.0.0 的历史行为
    {
        HttpServer server(0);
        assert(BoundAddress(server) == "0.0.0.0");
    }

    // 传回环：必须只绑回环
    {
        HttpServer server(0, "127.0.0.1");
        assert(BoundAddress(server) == "127.0.0.1");
        assert(server.GetPort() != 0);
    }

    std::cout << "HttpServer bind address tests passed\n";
    return 0;
}
