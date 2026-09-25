#include "protocol/HttpContext.hpp"
#include "base/Util.hpp"

#include <algorithm>
#include <limits>
#include <utility>

// 辅助函数与限制常量仅供本翻译单元使用，不暴露到其他模块。
namespace
{
// 请求行限制包含末尾 CRLF；头部限制包含每行 CRLF 和结束空行，不包含请求行。
// 正文限制针对实际字节数，接收前先检查声明长度，避免无限积累正文。
constexpr std::size_t kMaxRequestLine = 8 * 1024; // 请求行限定长度为8KB
constexpr std::size_t kMaxHeaders = 32 * 1024;    // 头字段限定长度为32KB (包含每行的 CRLF 和 结束的空行)
constexpr std::size_t kMaxBody = 8 * 1024 * 1024; // 请求正文限定长度为8MB

// 去掉字段值两端的空格和水平制表符，保留中间的空白。
// 只移动视图边界，不复制或修改底层字符串；返回值仍引用原存储。
std::string_view Trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}

} // namespace

//  核心功能: 找出一整行以拼写 CRLF 结尾 的扫描器
//  扫描上限（防 DoS）、严格 CRLF 校验（RFC 合规）、以及“未找到完整行时不推进读偏移”的重试契约。
//  从连接的输入缓冲区解析一个请求。数据不足时保留状态，下次从当前阶段续接。
//  请求行和头字段按行处理，正文按长度处理；完整请求以外的字节留给上层后续解析。
//  解析失败不回滚此前已保存的字段，但此时请求不能交给业务层使用。
HttpContext::ParseResult HttpContext::Parse(Buffer &buffer)
{
    // 一次调用可以跨越多个阶段；每轮都会消费数据、切换状态或返回结果。
    for (;;)
    {
        // 终态保持不变：完成后等待上层处理并 Reset，错误后等待上层关闭连接。
        // 重复调用不会消费缓冲区内属于下一个请求的数据。
        if (_state == State::Complete)
            return ParseResult::Complete;
        if (_state == State::Error)
            return ParseResult::Error;

        if (_state == State::Body)
        {
            // 只取当前正文还缺少的部分，不能把缓冲区剩余内容全部当作正文。
            // 已接收字节数直接使用 GetBodySize，避免维护重复计数。
            const auto count = std::min(_content_length - _request.GetBodySize(), buffer.GetReadableSize());

            if (count != 0)
            {
                // 明确指定长度，支持 NUL 和换行等二进制内容。
                // 先复制进请求再消费输入，避免追加失败时提前丢弃这批字节。
                _request.AppendBody(std::string_view(buffer.GetReadPosition(), count));
                buffer.MoveReadOffset(count);
            }

            // 正文未收齐时等待下一批数据；收齐则由循环顶部统一返回 Complete。
            if (_request.GetBodySize() != _content_length)
                return ParseResult::NeedMore;
            _state = State::Complete;
            continue;
        }

        // 请求行或头部阶段，两者共用 CRLF 扫描流程。
        const bool request_line = (_state == State::RequestLine);

        // 请求行单独限长，头部按整个字段区累计限长。
        const auto budget = request_line ? kMaxRequestLine : kMaxHeaders - _header_bytes;
        // 如果是请求行阶段过长返回错误码414 ; 如果是头部阶段返回错误码 431
        const int too_large = request_line ? 414 : 431;
        // 当前buffer缓冲区中的可读数据大小
        const auto readable = buffer.GetReadableSize();
        // 当前buffer缓冲区的读位置
        const char *data = buffer.GetReadPosition();

        // line_size为一行的长度
        // line_size为 0表示尚未找到完整行；找到后记录的长度包括结尾\r\n两个字节。
        std::size_t line_size = 0;

        // 仅扫描缓冲区中当前阶段允许的字节，不扫描后续正文或下一个请求。
        for (std::size_t i = 0; i < std::min(readable, budget); ++i)
        {
            // CR 后已有字节且不是 LF，可以立即报错。
            // CR 恰好位于缓冲区末尾时还不能判断，保留它等待下一次接收。
            if (data[i] == '\r' && i + 1 < readable && data[i + 1] != '\n')
            {
                Fail(400);
                return ParseResult::Error;
            }

            if (data[i] == '\n')
            {
                // 严格要求 CRLF
                // 开头的 LF 或 不带 CR 的 LF 都是非法行结束符。
                if (i == 0 || data[i - 1] != '\r')
                {
                    Fail(400);
                    return ParseResult::Error;
                }
                line_size = i + 1;
                break;
            }
        }

        // 当前buffer缓冲区还没有一行，不进行推进缓冲区
        if (line_size == 0)
        {
            // 可用额度已耗尽却仍没有完整行，再等待也不可能在限制内完成。
            // 使用 >=，因为额度本身已包含行结束符所需的字节。
            if (readable >= budget)
            {
                Fail(too_large);
                return ParseResult::Error;
            }
            // 未完成行不推进读偏移，后续数据到达后重新检查该行。
            return ParseResult::NeedMore;
        }

        // 运行到此，成功从buffer缓冲区中获取一行数据
        // 排除末尾 CRLF；视图借用 Buffer 内存，只在本轮处理期间有效。
        const std::string_view line(data, line_size - 2);

        // 分支一: 处理请求行
        // 分支二: 纯单独的CRLF,标志请求头处理完毕
        // 分支三: 处理请求头
        if (request_line)
        {
            // 请求行位置发了一个裸 CRLF，会被当作空的请求行传入ParseRequestLine
            // 并因找不到空格而被设定为错误码 400，而不是被误判成“头部结束”。
            if (!ParseRequestLine(line))
                return ParseResult::Error;
            // 请求行解析成功后，下一行开始按头字段解释。
            _state = State::Headers;
        }
        else if (line.empty())
        {
            // 单独的 CRLF 去掉结束符后为空，表示头部结束。
            // 必须等全部头字段收齐，才能统一校验正文边界。
            if (!FinishHeaders())
                return ParseResult::Error;
        }
        else if (!ParseHeaderLine(line))
        {
            return ParseResult::Error;
        }

        // 视图内容已复制到请求中，推进缓冲区后不再访问视图。
        buffer.MoveReadOffset(line_size);

        // 头部计数包含普通字段行和结束空行，不累计请求行。
        if (!request_line)
            _header_bytes += line_size;
    }
}

