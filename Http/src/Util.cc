#include "base/Util.hpp"
#include "base/Buffer.hpp"
#include "base/Logger.hpp"
#include <iostream>
#include <string>
#include <unordered_map>

// 字符串分割函数
size_t Util::Split(std::string_view input, std::string_view delimiter, std::vector<std::string> &result,
                   bool keep_empty)
{
    if (delimiter.empty())
    {
        throw std::invalid_argument("Split: delimiter is empty");
    }
    result.clear();
    std::size_t begin = 0;

    while (true)
    {
        // 1.分割符的位置
        const std::size_t end = input.find(delimiter, begin);
        // 2.截取字符串
        const auto part = input.substr(begin, end == std::string_view::npos ? input.size() - begin : end - begin);
        // 3.将截取的字符串保留到result中
        if (keep_empty || !part.empty())
        {
            result.emplace_back(part);
        }
        // 退出
        if (end == std::string_view::npos)
        {
            break;
        }
        // 4.更新起始位置
        begin = end + delimiter.size();
    }
    return result.size();
}

// 读取文件中的全部内容，支持二进制数据。
// 内容追加到 buffer 现有可读数据之后，不消费已有数据；
// buffer 为空指针、打开或读取失败时返回 false，此时缓冲区不被修改。
bool Util::ReadFile(const std::filesystem::path &path, Buffer *buffer)
{
    if (buffer == nullptr)
    {
        LOG_ERROR("ReadFile: buffer is null");
        return false;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open())
    {
        LOG_ERROR("open {} file failed", path.string());
        return false;
    }

    std::string content;
    char chunk[8192];
    while (true)
    {
        ifs.read(chunk, sizeof(chunk));
        const auto count = ifs.gcount();

        if (count > 0)
        {
            content.append(chunk, static_cast<std::size_t>(count));
        }

        if (ifs.bad())
        {
            LOG_ERROR("read {} file failed", path.string());
            return false;
        }

        if (ifs.eof())
        {
            break;
        }

        if (ifs.fail())
        {
            LOG_ERROR("read {} file failed", path.string());
            return false;
        }
    }

    buffer->Write(content.data(), content.size());
    return true;
}

// 覆盖写入；不自动创建父目录。
bool Util::WriteFile(const std::filesystem::path &path, std::string_view content)
{
    // write 接收 streamsize，转换前检查长度。
    const auto max_size = static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)());
    if (content.size() > max_size)
    {
        LOG_ERROR("file content too large: {}", path.string());
        return false;
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open())
    {
        LOG_ERROR("open {} file failed", path.string());
        return false;
    }

    if (!content.empty())
    {
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));

        if (!ofs)
        {
            LOG_ERROR("write {} file failed", path.string());
            return false;
        }
    }
    ofs.close();

    if (ofs.fail())
    {
        LOG_ERROR("close {} file failed", path.string());
        return false;
    }
    return true;
}

// 编码单个 URL 组成部分，不对完整 URL 直接编码;
// 将特殊的ASCII值，转换为两个16进制的字符%XX;
// 让字符串可以安全地作为 URL 中的一个参数值或路径片段。
// 按字节进行编码：保留字母、数字和 - _ . ~，其余字节转换成 %XX。
// input:为输入的带转换字符串 space_as_plus: 是否需要将空格转换为 +
std::string Util::UrlEncode(std::string_view input, bool space_as_plus)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(input.size());
    for (unsigned char ch : input)
    {
        const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                                ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved)
        {
            result += static_cast<char>(ch);
        }
        else if (ch == ' ' && space_as_plus)
        {
            result += '+';
        }
        else
        {
            result += '%';
            // 获取高四位
            result += hex[ch >> 4];
            // 获取低四位
            result += hex[ch & 0x0F];
        }
    }
    return result;
}

bool Util::UrlDecode(std::string_view input, std::string &output, bool plus_as_space)
{
    // 十六进制位只有 0-9 与 a-f/A-F，收窄到 g-z 之外才能拒绝 %G0 这类非法转义。
    const auto hex_value = [](unsigned char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); i++)
    {
        const char ch = input[i];
        if (ch == '%')
        {
            if (input.size() - i < 3)
                return false;

            // 处理低4位
            const int high = hex_value(input[i + 1]);
            const int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0)
            {
                return false;
            }
            result += static_cast<char>((high << 4) | low);
            i += 2;
        }
        else if (ch == '+' && plus_as_space)
        {
            result += ' ';
        }
        else
        {
            result += ch;
        }
    }
    output.swap(result);
    return true;
}

