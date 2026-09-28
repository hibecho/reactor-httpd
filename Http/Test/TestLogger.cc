#include "base/Logger.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <pthread.h>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
std::string Read(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
struct TempDirectory
{
    std::string path;
    TempDirectory()
    {
        char name[] = "/tmp/http-logger-XXXXXX";
        char *result = mkdtemp(name);
        if (!result)
            throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~TempDirectory()
    {
        Logger::Instance().Shutdown();
        DIR *dir = opendir(path.c_str());
        if (dir)
        {
            while (dirent *entry = readdir(dir))
            {
                std::string name = entry->d_name;
                if (name != "." && name != "..")
                    unlink((path + "/" + name).c_str());
            }
            closedir(dir);
        }
        rmdir(path.c_str());
    }
};
} // namespace

void TestLoggerMode(bool async)
{
    TempDirectory temp;
    Logger &log = Logger::Instance();
    Logger::Config cfg;
    cfg.async = async;
    cfg.console = false;
    cfg.file_path = temp.path + "/basic.log";
    cfg.pattern = "%l|%s:%#|%v";
    log.Init(cfg);
    const Logger::Level levels[] = {Logger::Level::Trace, Logger::Level::Debug, Logger::Level::Info,
                                    Logger::Level::Warn,  Logger::Level::Error, Logger::Level::Critical,
                                    Logger::Level::Off};
    for (int threshold = 0; threshold < 7; ++threshold)
    {
        log.SetLevel(levels[threshold]);
        assert(log.GetLevel() == levels[threshold]);
        for (int message = 0; message < 7; ++message)
            assert(log.ShouldLog(levels[message]) == (message < 6 && message >= threshold));
    }
    bool invalid_level_threw = false;
    try
    {
        log.SetLevel(static_cast<Logger::Level>(99));
    }
    catch (const std::invalid_argument &)
    {
        invalid_level_threw = true;
    }
    assert(invalid_level_threw && log.GetLevel() == Logger::Level::Off);
    assert(!log.ShouldLog(static_cast<Logger::Level>(99)));
    log.SetLevel(Logger::Level::Info);
    int evaluated = 0;
    LOG_DEBUG("hidden {}", ++evaluated);
    assert(evaluated == 0);
    const int source_line = __LINE__ + 1;
    LOG_INFO("hello {}", 42);
    if (true)
        LOG_WARN("single argument");
    else
        assert(false);
    LOG_ERROR("auto flush");
    if (async)
        log.Flush(); // 同步模式仍验证 ERROR 的即时刷新。
    std::string content = Read(cfg.file_path);
    assert(content.find("info|TestLogger.cc:" + std::to_string(source_line) + "|hello 42") != std::string::npos);
    assert(content.find("warning|") != std::string::npos);
    assert(content.find("auto flush") != std::string::npos);
    assert(content.find("hidden") == std::string::npos);
    log.SetLevel(Logger::Level::Trace);
    LOG_TRACE("trace enabled");
    LOG_DEBUG("debug enabled");
    LOG_CRITICAL("critical enabled");
    log.Flush();
    content = Read(cfg.file_path);
    assert(content.find("trace enabled") != std::string::npos);
    assert(content.find("debug enabled") != std::string::npos);
    assert(content.find("critical enabled") != std::string::npos);
    for (int kind = 0; kind < (async ? 7 : 5); ++kind)
    {
        Logger::Config bad = cfg;
        if (kind == 0)
            bad.file_path.clear();
        if (kind == 1)
            bad.max_file_size = 0;
        if (kind == 2)
            bad.max_files = 0;
        if (kind == 3)
            bad.level = static_cast<Logger::Level>(99);
        if (kind == 4)
            bad.flush_level = static_cast<Logger::Level>(99);
        if (kind == 5)
            bad.queue_size = 0;
        if (kind == 6)
            bad.queue_size = std::numeric_limits<size_t>::max();
        bool threw = false;
        try
        {
            log.Init(bad);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        assert(threw);
        assert(log.GetLevel() == Logger::Level::Trace);
    }
    Logger::Config bad = cfg;
    bad.file_path = cfg.file_path + "/impossible.log";
    bool threw = false;
    try
    {
        log.Init(bad);
    }
    catch (const spdlog::spdlog_ex &)
    {
        threw = true;
    }
    assert(threw);
    LOG_INFO("old logger survives");
    log.Shutdown();
    content = Read(cfg.file_path);
    assert(content.find("old logger survives") != std::string::npos);
    LOG_CRITICAL("closed {}", ++evaluated);
    log.Shutdown();
    assert(evaluated == 0 && log.GetLevel() == Logger::Level::Off);
    assert(Read(cfg.file_path) == content);
    log.Init(cfg);
    LOG_INFO("reopened");
    log.Flush();
    assert(Read(cfg.file_path).find(content) == 0);
    assert(Read(cfg.file_path).find("reopened") != std::string::npos);
    log.SetLevel(Logger::Level::Off);
    LOG_CRITICAL("off {}", ++evaluated);
    assert(evaluated == 0);

    cfg.file_path = temp.path + "/threads.log";
    cfg.pattern = "%v";
    log.Init(cfg);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 250; ++i)
                LOG_INFO("{}:{}", t, i);
        });
    for (auto &thread : threads)
        thread.join();
    log.Flush();
    std::ifstream input(cfg.file_path);
    std::set<std::string> lines;
    std::string line;
    int count = 0;
    while (std::getline(input, line))
    {
        lines.insert(line);
        ++count;
    }
    assert(count == 1000 && lines.size() == 1000);
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < 250; ++i)
            assert(lines.count(std::to_string(t) + ":" + std::to_string(i)) == 1);

    cfg.file_path = temp.path + "/rotate.log";
    cfg.max_file_size = 128;
    cfg.max_files = 2;
    log.Init(cfg);
    for (int i = 0; i < 100; ++i)
        LOG_INFO("record-{:03d}-abcdefghijklmnop", i);
    log.Shutdown();
    assert(Read(cfg.file_path).find("record-099") != std::string::npos);
    assert(!Read(temp.path + "/rotate.1.log").empty());
    assert(!Read(temp.path + "/rotate.2.log").empty());
    assert(access((temp.path + "/rotate.3.log").c_str(), F_OK) != 0);
    std::cout << (async ? "Asynchronous" : "Synchronous") << " logger tests passed\n";
}