// 返回内部请求的只读引用，避免复制；完整请求仅在 Parse 返回 Complete 后可用。
// Reset 会清空引用所指对象的内容，上下文销毁后引用失效。
const HttpRequest &HttpContext::GetRequest() const
{
    return _request;
}

// 返回已记录的错误状态码，供上层构造响应；初始化或 Reset 后为 0。
int HttpContext::GetErrorStatus() const
{
    return _error_status;
}

// 为下一条请求恢复初始状态。Buffer 由 Connection 管理，此处不清空它，
// 因为其中可能已经有下一条请求；正常情况下由上层处理完完整请求后调用。
void HttpContext::Reset()
{
    _request.Reset();
    _state = State::RequestLine;
    _content_length = 0;
    _header_bytes = 0;
    _error_status = 0;
}

// 统一设置错误终态和 HTTP 状态码，返回 false 便于校验函数直接 return Fail(...)。
// 此处不发送响应、不关闭套接字，连接处理由上层负责。
bool HttpContext::Fail(int status)
{
    _state = State::Error;
    _error_status = status;
    return false;
}

// 传入的请求行格式为：method SP target SP version。
// 校验失败后进入错误终态；成功时保存数据，状态推进由 Parse 负责。
bool HttpContext::ParseRequestLine(std::string_view line)
{
    // 用前两个空格分出三部分；额外空格会落入目标或版本校验并被拒绝。
    // 查找第一个空格
    const auto first = line.find(' ');
    if (first == std::string_view::npos)
        return Fail(400);
    // 查找第二个空格
    const auto second = line.find(' ', first + 1);
    if (second == std::string_view::npos)
        return Fail(400);
    // 提取method
    const auto method = line.substr(0, first);
    // 提取target
    const auto target = line.substr(first + 1, second - first - 1);
    // 提取版本号
    const auto version = line.substr(second + 1);
    // 方法仅检查 token 字符，不在此决定业务是否支持某种方法。
    if (!Util::IsToken(method) || target.empty())
        return Fail(400);
    if (version != "HTTP/1.0" && version != "HTTP/1.1")
        return Fail(400);
    // 第一版接受普通站点路径及 OPTIONS *，不处理代理形式或 CONNECT 隧道目标。
    // 前面已排除空目标，此处访问 front() 安全。
    if (target.front() != '/' && !(method == "OPTIONS" && target == "*"))
        return Fail(400);
    // 原始目标拒绝空白、控制字符、非 ASCII 字节和片段标记 #。
    // 非 ASCII 内容可用百分号编码传入，再由下面的解码步骤处理。
    for (unsigned char ch : target)
    {
        if (ch <= 0x20 || ch >= 0x7f || ch == '#')
            return Fail(400);
    }

    // 先拆分再解码，避免把 %3F、%26、%3D 当作结构分隔符。
    const auto question = target.find('?');
    std::string path;
    // 没有问号时 substr(0, npos) 取整个目标；路径解码保留 '+' 的字面含义。
    if (!Util::UrlDecode(target.substr(0, question), path))
        return Fail(400);
    // 再检查解码结果，拒绝 %00、%0D 等编码后隐藏的控制字符。
    for (unsigned char ch : path)
    {
        if (ch < 0x20 || ch == 0x7f)
            return Fail(400);
    }
    // query 保留原始编码，供 GetQuery 返回；参数容器另存解码后的名称和值。
    const auto query = question == std::string_view::npos ? std::string_view{} : target.substr(question + 1);
    auto remaining = query;
    while (!remaining.empty())
    {
        // 逐项处理 a=1&b=2；连续或末尾的 & 形成的空项忽略。
        const auto amp = remaining.find('&');
        const auto item = remaining.substr(0, amp);
        if (!item.empty())
        {
            // 只按第一个等号拆分，值中后续的 '=' 原样保留。
            // 没有等号的 flag 按名称存在、值为空保存。
            const auto equal = item.find('=');
            std::string name;
            std::string value;
            // 查询参数采用表单式解码：百分号转义还原，'+' 转为空格。
            // 任一非法转义都会拒绝当前请求。
            if (!Util::UrlDecode(item.substr(0, equal), name, true) ||
                !Util::UrlDecode(equal == std::string_view::npos ? std::string_view{} : item.substr(equal + 1), value,
                                 true))
                return Fail(400);
            // AddParam 使用名称唯一的容器，同名参数保留首次出现的值。
            _request.AddParam(std::move(name), std::move(value));
        }
        if (amp == std::string_view::npos)
            break;
        // 跳过本项及 '&'，视图继续指向下一项，不复制剩余字符串。
        remaining.remove_prefix(amp + 1);
    }
    // 请求对象拥有独立字符串，函数返回后不再依赖 Buffer 中的视图。
    // 解码后的 path 可以转移所有权，借用 Buffer 的视图则需要复制。
    _request.SetMethod(std::string(method));
    _request.SetPath(std::move(path));
    _request.SetQuery(std::string(query));
    _request.SetVersion(std::string(version));
    return true;
}

