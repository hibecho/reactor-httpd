#include "protocol/HttpResponse.hpp"
#include "base/Util.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

// 构造响应对象，默认状态码为 200。
HttpResponse::HttpResponse(int status)
    : _status(status)
    , _redirect_flag(false)
{
}

void HttpResponse::SetStatus(int status)
{
    _status = status;
}

void HttpResponse::SetVersion(std::string version)
{
    // 版本号出现在状态行，含控制字符会拆出额外的头行。
    ValidateFieldValue(version);
    _version = std::move(version);
}

void HttpResponse::SetClose(bool close)
{
    _close = close;
}

// 替换全部同名头，名称比较忽略大小写。
void HttpResponse::SetHeader(std::string name, std::string value)
{
    // 先校验再改动容器：被拒绝的字段不得留下任何痕迹。
    ValidateFieldName(name);
    ValidateFieldValue(value);

    for (auto it = _headers.begin(); it != _headers.end();)
    {
        if (Util::EqualsIgnoreCaseAscii(it->first, name))
        {
            it = _headers.erase(it);
        }
        else
        {
            ++it;
        }
    }
    _headers.emplace_back(std::move(name), std::move(value));
}

// 追加一条头字段，支持多条 Set-Cookie。
void HttpResponse::AddHeader(std::string name, std::string value)
{
    ValidateFieldName(name);
    ValidateFieldValue(value);
    _headers.emplace_back(std::move(name), std::move(value));
}

// 设置正文，同时设置 Content-Type。
void HttpResponse::SetBody(std::string body, std::string content_type)
{
    _body = std::move(body);
    SetHeader("Content-Type", std::move(content_type));
}

// 设置重定向状态码与 Location；status 默认 302 Found。
void HttpResponse::SetRedirect(std::string location, int status)
{
    ValidateUri(location);
    _status = status;
    _redirect_flag = true;
    _redirect_url = std::move(location);
}

int HttpResponse::GetStatus() const
{
    return _status;
}

bool HttpResponse::IsClose() const
{
    return _close;
}

const std::string &HttpResponse::GetBody() const
{
    return _body;
}

bool HttpResponse::IsRedirect() const
{
    return _redirect_flag;
}

// 生成完整报文；HEAD 请求仅输出状态行和响应头。
std::string HttpResponse::Serialize(bool head_request) const
{
    const bool forbid_body = IsBodyForbidden(_status);

    std::string result;
    result.reserve(_body.size() + 256);

    // 响应行: HTTP 版本 + 状态码 + 状态描述
    result += _version;
    result += ' ';
    result += std::to_string(_status);
    result += ' ';
    result += Util::StatusDescription(_status);
    result += "\r\n";

    // 调用方设置的头字段；Content-Length 与 Connection 由本类统一生成，避免两个来源。
    AppendUserFields(result, false);

    // 重定向目标
    if (_redirect_flag)
    {
        result += "Location: ";
        result += _redirect_url;
        result += "\r\n";
    }

    // 正文长度。1xx 与 204 按 RFC 9110 §8.6 禁止携带该字段，304 也选择省略；
    // 205 不允许正文，但必须显式声明零长度——否则在 keep-alive 连接上客户端会一直等待正文。
    if (!OmitsContentLength(_status))
    {
        result += "Content-Length: ";
        result += std::to_string(forbid_body ? std::size_t{0} : _body.size());
        result += "\r\n";
    }

    // 连接复用意图
    result += "Connection: ";
    result += (_close ? "close" : "keep-alive");
    result += "\r\n";

    // 响应头与正文之间以空行分隔
    result += "\r\n";

    // HEAD 请求不发送正文，但保留与 GET 一致的 Content-Length
    if (!head_request && !forbid_body)
    {
        result += _body;
    }

    return result;
}

// 输出调用方设置的头字段，跳过由本类统一生成的字段。
//
// streaming 为 true 时额外跳过 Transfer-Encoding：流式响应必须由本类决定这一字段
// （有就是 chunked，没有就是 close-delimited），调用方自己设一个会让两者打架。
void HttpResponse::AppendUserFields(std::string &out, bool streaming) const
{
    // 重定向时 Location 由 _redirect_url 提供，同样跳过调用方的同名头；不重定向则照常输出。
    for (const auto &field : _headers)
    {
        if (Util::EqualsIgnoreCaseAscii(field.first, "Content-Length") ||
            Util::EqualsIgnoreCaseAscii(field.first, "Connection") ||
            (streaming && Util::EqualsIgnoreCaseAscii(field.first, "Transfer-Encoding")) ||
            (_redirect_flag && Util::EqualsIgnoreCaseAscii(field.first, "Location")))
        {
            continue;
        }
        out += field.first;
        out += ": ";
        out += field.second;
        out += "\r\n";
    }
}

// 只生成状态行与响应头，供流式响应先发出去。
std::string HttpResponse::SerializeHead(bool chunked) const
{
    std::string result;
    result.reserve(256);

    // 响应行: HTTP 版本 + 状态码 + 状态描述
    result += _version;
    result += ' ';
    result += std::to_string(_status);
    result += ' ';
    result += Util::StatusDescription(_status);
    result += "\r\n";

    AppendUserFields(result, true);

    // 重定向目标
    if (_redirect_flag)
    {
        result += "Location: ";
        result += _redirect_url;
        result += "\r\n";
    }

    // 正文长度不在此时可知，因此两种情况下都不输出 Content-Length：
    //   chunked   —— 由分块编码自行界定
    //   非 chunked —— HTTP/1.0 的 close-delimited，靠关闭连接界定
    // 状态码禁止携带正文时同样不该出现这两个字段。
    if (chunked && !IsBodyForbidden(_status))
    {
        result += "Transfer-Encoding: chunked\r\n";
    }

    // 连接复用意图
    result += "Connection: ";
    result += (_close ? "close" : "keep-alive");
    result += "\r\n";

    // 响应头与正文之间以空行分隔
    result += "\r\n";

    return result;
}

// 1xx、204、205 与 304 不允许携带正文。
bool HttpResponse::IsBodyForbidden(int status)
{
    return (status >= 100 && status < 200) || status == 204 || status == 205 || status == 304;
}

// 是否完全不输出 Content-Length。205 不在其中：它只要声明长度为 0，不需要真给正文。
bool HttpResponse::OmitsContentLength(int status)
{
    return (status >= 100 && status < 200) || status == 204 || status == 304;
}

// 字段名必须是 token：空名或含冒号、空格、CR/LF 的名字都会破坏报文结构。
void HttpResponse::ValidateFieldName(std::string_view name)
{
    if (!Util::IsToken(name))
        throw std::invalid_argument("HttpResponse: header field name must be a non-empty RFC 9110 token");
}

// 字段值允许可见字符与水平制表符；其余 C0 控制字符与 DEL 会拆出额外的头行或被下游拒绝。
void HttpResponse::ValidateFieldValue(std::string_view value)
{
    for (unsigned char ch : value)
    {
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f)
            throw std::invalid_argument("HttpResponse: header field value contains a control character");
    }
}

// 重定向目标是 URI 引用：空格、C0 控制字符与 DEL 都不合法。
// 非 ASCII 字节不在本层校验，因为本类只负责报文边界，URI 语义由调用方保证。
void HttpResponse::ValidateUri(std::string_view value)
{
    for (unsigned char ch : value)
    {
        if (ch <= 0x20 || ch == 0x7f)
            throw std::invalid_argument("HttpResponse: redirect target is not a valid URI reference");
    }
}
