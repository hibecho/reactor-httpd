// 真实回环集成测试；通过公共 Stop() 完成关停。
#include <any>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "base/Logger.hpp"
#include "protocol/HttpContext.hpp"
#include "protocol/HttpResponse.hpp"
#include "reactor/EventLoop.hpp"
#include "tcp/Acceptor.hpp"
#include "tcp/Connection.hpp"
#define private public
#include "protocol/HttpServer.hpp"
#include "tcp/TcpServer.hpp"
#include "thread/LoopThread.hpp"
#include "thread/LoopThreadPool.hpp"
#undef private

namespace
{
    std::atomic<const char *> g_stage{"startup"};
    static_assert(std::atomic<const char *>::is_always_lock_free, "signal handler needs a lock-free stage pointer");
    void OnAlarm(int)
    {
        const char *prefix = "TestHttpServer timed out: ";
        const char *stage = g_stage.load();
        (void)!write(STDERR_FILENO, prefix, strlen(prefix));
        (void)!write(STDERR_FILENO, stage, strlen(stage));
        (void)!write(STDERR_FILENO, "\n", 1);
        _exit(2);
    }

    template <class Exception, class Function> void Rejects(Function function)
    {
        bool rejected = false;
        try
        {
            function();
        }
        catch (const Exception &)
        {
            rejected = true;
        }
        assert(rejected);
    }

    template <class Function> void InLoop(EventLoop *loop, Function function)
    {
        std::promise<void> done;
        auto ready = done.get_future();
        loop->QueueInLoop([&] {
            function();
            done.set_value();
        });
        assert(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    template <class Predicate> void WaitFor(Predicate predicate)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate())
        {
            assert(std::chrono::steady_clock::now() < end);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    int Connect(uint16_t port)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        timeval timeout{5, 0};
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
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
            ssize_t count = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
            assert(count > 0);
            offset += static_cast<std::size_t>(count);
        }
    }

    struct Response
    {
        std::string version;
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    Response Receive(int fd, bool head = false)
    {
        std::string header;
        while (header.size() < 4 || header.compare(header.size() - 4, 4, "\r\n\r\n") != 0)
        {
            char byte;
            assert(recv(fd, &byte, 1, 0) == 1);
            header += byte;
            assert(header.size() < 65536);
        }
        Response response;
        std::istringstream input(header);
        input >> response.version >> response.status;
        std::string line;
        std::getline(input, line);
        while (std::getline(input, line) && line != "\r")
        {
            const auto colon = line.find(':');
            assert(colon != std::string::npos);
            std::string name = line.substr(0, colon);
            for (char &c : name)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            std::size_t start = colon + 1;
            while (line[start] == ' ')
                ++start;
            response.headers[name] = line.substr(start, line.size() - start - 1);
        }
        if (!head)
        {
            response.body.resize(std::stoul(response.headers.at("content-length")));
            std::size_t offset = 0;
            while (offset < response.body.size())
            {
                ssize_t count = recv(fd, response.body.data() + offset, response.body.size() - offset, 0);
                assert(count > 0);
                offset += static_cast<std::size_t>(count);
            }
        }
        return response;
    }

    void Eof(int fd)
    {
        char byte;
        assert(recv(fd, &byte, 1, 0) == 0);
        close(fd);
    }

    std::string Request(const std::string &method, const std::string &path, bool close = true)
    {
        return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" + (close ? "Connection: close\r\n" : "") +
               "\r\n";
    }

