#ifndef _HTTPSERVER_H_
#define _HTTPSERVER_H_

#include <string>
#include <atomic>
#include <mutex>

class TcpServer;
class HttpRequest;

// 每个 TCP 连接对应一个 HttpServer 对象, 在连接生命周期内持续存在:
// - 内部维护输入/输出缓冲, 支持"半包累积、跨事件解析"与 HTTP 流水线
// - 输入输出缓冲由 _ioMutex 保护, 线程池与事件循环并发访问时保持安全
class HttpServer {
public:
    HttpServer(int fd, TcpServer* server);
    ~HttpServer();

    void handleRead();   // 读取并处理请求(由线程池调用)
    void handleWrite();  // 冲刷输出缓冲(EPOLLOUT 事件触发时调用)
    void markClosed();   // 标记连接已关闭(原子置位, 幂等)

    int fd() const { return _fd; }
    bool closed() const { return _closed.load(); }

private:
    void recvIntoBuffer();   // 非阻塞读取直到 EAGAIN, 追加到输入缓冲
    void processRequests();  // 从输入缓冲解析并响应(支持流水线)
    void dispatch(const HttpRequest& req);  // 路由与响应构造
    void tryFlush();         // 尝试发送输出缓冲, 发不完则注册 EPOLLOUT
    void registerWrite();
    void unregisterWrite();

    int _fd;
    TcpServer* _server;

    std::string _inBuffer;    // 输入缓冲: 跨事件累积
    std::string _outBuffer;   // 输出缓冲: 待发送数据
    bool _writeRegistered;    // 是否已注册 EPOLLOUT
    bool _keepAlive;          // 当前连接是否保持
    bool _closeAfterFlush;    // 输出缓冲发完后是否关闭连接

    std::atomic<bool> _closed;
    std::mutex _ioMutex;
};

#endif
