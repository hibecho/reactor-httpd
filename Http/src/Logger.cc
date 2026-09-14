#include "Logger.hpp"

#include <stdexcept>
#include <vector>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

spdlog::level::level_enum Logger::ToSpdlogLevel(Level level)
{
    switch (level)
    {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    default:
        throw std::invalid_argument("Logger: invalid log level");
    }
}

Logger::Level Logger::FromSpdlogLevel(spdlog::level::level_enum level)
{
    switch (level)
    {
    case spdlog::level::trace:
        return Level::Trace;
    case spdlog::level::debug:
        return Level::Debug;
    case spdlog::level::info:
        return Level::Info;
    case spdlog::level::warn:
        return Level::Warn;
    case spdlog::level::err:
        return Level::Error;
    case spdlog::level::critical:
        return Level::Critical;
    case spdlog::level::off:
        return Level::Off;
    default:
        throw std::invalid_argument("Logger: invalid backend log level");
    }
}

// 采用懒汉模式
Logger &Logger::Instance()
{
    // C++11保证了线程安全
    static Logger instance;
    return instance;
}

Logger::Logger()
{
    Init(Config{});
}

Logger::~Logger()
{
    try
    {
        Shutdown();
    }
    catch (...)
    {
        // 析构不抛异常；成员销毁时仍会释放日志输出资源。
    }
}

void Logger::Init(const Config &config)
{
    const auto backend_level = ToSpdlogLevel(config.level);
    const auto backend_flush_level = ToSpdlogLevel(config.flush_level);
    // 至少设定一种输出方式
    if (!config.console && config.file_path.empty())
        throw std::invalid_argument("Logger: at least one output is required");

    // 设置了文件输出日志，判断合法的文件大小，文件个数
    if (!config.file_path.empty() &&
        (config.max_file_size == 0 || config.max_files == 0 || config.max_files > 200000))
        throw std::invalid_argument("Logger: invalid rotation size or backup count");

    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<spdlog::sink_ptr> sinks;
        if (config.console)
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        // 同一路径重新初始化前刷新旧缓冲，确保新 sink 获取正确的文件大小。
        if (_logger)
            _logger->flush();
        if (!config.file_path.empty())
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.file_path, config.max_file_size, config.max_files));
        auto next = std::make_shared<spdlog::logger>("http", sinks.begin(), sinks.end());
        next->set_pattern(config.pattern);
        next->set_level(backend_level);
        next->flush_on(backend_flush_level);
        _logger.swap(next);
    }
}

void Logger::SetLevel(Level level)
{
    const auto backend_level = ToSpdlogLevel(level);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_logger)
            _logger->set_level(backend_level);
    }
}

Logger::Level Logger::GetLevel()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _logger ? FromSpdlogLevel(_logger->level()) : Level::Off;
}

bool Logger::ShouldLog(Level level)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _logger && level >= Level::Trace && level < Level::Off &&
           _logger->should_log(ToSpdlogLevel(level));
}

void Logger::Flush()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_logger)
        _logger->flush();
}

void Logger::Shutdown()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_logger)
        _logger->flush();
    _logger.reset();
}
