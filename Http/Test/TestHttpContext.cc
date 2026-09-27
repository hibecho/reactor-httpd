#include "protocol/HttpContext.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using Result = HttpContext::ParseResult;
const std::string kGet = "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";

// remaining 是否为 wire 的后缀：解析器只能丢弃前缀，不能改写或凭空产生字节。
bool IsSuffixOf(const std::string &remaining, const std::string &wire)
{
    return remaining.size() <= wire.size() &&
           wire.compare(wire.size() - remaining.size(), remaining.size(), remaining) == 0;
}

// 断言解析失败并给出指定状态码，同时验证错误终态的完整契约。
// 注意所有断言都建立在 Parse 之前就记录好的原始输入上：事后采样只能证明终态粘滞，
// 无法证明出错的那一次没有过度消费。
void ExpectError(const std::string &wire, int status)
{
    HttpContext context;
    Buffer buffer;
    buffer.WriteString(wire);
    assert(context.GetErrorStatus() == 0);

    const Result first = context.Parse(buffer);
    if (first != Result::Error || context.GetErrorStatus() != status)
    {
        std::cerr << "Expected HTTP error " << status << ", got " << context.GetErrorStatus() << " (result "
                  << static_cast<int>(first) << "), input: " << wire.substr(0, 100) << '\n';
        assert(false);
    }

    const std::string remaining = buffer.PeekAsString(buffer.GetReadableSize());
    // 解析器不能吞掉报错位置之后的数据，否则上层无法为同连接的下一条请求恢复。
    assert(!remaining.empty());
    assert(IsSuffixOf(remaining, wire));

    const Result again = context.Parse(buffer);
    assert(again == Result::Error);
    assert(buffer.PeekAsString(buffer.GetReadableSize()) == remaining);

    context.Reset();
    assert(context.GetErrorStatus() == 0);
    assert(context.GetRequest().GetMethod().empty());
    assert(buffer.PeekAsString(buffer.GetReadableSize()) == remaining);

    buffer.Clear();
    buffer.WriteString(kGet);
    assert(context.Parse(buffer) == Result::Complete);
}

// 断言解析成功、缓冲区被完整消费，并逐字段校验解析结果。
// 只断言"Complete + 缓冲区空"的话，字段解析写错也测不出来。
void ExpectComplete(const std::string &wire, const std::string &method, const std::string &path,
                    const std::string &version, const std::string &query = std::string())
{
    HttpContext context;
    Buffer buffer;
    buffer.WriteString(wire);
    const Result result = context.Parse(buffer);
    assert(result == Result::Complete);
    assert(buffer.GetReadableSize() == 0);

    const HttpRequest &request = context.GetRequest();
    assert(request.GetMethod() == method);
    assert(request.GetPath() == path);
    assert(request.GetVersion() == version);
    assert(request.GetQuery() == query);
}

void CheckRequest(const HttpRequest &request, const std::string &body)
{
    assert(request.GetMethod() == "POST");
    assert(request.GetPath() == "/a+b/c");
    assert(request.GetVersion() == "HTTP/1.1");
    assert(request.GetQuery() == "x=a%26b%3Dc&space=hello+world&x=second&empty=&flag&eq=a=b");
    assert(request.GetParams("x") == "a&b=c");
    assert(request.GetParams("space") == "hello world");
    assert(request.FindParam("empty") && request.GetParams("empty").empty());
    assert(request.FindParam("flag") && request.GetParams("flag").empty());
    assert(request.GetParams("eq") == "a=b");
    assert(request.GetHeaders("host") == "example.com");
    assert((request.GetHeaderValues("x-test") == std::vector<std::string>{"one", "two"}));
    assert(request.GetBody() == body);
}

