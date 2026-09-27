# 核心项目构建入口：main.cc + src/*.cc → .build/HttpServer
#
#   make                构建 .build/HttpServer（默认目标）
#   make all-sanitize   构建 .build/HttpServer-sanitize，供 Test/ 的 e2e 黑盒测试使用
#   make deps           获取并构建 spdlog
#   make clean          清理核心产物
#
# 测试不在本文件构建，一律在 Test/ 目录下进行：
#   cd Test && make check
#
# 本文件与 Test/Makefile 各自定义编译选项与对象目录，互不共享。因此 src/*.cc
# 在两侧各编译一次，换来的是两份构建互不干扰、改一处的选项不会污染另一侧产物。

.DEFAULT_GOAL := all
BUILD_DIR ?= .build
OBJ_DIR := $(BUILD_DIR)/obj
OBJ_SAN_DIR := $(BUILD_DIR)/obj-san

include spdlog.mk

CXX ?= g++
CORE_FLAGS = -std=c++17 -Wall -Wextra -Werror -pedantic -UNDEBUG
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-elide-constructors -g
CORE_CPPFLAGS = $(SPDLOG_CPPFLAGS) $(CPPFLAGS) -Iinclude $(CXXFLAGS) $(CORE_FLAGS)

# 默认开满核：不指定 -j 时按 CPU 核数并行。命令行给出的 -j 优先，因此
# make -j1 单线程、make -j8 八线程都照常生效，只有完全不指定时才用这里的默认值。
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

# 核心编译单元 = main.cc + 全部 src/*.cc，共 18 个 TU。
# 原先每个目标各自编译一整份源码，src/Buffer.cc 最多被编 12 次；现在每个 TU 只编一次。
CORE_SRCS := main.cc $(wildcard src/*.cc)

# -MMD -MP 产出 .d 记录该 TU 真正包含的头文件，由文件末尾 -include 回读。
# 这取代了旧 Makefile 里 $(wildcard include/*/*.hpp) 那种「任何头文件变动即全量重建」的粗放依赖。
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CORE_CPPFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_SAN_DIR)/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CORE_CPPFLAGS) $(SANITIZER_FLAGS) -MMD -MP -c -o $@ $<

CORE_OBJS := $(CORE_SRCS:%.cc=$(OBJ_DIR)/%.o)
CORE_OBJS_SAN := $(CORE_SRCS:%.cc=$(OBJ_SAN_DIR)/%.o)

.PHONY: all all-sanitize clean clean-objs

all: $(BUILD_DIR)/HttpServer

all-sanitize: $(BUILD_DIR)/HttpServer-sanitize

$(BUILD_DIR)/HttpServer: $(CORE_OBJS) $(SPDLOG_LIB)
	@mkdir -p $(@D)
	$(CXX) $(CORE_CPPFLAGS) $(LDFLAGS) -o $@ $(CORE_OBJS) $(LDLIBS) $(SPDLOG_LDLIBS)

$(BUILD_DIR)/HttpServer-sanitize: $(CORE_OBJS_SAN) $(SPDLOG_LIB)
	@mkdir -p $(@D)
	$(CXX) $(CORE_CPPFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) -o $@ $(CORE_OBJS_SAN) $(LDLIBS) $(SPDLOG_LDLIBS)

# 只清理核心产物。.build 下还有 Test/Makefile 产出的测试对象与二进制，
# 那些由 Test/ 的 clean 负责；要一次清干净用 cd Test && make clean-all。
clean-objs:
	rm -rf $(OBJ_DIR) $(OBJ_SAN_DIR)

clean: clean-objs
	rm -f $(BUILD_DIR)/HttpServer $(BUILD_DIR)/HttpServer-sanitize

-include $(shell find $(OBJ_DIR) $(OBJ_SAN_DIR) -name '*.d' 2>/dev/null)
