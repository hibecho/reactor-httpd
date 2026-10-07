// Util 模块测试：Split / ReadFile / WriteFile / UrlEncode / UrlDecode /
// StatusDescription / MimeType / IsDirectory / IsRegularFile。
//
// ResolveResourcePath 的边界矩阵由 TestResourcePath.cc 承担，这里只留冒烟用例。
//
// 每个用例组独立执行并单独上报，避免第一处失败掩盖其余缺陷。

#include "base/Buffer.hpp"
#include "base/Util.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #expr +              \
                                     " errno=" + std::to_string(errno));                                               \
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

namespace
{
    // 看门狗：ReadFile 的分块读循环一旦失控就是永久阻塞，必须留下可定位的失败痕迹。
    // 处理器只做异步信号安全的 write 与 _exit，直接退出的默认动作无法说明卡在哪。
    void OnAlarm(int)
    {
        const char message[] = "TIMEOUT: 30 秒内未完成\n";
        const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)ignored;
        _exit(EXIT_FAILURE);
    }

    void ArmWatchdog(unsigned seconds)
    {
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = OnAlarm;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (sigaction(SIGALRM, &action, nullptr) != 0)
        {
            throw std::runtime_error("sigaction failed");
        }
        alarm(seconds);
    }

    // 临时目录随用例结束整体删除，与 TestResourcePath.cc 的夹具一致。
    struct Fixture
    {
        fs::path base;
        Fixture()
        {
            char pattern[] = "/tmp/http-util-XXXXXX";
            const char *directory = ::mkdtemp(pattern);
            if (!directory)
                throw std::runtime_error("mkdtemp failed");
            base = directory;
        }
        ~Fixture()
        {
            std::error_code ec;
            fs::remove_all(base, ec);
        }
    };

    // 断言各组元素内容，而不是拿 initializer_list 直接比较：宏实参里的逗号会被预处理器切开。
    void ExpectParts(const std::vector<std::string> &actual, const std::vector<std::string> &expected)
    {
        CHECK(actual.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            CHECK(actual[i] == expected[i]);
        }
    }

    std::string Dump(const Buffer &buffer)
    {
        return buffer.PeekAsString(buffer.GetReadableSize());
    }

    // 独立于 Util 的落盘与读取，避免 ReadFile 的用例依赖 WriteFile 是否正确。
    void WriteRaw(const fs::path &path, std::string_view content)
    {
        std::ofstream ofs(path, std::ios::binary);
        CHECK(ofs.is_open());
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        CHECK(static_cast<bool>(ofs));
    }

    std::string ReadRaw(const fs::path &path)
    {
        std::ifstream ifs(path, std::ios::binary);
        CHECK(ifs.is_open());
        return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    // 0x00-0xFF 全覆盖，用于验证二进制安全与高位字节不被符号扩展。
    std::string AllByteValues()
    {
        std::string bytes;
        bytes.reserve(256);
        for (int value = 0; value <= 255; ++value)
        {
            bytes += static_cast<char>(value);
        }
        return bytes;
    }

    std::string RandomBytes(std::size_t size, unsigned seed)
    {
        std::mt19937 generator(seed);
        std::uniform_int_distribution<int> distribution(0, 255);
        std::string bytes(size, '\0');
        for (char &ch : bytes)
        {
            ch = static_cast<char>(distribution(generator));
        }
        return bytes;
    }

    // 同一个循环里重复断言时行号无法定位是哪个输入出错，消息里带上输入内容。
    // 同时校验失败路径不修改输出：UrlDecode 先构造局部结果，只有成功才 swap。
    void ExpectRejectedEscape(std::string_view input)
    {
        std::string output = "sentinel";
        if (Util::UrlDecode(input, output))
        {
            throw std::runtime_error("accepted malformed escape: \"" + std::string(input) + "\"");
        }
        if (output != "sentinel")
        {
            throw std::runtime_error("output modified for rejected escape: \"" + std::string(input) + "\"");
        }
    }

    template <class F> void Run(const char *name, F test, int &failures)
    {
        try
        {
            test();
            std::cout << "PASS " << name << "\n";
        }
        catch (const std::exception &error)
        {
            std::cerr << "FAIL " << name << ": " << error.what() << "\n";
            ++failures;
        }
        catch (...)
        {
            std::cerr << "FAIL " << name << ": unknown exception\n";
            ++failures;
        }
    }
} // namespace

