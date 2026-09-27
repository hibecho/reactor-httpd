// 基准工具：fork/exec 启动真实的 HttpServer 二进制，用真实回环连接压它，输出机器可解析的指标。
//
// 用法: BenchHttpServer <server-binary> <connections> <rounds-per-conn> <depth> <payload> <repeat>
//
// 设计上的几个要点（都是为了让数字可信）：
//  - 客户端自己设 TCP_NODELAY。否则 Nagle 会出现在客户端一侧，服务端有没有设就测不出来。
//  - 服务端进程绑核 0、客户端绑其余核。同机测试里两边抢 CPU 是最常见的噪声来源。
//  - 每连接一个线程，阻塞式一问一答，延迟用 steady_clock 逐次测量。
//  - 轮次之间重新启动服务器，避免长时间运行累积的状态影响后一轮。
//  - 同时报出客户端 CPU，用于确认客户端自己没有先饱和。
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
// ---------------------------------------------------------------- 基础工具

constexpr int kReadChunk = 64 * 1024;

void Die(const std::string &message)
{
    throw std::runtime_error(message);
}

void OnAlarm(int)
{
    const char *message = "BenchHttpServer timed out\n";
    (void)!write(STDERR_FILENO, message, strlen(message));
    _exit(2);
}

std::string ReadFile(const std::string &path)
{
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

int CpuCount()
{
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<int>(count) : 1;
}

// 把当前进程绑到 [first, last] 这些核上；核数不够时什么也不做并返回 false。
bool PinCurrentProcess(int first, int last)
{
    if (last < first)
        return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = first; cpu <= last; ++cpu)
        CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

// 解析 "0" 或 "0-2" 形式的核区间；环境变量未设置或格式不对时返回 false。
bool ParseCpuRange(const char *text, int &first, int &last)
{
    if (text == nullptr || *text == '\0')
        return false;
    const std::string value(text);
    const auto dash = value.find('-');
    if (dash == std::string::npos)
    {
        first = last = std::atoi(value.c_str());
    }
    else
    {
        first = std::atoi(value.substr(0, dash).c_str());
        last = std::atoi(value.substr(dash + 1).c_str());
    }
    return first >= 0 && last >= first;
}

// /proc/<pid>/stat 的第 14、15 个字段是用户态与内核态 CPU 时间，单位是时钟滴答。
bool ReadCpuSecondsFrom(const std::string &path, double &seconds);
bool ReadCpuSeconds(pid_t pid, double &seconds)
{
    return ReadCpuSecondsFrom("/proc/" + std::to_string(static_cast<long>(pid)) + "/stat", seconds);
}

bool ReadCpuSecondsFrom(const std::string &path, double &seconds)
{
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line))
        return false;
    // 进程名可能含空格且被括号包住，从最后一个右括号之后开始切分才安全。
    const auto close = line.rfind(')');
    if (close == std::string::npos)
        return false;
    std::istringstream rest(line.substr(close + 1));
    std::string field;
    long utime = 0;
    long stime = 0;
    // 跳过 state(0) 到第 14 个字段之前的 11 项。
    for (int index = 3; index <= 15; ++index)
    {
        if (!(rest >> field))
            return false;
        if (index == 14)
            utime = std::strtol(field.c_str(), nullptr, 10);
        else if (index == 15)
            stime = std::strtol(field.c_str(), nullptr, 10);
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0)
        return false;
    seconds = static_cast<double>(utime + stime) / static_cast<double>(ticks);
    return true;
}

// 读进程内每个线程的 CPU 时间，键是线程 id。用于观察连接在多个 EventLoop 线程间的分布：
// TcpServer 把连接轮询分给各个工作循环，所以线程 CPU 的分布就是连接分布的代理指标。
std::map<long, double> ReadThreadCpuSeconds(pid_t pid)
{
    std::map<long, double> values;
    const std::string dir = "/proc/" + std::to_string(static_cast<long>(pid)) + "/task";
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr)
        return values;
    while (dirent *entry = readdir(handle))
    {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        const long tid = std::strtol(entry->d_name, nullptr, 10);
        double seconds = 0.0;
        // 必须走 /proc/<pid>/task/<tid>/stat：直接读 /proc/<tid>/stat 拿到的是整个进程的
        // utime+stime，会让每个线程都报出进程总量（实测过，四个线程各报 1.05s 而进程总共 1.05s）。
        if (ReadCpuSecondsFrom(dir + "/" + entry->d_name + "/stat", seconds))
            values[tid] = seconds;
    }
    closedir(handle);
    return values;
}

