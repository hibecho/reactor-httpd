# 测试构建入口。
#
# 核心项目（.build/HttpServer）由 ../Makefile 负责，本文件不含核心构建规则，
# 只在 e2e 需要时通过 $(MAKE) -C .. 把核心二进制拉过来。
#
#   cd Test && make check          # 全部测试
#   cd Test && make check-socket   # 单个测试组
#   cd Test && make help           # 列出全部入口
#
# 编译选项与本文件的对象目录 .build/tobj{,-san} 均为独立一份，不与 ../Makefile 共享。
# 代价是 src/*.cc 在核心与测试两侧各编译一次；换来的是两份构建互不干扰、
# 改一处的选项不会污染另一侧的产物。若要消除重复，把两侧对象目录合并即可。

# 用 MAKEFILE_LIST 定位自身，保证从任意目录调用都得到正确路径。
TEST_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT := $(patsubst %/,%,$(dir $(TEST_DIR)))
BUILD_DIR := $(ROOT)/.build
OBJ_DIR := $(BUILD_DIR)/tobj
OBJ_SAN_DIR := $(BUILD_DIR)/tobj-san
LOGGER_BENCH_DIR := $(BUILD_DIR)/logger-bench

include $(ROOT)/spdlog.mk

CXX ?= g++
TEST_FLAGS = -std=c++17 -Wall -Wextra -Werror -pedantic -UNDEBUG
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-elide-constructors -g
TEST_CPPFLAGS = $(SPDLOG_CPPFLAGS) $(CPPFLAGS) -I$(ROOT)/include $(CXXFLAGS) $(TEST_FLAGS)

# 默认开满核：不指定 -j 时按 CPU 核数并行。命令行给出的 -j 优先，因此
# make -j1 单线程、make -j8 八线程都照常生效，只有完全不指定时才用这里的默认值。
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

.PHONY: help
help:
	@echo "测试入口（在 Test/ 目录下运行）："
	@echo "  make check              全部测试"
	@echo "  make check-sanitize     全部 ASan/UBSan 测试"
	@echo "  make check-<组名>       单组，如 check-socket / check-http-server"
	@echo "  make server             构建 .build/TestServer（TCP 回显示例）"
	@echo "  make echoserver         构建 .build/EchoServer"
	@echo "  make bench              构建 .build/BenchHttpServer"
	@echo "  make bench-logger       构建优化版 .build/BenchLogger"
	@echo "  make clean              清理测试产物"
	@echo "  make clean-all          连同核心产物一起清理"

# ---------------------------------------------------------------------------
# 编译：每个 TU 只编译一次，-MMD -MP 产出头文件依赖，由文件末尾 -include 回读。
# 取代了旧 Makefile 里 $(wildcard include/*/*.hpp) 那种「任何头文件变动即全量重建」
# 的粗放依赖。
# ---------------------------------------------------------------------------
$(OBJ_DIR)/%.o: $(ROOT)/%.cc
	@mkdir -p $(@D)
	$(CXX) $(TEST_CPPFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_SAN_DIR)/%.o: $(ROOT)/%.cc
	@mkdir -p $(@D)
	$(CXX) $(TEST_CPPFLAGS) $(SANITIZER_FLAGS) -MMD -MP -c -o $@ $<

# 源码集合（相对 $(ROOT)），沿用旧 Makefile 里刻意的最小链接划分。
# 例：Acceptor 只依赖 Reactor 组件，不链接 Connection.cc / Buffer.cc。
REACTOR_SRCS = src/Channel.cc src/EventLoop.cc src/Epoller.cc src/TimerQueue.cc \
	src/Socket.cc src/Logger.cc
CONNECTION_SRCS = src/Connection.cc src/Buffer.cc $(REACTOR_SRCS)
ACCEPTOR_SRCS = src/Acceptor.cc $(REACTOR_SRCS)
TCP_TIMER_SRCS = src/TcpServer.cc src/Acceptor.cc src/LoopThreadPool.cc src/LoopThread.cc \
	src/Connection.cc src/Buffer.cc $(REACTOR_SRCS)
HTTP_CORE_SRCS = src/HttpServer.cc src/HttpContext.cc src/HttpRequest.cc src/HttpResponse.cc \
	src/Util.cc $(TCP_TIMER_SRCS)
SOCKET_SRCS = src/Socket.cc src/Logger.cc
BUFFER_SRCS = src/Buffer.cc
LOGGER_SRCS = src/Logger.cc
UTIL_SRCS = src/Util.cc src/Buffer.cc src/Logger.cc
HTTP_REQUEST_SRCS = src/HttpRequest.cc $(UTIL_SRCS)
HTTP_RESPONSE_SRCS = src/HttpResponse.cc $(UTIL_SRCS)
HTTP_CONTEXT_SRCS = src/HttpContext.cc src/HttpRequest.cc $(UTIL_SRCS)

