// 流式响应（SSE 场景）的真实回环验证
//
// 与 TestHttpServer 的分工：那边验的是同步路由与一次性响应，这里只验异步/流式那条路。
// 用裸 socket 而不是复用那边的 Receive()，是因为流式响应没有 Content-Length，必须自己
// 解分块编码——而「自己解一遍」恰好也是对编码正确性的独立检查。
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

#include "protocol/HttpResponder.hpp"
#include "protocol/HttpServer.hpp"
#include "protocol/HttpStreamWriter.hpp"

namespace
{

const char *g_stage = "startup";

void OnAlarm(int)
{
    const char *prefix = "TestHttpStreaming timed out: ";
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

std::string Request(const std::string &method, const std::string &path, const std::string &version = "HTTP/1.1")
{
    return method + " " + path + " " + version + "\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
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

// 读一行（含 CRLF）
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

struct Head
{
    int status = 0;
    bool chunked = false;
    bool has_content_length = false;
    bool keep_alive = false;
    std::string content_type;
};

Head ReadHead(int fd)
{
    Head head;
    std::string status_line = ReadLine(fd);
    const auto first_space = status_line.find(' ');
    assert(first_space != std::string::npos);
    head.status = std::stoi(status_line.substr(first_space + 1));

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

        if (name == "transfer-encoding")
            head.chunked = (value == "chunked");
        else if (name == "content-length")
            head.has_content_length = true;
        else if (name == "connection")
            head.keep_alive = (value == "keep-alive");
        else if (name == "content-type")
            head.content_type = value;
    }
    return head;
}

// 解一个分块。返回该块的内容；终止块返回空串（并吃掉它后面的空行）。
std::string ReadChunk(int fd)
{
    const std::string size_line = ReadLine(fd);
    assert(size_line.size() >= 3);
    const std::size_t size = std::stoul(size_line.substr(0, size_line.size() - 2), nullptr, 16);
    if (size == 0)
    {
        assert(ReadLine(fd) == "\r\n");
        return std::string();
    }
    std::string data(size, '\0');
    ReadExact(fd, data.data(), size);
    assert(ReadLine(fd) == "\r\n");
    return data;
}

// --------------------------------------------------------------------------

// 逐块推送：这是整件事的核心。
//
// 判据刻意做成【结构性】而不是比时间。handler 写完第一块后等客户端确认收到，再写第二块；
// 若响应是攒齐了一次性发的，客户端永远读不到第一块，handler 会等超时并断言失败。
//
// 用墙钟阈值（「第一块必须在 N 毫秒内到达」）也能测，但在测试机负载高时会抖动，而且抖动
// 方向恰好是「把好的实现判成坏的」——实测单跑 8 次都是 0ms，混在 ctest 里跑却偶发失败。
// 这个版本与机器快慢无关。
void ScenarioIncrementalDelivery()
{
    g_stage = "incremental";
    alarm(30);

    std::atomic<bool> first_seen{false};

    HttpServer server(0);
    server.AddAsyncRoute(
        "GET", "/sse", [&first_seen](const HttpRequest &, const std::shared_ptr<HttpResponder> &reply) {
            HttpResponse head(200);
            head.SetHeader("Content-Type", "text/event-stream");
            auto writer = reply->BeginStream(std::move(head));
            assert(writer != nullptr);
            assert(writer->Write("data: one\n\n"));

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!first_seen.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            assert(first_seen.load());

            assert(writer->Write("data: two\n\n"));
            assert(writer->Finish());
        });
    // 流式/阻塞 handler 必须在事件循环之外执行，否则写出的数据要等 handler 返回才发得出去
    server.SetBusinessThreadCount(2);
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const int fd = Connect(port);
        Send(fd, Request("GET", "/sse"));

        const Head head = ReadHead(fd);
        assert(head.status == 200);
        assert(head.chunked);
        assert(!head.has_content_length);
        assert(head.content_type == "text/event-stream");

        // 这一行能返回，就说明第一块是在 handler 还在跑的时候到达的
        assert(ReadChunk(fd) == "data: one\n\n");
        first_seen.store(true);

        assert(ReadChunk(fd) == "data: two\n\n");
        assert(ReadChunk(fd).empty()); // 终止块

        // M1 的流式响应一律 close 语义
        char trailing = 0;
        assert(recv(fd, &trailing, 1, 0) == 0);
        close(fd);
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 空写入必须是 no-op：零长度块是分块编码的终止符，误发会让客户端以为响应提前结束。
void ScenarioEmptyWriteIsNoop()
{
    g_stage = "empty-write";
    alarm(30);

    HttpServer server(0);
    server.AddAsyncRoute("GET", "/empty", [](const HttpRequest &, const std::shared_ptr<HttpResponder> &reply) {
        auto writer = reply->BeginStream(HttpResponse(200));
        assert(writer != nullptr);
        assert(writer->Write(""));
        assert(writer->Write("payload"));
        assert(writer->Write(""));
        assert(writer->Finish());
    });
    // 流式/阻塞 handler 必须在事件循环之外执行，否则写出的数据要等 handler 返回才发得出去
    server.SetBusinessThreadCount(2);
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const int fd = Connect(port);
        Send(fd, Request("GET", "/empty"));
        const Head head = ReadHead(fd);
        assert(head.chunked);
        // 只有一块 payload，空写入没有插进来把流截断
        assert(ReadChunk(fd) == "payload");
        assert(ReadChunk(fd).empty());
        close(fd);
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// handler 一个字节都没发：框架必须补一个 500，不能把连接干挂着
void ScenarioNoResponseFallback()
{
    g_stage = "no-response";
    alarm(30);

    HttpServer server(0);
    server.AddAsyncRoute("GET", "/silent", [](const HttpRequest &, const std::shared_ptr<HttpResponder> &) {});
    // 流式/阻塞 handler 必须在事件循环之外执行，否则写出的数据要等 handler 返回才发得出去
    server.SetBusinessThreadCount(2);
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const int fd = Connect(port);
        Send(fd, Request("GET", "/silent"));
        const Head head = ReadHead(fd);
        assert(head.status == 500);
        assert(head.has_content_length);
        close(fd);
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 同一个 responder 只能产生一次响应；第二次调用要被拒绝
void ScenarioSingleResponse()
{
    g_stage = "single-response";
    alarm(30);

    HttpServer server(0);
    server.AddAsyncRoute("GET", "/once", [](const HttpRequest &, const std::shared_ptr<HttpResponder> &reply) {
        HttpResponse first(200);
        first.SetBody("first");
        assert(reply->Send(std::move(first)));
        assert(reply->Done());

        HttpResponse second(200);
        second.SetBody("second");
        assert(!reply->Send(std::move(second))); // 已经用过了
        assert(reply->BeginStream(HttpResponse(200)) == nullptr);
    });
    // 流式/阻塞 handler 必须在事件循环之外执行，否则写出的数据要等 handler 返回才发得出去
    server.SetBusinessThreadCount(2);
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const int fd = Connect(port);
        Send(fd, Request("GET", "/once"));
        const Head head = ReadHead(fd);
        assert(head.status == 200);
        std::string body(5, '\0');
        ReadExact(fd, body.data(), body.size());
        assert(body == "first");
        close(fd);
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

// 异步路由上抛异常：不能把连接丢掉，要变成可读的 500
void ScenarioHandlerThrows()
{
    g_stage = "handler-throws";
    alarm(30);

    HttpServer server(0);
    server.AddAsyncRoute("GET", "/boom", [](const HttpRequest &, const std::shared_ptr<HttpResponder> &) {
        throw std::runtime_error("handler failed on purpose");
    });
    // 流式/阻塞 handler 必须在事件循环之外执行，否则写出的数据要等 handler 返回才发得出去
    server.SetBusinessThreadCount(2);
    const uint16_t port = server.GetPort();

    std::thread driver([&] {
        const int fd = Connect(port);
        Send(fd, Request("GET", "/boom"));
        const Head head = ReadHead(fd);
        assert(head.status == 500);
        close(fd);
        server.Stop();
    });
    server.Start();
    driver.join();
    alarm(0);
}

} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGALRM, OnAlarm);

    ScenarioIncrementalDelivery();
    ScenarioEmptyWriteIsNoop();
    ScenarioNoResponseFallback();
    ScenarioSingleResponse();
    ScenarioHandlerThrows();

    std::cout << "HttpServer streaming tests passed\n";
    return 0;
}
