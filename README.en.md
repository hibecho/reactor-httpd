# reactor-httpd

High-performance HTTP server based on the Reactor pattern, implemented in C++17.

## Project Introduction

reactor-httpd is a lightweight HTTP server that utilizes an event-driven architecture centered around I/O multiplexing (epoll). The project includes complete HTTP protocol parsing, static file serving, connection management, and timer functionality, all subjected to systematic performance testing and optimization.

## Core Features

- **Reactor Event-Driven**: Efficient event loop based on epoll
- **Multi-threading Support**: Configurable worker thread pool to enhance concurrency
- **Complete HTTP Protocol Implementation**: Supports request parsing, response building, Keep-Alive, etc.
- **Static File Serving**: Built-in static resource serving with MIME type identification
- **Connection Management**: Supports connection timeout, automatic release, backpressure control
- **Logging System**: Supports synchronous/asynchronous logging modes, based on spdlog
- **Timer Wheel**: Efficient scheduled task management based on a time wheel
- **Resource Limits**: Configurable resource limits such as number of connections, buffer size, etc.

## Directory Structure

```
Http/
├── include/          # Header files
│   ├── base/         # Basic components (Buffer, Logger, Util, FdGuard)
│   ├── protocol/     # HTTP protocol (HttpContext, HttpRequest, HttpResponse)
│   ├── reactor/      # Reactor core (EventLoop, Epoller, Channel, TimerQueue)
│   ├── tcp/          # TCP layer (Socket, Acceptor, Connection, TcpServer)
│   └── thread/       # Thread management (LoopThread, LoopThreadPool)
├── src/              # Source code implementation
├── Test/             # Unit tests and integration tests
├── www/              # Static resources (HTML, CSS)
├── CMakeLists.txt    # CMake build configuration
├── Makefile          # Make build configuration
└── README.md         # Build and test instructions
```

## Quick Start

### Environment Requirements

- C++17 compatible compiler (GCC 9+ / Clang 10+)
- CMake 3.15+
- Linux system (depends on epoll)

### Building the Project

```bash
# Enter project directory
cd Http

# Build using CMake
mkdir build && cd build
cmake ..
make

# Or use Makefile
cd Http
make
```

### Running the Server

```bash
# Default port 8080
./HttpServer

# Specify port
./HttpServer --port 9090
```

### Running Tests

```bash
cd Http/Test
make
./TestBasic
./TestHttpServer
# ... Other test cases
```

## Performance Testing

The project includes a complete performance test report, see:

- `tasks/perf-report.md` - Single-threaded performance analysis
- `tasks/perf-threads-report.md` - Multi-threaded performance analysis

Key Performance Indicators (4-core shared machine, loopback test):

- Concurrency Scan: Up to hundreds of thousands of QPS under `conns=16, depth=32` scenario
- Latency Performance: Request-response latency can be controlled at the microsecond level

## Configuration Options

| Option | Description | Default Value |
|------|------|--------|
| `--port` | HTTP service port | 8080 |
| `--threads` | Number of worker threads | 1 |
| `--doc-root` | Static file root directory | ./www |
| `--grace` | Graceful shutdown wait time (ms) | 3000 |

## Module Description

### Core Layer

- **EventLoop**: EventLoop engine, manages epoll instance and task queue
- **Epoller**: epoll wrapper, handles I/O event monitoring
- **Channel**: Channel wrapper, associates file descriptors with callbacks
- **TimerWheel**: Time wheel timer, efficiently manages scheduled tasks

### Network Layer

- **Socket**: Socket wrapper, provides operations like creation, binding, listening, connecting
- **Acceptor**: Accepts connections, responsible for listening descriptor management
- **Connection**: Connection management, handles read/write, backpressure, protocol switching
- **TcpServer**: TCP server, integrates all components

### Protocol Layer

- **HttpContext**: HTTP parsing context
- **HttpRequest**: HTTP request structure
- **HttpResponse**: HTTP response construction
- **HttpServer**: HTTP server entry point

### Utility Layer

- **Buffer**: High-efficiency byte buffer, supports prefetching and line parsing
- **Logger**: Logging system, supports synchronous/asynchronous modes
- **Util**: Utility functions (URL encoding/decoding, file operations, MIME types, etc.)

## License

This project is for learning and reference purposes only.