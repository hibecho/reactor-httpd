# 构建与测试

需要 Linux、C++17 编译器、CMake 3.20+、Make 和 pthread。
CMake 是编译及依赖规则的唯一来源；Makefile 仅提供快捷入口。

## 常用命令

在 `Http/` 目录执行：

```sh
make JOBS=2               # Debug 服务器
make release JOBS=2       # Release 服务器
make check JOBS=2         # 构建并运行全部 25 个测试
make check-socket         # 按 CTest 标签运行单组测试
make check-sanitize       # ASan/UBSan 构建及全量测试
make check-socket-sanitize
make bench                # HTTP 压测客户端
make bench-logger         # 独立 -O2 日志基准
make server               # TestServer 示例
make echoserver           # EchoServer 示例
```

`Http/Test/` 下的 `make check` 等入口仍有效。编译并行度由 `JOBS` 控制，
默认 2；测试默认顺序运行。`make -j` 不负责设置 CMake 内部编译并行度。

构建目录分别为 `.build/debug`、`.build/release`、`.build/sanitize`，
可执行文件在各目录的 `bin/` 下。同一配置的服务器与测试共用核心静态库。
`make all` / `make release` 更新 `.build/HttpServer` 链接，
`make all-sanitize` 更新 `.build/HttpServer-sanitize`；示例及基准也保留
`.build/BenchLogger` 等快捷链接。测试直接使用所属配置的真实路径。
旧手写 Makefile 的对象文件不再使用；迁移不会主动清除旧产物。

## 直接使用 CMake

```sh
cmake -S . -B .build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/debug --target http_tests --parallel 2
ctest --test-dir .build/debug --output-on-failure

cmake -S . -B .build/release -DCMAKE_BUILD_TYPE=Release
cmake --build .build/release --parallel 2

cmake -S . -B .build/sanitize -DCMAKE_BUILD_TYPE=Debug -DHTTP_ENABLE_SANITIZERS=ON
cmake --build .build/sanitize --target http_tests --parallel 2
ctest --test-dir .build/sanitize --output-on-failure
```

CTest 不自动编译，因此先构建 `http_tests`。单组运行使用 `ctest -L '^socket$'`。
`-DBUILD_TESTING=OFF` 可关闭测试目标，示例和基准仍可按目标单独构建。
`compile_commands.json` 自动生成于构建目录，可供 IDE 使用。
警告、C++17 与断言设置仅施加于项目目标，不污染第三方库。
故障注入测试保留 GNU 链接器 `--wrap` 参数；`TestBufferAllocation` 在
Sanitizer 配置下仍不启用插桩，因为它覆盖分配函数。

## 依赖与资源

优先使用已有 `third_party/spdlog` 源码；缺失时由 FetchContent 获取固定提交
`7e635fca68d014934b4af8a1cf874f63989352b7`（v1.12.0），首次配置需要联网。
离线环境可提前准备该源码目录。依赖在各构建目录内编译，关闭上游示例及测试。
原 `spdlog.mk` 已合并到 CMake，无需单独维护。

服务器仍需在 `Http/` 目录运行，以便读取 `./www`；CTest 已为 E2E 配置该工作目录。

`make clean MODE=debug` 清除指定配置的编译产物，保留配置缓存；
`make clean-all` 清除三种配置的编译产物。由于核心对象现在共享，
`clean-tests` 是当前配置清理的别名，不再只清理测试对象。
可使用 `BUILD_DIR=/path/to/build` 指定快捷命令的构建根目录，
`CMAKE_ARGS=...` 传入额外 CMake 配置选项。

自定义编译器和参数请使用，例如：

```sh
make BUILD_DIR=.build-clang CMAKE_ARGS="-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-g"
```

旧 `make CXX=... CXXFLAGS=...` 不再直接控制编译；更换编译器使用新的构建目录。

若运行环境受 ptrace 监控，LeakSanitizer 可能报告无法运行；可临时执行
`ASAN_OPTIONS=detect_leaks=0 make check-sanitize` 验证 ASan/UBSan，
此结果不代表泄漏检测通过。项目默认保留泄漏检测。
