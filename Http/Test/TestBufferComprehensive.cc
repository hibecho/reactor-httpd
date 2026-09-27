#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "base/Buffer.hpp"

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #expr);                                 \
    } while (false)

template <class E, class F> void Throws(F operation)
{
    bool caught = false;
    try
    {
        operation();
    }
    catch (const E &)
    {
        caught = true;
    }
    CHECK(caught);
}

void Check(const Buffer &b, const std::string &expected)
{
    CHECK(b.GetReadableSize() == expected.size());
    CHECK(b.PeekAsString(expected.size()) == expected);
    const std::size_t crlf = expected.find("\r\n");
    const char *found = b.FindCRLF();
    if (crlf == std::string::npos)
    {
        CHECK(found == nullptr);
        CHECK(b.PeekLine().empty());
    }
    else
    {
        CHECK(found == b.GetReadPosition() + crlf);
        CHECK(b.PeekLine() == expected.substr(0, crlf + 2));
    }
}

void Basics()
{
    Buffer defaults;
    CHECK(defaults.GetWritableSize() == BUFFER_DEFAULT_SIZE);
    for (std::size_t size : {0u, 1u, 8u, 1024u})
    {
        Buffer b(size);
        Check(b, "");
        CHECK(b.GetPrependableSize() == 0 && b.GetWritableSize() == size);
        CHECK(b.Begin() == static_cast<const Buffer &>(b).Begin());
        CHECK(b.GetWritePosition() == b.Begin());
        CHECK(b.GetReadPosition() == b.Begin());
        b.Write(nullptr, 0);
        b.Peek(nullptr, 0);
        b.Read(nullptr, 0);
        b.MoveReadOffset(0);
        b.MoveWriteOffset(0);
        b.EnsureWritableSize(0);
        CHECK(b.ReadAsString(0).empty());
        CHECK(b.GetWritableSize() == size);
        b.WriteString("a");
        Check(b, "a");
        char dest[3] = {'!', '!', '!'};
        b.Peek(dest, 1);
        CHECK(dest[0] == 'a' && dest[1] == '!');
        Check(b, "a");
        b.Read(dest, 1);
        Check(b, "");
        CHECK(b.GetPrependableSize() == 0);
        b.Clear();
        b.Clear();
    }
    Buffer binary(0);
    std::string bytes;
    for (unsigned i = 0; i < 256; ++i)
        bytes.push_back(static_cast<char>(i));
    binary.WriteString(bytes);
    Check(binary, bytes);
    CHECK(binary.ReadAsString(bytes.size()) == bytes);
    Check(binary, "");
}

void Space()
{
    Buffer b(8);
    b.WriteString("abcdef");
    b.MoveReadOffset(2);
    char *storage = b.Begin();
    b.EnsureWritableSize(2);
    CHECK(b.Begin() == storage);
    CHECK(b.GetPrependableSize() == 2);
    Check(b, "cdef");
    b.EnsureWritableSize(4);
    CHECK(b.Begin() == storage);
    CHECK(b.GetPrependableSize() == 0);
    CHECK(b.GetWritableSize() == 4);
    Check(b, "cdef");
    std::memcpy(b.GetWritePosition(), "ghij", 4);
    b.MoveWriteOffset(4);
    Check(b, "cdefghij");
    b.MoveReadOffset(2);
    b.EnsureWritableSize(20);
    CHECK(b.GetWritableSize() >= 20);
    Check(b, "efghij");
    const std::size_t storage_size = b.GetPrependableSize() + b.GetReadableSize() + b.GetWritableSize();
    b.Clear();
    CHECK(b.GetWritableSize() == storage_size);
    Check(b, "");
}

void Errors()
{
    Buffer b(8);
    b.WriteString("abcdef");
    b.MoveReadOffset(2);
    const std::size_t read = b.GetPrependableSize(), write = b.GetWritableSize();
    char dest[8];
    std::memset(dest, '!', sizeof(dest));
    Throws<std::out_of_range>([&] { b.Peek(dest, 5); });
    Throws<std::out_of_range>([&] { b.Read(dest, 5); });
    for (char c : dest)
        CHECK(c == '!');
    Throws<std::out_of_range>([&] { b.MoveReadOffset(5); });
    Throws<std::out_of_range>([&] { b.MoveWriteOffset(write + 1); });
    Throws<std::out_of_range>([&] { b.PeekAsString(5); });
    Throws<std::out_of_range>([&] { b.ReadAsString(5); });
    Throws<std::invalid_argument>([&] { b.Write(nullptr, 1); });
    Throws<std::invalid_argument>([&] { b.Peek(nullptr, 1); });
    Throws<std::invalid_argument>([&] { b.Read(nullptr, 1); });
    const std::size_t huge = std::numeric_limits<std::size_t>::max();
    Throws<std::length_error>([&] { b.EnsureWritableSize(huge); });
    Throws<std::length_error>([&] { b.Write("x", huge); });
    Throws<std::out_of_range>([&] { b.Read(dest, huge); });
    CHECK(b.GetPrependableSize() == read && b.GetWritableSize() == write);
    Check(b, "cdef");
}

