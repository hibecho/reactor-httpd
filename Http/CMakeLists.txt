cmake_minimum_required(VERSION 3.18)

# 项目名称 HttpServer
project(HttpServer
    LANGUAGES CXX)

# 定义debug变量 和 release 变量
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
option(HTTP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
include(CTest)
find_package(Threads REQUIRED)

# Prefer the existing checkout; fetch exactly the previous pinned revision otherwise.
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/CMakeLists.txt")
    add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 7e635fca68d014934b4af8a1cf874f63989352b7)
    FetchContent_MakeAvailable(spdlog)
endif()

add_library(http_options INTERFACE)
target_compile_features(http_options INTERFACE cxx_std_17)
set(CMAKE_CXX_EXTENSIONS OFF)
target_include_directories(http_options INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(http_options INTERFACE -Wall -Wextra -Werror -pedantic -UNDEBUG)

add_library(http_sanitizers INTERFACE)

if(HTTP_ENABLE_SANITIZERS)
    target_compile_options(http_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-elide-constructors -g)
    target_link_options(http_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

add_library(http_core STATIC
    src/Acceptor.cc src/Buffer.cc src/Channel.cc src/Connection.cc
    src/Epoller.cc src/EventLoop.cc src/HttpContext.cc src/HttpRequest.cc
    src/BusinessThreadPool.cc src/HttpResponse.cc src/HttpResponder.cc
    src/HttpServer.cc src/HttpStreamWriter.cc
    src/Logger.cc src/LoopThread.cc
    src/LoopThreadPool.cc src/Socket.cc src/TcpServer.cc src/TimerQueue.cc src/Util.cc)
target_link_libraries(http_core PUBLIC http_options http_sanitizers spdlog::spdlog Threads::Threads)
add_executable(HttpServer main.cc)
target_link_libraries(HttpServer PRIVATE http_core)

# Also defines examples/benchmarks when BUILD_TESTING is OFF.
add_subdirectory(Test)
