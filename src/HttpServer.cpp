#include "HttpServer.h"
#include "TcpServer.h"
#include "Logger.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Router.h"
#include "KVStore.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <errno.h>
#include <cctype>
#include <utility>
#include <vector>

// 单次 recv 的缓冲大小
static const size_t RECV_BUF_SIZE = 4096;
// 单连接输入缓冲上限, 防止慢速攻击耗尽内存
static const size_t MAX_IN_BUFFER = 64 * 1024 * 1024;
// 输出队列段数上限(背压保护)
static const size_t MAX_OUT_SEGMENTS = 128;

// ==================== 静态文件工具 ====================

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL 百分号解码(路径段)
static std::string urlDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// 根据文件扩展名返回 Content-Type
static std::string mimeType(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "application/javascript; charset=utf-8";
    if (ext == "txt" || ext == "md") return "text/plain; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "webp") return "image/webp";
    if (ext == "pdf") return "application/pdf";
    if (ext == "wasm") return "application/wasm";
    return "application/octet-stream";
}

// 静态文件根目录: 兼容从项目根目录或 out/ 下运行
static const std::string& staticRoot()
{
    static const std::string root = []() {
        struct stat st;
        if (::stat("static", &st) == 0 && S_ISDIR(st.st_mode)) return std::string("static");
        if (::stat("../static", &st) == 0 && S_ISDIR(st.st_mode)) return std::string("../static");
        return std::string("static"); // 兜底: 目录不存在时 open 会失败并返回 404
    }();
    return root;
}

// ==================== 路由处理器 ====================

static void homeHandler(const HttpRequest&, const std::vector<std::string>&, HttpResponse& resp)
{
    resp.setBody("Hello, World! This is the home page.");
}

static void aboutHandler(const HttpRequest&, const std::vector<std::string>&, HttpResponse& resp)
{
    resp.setBody("About Us: This is a simple HTTP server.");
}

static void echoHandler(const HttpRequest& req, const std::vector<std::string>&, HttpResponse& resp)
{
    resp.setBody("Received POST data: " + req.getBody());
}

// 静态文件: 参数为 /static/ 之后的相对路径
static void staticFileHandler(const HttpRequest&, const std::vector<std::string>& params, HttpResponse& resp)
{
    if (params.empty()) {
        resp.setStatus(404);
        resp.setBody("404 Not Found");
        return;
    }
    std::string rel = urlDecode(params[0]);
    if (rel.empty() || rel[0] == '/') {
        resp.setStatus(403);
        resp.setBody("403 Forbidden");
        return;
    }
    // 路径穿越防护: 拒绝任何 ".." 段, 防止读取静态目录之外的文件
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t end = rel.find('/', pos);
        if (end == std::string::npos) end = rel.size();
        std::string seg = rel.substr(pos, end - pos);
        if (seg == "..") {
            resp.setStatus(403);
            resp.setBody("403 Forbidden");
            return;
        }
        pos = end + 1;
        if (pos > rel.size()) break;
    }
    resp.setFile(staticRoot() + "/" + rel);
}

// 内存 KV 存储
static void kvPutHandler(const HttpRequest& req, const std::vector<std::string>& params, HttpResponse& resp)
{
    if (params.empty()) { resp.setStatus(404); resp.setBody("not found"); return; }
    KVStore::instance().put(params[0], req.getBody());
    resp.setBody("ok");
}

static void kvGetHandler(const HttpRequest&, const std::vector<std::string>& params, HttpResponse& resp)
{
    if (params.empty()) { resp.setStatus(404); resp.setBody("not found"); return; }
    std::string value;
    if (KVStore::instance().get(params[0], value)) {
        resp.setBody(value);
    } else {
        resp.setStatus(404);
        resp.setBody("not found");
    }
}

static void kvDeleteHandler(const HttpRequest&, const std::vector<std::string>& params, HttpResponse& resp)
{
    if (params.empty()) { resp.setStatus(404); resp.setBody("not found"); return; }
    if (KVStore::instance().del(params[0])) {
        resp.setBody("deleted");
    } else {
        resp.setStatus(404);
        resp.setBody("not found");
    }
}

static void kvListHandler(const HttpRequest&, const std::vector<std::string>&, HttpResponse& resp)
{
    resp.setBody(KVStore::instance().dump());
}

// 路由表: 首次调用时构建, 之后只读
static Router& getRouter()
{
    static Router router = []() {
        Router r;
        r.add("GET", "/", homeHandler);
        r.add("GET", "/about", aboutHandler);
        r.add("GET", "/static/*", staticFileHandler);
        r.add("PUT", "/kv/:key", kvPutHandler);
        r.add("GET", "/kv/:key", kvGetHandler);
        r.add("DELETE", "/kv/:key", kvDeleteHandler);
        r.add("GET", "/kv", kvListHandler);
        r.add("POST", "/echo", echoHandler, true); // 示例: 对该路由开启限流
        return r;
    }();
    return router;
}

