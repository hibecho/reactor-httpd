#include "base/Buffer.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

Buffer::Buffer(std::size_t initial_size)
    : _buffer(initial_size)
{
}

Buffer::Buffer(Buffer &&other) noexcept
    : _buffer(std::move(other._buffer))
    , _read_index(other._read_index)
    , _write_index(other._write_index)
{
    other._buffer.clear();
    other.Clear();
}

Buffer &Buffer::operator=(Buffer &&other) noexcept
{
    if (this != &other)
    {
        _buffer = std::move(other._buffer);
        _read_index = other._read_index;
        _write_index = other._write_index;
        other._buffer.clear();
        other.Clear();
    }
    return *this;
}

void Buffer::MoveReadOffset(std::size_t length)
{
    if (length > GetReadableSize())
    {
        throw std::out_of_range("Buffer::MoveReadOffset");
    }
    _read_index += length;

    if (_read_index == _write_index)
    {
        _read_index = 0;
        _write_index = 0;
    }
}

void Buffer::MoveWriteOffset(std::size_t length)
{
    if (length > GetWritableSize())
    {
        throw std::out_of_range("Buffer::MoveWriteOffset");
    }
    _write_index += length;
}

void Buffer::EnsureWritableSize(std::size_t length)
{
    if (length <= GetWritableSize())
    {
        return;
    }

    if (length <= (GetPrependableSize() + GetWritableSize()))
    {
        // 1. 先搬动当前可读数据
        size_t readsize = GetReadableSize();
        auto start = GetReadPosition();
        auto end = start + readsize;
        auto dest = Begin();
        std::copy(start, end, dest);
        // 2. 更新读写指针
        _read_index = 0;         // 将读位置设为0
        _write_index = readsize; // 将写位置设置为当前可写位置
        return;
    }
    // 总空闲不足：在当前写位置之后扩展空间。
    // 先检查，避免下面的加法溢出或超过 vector 的长度限制。
    if (length > _buffer.max_size() - _write_index)
    {
        throw std::length_error("Buffer::EnsureWritableSize");
    }

    // 扩容
    _buffer.resize(_write_index + length);
}

void Buffer::Write(const void *data, std::size_t length)
{
    // 1.处理长度为0的情况
    if (length == 0)
        return;
    // 2.数据为空的情况
    if (data == nullptr)
    {
        throw std::invalid_argument("Buffer::Write: null data");
    }
    // 2.确保空间足够
    EnsureWritableSize(length);
    // 3.将数据进行拷贝
    std::memcpy(GetWritePosition(), data, length);
    // 4.移动指针
    MoveWriteOffset(length);
}

void Buffer::WriteBuffer(const Buffer &other)
{
    const std::size_t length = other.GetReadableSize();
    if (length == 0)
    {
        return;
    }

    // 先扩容或搬移，保证了新获取的迭代器不会失效
    EnsureWritableSize(length);
    // 自身追加时，扩容/搬移后重新获取源指针；未读区与后沿写入区不重叠。
    std::memcpy(GetWritePosition(), other.GetReadPosition(), length);
    MoveWriteOffset(length);
}

void Buffer::Peek(void *destination, std::size_t length) const
{
    if (length == 0)
    {
        return;
    }

    if (length > GetReadableSize())
    {
        throw std::out_of_range("Buffer::Peek: length out of bounds");
    }

    if (destination == nullptr)
    {
        throw std::invalid_argument("Buffer::Peek: null destination");
    }

    std::memcpy(destination, GetReadPosition(), length);
}

std::string Buffer::PeekAsString(std::size_t length) const
{
    if (length > GetReadableSize())
    {
        throw std::out_of_range("Buffer::PeekAsString: length out of bounds");
    }

    if (length == 0)
    {
        return {};
    }
    std::string result(GetReadPosition(), length); // 复制为字符串
    return result;
}

const char *Buffer::FindCRLF() const noexcept
{
    const std::size_t readable = GetReadableSize();
    if (readable < 2)
    {
        return nullptr;
    }

    const char *begin = GetReadPosition();
    const char *end = begin + readable;
    const char *p = begin;
    while (p < end)
    {
        p = static_cast<const char *>(std::memchr(p, '\r', static_cast<std::size_t>(end - p)));
        if (p == nullptr)
        {
            return nullptr;
        }
        // p + 1 < end 保证 p[1] 合法：末尾孤立的 '\r' 视为未找到。
        if (p + 1 < end && p[1] == '\n')
        {
            return p;
        }
        ++p;
    }
    return nullptr;
}

const char *Buffer::FindLF() const noexcept
{
    const std::size_t readable = GetReadableSize();
    if (readable == 0)
    {
        return nullptr;
    }
    return static_cast<const char *>(std::memchr(GetReadPosition(), '\n', readable));
}

char *Buffer::Begin()
{
    return _buffer.data();
}

const char *Buffer::Begin() const noexcept
{
    return _buffer.data();
}

char *Buffer::GetWritePosition() noexcept
{
    return Begin() + _write_index;
}

const char *Buffer::GetReadPosition() const noexcept
{
    return Begin() + _read_index;
}

std::size_t Buffer::GetPrependableSize() const noexcept
{
    return _read_index;
}

std::size_t Buffer::GetWritableSize() const noexcept
{
    return _buffer.size() - _write_index;
}

std::size_t Buffer::GetReadableSize() const noexcept
{
    return _write_index - _read_index;
}

void Buffer::WriteString(const std::string &str)
{
    Write(str.c_str(), str.size());
}

void Buffer::Read(void *destination, std::size_t length)
{
    Peek(destination, length);
    MoveReadOffset(length);
}

std::string Buffer::ReadAsString(std::size_t length)
{
    std::string result = PeekAsString(length);
    MoveReadOffset(length);
    return result;
}

std::string Buffer::PeekLine(LineMode mode) const
{
    const bool use_crlf = (mode == LineMode::CRLF);
    const char *pos = use_crlf ? FindCRLF() : FindLF();
    if (pos == nullptr)
    {
        return {};
    }
    const std::size_t delimiter_size = use_crlf ? 2 : 1;
    const std::size_t length = static_cast<std::size_t>(pos - GetReadPosition()) + delimiter_size;
    return PeekAsString(length);
}

std::string Buffer::ReadLine(LineMode mode)
{
    std::string line = PeekLine(mode);
    // PeekLine 的返回长度即待消费字节数：未找到时为空串，消费 0 字节。
    MoveReadOffset(line.size());
    return line;
}

void Buffer::Clear() noexcept
{
    _read_index = 0;
    _write_index = 0;
}
