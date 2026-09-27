#include "base/Buffer.hpp"
#include "base/Logger.hpp"
#include "tcp/TcpServer.hpp"
#include <memory>

class EchoServer
{
  public:
    EchoServer(int port)
        : _server(std::make_unique<TcpServer>(port))
    {
        _server->SetThreadCount(2);
        _server->EnableInactiveRelease(10);

        _server->SetClosedCallback([](const ConnectionPtr &conn) {
            LOG_DEBUG("Connection Closed:{}", conn->GetConnetId());
        });

        _server->SetConnectedCallback([](const ConnectionPtr &conn) {
            LOG_DEBUG("Connection Created:{}", conn->GetConnetId());
        });

        _server->SetMessageCallback([](const ConnectionPtr &conn, Buffer *buf) {
            const auto size = buf->GetReadableSize();
            if (size == 0)
            {
                return;
            }
            conn->Send(std::string(buf->GetReadPosition(), size));
            buf->MoveReadOffset(size);
        });
    }

    void Start()
    {
        _server->Start();
    }

  private:
    std::unique_ptr<TcpServer> _server;
};

int main()
{
    EchoServer server(8080);
    server.Start();
    return 0;
}