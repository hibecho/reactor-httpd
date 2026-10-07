#include "protocol/HttpServer.hpp"
#include "base/Logger.hpp"
#include "base/Util.hpp"
#include "protocol/HttpContext.hpp"

#include <fstream>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

namespace
{
constexpr std::size_t kMaxStaticFile = 8 * 1024 * 1024;

void SetErrorBody(HttpResponse &response, int status)
{
    response.SetStatus(status);
    response.SetBody(std::string(Util::StatusDescription(status)) + "\n");
}
} // namespace

HttpServer::HttpServer(uint16_t port)
    : _server(port)
{
    _server.SetConnectedCallback([this](const ConnectionPtr &conn) {
        OnConnected(conn);
    });
    _server.SetMessageCallback([this](const ConnectionPtr &conn, Buffer *buffer) {
        OnMessage(conn, buffer);
    });
}

void HttpServer::EnsureConfigurable() const
{
    // 先检查线程，错误线程不会读取可能正被构造线程修改的 _started。
    if (std::this_thread::get_id() != _owner_thread)
        throw std::logic_error("HttpServer: configure and start on the constructing thread");
    if (_started)
        throw std::logic_error("HttpServer: configuration is frozen after Start");
}

void HttpServer::SetThreadCount(int count)
{
    EnsureConfigurable();
    _server.SetThreadCount(count);
}

void HttpServer::EnableInactiveRelease(int timeout)
{
    EnsureConfigurable();
    if (timeout < 1 || timeout > 60)
        throw std::invalid_argument("HttpServer: inactivity timeout must be 1..60 ticks");
    _server.EnableInactiveRelease(timeout);
}

void HttpServer::AddRoute(std::string method, std::string path, Handler handler)
{
    EnsureConfigurable();
    if (!Util::IsToken(method) || !handler || path.empty() ||
        (path.front() != '/' && !(method == "OPTIONS" && path == "*")))
        throw std::invalid_argument("HttpServer: invalid route");
    for (unsigned char ch : path)
    {
        if (ch < 0x20 || ch == 0x7f)
            throw std::invalid_argument("HttpServer: control character in route path");
    }
    // path 已解码，字面量 '?' 或 '#' 也可能来自 %3F/%23，不能再拆查询串。
    if (!_routes.emplace(RouteKey{std::move(method), std::move(path)}, std::move(handler)).second)
        throw std::invalid_argument("HttpServer: duplicate route");
}

void HttpServer::SetDocumentRoot(std::filesystem::path root)
{
    EnsureConfigurable();
    std::error_code error;
    auto canonical = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(canonical, error) || error)
        throw std::invalid_argument("HttpServer: document root must be an existing directory");
    _document_root = std::move(canonical);
}

void HttpServer::Start()
{
    EnsureConfigurable();
    // 即使底层启动抛异常也不允许再次启动部分初始化过的服务器。
    _started = true;
    _server.Start();
}

void HttpServer::Stop()
{
    _server.Stop();
}

void HttpServer::SetResourceLimits(const ResourceLimits &limits)
{
    EnsureConfigurable();
    _server.SetResourceLimits(limits);
}

void HttpServer::SetShutdownGrace(std::chrono::milliseconds grace)
{
    EnsureConfigurable();
    _server.SetShutdownGrace(grace);
}

void HttpServer::EnableSignalStop()
{
    EnsureConfigurable();
    _server.EnableSignalStop();
}

void HttpServer::OnConnected(const ConnectionPtr &conn)
{
    conn->SetContext(HttpContext{});
}

void HttpServer::OnMessage(const ConnectionPtr &conn, Buffer *buffer)
{
    // 1.获取上下文， 失败 → 抛 logic_error
    HttpContext *context = std::any_cast<HttpContext>(&conn->GetContext());
    if (context == nullptr)
        throw std::logic_error("HttpServer: missing HTTP context");

    // 2. 每一轮，消化一个完整请求
    while (conn->CanProcessInput())
    {
        // 3.请求不完整，直接返回
        const auto result = context->Parse(*buffer);
        if (result == HttpContext::ParseResult::NeedMore)
            return;
        // 4.请求出现错误，构造错误响应
        if (result == HttpContext::ParseResult::Error)
        {
            HttpResponse error;
            SetErrorBody(error, context->GetErrorStatus());
            error.SetClose(true);
            SendResponse(conn, context->GetRequest(), error);
            return;
        }

        // 5.获得完整请求
        const auto &request = context->GetRequest();

        // 6.构造正常响应
        HttpResponse response;

        // 7.设置响应结束不关闭连接
        response.SetClose(false);

        try
        {
            // 8.根据request进行查找路由表，构造响应向客户端发送
            Dispatch(request, response);
        }
        catch (const std::bad_alloc &)
        {
            // 保持 Connection 的分配失败策略，不能假设此时还能分配错误响应。
            throw;
        }
        catch (const std::exception &error)
        {
            LOG_ERROR("HTTP request handler failed: {}", error.what());
            response = HttpResponse(500);
            SetErrorBody(response, 500);
        }
        catch (...)
        {
            LOG_ERROR("HTTP request handler failed: unknown exception");
            response = HttpResponse(500);
            SetErrorBody(response, 500);
        }

        // 9.序列化及 Send 不在业务异常捕获范围内，避免发送失败后重复生成响应。
        SendResponse(conn, request, response);
        if (response.IsClose())
            return;
        // 10.清空上下文
        context->Reset();
    }
}