void TestSplit()
{
    std::vector<std::string> parts;

    const std::string csv = "a,b,c";
    CHECK(Util::Split(csv, ",", parts) == 3);
    ExpectParts(parts, {"a", "b", "c"});
    CHECK(parts.size() == 3);

    // 连续分隔符按 keep_empty 决定是否保留空段。
    const std::string sparse = "a,,b";
    CHECK(Util::Split(sparse, ",", parts) == 2);
    ExpectParts(parts, {"a", "b"});
    CHECK(Util::Split(sparse, ",", parts, true) == 3);
    ExpectParts(parts, {"a", "", "b"});

    // 前导与尾随分隔符。
    const std::string padded = ",a,";
    CHECK(Util::Split(padded, ",", parts) == 1);
    ExpectParts(parts, {"a"});
    CHECK(Util::Split(padded, ",", parts, true) == 3);
    ExpectParts(parts, {"", "a", ""});

    // 复用同一个 vector 时必须先清空。
    parts.emplace_back("stale");
    CHECK(Util::Split(csv, ",", parts) == 3);
    ExpectParts(parts, {"a", "b", "c"});

    // 未命中分隔符时整串作为唯一元素。
    const std::string single = "abc";
    CHECK(Util::Split(single, ",", parts) == 1);
    ExpectParts(parts, {"abc"});

    // 空输入：不保留空段则为空结果，保留则为单个空串。
    const std::string empty;
    CHECK(Util::Split(empty, ",", parts) == 0);
    ExpectParts(parts, {});
    CHECK(Util::Split(empty, ",", parts, true) == 1);
    ExpectParts(parts, {""});

    // 只由分隔符组成的输入。
    const std::string only = ",";
    CHECK(Util::Split(only, ",", parts) == 0);
    ExpectParts(parts, {});
    CHECK(Util::Split(only, ",", parts, true) == 2);
    ExpectParts(parts, {"", ""});

    // 多字符分隔符。
    const std::string scoped = "a::b::c";
    CHECK(Util::Split(scoped, "::", parts) == 3);
    ExpectParts(parts, {"a", "b", "c"});

    // 分隔符比剩余内容长时不命中。
    const std::string tail = "a-";
    CHECK(Util::Split(tail, "::", parts) == 1);
    ExpectParts(parts, {"a-"});

    // 分隔符等于整个输入。
    CHECK(Util::Split(single, "abc", parts, true) == 2);
    ExpectParts(parts, {"", ""});

    // 从左到右不重叠扫描："aaa" 以 "aa" 切分得到 {"a"}。
    const std::string repeated = "aaa";
    CHECK(Util::Split(repeated, "aa", parts) == 1);
    ExpectParts(parts, {"a"});

    // 路由场景：路径分隔符。
    const std::string route = "/a//b/";
    CHECK(Util::Split(route, "/", parts) == 2);
    ExpectParts(parts, {"a", "b"});

    // 内嵌 NUL 仍按字节切分。
    const std::string with_nul("a\0b,c", 5);
    CHECK(Util::Split(with_nul, ",", parts) == 2);
    ExpectParts(parts, {std::string("a\0b", 3), "c"});

    // 空分隔符是调用错误。
    Throws<std::invalid_argument>([&] {
        Util::Split(csv, "", parts);
    });
}

