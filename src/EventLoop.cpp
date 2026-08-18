#include "EventLoop.h"
#include "HttpServer.h"
#include "Logger.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include <errno.h>
#include <cstdint>
#include <signal.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>

// epoll_wait 单次最多返回的事件数
#define MAXEPOLLEVENTS 4096
// Keep-Alive 超时时间(毫秒)
static const int KEEP_ALIVE_TIMEOUT_MS = 30000;

// 全局退出标志: 由 TcpServer 定义, 信号处理器置位
extern volatile sig_atomic_t g_stop;

// 全局 loop 唤醒 fd 广播表: 每个 EventLoop 构造时把自己的 eventfd 登记进来。
// 注意: 不能所有 loop 共享一个 eventfd——epoll 在 fd 上的等待回调是排他的,
// wake_up_poll(nr_exclusive=1) 只会唤醒一个 epoll 实例, 其余 loop 收不到事件。
int g_loopWakeupFds[64];
int g_loopWakeupCount = 0;

EventLoop::EventLoop(int id, const std::string& ip, int port)
    : _id(id), _ip(ip), _port(port), _listenFd(-1), _epfd(-1), _wakeupFd(-1)
{
    initialListenSocket();
}

EventLoop::~EventLoop()
{
    if (_listenFd >= 0) close(_listenFd);
    if (_epfd >= 0) close(_epfd);
    if (_wakeupFd >= 0) close(_wakeupFd);
}

static void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EventLoop::initialListenSocket()
{
    _listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listenFd < 0) {
        LOG(FATAL, "socket error");
        exit(1);
    }

    int opt = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // 关键: SO_REUSEPORT 允许多个监听 socket 绑定同一端口,
    // 内核将新连接按四元组哈希分发到各 socket, 实现多核并行 accept
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(static_cast<uint16_t>(_port));
    sin.sin_addr.s_addr = inet_addr(_ip.c_str());

    if (bind(_listenFd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        LOG(FATAL, "bind error");
        exit(1);
    }
    // backlog 尽量放大; 实际生效值受内核 net.core.somaxconn 限制(压测前需调大)
    if (listen(_listenFd, 4096) < 0) {
        LOG(FATAL, "listen error");
        exit(1);
    }
    setNonBlocking(_listenFd);

    _epfd = epoll_create1(0);
    if (_epfd < 0) {
        LOG(FATAL, "epoll_create1 error");
        exit(1);
    }

    epoll_event ev;
    ev.data.fd = _listenFd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(_epfd, EPOLL_CTL_ADD, _listenFd, &ev);

    // 本 loop 私有唤醒 fd: 退出信号到达时由信号处理器广播写入
    _wakeupFd = eventfd(0, EFD_NONBLOCK);
    if (_wakeupFd < 0) {
        LOG(FATAL, "eventfd error");
        exit(1);
    }
    if (g_loopWakeupCount < 64) {
        g_loopWakeupFds[g_loopWakeupCount++] = _wakeupFd;
    }
    ev.data.fd = _wakeupFd;
    ev.events = EPOLLIN;
    epoll_ctl(_epfd, EPOLL_CTL_ADD, _wakeupFd, &ev);

    LOG(INFO, "loop[" + std::to_string(_id) + "] listen " + _ip + ":" +
              std::to_string(_port) + " listenFd=" + std::to_string(_listenFd));
}

void EventLoop::loop()
{
    std::vector<epoll_event> events(MAXEPOLLEVENTS);
    bool running = true;

    while (running && !g_stop) {
        // epoll_wait 超时 = 最近定时器的剩余时间, 由本循环统一检查超时连接
        int timeoutMs = _timer.getNextTimeout();
        int num = epoll_wait(_epfd, events.data(), static_cast<int>(events.size()), timeoutMs);
        if (num < 0) {
            if (errno == EINTR) continue;
            LOG(ERROR, "loop[" + std::to_string(_id) + "] epoll_wait error");
            continue;
        }

        for (int i = 0; i < num; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == _wakeupFd) {
                // 收到退出信号
                uint64_t v;
                while (read(_wakeupFd, &v, sizeof(v)) > 0) {}
                running = false;
                break;
            } else if (fd == _listenFd) {
                acceptConnection();
            } else {
                handleEvent(fd, ev);
            }
        }

        _timer.checkExpired();
    }

    closeAllConnections();
    LOG(INFO, "loop[" + std::to_string(_id) + "] exit");
}

void EventLoop::acceptConnection()
{
    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int cfd = accept(_listenFd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG(ERROR, "accept error errno=" + std::to_string(errno));
            break;
        }

        setNonBlocking(cfd);

        epoll_event ev;
        ev.data.fd = cfd;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        epoll_ctl(_epfd, EPOLL_CTL_ADD, cfd, &ev);

        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, sizeof(ipStr));

        // 连接归属本 loop: 后续所有操作都在本线程完成, map 无需加锁
        auto conn = std::make_shared<HttpServer>(cfd, this, std::string(ipStr));
        _clients[cfd] = conn;

        setupTimer(cfd, KEEP_ALIVE_TIMEOUT_MS);
        // 连接级日志用 DEBUG: 短连接风暴场景下每连接 2 条 INFO 会放大日志线程压力
        LOG(DEBUG, "loop[" + std::to_string(_id) + "] 新连接 fd: " + std::to_string(cfd));
    }
}

void EventLoop::handleEvent(int fd, uint32_t events)
{
    auto it = _clients.find(fd);
    if (it == _clients.end()) return; // 连接已关闭
    auto conn = it->second;

    // 硬错误(EPOLLERR/EPOLLHUP): 直接关闭
    if (events & (EPOLLERR | EPOLLHUP)) {
        closeConnection(fd);
        return;
    }

    // EPOLLIN: 可读; EPOLLRDHUP: 对端半关闭。
    // 半关闭时接收队列可能仍有未读数据, 不能直接 close(否则内核会对带未读数据的
    // socket 发 RST), 必须由读操作把数据读完, recv 返回 0 后再干净关闭。
    if (events & (EPOLLIN | EPOLLRDHUP)) {
        conn->handleRead();
    }
    if (events & EPOLLOUT) {
        conn->handleWrite();
    }
}

// 幂等关闭: 先从连接表移除, 之后对同一 fd 的关闭调用直接返回
void EventLoop::closeConnection(int fd)
{
    auto it = _clients.find(fd);
    if (it == _clients.end()) return; // 已关闭

    auto conn = it->second;
    _clients.erase(it);

    conn->markClosed();
    _timer.removeTimer(fd);
    epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    LOG(DEBUG, "loop[" + std::to_string(_id) + "] 连接关闭 fd: " + std::to_string(fd));
}

void EventLoop::setupTimer(int fd, int timeout)
{
    _timer.addTimer(fd, timeout, [this, fd]() {
        LOG(INFO, "loop[" + std::to_string(_id) + "] 连接超时关闭 fd: " + std::to_string(fd));
        closeConnection(fd);
    });
}

void EventLoop::removeTimer(int fd)
{
    _timer.removeTimer(fd);
}

void EventLoop::updateEvents(int fd, uint32_t events)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev);
}

void EventLoop::closeAllConnections()
{
    for (auto& kv : _clients) {
        epoll_ctl(_epfd, EPOLL_CTL_DEL, kv.first, NULL);
        close(kv.first);
    }
    _clients.clear();
}
