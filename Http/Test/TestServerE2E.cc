// 进程级端到端测试：用 fork/exec 启动真实的 HttpServer 二进制，
// 只通过 TCP 与进程退出码观察它。本文件不链接任何项目源码——测的是二进制，不是模块。
//
// 与 TestHttpServer.cc 的分工：那个测试在进程内构造 HttpServer 对象，覆盖协议语义与路由；
// 这里覆盖进程内测试无法触及的部分：main.cc 的参数校验与退出码、客户端行为异常时的存活情况、
// 真实并发下的正确性，以及子进程的描述符回收。
//
// 一处需要说明的边界：main.cc 里的 SIGPIPE 忽略**测不出来**。Socket::Send 本来就带
// MSG_NOSIGNAL，且 RST 之后第一次写拿到的是 ECONNRESET（不是唯一会触发 SIGPIPE 的 EPIPE），
// 加上 HandleWrite 的错误分支立刻置位释放标志、不会有第二次写，所以那条防护是不可达的。
// 详见 G3 中 RST 用例的注释。
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::atomic<const char *> g_stage{"startup"};
static_assert(std::atomic<const char *>::is_always_lock_free, "signal handler needs a lock-free stage pointer");

void OnAlarm(int)
{
    const char *prefix = "TestServerE2E timed out: ";
    const char *stage = g_stage.load();
    (void)!write(STDERR_FILENO, prefix, strlen(prefix));
    (void)!write(STDERR_FILENO, stage, strlen(stage));
    (void)!write(STDERR_FILENO, "\n", 1);
    _exit(2);
}

void Stage(const char *stage)
{
    g_stage.store(stage);
}

std::string TempDir()
{
    char path[] = "/tmp/http-e2e-XXXXXX";
    char *created = mkdtemp(path);
    assert(created != nullptr);
    return created;
}

std::string ReadFile(const std::string &path)
{
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool ExitedWith(int status, int code)
{
    return WIFEXITED(status) != 0 && WEXITSTATUS(status) == code;
}

// 取一个当前空闲的端口：绑定后立刻释放，再把端口号交给子进程。
// 故意不设 SO_REUSEPORT——服务器的监听套接字会设，若探测方也设就检测不出冲突了。
uint16_t PickFreePort()
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
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
        // 1024 以下需要特权；8080 是本项目的默认端口，可能已有别的服务在跑。
        if (port >= 1024 && port != 8080)
            return port;
    }
    assert(false && "no free port available");
    return 0;
}

