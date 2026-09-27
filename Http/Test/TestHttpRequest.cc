#include "protocol/HttpRequest.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    HttpRequest request;
    assert(!request.IsKeepAlive());
    request.SetVersion("HTTP/1.1");
    assert(request.IsKeepAlive());
    request.AddHeader("CoNnEcTiOn", " , Upgrade, X-close, close-extra, \t");
    assert(request.IsKeepAlive());
    assert(request.FindHeader("CONNECTION"));
    assert(!request.FindHeader("missing"));
    request.AddHeader("connection", "\tClOsE \t");
    assert(!request.IsKeepAlive());
    // 两条 Connection 字段行都要保留：GetHeaders 取第一条，GetHeaderValues 取全部。
    assert(request.GetHeaderValues("connection").size() == 2);
    assert(request.GetHeaderValues("connection")[1].find("ClOsE") != std::string::npos);

    request.Reset();
    request.SetVersion("HTTP/1.0");
    assert(!request.IsKeepAlive());
    request.AddHeader("Connection", "x-keep-alive, keep-alive-extra");
    assert(!request.IsKeepAlive());
    request.AddHeader("CONNECTION", " Upgrade, \tKeEp-AlIvE\t, ");
    assert(request.IsKeepAlive());
    request.AddHeader("connection", "close");
    assert(!request.IsKeepAlive());

    request.Reset();
    request.AddHeader("Connection", "close");
    request.AddHeader("Connection", "keep-alive");
    request.SetVersion("HTTP/1.1");
    assert(!request.IsKeepAlive());
    request.Reset();
    request.SetVersion("HTTP/9.9");
    request.AddHeader("Connection", "keep-alive");
    assert(!request.IsKeepAlive());

    request.SetMethod("POST");
    request.SetPath("/users");
    request.SetQuery("name=&flag");
    // 三个 setter 必须能原值读回；只写不读等于没有验证。
    assert(request.GetMethod() == "POST");
    assert(request.GetPath() == "/users");
    assert(request.GetQuery() == "name=&flag");
    // SetQuery 只保存原文，不解析参数：参数容器由 AddParam 单独维护。
    assert(!request.FindParam("flag"));
    assert(request.GetParams("flag").empty());
    request.AddParam("name", "");
    request.AddHeader("Host", "example.com");
    request.AddHeader("HOST", "ignored.example");
    assert(request.GetHeaders("hOsT") == "example.com");
    assert(request.FindParam("name"));
    assert(!request.FindParam("Host"));
    assert(!request.FindParam("missing"));
    // 缺键与"存在但值为空"必须可区分：两者都返回空串，但 FindParam 结论相反。
    assert(request.GetParams("name").empty());
    assert(request.GetParams("missing").empty());
    // 同名参数保留首次出现的值，后续同名调用被忽略。
    request.AddParam("name", "second");
    assert(request.GetParams("name").empty());

    char bytes[] = {'a', '\0', 'b'};
    request.AppendBody(std::string_view(bytes, sizeof(bytes)));
    bytes[0] = 'x'; // 请求必须持有副本。
    request.AppendBody({});
    request.AppendBody("cd");
    assert(request.GetBody() == std::string("a\0bcd", 5));
    assert(request.GetBodySize() == 5);

    request.Reset();
    assert(request.GetMethod().empty());
    assert(request.GetPath().empty());
    assert(request.GetQuery().empty());
    assert(request.GetVersion().empty());
    assert(request.GetBody().empty());
    assert(request.GetBodySize() == 0);
    assert(!request.FindHeader("host"));
    assert(!request.FindHeader("connection"));
    assert(!request.FindParam("name"));
    assert(!request.IsKeepAlive());
    request.SetVersion("HTTP/1.1");
    assert(request.IsKeepAlive());
    request.AppendBody("next");
    assert(request.GetBody() == "next");

    // 重复的同名请求头必须全部保留，并按插入顺序取回。
    request.Reset();
    request.AddHeader("Cookie", "a=1");
    request.AddHeader("COOKIE", "b=2");
    const std::vector<std::string> cookies = request.GetHeaderValues("cookie");
    assert(cookies.size() == 2);
    assert(cookies[0] == "a=1");
    assert(cookies[1] == "b=2");
    assert(request.GetHeaders("Cookie") == "a=1");
    assert(request.GetHeaders("missing").empty());
    assert(request.GetHeaderValues("missing").empty());

    // close 出现在第二条 Connection 字段行时，仍要判定为不保持连接。
    request.Reset();
    request.SetVersion("HTTP/1.1");
    request.AddHeader("Connection", "X-Token");
    assert(request.IsKeepAlive());
    request.AddHeader("Connection", "upgrade, close");
    assert(!request.IsKeepAlive());
    assert(request.GetHeaderValues("Connection").size() == 2);

    // Reset 必须清空全部头字段。
    request.Reset();
    assert(!request.FindHeader("Connection"));
    assert(request.GetHeaderValues("Connection").empty());
    assert(request.GetHeaderValues("Cookie").empty());

    std::cout << "HttpRequest tests passed\n";
}
