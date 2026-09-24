#include "protocol/HttpResponse.hpp"
#include "base/Util.hpp"

#include <cstddef>
#include <utility>

namespace
{
// 把 ASCII 大写字母转为小写，其余字节原样返回。
char ToLowerAscii(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}
} // namespace

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
    _version = std::move(version);
}

void HttpResponse::SetClose(bool close)
{
    _close = close;
}

// 替换全部同名头，名称比较忽略大小写。
void HttpResponse::SetHeader(std::string name, std::string value)
{
    for (auto it = _headers.begin(); it != _headers.end();)
    {
        if (SameFieldName(it->first, name))
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

    // 调用方设置的头字段；Content-Length 与 Connection 由本类统一生成，避免两个来源
    for (const auto &field : _headers)
    {
        if (SameFieldName(field.first, "Content-Length") || SameFieldName(field.first, "Connection"))
        {
            continue;
        }
        result += field.first;
        result += ": ";
        result += field.second;
        result += "\r\n";
    }

    // 重定向目标
    if (_redirect_flag)
    {
        result += "Location: ";
        result += _redirect_url;
        result += "\r\n";
    }

    // 正文长度；禁止携带正文的状态码不输出该字段
    if (!forbid_body)
    {
        result += "Content-Length: ";
        result += std::to_string(_body.size());
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

// 两个字段名是否相同，按 ASCII 忽略大小写。
bool HttpResponse::SameFieldName(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i]))
        {
            return false;
        }
    }
    return true;
}

// 1xx、204、205 与 304 不允许携带正文。
bool HttpResponse::IsBodyForbidden(int status)
{
    return (status >= 100 && status < 200) || status == 204 || status == 205 || status == 304;
}
