#include "protocol/HttpRequest.hpp"

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

// 返回转为小写后的字符串副本。
std::string AsciiLower(std::string value)
{
    for (char &ch : value)
    {
        ch = ToLowerAscii(ch);
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
    for (const auto &field : _headers)
    {
        if (SameFieldName(field.first, name))
        {
            return true;
        }
    }
    return false;
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

// 获取指定的头部；同名字段重复出现时返回第一条。
std::string HttpRequest::GetHeaders(const std::string &key) const
{
    for (const auto &field : _headers)
    {
        if (SameFieldName(field.first, key))
        {
            return field.second;
        }
    }
    return "";
}

// 获取全部同名头部，保持插入顺序。
std::vector<std::string> HttpRequest::GetHeaderValues(const std::string &key) const
{
    std::vector<std::string> values;
    for (const auto &field : _headers)
    {
        if (SameFieldName(field.first, key))
        {
            values.push_back(field.second);
        }
    }
    return values;
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
    _method = std::move(method);
}
void HttpRequest::SetPath(std::string path)
{
    _path = std::move(path);
}
void HttpRequest::SetQuery(std::string query)
{
    _query = std::move(query);
}
void HttpRequest::SetVersion(std::string version)
{
    _version = std::move(version);
}

// 字段名统一转小写；原样追加，不去重、不合并。
void HttpRequest::AddHeader(std::string name, std::string value)
{
    _headers.emplace_back(AsciiLower(std::move(name)), std::move(value));
}

void HttpRequest::AddParam(std::string name, std::string value)
{
    _params.emplace(std::move(name), std::move(value));
}

void HttpRequest::AppendBody(std::string_view data)
{
    if (data.empty())
        return;
    _body.append(data.data(), data.size());
}

bool HttpRequest::HasConnectionToken(std::string_view token) const
{
    // 同名 Connection 可能分散在多条字段行里，需要逐条检查。
    for (const auto &field : _headers)
    {
        if (!SameFieldName(field.first, "connection"))
            continue;

        // 字段值按逗号拆分，忽略两端空白与 ASCII 大小写，完整匹配标记。
        const std::string value = AsciiLower(field.second);
        std::string_view remaining(value);
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

// 两个字段名是否相同，按 ASCII 忽略大小写。
bool HttpRequest::SameFieldName(std::string_view lhs, std::string_view rhs)
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
