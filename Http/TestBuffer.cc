#include <cassert>
#include <iostream>
#include <string>

#include "Buffer.hpp"

namespace
{
// 校验 FindCRLF 返回的是可读区内的 '\r'，且未消费数据。
void CheckFound(const Buffer &b, std::size_t offset)
{
    const char *found = b.FindCRLF();
    assert(found != nullptr);
    assert(found == b.GetReadPosition() + offset);
    assert(*found == '\r');
    assert(*(found + 1) == '\n');
}

void CheckNotFound(const Buffer &b)
{
    assert(b.FindCRLF() == nullptr);
}
} // namespace

int main()
{
    // 1. 空缓冲区
    {
        Buffer b;
        CheckNotFound(b);
        assert(b.ReadLine().empty());
        assert(b.GetReadableSize() == 0);
    }

    // 2. 长度不足 2：单个 '\r' 与单个普通字符
    {
        Buffer b;
        b.WriteString("\r");
        CheckNotFound(b);
        assert(b.ReadLine().empty());
        assert(b.GetReadableSize() == 1);
    }

    // 3. 跨两次写入的 CRLF：'\r' 到达时未找到，'\n' 到达后才找到
    {
        Buffer b;
        b.WriteString("abc\r");
        CheckNotFound(b);
        assert(b.ReadLine().empty());
        assert(b.GetReadableSize() == 4); // 未消费

        b.WriteString("\n");
        CheckFound(b, 3);
        assert(b.ReadLine() == "abc\r\n");
        assert(b.GetReadableSize() == 0); // 全部消费后偏移归零
    }

    // 4. 连续多行
    {
        Buffer b;
        b.WriteString("abc\r\ndef\r\n");
        assert(b.ReadLine() == "abc\r\n");
        assert(b.GetReadableSize() == 5);
        CheckFound(b, 3); // 读位置已前移，偏移相对当前读位置
        assert(b.ReadLine() == "def\r\n");
        assert(b.GetReadableSize() == 0);
    }

    // 5. 空行：返回 "\r\n"，与“未找到”返回的空串区分
    {
        Buffer b;
        b.WriteString("\r\n\r\n");
        assert(b.ReadLine() == "\r\n");
        assert(b.ReadLine() == "\r\n");
        assert(b.GetReadableSize() == 0);
        assert(b.ReadLine().empty()); // 已无数据
    }

    // 6. 孤立 '\r' 不是行尾：应跳过它找下一个 CRLF
    {
        Buffer b;
        b.WriteString("a\rb\r\n");
        CheckFound(b, 3);
        assert(b.ReadLine() == "a\rb\r\n");
    }

    // 7. 相邻 '\r\r\n'：第一个 '\r' 后是 '\r'，应从第二个开始匹配
    {
        Buffer b;
        b.WriteString("x\r\r\n");
        CheckFound(b, 2);
        assert(b.ReadLine() == "x\r\r\n");
    }

    // 8. 无 CRLF：未找到且不消费
    {
        Buffer b;
        b.WriteString("hello");
        CheckNotFound(b);
        assert(b.ReadLine().empty());
        assert(b.GetReadableSize() == 5);
        assert(b.PeekAsString(5) == "hello");
    }

    // 9. 含内嵌空字符的头部行
    {
        Buffer b;
        const char raw[] = {'a', '\0', 'b', '\r', '\n'};
        b.Write(raw, sizeof(raw));
        CheckFound(b, 3);
        const std::string line = b.ReadLine();
        assert(line.size() == 5);
        assert(line[1] == '\0');
        assert(b.GetReadableSize() == 0);
    }

    // 10. 消费后缓冲区复用：偏移归零后仍能正确查找
    {
        Buffer b;
        b.WriteString("one\r\n");
        assert(b.ReadLine() == "one\r\n");
        assert(b.GetReadableSize() == 0);
        b.WriteString("two\r\n");
        CheckFound(b, 3);
        assert(b.ReadLine() == "two\r\n");
    }

    // 11. 仅消费部分行后继续查找：剩余可读区内的 CRLF 仍可被定位
    {
        Buffer b;
        b.WriteString("head\r\ntail\r\n");
        assert(b.ReadLine() == "head\r\n");
        CheckFound(b, 4); // 此时读位置已指向 "tail\r\n"
        assert(b.ReadLine() == "tail\r\n");
    }

    // 12. PeekLine 不消费，可重复调用；ReadLine 才消费
    {
        Buffer b;
        b.WriteString("abc\r\n");
        assert(b.PeekLine() == "abc\r\n");
        assert(b.GetReadableSize() == 5); // 未消费
        assert(b.PeekLine() == "abc\r\n"); // 可重复查看
        CheckFound(b, 3);
        assert(b.ReadLine() == "abc\r\n"); // 消费
        assert(b.GetReadableSize() == 0);
    }

    // 13. 无完整行时 PeekLine/ReadLine 均返回空串且不消费
    {
        Buffer b;
        b.WriteString("hello");
        assert(b.PeekLine().empty());
        assert(b.GetReadableSize() == 5);
        assert(b.ReadLine().empty());
        assert(b.GetReadableSize() == 5);
        assert(b.PeekAsString(5) == "hello");
    }

    // 14. 空行经 PeekLine/ReadLine 返回 "\r\n"，与“未找到”的空串区分
    {
        Buffer b;
        b.WriteString("\r\n");
        assert(b.PeekLine() == "\r\n");
        assert(b.GetReadableSize() == 2);
        assert(b.ReadLine() == "\r\n");
        assert(b.GetReadableSize() == 0);
    }

    // 15. PeekLine 返回的行与随后 ReadLine 消费的内容一致
    {
        Buffer b;
        b.WriteString("host: a.com\r\nB\r\n");
        const std::string peeked = b.PeekLine();
        assert(peeked == "host: a.com\r\n");
        assert(b.ReadLine() == peeked);
        assert(b.PeekLine() == "B\r\n");
        assert(b.ReadLine() == "B\r\n");
        assert(b.GetReadableSize() == 0);
    }

    // const 查看接口：末尾孤立回车不影响此前完整行，查看不消费。
    {
        Buffer b;
        b.WriteString("abc\r\nxyz\r");
        const Buffer &view = b;
        CheckFound(view, 3);
        assert(view.PeekLine() == "abc\r\n");
        assert(view.PeekAsString(9) == "abc\r\nxyz\r");
        assert(b.GetReadableSize() == 9);
        assert(b.ReadLine() == "abc\r\n");
        assert(view.PeekLine().empty());
        assert(b.ReadLine().empty());
        assert(view.PeekAsString(4) == "xyz\r");
    }

    // 零容量缓冲区也能安全进行只读查找。
    {
        const Buffer b(0);
        CheckNotFound(b);
        assert(b.PeekLine().empty());
        assert(b.PeekAsString(0).empty());
    }

    std::cout << "all Buffer CRLF tests passed" << std::endl;
    return 0;
}
