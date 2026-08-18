#ifndef _TCPSERVER_H_
#define _TCPSERVER_H_

#include "ThreadPool.h"
#include "TcpTimer.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/epoll.h>

// epoll_wait 单次最多返回的事件数
#define MAXEPOLLEVENTS 4096

class HttpServer;

// 基于 Reactor 模型的 TCP 服务器:
// 主线程负责 epoll 事件分发与定时器检查, 请求的读写/解析/响应下沉到线程池,
// 避免耗时的业务处理阻塞事件循环
class TcpServer {
public:
    static TcpServer* GetInstance(std::string ip = "127.0.0.1", int port = 8848);
    void run();
    ~TcpServer();

    // 供 HttpServer 调用的公共接口
    void closeConnection(int fd);               // 幂等关闭连接, 可被任意线程调用
    void setupTimer(int fd, int timeout);       // 为连接设置/刷新超时定时器
    void removeTimer(int fd);
    void updateEvents(int fd, uint32_t events); // 修改连接在 epoll 中注册的事件

private:
    TcpServer(std::string ip, int port);
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void initialServer();
    void setNonBlocking(int fd);
    void acceptConnection();
    void handleEvent(int fd, uint32_t events);
    void shutdownAll();

    std::string _ip;
    int _port;
    int _sfd;
    int _epfd;
    int _wakeupFd;   // eventfd: 供信号处理器唤醒事件循环以实现优雅退出
    sockaddr_in _sin;

    ThreadPool _pool;
    TcpTimer _timer;

    // fd -> 连接对象; 对 _clients 的所有访问都在 _mapMutex 保护下
    std::unordered_map<int, std::shared_ptr<HttpServer>> _clients;
    std::mutex _mapMutex;
};

#endif
