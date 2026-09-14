#include "Logger.hpp"

#include <condition_variable>
#include <limits>
#include <stdexcept>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <vector>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace
{
    // 只接收刷新通知，放在异步 flusher 的最后一个 sink。
    class CompletionSink : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        void Reset()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            _done = false;
        }

        void Wait()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            _ready.wait(lock, [this]
                        { return _done; });
        }

    protected:
        void sink_it_(const spdlog::details::log_msg &) override {}
        void flush_() override
        {
            _done = true; // base_sink::flush 已持有 mutex_。
            _ready.notify_one();
        }

    private:
        std::condition_variable _ready;
        bool _done = false;
    };
}

struct Logger::AsyncState
{
    // pool 最后销毁：析构会排空队列并 join 唯一的后台线程。
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::shared_ptr<CompletionSink> completion;
    std::shared_ptr<spdlog::async_logger> flusher;

    AsyncState(std::size_t capacity, const std::vector<spdlog::sink_ptr> &sinks)
        : pool(std::make_shared<spdlog::details::thread_pool>(capacity, 1)),
          completion(std::make_shared<CompletionSink>())
    {
        auto flush_sinks = sinks;
        flush_sinks.push_back(completion);
        flusher = std::make_shared<spdlog::async_logger>(
            "http.flush", flush_sinks.begin(), flush_sinks.end(), pool,
            spdlog::async_overflow_policy::block);
    }

    void Flush()
    {
        completion->Reset();
        // 直接提交，入队异常向上传播，避免被 logger 吞掉后无限等待。
        // 总是使用 block，刷新请求本身不能覆盖尚未处理的日志。
        pool->post_flush(std::shared_ptr<spdlog::async_logger>(flusher),
                         spdlog::async_overflow_policy::block);
        completion->Wait();
    }
};

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
        // 析构不抛异常；成员销毁仍会等待线程池退出。
    }
}

void Logger::Init(const Config &config)
{
    const auto backend_level = ToSpdlogLevel(config.level);
    const auto backend_flush_level = ToSpdlogLevel(config.flush_level);
    if (config.async &&
        (config.queue_size == 0 || config.queue_size == std::numeric_limits<std::size_t>::max()))
        throw std::invalid_argument("Logger: invalid async queue capacity");
    if (config.overflow_policy != OverflowPolicy::Block &&
        config.overflow_policy != OverflowPolicy::OverrunOldest)
        throw std::invalid_argument("Logger: invalid overflow policy");
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
        FlushLocked();
        if (!config.file_path.empty())
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.file_path, config.max_file_size, config.max_files));
        std::unique_ptr<AsyncState> next_async;
        std::shared_ptr<spdlog::logger> next;
        if (config.async)
        {
            next_async.reset(new AsyncState(config.queue_size, sinks));
            const auto policy = config.overflow_policy == OverflowPolicy::Block
                                    ? spdlog::async_overflow_policy::block
                                    : spdlog::async_overflow_policy::overrun_oldest;
            next = std::make_shared<spdlog::async_logger>(
                "http", sinks.begin(), sinks.end(), next_async->pool, policy);
        }
        else
        {
            next = std::make_shared<spdlog::logger>("http", sinks.begin(), sinks.end());
        }
        next->set_pattern(config.pattern);
        next->set_level(backend_level);
        next->flush_on(backend_flush_level);
        _logger.swap(next);
        _async.swap(next_async);
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

void Logger::FlushLocked()
{
    if (_async)
        _async->Flush();
    else if (_logger)
        _logger->flush();
}

void Logger::Flush()
{
    std::lock_guard<std::mutex> lock(_mutex);
    FlushLocked();
}

void Logger::Shutdown()
{
    std::lock_guard<std::mutex> lock(_mutex);
    FlushLocked();
    _logger.reset();
    _async.reset();
}