void TestUrlEncode()
{
    // 未保留字符集之外的可见字符原样输出。
    const std::string unreserved = "AZaz09-_.~";
    CHECK(Util::UrlEncode(unreserved) == unreserved);

    // 空格的两种策略。
    const std::string spaced = "a b";
    CHECK(Util::UrlEncode(spaced) == "a%20b");
    CHECK(Util::UrlEncode(spaced, true) == "a+b");
    // space_as_plus 只影响空格。
    const std::string slashed = "a/b";
    CHECK(Util::UrlEncode(slashed, true) == "a%2Fb");

    // 查询串与路径中的保留字符都要编码。
    const std::string delimiters = "/?&=+%#:";
    CHECK(Util::UrlEncode(delimiters) == "%2F%3F%26%3D%2B%25%23%3A");

    // 控制字符与内嵌 NUL。
    CHECK(Util::UrlEncode("\n") == "%0A");
    CHECK(Util::UrlEncode("\r") == "%0D");
    CHECK(Util::UrlEncode("\t") == "%09");
    CHECK(Util::UrlEncode(std::string_view("\0", 1)) == "%00");

    // 十六进制固定为大写，小写字母本身属于未保留字符。
    CHECK(Util::UrlEncode("~") == "~");
    CHECK(Util::UrlEncode(std::string_view("\x1a", 1)) == "%1A");

    // 高位字节按 unsigned char 处理，不能因符号扩展输出成 %FFFFFF80 之类。
    CHECK(Util::UrlEncode(std::string_view("\x80", 1)) == "%80");
    CHECK(Util::UrlEncode(std::string_view("\xFF", 1)) == "%FF");

    // UTF-8 多字节逐字节编码。
    CHECK(Util::UrlEncode("中") == "%E4%B8%AD");

    // 完整 URL 不做特殊处理，协议与路径分隔符同样被编码。
    CHECK(Util::UrlEncode("http://x/y?a=1") == "http%3A%2F%2Fx%2Fy%3Fa%3D1");

    CHECK(Util::UrlEncode("").empty());

    // 输出长度上界：每个字节最多 3 个字符。
    const std::string bytes = AllByteValues();
    CHECK(Util::UrlEncode(bytes).size() <= 3 * bytes.size());
}

void TestUrlDecode()
{
    std::string output = "sentinel";

    CHECK(Util::UrlDecode("a%20b", output));
    CHECK(output == "a b");

    // 十六进制不区分大小写。
    CHECK(Util::UrlDecode("%2f", output));
    CHECK(output == "/");
    CHECK(Util::UrlDecode("%2F", output));
    CHECK(output == "/");
    CHECK(Util::UrlDecode("%e4%b8%ad", output));
    CHECK(output == "中");

    // 未编码字符原样保留。
    const std::string plain = "plain/path?a=1&b=2";
    CHECK(Util::UrlDecode(plain, output));
    CHECK(output == plain);

    // 默认 plus_as_space=false 时 '+' 是普通字符。
    CHECK(Util::UrlDecode("a+b", output));
    CHECK(output == "a+b");
    CHECK(Util::UrlDecode("a+b", output, true));
    CHECK(output == "a b");
    CHECK(Util::UrlDecode("name=John+Doe", output, true));
    CHECK(output == "name=John Doe");
    // plus_as_space=true 只把 '+' 变成空格，其余字符不受影响。
    CHECK(Util::UrlDecode("abc", output, true));
    CHECK(output == "abc");
    CHECK(Util::UrlDecode("a%2Fb", output, true));
    CHECK(output == "a/b");

    // %00 解出内嵌 NUL。
    CHECK(Util::UrlDecode("%00", output));
    CHECK(output.size() == 1);
    CHECK(output[0] == '\0');

    CHECK(Util::UrlDecode("", output));
    CHECK(output.empty());

    // 十六进制位只允许 0-9 与 a-f；先钉住合法边界，避免修复时把范围收得过窄。
    const std::pair<std::string_view, std::string> valid_escapes[] = {
        {"%00", std::string(1, '\0')},
        {"%09", "\t"},
        {"%0a", "\n"},
        {"%1A", std::string(1, '\x1a')},
        {"%AF", std::string(1, '\xaf')},
        {"%af", std::string(1, '\xaf')},
        {"%FF", std::string(1, '\xff')},
    };
    for (const auto &[escape, expected] : valid_escapes)
    {
        CHECK(Util::UrlDecode(escape, output));
        CHECK(output == expected);
    }

    // 非法转义必须拒绝，且失败时 output 保持原值。
    // g-z / G-Z 不是十六进制位，%G0 曾解出 NUL 字节、%zz 解出 '3'。
    const char *const malformed[] = {"%", "%4", "%2", "a%4", "abc%", "%G0", "%zz", "%1Z", "%g0", "%a", "%%41"};
    for (const char *const input : malformed)
    {
        ExpectRejectedEscape(input);
    }

    // 视图不以空字符结尾时，末尾残缺的转义同样只能看到 1 个字符。
    ExpectRejectedEscape(std::string_view("A%4B", 3));

    // 编码与解码往返，覆盖全部字节值。
    {
        const std::string original = RandomBytes(1024, 7);
        std::string decoded;
        CHECK(Util::UrlDecode(Util::UrlEncode(original), decoded));
        CHECK(decoded == original);
    }

    // 表单式往返：空格编码为 '+'，字面 '+' 编码为 %2B。
    {
        const std::string original = "a b+c";
        std::string decoded;
        CHECK(Util::UrlDecode(Util::UrlEncode(original, true), decoded, true));
        CHECK(decoded == original);
    }
}

