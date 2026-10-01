#include "protocol/HttpStreamWriter.hpp"
#include "protocol/HttpResponse.hpp"
#include "tcp/Connection.hpp"

#include <charconv>
#include <utility>

HttpStreamWriter::HttpStreamWriter(std::weak_ptr<Connection> conn, bool chunked, bool keep_alive)
    : _conn(std::move(conn))
    , _chunked(chunked)
    , _keep_alive(keep_alive)
{
}

bool HttpStreamWriter::Begin(const HttpResponse &head)
{
    if (_started)
    {
        return _writable;
    }
    _started = true;

    auto conn = _conn.lock();
    if (!conn)
    {
        _writable = false;
        return false;
    }

    if (!conn->Send(head.SerializeHead(_chunked)))
    {
        _writable = false;
        return false;
    }
    return true;
}

bool HttpStreamWriter::Write(std::string_view data)
{
    if (!_writable || _finished)
    {
        return false;
    }
    if (data.empty())
    {
        // 零长度块是终止符，绝不能在这里发出去
        return true;
    }

    auto conn = _conn.lock();
    if (!conn)
    {
        _writable = false;
        return false;
    }

    std::string frame;
    if (_chunked)
    {
        // 十六进制长度，不带前导零、不带 0x
        char header[32];
        const auto converted = std::to_chars(header, header + sizeof(header), data.size(), 16);
        frame.reserve(static_cast<std::size_t>(converted.ptr - header) + data.size() + 4);
        frame.append(header, converted.ptr);
        frame += "\r\n";
        frame.append(data.data(), data.size());
        frame += "\r\n";
    }
    else
    {
        // HTTP/1.0：正文原样写出，由关闭连接界定结束
        frame.assign(data.data(), data.size());
    }

    if (!conn->Send(std::move(frame)))
    {
        _writable = false;
        return false;
    }
    return true;
}

bool HttpStreamWriter::Finish()
{
    if (_finished)
    {
        return _writable;
    }
    _finished = true;

    auto conn = _conn.lock();
    if (!conn)
    {
        _writable = false;
        return false;
    }

    bool ok = true;
    if (_chunked)
    {
        ok = conn->Send("0\r\n\r\n");
        if (!ok)
        {
            _writable = false;
        }
    }
    if (!_keep_alive)
    {
        conn->ShutDown();
    }
    return ok;
}

bool HttpStreamWriter::IsWritable() const noexcept
{
    return _writable && !_finished;
}

bool HttpStreamWriter::Started() const noexcept
{
    return _started;
}

bool HttpStreamWriter::Finished() const noexcept
{
    return _finished;
}
