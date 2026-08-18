#ifndef _HTTPSERVER_H_
#define _HTTPSERVER_H_

#include <string>
#include <deque>
#include <atomic>
#include <mutex>
#include <sys/types.h>

class TcpServer;
class HttpRequest;

// 每个 TCP 连接对应一个 HttpServer 对象, 在连接生命周期内持续存在:
// - 输入缓冲跨事件累积, 支持半包解析与 HTTP 流水线
// - 输出采用"段队列": 文本段(响应头/小响应)与文件段(sendfile 零拷贝)按序发送,
//   保证流水线下多个响应的顺序正确
// - 输入输出缓冲由 _ioMutex 保护, 线程池与事件循环并发访问时保持安全
class HttpServer {
public:
    HttpServer(int fd, TcpServer* server, std::string clientIp);
    ~HttpServer();

    void handleRead();   // 读取并处理请求(由线程池调用)
    void handleWrite();  // 冲刷输出队列(EPOLLOUT 事件触发时调用)
    void markClosed();   // 标记连接已关闭(原子置位, 幂等)

    int fd() const { return _fd; }
    bool closed() const { return _closed.load(); }

private:
    // 输出段: 一段文本或一段文件(由 sendfile 发送)
    struct OutSegment {
        enum Type { BYTES, FILE } type;
        std::string bytes;      // type == BYTES
        int fileFd;             // type == FILE
        off_t fileOffset;
        size_t fileRemaining;
    };

    void recvIntoBuffer();   // 非阻塞读取直到 EAGAIN, 追加到输入缓冲
    void processRequests();  // 从输入缓冲解析并响应(支持流水线)
    void dispatch(const HttpRequest& req);  // 路由与响应构造
    void tryFlush();         // 按序发送输出队列
    void sendFileResponse(const std::string& path); // 打开文件并入队头+文件段
    void closeAllFiles();    // 关闭队列中所有文件段
    void registerWrite();
    void unregisterWrite();

    int _fd;
    TcpServer* _server;
    std::string _clientIp;   // 客户端 IP(用于限流)

    std::string _inBuffer;           // 输入缓冲: 跨事件累积
    std::deque<OutSegment> _outQueue; // 输出队列: 按序发送
    bool _writeRegistered;           // 是否已注册 EPOLLOUT
    bool _keepAlive;                 // 当前连接是否保持
    bool _closeAfterFlush;           // 输出队列清空后是否关闭连接

    std::atomic<bool> _closed;
    std::mutex _ioMutex;
};

#endif