void TestFragmentationAndPipelining()
{
    const std::string body("a\0\r\nb", 5);
    const std::string wire =
        "POST /a+b%2Fc?x=a%26b%3Dc&space=hello+world&x=second&empty=&flag&eq=a=b HTTP/1.1\r\n"
        "Host: \texample.com \t\r\nX-Test: one\r\nX-Test: two\r\nContent-Length: 5\r\n\r\n" + body;
    // 每个切分点都要可续接，尤其是 CR 与 LF 之间及正文内部。
    for (std::size_t split = 0; split <= wire.size(); ++split)
    {
        HttpContext context;
        Buffer buffer(1);
        buffer.Write(wire.data(), split);
        // 驱动调用必须先取出结果再断言，否则一旦用 -DNDEBUG 构建，整个 Parse 都不会执行。
        const Result partial = context.Parse(buffer);
        assert(partial == (split == wire.size() ? Result::Complete : Result::NeedMore));
        buffer.Write(wire.data() + split, wire.size() - split);
        buffer.WriteString(kGet);
        const Result complete = context.Parse(buffer);
        assert(complete == Result::Complete);
        CheckRequest(context.GetRequest(), body);
        assert(buffer.PeekAsString(buffer.GetReadableSize()) == kGet);
        const Result again = context.Parse(buffer);
        assert(again == Result::Complete);
        assert(buffer.PeekAsString(buffer.GetReadableSize()) == kGet);
        context.Reset();
        assert(context.GetRequest().GetBody().empty());
        assert(!context.GetRequest().FindHeader("x-test"));
        assert(!context.GetRequest().FindParam("x"));
        assert(context.Parse(buffer) == Result::Complete);
        assert(context.GetRequest().GetPath() == "/next");
        assert(buffer.GetReadableSize() == 0);
    }
    HttpContext context;
    Buffer buffer(0);
    assert(context.Parse(buffer) == Result::NeedMore);
    for (std::size_t i = 0; i < wire.size(); ++i)
    {
        buffer.Write(wire.data() + i, 1);
        const Result result = context.Parse(buffer);
        assert(result == (i + 1 == wire.size() ? Result::Complete : Result::NeedMore));
    }
    CheckRequest(context.GetRequest(), body);
}

