#include "protocol/HttpRequest.hpp"

namespace
{
// HTTP 字段采用 ASCII 大小写规则，不依赖系统区域设置。
std::string AsciiLower(std::string value)
{
    for (char &ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}
} // namespace

// 请求行及正文。
const std::string &HttpRequest::GetMethod() const
{
    return _method;
}
const std::string &HttpRequest::GetPath() const
{
    return _path;
}
const std::string &HttpRequest::GetQuery() const
{
    return _query;
}
const std::string &HttpRequest::GetVersion() const
{
    return _version;
}
const std::string &HttpRequest::GetBody() const
{
    return _body;
}

std::size_t HttpRequest::GetBodySize() const
{
    return _body.size();
}

// 查询请求头是否存在，名称忽略 ASCII 大小写。
bool HttpRequest::FindHeader(const std::string &name) const
{
    auto it = _headers.find(AsciiLower(name));
    if (it == _headers.end())
    {
        return false;
    }
    return true;
}

bool HttpRequest::FindParam(const std::string &name) const
{
    auto it = _params.find(name);
    if (it == _params.end())
    {
        return false;
    }
    return true;
}

// 获取指定的头部
std::string HttpRequest::GetHeaders(const std::string &key) const
{
    auto it = _headers.find(AsciiLower(key));
    if (it == _headers.end())
    {
        return "";
    }
    return it->second;
}

std::string HttpRequest::GetParams(const std::string &key) const
{
    auto it = _params.find(key);
    if (it == _params.end())
    {
        return "";
    }
    return it->second;
}

// 构建请求时使用。
void HttpRequest::SetMethod(std::string method)
{
    _method = method;
}
void HttpRequest::SetPath(std::string path)
{
    _path = path;
}
void HttpRequest::SetQuery(std::string query)
{
    _query = query;
}
void HttpRequest::SetVersion(std::string version)
{
    _version = version;
}

void HttpRequest::AddHeader(std::string name, std::string value)
{
    name = AsciiLower(name);
    if (name == "connection")
    {
        auto it = _headers.find(name);
        if (it != _headers.end())
        {
            it->second += ", ";
            it->second += value;
            return;
        }
    }
    _headers.emplace(name, value);
}

void HttpRequest::AddParam(std::string name, std::string value)
{
    _params.emplace(name, value);
}

void HttpRequest::AppendBody(std::string_view data)
{
    if (data.empty())
        return;
    _body.append(data.data(), data.size());
}

bool HttpRequest::HasConnectionToken(std::string_view token) const
{
    // 1.找到 Connection 请求头
    const auto it = _headers.find("connection");
    if (it == _headers.end())
        return false;
    // 2.，字段名比较忽略 ASCII 大小写。
    const std::string value = AsciiLower(it->second);
    std::string_view remaining(value);

    // 3.按逗号拆分字段值
    while (!remaining.empty())
    {
        const auto comma = remaining.find(',');
        auto item = remaining.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.remove_suffix(1);
        if (item == token)
            return true;
        if (comma == std::string_view::npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

bool HttpRequest::IsKeepAlive() const
{
    if (HasConnectionToken("close"))
        return false;
    if (_version == "HTTP/1.1")
        return true;
    if (_version == "HTTP/1.0")
        return HasConnectionToken("keep-alive");
    return false;
}

void HttpRequest::Reset()
{
    _method.clear();
    _path.clear();
    _query.clear();
    _version.clear();

    _headers.clear();
    _params.clear();
    _body.clear();
}
