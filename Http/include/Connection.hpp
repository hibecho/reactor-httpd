/**
 * @file Connection.hpp
 * @brief
 *
 * 设计目的: 对连接的全面管理，对通信连接的操作由该模块完成
 *
 * 管理的对象:
 *  1.套接字的管理
 *  2.连接事件的管理，可读、可写、错误、挂断、任意
 *  3.缓冲区的管理，将socket发送的数据 和 接收的数据 放入缓冲区。
 *  4.协议上下文管理，记录数据的处理过程
 *  5.回调函数的管理
 *      -因为连接收到数据之后，该如何处理，由用户决定，因此必须有业务处理回调函数
 *      -一个连接建立成功后，该如何处理，由用户决定，因此必须有建立连接成功的回调函数
 *      -一个连接关闭前，该如何处理，由用户决定，因此必须有关闭连接的回调函数
 *      -任意事件的产生，有没有某些处理，由用户决定，因此必须有任意事件的回调函数
 *
 * 功能的设计:
 *  1.发送数据
 *      -给用户提供的发送数据接口，将其保存在发送缓冲区中，启动写事件监控
 *      -写事件就绪，调用写事件回调，将数据发送
 *
 *  2.关闭连接
 *      -给用户提供的关闭连接接口，应该在实际释放连接之前，观察是否有数据待处理
 *
 *  3.启动非活跃连接的销毁功能
 *
 *  4.取消非活跃连接的销毁功能
 *
 *  5.协议切换
 *      -对于建立的连接接收数据如何进行业务处理，取决于上下文，以及业务处理的回调函数
 */

#pragma once
#include <any>
#include <functional>
#include <memory>
#include <stdint.h>
#include <string>

class Socket;
class Channel;
class Buffer;
class Connection;
class EventLoop;

enum class ConnStatus
{
    DISCONNECTED, // 连接关闭状态
    CONNECTING,   // 正在连接状态
    CONNECTED,    // 已连接状态
    DISCONNETING  // 正在关闭连接状态
};

using ConnectionPtr = std::shared_ptr<Connection>;

class Connection : public std::enable_shared_from_this<Connection>
{
  public:
    // 连接类事件：建立成功
    using ConnectedCallback = std::function<void(const ConnectionPtr &)>;
    // 连接类事件：建立关闭
    using ClosedCallback = std::function<void(const ConnectionPtr &)>;
    // 数据到达：业务层解析输入缓冲区
    using MessageCallback = std::function<void(const ConnectionPtr &, Buffer *)>;
    // 任意事件：刷新活跃度
    using AnyEventCallback = std::function<void(const ConnectionPtr &)>;

  public:
    // 接管 fd；loop 必须比本对象活得更久。已启动连接必须由服务器持有到关闭通知。
    Connection(EventLoop *loop, uint64_t id, int fd);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    // 获取管理的文件描述符
    int GetFd();
    // 获取连接ID
    uint64_t GetConnetId();
    // 获取Timer ID
    uint64_t GetTimerId();
    // 判断是否连接
    bool IsConnected();
    // 设置上下文
    void SetContext(std::any context);
    // 非const版本获取上下文
    std::any &GetContext() noexcept;
    // const版本获取上下文
    const std::any &GetContext() const noexcept;

    // 设置连接回调
    void SetConnectedCallback(ConnectedCallback cb);
    // 设置消息处理回调
    void SetMessageCallback(MessageCallback cb);
    // 设置关闭回调
    void SetClosedCallback(ClosedCallback cb);
    // 建立连接前设置；用于从服务器连接表中移除连接。
    void SetServerClosedCallback(ClosedCallback cb);
    // 设置任意事件回调
    void SetAnyEventCallback(AnyEventCallback cb);
    // 连接就绪后，进行Channel回调设置
    void Established();
    // 发送数据，将数据发送到缓冲区，开启事件监控
    void Send(std::string data);
    // 对外提供关闭接口，把操作交给所属 EventLoop
    void ShutDown();
    // 对外接口，可能在任意线程被调用;
    // 启动非活跃销毁；
    // timeout:指定多长时间被认定为非活跃连接，须大于 0，否则抛 std::invalid_argument。
    // 参数用有符号类型：对 uint32_t 做 timeout <= 0 判断等价于只判 0，
    // 负数会在实参转换处回绕成极大值并被时间轮静默钳位到容量上限。
    void EnableInactiveRelease(int timeout);
    // 可跨线程请求；取消已登记任务，重复调用无副作用。
    void CancelInactiveRelease();
    // 协议切换
    // 可跨线程请求；参数由任务持有，在所属线程一起替换。
    // 保留缓冲区与服务器关闭回调，不重复触发建立通知。
    // 关闭过程中忽略切换；切换后旧上下文的指针/引用不可继续使用。
    void SwitchProtocol(std::any context, ConnectedCallback conn, MessageCallback msg, ClosedCallback closed,
                        AnyEventCallback event);

  private:
    /* Channel的事件回调函数*/
    void HandleRead();
    void HandleWrite();
    void HandleClose();
    void HandleError();
    void HandleEvent();

    void SendInLoop(std::string data);
    void ReleaseInLoop();
    void EstablishedInLoop();
    void ShutDownInLoop();
    void EnableInactiveReleaseInLoop(uint32_t timeout);
    void CancelInactiveReleaseInLoop();
    void SwitchProtocolInLoop(std::any context, ConnectedCallback conn, MessageCallback msg, ClosedCallback closed,
                              AnyEventCallback event);

  private:
    /*Connection模块的基础属性*/
    uint64_t _conn_id;             // 连接的唯一ID值，便于连接的管理和查找
    uint64_t _timer_id;            // 复用_conn_id作为唯一的定时器ID
    int _sockfd;                   // 连接关联的文件描述符
    ConnStatus _status;            // 连接状态
    EventLoop *_loop;              //  指向所属事件循环，不拥有它
    bool _release_pending = false; // 已安排延迟释放
    bool _released = false;        // 已完成资源清理
    bool _registered = false;      // Channel 已登记到 Epoller
    bool enable_inactive_release;  // 设定是否需要非活跃销毁连接

    /*Connection模块的管理对象*/
    std::unique_ptr<Socket> _socket;     // 套接字管理
    std::unique_ptr<Channel> _channel;   // 连接的事件管理
    std::unique_ptr<Buffer> _in_buffer;  // 输入缓冲区，从Sokcet读取到的数据
    std::unique_ptr<Buffer> _out_buffer; // 输出缓冲区，向Socket发送数据
    std::any _context;                   // 协议上下文

    /*Connection模块事件的回调，由组件使用者设置*/
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;

    /*组件内的连接关闭回调*/
    ClosedCallback _server_closed_callback;
};