// 峰值常驻内存（VmHWM），单位 KiB。
std::size_t ReadPeakRssKb(pid_t pid)
{
    const std::string status = ReadFile("/proc/" + std::to_string(static_cast<long>(pid)) + "/status");
    const auto position = status.find("VmHWM:");
    if (position == std::string::npos)
        return 0;
    return static_cast<std::size_t>(std::strtoul(status.c_str() + position + 6, nullptr, 10));
}

uint16_t PickFreePort()
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(0);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool bound = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
        socklen_t length = sizeof(address);
        const bool named = bound && getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0;
        close(fd);
        if (!named)
            continue;
        const uint16_t port = ntohs(address.sin_port);
        if (port >= 1024 && port != 8080)
            return port;
    }
    Die("no free port available");
    return 0;
}

int Connect(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        Die("socket failed");

    // 客户端侧关掉 Nagle：基准要测的是服务端有没有设 TCP_NODELAY，客户端不能先成为变量。
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval timeout{30, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

// 被测服务器进程；析构时兜底终止并回收。
class ServerProcess
{
  public:
    // 端口必须在构造时就确定：fork 之后子进程立刻 exec，之后再 SetPort 已经来不及。
    ServerProcess(const std::string &binary, uint16_t port, const std::string &log_path, int pin_cpu,
                  int cpu_last)
        : _port(port)
        , _log_path(log_path)
    {
        const pid_t parent = getpid();
        _pid = fork();
        if (_pid < 0)
            Die("fork failed");
        if (_pid != 0)
            return;

        // 父进程若异常退出，内核直接杀掉子进程，避免留下占着端口的孤儿。
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);
        if (pin_cpu >= 0)
            (void)PinCurrentProcess(pin_cpu, cpu_last);

        const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0)
        {
            (void)dup2(log, STDOUT_FILENO);
            (void)dup2(log, STDERR_FILENO);
            close(log);
        }
        std::vector<char *> argv;
        std::string program = binary;
        std::string port_text = std::to_string(_port);
        argv.push_back(program.data());
        argv.push_back(port_text.data());
        argv.push_back(nullptr);
        execv(program.c_str(), argv.data());
        _exit(127);
    }

    ~ServerProcess() { Stop(); }

    ServerProcess(const ServerProcess &) = delete;
    ServerProcess &operator=(const ServerProcess &) = delete;

    pid_t Pid() const { return _pid; }

    bool WaitReady(int seconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (TryReap())
                return false;
            const int fd = Connect(_port);
            if (fd >= 0)
            {
                close(fd);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    void Stop()
    {
        if (TryReap())
            return;
        kill(_pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!TryReap() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (TryReap())
            return;
        kill(_pid, SIGKILL);
        int status = 0;
        if (waitpid(_pid, &status, 0) == _pid)
            _reaped = true;
    }

  private:
    bool TryReap()
    {
        if (_reaped)
            return true;
        int status = 0;
        if (waitpid(_pid, &status, WNOHANG) != _pid)
            return false;
        _reaped = true;
        return true;
    }

    pid_t _pid = -1;
    bool _reaped = false;
    uint16_t _port = 0;
    std::string _log_path;
};

// ---------------------------------------------------------------- 连接封装

// 带缓冲的阻塞式客户端连接：读整行、读定长，正文不落地保存（只丢弃）。
class Conn
{
  public:
    explicit Conn(uint16_t port)
        : _fd(Connect(port))
        , _in(kReadChunk)
    {
        if (_fd < 0)
            Die("connect failed");
    }

    ~Conn()
    {
        if (_fd >= 0)
            close(_fd);
    }

    Conn(const Conn &) = delete;
    Conn &operator=(const Conn &) = delete;

    void Send(const char *data, std::size_t length)
    {
        std::size_t offset = 0;
        while (offset != length)
        {
            const ssize_t sent = send(_fd, data + offset, length - offset, MSG_NOSIGNAL);
            if (sent <= 0)
                Die("send failed");
            offset += static_cast<std::size_t>(sent);
        }
    }

    void ReadLine(std::string &out)
    {
        out.clear();
        for (;;)
        {
            if (_pos == _end)
                Fill();
            const char *begin = _in.data() + _pos;
            const char *newline = static_cast<const char *>(memchr(begin, '\n', _end - _pos));
            if (newline != nullptr)
            {
                out.append(begin, static_cast<std::size_t>(newline - begin));
                _pos = static_cast<std::size_t>(newline - _in.data()) + 1;
                if (!out.empty() && out.back() == '\r')
                    out.pop_back();
                return;
            }
            out.append(begin, _end - _pos);
            _pos = _end;
        }
    }

    void ReadExact(char *destination, std::size_t length)
    {
        std::size_t got = 0;
        while (got < length)
        {
            if (_pos == _end)
                Fill();
            const std::size_t available = std::min(_end - _pos, length - got);
            memcpy(destination + got, _in.data() + _pos, available);
            _pos += available;
            got += available;
        }
    }

  private:
    void Fill()
    {
        const ssize_t count = recv(_fd, _in.data(), _in.size(), 0);
        if (count <= 0)
            Die("recv failed");
        _pos = 0;
        _end = static_cast<std::size_t>(count);
    }

    int _fd = -1;
    std::vector<char> _in;
    std::size_t _pos = 0;
    std::size_t _end = 0;
};

// ---------------------------------------------------------------- 场景参数

struct Options
{
    std::string binary;
    int connections = 1;
    int rounds = 100;
    int depth = 1;      // 一轮写入多少个请求（流水线深度）
    std::size_t payload = 0; // POST /echo 的正文长度；0 表示用 GET /
    int repeat = 3;
    int pin_server = -1;    // 服务端绑到哪个核；-1 表示不绑
    int server_cpu_last = 0; // 服务端核区间的末核（多线程实验里会大于 pin_server）
    bool close_mode = false; // 每个请求一条新连接，用于压主循环的 accept 路径
    bool pin_client = false;
};

std::string SmallRequest(bool close_mode)
{
    return std::string("GET / HTTP/1.1\r\nHost: localhost\r\n") + (close_mode ? "Connection: close\r\n" : "") + "\r\n";
}

std::string LargeRequest(std::size_t payload, bool close_mode)
{
    std::string request = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                          std::to_string(payload) + "\r\n" + (close_mode ? "Connection: close\r\n" : "") + "\r\n";
    request.append(payload, 'x');
    return request;
}

// 读一个完整响应；返回状态码。正文读进复用缓冲后丢弃，避免每请求都分配。
int ReadResponse(Conn &conn, std::vector<char> &scratch)
{
    std::string line;
    conn.ReadLine(line);
    const auto first_space = line.find(' ');
    if (first_space == std::string::npos || line.size() < first_space + 4)
        Die("malformed status line");
    const int status = std::atoi(line.c_str() + first_space + 1);

    std::size_t length = 0;
    for (;;)
    {
        conn.ReadLine(line);
        if (line.empty())
            break;
        constexpr const char *kLength = "Content-Length:";
        if (line.compare(0, strlen(kLength), kLength) == 0)
            length = static_cast<std::size_t>(std::strtoul(line.c_str() + strlen(kLength), nullptr, 10));
    }
    if (length != 0)
    {
        if (scratch.size() < length)
            scratch.resize(length);
        conn.ReadExact(scratch.data(), length);
    }
    return status;
}

struct ThreadResult
{
    std::vector<double> latencies_us; // 每轮的耗时（微秒）；平均到每请求时再除以 depth
    long requests = 0;
    long status_errors = 0;
};

// 一个客户端线程：独占一条连接，跑 rounds 轮；每轮发 depth 个请求再收 depth 个响应。
void RunClient(const Options &options, uint16_t port, int warmup_rounds, ThreadResult &result)
{
    const std::string request =
        options.payload == 0 ? SmallRequest(options.close_mode) : LargeRequest(options.payload, options.close_mode);
    std::vector<char> scratch;

    // close 模式：每个请求一条新连接。它在客户端一侧的成本是每请求一次 connect，
    // 在服务端一侧压的是主循环的 accept 路径——这正是要测的。
    if (options.close_mode)
    {
        const int close_rounds = warmup_rounds + options.rounds;
        result.latencies_us.reserve(static_cast<std::size_t>(options.rounds));
        for (int round = 0; round < close_rounds; ++round)
        {
            Conn conn(port);
            const auto begin = std::chrono::steady_clock::now();
            conn.Send(request.data(), request.size());
            const int status = ReadResponse(conn, scratch);
            const auto end = std::chrono::steady_clock::now();
            if (status != 200)
                ++result.status_errors;
            if (round >= warmup_rounds)
            {
                result.latencies_us.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
                ++result.requests;
            }
        }
        return;
    }

    Conn conn(port);
    std::string batch;
    if (options.depth > 1)
    {
        batch.reserve(request.size() * static_cast<std::size_t>(options.depth));
        for (int i = 0; i < options.depth; ++i)
            batch += request;
    }
    const std::string &payload = options.depth > 1 ? batch : request;

    result.latencies_us.reserve(static_cast<std::size_t>(options.rounds));

    const int total_rounds = warmup_rounds + options.rounds;
    for (int round = 0; round < total_rounds; ++round)
    {
        const bool timed = round >= warmup_rounds;
        const auto begin = std::chrono::steady_clock::now();
        conn.Send(payload.data(), payload.size());
        for (int i = 0; i < options.depth; ++i)
        {
            const int status = ReadResponse(conn, scratch);
            if (status != 200)
                ++result.status_errors;
        }
        const auto end = std::chrono::steady_clock::now();
        if (timed)
        {
            result.latencies_us.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
            result.requests += options.depth;
        }
    }
}

// ---------------------------------------------------------------- 统计

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

struct Metrics
{
    double wall_seconds = 0.0;
    double throughput_rps = 0.0;
    double mbps = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
    double server_cpu_s = 0.0;
    double client_cpu_s = 0.0;
    std::size_t peak_rss_kb = 0;
    long requests = 0;
    long status_errors = 0;
    double cpu_per_request_us = 0.0;
    std::vector<double> thread_cpu_s; // 每个线程的 CPU 增量，降序
    double thread_cpu_sum = 0.0;       // 各线程之和，用于与进程总 CPU 交叉校验
};

double NowCpuSeconds()
{
    double seconds = 0.0;
    if (ReadCpuSeconds(getpid(), seconds))
        return seconds;
    return 0.0;
}

// 读被测服务器的 CPU 时间；进程已退出时返回上一次的值。
double NowCpuSecondsFor(pid_t pid)
{
    double seconds = 0.0;
    if (ReadCpuSeconds(pid, seconds))
        return seconds;
    return 0.0;
}

Metrics RunOnce(const Options &options, const std::string &temp_dir, int index)
{
    const uint16_t port = PickFreePort();
    ServerProcess server(options.binary, port, temp_dir + "/server-" + std::to_string(index) + ".log",
                         options.pin_server, options.server_cpu_last);
    if (!server.WaitReady(10))
        Die("server did not become ready");

    // 预热：建连、首次分配、TCP 慢启动都不计入。
    constexpr int kWarmupRounds = 50;
    {
        std::vector<std::thread> warm;
        std::vector<ThreadResult> results(static_cast<std::size_t>(options.connections));
        for (int i = 0; i < options.connections; ++i)
            warm.emplace_back([&options, port, &results, i] {
                RunClient(options, port, kWarmupRounds, results[static_cast<std::size_t>(i)]);
            });
        for (std::thread &worker : warm)
            worker.join();
    }

    const double server_cpu_before = NowCpuSecondsFor(server.Pid());
    const double client_cpu_before = NowCpuSeconds();
    const std::map<long, double> threads_before = ReadThreadCpuSeconds(server.Pid());

    std::vector<std::thread> workers;
    std::vector<ThreadResult> results(static_cast<std::size_t>(options.connections));
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < options.connections; ++i)
    {
        workers.emplace_back([&options, port, &results, i] {
            RunClient(options, port, 0, results[static_cast<std::size_t>(i)]);
        });
    }
    for (std::thread &worker : workers)
        worker.join();
    const auto end = std::chrono::steady_clock::now();

    const double server_cpu_after = NowCpuSecondsFor(server.Pid());
    const double client_cpu_after = NowCpuSeconds();
    const std::map<long, double> threads_after = ReadThreadCpuSeconds(server.Pid());

    Metrics metrics;
    metrics.wall_seconds = std::chrono::duration<double>(end - begin).count();
    metrics.server_cpu_s = server_cpu_after - server_cpu_before;
    metrics.client_cpu_s = client_cpu_after - client_cpu_before;
    metrics.peak_rss_kb = ReadPeakRssKb(server.Pid());

    // 每线程 CPU 增量；与进程总 CPU 的差额就是采样本身的不确定度，用来交叉校验读数是否可信。
    for (const auto &entry : threads_after)
    {
        const auto before = threads_before.find(entry.first);
        const double delta = entry.second - (before == threads_before.end() ? 0.0 : before->second);
        if (delta > 0.0)
            metrics.thread_cpu_s.push_back(delta);
    }
    std::sort(metrics.thread_cpu_s.begin(), metrics.thread_cpu_s.end(), std::greater<double>());
    for (const double value : metrics.thread_cpu_s)
        metrics.thread_cpu_sum += value;

    std::vector<double> latencies;
    for (const ThreadResult &result : results)
    {
        metrics.requests += result.requests;
        metrics.status_errors += result.status_errors;
        latencies.insert(latencies.end(), result.latencies_us.begin(), result.latencies_us.end());
    }

    // 一轮里有 depth 个请求，折算成每请求的耗时。
    const double depth = static_cast<double>(options.depth);
    for (double &value : latencies)
        value /= depth;

    metrics.p50_us = Percentile(latencies, 0.50);
    metrics.p90_us = Percentile(latencies, 0.90);
    metrics.p99_us = Percentile(latencies, 0.99);
    metrics.max_us = latencies.empty() ? 0.0 : *std::max_element(latencies.begin(), latencies.end());
    metrics.throughput_rps = metrics.wall_seconds > 0.0 ? static_cast<double>(metrics.requests) / metrics.wall_seconds : 0.0;
    metrics.cpu_per_request_us =
        metrics.requests > 0 ? metrics.server_cpu_s * 1e6 / static_cast<double>(metrics.requests) : 0.0;
    // 双向都算：请求正文发出去、响应正文收回来。
    const double bytes = static_cast<double>(metrics.requests) * static_cast<double>(options.payload) * 2.0;
    metrics.mbps = metrics.wall_seconds > 0.0 && options.payload > 0 ? bytes / metrics.wall_seconds / (1024.0 * 1024.0) : 0.0;

    server.Stop();
    return metrics;
}

std::string TempDir()
{
    char path[] = "/tmp/http-bench-XXXXXX";
    char *created = mkdtemp(path);
    if (created == nullptr)
        Die("mkdtemp failed");
    return created;
}

void PrintUsage()
{
    std::cerr << "用法: BenchHttpServer <server-binary> <connections> <rounds> <depth> <payload> <repeat>\n"
                 "  connections  并发连接数（每连接一个客户端线程）\n"
                 "  rounds       每连接跑多少轮\n"
                 "  depth        每轮写入的请求数（流水线深度，1 表示一问一答）\n"
                 "  payload      POST /echo 的正文字节数；0 表示改用 GET /\n"
                 "  repeat       重复轮数，每轮重启服务器\n";
}
} // namespace

int main(int argc, char *argv[])
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGALRM, OnAlarm);
    alarm(1200);

    if (argc != 7)
    {
        PrintUsage();
        return 1;
    }

    Options options;
    options.binary = argv[1];
    options.connections = std::atoi(argv[2]);
    options.rounds = std::atoi(argv[3]);
    options.depth = std::atoi(argv[4]);
    options.payload = static_cast<std::size_t>(std::strtoull(argv[5], nullptr, 10));
    options.repeat = std::atoi(argv[6]);
    // close 模式用环境变量开关：命令行的位置参数已经够多了。
    const char *close_env = std::getenv("BENCH_CLOSE");
    options.close_mode = close_env != nullptr && std::string(close_env) == "1";
    if (options.connections <= 0 || options.rounds <= 0 || options.depth <= 0 || options.repeat <= 0)
    {
        PrintUsage();
        return 1;
    }

    // 绑核策略：默认服务端独占核 0、客户端用其余核，减少两者互抢带来的噪声。
    // 多线程服务端的实验必须换个分法——把 3 个工作线程和主循环压在同一个核上会把
    // 并行度人为抹掉，所以允许用环境变量指定两边的核区间。
    const int cpus = CpuCount();
    int server_first = 0;
    int server_last = 0;
    int client_first = 1;
    int client_last = cpus - 1;
    const char *server_env = std::getenv("BENCH_SERVER_CPUS");
    const char *client_env = std::getenv("BENCH_CLIENT_CPUS");
    if (!ParseCpuRange(server_env, server_first, server_last) || !ParseCpuRange(client_env, client_first, client_last))
    {
        server_first = 0;
        server_last = 0;
        client_first = 1;
        client_last = cpus - 1;
    }
    // 双边都要吃 CPU 的场景（多线程服务端）里，绑核反而会让某一侧先饱和。
    // 设 BENCH_SERVER_CPUS=off 可以整体关闭绑核。
    const bool pin_off = server_env != nullptr && std::string(server_env) == "off";
    if (cpus >= 4 && !pin_off)
    {
        options.pin_server = server_first;
        options.pin_client = PinCurrentProcess(client_first, client_last);
    }
    options.server_cpu_last = server_last;

    std::cout << "BENCH binary=" << options.binary << " conns=" << options.connections << " rounds=" << options.rounds
              << " depth=" << options.depth << " payload=" << options.payload << " repeat=" << options.repeat
              << " cpus=" << cpus << " close_mode=" << (options.close_mode ? 1 : 0)
              << " server_pinned=" << options.pin_server << "-" << options.server_cpu_last
              << " client_pinned=" << (options.pin_client ? 1 : 0) << "\n";

    const std::string temp_dir = TempDir();
    std::vector<Metrics> runs;
    try
    {
        for (int i = 0; i < options.repeat; ++i)
        {
            const Metrics metrics = RunOnce(options, temp_dir, i);
            runs.push_back(metrics);
            std::cout << "RUN i=" << (i + 1) << " requests=" << metrics.requests << " wall_s=" << metrics.wall_seconds
                      << " throughput_rps=" << metrics.throughput_rps << " mbps=" << metrics.mbps
                      << " p50_us=" << metrics.p50_us << " p90_us=" << metrics.p90_us << " p99_us=" << metrics.p99_us
                      << " max_us=" << metrics.max_us << " server_cpu_s=" << metrics.server_cpu_s
                      << " server_cpu_per_req_us=" << metrics.cpu_per_request_us
                      << " client_cpu_s=" << metrics.client_cpu_s << " peak_rss_kb=" << metrics.peak_rss_kb
                      << " status_errors=" << metrics.status_errors << " threads=" << metrics.thread_cpu_s.size()
                      << " thread_cpu_s=";
            for (std::size_t index = 0; index < metrics.thread_cpu_s.size(); ++index)
            {
                if (index != 0)
                    std::cout << ",";
                std::cout << metrics.thread_cpu_s[index];
            }
            std::cout << " thread_cpu_sum=" << metrics.thread_cpu_sum << "\n";
            std::cout.flush();
        }

        // 泛型参数：Metrics 的字段既有 double 也有 std::size_t。
        auto median = [&runs](auto Metrics::*field) {
            std::vector<double> values;
            values.reserve(runs.size());
            for (const Metrics &metrics : runs)
                values.push_back(static_cast<double>(metrics.*field));
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        };
        std::cout << "MEDIAN throughput_rps=" << median(&Metrics::throughput_rps) << " mbps=" << median(&Metrics::mbps)
                  << " p50_us=" << median(&Metrics::p50_us) << " p90_us=" << median(&Metrics::p90_us)
                  << " p99_us=" << median(&Metrics::p99_us) << " max_us=" << median(&Metrics::max_us)
                  << " server_cpu_s=" << median(&Metrics::server_cpu_s)
                  << " server_cpu_per_req_us=" << median(&Metrics::cpu_per_request_us)
                  << " client_cpu_s=" << median(&Metrics::client_cpu_s)
                  << " peak_rss_kb=" << median(&Metrics::peak_rss_kb)
                  // 统计口径要把测量内部的墙钟一并给出：外层脚本的墙钟含进程启动与预热，
                  // 用它算"占几个核"会系统性偏低。
                  << " wall_s=" << median(&Metrics::wall_seconds) << "\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << "\n";
        alarm(0);
        std::error_code ignored;
        std::filesystem::remove_all(temp_dir, ignored);
        return 1;
    }

    alarm(0);
    std::error_code ignored;
    std::filesystem::remove_all(temp_dir, ignored);
    return 0;
}