void CopiesAndMoves()
{
    Buffer original(16);
    original.WriteString("prefix-data");
    original.MoveReadOffset(7);
    Buffer copy(original);
    Check(copy, "data");
    copy.WriteString("!");
    Check(original, "data");
    Buffer assigned(1);
    assigned = original;
    assigned = assigned;
    Check(assigned, "data");
    Buffer moved(std::move(original));
    Check(moved, "data");
    CHECK(original.GetReadableSize() == 0);
    CHECK(original.GetPrependableSize() == 0);
    original.WriteString("reuse");
    Check(original, "reuse");
    Buffer target;
    target.WriteString("discard");
    target = std::move(moved);
    Check(target, "data");
    CHECK(moved.GetReadableSize() == 0);
    moved.WriteString("again");
    Check(moved, "again");
    Buffer &same = target;
    target = std::move(same);
    Check(target, "data");
    Buffer empty(0);
    Buffer empty_moved(std::move(empty));
    Check(empty, "");
    Check(empty_moved, "");
    Buffer source;
    source.WriteString("xy");
    target.Clear();
    target.WriteBuffer(source);
    Check(source, "xy");
    Check(target, "xy");
}

void SelfAppend()
{
    for (std::size_t size : {0u, 3u, 8u, 32u})
    {
        Buffer b(size);
        b.WriteString("abcdef");
        b.MoveReadOffset(2);
        b.WriteBuffer(b);
        Check(b, "cdefcdef");
    }
    Buffer empty(0);
    empty.WriteBuffer(empty);
    Check(empty, "");
}

void FindLFTests()
{
    Buffer b(0);
    const Buffer &view = b;
    CHECK(view.FindLF() == nullptr);
    b.WriteString("abc\r");
    CHECK(view.FindLF() == nullptr);
    b.WriteString("\n\n");
    CHECK(view.FindLF() == b.GetReadPosition() + 4);
    CHECK(view.FindCRLF() == b.GetReadPosition() + 3);
    CHECK(b.GetReadableSize() == 6);
    b.MoveReadOffset(5);
    CHECK(view.FindLF() == b.GetReadPosition());
    CHECK(view.FindCRLF() == nullptr);
    b.MoveReadOffset(1);
    CHECK(view.FindLF() == nullptr);
    const char data[] = {'a', '\0', '\n', 'b'};
    b.Write(data, sizeof(data));
    CHECK(view.FindLF() == b.GetReadPosition() + 2);
    b.MoveReadOffset(3);
    CHECK(view.FindLF() == nullptr); // 已消费区中的换行不参与查找。
    b.EnsureWritableSize(1);
    b.GetWritePosition()[0] = '\n';
    CHECK(view.FindLF() == nullptr); // 尚未提交的数据不参与查找。
    b.MoveWriteOffset(1);
    CHECK(view.FindLF() == b.GetReadPosition() + 1);
}

void LineModes()
{
    using Mode = Buffer::LineMode;
    Buffer b(0);
    const Buffer &view = b;
    CHECK(view.PeekLine(Mode::LF).empty());
    CHECK(b.ReadLine(Mode::LF).empty());
    b.WriteString("a\nb\r\n\nend");
    CHECK(view.PeekLine() == "a\nb\r\n");
    CHECK(view.PeekLine(Mode::CRLF) == view.PeekLine());
    CHECK(view.PeekLine(Mode::LF) == "a\n");
    CHECK(b.GetReadableSize() == 9);
    CHECK(b.ReadLine(Mode::LF) == "a\n");
    CHECK(b.ReadLine(Mode::LF) == "b\r\n");
    CHECK(b.ReadLine(Mode::LF) == "\n");
    CHECK(b.ReadLine(Mode::LF).empty());
    CHECK(b.ReadLine().empty());
    CHECK(b.PeekAsString(3) == "end");
    b.WriteString("\r");
    CHECK(b.ReadLine().empty());
    CHECK(b.ReadLine(Mode::LF).empty());
    b.WriteString("\n");
    CHECK(view.PeekLine(Mode::LF) == "end\r\n");
    CHECK(b.ReadLine() == "end\r\n");
    b.WriteString("\r\n");
    CHECK(b.ReadLine(Mode::CRLF) == "\r\n");
    const char raw[] = {'a', '\0', '\n'};
    b.Write(raw, sizeof(raw));
    CHECK(b.ReadLine(Mode::LF) == std::string(raw, sizeof(raw)));
    CHECK(b.GetReadableSize() == 0);
}

