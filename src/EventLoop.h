#ifndef _EVENTLOOP_H_
#define _EVENTLOOP_H_

#include "TcpTimer.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <sys/epoll.h>

class HttpServer;

// 单事件循环(one loop per thread):
// 每个 CPU 核一个 EventLoop, 各自在自己的线程中 epoll_wait, 独占一批连接。
// 连接的所有读写/解析/响应都在所属 loop 线程内串行完成, 因此连接状态无需加锁。
//
// 监听 socket 使用 SO_REUSEPORT: 多个 loop 的监听 socket 绑定同一端口,
// 由内核按连接四元组哈希将新连接近似均匀地分发到各 loop, 天然实现多核负载均衡。
class EventLoop {
public:
    EventLoop(int id, const std::string& ip, int port);
    ~EventLoop();

    // 事件循环主函数(在独立线程中运行, 直到收到退出信号)
    void loop();

    // 供 HttpServer 调用(调用方均为本 loop 线程, 无需加锁)
    void updateEvents(int fd, uint32_t events);   // 修改连接注册事件
    void closeConnection(int fd);                 // 幂等关闭连接
    void setupTimer(int fd, int timeout);         // 设置/刷新超时定时器
    void removeTimer(int fd);

    int id() const { return _id; }

private:
    void initialListenSocket();
    void acceptConnection();
    void handleEvent(int fd, uint32_t events);
    void closeAllConnections();

    int _id;
    std::string _ip;
    int _port;
    int _listenFd;
    int _epfd;
    int _wakeupFd;   // 本 loop 私有唤醒 fd(eventfd), 构造时登记到全局广播表

    TcpTimer _timer;

    // fd -> 连接对象; 仅由本 loop 线程访问, 无需加锁
    std::unordered_map<int, std::shared_ptr<HttpServer>> _clients;
};

#endif
