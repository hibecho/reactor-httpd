/**
 * @file Buffer.hpp
 * @author your name (you@domain.com)
 * @brief
 *
 * 提供的功能:存储数据 + 取出数据
 * 实现的思想:
 * 1.实现的缓冲区有一块内存空间，采用vector<char> --利用连续的线性内存空间
 * 2.实现要素:
 *  -默认的空间大小
 *  -当前的读取数据位置
 *  -当前的写入数据位置
 * 3.操作:
 *   a.写入数据:
 *     - 当前写入位置指向哪里，就从哪里开始写入
 *     - 考虑整体剩余空间是否足够
 *        + 足够:将数据移动到起始位置
 *        + 不够:进行扩容操作，从当前写位置开始向后扩容足够大小
 *    b.读取数据:
 *     - 当前读取位置指向哪里，就从哪里开始读取
 *     - 可读数据大小: 当前写入位置 - 当前读取位置
 * 4.接口设计
 *
 */

#include <iostream>
#include <stdint-gcc.h>
#include <vector>

class Buffer
{
public:

private:
    std::vector<char> _buffer;
    uint64_t _read_index;
    uint64_t _write_index;
};