# 官方编译库方式：固定 v1.12.0，提交 7e635fca68d014934b4af8a1cf874f63989352b7。
# 由 Http/Makefile 引入，提供 spdlog 的构建与链接参数。
SPDLOG_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
SPDLOG_DIR := $(dir $(SPDLOG_MAKEFILE))third_party/spdlog
SPDLOG_LIB := $(SPDLOG_DIR)/build/libspdlog.a
SPDLOG_CPPFLAGS := -I$(SPDLOG_DIR)/include -DSPDLOG_COMPILED_LIB
SPDLOG_LDLIBS := $(SPDLOG_LIB) -pthread

.PHONY: deps
deps: $(SPDLOG_LIB)

$(SPDLOG_DIR)/CMakeLists.txt:
	mkdir -p $(dir $(SPDLOG_DIR))
	git clone --depth 1 --branch v1.12.0 https://github.com/gabime/spdlog.git $(SPDLOG_DIR)

# 前置依赖只有上游的 CMakeLists.txt，不包含本文件自身。
# 曾经把 $(SPDLOG_MAKEFILE) 列进来，导致每次编辑 spdlog.mk（哪怕只改注释）都会
# 触发一次完整的 spdlog 重建，约 40 秒。若要改上面的 cmake 参数，用 make deps 强制重建：
#   rm -f $(SPDLOG_LIB) && make deps
$(SPDLOG_LIB): $(SPDLOG_DIR)/CMakeLists.txt
	cmake -S $(SPDLOG_DIR) -B $(SPDLOG_DIR)/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=11 -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_FMT_EXTERNAL=OFF -DSPDLOG_FMT_EXTERNAL_HO=OFF -DSPDLOG_BUILD_EXAMPLE=ON
	cmake --build $(SPDLOG_DIR)/build --parallel 2
