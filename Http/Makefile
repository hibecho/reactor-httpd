all:TestBuffer

TestBuffer:TestBuffer.cc Buffer.hpp Buffer.cc 
	g++ -o $@ $^ -std=c++11
.PHONY:clean
clean:
	rm -f TestBuffer