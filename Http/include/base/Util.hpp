/**
 * @file Util.hpp
 * @brief
 *
 * 功能设计:
 *  - 读取文件内容
 *  - 向文件写入内容
 *  - URL编码
 *  - URL解码
 *  - 通过HTTP状态码获取描述信息
 *  - 根据文件后缀名获取
 *  - 判断一个文件是否是目录
 *  - 判断一个文件是否是一个普通文件
 *  - HTTP资源路径的有效性判断
 *
 */

#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
class Buffer;

class Util
{
  public:
    Util() = delete;
    // 字符串分割函数
    static size_t Split(std::string_view input, std::string_view delimiter, std::vector<std::string> &result,
                        bool keep_empty = false);
    // 读取文件全部内容并追加到 buffer 现有可读数据之后，支持二进制数据。
    // buffer 为空指针、打开或读取失败时返回 false，且不修改缓冲区。
    static bool ReadFile(const std::filesystem::path &path, Buffer *buffer);

    // 覆盖写入，支持二进制数据；不自动创建父目录，失败返回 false。
    static bool WriteFile(const std::filesystem::path &path, std::string_view content);

    // 编码单个 URL 组成部分，不对完整 URL 直接编码。
    static std::string UrlEncode(std::string_view input, bool space_as_plus = false);

    static bool UrlDecode(std::string_view input, std::string &output, bool plus_as_space = false);
    // 状态码到描述文本的映射
    static std::string_view StatusDescription(int status);
    // 根据文件扩展名返回媒体类型
    static std::string_view MimeType(std::string_view filename);

    static bool IsDirectory(const std::filesystem::path &path);

    static bool IsRegularFile(const std::filesystem::path &path);

    // 将已经解码的 URL 路径解析为站点根目录内的文件路径。
    // 输入须以 '/' 开头且不含 NUL；不重复解码。根目录必须存在。
    // 解析符号链接后检查目录边界；要求目标存在，不判断目标文件类型。
    // 成功输出规范化绝对路径，失败保持输出不变；分配异常仍可传播。
    // 目录结构须稳定：不能防止检查后符号链接被并发替换。
    static bool ResolveResourcePath(const std::filesystem::path &document_root, std::string_view decoded_path,
                                    std::filesystem::path &resolved_path);
};