// 全部 LOG_* 一律不抛异常：单例获取、等级判断、参数求值与后端写入都在宏内兜底。
void TestNonThrowingLogger(bool async)
{
    TempDirectory temp;
    Logger &log = Logger::Instance();
    Logger::Config cfg;
    cfg.async = async;
    cfg.console = false;
    cfg.file_path = temp.path + "/safe.log";
    cfg.pattern = "%l|%s:%#|%v";
    log.Init(cfg);

    for (int kind = 0; kind < 3; ++kind)
    {
        int evaluated = 0;
        auto fail = [&]() -> int {
            ++evaluated;
            if (kind == 0)
                throw std::runtime_error("log argument failure");
            if (kind == 1)
                throw std::bad_alloc();
            throw 42;
        };
        // 三档异常都必须被宏吞掉。第三档 throw 42 是非 std 异常，专门证明
        // 宏里的 catch 不能收窄成 std::exception：spdlog 自己的 SPDLOG_LOGGER_CATCH
        // 对非 std 异常是记一条错误再重新抛出。
        bool propagated = false;
        try
        {
            LOG_ERROR("failed {}", fail());
        }
        catch (...)
        {
            propagated = true;
        }
        assert(!propagated && evaluated == 1);
    }

    const int source_line = __LINE__ + 1;
    LOG_ERROR("after failures {}", 42);
    if (true)
        LOG_ERROR("single argument");
    else
        assert(false);
    if (async)
        log.Flush();
    const std::string content = Read(cfg.file_path);
    assert(content.find("error|TestLogger.cc:" + std::to_string(source_line) + "|after failures 42") !=
           std::string::npos);
    assert(content.find("single argument") != std::string::npos);
    assert(content.find("failed") == std::string::npos);

    int evaluated = 0;
    log.SetLevel(Logger::Level::Critical);
    LOG_ERROR("filtered {}", ++evaluated);
    assert(evaluated == 0);
    log.Shutdown();
    LOG_ERROR("closed {}", ++evaluated);
    assert(evaluated == 0 && Read(cfg.file_path) == content);
    std::cout << "Non-throwing logger tests passed\n";
}

