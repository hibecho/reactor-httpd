/**
 * @file HttpStreamWriter.hpp
 * @brief 流式响应写入器：先发响应头，之后按块持续追加正文。
 *
 * 面向 SSE 这类「响应头立刻发出、内容随时间陆续到达」的场景。普通响应一次性
 * 交给 HttpResponse 序列化即可，不需要本类。
 *
 * 分块编码（chunked）下一块正文写成 `<十六进制长度>\r\n<数据>\r\n`，结束时补一个
 * 零长度块 `0\r\n\r\n` 作为终止符。HTTP/1.0 不支持分块，此时正文原样写出，由关闭
 * 连接界定结束——所以那种情况下 keep_alive 必须为 false。
 */
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Connection;
class HttpResponse;

class HttpStreamWriter : public std::enable_shared_from_this<HttpStreamWriter>
{
  public:
    // conn 用弱引用：写入期间连接随时可能被关闭，而强引用会让本对象成为最后一个
    // owner，导致连接在业务线程上析构——连接必须在自己的事件循环线程里析构。
    HttpStreamWriter(std::weak_ptr<Connection> conn, bool chunked, bool keep_alive);

    HttpStreamWriter(const HttpStreamWriter &) = delete;
    HttpStreamWriter &operator=(const HttpStreamWriter &) = delete;

    // 发出响应头。幂等，重复调用只在第一次生效。
    // 连接已不可写时返回 false，调用方应就此收手。
    bool Begin(const HttpResponse &head);

    // 追加一块正文。data 为空时是 no-op 并返回 true——零长度块是分块编码的终止符，
    // 误发会让客户端以为响应到此结束。
    //
    // 返回 false 表示连接已经不可写（对端关闭、或被强制关闭），继续写没有意义。
    bool Write(std::string_view data);

    // 结束响应：分块编码下补终止块；keep_alive 为 false 时关闭连接。幂等。
    bool Finish();

    // 结束回调，Finish() 生效的那一次触发一次。协议层用它复位连接状态、续解析同一
    // 连接上已经到达的后续请求。回调在调用 Finish 的线程上执行。
    void SetFinishHandler(std::function<void()> handler) { _on_finish = std::move(handler); }

    // 还能不能继续写。任一次写失败之后恒为 false。
    bool IsWritable() const noexcept;
    bool Started() const noexcept;
    bool Finished() const noexcept;

  private:
    std::weak_ptr<Connection> _conn;
    bool _chunked;
    bool _keep_alive;
    bool _started = false;
    bool _finished = false;
    bool _writable = true;
    std::function<void()> _on_finish;
};
