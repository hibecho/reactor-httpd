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
        // 1.构建Http服务器及网络底层设施
        HttpServer server(static_cast<uint16_t>(port));

        // 2.将 SIGINT、SIGTERM 接入正常停止流程
        server.EnableSignalStop();

        // 3.配置 4 个 I/O 工作线程
        server.SetThreadCount(4);

        // 4.启用连接不活跃超时回收，设置超时参数
        server.EnableInactiveRelease(30);

        // 5.配置静态资源目录
        // 从 Http 目录启动程序，URL 路径映射到 www 中的静态文件。
        server.SetDocumentRoot("./www");

        // 6. 请求方法 + 请求路径 + 请求调用函数
        server.AddRoute("POST", "/echo", [](const HttpRequest &request, HttpResponse &response) {
            response.SetBody(request.GetBody());
        });

        std::cout << "访问 http://127.0.0.1:" << port << "/，按 Ctrl+C 结束进程。" << std::endl;

        // 7.启动整个服务
        server.Start(); // 阻塞运行；所有配置在启动前完成。
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("服务器运行失败: {}", error.what());
        return 1;
    }
    return 0;
}
