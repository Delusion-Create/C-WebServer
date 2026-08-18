#include "TcpServer.h"
#include "HttpServer.h"
#include "Logger.h"
#include "Util.h"
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <vector>
#include <errno.h>
#include <utility>
#include <thread>
#include <algorithm>
#include <sys/eventfd.h>
#include <cstring>

// Keep-Alive 超时时间(毫秒)
static const int KEEP_ALIVE_TIMEOUT_MS = 30000;

// 全局唤醒 fd: 信号处理器通过向它写入数据唤醒阻塞中的 epoll_wait
static int g_wakeupFd = -1;

static void signalHandler(int)
{
    if (g_wakeupFd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(g_wakeupFd, &one, sizeof(one));
        (void)r;
    }
}

TcpServer* TcpServer::GetInstance(std::string ip, int port)
{
    static TcpServer instance(ip, port);
    return &instance;
}

TcpServer::TcpServer(std::string ip, int port)
    : _ip(ip), _port(port), _sfd(-1), _epfd(-1), _wakeupFd(-1),
      // 线程池大小 = CPU 核数(IO 密集场景下与核数相当即可), 自动适配部署机器
      _pool(std::max(1u, std::thread::hardware_concurrency()))
{
    initialServer();
}

TcpServer::~TcpServer()
{
    shutdownAll();
    if (_sfd >= 0) close(_sfd);
    if (_epfd >= 0) close(_epfd);
    if (_wakeupFd >= 0) close(_wakeupFd);
}

void TcpServer::setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TcpServer::initialServer()
{
    _sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (_sfd < 0) {
        LOG(FATAL, "socket error");
        exit(1);
    }

    // 端口复用: 便于快速重启
    int opt = 1;
    setsockopt(_sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    _sin.sin_family = AF_INET;
    _sin.sin_port = htons(static_cast<uint16_t>(_port));
    _sin.sin_addr.s_addr = inet_addr(_ip.c_str());

    if (bind(_sfd, reinterpret_cast<sockaddr*>(&_sin), sizeof(_sin)) < 0) {
        LOG(FATAL, "bind error");
        exit(1);
    }
    if (listen(_sfd, 1024) < 0) {
        LOG(FATAL, "listen error");
        exit(1);
    }
    setNonBlocking(_sfd);

    _epfd = epoll_create1(0);
    if (_epfd < 0) {
        LOG(FATAL, "epoll_create1 error");
        exit(1);
    }

    epoll_event ev;
    ev.data.fd = _sfd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(_epfd, EPOLL_CTL_ADD, _sfd, &ev);

    // 注册唤醒 fd: 收到 SIGINT/SIGTERM 时优雅退出
    _wakeupFd = eventfd(0, EFD_NONBLOCK);
    if (_wakeupFd >= 0) {
        ev.data.fd = _wakeupFd;
        ev.events = EPOLLIN;
        epoll_ctl(_epfd, EPOLL_CTL_ADD, _wakeupFd, &ev);
        g_wakeupFd = _wakeupFd;
        Util::handle_signal(SIGINT, signalHandler);
        Util::handle_signal(SIGTERM, signalHandler);
    }

    _pool.startThreadPool();
    LOG(INFO, "server initial success: " + _ip + ":" + std::to_string(_port));
}

void TcpServer::run()
{
    std::vector<epoll_event> events(MAXEPOLLEVENTS);
    bool running = true;

    while (running) {
        // epoll_wait 超时 = 最近定时器的剩余时间, 由事件循环统一检查超时连接
        int timeoutMs = _timer.getNextTimeout();
        int num = epoll_wait(_epfd, events.data(), static_cast<int>(events.size()), timeoutMs);
        if (num < 0) {
            if (errno == EINTR) continue;
            LOG(ERROR, "epoll_wait error");
            continue;
        }

        for (int i = 0; i < num; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == _wakeupFd) {
                // 收到退出信号: 排空唤醒 fd 并退出循环
                uint64_t v;
                while (read(_wakeupFd, &v, sizeof(v)) > 0) {}
                running = false;
                break;
            } else if (fd == _sfd) {
                acceptConnection();
            } else {
                handleEvent(fd, ev);
            }
        }

        _timer.checkExpired();
    }

    LOG(INFO, "server shutdown");
}

void TcpServer::acceptConnection()
{
    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int cfd = accept(_sfd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
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

        // 记录客户端 IP(用于限流等按来源区分的策略)
        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, sizeof(ipStr));

        auto conn = std::make_shared<HttpServer>(cfd, this, std::string(ipStr));
        {
            std::lock_guard<std::mutex> lock(_mapMutex);
            _clients[cfd] = conn;
        }

        // 新连接立即挂一个超时定时器: 30 秒无活动则回收
        setupTimer(cfd, KEEP_ALIVE_TIMEOUT_MS);
        LOG(INFO, "新连接 fd: " + std::to_string(cfd));
    }
}

void TcpServer::handleEvent(int fd, uint32_t events)
{
    // 在锁内取出连接对象的 shared_ptr: 即使连接随后被关闭, 对象也不会被销毁
    std::shared_ptr<HttpServer> conn;
    {
        std::lock_guard<std::mutex> lock(_mapMutex);
        auto it = _clients.find(fd);
        if (it == _clients.end()) return; // 连接已关闭
        conn = it->second;
    }

    // 硬错误(EPOLLERR/EPOLLHUP): 直接关闭
    if (events & (EPOLLERR | EPOLLHUP)) {
        closeConnection(fd);
        return;
    }

    // EPOLLIN: 有可读数据; EPOLLRDHUP: 对端已关闭写端(半关闭)。
    // 注意: 半关闭时接收队列中可能仍有未读数据, 不能直接 close(否则内核会
    // 对带未读数据的 socket 发 RST)。必须让读任务把数据读完, recv 返回 0 后
    // 再由连接自行干净关闭。
    if (events & (EPOLLIN | EPOLLRDHUP)) {
        _pool.addTask([conn]() { conn->handleRead(); });
    }
    if (events & EPOLLOUT) {
        _pool.addTask([conn]() { conn->handleWrite(); });
    }
}

// 幂等关闭: 先从连接表中移除, 之后对同一 fd 的关闭调用直接返回,
// 避免多线程路径(定时器回调/线程池/事件循环)对同一 fd 二次 close
void TcpServer::closeConnection(int fd)
{
    std::shared_ptr<HttpServer> conn;
    {
        std::lock_guard<std::mutex> lock(_mapMutex);
        auto it = _clients.find(fd);
        if (it == _clients.end()) return; // 已关闭
        conn = it->second;
        _clients.erase(it);
    }

    conn->markClosed();
    _timer.removeTimer(fd);
    epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    LOG(INFO, "连接关闭 fd: " + std::to_string(fd));
}

void TcpServer::setupTimer(int fd, int timeout)
{
    _timer.addTimer(fd, timeout, [this, fd]() {
        LOG(INFO, "连接超时关闭 fd: " + std::to_string(fd));
        closeConnection(fd);
    });
}

void TcpServer::removeTimer(int fd)
{
    _timer.removeTimer(fd);
}

void TcpServer::updateEvents(int fd, uint32_t events)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev);
}

void TcpServer::shutdownAll()
{
    std::vector<std::shared_ptr<HttpServer>> conns;
    {
        std::lock_guard<std::mutex> lock(_mapMutex);
        for (auto& kv : _clients) {
            conns.push_back(kv.second);
        }
        _clients.clear();
    }
    for (auto& conn : conns) {
        epoll_ctl(_epfd, EPOLL_CTL_DEL, conn->fd(), NULL);
        close(conn->fd());
    }
}
