all:TestBuffer

TestBuffer:TestBuffer.cc Buffer.hpp Buffer.cc 
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++11 $(LDFLAGS) -o $@ $(filter %.cc,$^) $(LDLIBS)
.PHONY:clean
clean:
	rm -f TestBuffer

# 独立测试产物，不覆盖已跟踪的 TestBuffer 程序。
TEST_BUILD_DIR ?= .build
TEST_FLAGS = -std=c++11 -Wall -Wextra -Werror -pedantic -UNDEBUG
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-elide-constructors -g

$(TEST_BUILD_DIR):
	mkdir -p $@

$(TEST_BUILD_DIR)/%: %.cc Buffer.cc Buffer.hpp Makefile | $(TEST_BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_FLAGS) $(LDFLAGS) -o $@ $< Buffer.cc $(LDLIBS)

$(TEST_BUILD_DIR)/%-sanitize: %.cc Buffer.cc Buffer.hpp Makefile | $(TEST_BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_FLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) -o $@ $< Buffer.cc $(LDLIBS)

.PHONY: check check-sanitize clean-tests
check: $(TEST_BUILD_DIR)/TestBuffer $(TEST_BUILD_DIR)/TestBufferComprehensive $(TEST_BUILD_DIR)/TestBufferAllocation
	$(TEST_BUILD_DIR)/TestBuffer
	$(TEST_BUILD_DIR)/TestBufferComprehensive
	$(TEST_BUILD_DIR)/TestBufferAllocation

# 故障注入使用独立的普通可执行程序，不与 ASan 的分配器混用。
check-sanitize: $(TEST_BUILD_DIR)/TestBuffer-sanitize $(TEST_BUILD_DIR)/TestBufferComprehensive-sanitize $(TEST_BUILD_DIR)/TestBufferAllocation
	$(TEST_BUILD_DIR)/TestBuffer-sanitize
	$(TEST_BUILD_DIR)/TestBufferComprehensive-sanitize
	$(TEST_BUILD_DIR)/TestBufferAllocation

clean-tests:
	rm -f $(TEST_BUILD_DIR)/TestBuffer $(TEST_BUILD_DIR)/TestBufferComprehensive $(TEST_BUILD_DIR)/TestBufferAllocation $(TEST_BUILD_DIR)/TestBuffer-sanitize $(TEST_BUILD_DIR)/TestBufferComprehensive-sanitize
