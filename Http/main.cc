#include "base/Logger.hpp"
#include "protocol/HttpServer.hpp"

#include <charconv>
#include <csignal>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>

int main(int argc, char *argv[])
{
    // 先校验参数，避免非法端口截断为 uint16_t 后绑定到其他端口。
    int port = 8080;
    if (argc > 2)
    {
        std::cerr << "用法: " << argv[0] << " [端口: 1..65535]\n";
        return 1;
    }
    if (argc == 2)
    {
        const std::string_view argument(argv[1]);
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), port);
        if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || port < 1 || port > 65535)
        {
            std::cerr << "端口必须是 1..65535 范围内的整数\n";
            return 1;
        }
    }

    // 对端提前关闭时，由连接层处理写入错误，避免 SIGPIPE 终止整个进程。
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        LOG_ERROR("无法忽略 SIGPIPE");
        return 1;
    }
    try
    {
        HttpServer server(static_cast<uint16_t>(port));
        server.EnableSignalStop(); // signalfd 将 SIGINT/SIGTERM 转换为正常 Stop。
        server.SetThreadCount(4);  // 主线程运行事件循环，保持示例简单。
        server.EnableInactiveRelease(30);
        // 从 Http 目录启动程序，URL 路径映射到 www 中的静态文件。
        server.SetDocumentRoot("./www");
        server.AddRoute("POST", "/echo", [](const HttpRequest &request, HttpResponse &response) {
            response.SetBody(request.GetBody());
        });

        std::cout << "访问 http://127.0.0.1:" << port << "/，按 Ctrl+C 结束进程。" << std::endl;
        server.Start(); // 阻塞运行；所有配置在启动前完成。
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("服务器运行失败: {}", error.what());
        return 1;
    }
    return 0;
}