void TestStatusDescription()
{
    CHECK(Util::StatusDescription(100) == "Continue");
    CHECK(Util::StatusDescription(101) == "Switching Protocols");
    CHECK(Util::StatusDescription(200) == "OK");
    CHECK(Util::StatusDescription(201) == "Created");
    CHECK(Util::StatusDescription(204) == "No Content");
    CHECK(Util::StatusDescription(206) == "Partial Content");
    CHECK(Util::StatusDescription(301) == "Moved Permanently");
    CHECK(Util::StatusDescription(304) == "Not Modified");
    CHECK(Util::StatusDescription(404) == "Not Found");
    CHECK(Util::StatusDescription(405) == "Method Not Allowed");
    CHECK(Util::StatusDescription(414) == "URI Too Long");
    CHECK(Util::StatusDescription(429) == "Too Many Requests");
    CHECK(Util::StatusDescription(500) == "Internal Server Error");
    CHECK(Util::StatusDescription(501) == "Not Implemented");
    CHECK(Util::StatusDescription(505) == "HTTP Version Not Supported");

    // 未登记的状态码统一为 "Unknown"。
    const int unknown[] = {0, 99, 199, 218, 300, 599, 999, -1};
    for (const int status : unknown)
    {
        CHECK(Util::StatusDescription(status) == "Unknown");
    }

    // 返回值指向静态存储，重复调用地址稳定。
    CHECK(Util::StatusDescription(200).data() == Util::StatusDescription(200).data());
}

