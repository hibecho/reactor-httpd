/**
 * @file HttpServer.hpp
 * @brief HTTP 请求调度层：组合 TcpServer，连接解析、路由和响应发送。
 *
 * 每条 Connection 独立保存 HttpContext。一次读取可包含半个或多个请求，
 * 按顺序解析、同步调用业务并发送响应；连接关闭后不再处理后续请求。
 * 支持 HTTP/1.0、HTTP/1.1 定长报文；不支持 Expect、分块传输及协议升级。
 */
#pragma once

#include "protocol/HttpRequest.hpp"
#include "protocol/HttpResponse.hpp"
#include "tcp/TcpServer.hpp"
#include "thread/BusinessThreadPool.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class HttpResponder;

class HttpServer
{
  public:
    // 同步处理一条完整请求；请求引用不得在回调返回后继续使用。
    // 不同连接可并发调用同一 handler，共享业务数据由调用者同步。
    // handler 填写最终响应，不执行耗时任务，也不设置未经校验的响应头值。
    using Handler = std::function<void(const HttpRequest &, HttpResponse &)>;

    // 异步/流式 handler：拿到 responder 后自行把响应写到连接上。
    //
    // 与同步 handler 的区别在于「谁负责发送」：同步的填完 HttpResponse 交给框架，
    // 异步的自己发——因为流式响应要在回调返回之后继续写。request 是副本，可以长期持有。
    using AsyncHandler = std::function<void(const HttpRequest &, const std::shared_ptr<HttpResponder> &)>;

    /*构造函数*/
    explicit HttpServer(uint16_t port);
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // 1.添加路由信息
    //  - method 按大小写精确匹配；path 为已解码路径，不含查询字符串。
    //  - HEAD 无专用路由时回退到 GET；支持显式注册 OPTIONS *。
    //  - 空回调、非法方法/路径或重复注册均抛 invalid_argument。
    void AddRoute(std::string method, std::string path, Handler handler);

    // 注册异步/流式路由。匹配规则、校验与 AddRoute 相同，两张表不重名。
    //
    // 命中异步路由时不再走 SendResponse：响应的序列化与发送全部由 responder 完成，
    // handler 必须在返回前至少调用一次 Send 或 BeginStream，否则框架补一个 500。
    void AddAsyncRoute(std::string method, std::string path, AsyncHandler handler);

    // 带路径参数的路由：模式里以 ':' 开头的段是参数名。
    //   /api/session/:id/history  匹配 /api/session/abc/history，注入 id="abc"
    //
    // 刻意不走 AddRoute：':' 在 URI 路径里是合法字面字符，把两者合并成一套，
    // 会让一条本意是字面量的路径悄悄变成模式。
    //
    // 匹配优先级低于精确路由——先查精确表，都没命中才试这些。
    void AddParamRoute(std::string method, std::string pattern, Handler handler);
    void AddParamAsyncRoute(std::string method, std::string pattern, AsyncHandler handler);

    // 路径未知时的兜底响应。
    // 默认按状态码生成一句纯文本；设了钩子就交给它，由它决定状态码与正文
    // （例如 /api 下统一回 JSON 信封）。钩子不设时行为与原来完全一致。
    void SetNotFoundHandler(std::function<void(const HttpRequest &, HttpResponse &)> handler);

    // 每解析出一个完整请求、在路由之前调用一次。用于访问日志这类旁路记录；
    // 它不该改变响应，因此拿不到可写的响应对象。
    void SetRequestHook(std::function<void(const HttpRequest &)> hook);

    // 2.设置静态目录
    //  - 可选静态站点目录，必须存在；GET/HEAD 使用，动态路由优先。
    //  - 目录读取 index.html，不列目录；普通文件上限 8 MiB，超限返回 500 并关闭。
    //  - 文件树及符号链接须稳定：现有路径工具不防止检查后的并发替换。
    void SetDocumentRoot(std::filesystem::path root);

    // 3.设置线程数量
    // - 以下配置及 Start 必须在构造线程调用，启动后不可修改或重复启动。
    void SetThreadCount(int count);