objs = $(addprefix $(OBJ_DIR)/,$(1:.cc=.o))
objs_san = $(addprefix $(OBJ_SAN_DIR)/,$(1:.cc=.o))

# 注意：这些必须经由变量传给 TEST_BIN。$(call) 的参数以逗号分隔，
# 直接把 -Wl,--wrap=xxx 写成字面量会被拆成两个参数。
SOCKET_WRAP = -Wl,--wrap=socket,--wrap=bind,--wrap=connect,--wrap=listen,--wrap=accept,--wrap=recv,--wrap=send,--wrap=setsockopt,--wrap=fcntl,--wrap=close
BUFFER_WRITE_WRAP = -Wl,--wrap=_ZN6Buffer5WriteEPKvm
EPOLL_WAIT_WRAP = -Wl,--wrap=epoll_wait
THREAD_WRAP = -Wl,--wrap=eventfd
SEND_LIMIT_WRAP = -Wl,--wrap=write,--wrap=_ZN6Buffer5WriteEPKvm
THREAD_SRCS = src/LoopThread.cc src/LoopThreadPool.cc $(REACTOR_SRCS)

# ---------------------------------------------------------------------------
# 链接：每个测试由 TEST_BIN 一次生成普通与 sanitize 两条规则。
# $(1) 测试名，对应 Test/$(1).cc  $(2) 源码集合  $(3) 额外链接参数（可选）
# ---------------------------------------------------------------------------
define TEST_BIN
$(BUILD_DIR)/$(1): $(OBJ_DIR)/Test/$(1).o $(call objs,$(2)) $(SPDLOG_LIB)
	$$(CXX) $$(TEST_CPPFLAGS) $$(LDFLAGS) -o $$@ $$(OBJ_DIR)/Test/$(1).o $(call objs,$(2)) $(3) $$(LDLIBS) $$(SPDLOG_LDLIBS)

$(BUILD_DIR)/$(1)-sanitize: $(OBJ_SAN_DIR)/Test/$(1).o $(call objs_san,$(2)) $(SPDLOG_LIB)
	$$(CXX) $$(TEST_CPPFLAGS) $$(SANITIZER_FLAGS) $$(LDFLAGS) -o $$@ $$(OBJ_SAN_DIR)/Test/$(1).o $(call objs_san,$(2)) $(3) $$(LDLIBS) $$(SPDLOG_LDLIBS)
endef

$(eval $(call TEST_BIN,TestBufferComprehensive,$(BUFFER_SRCS)))
$(eval $(call TEST_BIN,TestBufferAllocation,$(BUFFER_SRCS)))
$(eval $(call TEST_BIN,TestLogger,$(LOGGER_SRCS)))
$(eval $(call TEST_BIN,TestUtil,$(UTIL_SRCS)))
$(eval $(call TEST_BIN,TestResourcePath,$(UTIL_SRCS)))
$(eval $(call TEST_BIN,TestHttpRequest,$(HTTP_REQUEST_SRCS)))
$(eval $(call TEST_BIN,TestHttpResponse,$(HTTP_RESPONSE_SRCS)))
$(eval $(call TEST_BIN,TestHttpContext,$(HTTP_CONTEXT_SRCS)))
$(eval $(call TEST_BIN,TestSocket,$(SOCKET_SRCS)))
$(eval $(call TEST_BIN,TestSocketFaults,$(SOCKET_SRCS),$(SOCKET_WRAP)))
$(eval $(call TEST_BIN,TestEpoller,$(REACTOR_SRCS)))
$(eval $(call TEST_BIN,TestChannel,$(REACTOR_SRCS)))
$(eval $(call TEST_BIN,TestBasic,$(REACTOR_SRCS)))
$(eval $(call TEST_BIN,TestEventLoop,$(REACTOR_SRCS)))
$(eval $(call TEST_BIN,TestTimerQueue,$(REACTOR_SRCS)))
$(eval $(call TEST_BIN,TestEventLoopQuit,$(REACTOR_SRCS),$(EPOLL_WAIT_WRAP)))
$(eval $(call TEST_BIN,TestConnection,$(CONNECTION_SRCS),$(BUFFER_WRITE_WRAP)))
$(eval $(call TEST_BIN,TestAcceptor,$(ACCEPTOR_SRCS)))
$(eval $(call TEST_BIN,TestTcpServerTimers,$(TCP_TIMER_SRCS)))
$(eval $(call TEST_BIN,TestTcpServer,$(TCP_TIMER_SRCS)))
$(eval $(call TEST_BIN,TestLoopThread,$(THREAD_SRCS),$(THREAD_WRAP)))
$(eval $(call TEST_BIN,TestSendLimits,$(CONNECTION_SRCS),$(SEND_LIMIT_WRAP)))
$(eval $(call TEST_BIN,TestServerStop,$(HTTP_CORE_SRCS),$(THREAD_WRAP)))
$(eval $(call TEST_BIN,TestHttpServer,$(HTTP_CORE_SRCS)))
$(eval $(call TEST_BIN,TestServer,$(REACTOR_SRCS)))
$(eval $(call TEST_BIN,EchoServer,$(TCP_TIMER_SRCS)))

