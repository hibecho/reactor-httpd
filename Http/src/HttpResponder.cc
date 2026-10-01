#include "protocol/HttpResponder.hpp"

#include "protocol/HttpStreamWriter.hpp"
#include "tcp/Connection.hpp"

#include <utility>

HttpResponder::HttpResponder(std::weak_ptr<Connection> conn, std::string version, bool head_request, bool keep_alive)
    : _conn(std::move(conn))
    , _version(std::move(version))
    , _head_request(head_request)
    , _keep_alive(keep_alive)
{
}

bool HttpResponder::Send(HttpResponse response)
{
    if (_done)
    {
        return false;
    }
    _done = true;

    auto conn = _conn.lock();
    if (!conn)
    {
        return false;
    }

    response.SetClose(!_keep_alive);
    const bool sent = conn->Send(response.Serialize(_head_request));
    if (_keep_alive && _on_finish)
    {
        _on_finish();
    }
    return sent;
}

std::shared_ptr<HttpStreamWriter> HttpResponder::BeginStream(HttpResponse head)
{
    if (_done)
    {
        return nullptr;
    }
    _done = true;

    // HTTP/1.0 不认识分块编码，只能原样写出正文、靠关闭连接界定结束
    const bool chunked = (_version != "HTTP/1.0");
    if (!chunked)
    {
        _keep_alive = false;
    }

    head.SetClose(!_keep_alive);

    auto writer = std::make_shared<HttpStreamWriter>(_conn, chunked, _keep_alive);
    writer->SetFinishHandler(_on_finish);
    if (!writer->Begin(head))
    {
        return nullptr;
    }
    return writer;
}

bool HttpResponder::IsWritable() const noexcept
{
    auto conn = _conn.lock();
    return conn != nullptr;
}
