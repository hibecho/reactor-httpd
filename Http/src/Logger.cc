#include "base/Logger.hpp"
#include <condition_variable>
#include <limits>
#include <pthread.h>
#include <signal.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace
{
// 刷新屏障：spdlog 没有「等待队列排空」的公开 API，async_logger::flush 只入队就返回，
// 这里用一个只做通知的 sink 补上完成信号。计数与等待共用 base_sink 的锁——flush_ 由
// 基类持锁调用，若另起一把锁会丢唤醒。
class CompletionSink : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    std::size_t Snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }
    void Wait(std::size_t before)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] {
            return completed_ != before;
        });
    }

  protected:
    void sink_it_(const spdlog::details::log_msg &) override
    {
    }
    void flush_() override
    {
        ++completed_;
        changed_.notify_all();
    }

  private:
    std::condition_variable changed_;
    std::size_t completed_ = 0;
};

// 新线程继承掩码，消除「线程创建后才屏蔽」期间抢走应用信号的窗口。
class BlockWorkerSignals
{
  public:
    BlockWorkerSignals()
    {
        sigset_t all;
        sigfillset(&all);
        const int error = pthread_sigmask(SIG_BLOCK, &all, &previous_);
        if (error != 0)
            throw std::system_error(error, std::generic_category(), "logger signal mask");
    }
    ~BlockWorkerSignals()
    {
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

  private:
    sigset_t previous_;
};
} // namespace

// 异步后端：队列与消费线程全部复用 spdlog 的 thread_pool，Logger 只持有它，
// 并补上 spdlog 不提供的刷新完成通知。
struct Logger::AsyncState
{
    explicit AsyncState(std::size_t capacity)
        : completion(std::make_shared<CompletionSink>())
    {
        // 先屏蔽调用线程，让随后创建的 worker 原子继承该掩码，再由 RAII 恢复本线程原掩码。
        BlockWorkerSignals mask;
        pool = std::make_shared<spdlog::details::thread_pool>(capacity, 1);
        // 屏障挂在独立日志器上：主日志器的 flush_on 自动刷新会未经请求推进计数，
        // 让等待提前返回，此时更早提交的消息可能尚未落盘。
        marker = std::make_shared<spdlog::async_logger>("http.flush", completion, pool,
                                                        spdlog::async_overflow_policy::block);
    }

    // 调用者持有 Logger 的锁，阻止新日志或配置越过刷新边界。
    void Flush(spdlog::logger &logger)
    {
        const auto before = completion->Snapshot();
        logger.flush();
        // 单 worker FIFO：实际输出刷新完成后，屏障才会通知等待者。
        // 直接提交，避免提交异常被 logger::flush 吞掉后永久等待。
        auto barrier = marker;
        pool->post_flush(std::move(barrier), spdlog::async_overflow_policy::block);
        completion->Wait(before);
    }

    std::shared_ptr<CompletionSink> completion;
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::shared_ptr<spdlog::async_logger> marker;
};

spdlog::level::level_enum Logger::ToSpdlogLevel(Level level)
{
    // 合法取值与 spdlog 逐一对应，越界只可能来自外部强转。
    if (static_cast<unsigned>(level) > static_cast<unsigned>(Level::Off))
        throw std::invalid_argument("Logger: invalid log level");
    return static_cast<spdlog::level::level_enum>(level);
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
    // 底层循环队列另需一个槽位，容量为 0 或 SIZE_MAX 都不可用。
    if (config.async && (config.queue_size == 0 || config.queue_size == std::numeric_limits<std::size_t>::max()))
        throw std::invalid_argument("Logger: invalid async queue size");

    // 至少设定一种输出方式
    if (!config.console && config.file_path.empty())
        throw std::invalid_argument("Logger: at least one output is required");

    // 设置了文件输出日志，判断合法的文件大小，文件个数
    if (!config.file_path.empty() && (config.max_file_size == 0 || config.max_files == 0 || config.max_files > 200000))
        throw std::invalid_argument("Logger: invalid rotation size or backup count");

    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console)
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    // 同一路径重新初始化前刷新旧缓冲，确保新 sink 获取正确的文件大小。
    FlushLocked();
    if (!config.file_path.empty())
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(config.file_path, config.max_file_size,
                                                                               config.max_files));
    std::unique_ptr<AsyncState> next_async;
    std::shared_ptr<spdlog::logger> next;
    if (config.async)
    {
        next_async = std::make_unique<AsyncState>(config.queue_size);
        next = std::make_shared<spdlog::async_logger>("http", sinks.begin(), sinks.end(), next_async->pool,
                                                      spdlog::async_overflow_policy::block);
    }
    else
        next = std::make_shared<spdlog::logger>("http", sinks.begin(), sinks.end());
    next->set_pattern(config.pattern);
    next->set_level(backend_level);
    next->flush_on(backend_flush_level);
    _logger.swap(next);
    // 旧 AsyncState 在此析构：thread_pool 发送终止消息并 join，此前入队的消息先被处理完。
    _async.swap(next_async);
}

void Logger::SetLevel(Level level)
{
    const auto backend_level = ToSpdlogLevel(level);

    std::lock_guard<std::mutex> lock(_mutex);
    if (_logger)
        _logger->set_level(backend_level);
}

Logger::Level Logger::GetLevel()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _logger ? static_cast<Level>(_logger->level()) : Level::Off;
}

bool Logger::ShouldLog(Level level)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _logger && level >= Level::Trace && level < Level::Off && _logger->should_log(ToSpdlogLevel(level));
}

void Logger::FlushLocked()
{
    if (!_logger)
        return;
    if (_async)
        _async->Flush(*_logger);
    else
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
    _async.reset(); // thread_pool 析构发送终止消息并 join。
}
