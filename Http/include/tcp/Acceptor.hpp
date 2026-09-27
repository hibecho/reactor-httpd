/**
 * @file Acceptor.hpp
 * @brief
 *
 * 功能设计:
 * 1.创建一个监听套接字
 * 2.启动读事件监控
 * 3.事件触发后，获取新连接
 * 4.把 accept 到的 fd 通过回调交给服务器模块，由其创建并管理 Connection
 *   - 服务器模块给 Connection 设置回调函数，对新连接进行管理
 *
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

class Socket;
class EventLoop;
class Channel;

class Acceptor
{
    // accept 成功后的 fd 所有权随本次调用交给回调方。
    // 该 fd 不继承监听套接字的 O_NONBLOCK（Linux accept 语义），需要非阻塞时
    // 由回调方自行设置——Connection 的构造函数已完成这一步。Acceptor 不改动新 fd 的任何状态标志。
    // 回调开始执行即接管该 fd，应立即用 RAII 管理；若回调抛异常，Acceptor 不代为关闭它，
    // 因为此时 fd 可能已被回调方接管并释放，重复 close 会误伤被复用的描述符号。
    using AcceptCallback = std::function<void(int fd)>;

  public:
    /*Acceptor构造函数*/

    // loop 必须比本对象活得更久。
    // 参数非法抛 std::invalid_argument；错误线程抛 std::logic_error。
    // 创建监听失败抛 std::runtime_error。
    // 构造成功即意味着已完成绑定与监听，但**尚未登记读事件**：
    // 登记要等 StartAccepting()，以便调用方先把 accept 回调设置好。
    Acceptor(EventLoop *loop, uint16_t port);
    // 若已 StartAccepting() 则注销读事件登记，再关闭监听套接字。
    // 所属 EventLoop 必须仍然存活，且析构需发生在所属线程——注销必须早于
    // EventLoop 析构，否则会在已销毁的 epoll 上做 EPOLL_CTL_DEL。
    ~Acceptor();
    Acceptor(const Acceptor &) = delete;
    Acceptor &operator=(const Acceptor &) = delete;

    // 登记监听套接字的读事件，开始接收连接。重复调用无副作用。
    // 未调用时 accept 回调不会被执行——登记前到达的连接也不会被 accept。
    void StartAccepting();
    // 所属 loop 线程调用；终止监听并关闭 fd，重复调用无副作用。
    // Stop 后不允许重新 Start；Channel 保留到析构，允许同轮残余事件安全返回。
    void StopAccepting();
    // 须于所属 loop 线程调用，否则抛 std::logic_error。
    // 支持回调内替换或清空，下一条连接生效；对象必须存活至当前事件分发结束。
    void SetAcceptCallback(AcceptCallback cb);
    // 借用监听描述符，不转移所有权。
    int GetListenFd() const noexcept;

  private:
    void HandleRead();
    void DispatchAcceptedFd(int fd);
    void PauseAccepting();
    void HandleRetry();

  private:
    std::unique_ptr<Socket> _listener; // 对监听套接字进行操作
    EventLoop *_loop;                  // 对监听套接字进行事件监控，不拥有它
    std::unique_ptr<Channel> _channel; // 对监听套接字进行事件管理
    AcceptCallback _accept_callback;   // 设置监听回调函数
    // 注册状态独立于读监听：退避期间仍登记，但禁读。
    bool _registered = false;
    bool _accepting = false;
    bool _stopped = false;
    // 构造时预先分配，EMFILE 后不再需要申请描述符。
    int _retry_fd = -1;
    std::unique_ptr<Channel> _retry_channel;
    bool _retry_registered = false;
};