size_t TaskCount()
{
    DIR *dir = opendir("/proc/self/task");
    assert(dir);
    size_t count = 0;
    while (dirent *entry = readdir(dir))
        if (entry->d_name[0] != '.')
            ++count;
    closedir(dir);
    return count;
}

// 等待线程数回落到期望值。pthread_join 返回不代表内核已摘除 /proc/self/task 条目：
// 唤醒 join 的 clear_child_tid 在 exit_mm 阶段，条目清除在随后的 release_task。
// 实测空载下 20000 次 join 有 136 次能立刻读到残留条目，重负载下窗口更宽，所以
// 只在「线程消失」这个方向上等待，并设有上限——真泄漏的线程不会自行消失，仍会被抓到。
size_t WaitForTaskCount(size_t expected)
{
    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        if (TaskCount() == expected)
            return expected;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return TaskCount();
}

void TestImplicitDefaultAndSignalMask()
{
    std::set<std::string> before;
    DIR *tasks = opendir("/proc/self/task");
    assert(tasks != nullptr);
    while (dirent *entry = readdir(tasks))
        if (entry->d_name[0] != '.')
            before.insert(entry->d_name);
    closedir(tasks);
    sigset_t original, after;
    assert(pthread_sigmask(SIG_SETMASK, nullptr, &original) == 0);
    // 本用例必须最先执行，证明不调用 Init 也默认异步。
    Logger &log = Logger::Instance();
    assert(log.GetLevel() == Logger::Level::Info);
    assert(TaskCount() == before.size() + 1);
    assert(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
    for (int signal = 1; signal < NSIG; ++signal)
        assert(sigismember(&original, signal) == sigismember(&after, signal));
    tasks = opendir("/proc/self/task");
    assert(tasks != nullptr);
    int workers = 0;
    while (dirent *entry = readdir(tasks))
    {
        if (entry->d_name[0] == '.' || before.count(entry->d_name))
            continue;
        ++workers;
        std::ifstream status(std::string("/proc/self/task/") + entry->d_name + "/status");
        std::string line;
        bool checked = false;
        while (std::getline(status, line))
        {
            if (line.compare(0, 7, "SigBlk:") != 0)
                continue;
            const auto mask = std::stoull(line.substr(7), nullptr, 16);
            for (int signal : {SIGINT, SIGTERM, SIGUSR1})
                assert((mask & (std::uint64_t(1) << (signal - 1))) != 0);
            checked = true;
        }
        assert(checked);
    }
    closedir(tasks);
    assert(workers == 1);
    log.Shutdown();
    assert(WaitForTaskCount(before.size()) == before.size());
    std::cout << "Implicit asynchronous default and signal mask tests passed\n";
}

void TestDefaultWorkerAndSmallQueue()
{
    TempDirectory temp;
    Logger &log = Logger::Instance();
    log.Shutdown();
    const size_t baseline = WaitForTaskCount(1); // 本测试进程除主线程外没有常驻线程。
    Logger::Config cfg;
    assert(cfg.async && cfg.queue_size == 8192);
    cfg.console = false;
    cfg.file_path = temp.path + "/queue.log";
    cfg.pattern = "%v";
    cfg.queue_size = 1;
    cfg.flush_level = Logger::Level::Off;
    log.Init(cfg);
    assert(TaskCount() == baseline + 1); // 真实独立 worker，不能伪装成同步实现。
    std::string expected;
    for (int batch = 0; batch < 4; ++batch)
    {
        for (int i = 0; i < 2000; ++i)
        {
            const int id = batch * 2000 + i;
            LOG_INFO("queue-{}", id);
            expected += "queue-" + std::to_string(id) + "\n";
        }
        log.Flush();
        log.Flush();
        assert(Read(cfg.file_path) == expected);
    }
    for (int i = 8000; i < 10000; ++i)
    {
        LOG_INFO("queue-{}", i);
        expected += "queue-" + std::to_string(i) + "\n";
    }
    log.Shutdown(); // 不预先 Flush，验证排空最后一批。
    assert(Read(cfg.file_path) == expected);
    assert(WaitForTaskCount(baseline) == baseline);
    cfg.async = false;
    log.Init(cfg);
    assert(TaskCount() == baseline);
    log.Shutdown();
    std::cout << "Default worker and small-queue drain tests passed\n";
}

void TestModeSwitchRotation()
{
    TempDirectory temp;
    Logger &log = Logger::Instance();
    Logger::Config cfg;
    cfg.console = false;
    cfg.file_path = temp.path + "/switch.log";
    cfg.pattern = "%v";
    cfg.max_file_size = 512;
    cfg.max_files = 20;
    cfg.queue_size = 1;
    cfg.flush_level = Logger::Level::Off;
    std::string expected;
    for (int batch = 0; batch < 6; ++batch)
    {
        cfg.async = batch % 2 == 0;
        log.Init(cfg); // 同路径切换必须先排空旧后端，再重建文件尺寸状态。
        for (int i = 0; i < 20; ++i)
        {
            int id = batch * 20 + i;
            LOG_INFO("switch-{:03d}-abcdefghijklmnop", id);
            char line[64];
            snprintf(line, sizeof(line), "switch-%03d-abcdefghijklmnop\n", id);
            expected += line;
        }
    }
    log.Shutdown();
    std::string actual;
    for (int i = 20; i > 0; --i)
        actual += Read(temp.path + "/switch." + std::to_string(i) + ".log");
    actual += Read(cfg.file_path);
    assert(actual == expected);
    assert(!Read(temp.path + "/switch.1.log").empty());
    std::cout << "Same-path mode switching and rotation tests passed\n";
}

void TestConcurrentConfiguration()
{
    TempDirectory temp;
    Logger &log = Logger::Instance();
    Logger::Config cfg;
    cfg.console = false;
    cfg.file_path = temp.path + "/concurrent.log";
    cfg.pattern = "%v";
    cfg.queue_size = 1;
    cfg.flush_level = Logger::Level::Off;
    log.Init(cfg);
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([&, t] {
            while (!start.load())
                std::this_thread::yield();
            for (int i = 0; i < 1000; ++i)
                LOG_INFO("{}:{}", t, i);
        });
    std::thread configure([&] {
        while (!start.load())
            std::this_thread::yield();
        for (int i = 0; i < 20; ++i)
        {
            cfg.async = i % 2 == 0;
            log.Init(cfg);
            log.SetLevel(Logger::Level::Info);
        }
    });
    std::thread flush([&] {
        while (!start.load())
            std::this_thread::yield();
        for (int i = 0; i < 30; ++i)
            log.Flush();
    });
    start.store(true);
    for (auto &producer : producers)
        producer.join();
    configure.join();
    flush.join();
    log.Shutdown();
    std::ifstream input(cfg.file_path);
    std::set<std::string> records;
    std::string line;
    size_t count = 0;
    while (std::getline(input, line))
    {
        records.insert(line);
        ++count;
    }
    assert(count == 4000 && records.size() == 4000);
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < 1000; ++i)
            assert(records.count(std::to_string(t) + ":" + std::to_string(i)) == 1);
    std::cout << "Concurrent mode switching, logging and flush tests passed\n";
}

