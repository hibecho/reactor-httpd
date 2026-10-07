#include "protocol/HttpServer.hpp"
#include "base/Logger.hpp"
#include "base/Util.hpp"
#include "protocol/HttpContext.hpp"
#include "protocol/HttpResponder.hpp"
#include "reactor/EventLoop.hpp"
#include "thread/BusinessThreadPool.hpp"

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

    // 把路径按 '/' 切成段。开头的 '/' 不产生段；结尾的 '/' 也不产生空段。
    std::vector<std::string> SplitSegments(const std::string &path)
    {
        std::vector<std::string> segments;
        std::size_t start = (!path.empty() && path.front() == '/') ? 1 : 0;
        while (start <= path.size())
        {
            const std::size_t slash = path.find('/', start);
            const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
            segments.push_back(path.substr(start, end - start));
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        if (!segments.empty() && segments.back().empty())
            segments.pop_back();
        return segments;
    }

    // 把参数模式切成「字面段」与「参数名」。返回 false 表示模式非法。
    //
    // 参数名要求非空、是合法 token 且不重复：重名会让后注入的值覆盖先注入的，
    // 而调用方按名字取值时拿到的究竟是哪一个段就说不清了。
    bool BuildPattern(const std::string &pattern, std::vector<std::string> &literals, std::vector<std::string> &names)
    {
        const std::vector<std::string> segments = SplitSegments(pattern);
        if (segments.empty())
            return false;

        std::set<std::string> seen;
        for (const auto &segment : segments)
        {
            if (!segment.empty() && segment.front() == ':')
            {
                const std::string name = segment.substr(1);
                if (name.empty() || !Util::IsToken(name) || !seen.insert(name).second)
                    return false;
                literals.emplace_back();
                names.push_back(name);
            }
            else
            {
                literals.push_back(segment);
                names.emplace_back();
            }
        }
        return true;
    }

    // 每连接的协议状态。
    //
    // busy 表示这条连接上有一条异步/流式响应正在写。期间不能再解析后续请求——否则会在
    // 同一条连接上写出两份响应，报文边界就乱了。响应结束（或连接关闭）时清掉。
    //
    // 之所以把 context 一起装进来而不是各存一份：两者生命周期完全一致，分开存迟早会漂移。
    struct ConnState
    {
        HttpContext context;
        bool busy = false;
    };
} // namespace

HttpServer::HttpServer(uint16_t port, const std::string &ip)
    : _server(port, ip)
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

void HttpServer::SetBusinessThreadCount(int count)
{
    EnsureConfigurable();
    if (count < 0)
        throw std::invalid_argument("HttpServer: business thread count must not be negative");
    _business_threads = static_cast<std::size_t>(count);
}

void HttpServer::EnableInactiveRelease(int timeout)
{
    EnsureConfigurable();
    if (timeout < 1 || timeout > 60)
        throw std::invalid_argument("HttpServer: inactivity timeout must be 1..60 ticks");
    _server.EnableInactiveRelease(timeout);
}

// 两张路由表共用的注册前校验。handler_ok 由调用方判断（两种 handler 类型不同）。
static void ValidateRoute(const std::string &method, const std::string &path, bool handler_ok)
{
    if (!Util::IsToken(method) || !handler_ok || path.empty() ||
        (path.front() != '/' && !(method == "OPTIONS" && path == "*")))
        throw std::invalid_argument("HttpServer: invalid route");
    for (unsigned char ch : path)
    {
        if (ch < 0x20 || ch == 0x7f)
            throw std::invalid_argument("HttpServer: control character in route path");
    }
}

void HttpServer::AddRoute(std::string method, std::string path, Handler handler)
{
    EnsureConfigurable();
    ValidateRoute(method, path, static_cast<bool>(handler));
    // path 已解码，字面量 '?' 或 '#' 也可能来自 %3F/%23，不能再拆查询串。
    if (!_routes.emplace(RouteKey{std::move(method), std::move(path)}, std::move(handler)).second)
        throw std::invalid_argument("HttpServer: duplicate route");
}

void HttpServer::AddAsyncRoute(std::string method, std::string path, AsyncHandler handler)
{
    EnsureConfigurable();
    ValidateRoute(method, path, static_cast<bool>(handler));

    // 两张表不能重名：同名的方法+路径只允许有一个归属，否则命中哪张表取决于实现顺序
    const RouteKey key{std::move(method), std::move(path)};
    if (_routes.count(key) != 0 || _async_routes.count(key) != 0)
        throw std::invalid_argument("HttpServer: duplicate route");
    _async_routes.emplace(key, std::move(handler));
}

void HttpServer::AddParamRoute(std::string method, std::string pattern, Handler handler)
{
    EnsureConfigurable();
    ValidateRoute(method, pattern, static_cast<bool>(handler));

    ParamRoute route;
    route.method = std::move(method);
    route.handler = std::move(handler);
    route.is_async = false;
    if (!BuildPattern(pattern, route.literals, route.names))
        throw std::invalid_argument("HttpServer: invalid path parameter in route");
    _param_routes.push_back(std::move(route));
}

