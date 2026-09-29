# reactor-httpd

基于 Reactor 模式的高性能 HTTP 服务器，采用 C++17 实现。

## 项目简介

reactor-httpd 是一个轻量级 HTTP 服务器，核心采用 I/O 多路复用（epoll）机制实现事件驱动架构。项目包含完整的 HTTP 协议解析、静态文件服务、连接管理和定时器功能，并经过系统性性能测试与优化。

## 核心特性

- **Reactor 事件驱动**：基于 epoll 的高效事件循环
- **多线程支持**：支持配置工作线程池提升并发能力
- **HTTP 协议完整实现**：支持请求解析、响应构建、Keep-Alive 等
- **静态文件服务**：内置静态资源服务与 MIME 类型识别
- **连接管理**：支持连接超时、自动释放、背压控制
- **日志系统**：支持同步/异步日志模式，基于 spdlog
- **定时器轮**：基于时间轮的高效定时任务管理
- **资源限制**：可配置连接数、缓冲区大小等资源上限

## 目录结构

```
Http/
├── include/          # 头文件
│   ├── base/         # 基础组件（Buffer, Logger, Util, FdGuard）
│   ├── protocol/     # HTTP 协议（HttpContext, HttpRequest, HttpResponse）
│   ├── reactor/      # 反应堆核心（EventLoop, Epoller, Channel, TimerQueue）
│   ├── tcp/          # TCP 层（Socket, Acceptor, Connection, TcpServer）
│   └── thread/       # 线程管理（LoopThread, LoopThreadPool）
├── src/              # 源代码实现
├── Test/             # 单元测试与集成测试
├── www/              # 静态资源（HTML, CSS）
├── CMakeLists.txt    # CMake 构建配置
├── Makefile          # Make 构建配置
└── README.md         # 构建与测试说明
```

## 快速开始

### 环境要求

- C++17 兼容编译器（GCC 9+ / Clang 10+）
- CMake 3.15+
- Linux 系统（依赖 epoll）

### 构建项目

```bash
# 进入项目目录
cd Http

# 使用 CMake 构建
mkdir build && cd build
cmake ..
make

# 或使用 Makefile
cd Http
make
```

### 运行服务器

```bash
# 默认端口 8080
./HttpServer

# 指定端口
./HttpServer --port 9090
```

### 运行测试

```bash
cd Http/Test
make
./TestBasic
./TestHttpServer
# ... 其他测试用例
```

## 性能测试

项目包含完整的性能测试报告，参见：

- `tasks/perf-report.md` - 单线程性能分析
- `tasks/perf-threads-report.md` - 多线程性能分析

关键性能指标（4核共享机器，回环测试）：

- 并发扫描：`conns=16, depth=32` 场景下可达数十万 QPS
- 延迟表现：一问一答场景延迟可控制在微秒级

## 配置选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `--port` | HTTP 服务端口 | 8080 |
| `--threads` | 工作线程数 | 1 |
| `--doc-root` | 静态文件根目录 | ./www |
| `--grace` | 优雅退出等待时间(ms) | 3000 |

## 模块说明

### 核心层

- **EventLoop**：事件循环引擎，管理 epoll 实例和任务队列
- **Epoller**：epoll 封装，处理 I/O 事件监控
- **Channel**：通道封装，关联文件描述符与回调
- **TimerWheel**：时间轮定时器，高效管理定时任务

### 网络层

- **Socket**：Socket 封装，提供创建、绑定、监听、连接等操作
- **Acceptor**：接受连接，负责监听描述符管理
- **Connection**：连接管理，处理读写、背压、协议切换
- **TcpServer**：TCP 服务器，整合所有组件

### 协议层

- **HttpContext**：HTTP 解析上下文
- **HttpRequest**：HTTP 请求结构
- **HttpResponse**：HTTP 响应构建
- **HttpServer**：HTTP 服务器入口

### 工具层

- **Buffer**：高效字节缓冲区，支持预读和行解析
- **Logger**：日志系统，支持同步/异步模式
- **Util**：工具函数（URL 编解码、文件操作、MIME 类型等）

## 许可证

本项目仅供学习参考使用。