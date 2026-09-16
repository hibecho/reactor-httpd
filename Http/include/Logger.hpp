/**
 * @file Logger.hpp
 * @brief
 *
 *
 *
 *
 */

#pragma once
#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// 同步日志模块：配置、写入、刷新与关闭可并发调用。
class Logger
{
    static const int MAX_FILE_SIZE = 5 * 1024 * 1024;

  public:
    // 模块自己的等级；与底层枚举通过显式映射转换。
    enum class Level
    {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
        Critical,
        Off
    };

    // 配置项
    struct Config
    {
        bool console = true;                       // 是否向控制台输出日志
        std::string file_path;                     // 空字符串禁用文件输出。
        std::size_t max_file_size = MAX_FILE_SIZE; // 单个日志文件的滚动阈值 5 MB
        std::size_t max_files = 3;                 // 备份数，不含当前文件；范围 1..200000。
        Level level = Logger::Level::Info;         // 打印日志消息的最低等级
        Level flush_level = Logger::Level::Error;  // 达到该级别时自动刷新日志缓冲
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [pid:%P] [tid:%t] [%s:%#] %v";
    };

    // 1.获取Logger单例
    static Logger &Instance();

    // 2.初始化，将config配置进行应用
    //  默认控制台 info；可重复初始化，失败保留原日志器。
    //  非法配置抛 invalid_argument，文件打开失败抛 spdlog_ex。
    void Init(const Config &config);

    // 3.设置Level等级
    void SetLevel(Level level); // 关闭时无操作。

    // 4.获取Level等级
    Level GetLevel(); // 关闭时返回 off。

    // 5.判断是否需要输出日志
    bool ShouldLog(Level level);

    // 6.刷新输出缓冲，返回后仍可继续写日志；不等于 fsync。
    void Flush();

    // 7.刷新并关闭，直到 Init 前保持静默；建议工作线程退出后调用。
    void Shutdown();

    // 8.调用日志
    template <typename... Args>
    void Log(spdlog::source_loc location, Level level, spdlog::format_string_t<Args...> format, Args &&...args)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const bool should_log = _logger && level >= Logger::Level::Trace && level < Logger::Level::Off;
        if (should_log)
        {
            _logger->log(location, ToSpdlogLevel(level), format, std::forward<Args>(args)...);
        }
    }

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

  private:
    Logger();
    ~Logger();
    static spdlog::level::level_enum ToSpdlogLevel(Level level);
    static Level FromSpdlogLevel(spdlog::level::level_enum level);
    std::mutex _mutex;
    std::shared_ptr<spdlog::logger> _logger;
};

// 全部级别均编译保留，不受 SPDLOG_ACTIVE_LEVEL 影响。
// 并发切换配置时以实际写入时的级别为准；参数不要用于业务副作用。
#define HTTP_LOG_AT(level, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (Logger::Instance().ShouldLog(level))                                                                       \
        {                                                                                                              \
            Logger::Instance().Log(spdlog::source_loc{__FILE__, __LINE__, __func__}, level, __VA_ARGS__);              \
        }                                                                                                              \
    } while (false)

#define LOG_TRACE(...) HTTP_LOG_AT(Logger::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) HTTP_LOG_AT(Logger::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...) HTTP_LOG_AT(Logger::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) HTTP_LOG_AT(Logger::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) HTTP_LOG_AT(Logger::Level::Error, __VA_ARGS__)
#define LOG_CRITICAL(...) HTTP_LOG_AT(Logger::Level::Critical, __VA_ARGS__)