void HttpServer::AddParamAsyncRoute(std::string method, std::string pattern, AsyncHandler handler)
{
    EnsureConfigurable();
    ValidateRoute(method, pattern, static_cast<bool>(handler));

    ParamRoute route;
    route.method = std::move(method);
    route.async_handler = std::move(handler);
    route.is_async = true;
    if (!BuildPattern(pattern, route.literals, route.names))
        throw std::invalid_argument("HttpServer: invalid path parameter in route");
    _param_routes.push_back(std::move(route));
}

void HttpServer::SetNotFoundHandler(std::function<void(const HttpRequest &, HttpResponse &)> handler)
{
    EnsureConfigurable();
    _not_found_handler = std::move(handler);
}

void HttpServer::SetRequestHook(std::function<void(const HttpRequest &)> hook)
{
    EnsureConfigurable();
    _request_hook = std::move(hook);
}

const HttpServer::ParamRoute *HttpServer::MatchParamRoute(const std::string &method, const std::string &path,
                                                          HttpRequest *request) const
{
    const std::vector<std::string> segments = SplitSegments(path);
    for (const auto &route : _param_routes)
    {
        if (route.method != method || route.literals.size() != segments.size())
            continue;

        bool matched = true;
        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            if (!route.names[index].empty())
            {
                // 参数段不接受空值：/a//b 不该匹配 /a/:x/b
                if (segments[index].empty())
                {
                    matched = false;
                    break;
                }
                continue;
            }
            if (route.literals[index] != segments[index])
            {
                matched = false;
                break;
            }
        }
        if (!matched)
            continue;

        if (request != nullptr)
        {
            for (std::size_t index = 0; index < segments.size(); ++index)
            {
                if (!route.names[index].empty())
                    request->SetPathParam(route.names[index], segments[index]);
            }
        }
        return &route;
    }
    return nullptr;
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

    // 业务线程要赶在开始受理请求之前就位
    if (_business_threads > 0)
        _business = std::make_unique<BusinessThreadPool>(_business_threads);

    _server.Start();

    // Start 返回意味着所有连接已被强制关闭、业务线程手上的连接也都失效了
    // （IsWritable 早已转为 false，不会再写数据），这时候回收最干净。
    if (_business)
    {
        _business->Stop();
        _business.reset();
    }
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
    conn->SetContext(ConnState{});
}

void HttpServer::OnMessage(const ConnectionPtr &conn, Buffer *buffer)
{
    // 1.获取协议状态， 失败 → 抛 logic_error
    ConnState *state = std::any_cast<ConnState>(&conn->GetContext());
    if (state == nullptr)
        throw std::logic_error("HttpServer: missing HTTP context");
    HttpContext *context = &state->context;

    // 2. 每一轮，消化一个完整请求
    //
    // busy 时不再解析：连接上有一条流正在写，这时若处理下一个请求，两份响应会交织在
    // 同一条连接上。流结束后由 OnAsyncFinished 清掉 busy 并重新进入这里。
    while (!state->busy && conn->CanProcessInput())
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

        // 6.路由优先级：精确路由 → 参数路由 → 静态文件 → 404/405。
        //
        //   精确表里同步与异步不会重名（注册时已拒重复），所以先查异步表、再由 Dispatch
        //   查同步表，两者合起来就是「精确路由」这一档。
        if (_request_hook)
            _request_hook(request);

        const auto exact_async = _async_routes.find({request.GetMethod(), request.GetPath()});
        if (exact_async != _async_routes.end())
        {
            state->busy = true;
            RespondAsync(conn, request, exact_async->second, buffer);
            return;
        }

        // 精确路由命中时不再考虑参数路由，否则一条字面路径会被模式抢走
        const ParamRoute *param = nullptr;
        if (_routes.find({request.GetMethod(), request.GetPath()}) == _routes.end())
            param = MatchParamRoute(request.GetMethod(), request.GetPath(), &context->MutableRequest());

        // 参数路由的同步与异步同属一档，按注册顺序先到先得；命中异步的才走异步路径
        if (param != nullptr && param->is_async)
        {
            state->busy = true;
            RespondAsync(conn, request, param->async_handler, buffer);
            return;
        }

        // 7.构造正常响应
        HttpResponse response;

        // 8.设置响应结束不关闭连接
        response.SetClose(false);

        try
        {
            // 9.根据request进行查找路由表，构造响应向客户端发送
            Dispatch(request, response, param);
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

        // 10.序列化及 Send 不在业务异常捕获范围内，避免发送失败后重复生成响应。
        SendResponse(conn, request, response);
        if (response.IsClose())
            return;
        // 11.清空上下文
        context->Reset();
    }
}

