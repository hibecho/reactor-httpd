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
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>

class HttpServer
{
  public:
    // 同步处理一条完整请求；请求引用不得在回调返回后继续使用。
    // 不同连接可并发调用同一 handler，共享业务数据由调用者同步。
    // handler 填写最终响应，不执行耗时任务，也不设置未经校验的响应头值。
    using Handler = std::function<void(const HttpRequest &, HttpResponse &)>;

    /*构造函数*/
    explicit HttpServer(uint16_t port);
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // 1.添加路由信息
    //  - method 按大小写精确匹配；path 为已解码路径，不含查询字符串。
    //  - HEAD 无专用路由时回退到 GET；支持显式注册 OPTIONS *。
    //  - 空回调、非法方法/路径或重复注册均抛 invalid_argument。
    void AddRoute(std::string method, std::string path, Handler handler);

    // 2.设置静态目录
    //  - 可选静态站点目录，必须存在；GET/HEAD 使用，动态路由优先。
    //  - 目录读取 index.html，不列目录；普通文件上限 8 MiB，超限返回 500 并关闭。
    //  - 文件树及符号链接须稳定：现有路径工具不防止检查后的并发替换。
    void SetDocumentRoot(std::filesystem::path root);

    // 3.设置线程数量
    // - 以下配置及 Start 必须在构造线程调用，启动后不可修改或重复启动。
    void SetThreadCount(int count);

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
    void EnsureConfigurable() const;

    // 1.设置协议上下文
    void OnConnected(const ConnectionPtr &conn);
    // 2.用于进行缓冲区的数据处理
    void OnMessage(const ConnectionPtr &conn, Buffer *buffer);
    // 决策：产出响应【不碰网络】
    // 判断该由谁来产生响应：注册的 handler、磁盘上的静态文件，还是直接生成 404/405。
    void Dispatch(const HttpRequest &request, HttpResponse &response) const;
    // 判定：这路径是不是文件
    bool ResolveStaticFile(const std::string &path, std::filesystem::path &file) const;
    // 读取：文件内容填进响应
    void ServeStaticFile(const std::filesystem::path &file, HttpResponse &response) const;
    // 收尾：定版本/连接头/发送/关闭【碰网络】
    void SendResponse(const ConnectionPtr &conn, const HttpRequest &request, HttpResponse &response);

  private:
    using RouteKey = std::pair<std::string, std::string>;
    const std::thread::id _owner_thread = std::this_thread::get_id();

    // GET  /             →  HandlerA: 返回首页
    // GET  /users        →  HandlerB: 列出用户
    // POST /users        -> HandlerC: 创建用户
    std::map<RouteKey, Handler> _routes; // 路由表

    std::filesystem::path _document_root;
    bool _started = false;
    // 最后声明，析构时先销毁底层服务器；运行期间本对象必须一直存活。
    TcpServer _server;
};