# 日志模块

接口位于 `Http/include/base/Logger.hpp`，实现位于 `Http/src/Logger.cc`。队列、消费线程与日志器全部复用项目自带 spdlog v1.12 的实现，不修改 spdlog 默认日志器或全局注册表。

## 使用

```cpp
#include "base/Logger.hpp"

Logger::Config config;
config.file_path = "logs/http.log";
config.level = Logger::Level::Debug;
// config.async = false; // 如需同步输出，在初始化前设置；默认 true。
Logger::Instance().Init(config);

LOG_INFO("server listening on {}", 8080);
LOG_ERROR("request failed: {}", "timeout");
Logger::Instance().Flush();    // 等待此前已提交日志处理并刷新
Logger::Instance().Shutdown(); // 业务线程停止记录日志后，排空并回收后台线程
```

无需显式初始化，第一次调用就会建立默认异步控制台日志器。支持 `LOG_TRACE`、`LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERROR`、`LOG_CRITICAL`，保留源码文件名和行号。外部字符串用 `LOG_INFO("{}", message)`，不要把它作为格式模板。不要直接调用 `SPDLOG_*`，那会落到 spdlog 的默认日志器，绕过本模块的配置。

## 配置

| 字段 | 默认值与含义 |
| --- | --- |
| async | true，独立单后台线程输出；false 使用调用线程同步输出 |
| queue_size | 8192 条；异步队列容量，满时阻塞，不主动覆盖或丢弃；同步模式忽略 |
| console | true，彩色控制台输出 |
| file_path | 空，禁用文件；非空启用滚动文件 |
| max_file_size | 5 MiB，单文件滚动阈值 |
| max_files | 3 个备份，不含当前文件；范围 1..200000 |
| level | Info，最低输出等级 |
| flush_level | Error，error 及以上触发后端刷新；Off 禁用自动刷新 |
| pattern | 时间、级别、进程、记录日志的线程、源码位置及消息 |

异步容量必须大于零，且不能为 `SIZE_MAX`（底层循环队列另需一个槽位）。至少启用一个输出目标；启用文件时滚动大小必须大于零。文件以追加方式打开，备份例如 `http.1.log`；超出备份数量会淘汰最旧文件。单条消息超过阈值时文件可能暂时超过指定大小。

等级为模块自己的 `Logger::Level::Trace / Debug / Info / Warn / Error / Critical / Off`，取值与 spdlog 等级一一对应，转换在模块内部完成。`SetLevel` 动态修改，`GetLevel` 查询，`ShouldLog` 判断是否启用；Off 关闭所有级别。全部级别均编译保留，不受 `SPDLOG_ACTIVE_LEVEL` 影响。

## 同步、异步与刷新

同步模式在调用线程完成格式化、输出及所需自动刷新。异步模式仍在调用线程求值参数、格式化和复制消息，由专用线程执行输出及滚动文件操作；小队列或慢输出可能使生产者阻塞，因此异步不等于永不阻塞。

`Flush()` 等待此前已提交的日志完成处理并刷新用户态缓冲，但不执行 fsync。异步模式下这是真屏障：spdlog 的 `async_logger::flush` 本身只入队就返回，模块用一块只做通知的 sink 挂在独立日志器上补齐完成信号，所以 `Flush()` 返回后可以立刻读到已写入的内容。自动刷新（`flush_level`）也由后台线程执行。

`Init(config)` 支持同步/异步切换；切换前等待旧队列排空再建立新输出，避免同一路径重建时旧队列仍在写，切换过程不丢已提交的日志。初始化失败保留旧日志器：非法配置抛 `invalid_argument`，文件打开失败抛 `spdlog_ex`。已创建的目录或文件等文件系统副作用不回滚。

`Shutdown()` 排空、刷新并 join 后台线程，重复调用安全。关闭后日志静默，`SetLevel` 无操作，`GetLevel` 返回 Off，`Init` 可重新启用。正常进程退出时单例析构同样尝试关闭；异常终止、`_exit` 不保证日志写出。不要在其他静态对象析构期间调用本单例。

## 并发、异常与信号

配置、写入、刷新和关闭由模块互斥锁串行化。全部 `LOG_*` 保留不抛异常契约，保护单例获取、等级判断、参数求值和后端调用；被过滤或关闭时不求值参数。参数不能承担业务副作用。配置接口不吞异常。

创建日志线程时临时屏蔽调用线程的可屏蔽信号，让后台线程在创建时继承屏蔽状态，再恢复调用线程原掩码，避免它抢走主线程或 signalfd 处理的 SIGINT、SIGTERM。模块不修改其他既有线程的掩码，也不安装信号处理器。

## 构建与验证

```sh
make -C Http
make -C Http/Test check-logger
make -C Http/Test check-logger-sanitize
make -C Http/Test check
```

构建使用 `Http/spdlog.mk` 中的本地头文件、`SPDLOG_COMPILED_LIB`、静态库和 pthread 参数，统一按 C++17 严格警告配置构建。若运行环境受 ptrace 跟踪，LeakSanitizer 可能报告不支持该环境，此时用 `ASAN_OPTIONS=detect_leaks=0` 单独验证 ASan/UBSan；该结果不含泄漏检测。