// 请求行必须是 "method SP target SP version" 三段；缺段或空行都报 400，不能被当成"头部结束"。
void TestRequestLineStructure()
{
    ExpectError("\r\nHost: x\r\n\r\n", 400);                // 裸 CRLF 作为请求行
    ExpectError("GET\r\nHost: x\r\n\r\n", 400);             // 只有方法，缺目标与版本
    ExpectError("GET /\r\nHost: x\r\n\r\n", 400);           // 缺版本
    ExpectError("GET  HTTP/1.1\r\nHost: x\r\n\r\n", 400);   // 目标为空
    ExpectError(" / HTTP/1.1\r\nHost: x\r\n\r\n", 400);     // 方法为空
    ExpectError("G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400);  // 方法含非 token 字符
    ExpectError("GET / HTTP/1.1 \r\nHost: x\r\n\r\n", 400); // 版本后多一个空格
    // 版本只接受 HTTP/1.0 与 HTTP/1.1；不支持的版本返回 400，不是 505。
    ExpectError("GET / HTTP/2.0\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.1x\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET / http/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.2\r\nHost: x\r\n\r\n", 400);
}

// 原始目标中的空白、控制字符、非 ASCII 字节与片段标记都必须拒绝。
void TestTargetByteValidation()
{
    ExpectError("GET /a#b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /a\tb HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /a" + std::string(1, static_cast<char>(0x7f)) + "b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /a" + std::string(1, static_cast<char>(0x80)) + "b HTTP/1.1\r\nHost: x\r\n\r\n", 400);

    // 解码之后才出现的控制字符必须单独再检查一次，否则 %00 会绕过上面的原始字节校验。
    ExpectError("GET /%00 HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /%0d%0a HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /%1f HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /%7f HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    // 解码结果里合法出现的可打印字符不该被误伤：只解码一次，%3F/%23 不是结构分隔符。
    ExpectComplete("GET /a%20b%2Fc%3Fd%23e HTTP/1.1\r\nHost: x\r\n\r\n", "GET", "/a b/c?d#e", "HTTP/1.1");
    // 非法转义无论出现在路径还是查询串都要拒绝。
    ExpectError("GET /bad% HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /%GG HTTP/1.1\r\nHost: x\r\n\r\n", 400);
}

// 查询串的分项规则：空项忽略、只按第一个等号拆分、无等号的 flag 值为空、加号按表单规则解码。
void TestQueryBoundaries()
{
    ExpectComplete("GET /path? HTTP/1.1\r\nHost: x\r\n\r\n", "GET", "/path", "HTTP/1.1");
    ExpectComplete("GET /?=v HTTP/1.1\r\nHost: x\r\n\r\n", "GET", "/", "HTTP/1.1", "=v");
    // 查询名部分的非法转义同样要拒绝，不能只检查值。
    ExpectError("GET /?%GG=1 HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("GET /?a=%GG HTTP/1.1\r\nHost: x\r\n\r\n", 400);

    HttpContext context;
    Buffer buffer;
    buffer.WriteString("GET /?&a=1&&b&c=x=y& HTTP/1.1\r\nHost: x\r\n\r\n");
    const Result result = context.Parse(buffer);
    assert(result == Result::Complete);
    const HttpRequest &request = context.GetRequest();
    assert(request.GetQuery() == "&a=1&&b&c=x=y&");
    assert(request.FindParam("a") && request.GetParams("a") == "1");
    assert(request.FindParam("b") && request.GetParams("b").empty());
    assert(request.FindParam("c") && request.GetParams("c") == "x=y");
    // 连续与首尾的 '&' 产生的空项被忽略，不会留下空名参数。
    assert(!request.FindParam(""));

    // 只有 '?' 时查询串为空，不产生任何参数。
    HttpContext empty_context;
    Buffer empty_buffer;
    empty_buffer.WriteString("GET /path? HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(empty_context.Parse(empty_buffer) == Result::Complete);
    assert(empty_context.GetRequest().GetQuery().empty());
    assert(empty_context.GetRequest().GetPath() == "/path");
}

// 字段名必须是 token，字段值只允许可见字符与 HTAB；两端空白被去掉，内部的保留。
void TestHeaderFieldBoundaries()
{
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\n:v\r\n\r\n", 400);                     // 空字段名
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\nHo@st: x\r\n\r\n", 400);               // 名字含非 token 字符
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\nX: a\rb\r\n\r\n", 400);                // 值含裸 CR
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\nX: a\nb\r\n\r\n", 400);                // 值含裸 LF
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\nX: a" + std::string(1, '\x01') + "b\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\nX: a" + std::string(1, static_cast<char>(0x7f)) + "b\r\n\r\n", 400);

    HttpContext context;
    Buffer buffer;
    buffer.WriteString("GET / HTTP/1.1\r\nHost: x\r\nX-Empty:\r\nX-Tab: \ta b\t\r\n\r\n");
    const Result result = context.Parse(buffer);
    assert(result == Result::Complete);
    const HttpRequest &request = context.GetRequest();
    // 非 Host 的空值是合法的：字段存在，值为空串。
    assert(request.FindHeader("x-empty"));
    assert(request.GetHeaders("x-empty").empty());
    // 值两端的空格与制表符被去掉，内部的空白原样保留。
    assert(request.GetHeaders("x-tab") == "a b");
}

// Host 必须存在且唯一：HTTP/1.1 缺 Host 报错，重复 Host 在任何版本下都报错，大小写不敏感。
void TestHostCombinations()
{
    ExpectError("GET / HTTP/1.1\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.1\r\nHost: \t\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.1\r\nHost: a\r\nHOST: b\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.1\r\nHost: a\r\nhost: a\r\n\r\n", 400);
    ExpectComplete("GET / HTTP/1.1\r\nHost: a\r\n\r\n", "GET", "/", "HTTP/1.1");
    // HTTP/1.0 可以省略 Host，但不能重复或为空。
    ExpectComplete("GET / HTTP/1.0\r\n\r\n", "GET", "/", "HTTP/1.0");
    ExpectError("GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", 400);
    ExpectError("GET / HTTP/1.0\r\nHost:\r\n\r\n", 400);
}

// Transfer-Encoding 一律不支持：单独出现 501，与 Content-Length 并存 400（字段名大小写不敏感）。
void TestTransferEncodingCombinations()
{
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: identity\r\n\r\n", 501);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\r\n\r\n", 501);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\ntRaNsFeR-Encoding: chunked\r\ncontent-length: 1\r\n\r\n", 400);
}

// CR 恰好落在批次末尾时不能立即判定，必须等下一批；后一个字节不是 LF 才是错误。
void TestCrossCallCarriageReturn()
{
    HttpContext context;
    Buffer buffer;
    buffer.WriteString("GET / HTTP/1.1\r");
    assert(context.Parse(buffer) == Result::NeedMore);
    buffer.WriteString("X");
    assert(context.Parse(buffer) == Result::Error);
    assert(context.GetErrorStatus() == 400);
}

// 解析失败时只能丢弃已经成功解析的行；出错那一行及其后的字节必须原样保留。
// 这是"Error 不越界消费"契约唯一能区分变异的观测点：在 Parse 之前就要记录期望值。
void TestErrorLeavesOffendingBytesIntact()
{
    const std::string bad_header = "Bad Header\r\n";
    const std::string next = "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    HttpContext context;
    Buffer buffer;
    buffer.WriteString("GET /first HTTP/1.1\r\nHost: example.com\r\n" + bad_header + next);

    const Result result = context.Parse(buffer);
    assert(result == Result::Error);
    assert(context.GetErrorStatus() == 400);
    // 请求行与合法头已被消费，从出错行开始（含后续完整请求）必须完好保留。
    assert(buffer.PeekAsString(buffer.GetReadableSize()) == bad_header + next);
}

// 溢出判据的精确边界：恰好 SIZE_MAX 仍在可表示范围内，随后被 8 MiB 上限拦成 413；
// 再多一位才在长度解析阶段判定溢出，返回 400。
void TestContentLengthOverflowBoundary()
{
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551615\r\n\r\n", 413);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551616\r\n\r\n", 400);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n", 400);
}

// Reset 必须清零累计头部字节数，否则连续两条大头部的请求，第二条会被误判成 431。
void TestResetClearsAccumulatedHeaderBytes()
{
    const std::string headers = "Host: x\r\nH: " + std::string(20000, 'a') + "\r\n";
    const std::string request = "GET / HTTP/1.1\r\n" + headers + "\r\n";
    assert(headers.size() == 20014);
    assert(request.size() < 32768);

    HttpContext context;
    Buffer buffer;
    for (int i = 0; i < 2; ++i)
    {
        buffer.WriteString(request);
        const Result result = context.Parse(buffer);
        assert(result == Result::Complete);
        assert(buffer.GetReadableSize() == 0);
        context.Reset();
    }
}

void TestSyntaxAndFraming()
{
    ExpectComplete("GET / HTTP/1.0\r\n\r\n", "GET", "/", "HTTP/1.0");
    ExpectComplete("OPTIONS * HTTP/1.1\r\nHost: example.com\r\n\r\n", "OPTIONS", "*", "HTTP/1.1");
    ExpectComplete("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 003, 3\r\ncontent-length: 3\r\n\r\nabc", "POST", "/",
                   "HTTP/1.1");
    ExpectComplete("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "GET", "/", "HTTP/1.1");
    // 星号目标只与 OPTIONS 搭配，且不能再带查询串。
    ExpectError("GET * HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("HEAD * HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ExpectError("OPTIONS *?x=1 HTTP/1.1\r\nHost: x\r\n\r\n", 400);

    const std::vector<std::string> invalid = {
        "GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n",   "GET  / HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\nHost: x\n\n",                 "GET / HTTP/1.1\rX",
        "GET / HTTP/1.1\r\nHost: x\n\n",               "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", "GET / HTTP/1.1\r\nHost: x\r\nMissingColon\r\n\r\n",
    };
    for (const auto &wire : invalid)
        ExpectError(wire, 400);

    for (const std::string value : {"-1", "+1", "1x", "", "1,", ",1", "1,,1", "1,2", "1 1", "0x10"})
        ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + value + "\r\n\r\n", 400);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", 400);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", 501);
    // 仅发送头部也必须立即拒绝，不能等待客户端在收到 100 Continue 后才发送的正文。
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n", 417);
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\neXpEcT: custom\r\n\r\n", 417);
    // 字段值里直接出现的 NUL 必须拒绝。
    ExpectError(std::string("GET / HTTP/1.1\r\nHost: x\r\nX: a") + '\0' + "b\r\n\r\n", 400);
}

void TestLimits()
{
    const std::string prefix = "GET /";
    const std::string suffix = " HTTP/1.1\r\n";
    const std::string line = prefix + std::string(8192 - prefix.size() - suffix.size(), 'a') + suffix;
    // 恰好 8 KiB（含 CRLF）的请求行合法。
    ExpectComplete(line + "Host: x\r\n\r\n", "GET", line.substr(4, line.size() - 4 - suffix.size()), "HTTP/1.1");
    ExpectError(prefix + std::string(8193 - prefix.size() - suffix.size(), 'a') + suffix, 414);
    ExpectError(std::string(8193, 'a'), 414);

    const std::string headers = "Host: x\r\nX: " + std::string(32768 - 16, 'a') + "\r\n\r\n";
    assert(headers.size() == 32768);
    ExpectComplete("GET / HTTP/1.1\r\n" + headers, "GET", "/", "HTTP/1.1");
    ExpectError("GET / HTTP/1.1\r\nX: a\r\n" + headers, 431);
    ExpectError("GET / HTTP/1.1\r\nHost: x\r\nX: " + std::string(32768, 'a'), 431);

    constexpr std::size_t max_body = 8 * 1024 * 1024;
    HttpContext context;
    Buffer buffer;
    buffer.WriteString("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(max_body) + "\r\n\r\n");
    assert(context.Parse(buffer) == Result::NeedMore);
    const std::string body(max_body, 'b');
    buffer.WriteString(body);
    buffer.WriteString(kGet);
    assert(context.Parse(buffer) == Result::Complete);
    assert(context.GetRequest().GetBody() == body);
    assert(buffer.PeekAsString(buffer.GetReadableSize()) == kGet);
    ExpectError("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(max_body + 1) + "\r\n\r\n", 413);
}

// 上限必须跨多次 Parse 生效，且头部额度要按"已消费的头部字节"扣减。
void TestLimitsAcrossCalls()
{
    // 请求行：片段未越界时等待，补足到越界后报 414。
    const std::string prefix = "GET /";
    const std::string suffix = " HTTP/1.1\r\n";
    const std::string oversized = prefix + std::string(8193 - prefix.size() - suffix.size(), 'a') + suffix;
    assert(oversized.size() == 8193);
    {
        HttpContext context;
        Buffer buffer;
        buffer.WriteString(oversized.substr(0, 4096));
        assert(context.Parse(buffer) == Result::NeedMore);
        buffer.WriteString(oversized.substr(4096));
        assert(context.Parse(buffer) == Result::Error);
        assert(context.GetErrorStatus() == 414);
    }

    // 头部：先消费掉 1000 字节头部，剩余额度变成 31768。
    // 再收到一条 32000 字节且没有 CRLF 的行——超过剩余额度但不足整个 32 KiB，
    // 只有按累计已用字节记账才会在此判定 431；若实现忽略已用额度，则会错误地继续等待。
    const std::string headers = "H: " + std::string(995, 'a') + "\r\n";
    assert(headers.size() == 1000);
    {
        HttpContext context;
        Buffer buffer;
        buffer.WriteString("GET / HTTP/1.1\r\n" + headers + "X: " + std::string(16000, 'b'));
        assert(context.Parse(buffer) == Result::NeedMore);
        buffer.WriteString(std::string(16000, 'c'));
        assert(context.Parse(buffer) == Result::Error);
        assert(context.GetErrorStatus() == 431);
    }
}
} // namespace

int main()
{
    TestFragmentationAndPipelining();
    TestRequestLineStructure();
    TestTargetByteValidation();
    TestQueryBoundaries();
    TestHeaderFieldBoundaries();
    TestHostCombinations();
    TestTransferEncodingCombinations();
    TestCrossCallCarriageReturn();
    TestErrorLeavesOffendingBytesIntact();
    TestContentLengthOverflowBoundary();
    TestResetClearsAccumulatedHeaderBytes();
    TestSyntaxAndFraming();
    TestLimits();
    TestLimitsAcrossCalls();
    std::cout << "HttpContext tests passed\n";
}
