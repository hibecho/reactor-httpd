/**
 * @file Buffer.hpp
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
 *  a.写入数据:
 *     - 当前写入位置指向哪里，就从哪里开始写入
 *     - 考虑整体剩余空间是否足够
 *        + 足够:将数据移动到起始位置
 *        + 不够:进行扩容操作，从当前写位置开始向后扩容足够大小
 *  b.读取数据:
 *     - 当前读取位置指向哪里，就从哪里开始读取
 *     - 可读数据大小: 当前写入位置 - 当前读取位置
 * 4.接口设计
 *
 *  - 获取当前的写位置
 *  - 获取当前读位置
 *  - 获取前沿空闲空间大小
 *  - 获取后沿空间的大小
 *  - 将读位置向后移动指定长度
 *  - 将写位置向后移动指定的长度
 *  - 确保当前可写空间足够 (扩容 + 移动)
 *  - 获取当前可读空间的大小
 *  - 写入数据
 *  - 读取数据
 *  - 查找换行字符
 *  - 清理缓冲区功能
 */

#pragma once
#include <string>
#include <cstddef>
#include <vector>

#define BUFFER_DEFAULT_SIZE 1024
class Buffer
{
public:
    // 初始可写空间，单位为字节；允许为 0，初始可读数据为空。
    explicit Buffer(std::size_t initial_size = BUFFER_DEFAULT_SIZE);

    // 安全获取起始位置的指针 -非const对象
    // 扩容会导致迭代器失效，需要再次安全申请
    char *Begin() { return _buffer.data(); }

    // 安全获取起始位置的指针 -const对象
    const char *Begin() const noexcept { return _buffer.data(); }

    // 连续可写区域起点，不推进写偏移；最多写 GetWritableSize() 字节。
    // 长度为 0 时不可解引用。
    char *GetWritePosition() noexcept { return Begin() + _write_index; }

    // 连续可读区域起点，不消费数据，不保证以空字符结尾。
    // 最多读 GetReadableSize() 字节；长度为 0 时不可解引用。
    const char *GetReadPosition() const noexcept { return Begin() + _read_index; }

    // 前沿可回收空间，即已消费的数据所占空间。
    std::size_t GetPrependableSize() const noexcept { return _read_index; }

    // 后沿连续可写空间，不包含前沿可回收空间。
    std::size_t GetWritableSize() const noexcept { return _buffer.size() - _write_index; }

    // 当前可读数据的字节数。
    std::size_t GetReadableSize() const noexcept { return _write_index - _read_index; }

    // 消费 length 字节；全部消费后将读写偏移归零。
    // length > GetReadableSize() 时抛出 std::out_of_range，状态不变。
    void MoveReadOffset(std::size_t length);

    // 提交已写入的 length 字节，不负责复制数据或扩容。
    // length > GetWritableSize() 时抛出 std::out_of_range，状态不变。
    void MoveWriteOffset(std::size_t length);

    // 保证后沿至少有 length 字节可写，保留未读数据及其顺序。
    // 后沿不足时优先回收前沿空间，否则扩容；可能抛出分配或长度异常。
    // 调用后应重新获取读写指针。
    void EnsureWritableSize(std::size_t length);

    // 将 data 指向的 length 字节追加到缓冲区，自动保证可写空间并推进写偏移。
    // length 为 0 时不做任何操作，允许 data 为 nullptr。
    // length 非 0 时，data 必须指向至少 length 字节的有效可读内存，
    // 且不得与本缓冲区的底层存储重叠（扩容或搬移可能使源指针失效）。
    // 可能抛出分配或长度异常；调用后应重新获取读写指针。
    void Write(const void *data, std::size_t length);

    void WriteString(const std::string &str) { Write(str.c_str(), str.size()); }

    void WriteBuffer(const Buffer &other) { Write(other.GetReadPosition(), other.GetReadableSize()); }

    // 查看 length 字节，不消费数据，不进行推进读偏移。
    // destination 必须有足够的可写空间，且不得与缓冲区存储重叠。
    void Peek(void *destination, std::size_t length) const;

    // 将 length 字节可读数据复制到 destination，然后推进读偏移。
    // 全部消费后将读写偏移归零，不自动添加字符串结束符。
    // length > GetReadableSize() 时抛出 std::out_of_range，
    // 缓冲区状态及目标内存均不变。
    // length 为 0 时不做任何操作，允许 destination 为 nullptr。
    // length 非 0 时，destination 必须指向至少 length 字节的有效可写内存，
    // 且不得与本缓冲区的底层存储重叠。
    void Read(void *destination, std::size_t length)
    {
        Peek(destination, length);
        MoveReadOffset(length);
    }

    // 读取并消费 length 字节，保留内嵌空字符；length 为 0 时返回空字符串。
    // 超过可读长度时抛出 std::out_of_range；字符串构造失败时抛出异常且不消费数据。
    std::string ReadAsString(std::size_t length)
    {
        std::string result = PeekAsString(length);
        MoveReadOffset(length);
        return result;
    }

    // 读取 length 字节，保留内嵌空字符；length 为 0 时返回空字符串。
    // 超过可读长度时抛出 std::out_of_range；字符串构造失败时抛出异常。
    std::string PeekAsString(std::size_t length) const;

    // 在可读数据中查找第一个 CRLF（"\r\n"）。
    // 找到时返回指向 '\r' 的指针，未找到返回 nullptr；不消费数据，不改变偏移。
    // 可读长度小于 2 或没有完整 CRLF 时返回 nullptr，而不是错误。
    // 末尾孤立的 '\r' 需等待后续数据，但不影响查找此前已有的完整 CRLF。
    // 返回指针指向缓冲区内部存储，任何可能扩容或搬移的操作
    // （Write/EnsureWritableSize）之后即失效，应尽快使用。
    const char *FindCRLF() const noexcept;

    // 返回包含结尾 CRLF 在内的一整行，并不消费该行；
    // 缓冲区中没有完整 CRLF 时，返回空字符串且不消费任何数据。
    // 注意：
    // 真正的空行返回 "\r\n"，因此空字符串唯一表示"尚未收到完整行"。
    // 字符串构造失败时抛出异常且不消费数据。
    std::string PeekLine() const
    {
        const char *pos = FindCRLF();
        if (pos == nullptr)
        {
            return {};
        }
        const std::size_t length = static_cast<std::size_t>(pos - GetReadPosition()) + 2;
        return PeekAsString(length);
    }

    // 返回包含结尾 CRLF 在内的一整行，并消费该行。
    // 缓冲区中没有完整 CRLF 时返回空字符串且不消费任何数据。
    // 空字符串唯一表示“尚未收到完整行”，真正的空行返回 "\r\n"。
    // 字符串构造失败时抛出异常且不消费数据。
    std::string ReadLine()
    {
        std::string line = PeekLine();
        // PeekLine 的返回长度即待消费字节数：未找到时为空串，消费 0 字节。
        MoveReadOffset(line.size());
        return line;
    }

    // 丢弃全部数据并将偏移归零；保留存储空间，不擦除内存内容。
    void Clear() noexcept
    {
        _read_index = 0;
        _write_index = 0;
    }

private:
    // 始终满足：0 <= _read_index <= _write_index <= _buffer.size()。
    std::vector<char> _buffer;
    std::size_t _read_index = 0;
    std::size_t _write_index = 0;
};
