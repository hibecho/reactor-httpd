#include "protocol/HttpResponse.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    template <class Exception, class Function> void Rejects(Function function)
    {
        bool rejected = false;
        try
        {
            function();
        }
        catch (const Exception &)
        {
            rejected = true;
        }
        assert(rejected);
    }

    void TestDefaults()
    {
        HttpResponse response;
        assert(response.GetStatus() == 200);
        assert(response.IsClose());
        assert(!response.IsRedirect());
        assert(response.GetBody().empty());
    }

    void TestBodyAndHeaders()
    {
        HttpResponse response;
        response.SetBody("hello world");
        assert(response.GetBody() == "hello world");
        assert(!response.IsRedirect());

        const std::string wire = response.Serialize();
        assert(wire == "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain; charset=utf-8\r\n"
                       "Content-Length: 11\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "hello world");
        // 序列化不应改变对象状态，连续两次结果一致。
        assert(response.Serialize() == wire);
    }

    void TestStatusVersionContentTypeAndClose()
    {
        HttpResponse response(201);
        response.SetVersion("HTTP/1.0");
        response.SetClose(false);
        response.SetBody("<html></html>", "text/html; charset=utf-8");

        assert(!response.IsClose());
        assert(response.Serialize() == "HTTP/1.0 201 Created\r\n"
                                       "Content-Type: text/html; charset=utf-8\r\n"
                                       "Content-Length: 13\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n"
                                       "<html></html>");
    }

    void TestHeadRequestKeepsContentLength()
    {
        HttpResponse response;
        response.SetBody("hello world");

        assert(response.Serialize(true) == "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/plain; charset=utf-8\r\n"
                                           "Content-Length: 11\r\n"
                                           "Connection: close\r\n"
                                           "\r\n");
    }

    void TestRedirect()
    {
        HttpResponse response;
        response.SetRedirect("/login");
        assert(response.GetStatus() == 302);
        assert(response.IsRedirect());
        assert(response.Serialize() == "HTTP/1.1 302 Found\r\n"
                                       "Location: /login\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");

        HttpResponse moved;
        moved.SetRedirect("/moved", 301);
        assert(moved.GetStatus() == 301);
        assert(moved.Serialize() == "HTTP/1.1 301 Moved Permanently\r\n"
                                    "Location: /moved\r\n"
                                    "Content-Length: 0\r\n"
                                    "Connection: close\r\n"
                                    "\r\n");
    }

    void TestAddHeaderKeepsDuplicateFields()
    {
        HttpResponse response;
        response.AddHeader("Set-Cookie", "sid=abc");
        response.AddHeader("Set-Cookie", "theme=dark");

        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "Set-Cookie: sid=abc\r\n"
                                       "Set-Cookie: theme=dark\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");
    }

    void TestSetHeaderReplacesAllSameName()
    {
        HttpResponse response;
        response.AddHeader("X-Token", "first");
        response.AddHeader("x-token", "second");
        response.SetHeader("X-TOKEN", "replaced");

        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "X-TOKEN: replaced\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");
    }

    void TestBodyForbiddenStatus()
    {
        // 精确比较整段报文：只断言"不含 Content-Length"的话，实现把头块整个删空也能通过。
        // 第一组完全不输出 Content-Length：1xx 与 204 由 RFC 9110 §8.6 禁止携带该字段，304 也选择省略。
        const std::pair<int, const char *> omitted[] = {
            {100, "Continue"},
            {101, "Switching Protocols"},
            {204, "No Content"},
            {304, "Not Modified"},
        };
        for (const auto &entry : omitted)
        {
            HttpResponse response(entry.first);
            response.SetBody("must not be sent");

            const std::string expected = std::string("HTTP/1.1 ") + std::to_string(entry.first) + " " + entry.second +
                                         "\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n";
            assert(response.Serialize() == expected);
            // 禁止正文的状态码下，HEAD 与 GET 的报文相同。
            assert(response.Serialize(true) == expected);
        }

        // 205 是另一种情形：不允许正文，但必须显式声明零长度，
        // 否则在 keep-alive 连接上客户端会一直等待一个永远不会到来的正文。
        HttpResponse reset_content(205);
        reset_content.SetBody("must not be sent");
        const std::string expected = "HTTP/1.1 205 Reset Content\r\n"
                                     "Content-Type: text/plain; charset=utf-8\r\n"
                                     "Content-Length: 0\r\n"
                                     "Connection: close\r\n"
                                     "\r\n";
        assert(reset_content.Serialize() == expected);
        assert(reset_content.Serialize(true) == expected);
    }

    // 1xx 整段都不允许正文，端点上同样要成立；206 则是普通响应，正文与长度照常输出。
    void TestBodyForbiddenBoundaries()
    {
        for (int status : {100, 101, 103, 199})
        {
            HttpResponse response(status);
            response.SetBody("must not be sent");
            assert(response.Serialize().find("Content-Length") == std::string::npos);
            assert(response.Serialize().find("must not be sent") == std::string::npos);
        }

        HttpResponse partial(206);
        partial.SetBody("part");
        assert(partial.Serialize() == "HTTP/1.1 206 Partial Content\r\n"
                                      "Content-Type: text/plain; charset=utf-8\r\n"
                                      "Content-Length: 4\r\n"
                                      "Connection: close\r\n"
                                      "\r\n"
                                      "part");
    }

    // 响应拆分：字段名、字段值与重定向目标都不得携带 CR/LF。
    void TestHeaderInjectionIsRejected()
    {
        HttpResponse response;
        Rejects<std::invalid_argument>([&] {
            response.SetHeader("X-Bad", "v\r\nEvil: 1");
        });
        Rejects<std::invalid_argument>([&] {
            response.AddHeader("X-Bad", "v\nEvil: 1");
        });
        Rejects<std::invalid_argument>([&] {
            response.SetHeader("X-Bad\r\nEvil", "v");
        });
        Rejects<std::invalid_argument>([&] {
            response.AddHeader("X-Bad", "v\revil");
        });
        Rejects<std::invalid_argument>([&] {
            response.SetHeader("X Bad", "v");
        });
        Rejects<std::invalid_argument>([&] {
            response.SetHeader("", "v");
        });
        Rejects<std::invalid_argument>([&] {
            response.AddHeader("X-Bad", std::string("v\0w", 3));
        });
        Rejects<std::invalid_argument>([&] {
            response.SetRedirect("/ok\r\nEvil: 1");
        });
        Rejects<std::invalid_argument>([&] {
            response.SetVersion("HTTP/1.1\r\nEvil: 1");
        });

        // 被拒绝的字段不得进入对象，也不得影响先前已写入的内容。
        response.SetHeader("X-Ok", "v");
        Rejects<std::invalid_argument>([&] {
            response.SetHeader("X-Bad", "v\r\nEvil: 1");
        });
        // 水平制表符是合法字段值字节，必须放行。
        response.SetHeader("X-Tab", "a\tb");
        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "X-Ok: v\r\n"
                                       "X-Tab: a\tb\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");
    }

    // 重定向目标只能有一个来源；不重定向时调用方设置的 Location 仍应原样输出。
    void TestRedirectLocationHasSingleSource()
    {
        HttpResponse response;
        response.SetRedirect("/from-redirect");
        response.SetHeader("Location", "/from-header");
        assert(response.Serialize() == "HTTP/1.1 302 Found\r\n"
                                       "Location: /from-redirect\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");

        HttpResponse manual;
        manual.SetHeader("Location", "/manual");
        assert(manual.Serialize() == "HTTP/1.1 200 OK\r\n"
                                     "Location: /manual\r\n"
                                     "Content-Length: 0\r\n"
                                     "Connection: close\r\n"
                                     "\r\n");
    }

    // SetStatus 在 SetRedirect 之后调用不会清掉重定向标记：状态码被改写、Location 仍然输出。
    // 这是当前选定的行为，不是缺陷，固定下来避免无声漂移。
    void TestSetStatusAfterRedirectKeepsLocation()
    {
        HttpResponse response;
        response.SetRedirect("/login", 301);
        response.SetStatus(200);
        assert(response.GetStatus() == 200);
        assert(response.IsRedirect());
        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "Location: /login\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");
    }

    void TestSetStatusTakesEffectImmediately()
    {
        HttpResponse response;
        response.SetStatus(404);
        assert(response.GetStatus() == 404);
        response.SetStatus(204);
        assert(response.GetStatus() == 204);
    }

    // 第二次 SetBody 应替换 Content-Type，而不是留下两条同名头。
    void TestSetBodyReplacesContentType()
    {
        HttpResponse response;
        response.SetBody("a", "text/html");
        response.SetBody("bc");
        assert(response.GetBody() == "bc");
        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain; charset=utf-8\r\n"
                                       "Content-Length: 2\r\n"
                                       "Connection: close\r\n"
                                       "\r\n"
                                       "bc");
    }

    // 派生字段的跳过必须覆盖全部同名项，不能只跳第一条。
    void TestAllDuplicateDerivedFieldsAreSkipped()
    {
        HttpResponse response;
        response.AddHeader("Content-Length", "1");
        response.AddHeader("content-length", "2");
        response.AddHeader("CONNECTION", "upgrade");
        response.AddHeader("Connection", "keep-alive");
        response.SetBody("abc");
        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain; charset=utf-8\r\n"
                                       "Content-Length: 3\r\n"
                                       "Connection: close\r\n"
                                       "\r\n"
                                       "abc");
    }

    void TestEmptyHeaderTableSerialize()
    {
        HttpResponse response;
        const std::string expected = "HTTP/1.1 200 OK\r\n"
                                     "Content-Length: 0\r\n"
                                     "Connection: close\r\n"
                                     "\r\n";
        assert(response.Serialize() == expected);
        assert(response.Serialize(true) == expected);
    }

    void TestDerivedFramingFieldsWin()
    {
        HttpResponse response;
        response.SetHeader("Content-Length", "999");
        response.AddHeader("connection", "upgrade");
        response.SetBody("abc");

        assert(response.Serialize() == "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain; charset=utf-8\r\n"
                                       "Content-Length: 3\r\n"
                                       "Connection: close\r\n"
                                       "\r\n"
                                       "abc");
    }

    void TestBinaryBodyIsCopiedAndCountedByBytes()
    {
        const char raw[] = {'a', '\0', 'b', '\r', '\n'};
        HttpResponse response;
        response.SetBody(std::string(raw, sizeof(raw)));

        assert(response.GetBody() == std::string(raw, sizeof(raw)));

        const std::string prefix = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain; charset=utf-8\r\n"
                                   "Content-Length: 5\r\n"
                                   "Connection: close\r\n"
                                   "\r\n";
        const std::string wire = response.Serialize();
        assert(wire.size() == prefix.size() + sizeof(raw));
        assert(wire.compare(0, prefix.size(), prefix) == 0);
        assert(wire.compare(prefix.size(), sizeof(raw), std::string(raw, sizeof(raw))) == 0);
    }

    void TestUnregisteredStatusFallsBackToUnknown()
    {
        HttpResponse response(599);
        assert(response.Serialize() == "HTTP/1.1 599 Unknown\r\n"
                                       "Content-Length: 0\r\n"
                                       "Connection: close\r\n"
                                       "\r\n");
    }
} // namespace

int main()
{
    TestDefaults();
    TestBodyAndHeaders();
    TestStatusVersionContentTypeAndClose();
    TestHeadRequestKeepsContentLength();
    TestRedirect();
    TestAddHeaderKeepsDuplicateFields();
    TestSetHeaderReplacesAllSameName();
    TestBodyForbiddenStatus();
    TestBodyForbiddenBoundaries();
    TestHeaderInjectionIsRejected();
    TestRedirectLocationHasSingleSource();
    TestSetStatusAfterRedirectKeepsLocation();
    TestSetStatusTakesEffectImmediately();
    TestSetBodyReplacesContentType();
    TestAllDuplicateDerivedFieldsAreSkipped();
    TestEmptyHeaderTableSerialize();
    TestDerivedFramingFieldsWin();
    TestBinaryBodyIsCopiedAndCountedByBytes();
    TestUnregisteredStatusFallsBackToUnknown();

    std::cout << "HttpResponse tests passed" << std::endl;
    return 0;
}