    struct Files
    {
        std::filesystem::path base;
        Files()
        {
            char path[] = "/tmp/http-server-test-XXXXXX";
            char *created = mkdtemp(path);
            assert(created);
            base = created;
            std::filesystem::create_directories(base / "root" / "dir");
            std::filesystem::create_directories(base / "root" / "escape");
            // 存在但没有 index.html 的目录：必须 404，不能退化成目录列表。
            std::filesystem::create_directories(base / "root" / "noindex");
            std::ofstream(base / "secret") << "outside";
            std::ofstream(base / "root" / "plain.txt") << "static-body";
            std::ofstream(base / "root" / "file.txt") << "static-body";
            std::ofstream(base / "root" / "dir" / "index.html") << "index-body";
            std::ofstream(base / "root" / "%2e%2e") << "literal-percent";
            std::ofstream(base / "root" / "noindex" / "data.txt") << "unlisted";
            std::ofstream(base / "root" / "blob.zzz") << "blob";
            std::ofstream(base / "root" / "UPPER.TXT") << "upper";
            std::ofstream(base / "root" / "large.bin") << 'x';
            std::filesystem::resize_file(base / "root" / "large.bin", 8 * 1024 * 1024 + 1);
            // 恰好 8 MiB 的文件必须能正常送出，边界不能写成"大于等于上限就拒绝"。
            std::ofstream(base / "root" / "exact.bin") << 'y';
            std::filesystem::resize_file(base / "root" / "exact.bin", 8 * 1024 * 1024);
            std::filesystem::create_symlink(base / "secret", base / "root" / "leak");
            std::filesystem::create_symlink(base / "secret", base / "root" / "escape" / "index.html");
        }
        ~Files()
        {
            std::filesystem::remove_all(base);
        }
    };