// ==================== HttpServer 实现 ====================

HttpServer::HttpServer(int fd, TcpServer* server, std::string clientIp)
    : _fd(fd), _server(server), _clientIp(std::move(clientIp)),
      _writeRegistered(false), _keepAlive(false), _closeAfterFlush(false), _closed(false)
{
}

HttpServer::~HttpServer()
{
    closeAllFiles();
}

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
            _outQueue.push_back(OutSegment{OutSegment::BYTES, resp.toString(), -1, 0, 0});
            _closeAfterFlush = true;
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

        // 背压保护: 输出队列过长时直接断开, 避免内存无限增长
        if (_outQueue.size() > MAX_OUT_SEGMENTS) {
            LOG(WARNING, "输出队列超限, 关闭连接 fd: " + std::to_string(_fd));
            _closeAfterFlush = true;
            return;
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

    if (!getRouter().dispatch(req, resp, _clientIp)) {
        // 路由未匹配
        resp.setStatus(404);
        resp.setBody("404 Not Found: The requested resource was not found on this server.");
    }

    if (resp.hasFile()) {
        sendFileResponse(resp.filePath());
    } else {
        _outQueue.push_back(OutSegment{OutSegment::BYTES, resp.toString(), -1, 0, 0});
    }
}

// 打开静态文件, 将响应头作为文本段、文件内容作为文件段依次入队
void HttpServer::sendFileResponse(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG(WARNING, "打开静态文件失败: " + path);
        HttpResponse resp;
        resp.setStatus(404);
        resp.setBody("404 Not Found: The requested file does not exist.");
        _outQueue.push_back(OutSegment{OutSegment::BYTES, resp.toString(), -1, 0, 0});
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        HttpResponse resp;
        resp.setStatus(404);
        resp.setBody("404 Not Found");
        _outQueue.push_back(OutSegment{OutSegment::BYTES, resp.toString(), -1, 0, 0});
        return;
    }

    // 响应头(文本段): Content-Type 由扩展名推断, Content-Length 来自文件大小
    HttpResponse resp;
    resp.setStatus(200);
    resp.addHeader("Content-Type", mimeType(path));
    resp.addHeader("Content-Length", std::to_string(st.st_size));
    resp.addHeader("Connection", _keepAlive ? "keep-alive" : "close");
    if (_keepAlive) {
        resp.addHeader("Keep-Alive", "timeout=30");
    }
    _outQueue.push_back(OutSegment{OutSegment::BYTES, resp.toStringHeaders(), -1, 0, 0});

    // 文件内容(文件段): 由 sendfile 零拷贝发送, 不进入用户态缓冲
    _outQueue.push_back(OutSegment{OutSegment::FILE, "", fd, 0, static_cast<size_t>(st.st_size)});
}

// 按序发送输出队列: 文本段走 send 系统调用, 文件段走 sendfile 零拷贝
void HttpServer::tryFlush()
{
    while (!_outQueue.empty()) {
        OutSegment& seg = _outQueue.front();

        if (seg.type == OutSegment::BYTES) {
            while (!seg.bytes.empty()) {
                ssize_t n = send(_fd, seg.bytes.data(), seg.bytes.size(), MSG_NOSIGNAL);
                if (n > 0) {
                    seg.bytes.erase(0, static_cast<size_t>(n));
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    registerWrite();
                    return;
                } else {
                    LOG(WARNING, "send 错误 fd: " + std::to_string(_fd) + " errno: " + std::to_string(errno));
                    _server->closeConnection(_fd);
                    return;
                }
            }
            _outQueue.pop_front();
            continue;
        }

        // FILE 段: sendfile 零拷贝(内核态直接 DMA 到网卡, 不经过用户态缓冲)
        while (seg.fileRemaining > 0) {
            ssize_t n = sendfile(_fd, seg.fileFd, &seg.fileOffset, seg.fileRemaining);
            if (n > 0) {
                seg.fileRemaining -= static_cast<size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                registerWrite();
                return;
            } else {
                LOG(WARNING, "sendfile 错误 fd: " + std::to_string(_fd) + " errno: " + std::to_string(errno));
                _server->closeConnection(_fd);
                return;
            }
        }
        close(seg.fileFd);
        seg.fileFd = -1;
        _outQueue.pop_front();
    }

    // 全部发送完成
    if (_writeRegistered) {
        unregisterWrite();
    }
    if (_closeAfterFlush) {
        _server->closeConnection(_fd);
    }
}

void HttpServer::closeAllFiles()
{
    for (auto& seg : _outQueue) {
        if (seg.type == OutSegment::FILE && seg.fileFd >= 0) {
            close(seg.fileFd);
            seg.fileFd = -1;
        }
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
