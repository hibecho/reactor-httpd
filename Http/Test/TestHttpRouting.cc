// 路由与钩子：路径参数、优先级、404 兜底、访问日志钩子、405 的 Allow 归集
//
// 与 TestHttpServer 的分工：那边覆盖的是精确路由与静态文件这套既有行为，
// 这里只验当前这一轮新增的几项，并确认它们没有把既有优先级搞乱。
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

#include "protocol/HttpServer.hpp"

namespace
{

const char *g_stage = "startup";

void OnAlarm(int)
{
    const char *prefix = "TestHttpRouting timed out: ";
    (void)!write(STDERR_FILENO, prefix, strlen(prefix));
    (void)!write(STDERR_FILENO, g_stage, strlen(g_stage));
    (void)!write(STDERR_FILENO, "\n", 1);
    _exit(2);
}

int Connect(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    timeval timeout{5, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    return fd;
}

void Send(int fd, const std::string &data)
{
    std::size_t offset = 0;
    while (offset != data.size())
    {
        const ssize_t count = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

std::string Request(const std::string &method, const std::string &path)
{
    return method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
}

void ReadExact(int fd, char *out, std::size_t size)
{
    std::size_t offset = 0;
    while (offset != size)
    {
        const ssize_t count = recv(fd, out + offset, size - offset, 0);
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

std::string ReadLine(int fd)
{
    std::string line;
    char byte = 0;
    while (true)
    {
        const ssize_t count = recv(fd, &byte, 1, 0);
        assert(count == 1);
        line += byte;
        if (byte == '\n')
            return line;
        assert(line.size() < 8192);
    }
}

struct Response
{
    int status = 0;
    std::string allow;
    std::string body;
};

Response Receive(int fd)
{
    Response response;
    std::string status_line = ReadLine(fd);
    const auto first_space = status_line.find(' ');
    assert(first_space != std::string::npos);
    response.status = std::stoi(status_line.substr(first_space + 1));

    std::size_t length = 0;
    while (true)
    {
        const std::string line = ReadLine(fd);
        if (line == "\r\n")
            break;
        const auto colon = line.find(':');
        assert(colon != std::string::npos);
        std::string name = line.substr(0, colon);
        for (char &c : name)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        std::size_t start = colon + 1;
        while (line[start] == ' ')
            ++start;
        const std::string value = line.substr(start, line.size() - start - 2);
        if (name == "content-length")
            length = std::stoul(value);
        else if (name == "allow")
            response.allow = value;
    }

    response.body.resize(length);
    if (length != 0)
        ReadExact(fd, response.body.data(), length);
    return response;
}

// 一个连接发一条请求，收完就关
Response Exchange(uint16_t port, const std::string &request)
{
    const int fd = Connect(port);
    Send(fd, request);
    Response response = Receive(fd);
    close(fd);
    return response;
}

// --------------------------------------------------------------------------

// 路径参数：模式里的 :name 抓下一段，交给 handler 按名字取
void ScenarioPathParams()
{
    g_stage = "path-params";
    alarm(30);

    HttpServer server(0);
    server.AddParamRoute("GET", "/api/session/:id/history", [](const HttpRequest &request, HttpResponse &response) {
        response.SetBody("history of " + request.GetPathParam("id"));
    });
    server.AddParamRoute("DELETE", "/api/session/:id", [](const HttpRequest &request, HttpResponse &response) {
        assert(request.FindPathParam("id"));
        response.SetBody("deleted " + request.GetPathParam("id"));
    });
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const Response history = Exchange(port, Request("GET", "/api/session/abc123/history"));
        assert(history.status == 200);
        assert(history.body == "history of abc123");

        const Response removed = Exchange(port, Request("DELETE", "/api/session/xyz"));
        assert(removed.status == 200);
        assert(removed.body == "deleted xyz");

        // /api/session/:id 这个模式是存在的，只是没注册 GET——归成 405 而不是 404
        const Response wrong_method = Exchange(port, Request("GET", "/api/session/abc123"));
        assert(wrong_method.status == 405);
        assert(wrong_method.allow.find("DELETE") != std::string::npos);

        // 段数不对、或参数段为空，都不算命中
        assert(Exchange(port, Request("GET", "/api/session//history")).status == 404);
        assert(Exchange(port, Request("GET", "/api/session/abc123/history/extra")).status == 404);
        assert(Exchange(port, Request("GET", "/api/nothing/here")).status == 404);

        // 参数不存在时取到空串，而不是抛异常
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 精确路由优先于参数路由：一条字面路径不能被模式抢走
void ScenarioExactBeatsParam()
{
    g_stage = "exact-beats-param";
    alarm(30);

    HttpServer server(0);
    server.AddParamRoute("GET", "/api/session/:id", [](const HttpRequest &request, HttpResponse &response) {
        response.SetBody("param:" + request.GetPathParam("id"));
    });
    server.AddRoute("GET", "/api/session/special", [](const HttpRequest &, HttpResponse &response) {
        response.SetBody("exact");
    });
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        assert(Exchange(port, Request("GET", "/api/session/special")).body == "exact");
        assert(Exchange(port, Request("GET", "/api/session/other")).body == "param:other");
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 404 兜底钩子：路径未知时由它决定响应；但「路径存在、方法不对」仍走 405
void ScenarioNotFoundHandler()
{
    g_stage = "not-found";
    alarm(30);

    HttpServer server(0);
    server.AddRoute("GET", "/known", [](const HttpRequest &, HttpResponse &response) {
        response.SetBody("known");
    });
    server.SetNotFoundHandler([](const HttpRequest &request, HttpResponse &response) {
        if (request.GetPath().rfind("/api/", 0) == 0)
        {
            response.SetStatus(404);
            response.SetBody(R"({"success":false,"message":"no such endpoint"})", "application/json");
            return;
        }
        response.SetStatus(404);
        response.SetBody("not found");
    });
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const Response api = Exchange(port, Request("GET", "/api/nope"));
        assert(api.status == 404);
        assert(api.body == R"({"success":false,"message":"no such endpoint"})");

        const Response other = Exchange(port, Request("GET", "/nope"));
        assert(other.status == 404);
        assert(other.body == "not found");

        // 路径存在但方法不对：走 405，不该被 404 钩子接管
        const Response wrong_method = Exchange(port, Request("POST", "/known"));
        assert(wrong_method.status == 405);
        assert(wrong_method.allow.find("GET") != std::string::npos);

        assert(Exchange(port, Request("GET", "/known")).body == "known");
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 参数路由也要计入 Allow：否则「路径存在、方法不对」会被误判成 404
void ScenarioParamAllow()
{
    g_stage = "param-allow";
    alarm(30);

    HttpServer server(0);
    server.AddParamRoute("GET", "/api/session/:id", [](const HttpRequest &, HttpResponse &response) {
        response.SetBody("ok");
    });
    server.SetNotFoundHandler([](const HttpRequest &, HttpResponse &response) {
        response.SetStatus(404);
        response.SetBody("hooked-not-found");
    });
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const Response wrong_method = Exchange(port, Request("DELETE", "/api/session/abc"));
        assert(wrong_method.status == 405);
        assert(wrong_method.allow.find("GET") != std::string::npos);
        assert(wrong_method.body != "hooked-not-found");

        // 完全不存在的路径才落到钩子
        assert(Exchange(port, Request("DELETE", "/api/nowhere")).body == "hooked-not-found");
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 访问日志钩子：每解析出一个请求调用一次，拿不到可写响应（它不该改变响应）
void ScenarioRequestHook()
{
    g_stage = "request-hook";
    alarm(30);

    std::atomic<int> seen{0};
    std::atomic<bool> saw_path{false};

    HttpServer server(0);
    server.AddRoute("GET", "/ping", [](const HttpRequest &, HttpResponse &response) {
        response.SetBody("pong");
    });
    server.SetRequestHook([&seen, &saw_path](const HttpRequest &request) {
        ++seen;
        if (request.GetMethod() == "GET" && request.GetPath() == "/ping")
            saw_path.store(true);
    });
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        assert(Exchange(port, Request("GET", "/ping")).body == "pong");
        assert(Exchange(port, Request("GET", "/nope")).status == 404);
        assert(seen.load() == 2);
        assert(saw_path.load());
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 模式的写法非法时要显式拒绝，不能悄悄降级成字面路由
void ScenarioInvalidPattern()
{
    g_stage = "invalid-pattern";
    alarm(30);

    HttpServer server(0);
    auto rejects = [&server](const std::string &pattern) {
        bool rejected = false;
        try
        {
            server.AddParamRoute("GET", pattern, [](const HttpRequest &, HttpResponse &response) {
                response.SetBody("x");
            });
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        assert(rejected);
    };

    rejects("/a/:");          // 空参数名
    rejects("/a/:x/:x");      // 重名
    rejects("/a/:x y");       // 参数名含非 token 字符
    rejects("");              // 空模式

}

} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGALRM, OnAlarm);

    ScenarioInvalidPattern();
    ScenarioPathParams();
    ScenarioExactBeatsParam();
    ScenarioNotFoundHandler();
    ScenarioParamAllow();
    ScenarioRequestHook();

    std::cout << "HttpServer routing tests passed\n";
    return 0;
}
