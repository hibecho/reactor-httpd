# 日志模块

`include/Logger.hpp` 提供单例 `Logger::Instance()`，实现位于 `src/Logger.cc`。兼容 C++11，本机使用 spdlog 1.12。

```cpp
#include "Logger.hpp"

int main()
{
    Logger::Config config;
    config.console = true;
    config.file_path = "logs/http.log";
    config.level = Logger::Level::Debug;
    Logger::Instance().Init(config);

    LOG_INFO("server listening on {}", 8080);
    LOG_DEBUG("connection id: {}", 12);
    LOG_ERROR("request failed: {}", "timeout");

    Logger::Instance().Flush();
    Logger::Instance().Shutdown();
}
```

无需显式初始化即可输出控制台 info 日志。宏提供 `LOG_TRACE`、`LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERROR`、`LOG_CRITICAL`，采用 `{}` 格式化，记录调用处文件名和行号。外部字符串使用 `LOG_INFO("{}", message)`，不要将其直接作为格式模板。

全项目只保留 `LOG_*` 这一个日志出口：业务模块直接调用宏，不要为转发日志自建包装函数（`Connection.cc` 早期的 `ReportFailure` 已删除），也不要在调用点自己包 `try/catch`——宏已保证不抛；同时不要直接调用 `SPDLOG_*`，那会落到 spdlog 的默认日志器，绕过本模块的等级、格式与文件配置。

配置项：

| 字段 | 默认值与含义 |
| --- | --- |
| console | true，彩色控制台输出 |
| file_path | 空，禁用文件输出；非空启用滚动文件 |
| max_file_size | 5 MiB，单文件滚动阈值 |
| max_files | 3 个备份，不含当前文件；允许 1..200000 |
| level | info，最低输出级别 |
| flush_level | err，error 及以上自动刷新；off 禁用自动刷新 |
| pattern | 时间、级别、进程、线程、文件名及行号、消息 |

至少启用一个输出目标；文件输出启用时滚动大小必须大于零。文件追加写入，备份名例如 `http.1.log`，超过备份数量后淘汰最旧文件。单条消息超过阈值时文件可能暂时超过设定大小。

`SetLevel(Logger::Level::Debug)` 动态调整级别，`GetLevel()` 查询级别，`ShouldLog(level)` 判断是否启用；off 关闭所有级别。宏不受 `SPDLOG_ACTIVE_LEVEL` 影响，被过滤时不求值参数。并发修改配置时以实际写入时级别为准，不要依赖参数副作用。

`Init(config)` 支持重新配置，失败保留原日志器；非法配置抛 `std::invalid_argument`，文件打开失败抛 `spdlog::spdlog_ex`，调用方应处理初始化异常。初始化可能创建目录或文件，失败不回滚这些文件系统副作用。运行时输出错误沿用 spdlog 默认 stderr 错误处理；分配异常仍可能传播。

模块采用同步输出，在调用日志的线程中完成格式化和输出，互斥锁保证多线程调用安全；写文件可能阻塞业务线程。不要在自定义 fmt formatter 中递归调用 Logger。模块不修改 spdlog 默认日志器及全局注册表，不与直接的 `SPDLOG_*` 调用共享配置；避免其他日志器同时滚动同一文件。

`Flush()` 直接刷新用户态缓冲，不保证 fsync 级别持久化。建议业务线程退出后调用 `Shutdown()`，刷新并释放日志输出资源；关闭后日志静默，重复关闭安全，`Init()` 可重新启用。关闭状态下 SetLevel 无操作，GetLevel 返回 off。不要在进程静态对象析构期间调用本单例。

构建及验证：

```sh
make -C Http
./Http/Test/main
make -C Http check
# 仅日志测试
make -C Http/Test check-logger
```

手动构建时将 `Http/src/Logger.cc` 加入源文件，添加 `-IHttp/include -std=c++11 -DSPDLOG_COMPILED_LIB`，链接 `-lspdlog -lfmt -pthread`。需要系统安装 spdlog 与 fmt 开发文件。

等级由模块独立定义：`Logger::Level::Trace / Debug / Info / Warn / Error / Critical / Off`。`Config::level`、`Config::flush_level`、`SetLevel`、`GetLevel` 和 `ShouldLog` 均使用该枚举，内部显式转换为 spdlog 等级。旧的 `spdlog::level::*` 配置写法需改用上述枚举。

正常进程退出时单例析构也会尝试刷新并关闭；异常终止不能保证缓冲中的日志写出。
