if(BUILD_TESTING)
# Keep test executables separate from the default server build. The http_tests
# target builds the complete suite before CTest is invoked.
function(http_add_test name label)
    add_executable(${name} EXCLUDE_FROM_ALL ${name}.cc)
    target_link_libraries(${name} PRIVATE http_core http_sanitizers)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${label}" TIMEOUT 180)
    set_property(GLOBAL APPEND PROPERTY HTTP_TEST_TARGETS ${name})
endfunction()

http_add_test(TestBufferComprehensive buffer)

# Allocation-failure injection overrides allocation functions and conflicts with
# ASan. Compile Buffer separately so sanitizer instrumentation cannot leak in
# through http_core, even in a sanitizer build.
add_executable(TestBufferAllocation EXCLUDE_FROM_ALL
    TestBufferAllocation.cc ../src/Buffer.cc)
target_link_libraries(TestBufferAllocation PRIVATE http_options)
add_test(NAME TestBufferAllocation COMMAND TestBufferAllocation)
set_tests_properties(TestBufferAllocation PROPERTIES LABELS buffer TIMEOUT 180)
set_property(GLOBAL APPEND PROPERTY HTTP_TEST_TARGETS TestBufferAllocation)

http_add_test(TestLogger logger)
http_add_test(TestUtil util)
http_add_test(TestResourcePath resource-path)
http_add_test(TestHttpRequest http-request)
http_add_test(TestHttpResponse http-response)
http_add_test(TestHttpContext http-context)
http_add_test(TestSocket socket)
http_add_test(TestSocketFaults socket)
http_add_test(TestEpoller epoller)
http_add_test(TestChannel channel)
http_add_test(TestBasic integration)
http_add_test(TestEventLoop eventloop)
http_add_test(TestTimerQueue timerqueue)
http_add_test(TestEventLoopQuit eventloop-quit)
http_add_test(TestConnection connection)
http_add_test(TestAcceptor acceptor)
http_add_test(TestTcpServerTimers tcpserver-timers)
http_add_test(TestTcpServer tcpserver)
http_add_test(TestLoopThread loopthread)
http_add_test(TestSendLimits send-limits)
http_add_test(TestServerStop server-stop)
http_add_test(TestHttpServer http-server)
http_add_test(TestHttpStreaming http-streaming)

foreach(symbol socket bind connect listen accept recv send setsockopt fcntl close)
    target_link_options(TestSocketFaults PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
target_link_options(TestEventLoopQuit PRIVATE "LINKER:--wrap=epoll_wait")
target_link_options(TestConnection PRIVATE "LINKER:--wrap=_ZN6Buffer5WriteEPKvm")
target_link_options(TestLoopThread PRIVATE "LINKER:--wrap=eventfd")
target_link_options(TestSendLimits PRIVATE
    "LINKER:--wrap=write" "LINKER:--wrap=_ZN6Buffer5WriteEPKvm")
target_link_options(TestServerStop PRIVATE "LINKER:--wrap=eventfd")
# Constructor-failure paths: a descriptor opened before a later member throws must still be released.
target_link_options(TestEventLoop PRIVATE "LINKER:--wrap=timerfd_create")
target_link_options(TestTimerQueue PRIVATE "LINKER:--wrap=epoll_ctl")

add_executable(TestServerE2E EXCLUDE_FROM_ALL TestServerE2E.cc)
target_link_libraries(TestServerE2E PRIVATE http_options http_sanitizers Threads::Threads)
add_dependencies(TestServerE2E HttpServer)
add_test(NAME TestServerE2E COMMAND TestServerE2E $<TARGET_FILE:HttpServer>)
set_tests_properties(TestServerE2E PROPERTIES
    LABELS e2e TIMEOUT 180 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_property(GLOBAL APPEND PROPERTY HTTP_TEST_TARGETS TestServerE2E)

get_property(http_test_targets GLOBAL PROPERTY HTTP_TEST_TARGETS)
add_custom_target(http_tests DEPENDS ${http_test_targets} HttpServer)

endif()

foreach(example TestServer EchoServer)
    add_executable(${example} EXCLUDE_FROM_ALL ${example}.cc)
    target_link_libraries(${example} PRIVATE http_core http_sanitizers)
endforeach()

add_executable(BenchHttpServer EXCLUDE_FROM_ALL BenchHttpServer.cc)
target_link_libraries(BenchHttpServer PRIVATE http_options Threads::Threads)

# Benchmark optimized Logger code regardless of the main build type; do not
# reuse the potentially unoptimized or instrumented http_core implementation.
add_executable(BenchLogger EXCLUDE_FROM_ALL BenchLogger.cc ../src/Logger.cc)
target_link_libraries(BenchLogger PRIVATE http_options spdlog::spdlog Threads::Threads)
target_compile_options(BenchLogger PRIVATE -O2)