void HttpServer::Dispatch(const HttpRequest &request, HttpResponse &response) const
{
    // 1.获得请求方法: method
    const auto &method = request.GetMethod();

    // 2.获得请求路径:path
    const auto &path = request.GetPath();

    // 3.查动态路由 _routes[{method, path}]
    auto route = _routes.find({method, path});

    // 4.路由表里 HEAD 查不到时再查 GET，是为了复用 GET 的handler 来生成正确的响应头和状态码
    // 此时只发送头、不发 body。
    if (route == _routes.end() && method == "HEAD")
        route = _routes.find({"GET", path});

    if (route != _routes.end())
    {
        route->second(request, response);
        return;
    }

    // 5.这个路径其实是磁盘上的静态文件
    std::filesystem::path file;
    const bool static_file = ResolveStaticFile(path, file);
    if (static_file && (method == "GET" || method == "HEAD"))
    {
        ServeStaticFile(file, response);
        return;
    }

    // 6.没能命中时的归因
    // 判断这个路径到底存不存在：完全不存在回 404，存在但方法不对回 405
    std::set<std::string> allowed;
    for (const auto &entry : _routes)
    {
        if (entry.first.second == path)
            allowed.insert(entry.first.first);
    }
    if (static_file)
        allowed.insert("GET");
    if (allowed.count("GET") != 0)
        allowed.insert("HEAD");
    if (allowed.empty())
    {
        SetErrorBody(response, 404);
        return;
    }

    std::string allow;
    for (const auto &value : allowed)
    {
        if (!allow.empty())
            allow += ", ";
        allow += value;
    }
    SetErrorBody(response, 405);
    response.SetHeader("Allow", std::move(allow));
}

bool HttpServer::ResolveStaticFile(const std::string &path, std::filesystem::path &file) const
{
    if (_document_root.empty() || !Util::ResolveResourcePath(_document_root, path, file))
        return false;
    if (Util::IsDirectory(file))
    {
        std::string index_path = path;
        if (index_path.back() != '/')
            index_path += '/';
        index_path += "index.html";
        // index.html 也可能是指向站外的符号链接，不能直接拼磁盘路径读取。
        if (!Util::ResolveResourcePath(_document_root, index_path, file))
            return false;
    }
    return Util::IsRegularFile(file);
}

void HttpServer::ServeStaticFile(const std::filesystem::path &file, HttpResponse &response) const
{
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > kMaxStaticFile)
        throw std::runtime_error("static file unavailable or exceeds 8 MiB");

    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open static file");

    std::string body;
    body.reserve(static_cast<std::size_t>(size));
    char chunk[8192];
    for (;;)
    {
        input.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(input.gcount());
        // 除预检查外，实际读取时继续约束大小，避免文件增长绕过限制。
        if (count > kMaxStaticFile - body.size())
            throw std::runtime_error("static file exceeds 8 MiB while reading");
        body.append(chunk, count);
        if (input.bad() || (input.fail() && !input.eof()))
            throw std::runtime_error("cannot read static file");
        if (input.eof())
            break;
    }
    response.SetBody(std::move(body), std::string(Util::MimeType(file.string())));
}

void HttpServer::SendResponse(const ConnectionPtr &conn, const HttpRequest &request, HttpResponse &response)
{
    // 请求行解析失败时版本可能尚未就绪，错误响应默认使用 HTTP/1.1。
    response.SetVersion(request.GetVersion() == "HTTP/1.0" ? "HTTP/1.0" : "HTTP/1.1");
    // 复用需要双方都同意，关闭只需要一方提出。
    response.SetClose(!request.IsKeepAlive() || response.IsClose());
    if (!conn->Send(response.Serialize(request.GetMethod() == "HEAD")))
    {
        response.SetClose(true);
        return;
    }
    if (response.IsClose())
        conn->ShutDown();
}