void TestNormalExit()
{
    TempDirectory temp;
    const std::string path = temp.path + "/exit.log";
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        execl("/proc/self/exe", "TestLogger", "--normal-exit", path.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::string expected;
    for (int i = 0; i < 512; ++i)
        expected += "exit-" + std::to_string(i) + "\n";
    assert(Read(path) == expected);
    std::cout << "Default asynchronous normal-exit test passed\n";
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--normal-exit")
    {
        Logger::Config cfg;
        cfg.console = false;
        cfg.file_path = argv[2];
        cfg.pattern = "%v";
        cfg.flush_level = Logger::Level::Off;
        cfg.queue_size = 1;
        alarm(30);
        Logger::Instance().Init(cfg);
        for (int i = 0; i < 512; ++i)
            LOG_INFO("exit-{}", i);
        return 0; // 不显式关闭，验证单例析构能刷新输出缓冲。
    }
    // 包括 Flush/Shutdown 死锁和子进程退出挂起，统一设置硬超时。
    alarm(60);
    TestImplicitDefaultAndSignalMask();
    for (bool async : {false, true})
    {
        TestLoggerMode(async);
        TestNonThrowingLogger(async);
    }
    TestDefaultWorkerAndSmallQueue();
    TestModeSwitchRotation();
    TestConcurrentConfiguration();
    TestNormalExit();
    alarm(0);
}