// 解析不含 CRLF 的非空头字段行；结束空行由 Parse 单独识别。
bool HttpContext::ParseHeaderLine(std::string_view line)
{
    // 按第一个冒号拆分，字段值可以包含其他冒号。
    // token 校验同时拒绝空字段名、冒号前空白及以空白开头的折叠行。
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || !Util::IsToken(line.substr(0, colon)))
        return Fail(400);
    // 只去除值两端的空格和制表符，空字段值在语法层允许。
    const auto value = Trim(line.substr(colon + 1));
    for (unsigned char ch : value)
    {
        // 值内部允许水平制表符，拒绝 NUL、CR、LF 等其余控制字符与 DEL。
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f)
            return Fail(400);
    }
    // AddHeader 将名称转为小写并追加保存，保留重复字段及到达顺序。
    // 与字段含义相关的校验留到 FinishHeaders，不在这里合并重复字段。
    _request.AddHeader(std::string(line.substr(0, colon)), std::string(value));
    return true;
}

// 全部头字段收齐后确定正文长度，并转入 Body 或 Complete。
// 必须查询全部同名字段，不能只用返回第一条值的 GetHeaders。
bool HttpContext::FinishHeaders()
{
    // 本实现要求 HTTP/1.1 存在单个非空 Host；HTTP/1.0 可省略。
    // 两种版本都拒绝重复或空 Host；这里尚未校验完整主机名、端口语法。
    const auto hosts = _request.GetHeaderValues("host");
    if (hosts.size() > 1 || (!hosts.empty() && hosts.front().empty()) ||
        (_request.GetVersion() == "HTTP/1.1" && hosts.empty()))
        return Fail(400);

    const auto lengths = _request.GetHeaderValues("content-length");
    // 第一版不实现传输编码：单独出现返回 501；与长度字段同时出现返回 400。
    // 错误后由上层关闭连接，不能忽略编码字段继续猜测报文边界。
    if (_request.FindHeader("transfer-encoding"))
        return Fail(lengths.empty() ? 501 : 400);

    // 允许重复字段及逗号列表，如 "3, 003"，但所有项的数值必须相同。
    // 用额外标记区分“尚未读取长度”和“明确声明长度为 0”。
    bool seen_length = false;
    for (const auto &field : lengths)
    {
        std::string_view remaining(field);
        for (;;)
        {
            const auto comma = remaining.find(',');
            const auto item = Trim(remaining.substr(0, comma));
            // 空值、前后多余逗号、连续逗号产生的空项都视为非法长度。
            if (item.empty())
                return Fail(400);
            // 仅接受十进制数字，不接受正负号、小数点或数字中间的空白。
            std::size_t value = 0;
            for (unsigned char ch : item)
            {
                if (ch < '0' || ch > '9')
                    return Fail(400);
                const auto digit = static_cast<std::size_t>(ch - '0');
                // 在 value * 10 + digit 之前检查溢出，不能等无符号数回绕后再判断。
                if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
                    return Fail(400);
                value = value * 10 + digit;
            }
            // 跨同一字段的逗号列表和不同字段行比较，拒绝所有长度冲突。
            if (seen_length && value != _content_length)
                return Fail(400);
            _content_length = value;
            seen_length = true;
            if (comma == std::string_view::npos)
                break;
            // 继续下一项；若逗号位于结尾，下轮会通过空项检查报错。
            remaining.remove_prefix(comma + 1);
        }
    }
    // 数值格式与一致性检查完成后，再应用本服务器的正文大小限制。
    if (_content_length > kMaxBody)
        return Fail(413);
    // 第一版不支持 Expect；必须在等正文之前拒绝，否则 100-continue 客户端
    // 会等待服务器答复，服务器却在等待客户端正文。由 HttpServer 发送 417 并关闭。
    if (_request.FindHeader("expect"))
        return Fail(417);
    // 没有长度字段时保持初始值 0，与显式 Content-Length: 0 一样直接完成。
    // 有正文时仅设置目标长度，实际字节由 Parse 的 Body 分支分批接收。
    _state = _content_length == 0 ? State::Complete : State::Body;
    return true;
}