std::string_view Util::StatusDescription(int status)
{
    static const std::unordered_map<int, std::string_view> descriptions = {
        {100, "Continue"},
        {101, "Switching Protocols"},

        {200, "OK"},
        {201, "Created"},
        {202, "Accepted"},
        {204, "No Content"},
        {205, "Reset Content"},
        {206, "Partial Content"},

        {301, "Moved Permanently"},
        {302, "Found"},
        {303, "See Other"},
        {304, "Not Modified"},
        {307, "Temporary Redirect"},
        {308, "Permanent Redirect"},

        {400, "Bad Request"},
        {401, "Unauthorized"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {408, "Request Timeout"},
        {409, "Conflict"},
        {413, "Content Too Large"},
        {414, "URI Too Long"},
        {415, "Unsupported Media Type"},
        {416, "Range Not Satisfiable"},
        {429, "Too Many Requests"},
        {431, "Request Header Fields Too Large"},

        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {502, "Bad Gateway"},
        {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
        {505, "HTTP Version Not Supported"},
    };
    const auto it = descriptions.find(status);
    return it == descriptions.end() ? "Unknown" : it->second;
}

std::string_view Util::MimeType(std::string_view filename)
{
    static const std::unordered_map<std::string_view, std::string_view> types = {
        {".html", "text/html"},        {".htm", "text/html"},         {".css", "text/css"},
        {".js", "text/javascript"},    {".txt", "text/plain"},        {".csv", "text/csv"},

        {".json", "application/json"}, {".xml", "application/xml"},   {".pdf", "application/pdf"},
        {".zip", "application/zip"},   {".wasm", "application/wasm"},

        {".jpg", "image/jpeg"},        {".jpeg", "image/jpeg"},       {".png", "image/png"},
        {".gif", "image/gif"},         {".webp", "image/webp"},       {".svg", "image/svg+xml"},

        {".mp3", "audio/mpeg"},        {".mp4", "video/mp4"},         {".woff", "font/woff"},
        {".woff2", "font/woff2"},
    };
    // fallback: 未知的二进制数据流
    constexpr std::string_view fallback = "application/octet-stream";

    //  去掉目录部分，只在文件名里查找扩展名
    //  在 filename 中查找最后一次出现的 / 或 \。
    //  "/\\" 就是一个包含两个字符的字符集合：{'/', '\\'}
    const auto slash = filename.find_last_of("/\\");
    if (slash != std::string_view::npos)
    {
        //"/home/user/test.txt"
        //            ↑
        // 移动到t字符的位置
        filename.remove_prefix(slash + 1);
    }
    const auto dot = filename.find_last_of('.');
    // 无扩展名、单纯的隐藏文件名或以 '.' 结尾。
    // 1.无扩展名: README Makefile LICENSE
    // 2.单纯的隐藏文件名: .gitignore .bashrc .env
    // 3.以 '.' 结尾:      file. archive.  test.
    if (dot == std::string_view::npos || dot == 0 || dot == filename.size() - 1)
    {
        return fallback;
    }
    // 从'.'位置往后截取得到扩展名；转小写后 .JPG 与 .jpg 得到相同结果。
    const std::string extension = Util::ToLowerAscii(std::string(filename.substr(dot)));
    const auto it = types.find(extension);
    return it == types.end() ? fallback : it->second;
}

// RFC 9110 token 的字符集。请求行/字段名解析与响应头字段名校验必须用同一份定义，
// 否则两侧对"什么名字合法"的判断会各自漂移。
bool Util::IsToken(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    for (unsigned char ch : value)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
        {
            continue;
        }
        // 按 unsigned char 判断，避免 char 的有符号性把非 ASCII 字节判成合法。
        if (std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(ch)) == std::string_view::npos)
        {
            return false;
        }
    }
    return true;
}

std::string Util::ToLowerAscii(std::string value)
{
    for (char &ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool Util::EqualsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        char left = lhs[i];
        char right = rhs[i];
        if (left >= 'A' && left <= 'Z')
        {
            left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z')
        {
            right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right)
        {
            return false;
        }
    }
    return true;
}

bool Util::IsDirectory(const std::filesystem::path &path)
{
    std::error_code ec;
    const bool result = std::filesystem::is_directory(path, ec);
    return !ec && result;
}

bool Util::IsRegularFile(const std::filesystem::path &path)
{
    std::error_code ec;
    const bool result = std::filesystem::is_regular_file(path, ec);
    return !ec && result;
}

// 核心功能：把已解码的 URL 路径转换为服务器上的实际路径，要求目标存在，而且位于站点根目录内。
// document_root：存放网站资源的根目录
// decoded_path：已经完成 URL 解码的请求路径，以 / 开头，不包含查询参数
// resolved_path： 检查通过后的服务器文件绝对路径
// 示例：1. document_root：/home/hamber/www
//      2. decoded_path：/images/logo.png
//      3. resolved_path：/home/hamber/www/images/logo.png
bool Util::ResolveResourcePath(const std::filesystem::path &document_root, std::string_view decoded_path,
                               std::filesystem::path &resolved_path)
{
    // 1.decoded_path输入必须以 / 开头，拒绝空路径和内嵌 '\0'；
    if (decoded_path.empty() || decoded_path.front() != '/' || decoded_path.find('\0') != std::string_view::npos)
    {
        return false;
    }

    // 2.确认 document_root 为磁盘中真实存在的目录
    std::error_code ec;
    const auto root = std::filesystem::canonical(document_root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec)
    {
        return false;
    }

    // 3.URL 的前导斜杠属于站点，不是操作系统的根目录，将其移除
    // 示例: decoded_path = "/images/logo.png";   -> relative= images/logo.png
    const auto relative = std::filesystem::path(decoded_path).relative_path();

    // 4.拼接并规范化路径，要求目标及中间目录真实存在，若请求路径不存在会返回false
    // candidate = document_root + relative
    // /home/hamber/www + images/logo.png → /home/hamber/www/images/logo.png
    auto candidate = std::filesystem::canonical(root / relative, ec);
    if (ec)
    {
        return false;
    }

    // 5.按目录分量比较，避免目标存在，但超出站点根目录
    // 例如：
    // document_root = "/srv/www";
    // candidate = /srv/www/../private/secret.txt -> /srv/private/secret.txt
    // 逐个比较路径分量
    auto candidate_it = candidate.begin();
    for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++candidate_it)
    {
        if (candidate_it == candidate.end() || *candidate_it != *root_it)
        {
            return false;
        }
    }

    resolved_path.swap(candidate);
    return true;
}