# 这两个自带 main、不链接任何项目源码，单独写规则。
$(BUILD_DIR)/BenchHttpServer: $(ROOT)/Test/BenchHttpServer.cc
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -I$(ROOT)/include $(CXXFLAGS) $(TEST_FLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD_DIR)/TestServerE2E: $(ROOT)/Test/TestServerE2E.cc
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -I$(ROOT)/include $(CXXFLAGS) $(TEST_FLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD_DIR)/TestServerE2E-sanitize: $(ROOT)/Test/TestServerE2E.cc
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -I$(ROOT)/include $(CXXFLAGS) $(TEST_FLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# e2e 是黑盒测试：被测的是真实的 HttpServer 二进制，因此要向核心 Makefile 索取。
.PHONY: core core-sanitize
core:
	$(MAKE) -C $(ROOT) all
core-sanitize:
	$(MAKE) -C $(ROOT) all-sanitize

# ---------------------------------------------------------------------------
# 运行入口：多数是一个 check-* 对应一个二进制，由 CHECK_BIN 生成。
# $(1) 检查名（check- 之后的部分）  $(2) 测试名
# ---------------------------------------------------------------------------
define CHECK_BIN
.PHONY: check-$(1) check-$(1)-sanitize
check-$(1): $$(BUILD_DIR)/$(2)
	$$(BUILD_DIR)/$(2)

check-$(1)-sanitize: $$(BUILD_DIR)/$(2)-sanitize
	$$(BUILD_DIR)/$(2)-sanitize
endef

$(eval $(call CHECK_BIN,http-request,TestHttpRequest))
$(eval $(call CHECK_BIN,util,TestUtil))
$(eval $(call CHECK_BIN,resource-path,TestResourcePath))
$(eval $(call CHECK_BIN,http-response,TestHttpResponse))
$(eval $(call CHECK_BIN,http-context,TestHttpContext))
$(eval $(call CHECK_BIN,epoller,TestEpoller))
$(eval $(call CHECK_BIN,channel,TestChannel))
$(eval $(call CHECK_BIN,integration,TestBasic))
$(eval $(call CHECK_BIN,eventloop,TestEventLoop))
$(eval $(call CHECK_BIN,timerqueue,TestTimerQueue))
$(eval $(call CHECK_BIN,eventloop-quit,TestEventLoopQuit))
$(eval $(call CHECK_BIN,connection,TestConnection))
$(eval $(call CHECK_BIN,acceptor,TestAcceptor))
$(eval $(call CHECK_BIN,tcpserver-timers,TestTcpServerTimers))
$(eval $(call CHECK_BIN,tcpserver,TestTcpServer))
$(eval $(call CHECK_BIN,loopthread,TestLoopThread))
$(eval $(call CHECK_BIN,send-limits,TestSendLimits))
$(eval $(call CHECK_BIN,server-stop,TestServerStop))
$(eval $(call CHECK_BIN,http-server,TestHttpServer))

# ---------------------------------------------------------------------------
# 以下入口不对应单一二进制，或有意保留历史形态，单独声明。
# ---------------------------------------------------------------------------

# 一个入口跑两个二进制。
.PHONY: check-buffer
check-buffer: $(BUILD_DIR)/TestBufferComprehensive $(BUILD_DIR)/TestBufferAllocation
	$(BUILD_DIR)/TestBufferComprehensive
	$(BUILD_DIR)/TestBufferAllocation

.PHONY: check-socket check-socket-sanitize
check-socket: $(BUILD_DIR)/TestSocket $(BUILD_DIR)/TestSocketFaults
	$(BUILD_DIR)/TestSocket
	$(BUILD_DIR)/TestSocketFaults

check-socket-sanitize: $(BUILD_DIR)/TestSocket-sanitize $(BUILD_DIR)/TestSocketFaults-sanitize
	$(BUILD_DIR)/TestSocket-sanitize
	$(BUILD_DIR)/TestSocketFaults-sanitize

# 日志的后台线程、模式切换和退出同样纳入 sanitizer。
$(eval $(call CHECK_BIN,logger,TestLogger))

.PHONY: server echoserver bench
server: $(BUILD_DIR)/TestServer
echoserver: $(BUILD_DIR)/EchoServer
bench: $(BUILD_DIR)/BenchHttpServer

# e2e 是黑盒测试：被测的是核心二进制，路径作为参数传给测试程序，
# 因此不能走 CHECK_BIN（它只生成不带参数的调用）。先向 ../Makefile 索取二进制。
#
# 必须在 $(ROOT) 下运行：TestServerE2E 用 fork+execv 启动被测服务器且不改变
# 子进程工作目录，而 main.cc 的 SetDocumentRoot("./www") 要求 ./www 存在。
# 从 Test/ 目录启动会因找不到 ./www 而启动失败。旧 Makefile 一直在 Http/ 下运行，
# 这里用 cd 保持同样的工作目录。
.PHONY: check-e2e check-e2e-sanitize
check-e2e: core $(BUILD_DIR)/TestServerE2E
	cd $(ROOT) && $(BUILD_DIR)/TestServerE2E $(BUILD_DIR)/HttpServer

check-e2e-sanitize: core-sanitize $(BUILD_DIR)/TestServerE2E-sanitize
	cd $(ROOT) && $(BUILD_DIR)/TestServerE2E-sanitize $(BUILD_DIR)/HttpServer-sanitize

# ---------------------------------------------------------------------------
# 汇总入口。两者的成员与旧 Makefile 逐项一致。
# ---------------------------------------------------------------------------
.PHONY: check
check: check-loopthread check-send-limits check-server-stop check-http-request check-logger check-buffer check-socket check-epoller \
	check-channel check-eventloop check-timerqueue check-integration \
	check-connection check-acceptor check-eventloop-quit \
	check-tcpserver-timers check-tcpserver check-util check-resource-path \
	check-http-response check-http-context check-http-server check-e2e

# TestBufferAllocation 走非 sanitize 版本：它的分配失败注入用例与 ASan 冲突，
# 这一直是旧 Makefile 的既有行为，此处原样保留。
.PHONY: check-sanitize
check-sanitize: check-logger-sanitize check-loopthread-sanitize check-send-limits-sanitize check-server-stop-sanitize check-http-request-sanitize check-socket-sanitize \
	check-epoller-sanitize check-channel-sanitize check-eventloop-sanitize \
	check-timerqueue-sanitize check-integration-sanitize \
	check-connection-sanitize check-acceptor-sanitize \
	check-eventloop-quit-sanitize check-tcpserver-timers-sanitize \
	check-tcpserver-sanitize check-util-sanitize check-resource-path-sanitize \
	check-http-response-sanitize check-http-context-sanitize \
	check-http-server-sanitize check-e2e-sanitize \
	$(BUILD_DIR)/TestBufferComprehensive-sanitize $(BUILD_DIR)/TestBufferAllocation
	$(BUILD_DIR)/TestBufferComprehensive-sanitize
	$(BUILD_DIR)/TestBufferAllocation

# ---------------------------------------------------------------------------
# 清理
# ---------------------------------------------------------------------------
.PHONY: clean clean-tests clean-all
clean-tests clean:
	rm -rf $(OBJ_DIR) $(OBJ_SAN_DIR) $(LOGGER_BENCH_DIR)
	rm -f $(BUILD_DIR)/Test* $(BUILD_DIR)/EchoServer $(BUILD_DIR)/BenchHttpServer $(BUILD_DIR)/BenchLogger

clean-all: clean
	$(MAKE) -C $(ROOT) clean

# 头文件依赖：-MMD 生成，随对象文件一同清理。
-include $(shell find $(OBJ_DIR) $(OBJ_SAN_DIR) -name '*.d' 2>/dev/null)

# 日志基准单独优化编译，避免复用普通测试对象。
.PHONY: bench-logger
bench-logger: $(BUILD_DIR)/BenchLogger

$(LOGGER_BENCH_DIR)/%.o: $(ROOT)/%.cc
	@mkdir -p $(@D)
	$(CXX) $(TEST_CPPFLAGS) -O2 -MMD -MP -c -o $@ $<

$(BUILD_DIR)/BenchLogger: $(LOGGER_BENCH_DIR)/Test/BenchLogger.o $(LOGGER_BENCH_DIR)/src/Logger.o $(SPDLOG_LIB)
	$(CXX) $(TEST_CPPFLAGS) -O2 $(LDFLAGS) -o $@ $(LOGGER_BENCH_DIR)/Test/BenchLogger.o $(LOGGER_BENCH_DIR)/src/Logger.o $(LDLIBS) $(SPDLOG_LDLIBS)

-include $(LOGGER_BENCH_DIR)/Test/BenchLogger.d $(LOGGER_BENCH_DIR)/src/Logger.d