    void Scenario(int workers)
    {
        alarm(30);
        Files files;
        HttpServer server(0);
        std::thread foreign_config([&] {
            Rejects<std::logic_error>([&] {
                server.SetThreadCount(0);
            });
            Rejects<std::logic_error>([&] {
                server.EnableInactiveRelease(1);
            });
            Rejects<std::logic_error>([&] {
                server.SetDocumentRoot(files.base / "root");
            });
            Rejects<std::logic_error>([&] {
                server.AddRoute("GET", "/foreign", [](const HttpRequest &, HttpResponse &) {
                });
            });
            Rejects<std::logic_error>([&] {
                server.Start();
            });
        });
        foreign_config.join();
        Rejects<std::invalid_argument>([&] {
            server.SetThreadCount(-1);
        });
        Rejects<std::invalid_argument>([&] {
            server.EnableInactiveRelease(0);
        });
        Rejects<std::invalid_argument>([&] {
            server.EnableInactiveRelease(61);
        });
        Rejects<std::invalid_argument>([&] {
            server.EnableInactiveRelease(-1);
        });
        Rejects<std::invalid_argument>([&] {
            server.SetDocumentRoot(files.base / "missing");
        });
        // 静态目录必须是目录：指向普通文件同样要拒绝。
        Rejects<std::invalid_argument>([&] {
            server.SetDocumentRoot(files.base / "secret");
        });
        // 1..60 是闭区间，两个端点都要接受。
        server.EnableInactiveRelease(1);
        server.EnableInactiveRelease(60);
        server.SetThreadCount(workers);
        server.EnableInactiveRelease(20);
        server.SetDocumentRoot(files.base / "root");
        std::atomic<int> hits{0};
        std::atomic<int> partials{0};
        auto hello = [&](const HttpRequest &, HttpResponse &response) {
            ++hits;
            response.SetBody("hello");
        };
        server.AddRoute("GET", "/hello", hello);
        server.AddRoute("GET", "/plain.txt", hello);
        server.AddRoute("GET", "/head", hello);
        server.AddRoute("HEAD", "/head", [](const HttpRequest &, HttpResponse &response) {
            response.SetBody("head-only");
        });
        server.AddRoute("POST", "/echo", [](const HttpRequest &request, HttpResponse &response) {
            response.SetBody(request.GetBody());
        });
        server.AddRoute("GET", "/fail", [](const HttpRequest &, HttpResponse &response) {
            response.SetHeader("X-Partial", "discard");
            throw std::runtime_error("handler failure");
        });
        server.AddRoute("GET", "/close", [](const HttpRequest &, HttpResponse &response) {
            response.SetBody("bye");
            response.SetClose(true);
        });
        // 抛出非 std 异常：必须与 std::exception 走同一条 500 分支，而不是让异常逃逸出去。
        server.AddRoute("GET", "/fail-unknown", [](const HttpRequest &, HttpResponse &) {
            throw 42;
        });
        // 只注册 POST /only，用来观察 405 的 Allow 精确取值。
        server.AddRoute("POST", "/only", [](const HttpRequest &, HttpResponse &response) {
            response.SetBody("only");
        });
        // 方法名按大小写精确匹配：注册 "Get" 不应命中 "GET"。
        server.AddRoute("Get", "/case", hello);
        // OPTIONS * 是唯一允许的非路径目标，需要显式注册才能命中。
        server.AddRoute("OPTIONS", "*", [](const HttpRequest &, HttpResponse &response) {
            response.SetBody("options-star");
        });
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("GET", "/hello", hello);
        });
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("GET", "/empty", {});
        });
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("GET", "", hello);
        }); // 空路径
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("GET", "hello", hello);
        }); // 缺前导斜杠
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("GET",
                            "/a\x01"
                            "b",
                            hello);
        }); // 路径含控制字符
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("", "/x", hello);
        }); // 空方法
        Rejects<std::invalid_argument>([&] {
            server.AddRoute("G@T", "/x", hello);
        }); // 方法含非 token 字符
        // 观察真实读回调完成，确保下一片发送前，前一片已经由 HttpServer 消费。
        server._server.SetMessageCallback([&](const ConnectionPtr &conn, Buffer *buffer) {
            server.OnMessage(conn, buffer);
            ++partials;
        });
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        assert(getsockname(server._server._acceptor->GetListenFd(), reinterpret_cast<sockaddr *>(&address), &length) ==
               0);
        const uint16_t port = ntohs(address.sin_port);

        std::thread driver([&] {
            g_stage = "configuration freeze";
            InLoop(server._server._baseloop.get(), [&] {
                Rejects<std::logic_error>([&] {
                    server.SetThreadCount(0);
                });
                Rejects<std::logic_error>([&] {
                    server.EnableInactiveRelease(1);
                });
                Rejects<std::logic_error>([&] {
                    server.SetDocumentRoot(files.base);
                });
                Rejects<std::logic_error>([&] {
                    server.AddRoute("GET", "/late", hello);
                });
                Rejects<std::logic_error>([&] {
                    server.Start();
                });
            });
            int fd = Connect(port);
            g_stage = "fragmented requests and pipelining";
            int observed = partials.load();
            Send(fd, "POST /echo HTTP/1.1\r\nHost: local");
            WaitFor([&] {
                return partials.load() > observed;
            });
            observed = partials.load();
            Send(fd, "host\r\nContent-Length: 5\r\n\r\nhe");
            WaitFor([&] {
                return partials.load() > observed;
            });
            Send(fd, "llo");
            auto response = Receive(fd);
            assert(response.status == 200 && response.body == "hello");
            assert(response.headers.at("connection") == "keep-alive");
            Send(fd, Request("HEAD", "/hello", false) + Request("GET", "/hello"));
            response = Receive(fd, true);
            assert(response.status == 200 && response.headers.at("content-length") == "5");
            assert(Receive(fd).body == "hello");
            Eof(fd);

            for (const std::string &path : {std::string("/hello"), std::string("/close")})
            {
                const int before = hits.load();
                fd = Connect(port);
                Send(fd, Request("GET", path, path == "/hello") + Request("GET", "/hello"));
                response = Receive(fd);
                assert(response.status == 200);
                Eof(fd);
                assert(hits.load() == before + (path == "/hello" ? 1 : 0));
            }
            fd = Connect(port);
            g_stage = "HTTP/1.0 reuse";
            Send(fd, "GET /hello HTTP/1.0\r\n\r\n");
            response = Receive(fd);
            assert(response.version == "HTTP/1.0" && response.headers.at("connection") == "close");
            Eof(fd);
            fd = Connect(port);
            Send(fd, "GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
            assert(Receive(fd).headers.at("connection") == "keep-alive");
            Send(fd, Request("GET", "/hello"));
            assert(Receive(fd).body == "hello");
            Eof(fd);

            auto exchange = [&](const std::string &request, bool head = false) {
                int client = Connect(port);
                Send(client, request);
                Response result = Receive(client, head);
                Eof(client);
                return result;
            };
            assert(exchange(Request("GET", "/missing")).status == 404);
            // 405 的 Allow 必须精确：Allow 由 set 排序后拼接，只断言"包含 GET"会让重复、乱序、多出方法的变异存活。
            response = exchange(Request("POST", "/hello"));
            assert(response.status == 405 && response.headers.at("allow") == "GET, HEAD");
            // 只注册了 POST /only：没有 GET 就不该派生 HEAD，Allow 恰为 POST。
            response = exchange(Request("GET", "/only"));
            assert(response.status == 405 && response.headers.at("allow") == "POST");
            // 405 的响应体与 404 一样是状态描述，不能凭状态码区间下结论。
            assert(response.body == "Method Not Allowed\n");
            assert(exchange(Request("DELETE", "/missing")).status == 404);
            g_stage = "routing and method matching";
            // 方法名按大小写精确匹配：注册的是 "Get"，所以 "GET" 未命中；Allow 里原样回显 "Get"，
            // 若实现把方法名折叠过大小写，这里就会变成 "GET, HEAD"。
            response = exchange(Request("GET", "/case"));
            assert(response.status == 405 && response.headers.at("allow") == "Get");
            assert(exchange(Request("Get", "/case")).status == 200);
            // OPTIONS * 必须显式注册才命中；未注册的其它路径照常 404。
            assert(exchange(Request("OPTIONS", "*")).body == "options-star");
            assert(exchange(Request("OPTIONS", "/nope")).status == 404);
            g_stage = "error responses and close policy";
            response = exchange(Request("GET", "/fail", false));
            assert(response.status == 500 && response.headers.count("x-partial") == 0);
            assert(response.headers.at("connection") == "close");
            // 非 std 异常走同一条 500 分支并关闭连接。
            response = exchange(Request("GET", "/fail-unknown", false));
            assert(response.status == 500 && response.headers.at("connection") == "close");
            const int before_error = hits.load();
            response = exchange("GET / HTTP/1.1\r\n\r\n" + Request("GET", "/hello"));
            assert(response.status == 400);
            // 请求行解析失败后版本号可能还没就绪，错误响应回退到 HTTP/1.1 并要求关闭。
            assert(response.version == "HTTP/1.1" && response.headers.at("connection") == "close");
            assert(response.body == "Bad Request\n");
            assert(hits.load() == before_error);
            response = exchange("GARBAGE\r\n\r\n");
            assert(response.status == 400 && response.version == "HTTP/1.1");
            assert(exchange("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 8388609\r\n\r\n").status == 413);
            assert(
                exchange("POST /echo HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n")
                    .status == 417);
            // 请求行与头部超限经由 HttpServer 转换成对应状态码，而不是被当成 400。
            assert(exchange("GET /" + std::string(8200, 'a') + " HTTP/1.1\r\nHost: localhost\r\n\r\n").status == 414);
            assert(exchange("GET / HTTP/1.1\r\nHost: localhost\r\nX: " + std::string(32768, 'a') + "\r\n\r\n").status ==
                   431);
            assert(exchange(Request("GET", "/plain.txt")).body == "hello");
            g_stage = "static files and path boundaries";
            response = exchange(Request("GET", "/file.txt"));
            assert(response.body == "static-body");
            assert(response.headers.at("content-type") == "text/plain");
            // 未知扩展名退化为通用二进制类型，大写扩展名仍按小写查表。
            assert(exchange(Request("GET", "/blob.zzz")).headers.at("content-type") == "application/octet-stream");
            assert(exchange(Request("GET", "/UPPER.TXT")).headers.at("content-type") == "text/plain");
            assert(exchange(Request("HEAD", "/head"), true).headers.at("content-length") == "9");
            assert(exchange(Request("GET", "/dir/")).body == "index-body");
            // index.html 的类型来自查表，不是 SetBody 的默认值。
            assert(exchange(Request("GET", "/dir/")).headers.at("content-type") == "text/html");
            response = exchange(Request("HEAD", "/dir/"), true);
            assert(response.status == 200 && response.headers.at("content-length") == "10");
            response = exchange(Request("POST", "/dir/"));
            assert(response.status == 405 && response.headers.at("allow") == "GET, HEAD");
            // HEAD 命中 404 与静态文件时都不带正文，但仍要给出该资源的 Content-Length。
            response = exchange(Request("HEAD", "/missing"), true);
            assert(response.status == 404 && response.headers.at("content-length") == "10");
            response = exchange(Request("HEAD", "/file.txt"), true);
            assert(response.status == 200 && response.headers.at("content-length") == "11");
            // 存在但没有 index.html 的目录：404，绝不退化成目录列表。
            // 该路径根本不可静态服务，因此也不参与 Allow 推导，POST 同样是 404 而不是 405。
            assert(exchange(Request("GET", "/noindex/")).status == 404);
            assert(exchange(Request("POST", "/noindex/")).status == 404);
            assert(exchange(Request("GET", "/%252e%252e")).body == "literal-percent");
            for (const std::string &path :
                 {std::string("/leak"), std::string("/escape/"), std::string("/%2e%2e/secret")})
            {
                // 越界目标是明确的 404，而不是"任何 4xx/5xx"，否则 500 也能蒙混过关。
                response = exchange(Request("GET", path));
                assert(response.status == 404 && response.body != "outside");
            }
            assert(exchange(Request("GET", "/large.bin", false)).status == 500);
            // 恰好 8 MiB 必须送出；上限写成"大于等于即拒绝"会让这条失败。
            response = exchange(Request("GET", "/exact.bin"));
            assert(response.status == 200 && response.headers.at("content-length") == "8388608");
            assert(response.body.size() == 8u * 1024u * 1024u && response.body[0] == 'y');
            g_stage = "sequential requests on one connection";
            fd = Connect(port);
            Send(fd, Request("GET", "/hello", false) + Request("GET", "/hello", false) + Request("GET", "/hello"));
            assert(Receive(fd).body == "hello");
            assert(Receive(fd).body == "hello");
            assert(Receive(fd).body == "hello");
            Eof(fd);

            g_stage = "connection cleanup and shutdown";
            server.Stop();
        });
        server.Start();
        driver.join();
        // Start() 返回后仍处于冻结状态。这条断言不依赖 base loop 跑在哪个线程：
        // 上面那个冻结用例是靠在 base loop 线程里回调，才让"线程不符"的检查让路的，
        // 一旦 base loop 改到独立线程上，它就会退化成只测线程检查而静默失效。
        bool frozen = false;
        try
        {
            server.SetThreadCount(0);
        }
        catch (const std::logic_error &error)
        {
            frozen = std::string(error.what()).find("frozen") != std::string::npos;
        }
        assert(frozen);
        alarm(0);
    }

    // 未配置静态目录时静态路径一律 404：这条覆盖 ResolveStaticFile 的 _document_root.empty() 分支，
    // 该分支在配置了站点目录的 Scenario 里永远走不到。
    void ScenarioWithoutDocumentRoot()
    {
        alarm(30);
        HttpServer server(0);
        server.AddRoute("GET", "/hello", [](const HttpRequest &, HttpResponse &response) {
            response.SetBody("hello");
        });
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        assert(getsockname(server._server._acceptor->GetListenFd(), reinterpret_cast<sockaddr *>(&address), &length) ==
               0);
        const uint16_t port = ntohs(address.sin_port);

        std::thread driver([&] {
            auto exchange = [&](const std::string &request) {
                int client = Connect(port);
                Send(client, request);
                Response result = Receive(client);
                Eof(client);
                return result;
            };
            assert(exchange(Request("GET", "/hello")).body == "hello");
            assert(exchange(Request("GET", "/plain.txt")).status == 404);
            assert(exchange(Request("GET", "/")).status == 404);
            // 静态关闭时 POST 同样落到"资源不存在"，而不是 405。
            assert(exchange(Request("POST", "/plain.txt")).status == 404);

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
    Scenario(0);
    Scenario(2);
    ScenarioWithoutDocumentRoot();
    std::cout << "HttpServer integration tests passed\n";
}
