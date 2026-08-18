#include "HttpServer.h"
#include "TcpServer.h"
#include "Logger.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>

// 单次 recv 的缓冲大小
static const size_t RECV_BUF_SIZE = 4096;
// 单连接输入缓冲上限, 防止慢速攻击耗尽内存
static const size_t MAX_IN_BUFFER = 64 * 1024 * 1024;
// 长连接超时(与 TcpServer 中设置的值保持一致)
static const int KEEP_ALIVE_TIMEOUT_MS = 30000;

HttpServer::HttpServer(int fd, TcpServer* server)
    : _fd(fd), _server(server), _writeRegistered(false), _keepAlive(false),
      _closeAfterFlush(false), _closed(false)
{
}

HttpServer::~HttpServer() {}

void HttpServer::markClosed()
{
    _closed.store(true);
}

// 非阻塞读取直到 EAGAIN: 边缘触发(ET)模式下必须一次把可读数据全部读完
void HttpServer::recvIntoBuffer()
{
    char buf[RECV_BUF_SIZE];
    while (true) {
        ssize_t n = recv(_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            _inBuffer.append(buf, static_cast<size_t>(n));
            if (_inBuffer.size() > MAX_IN_BUFFER) {
                LOG(WARNING, "输入缓冲超限, 关闭连接 fd: " + std::to_string(_fd));
                _server->closeConnection(_fd);
                return;
            }
        } else if (n == 0) {
            _server->closeConnection(_fd); // 对端主动关闭
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; // 数据已读尽
            }
            LOG(WARNING, "recv 错误 fd: " + std::to_string(_fd) + " errno: " + std::to_string(errno));
            _server->closeConnection(_fd);
            return;
        }
    }
}

void HttpServer::handleRead()
{
    std::lock_guard<std::mutex> lock(_ioMutex);
    if (_closed.load()) return;

    recvIntoBuffer();
    if (_closed.load()) return; // recv 过程中连接可能已被并发关闭

    processRequests();
    if (_closed.load()) return;

    tryFlush();
}

void HttpServer::handleWrite()
{
    std::lock_guard<std::mutex> lock(_ioMutex);
    if (_closed.load()) return;
    tryFlush();
}

// 从输入缓冲中尽可能多地解析出完整请求并响应(支持流水线)
void HttpServer::processRequests()
{
    while (true) {
        HttpRequest req;
        size_t consumed = 0;
        ParseResult result = req.parse(_inBuffer, consumed);

        if (result == ParseResult::NEED_MORE) {
            break; // 半包: 数据保留在输入缓冲, 等下一次事件继续累积
        }

        if (result == ParseResult::ERROR) {
            // 协议错误: 返回错误码后关闭连接
            HttpResponse resp;
            resp.setStatus(req.errorCode());
            resp.addHeader("Content-Type", "text/plain; charset=utf-8");
            resp.addHeader("Connection", "close");
            resp.setBody("HTTP parse error (" + std::to_string(req.errorCode()) + ")");
            _outBuffer += resp.toString();
            _closeAfterFlush = true; // 协议错误: 响应发送完毕后关闭连接
            return;
        }

        // 解析出一个完整请求
        _keepAlive = req.keepAlive();
        if (!_keepAlive) {
            _closeAfterFlush = true; // Connection: close, 响应发完后关闭
        }
        dispatch(req);
        _inBuffer.erase(0, consumed);

        if (!_keepAlive) {
            return; // 本连接最后一个请求
        }
    }
}

void HttpServer::dispatch(const HttpRequest& req)
{
    HttpResponse resp;
    resp.addHeader("Content-Type", "text/plain; charset=utf-8");
    resp.addHeader("Connection", _keepAlive ? "keep-alive" : "close");
    if (_keepAlive) {
        resp.addHeader("Keep-Alive", "timeout=30");
    }

    const std::string& method = req.getMethod();
    const std::string& path = req.getPath();

    if (method == "GET") {
        if (path == "/") {
            resp.setBody("Hello, World! This is the home page.");
        } else if (path == "/about") {
            resp.setBody("About Us: This is a simple HTTP server.");
        } else {
            resp.setStatus(404);
            resp.setBody("404 Not Found: The requested resource was not found on this server.");
        }
    } else if (method == "POST") {
        resp.setBody("Received POST data: " + req.getBody());
    } else {
        resp.setStatus(405);
        resp.setBody("405 Method Not Allowed: The requested method is not supported.");
    }

    _outBuffer += resp.toString();
}

// 尝试发送输出缓冲; 一次发不完时注册 EPOLLOUT, 等可写事件再继续
void HttpServer::tryFlush()
{
    while (!_outBuffer.empty()) {
        ssize_t n = send(_fd, _outBuffer.data(), _outBuffer.size(), MSG_NOSIGNAL);
        if (n > 0) {
            _outBuffer.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            registerWrite();
            return;
        } else {
            LOG(WARNING, "send 错误 fd: " + std::to_string(_fd) + " errno: " + std::to_string(errno));
            _server->closeConnection(_fd);
            return;
        }
    }

    // 全部发送完成
    if (_writeRegistered) {
        unregisterWrite();
    }
    if (_closeAfterFlush) {
        _server->closeConnection(_fd);
    }
}

void HttpServer::registerWrite()
{
    if (_writeRegistered) return;
    _writeRegistered = true;
    _server->updateEvents(_fd, EPOLLIN | EPOLLET | EPOLLOUT);
}

void HttpServer::unregisterWrite()
{
    if (!_writeRegistered) return;
    _writeRegistered = false;
    _server->updateEvents(_fd, EPOLLIN | EPOLLET);
}