void HttpServer::RespondAsync(const ConnectionPtr &conn, const HttpRequest &request, const AsyncHandler &handler,
                              Buffer *buffer)
{
    // 复用与否交给请求本身决定：HTTP/1.0 的请求不会要求 keep-alive，而 BeginStream 在
    // 分块不可用时也会把复用关掉（那种情况只能靠关闭连接界定正文）。
    const bool keep_alive = request.IsKeepAlive();
    auto responder =
        std::make_shared<HttpResponder>(conn, request.GetVersion(), request.GetMethod() == "HEAD", keep_alive);

    // 响应结束后回连接所属的循环线程复位状态、并续解析已经到达的后续请求。
    // 必须在循环线程上做——这里会被业务线程调用，跨线程直接碰连接状态是数据竞争。
    responder->SetFinishHandler([this, weak = std::weak_ptr<Connection>(conn), buffer]() {
        ConnectionPtr locked = weak.lock();
        if (!locked)
            return;
        locked->GetLoop()->RunInLoop([this, conn = std::move(locked), buffer]() {
            OnAsyncFinished(conn, buffer);
        });
    });

    // request 的引用指向解析上下文，那个上下文在本轮结束时就地复用了，所以必须拷贝一份
    // 交给业务线程长期持有。
    HttpRequest owned = request;

    auto task = [handler, owned = std::move(owned), responder]() mutable {
        auto fallback = [&responder](int status) {
            HttpResponse response(status);
            SetErrorBody(response, status);
            responder->Send(std::move(response));
        };

        try
        {
            handler(owned, responder);
        }
        catch (const std::exception &error)
        {
            // 业务线程上不能像同步路径那样把 bad_alloc 继续往上抛——那会一路逃出
            // 线程入口函数触发 std::terminate。这里统一兜成 500。
            LOG_ERROR("HTTP async handler failed: {}", error.what());
            fallback(500);
            return;
        }
        catch (...)
        {
            LOG_ERROR("HTTP async handler failed: unknown exception");
            fallback(500);
            return;
        }

        // handler 返回时还没产生任何响应：不能把连接就这么挂着，补一个 500
        if (!responder->Done())
        {
            LOG_ERROR("HTTP async handler produced no response");
            fallback(500);
        }
    };

    if (_business)
    {
        // 队列满时不阻塞事件循环去等业务线程——那等于把刚解决的问题原样搬回来
        if (!_business->Submit(std::move(task)))
        {
            HttpResponse busy(503);
            SetErrorBody(busy, 503);
            responder->Send(std::move(busy));
        }
        return;
    }

    // 没有业务线程：在事件循环线程上内联执行。只适合立刻返回的 handler，
    // 阻塞式 handler 会连带把自己写出的数据也堵在输出缓冲里发不出去。
    task();
}

void HttpServer::OnAsyncFinished(const ConnectionPtr &conn, Buffer *buffer)
{
    ConnState *state = std::any_cast<ConnState>(&conn->GetContext());
    if (state == nullptr)
        return;

    state->busy = false;
    state->context.Reset();

    // 流跑的时候客户端可能已经把下一个请求发过来了（流水线），也可能就躺在缓冲区里。
    // 这里补一次解析；没有残留数据时直接返回，等下一条请求的读事件即可。
    if (conn->CanProcessInput() && buffer != nullptr && buffer->GetReadableSize() > 0)
        OnMessage(conn, buffer);
}

void HttpServer::Dispatch(const HttpRequest &request, HttpResponse &response, const ParamRoute *param) const
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

    // 5.参数路由。调用方已经保证精确路由没命中，也按优先级筛过了；
    //   走到这里还带着异步的说明调用方漏判，落到兜底去。
    if (param != nullptr && param->handler)
    {
        param->handler(request, response);
        return;
    }

    // 6.这个路径其实是磁盘上的静态文件
    std::filesystem::path file;
    const bool static_file = ResolveStaticFile(path, file);
    if (static_file && (method == "GET" || method == "HEAD"))
    {
        ServeStaticFile(file, response);
        return;
    }

    // 7.没能命中时的归因
    // 判断这个路径到底存不存在：完全不存在回 404，存在但方法不对回 405
    std::set<std::string> allowed;
    for (const auto &entry : _routes)
    {
        if (entry.first.second == path)
            allowed.insert(entry.first.first);
    }
    // 异步路由也要计入：否则只注册在异步表里的路径会被归成 404 而不是 405
    for (const auto &entry : _async_routes)
    {
        if (entry.first.second == path)
            allowed.insert(entry.first.first);
    }
    // 参数路由同理，但只算那些模式能匹配上这条路径的
    for (const auto &entry : _param_routes)
    {
        if (MatchParamRoute(entry.method, path, nullptr) == &entry)
            allowed.insert(entry.method);
    }
    if (static_file)
        allowed.insert("GET");
    if (allowed.count("GET") != 0)
        allowed.insert("HEAD");
    if (allowed.empty())
    {
        // 路径未知：默认按状态码生成一句纯文本，设了钩子就交给它
        if (_not_found_handler)
        {
            _not_found_handler(request, response);
            return;
        }
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
