all:TestBuffer

TestBuffer:TestBuffer.cc Buffer.hpp Buffer.cc 
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++11 $(LDFLAGS) -o $@ $(filter %.cc,$^) $(LDLIBS)
.PHONY:clean
clean:
	rm -f TestBuffer
