/**
 * @brief
 *
 * 1.HTTP报文结构
 *
 * - 请求行: 请求方法 + 空格 + 请求目标 + 空格 + HTTP 版本 + CRLF (\r\n)
 *
 * 示例：
 * GET /users/42?expand=orders HTTP/1.1 \r\n
 *
 * - 请求头: key:value
 *
 * 示例：
 * Host: api.example.com
 * Content-Type: application/json
 * Content-Length: 16
 * Accept: application/json
 *
 * - 请求正文:
 *
 * \r\n
 * hello
 *
 * 2.功能性接口
 *
 * -提供查询字符串，以及头部字段的单个查询和获取，插入功能
 * -获取正文长度
 * -判断是否为长短链接
 *
 */
#pragma once
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class HttpRequest
{
    // 头字段按插入顺序保存，允许同名字段重复出现。
    using Headers = std::vector<std::pair<std::string, std::string>>;
    // 查询参数按名称唯一保存。
    using Params = std::unordered_map<std::string, std::string>;

  public:
    // 以下接口返回内部字符串的只读引用，避免复制；引用不得超出请求对象的生命周期。
    // 获取请求方法，例如 "GET"、"POST"。
    const std::string &GetMethod() const;
    // 获取请求路径，例如 "/users"，不包含查询字符串。
    const std::string &GetPath() const;
    // 获取原始查询字符串，例如 "role=admin"，不包含开头的 '?'。
    const std::string &GetQuery() const;
    // 获取 HTTP 协议版本，例如 "HTTP/1.1"。
    const std::string &GetVersion() const;
    // 获取请求正文；正文可以包含二进制数据。
    const std::string &GetBody() const;

    // 获取当前已保存正文的字节数，不等同于请求头声明的 Content-Length。
    std::size_t GetBodySize() const;

    // 判断名为 name 的请求头是否存在：存在返回 true，否则返回 false。
    bool FindHeader(const std::string &name) const;
    // 判断名为 name 的查询参数是否存在；值为空不代表参数不存在。
    bool FindParam(const std::string &name) const;

    // 获取名称为 key 的第一个请求头值，返回字符串副本；键不存在时返回空字符串。
    // 同名字段重复出现时只取第一条，需要全部取值请用 GetHeaderValues。
    std::string GetHeaders(const std::string &key) const;
    // 获取名称为 key 的全部请求头值，按插入顺序返回；键不存在时返回空 vector。
    std::vector<std::string> GetHeaderValues(const std::string &key) const;
    // 获取名称为 key 的查询参数值，返回字符串副本；键不存在时的行为需在实现中明确。
    std::string GetParams(const std::string &key) const;

    // 设置请求方法，method 例如 "GET"、"POST"。
    void SetMethod(std::string method);
    // 设置请求路径，path 例如 "/users"，不包含查询字符串。
    void SetPath(std::string path);
    // 设置原始查询字符串，query 不包含开头的 '?'；是否同步解析参数需在实现中明确。
    void SetQuery(std::string query);
    // 设置 HTTP 协议版本，version 例如 "HTTP/1.1"。
    void SetVersion(std::string version);

    // 字段名统一转为小写；按插入顺序追加，允许同名字段重复出现。
    void AddHeader(std::string name, std::string value);
    // 添加查询参数，name 为参数名称，value 为参数值；同名键的处理策略需在实现中明确。
    void AddParam(std::string name, std::string value);
    // 将 data 指定的字节追加到正文，支持分批接收；保存内容时需复制，不持有该视图。
    void AppendBody(std::string_view data);

    // 根据 HTTP 版本和 Connection 请求头判断连接复用意图；最终是否关闭由服务器决定。
    bool IsKeepAlive() const;
    // 清空本次请求的所有数据，为处理下一条请求做准备。
    void Reset();

  private:
    // 按逗号拆分 Connection 值，忽略两端空白和 ASCII 大小写，完整匹配小写标记。
    bool HasConnectionToken(std::string_view token) const;
    // 两个字段名是否相同，按 ASCII 忽略大小写。
    static bool SameFieldName(std::string_view lhs, std::string_view rhs);

    std::string _method;  // 请求方法，表示客户端希望执行的操作
    std::string _path;    // 请求目标的路径部分，通常用于路由匹配
    std::string _query;   // ? 后面的原始查询字符串，不包含 ?
    std::string _version; // HTTP 协议版本

    Headers _headers;     // 请求头字段，按插入顺序保存，允许同名重复
    Params _params;       // 将查询字符串拆分后得到的名称和值
    std::string _body;    // 空行后面的正文
    std::smatch _matches; // 资源路径的正则提取数据
};
