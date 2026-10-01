/**
 * @file HttpResponder.hpp
 * @brief 异步/流式 handler 的响应入口。
 *
 * 同步 handler 拿到的是一份 HttpResponse，填完由 HttpServer 负责序列化与发送；
 * 异步 handler 需要在回调返回之后才决定响应（甚至要边算边发），所以改成拿到本对象，
 * 由它自己把内容写到连接上。
 *
 * 两种用法二选一，且只能用一次：
 *   - Send(response)        一次性响应，适合普通的 JSON 接口
 *   - BeginStream(head)     流式响应，拿到 HttpStreamWriter 后持续追加正文
 *
 * 连接用弱引用持有：异步执行期间连接可能已经被关闭，强引用会让本对象成为最后一个
 * owner，导致连接在业务线程上析构。
 */
#pragma once
#include <memory>
#include <string>

#include "protocol/HttpResponse.hpp"

class Connection;
class HttpStreamWriter;

class HttpResponder : public std::enable_shared_from_this<HttpResponder>
{
  public:
    // keep_alive 决定响应发完后是否复用连接，由 HttpServer 按请求与当前能力决定。
    // version 用于选择传输方式：HTTP/1.1 可分块，HTTP/1.0 只能靠关闭连接界定正文。
    HttpResponder(std::weak_ptr<Connection> conn, std::string version, bool head_request, bool keep_alive);

    HttpResponder(const HttpResponder &) = delete;
    HttpResponder &operator=(const HttpResponder &) = delete;

    // 一次性发送完整响应。已经用过（Send 或 BeginStream）时返回 false。
    bool Send(HttpResponse response);

    // 开始流式响应：先发出响应头，返回写出正文用的 writer。
    // 已经用过或连接不可写时返回 nullptr。
    std::shared_ptr<HttpStreamWriter> BeginStream(HttpResponse head);

    // 连接是否还可写。注意它只表示「还没失败过」，不主动探测对端。
    bool IsWritable() const noexcept;

    // 是否已经产生过响应（Send 或 BeginStream）。
    bool Done() const noexcept { return _done; }

  private:
    std::weak_ptr<Connection> _conn;
    std::string _version;
    bool _head_request;
    bool _keep_alive;
    bool _done = false;
};
