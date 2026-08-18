#ifndef _TCPSERVER_H_
#define _TCPSERVER_H_

#include <string>
#include <vector>
#include <memory>
#include <thread>

class EventLoop;

// MultiReactor 总控:
// 创建 N 个 EventLoop(每核一个), 每个 EventLoop 独占自己的线程与 epoll 实例,
// 监听 socket 使用 SO_REUSEPORT 由内核负载均衡。本类只负责装配与退出协调。
class TcpServer {
public:
    static TcpServer* GetInstance(std::string ip = "127.0.0.1", int port = 8848);
    void run();
    ~TcpServer();

private:
    TcpServer(std::string ip, int port);
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    std::string _ip;
    int _port;

    std::vector<std::unique_ptr<EventLoop>> _loops;
    std::vector<std::thread> _loopThreads;
};

#endif