void RandomModel()
{
    for (unsigned seed = 0; seed < 16; ++seed)
    {
        std::mt19937 random(seed);
        Buffer b(seed % 4);
        std::string model;
        for (unsigned step = 0; step < 5000; ++step)
        {
            unsigned op = random() % 10;
            const std::size_t count = random() % (model.size() + 1);
            if (model.size() > 4096)
            {
                b.Clear();
                model.clear();
                Check(b, model);
                continue;
            }
            if (op < 3)
            {
                std::string data(random() % 32, '\0');
                for (char &c : data)
                {
                    unsigned x = random() % 8;
                    c = x == 0 ? '\r' : x == 1 ? '\n' : static_cast<char>(random() % 256);
                }
                if (op == 0)
                    b.WriteString(data);
                else if (op == 1)
                {
                    Buffer other;
                    other.WriteString(data);
                    b.WriteBuffer(other);
                    Check(other, data);
                }
                else
                {
                    b.EnsureWritableSize(data.size());
                    if (!data.empty())
                        std::memcpy(b.GetWritePosition(), data.data(), data.size());
                    b.MoveWriteOffset(data.size());
                }
                model += data;
            }
            else if (op == 3)
            {
                CHECK(b.ReadAsString(count) == model.substr(0, count));
                model.erase(0, count);
            }
            else if (op == 4)
            {
                b.MoveReadOffset(count);
                model.erase(0, count);
            }
            else if (op == 5)
            {
                const std::size_t end = model.find("\r\n");
                const std::size_t n = end == std::string::npos ? 0 : end + 2;
                CHECK(b.ReadLine() == model.substr(0, n));
                model.erase(0, n);
            }
            else if (op == 6)
            {
                b.EnsureWritableSize(random() % 128);
            }
            else if (op == 7)
            {
                std::string out(count, '\0');
                b.Read(count ? &out[0] : nullptr, count);
                CHECK(out == model.substr(0, count));
                model.erase(0, count);
            }
            else if (op == 8)
            {
                b.WriteBuffer(b);
                model += std::string(model);
            }
            else
            {
                Buffer copy = b;
                Buffer moved = std::move(copy);
                Check(copy, "");
                b = std::move(moved);
                Check(moved, "");
            }
            Check(b, model);
        }
    }
}

void StreamOutput()
{
    Buffer b(2);
    // 空缓冲区不写入任何字节，可安全参与链式输出。
    std::ostringstream empty;
    empty << b << '|';
    CHECK(empty.str() == "|");
    CHECK(b.GetReadableSize() == 0);

    const std::string content("hel\0lo\r\n", 8); // 含内嵌空字符与 CRLF
    b.WriteString(content);

    std::ostringstream out;
    out << b;
    CHECK(out.str() == content); // 按字节原样输出，保留内嵌空字符
    CHECK(b.GetReadableSize() == content.size());
    CHECK(b.PeekAsString(content.size()) == content); // 不消费数据、不改变偏移

    CHECK(b.ReadLine() == content); // 输出操作后读偏移仍然可用
    std::ostringstream tail;
    tail << b << b;
    CHECK(tail.str().empty()); // 数据已消费完，重新输出为空
}

int main(int argc, char **argv)
{
    try
    {
        const std::string mode = argc > 1 ? argv[1] : "all";
        if (mode == "all" || mode == "base")
        {
            Basics();
            Space();
            Errors();
            FindLFTests();
            LineModes();
        }
        if (mode == "all" || mode == "stream")
            StreamOutput();
        if (mode == "all" || mode == "move")
            CopiesAndMoves();
        if (mode == "all" || mode == "self")
            SelfAppend();
        if (mode == "all" || mode == "random")
            RandomModel();
        std::cout << "Buffer comprehensive tests passed: " << mode << '\n';
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