    // 3b.设置业务线程数量：异步路由的 handler 在业务线程上执行。
    //
    // 为 0（默认）时异步 handler 在事件循环线程上内联执行——只适合「立刻返回」的
    // handler。**会阻塞的 handler 必须配业务线程**：事件循环被占住时，连 handler
    // 自己刚写出的数据都发不出去（send 要等循环回到 epoll_wait 才发生），
    // 流式响应会因此退化成「攒完一次性发」。
    void SetBusinessThreadCount(int count);

    // 4.设置超时连接销毁
    // - [1,60] 个 tick，沿用底层时间轮；超范围显式拒绝，避免隐式钳位。
    void EnableInactiveRelease(int timeout);

    // 5.启动运行
    // - 阻塞运行；返回时停止清理和线程回收完成，运行中不得析构。
    void Start();
    void Stop();
    void SetResourceLimits(const ResourceLimits &limits);
    void SetShutdownGrace(std::chrono::milliseconds grace);
    void EnableSignalStop();
    uint16_t GetPort() const noexcept { return _server.GetPort(); }

  private:
    // 参数路由。handler 与 async_handler 二选一，is_async 指明用了哪个。
    // 声明在私有方法之前：下面的声明要用到它。
    struct ParamRoute
    {
        std::string method;
        std::vector<std::string> literals; // 每段的字面值；参数段为空串
        std::vector<std::string> names;    // 每段的参数名；字面段为空串
        Handler handler;
        AsyncHandler async_handler;
        bool is_async = false;
    };

    void EnsureConfigurable() const;

    // 1.设置协议上下文
    void OnConnected(const ConnectionPtr &conn);
    // 2.用于进行缓冲区的数据处理
    void OnMessage(const ConnectionPtr &conn, Buffer *buffer);
    // 决策：产出响应【不碰网络】
    // 判断该由谁来产生响应：注册的 handler、磁盘上的静态文件，还是直接生成 404/405。
    void Dispatch(const HttpRequest &request, HttpResponse &response, const ParamRoute *param) const;
    // 判定：这路径是不是文件
    bool ResolveStaticFile(const std::string &path, std::filesystem::path &file) const;
    // 读取：文件内容填进响应
    void ServeStaticFile(const std::filesystem::path &file, HttpResponse &response) const;
    // 收尾：定版本/连接头/发送/关闭【碰网络】
    void SendResponse(const ConnectionPtr &conn, const HttpRequest &request, HttpResponse &response);
    // 异步/流式路由的执行入口：建 responder、调 handler、兜底补响应【碰网络】
    void RespondAsync(const ConnectionPtr &conn, const HttpRequest &request, const AsyncHandler &handler,
                      Buffer *buffer);
    // 一条异步/流式响应结束之后，回循环线程复位状态并续解析残留请求
    void OnAsyncFinished(const ConnectionPtr &conn, Buffer *buffer);
    // 在参数路由里找匹配项；命中返回该条目，否则返回 nullptr。
    // request 非空时把路径参数写进去；只想知道「模式能不能匹配这条路径」就传 nullptr。
    const ParamRoute *MatchParamRoute(const std::string &method, const std::string &path, HttpRequest *request) const;

  private:
    using RouteKey = std::pair<std::string, std::string>;
    const std::thread::id _owner_thread = std::this_thread::get_id();

    // GET  /             →  HandlerA: 返回首页
    // GET  /users        →  HandlerB: 列出用户
    // POST /users        -> HandlerC: 创建用户
    std::map<RouteKey, Handler> _routes; // 路由表

    // 异步/流式路由单独一张表：同步那套的行为（尤其是序列化与发送时机）不能受影响
    std::map<RouteKey, AsyncHandler> _async_routes;

    std::vector<ParamRoute> _param_routes;

    std::function<void(const HttpRequest &, HttpResponse &)> _not_found_handler;
    std::function<void(const HttpRequest &)> _request_hook;

    // 业务线程池。0 表示不用池、在事件循环线程上内联执行。
    // 只在 Start 期间存在：Start 返回即代表所有连接已收尾，此时回收最干净。
    std::size_t _business_threads = 0;
    std::unique_ptr<BusinessThreadPool> _business;

    std::filesystem::path _document_root;
    bool _started = false;
    // 最后声明，析构时先销毁底层服务器；运行期间本对象必须一直存活。
    TcpServer _server;
};