void TestAsciiHelpers()
{
    // IsToken 的合法字符集：字母、数字与这 12 个特殊符号。逐个字节穷举 0x00..0xff，
    // 高位字节会被误判成合法 token 正是这类实现最容易出的错。
    const std::string allowed = "!#$%&'*+-.^_`|~";
    for (int code = 0; code < 256; ++code)
    {
        const char byte = static_cast<char>(code);
        const bool alnum = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
        const bool special = allowed.find(byte) != std::string::npos;
        CHECK(Util::IsToken(std::string(1, byte)) == (alnum || special));
    }
    CHECK(!Util::IsToken(""));
    CHECK(!Util::IsToken(" "));
    CHECK(!Util::IsToken("a b"));
    CHECK(!Util::IsToken("a\r\nb"));
    CHECK(!Util::IsToken("a:b"));
    CHECK(Util::IsToken("GET"));
    CHECK(Util::IsToken("Content-Type"));
    CHECK(Util::IsToken(allowed));

    // ToLowerAscii 只折叠 'A'-'Z'，其余字节（含高位字节）原样保留。
    CHECK(Util::ToLowerAscii("") == "");
    CHECK(Util::ToLowerAscii("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "abcdefghijklmnopqrstuvwxyz");
    CHECK(Util::ToLowerAscii("Hello World 123") == "hello world 123");
    CHECK(Util::ToLowerAscii(allowed) == allowed);
    const char high = static_cast<char>(0xc0);
    CHECK(Util::ToLowerAscii(std::string(1, high) + "A") == std::string(1, high) + "a");

    // EqualsIgnoreCaseAscii：长度不同直接不等，非 ASCII 字节不参与折叠。
    CHECK(Util::EqualsIgnoreCaseAscii("", ""));
    CHECK(Util::EqualsIgnoreCaseAscii("Host", "HOST"));
    CHECK(Util::EqualsIgnoreCaseAscii("hOsT", "HoSt"));
    CHECK(!Util::EqualsIgnoreCaseAscii("", "a"));
    CHECK(!Util::EqualsIgnoreCaseAscii("hosts", "host"));
    CHECK(!Util::EqualsIgnoreCaseAscii("host", "hoss"));
    CHECK(!Util::EqualsIgnoreCaseAscii("Host", "Hust"));
    CHECK(Util::EqualsIgnoreCaseAscii(std::string(1, high), std::string(1, high)));
    CHECK(!Util::EqualsIgnoreCaseAscii(std::string(1, high), std::string(1, static_cast<char>(0xe0))));
}

void TestMimeType()
{
    // 返回值先落到 std::string 再比较，避免把比较过程本身的不确定性当成断言结果。
    const auto mime = [](std::string_view name) {
        return std::string(Util::MimeType(name));
    };

    CHECK(mime("index.html") == "text/html");
    CHECK(mime("index.htm") == "text/html");
    CHECK(mime("style.css") == "text/css");
    CHECK(mime("app.js") == "text/javascript");
    CHECK(mime("notes.txt") == "text/plain");
    CHECK(mime("table.csv") == "text/csv");
    CHECK(mime("data.json") == "application/json");
    CHECK(mime("feed.xml") == "application/xml");
    CHECK(mime("doc.pdf") == "application/pdf");
    CHECK(mime("pack.zip") == "application/zip");
    CHECK(mime("mod.wasm") == "application/wasm");
    CHECK(mime("photo.jpg") == "image/jpeg");
    CHECK(mime("photo.jpeg") == "image/jpeg");
    CHECK(mime("icon.png") == "image/png");
    CHECK(mime("anim.gif") == "image/gif");
    CHECK(mime("pic.webp") == "image/webp");
    CHECK(mime("logo.svg") == "image/svg+xml");
    CHECK(mime("sound.mp3") == "audio/mpeg");
    CHECK(mime("clip.mp4") == "video/mp4");
    CHECK(mime("font.woff") == "font/woff");
    CHECK(mime("font.woff2") == "font/woff2");

    // 扩展名大小写不敏感，结果统一为规范写法。
    CHECK(mime("INDEX.HTML") == "text/html");
    CHECK(mime("Logo.SvG") == "image/svg+xml");
    CHECK(mime("PHOTO.JPG") == "image/jpeg");
    CHECK(mime("README.HTML") == "text/html");

    // 目录部分不参与匹配。
    CHECK(mime("/var/www/site/logo.png") == "image/png");
    CHECK(mime("./style.css") == "text/css");
    CHECK(mime("../a/b.js") == "text/javascript");
    CHECK(mime("C:\\www\\logo.png") == "image/png");
    CHECK(mime("dir/.hidden.txt") == "text/plain");
    // 目录名里的点号不构成扩展名。
    CHECK(mime("/srv/v1.2/index") == "application/octet-stream");
    CHECK(mime("dir.with.dots/plain") == "application/octet-stream");

    // 无扩展名、纯隐藏文件、以点结尾、空文件名都退化为二进制流。
    CHECK(mime("README") == "application/octet-stream");
    CHECK(mime("Makefile") == "application/octet-stream");
    CHECK(mime(".gitignore") == "application/octet-stream");
    CHECK(mime(".env") == "application/octet-stream");
    CHECK(mime("file.") == "application/octet-stream");
    CHECK(mime("") == "application/octet-stream");
    CHECK(mime("dir.with.dots/") == "application/octet-stream");

    // 未登记的扩展名退化为二进制流，取最后一个点号之后的部分。
    CHECK(mime("archive.bin") == "application/octet-stream");
    CHECK(mime("archive.tar.gz") == "application/octet-stream");
    CHECK(mime("file.this_extension_is_far_too_long") == "application/octet-stream");

    // 接口原貌：直接以 string_view 比较已知映射。
    CHECK(Util::MimeType("index.html") == "text/html");
    CHECK(Util::MimeType("LICENSE") == "application/octet-stream");
}

void TestPathKinds(const fs::path &base)
{
    const fs::path directory = base / "kind-dir";
    const fs::path file = base / "kind-file";
    const fs::path missing = base / "kind-missing";
    const fs::path fifo = base / "kind-fifo";

    fs::create_directory(directory);
    WriteRaw(file, "x");
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    fs::create_symlink(missing, base / "kind-dangling");
    fs::create_directory_symlink(directory, base / "kind-dir-link");
    fs::create_symlink(file, base / "kind-file-link");

    CHECK(Util::IsDirectory(directory));
    CHECK(!Util::IsRegularFile(directory));

    CHECK(Util::IsRegularFile(file));
    CHECK(!Util::IsDirectory(file));

    // 不存在的路径对两者都为假。
    CHECK(!Util::IsDirectory(missing));
    CHECK(!Util::IsRegularFile(missing));

    // 悬垂符号链接按目标不存在处理，不因链接本身存在而返回真。
    CHECK(!Util::IsDirectory(base / "kind-dangling"));
    CHECK(!Util::IsRegularFile(base / "kind-dangling"));

    // 有效符号链接跟随目标。
    CHECK(Util::IsDirectory(base / "kind-dir-link"));
    CHECK(!Util::IsRegularFile(base / "kind-dir-link"));
    CHECK(Util::IsRegularFile(base / "kind-file-link"));
    CHECK(!Util::IsDirectory(base / "kind-file-link"));

    // FIFO 既不是目录也不是普通文件。
    CHECK(!Util::IsDirectory(fifo));
    CHECK(!Util::IsRegularFile(fifo));

    // 空路径不抛异常，返回假。
    CHECK(!Util::IsDirectory(fs::path()));
    CHECK(!Util::IsRegularFile(fs::path()));
}

void TestReadFile(const fs::path &base)
{
    const fs::path content_path = base / "read-content.bin";
    WriteRaw(content_path, "line1\r\nline2");

    // 空指针与不存在的文件。
    CHECK(!Util::ReadFile(content_path, nullptr));
    Buffer guarded;
    guarded.WriteString("keep");
    CHECK(!Util::ReadFile(base / "read-missing.bin", &guarded));
    CHECK(Dump(guarded) == "keep"); // 失败不触碰缓冲区

    // 普通文本内容按字节保留（不做换行转换）。
    Buffer buffer;
    CHECK(Util::ReadFile(content_path, &buffer));
    CHECK(Dump(buffer) == "line1\r\nline2");

    // 追加语义：不清空已有可读数据，也不消费它。
    CHECK(Util::ReadFile(content_path, &buffer));
    CHECK(Dump(buffer) == "line1\r\nline2line1\r\nline2");

    // 空文件成功且不产生数据。
    const fs::path empty_path = base / "read-empty.bin";
    WriteRaw(empty_path, "");
    Buffer empty_read;
    CHECK(Util::ReadFile(empty_path, &empty_read));
    CHECK(empty_read.GetReadableSize() == 0);

    // 分块读取的边界：8 KiB 块的前后各一字节。
    const std::size_t chunk_sizes[] = {1, 8191, 8192, 8193, 16384, 16385};
    for (const std::size_t size : chunk_sizes)
    {
        const std::string content(size, 'x');
        const fs::path path = base / ("read-chunk-" + std::to_string(size) + ".bin");
        WriteRaw(path, content);
        Buffer read;
        CHECK(Util::ReadFile(path, &read));
        CHECK(read.GetReadableSize() == size);
        CHECK(Dump(read) == content);
    }

    // 二进制安全：全部 256 种字节值，含内嵌 NUL。
    const std::string bytes = AllByteValues();
    const fs::path bytes_path = base / "read-bytes.bin";
    WriteRaw(bytes_path, bytes);
    Buffer byte_read;
    CHECK(Util::ReadFile(bytes_path, &byte_read));
    CHECK(byte_read.GetReadableSize() == 256);
    CHECK(Dump(byte_read) == bytes);

    // 符号链接跟随目标。
    Buffer linked;
    CHECK(!Util::ReadFile(base / "read-link.bin", &linked)); // 链接尚未创建
    fs::create_symlink(content_path, base / "read-link.bin");
    CHECK(Util::ReadFile(base / "read-link.bin", &linked));
    CHECK(Dump(linked) == "line1\r\nline2");

    // 目录不是可读的文件内容。
    Buffer directory_read;
    CHECK(!Util::ReadFile(base, &directory_read));
    CHECK(directory_read.GetReadableSize() == 0);

    // 大文件与 WriteFile 往返一致。
    const std::string large = RandomBytes(1024 * 1024, 11);
    const fs::path large_path = base / "read-large.bin";
    CHECK(Util::WriteFile(large_path, large));
    Buffer large_read;
    CHECK(Util::ReadFile(large_path, &large_read));
    CHECK(Dump(large_read) == large);
}

void TestWriteFile(const fs::path &base)
{
    const fs::path path = base / "write-content.bin";

    CHECK(Util::WriteFile(path, "first version"));
    CHECK(ReadRaw(path) == "first version");

    // 覆盖写入：短内容必须截断掉旧数据。
    CHECK(Util::WriteFile(path, "short"));
    CHECK(ReadRaw(path) == "short");

    // 空内容清空文件。
    CHECK(Util::WriteFile(path, ""));
    CHECK(fs::is_regular_file(path));
    CHECK(fs::file_size(path) == 0);

    // 二进制内容与内嵌 NUL，且不做换行转换。
    const std::string bytes = AllByteValues();
    CHECK(Util::WriteFile(path, bytes));
    CHECK(ReadRaw(path) == bytes);

    const std::string crlf = "a\r\nb";
    CHECK(Util::WriteFile(path, crlf));
    CHECK(ReadRaw(path) == crlf);
    CHECK(fs::file_size(path) == crlf.size());

    // 父目录不存在：失败且不产生文件。
    const fs::path missing_parent = base / "write-no-such-dir" / "file.bin";
    CHECK(!Util::WriteFile(missing_parent, "x"));
    CHECK(!fs::exists(missing_parent));
    CHECK(!fs::exists(missing_parent.parent_path()));

    // 目标是目录：失败。
    CHECK(!Util::WriteFile(base, "x"));
    CHECK(fs::is_directory(base));

    // 父路径是文件：失败。
    const fs::path under_file = base / "write-parent-file" / "file.bin";
    CHECK(Util::WriteFile(base / "write-parent-file", "x"));
    CHECK(!Util::WriteFile(under_file, "x"));
    CHECK(ReadRaw(base / "write-parent-file") == "x");

    // 覆盖已有文件的权限与大小。
    const fs::path sized = base / "write-sized.bin";
    CHECK(Util::WriteFile(sized, std::string(4096, 'y')));
    CHECK(fs::file_size(sized) == 4096);
    CHECK(Util::WriteFile(sized, std::string(10, 'z')));
    CHECK(fs::file_size(sized) == 10);

    // 写入后由 ReadFile 读回，1 MiB 随机数据往返一致。
    const std::string large = RandomBytes(1024 * 1024, 13);
    const fs::path large_path = base / "write-large.bin";
    CHECK(Util::WriteFile(large_path, large));
    Buffer read;
    CHECK(Util::ReadFile(large_path, &read));
    CHECK(Dump(read) == large);
}

// ResolveResourcePath 的完整边界矩阵在 TestResourcePath.cc，这里只做冒烟校验。
void TestResolveResourcePathSmoke(const fs::path &base)
{
    const fs::path root = base / "www-smoke";
    fs::create_directories(root / "assets");
    WriteRaw(root / "index.html", "hi");

    fs::path resolved;
    CHECK(Util::ResolveResourcePath(root, "/index.html", resolved));
    CHECK(resolved == root / "index.html");
    CHECK(Util::ResolveResourcePath(root, "/assets/../index.html", resolved));
    CHECK(resolved == root / "index.html");

    resolved = "unchanged";
    CHECK(!Util::ResolveResourcePath(root, "/../secret", resolved));
    CHECK(resolved == "unchanged");
    CHECK(!Util::ResolveResourcePath(root, "/missing.txt", resolved));
    CHECK(!Util::ResolveResourcePath(root, "index.html", resolved));
}

int main()
{
    int failures = 0;
    try
    {
        ArmWatchdog(30);

        Fixture fixture;
        Run("split", TestSplit, failures);
        Run("urlencode", TestUrlEncode, failures);
        Run("urldecode", TestUrlDecode, failures);
        Run("status", TestStatusDescription, failures);
        Run("ascii", TestAsciiHelpers, failures);
        Run("mimetype", TestMimeType, failures);
        Run(
            "readfile",
            [&] {
                TestReadFile(fixture.base);
            },
            failures);
        Run(
            "writefile",
            [&] {
                TestWriteFile(fixture.base);
            },
            failures);
        Run(
            "pathkinds",
            [&] {
                TestPathKinds(fixture.base);
            },
            failures);
        Run(
            "resolve-smoke",
            [&] {
                TestResolveResourcePathSmoke(fixture.base);
            },
            failures);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL setup: " << error.what() << "\n";
        return 1;
    }

    if (failures != 0)
    {
        std::cerr << failures << " case group(s) failed\n";
        return 1;
    }
    std::cout << "Util tests passed\n";
    return 0;
}
