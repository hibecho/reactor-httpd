/**
 * @file HttpContext.hpp
 *
 * @brief HTTP 请求解析上下文，保存单条连接上的请求数据与解析进度。
 *
 * 设计目的：
 * - TCP 接收的数据可能只是一个请求的片段，也可能包含多个请求。
 * - 每条连接独立持有一个上下文，使解析能在数据不足时暂停，后续继续。
 *
 * 核心职责：
 * 1. 从连接的输入 Buffer 中逐步解析数据，将结果保存到 HttpRequest。
 * 2. 按"请求行 -> 请求头 -> 正文 -> 完成"推进状态，发现非法数据时进入错误状态。
 * 3. 区分数据不足、请求完成和解析错误，交由上层决定后续处理。
 * 4. 记录预期正文长度并检查报文大小限制，防止越界消费和无限积累数据。
 *
 * 处理原则：
 * - 未收完整的行留在 Buffer 中；已解析的字段和正文保存到 HttpRequest。
 * - 一次最多完成一个请求，后续请求的字节保留在 Buffer 中。
 * - 上层处理完请求后重置上下文；重置只清空请求数据与解析进度，不清空 Buffer。
 * - 本模块负责解析；网络收发、业务处理与响应发送由 Connection 和 HttpServer 负责。
 *
 */

#pragma once
#include "base/Buffer.hpp"
#include "protocol/HttpRequest.hpp"
#include <cstddef>
#include <string_view>

class HttpContext
{
    // 支持 HTTP/1.0、HTTP/1.1 定长正文；暂不支持分块传输和 Expect（头部完成即 417）。
    // 请求目标支持 /path?query 和 OPTIONS *；路径解码一次，查询按表单规则解码。
    // 请求行含 CRLF 上限 8 KiB，头部含末尾空行上限 32 KiB，正文上限 8 MiB。
  public:
    // 单次解析的结果；数据不足属于正常接收过程，不等同于报文错误。
    enum class ParseResult
    {
        NeedMore, // 当前请求尚未完整，保留进度，等待后续数据。
        Complete, // 当前请求已完整，可交给业务层处理。
        Error     // 报文非法或超出限制，由上层处理错误并关闭连接。
    };

  public:
    // 尽可能向前解析，但一次最多完成一个请求。
    // buffer 为连接的输入缓冲区；只消费属于当前请求且已处理的字节。
    // 返回 NeedMore 时保留请求数据与状态，下次传入同一连接的缓冲区继续解析。
    // 返回 Complete 后保留请求供上层读取，处理完毕后调用 Reset 再解析下一条。
    // 返回 Error 后通过 GetErrorStatus 获取错误状态码，不再尝试解析后续请求。
    ParseResult Parse(Buffer &buffer);

    // 业务层仅在 Complete 后使用完整请求。
    // 返回内部对象的只读引用；Reset 会清空其内容，上下文销毁后引用失效。
    // 异步业务若需在重置后继续使用请求，应先复制请求数据。
    const HttpRequest &GetRequest() const;

    // 可在解析完成后写入的属性（目前只有路由匹配注入的路径参数）。
    // 单独开一个可写入口，而不是把 GetRequest 改成非 const：解析器只负责产出请求，
    // 「路由给请求贴标签」是上层的事，让解析结果默认只读能挡住越权改写。
    HttpRequest &MutableRequest();

    // 获取解析错误对应的 HTTP 状态码，仅在返回 Error 后有意义。
    // 例如 400 表示报文非法，413 表示正文过大，414 表示请求目标过长，431 表示头部过大。
    // 本接口只提供状态码，错误响应的构造与发送由上层负责。
    int GetErrorStatus() const;

    // 清空当前请求及解析状态，不清空连接的 Buffer。
    // 恢复到 RequestLine 状态，同时清零正文长度、头部累计大小和错误状态码。
    void Reset();

  private:
    // 跨多次 Parse 调用保存的解析阶段，与单次调用的 ParseResult 分开表达。
    enum class State
    {
        RequestLine, // 等待完整请求行，解析方法、请求目标与 HTTP 版本。
        Headers,     // 逐行解析头字段，遇到空行后确定正文边界。
        Body,        // 按预期长度追加正文，不按换行符分割。
        Complete,    // 当前请求已完成，等待上层处理并重置。
        Error        // 当前请求解析失败，停止继续消费报文。
    };

    // 解析一条完整请求行，将方法、路径、查询字符串与版本保存到 _request。
    // line 不包含末尾 CRLF；视图仅在本次调用中使用，不保存其底层指针。
    // 成功返回 true；格式非法时返回 false，并记录错误状态与状态码。
    bool ParseRequestLine(std::string_view line);

    // 解析一条非空头字段行，校验字段格式后追加到 _request。
    // line 不包含末尾 CRLF；头部结束的空行由 Parse 单独处理。
    // 成功返回 true；格式非法时返回 false，并记录错误状态与状态码。
    bool ParseHeaderLine(std::string_view line);

    // 收到头部结束空行后，统一校验头字段并确定预期正文长度。
    // 校验长度时应检查全部同名字段，不能只读取第一个 Content-Length。
    // 成功后进入 Body 或 Complete；失败时记录错误状态与状态码并返回 false。
    bool FinishHeaders();

    // 统一记录解析失败，返回 false 供内部校验函数直接返回。
    bool Fail(int status);

  private:
    State _state = State::RequestLine; // 当前解析阶段，初始从请求行开始。
    HttpRequest _request;              // 正在构建的请求；完成后交给上层只读访问。

    // 头部校验后确定的正文总长度；已接收长度由 _request.GetBodySize() 获取。
    std::size_t _content_length = 0;
    // 已消费的头部字节数，含各行 CRLF 与末尾空行，不含请求行。
    std::size_t _header_bytes = 0;
    // 解析错误对应的 HTTP 状态码，0 表示尚未记录错误。
    int _error_status = 0;
};