int Connect(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    timeval timeout{10, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
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

int ConnectOrDie(uint16_t port)
{
    const int fd = Connect(port);
    assert(fd >= 0);
    return fd;
}

// 带一个很小的接收缓冲连接：服务端写不完响应，因此会一直保持"有数据待发"的状态。
// 用于验证客户端在服务端写入过程中断开时的行为。
int ConnectWithTinyBuffer(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    const int small = 4096;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
    timeval timeout{10, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    assert(connected == 0);
    return fd;
}

// 非断言版本：并发线程里用它，失败只返回 false 由调用方计数，避免在子线程里直接中止进程。
bool SendAll(int fd, const std::string &data)
{
    std::size_t offset = 0;
    while (offset != data.size())
    {
        const ssize_t count = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

struct Response
{
    std::string version;
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

bool ReadResponse(int fd, Response &response, bool head = false)
{
    std::string header;
    while (header.size() < 4 || header.compare(header.size() - 4, 4, "\r\n\r\n") != 0)
    {
        char byte = 0;
        if (recv(fd, &byte, 1, 0) != 1)
            return false;
        header += byte;
        if (header.size() >= 65536)
            return false;
    }

    std::istringstream input(header);
    input >> response.version >> response.status;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line) && line != "\r")
    {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            return false;
        std::string name = line.substr(0, colon);
        for (char &c : name)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        std::size_t start = colon + 1;
        while (start < line.size() && line[start] == ' ')
            ++start;
        response.headers[name] = line.substr(start, line.size() - start - 1);
    }

    if (head)
        return true;
    const auto length = response.headers.find("content-length");
    if (length == response.headers.end())
        return true;
    response.body.resize(std::stoul(length->second));
    std::size_t offset = 0;
    while (offset < response.body.size())
    {
        const ssize_t count = recv(fd, response.body.data() + offset, response.body.size() - offset, 0);
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

// 断言版本：单线程用例使用。驱动动作先取值再断言，不用副作用写在 assert 里。
void Send(int fd, const std::string &data)
{
    const bool sent = SendAll(fd, data);
    assert(sent);
}

Response Receive(int fd, bool head = false)
{
    Response response;
    const bool read = ReadResponse(fd, response, head);
    assert(read);
    return response;
}

void Eof(int fd)
{
    char byte = 0;
    const ssize_t count = recv(fd, &byte, 1, 0);
    assert(count == 0);
    close(fd);
}

std::string Request(const std::string &method, const std::string &path, bool close = true)
{
    return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" + (close ? "Connection: close\r\n" : "") + "\r\n";
}

std::string PostRequest(const std::string &path, const std::string &body, bool close = true)
{
    return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
           (close ? "Connection: close\r\n" : "") + "\r\n" + body;
}

std::size_t CountChildFds(pid_t pid)
{
    const std::string path = "/proc/" + std::to_string(static_cast<long>(pid)) + "/fd";
    DIR *handle = opendir(path.c_str());
    if (handle == nullptr)
        return 0;
    std::size_t count = 0;
    while (readdir(handle) != nullptr)
        ++count;
    closedir(handle);
    return count;
}

// 等描述符数量稳定，避免把"还在延迟释放中"读成泄漏。
std::size_t WaitStableFds(pid_t pid, int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::size_t previous = CountChildFds(pid);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::size_t current = CountChildFds(pid);
        if (current == previous)
            return current;
        previous = current;
    }
    return previous;
}

// 被测服务器进程。析构时兜底终止并回收，避免用例失败后留下孤儿进程。
class ServerProcess
{
  public:
    ServerProcess(const std::string &binary, const std::vector<std::string> &args, const std::string &log_path)
        : _log_path(log_path)
    {
        const pid_t parent = getpid();
        _pid = fork();
        assert(_pid >= 0);
        if (_pid != 0)
            return;

        // 父进程若因断言失败而中止，RAII 析构根本不会执行，子进程就会变成孤儿服务器一直占着端口。
        // 让内核在父进程消失时直接杀掉子进程。fork 与 prctl 之间存在父进程已经退出的窗口，
        // 那时 PDEATHSIG 不会再触发，所以紧接着核对一次父进程身份。
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);

        // 父进程为自保忽略了 SIGPIPE，而"忽略"这种处置会跨越 exec 传给子进程，
        // 于是被测程序自己的 SIGPIPE 处理形同虚设——无论它有没有设置，行为都一样。
        // 必须在这里显式恢复默认处置，子进程才是从干净状态启动的。
        (void)signal(SIGPIPE, SIG_DFL);

        // 子进程：stdout/stderr 一并写入日志文件，既避免与父进程输出交错，
        // 也让"子进程是否打出 sanitizer 报告"成为可断言的观测点。
        const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0)
        {
            (void)dup2(log, STDOUT_FILENO);
            (void)dup2(log, STDERR_FILENO);
            close(log);
        }
        std::vector<char *> argv;
        argv.reserve(args.size() + 2);
        std::string program = binary;
        argv.push_back(program.data());
        for (const std::string &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execv(program.c_str(), argv.data());
        _exit(127);
    }

    ~ServerProcess()
    {
        Stop();
    }

    ServerProcess(const ServerProcess &) = delete;
    ServerProcess &operator=(const ServerProcess &) = delete;

    pid_t Pid() const
    {
        return _pid;
    }
    const std::string &LogPath() const
    {
        return _log_path;
    }

    // 轮询 connect 直到可连；子进程先退出则返回 false。
    bool WaitReady(uint16_t port, int seconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (TryReap())
                return false;
            const int fd = Connect(port);
            if (fd >= 0)
            {
                close(fd);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool WaitExit(int &status, int seconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        for (;;)
        {
            if (TryReap())
            {
                status = _status;
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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
        {
            _status = status;
            _reaped = true;
        }
    }

  private:
    bool TryReap()
    {
        if (_reaped)
            return true;
        int status = 0;
        if (waitpid(_pid, &status, WNOHANG) != _pid)
            return false;
        _status = status;
        _reaped = true;
        return true;
    }

    pid_t _pid = -1;
    int _status = 0;
    bool _reaped = false;
    std::string _log_path;
};

struct Running
{
    uint16_t port = 0;
    std::string log_path;
    std::unique_ptr<ServerProcess> process;
};

// 启动服务器；端口偶尔会被别的进程抢走，因此失败就换端口重试。
Running StartServer(const std::string &binary, const std::string &temp_dir, const std::string &name, int seconds)
{
    Stage("starting server");
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        Running running;
        running.port = PickFreePort();
        running.log_path = temp_dir + "/" + name + "-" + std::to_string(attempt) + ".log";
        running.process = std::make_unique<ServerProcess>(
            binary, std::vector<std::string>{std::to_string(running.port)}, running.log_path);
        if (running.process->WaitReady(running.port, seconds))
            return running;
    }
    assert(false && "server did not become ready");
    return Running{};
}

// G1：启动参数与启动失败。这一组不需要服务器长时间运行。
void TestSignalShutdown(const std::string &binary, const std::string &temp_dir)
{
    alarm(30);
    for (const int signal : {SIGINT, SIGTERM})
    {
        Running server = StartServer(binary, temp_dir, "signal-" + std::to_string(signal), 10);
        assert(kill(server.process->Pid(), signal) == 0);
        int status = 0;
        assert(server.process->WaitExit(status, 10));
        assert(ExitedWith(status, 0));
    }
}

void TestStartup(const std::string &binary, const std::string &temp_dir)
{
    alarm(60);
    Stage("G1 非法参数");
    const std::vector<std::vector<std::string>> invalid = {
        {"0"}, {"65536"}, {"-1"}, {"abc"}, {"1.5"}, {""}, {" "}, {"8080", "extra"},
    };
    for (std::size_t i = 0; i < invalid.size(); ++i)
    {
        ServerProcess process(binary, invalid[i], temp_dir + "/arg-" + std::to_string(i) + ".log");
        int status = 0;
        assert(process.WaitExit(status, 10));
        assert(ExitedWith(status, 1));
        // 失败必须留下可读的提示，而不是静默退出。
        assert(!ReadFile(process.LogPath()).empty());
    }

    Stage("G1 端口被占用");
    // 占用方必须用裸 socket 且不设 SO_REUSEPORT：服务器的监听套接字同时开了
    // SO_REUSEADDR 与 SO_REUSEPORT，占用方若也开，bind 不会失败、用例会静默失效。
    const uint16_t busy_port = PickFreePort();
    const int holder = socket(AF_INET, SOCK_STREAM, 0);
    assert(holder >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(busy_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(holder, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(listen(holder, 16) == 0);

    ServerProcess occupied(binary, {std::to_string(busy_port)}, temp_dir + "/occupied.log");
    int status = 0;
    assert(occupied.WaitExit(status, 10));
    assert(ExitedWith(status, 1));
    close(holder);
}

// G2：线上协议语义。全部通过真实 socket 驱动生产路由。
void TestLiveProtocol(const std::string &binary, const std::string &temp_dir)
{
    alarm(90);
    Running server = StartServer(binary, temp_dir, "live", 15);
    const uint16_t port = server.port;

    auto exchange = [&](const std::string &request, bool head = false) {
        const int client = ConnectOrDie(port);
        Send(client, request);
        Response response = Receive(client, head);
        Eof(client);
        return response;
    };

    Stage("G2 生产路由");
    Response response = exchange(Request("GET", "/"));
    assert(response.version == "HTTP/1.1" && response.status == 200);
    assert(response.headers.at("content-type") == "text/html");
    assert(response.body == ReadFile("./www/index.html"));

    // 二进制安全：正文含内嵌 NUL 与 CRLF，必须逐字节回显。
    const std::string payload("a\0b\r\nc", 6);
    response = exchange(PostRequest("/echo", payload));
    assert(response.status == 200 && response.body == payload);

    assert(exchange(Request("GET", "/missing")).status == 404);
    response = exchange(Request("POST", "/"));
    assert(response.status == 405 && response.headers.at("allow") == "GET, HEAD");

    response = exchange(Request("HEAD", "/"), true);
    assert(response.status == 200 && !response.headers.at("content-length").empty());

    Stage("G2 连接复用");
    {
        const int client = ConnectOrDie(port);
        for (int i = 0; i < 2; ++i)
        {
            Send(client, Request("GET", "/", false));
            const Response each = Receive(client);
            assert(each.status == 200 && each.headers.at("connection") == "keep-alive");
        }
        Send(client, Request("GET", "/"));
        assert(Receive(client).headers.at("connection") == "close");
        Eof(client);
    }

    Stage("G2 流水线");
    {
        const int client = ConnectOrDie(port);
        // 一次性写入三个请求，按序取回三个响应；最后一个要求关闭，才能验证连接被释放。
        Send(client,
             Request("GET", "/", false) + PostRequest("/echo", "first", false) + PostRequest("/echo", "second"));
        assert(Receive(client).status == 200);
        assert(Receive(client).body == "first");
        assert(Receive(client).body == "second");
        Eof(client);
    }

    Stage("G2 逐字节分片");
    {
        const int client = ConnectOrDie(port);
        const std::string wire = Request("GET", "/");
        for (const char byte : wire)
        {
            Send(client, std::string(1, byte));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(Receive(client).status == 200);
        Eof(client);
    }

    Stage("G2 HTTP/1.0");
    response = exchange("GET / HTTP/1.0\r\n\r\n");
    assert(response.version == "HTTP/1.0" && response.headers.at("connection") == "close");
    {
        const int client = ConnectOrDie(port);
        Send(client, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        assert(Receive(client).headers.at("connection") == "keep-alive");
        Send(client, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        assert(Receive(client).status == 200);
        close(client); // 显式 keep-alive 不会自行关闭，直接断开让服务端走对端关闭路径。
    }

    Stage("G2 畸形报文");
    const std::vector<std::pair<std::string, int>> malformed = {
        {"GET / HTTP/1.1\r\n\r\n", 400},                       // 缺 Host
        {"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400}, // 重复 Host
        {"GET / HTTP/1.1\nHost: a\n\n", 400},                  // 裸 LF
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n", 417},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 8388609\r\n\r\n", 413},
        {"GET /" + std::string(8200, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n", 414},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(32768, 'a') + "\r\n\r\n", 431},
    };
    for (const auto &entry : malformed)
    {
        response = exchange(entry.first);
        assert(response.status == entry.second);
        // 解析错误一律要求关闭连接，且带上可读的错误正文。
        assert(response.headers.at("connection") == "close");
        assert(!response.body.empty());
    }

    Stage("G2 收尾");
    server.process->Stop();
}

// G3：客户端行为异常时服务端必须存活。每组结束后都用新连接做一次探针。
void TestClientMisbehavior(const std::string &binary, const std::string &temp_dir)
{
    alarm(90);
    Running server = StartServer(binary, temp_dir, "misbehave", 15);
    const uint16_t port = server.port;

    auto probe = [&]() {
        const int client = ConnectOrDie(port);
        Send(client, Request("GET", "/"));
        assert(Receive(client).status == 200);
        Eof(client);
    };

    Stage("G3 连上即断");
    for (int i = 0; i < 20; ++i)
    {
        const int client = ConnectOrDie(port);
        close(client);
    }
    probe();

    Stage("G3 半个请求后断开");
    for (int i = 0; i < 20; ++i)
    {
        const int client = ConnectOrDie(port);
        Send(client, "GET / HTTP/1.1\r\nHost: local");
        close(client);
    }
    probe();

    Stage("G3 声明长度大于实发");
    for (int i = 0; i < 10; ++i)
    {
        const int client = ConnectOrDie(port);
        Send(client, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1000000\r\n\r\nonly-a-few-bytes");
        close(client);
    }
    probe();

    Stage("G3 同一连接内多次触发");
    for (int i = 0; i < 10; ++i)
    {
        const int client = ConnectOrDie(port);
        Send(client, "GET / HTTP/1.1\r");
        Send(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        close(client);
    }
    probe();

    Stage("G3 RST 断开");
    // 构造：把客户端的接收缓冲压到最小（4 KiB）、用上限大小的正文把响应撑到 8 MiB，
    // 让服务端在 RST 到达时**确实还有数据待发**，而不是早已写完。
    //
    // 实测这条路径的落点（服务端日志里 send 失败的 errno）：三轮 RST 各打出一条
    // "Failed to send: errno=104"，即 ECONNRESET，也就是真的走到了写路径。
    //
    // 但这条用例**证明不了 SIGPIPE 防护有效**，实测两处防护一并去掉、进程依然存活。原因已定位：
    // Linux 上只有 EPIPE 才带 SIGPIPE，peer 复位后第一次写拿到的是 ECONNRESET；
    // 而 HandleWrite 的错误分支立刻调用 HandleError 置位 _release_pending，
    // 后续写回调在函数开头就被守卫挡掉，**不会有第二次写**，EPIPE 于是永远不出现。
    // 因此 Socket::Send 的 MSG_NOSIGNAL 与 main.cc 里的 SIGPIPE 忽略在本项目的当前结构下
    // 都是不可达的纵深防御——它们不是错的，只是没有测试能覆盖，这里如实标注。
    for (int i = 0; i < 3; ++i)
    {
        const int client = ConnectWithTinyBuffer(port);
        const std::string body(8 * 1024 * 1024, 'z');
        Send(client, PostRequest("/echo", body));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        linger reset{};
        reset.l_onoff = 1;
        reset.l_linger = 0;
        assert(setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
        close(client);
    }
    probe();

    Stage("G3 收尾");
    server.process->Stop();
}

// G4：并发与资源。服务端是单线程事件循环，这组是对该模型的真实压力。
void TestConcurrencyAndResources(const std::string &binary, const std::string &temp_dir)
{
    alarm(180);
    Running server = StartServer(binary, temp_dir, "concurrent", 20);
    const uint16_t port = server.port;
    const pid_t pid = server.process->Pid();

    Stage("G4 fd 基线");
    {
        const int client = ConnectOrDie(port);
        Send(client, Request("GET", "/"));
        assert(Receive(client).status == 200);
        Eof(client);
    }
    const std::size_t baseline = WaitStableFds(pid, 10);

    Stage("G4 并发 keep-alive");
    constexpr int kClients = 32;
    constexpr int kRequests = 20;
    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int index = 0; index < kClients; ++index)
    {
        clients.emplace_back([&, index] {
            const int client = Connect(port);
            if (client < 0)
            {
                ++failures;
                return;
            }
            const std::string body = "client-" + std::to_string(index) + "-payload";
            for (int round = 0; round < kRequests; ++round)
            {
                const bool last = (round + 1 == kRequests);
                Response response;
                if (!SendAll(client, PostRequest("/echo", body, last)) || !ReadResponse(client, response) ||
                    response.status != 200 || response.body != body)
                {
                    ++failures;
                    close(client);
                    return;
                }
            }
            close(client);
        });
    }
    for (std::thread &client : clients)
        client.join();
    assert(failures.load() == 0);

    Stage("G4 混合负载");
    {
        // 一半正常、一半畸形、一部分发一半就断：正常请求必须全部成功，进程必须存活。
        std::atomic<int> mixed_failures{0};
        std::vector<std::thread> mixed;
        mixed.reserve(24);
        for (int index = 0; index < 24; ++index)
        {
            mixed.emplace_back([&, index] {
                const int client = Connect(port);
                if (client < 0)
                {
                    ++mixed_failures;
                    return;
                }
                if (index % 3 == 0)
                {
                    Response response;
                    if (!SendAll(client, Request("GET", "/")) || !ReadResponse(client, response) ||
                        response.status != 200)
                        ++mixed_failures;
                }
                else if (index % 3 == 1)
                {
                    Response response;
                    // 畸形请求应当拿到 400，而不是让服务端出问题。
                    if (!SendAll(client, "GET / HTTP/1.1\r\n\r\n") || !ReadResponse(client, response) ||
                        response.status != 400)
                        ++mixed_failures;
                }
                else
                {
                    SendAll(client, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999999\r\n\r\npartial");
                }
                close(client);
            });
        }
        for (std::thread &worker : mixed)
            worker.join();
        assert(mixed_failures.load() == 0);
    }

    Stage("G4 资源回收");
    const std::size_t after = WaitStableFds(pid, 15);
    std::cout << "[e2e] 子进程描述符条目：基线 " << baseline << "，结束后 " << after << "\n";
    assert(after <= baseline + 2);

    Stage("G4 子进程输出");
    server.process->Stop();
    int status = 0;
    assert(server.process->WaitExit(status, 10));
    // SIGTERM 经由正常停机路径返回，不能由信号直接终止或被兜底 SIGKILL。
    assert(ExitedWith(status, 0));

    // UBSan 默认不中止进程，只往 stderr 打一行，所以这条断言是它在子进程里唯一的观测点。
    const std::string log = ReadFile(server.log_path);
    for (const char *marker : {"AddressSanitizer", "runtime error", "LeakSanitizer", "UndefinedBehaviorSanitizer"})
        assert(log.find(marker) == std::string::npos);
}
} // namespace

int main(int argc, char *argv[])
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGALRM, OnAlarm);

    const std::string binary = argc > 1 ? argv[1] : "./.build/HttpServer";
    const std::string temp_dir = TempDir();

    int result = 0;
    try
    {
        TestStartup(binary, temp_dir);
        TestSignalShutdown(binary, temp_dir);
        TestLiveProtocol(binary, temp_dir);
        TestClientMisbehavior(binary, temp_dir);
        TestConcurrencyAndResources(binary, temp_dir);
        std::cout << "Server end-to-end tests passed\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << "\n";
        result = 1;
    }

    alarm(0);
    std::error_code ignored;
    std::filesystem::remove_all(temp_dir, ignored);
    return result;
}
