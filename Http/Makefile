# CMake owns compilation and dependencies; this file only provides shortcuts.
.DEFAULT_GOAL := all
ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD_DIR ?= $(ROOT)/.build
MODE ?= debug
JOBS ?= 2
CONFIG_DIR = $(abspath $(BUILD_DIR))/$(MODE)
BUILD_TYPE = $(if $(filter release,$(MODE)),Release,Debug)
SANITIZE = $(if $(filter sanitize,$(MODE)),ON,OFF)
CMAKE ?= cmake
CTEST ?= ctest
CMAKE_ARGS ?=

.PHONY: all configure release all-sanitize check check-sanitize deps clean clean-objs clean-tests clean-all help server echoserver bench bench-logger core core-sanitize
configure:
	$(CMAKE) -S "$(ROOT)" -B "$(CONFIG_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DHTTP_ENABLE_SANITIZERS=$(SANITIZE) -DBUILD_TESTING=ON $(CMAKE_ARGS)

all: configure
	$(CMAKE) --build "$(CONFIG_DIR)" --target HttpServer --parallel $(JOBS)
	ln -sfn "$(CONFIG_DIR)/bin/HttpServer" "$(BUILD_DIR)/HttpServer$(if $(filter sanitize,$(MODE)),-sanitize)"

release:
	$(MAKE) MODE=release all
all-sanitize:
	$(MAKE) MODE=sanitize all
core: all
core-sanitize: all-sanitize

deps: configure
	$(CMAKE) --build "$(CONFIG_DIR)" --target spdlog --parallel $(JOBS)

check: configure
	$(CMAKE) --build "$(CONFIG_DIR)" --target http_tests --parallel $(JOBS)
	$(CTEST) --test-dir "$(CONFIG_DIR)" --output-on-failure
check-sanitize:
	$(MAKE) MODE=sanitize check

# Build tests before CTest; CTest alone does not compile targets.
check-%-sanitize:
	$(MAKE) MODE=sanitize check-$*
check-%: configure
	$(CMAKE) --build "$(CONFIG_DIR)" --target http_tests --parallel $(JOBS)
	$(CTEST) --test-dir "$(CONFIG_DIR)" --output-on-failure -L '^$*$$' --no-tests=error

server: APP_TARGET = TestServer
echoserver: APP_TARGET = EchoServer
bench: APP_TARGET = BenchHttpServer
bench-logger: APP_TARGET = BenchLogger
server echoserver bench bench-logger: configure
	$(CMAKE) --build "$(CONFIG_DIR)" --target $(APP_TARGET) --parallel $(JOBS)
	ln -sfn "$(CONFIG_DIR)/bin/$(APP_TARGET)" "$(BUILD_DIR)/$(APP_TARGET)"

clean clean-objs clean-tests:
	@if test -f "$(CONFIG_DIR)/CMakeCache.txt"; then $(CMAKE) --build "$(CONFIG_DIR)" --target clean; fi
clean-all:
	@for mode in debug release sanitize; do if test -f "$(BUILD_DIR)/$$mode/CMakeCache.txt"; then $(CMAKE) --build "$(BUILD_DIR)/$$mode" --target clean || exit $$?; fi; done

help:
	@echo 'make [all|release|all-sanitize|check|check-sanitize|check-<group>|deps]'
	@echo 'make [server|echoserver|bench|bench-logger|clean|clean-all]'
	@echo 'Options: JOBS=2 MODE=debug|release|sanitize BUILD_DIR=... CMAKE_ARGS=...'
