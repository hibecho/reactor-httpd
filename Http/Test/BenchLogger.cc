#include "base/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    void Timeout(int)
    {
        static const char message[] = "BenchLogger timed out\n";
        const ssize_t written = ::write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)written;
        ::_exit(124);
    }

    std::size_t Number(const char *value, std::size_t limit)
    {
        const std::string text(value);
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("arguments must be positive decimal integers");
        std::size_t consumed = 0;
        const unsigned long long number = std::stoull(text, &consumed);
        if (consumed != text.size() || number == 0 || number > limit)
            throw std::invalid_argument("argument exceeds its allowed range");
        return static_cast<std::size_t>(number);
    }

    class TemporaryDirectory
    {
      public:
        TemporaryDirectory()
        {
            char path[] = "/tmp/http-logger-bench-XXXXXX";
            const char *result = ::mkdtemp(path);
            if (!result)
                throw std::runtime_error("mkdtemp failed");
            path_ = result;
        }
        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
        std::string File() const
        {
            return path_ + "/bench.log";
        }

      private:
        std::string path_;
    };

    class LoggerLifetime
    {
      public:
        ~LoggerLifetime()
        {
            try
            {
                Logger::Instance().Shutdown();
            }
            catch (...)
            {
            }
        }
    };

    double Milliseconds(Clock::duration value)
    {
        return std::chrono::duration<double, std::milli>(value).count();
    }
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc != 6 || (std::string(argv[1]) != "sync" && std::string(argv[1]) != "async"))
            throw std::invalid_argument("usage: BenchLogger <sync|async> <threads:1..64> <payload_bytes:1..65536> "
                                        "<queue_size:1..65536> <messages_per_thread:1..4000000>");
        const bool async = std::string(argv[1]) == "async";
        const std::size_t threads = Number(argv[2], 64);
        const std::size_t payload_bytes = Number(argv[3], 65536);
        const std::size_t queue = Number(argv[4], 65536);
        const std::size_t per_thread = Number(argv[5], 4000000);
        const std::size_t messages = threads * per_thread;
        const std::size_t max_bytes = 512ULL * 1024 * 1024;
        if (messages > 4000000 || messages > max_bytes / (payload_bytes + 1))
            throw std::invalid_argument("total messages must be <=4000000 and output <=512 MiB");

        struct sigaction action{};
        action.sa_handler = Timeout;
        ::sigemptyset(&action.sa_mask);
        if (::sigaction(SIGALRM, &action, nullptr) != 0)
            throw std::runtime_error("sigaction failed");
        ::alarm(120);

        TemporaryDirectory temporary;
        LoggerLifetime lifetime;
        Logger::Config config;
        config.async = async;
        config.queue_size = queue;
        config.console = false;
        config.file_path = temporary.File();
        // Use the production rotating sink without rotations changing record retention.
        config.max_file_size = max_bytes + 1;
        config.max_files = 1;
        config.level = Logger::Level::Info;
        config.flush_level = Logger::Level::Off;
        config.pattern = "%v";
        const std::string payload(payload_bytes, 'x');
        Logger &logger = Logger::Instance();
        logger.Init(config);
        for (std::size_t i = 0; i < 1000; ++i)
            LOG_INFO("{}", payload);
        logger.Shutdown();
        if (!std::filesystem::remove(config.file_path))
            throw std::runtime_error("cannot remove warmup output");
        logger.Init(config);

        std::vector<std::vector<std::uint64_t>> samples(threads);
        for (auto &sample : samples)
            sample.resize(per_thread);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t ready = 0;
        std::size_t done = 0;
        bool start = false;
        bool cancelled = false;
        Clock::time_point begin;
        Clock::time_point producer_end;
        try
        {
            for (std::size_t thread = 0; thread < threads; ++thread)
            {
                workers.emplace_back([&, thread] {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        ++ready;
                        changed.notify_all();
                        changed.wait(lock, [&] {
                            return start || cancelled;
                        });
                        if (cancelled)
                            return;
                    }
                    for (std::size_t i = 0; i < per_thread; ++i)
                    {
                        const auto before = Clock::now();
                        LOG_INFO("{}", payload);
                        const auto after = Clock::now();
                        samples[thread][i] = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    if (++done == threads)
                        producer_end = Clock::now();
                });
            }
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                cancelled = true;
            }
            changed.notify_all();
            for (auto &worker : workers)
                worker.join();
            throw;
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] {
                return ready == threads;
            });
            begin = Clock::now();
            start = true;
        }
        changed.notify_all();
        for (auto &worker : workers)
            worker.join();
        const auto flush_begin = Clock::now();
        logger.Flush();
        const auto end = Clock::now();
        logger.Shutdown();

        std::ifstream file(config.file_path, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot open benchmark output");
        std::string line;
        std::size_t lines = 0;
        while (std::getline(file, line))
        {
            if (line != payload)
                throw std::runtime_error("output payload mismatch");
            ++lines;
        }
        if (!file.eof() || lines != messages)
            throw std::runtime_error("output read failure or message count mismatch");

        std::vector<std::uint64_t> sorted;
        sorted.reserve(messages);
        for (const auto &sample : samples)
            sorted.insert(sorted.end(), sample.begin(), sample.end());
        std::sort(sorted.begin(), sorted.end());
        // Nearest-rank percentiles over all individual producer calls.
        const auto percentile = [&](std::size_t percent) {
            return sorted[(messages * percent + 99) / 100 - 1] / 1000.0;
        };
        const double total_ms = Milliseconds(end - begin);
        std::cout
            << "mode,threads,payload,queue,messages,producer_ms,total_ms,throughput,p50_us,p99_us,max_us,flush_ms\n"
            << std::fixed << std::setprecision(3) << (async ? "async" : "sync") << ',' << threads << ','
            << payload_bytes << ',' << queue << ',' << messages << ',' << Milliseconds(producer_end - begin) << ','
            << total_ms << ',' << messages * 1000.0 / total_ms << ',' << percentile(50) << ',' << percentile(99) << ','
            << sorted.back() / 1000.0 << ',' << Milliseconds(end - flush_begin) << '\n';
        ::alarm(0);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "BenchLogger: " << error.what() << '\n';
        return 1;
    }
}
