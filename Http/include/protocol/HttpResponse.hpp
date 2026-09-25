/**
 * @brief HTTP 响应报文的数据模型
 *
 * 该类只负责保存服务器要返回的内容，并把它们序列化为一段完整报文；
 * 报文的实际发送由连接层按字节完成（例如 Connection::Send）。
 *
 * 1.响应报文结构
 *
 * - 响应行: HTTP 版本 + 空格 + 状态码 + 空格 + 状态描述 + CRLF (\r\n)
 *
 * 示例:
 * HTTP/1.1 200 OK\r\n
 *
 * - 响应头: key:value
 *
 * 示例:
 * Content-Type: text/plain; charset=utf-8\r\n
 * Content-Length: 5\r\n
 * Connection: keep-alive\r\n
 *
 * - 响应正文: 位于响应头之后的空行之后，按字节原样发送
 *
 * 示例:
 * \r\n
 * hello
 *
 * 2.功能性接口
 *
 * - 设置状态码、协议版本与连接复用意图
 * - 设置或追加响应头字段
 * - 设置响应正文，或直接构造重定向响应
 * - 把上述内容序列化为可发送的完整报文
 *
 * 3.序列化规则
 *
 * - Content-Length 由正文长度自动生成；1xx、204、304 不输出该字段，205 输出 0
 * - Connection 由 _close 自动生成，调用方设置的同名字段不会重复输出
 * - 重定向的 Location 由 SetRedirect 记录，序列化时统一输出
 * - HEAD 请求只输出响应行与响应头，不输出正文
 *
 * 4.入参校验
 *
 * - 字段名、字段值、重定向目标与协议版本在写入时即校验，含 CR/LF 等控制字符的
 *   输入会抛出 std::invalid_argument，避免在序列化时拆出额外的头行（响应拆分）
 * - 校验只覆盖报文语法层；数值、URI 语义与业务含义不在本类职责内
 *
 */
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class HttpResponse
{
  public:
    // 构造响应对象，默认状态码为 200。
    explicit HttpResponse(int status = 200);

    // 设置 HTTP 协议版本，例如 "HTTP/1.1"；含控制字符时抛出 std::invalid_argument。
    void SetVersion(std::string version);
    // 设置状态码，例如 200、404；状态描述在序列化时按状态码生成。
    void SetStatus(int status);
    // 设置响应发送完成后是否关闭连接：true 表示关闭。
    void SetClose(bool close);

    // 替换全部同名头（名称忽略 ASCII 大小写），再插入该字段。
    // 字段名必须是 RFC 9110 token，字段值不得含除 HTAB 外的控制字符；违反抛 std::invalid_argument。
    void SetHeader(std::string name, std::string value);

    // 追加一条头字段，允许出现同名字段，例如多条 Set-Cookie。
    // 校验规则与 SetHeader 相同。
    void AddHeader(std::string name, std::string value);

    // 设置响应正文，并把 Content-Type 替换为 content_type；content_type 按头字段值校验。
    void SetBody(std::string body, std::string content_type = "text/plain; charset=utf-8");

    // 设置重定向：状态码与 Location 目标，status 默认 302 Found。
    // 目标必须是合法的 URI 引用（禁止空格与控制字符），否则抛 std::invalid_argument。
    void SetRedirect(std::string location, int status = 302);

    // 获取当前状态码。
    int GetStatus() const;
    // 判断响应发送完成后是否需要关闭连接。
    bool IsClose() const;
    // 获取响应正文的只读引用；引用不得超出该响应对象的生命周期。
    const std::string &GetBody() const;
    // 判断该响应是否由 SetRedirect 设置为重定向。
    bool IsRedirect() const;

    // 生成完整报文；HEAD 请求仅输出状态行和响应头。
    std::string Serialize(bool head_request = false) const;

  private:
    // 以插入顺序保存响应头，允许同名字段重复出现。
    using Headers = std::vector<std::pair<std::string, std::string>>;

    // 该状态码是否禁止携带正文：1xx、204、205、304。
    static bool IsBodyForbidden(int status);
    // 该状态码是否完全不输出 Content-Length：1xx、204、304。205 不在此列，它输出 0。
    static bool OmitsContentLength(int status);

    // 校验字段名与字段值；失败抛出 std::invalid_argument，绝不静默截断或改写输入。
    // 字段名的 token 判据与请求侧解析共用 Util::IsToken，两边不会各自漂移。
    static void ValidateFieldName(std::string_view name);
    static void ValidateFieldValue(std::string_view value);
    // 校验重定向目标：URI 引用不得含空格、C0 控制字符或 DEL。
    static void ValidateUri(std::string_view value);

    std::string _version = "HTTP/1.1"; // HTTP 版本
    int _status = 200;                 // 状态码
    bool _close = true;                // 是否关闭连接

    Headers _headers;          // 响应头，允许同名重复
    std::string _body;         // 响应正文
    bool _redirect_flag;       // 是否为重定向响应
    std::string _redirect_url; // 重定向目标地址，序列化时写入 Location 头
};
