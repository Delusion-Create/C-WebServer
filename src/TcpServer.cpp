#include "TcpServer.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Util.h"
#include <unistd.h>
#include <signal.h>
#include <algorithm>

// 全局退出标志: 信号处理器置位, 所有事件循环检查
volatile sig_atomic_t g_stop = 0;

// 各 loop 的私有唤醒 fd(由 EventLoop 构造时登记)
extern int g_loopWakeupFds[64];
extern int g_loopWakeupCount;

static void signalHandler(int)
{
    g_stop = 1;
    // 向所有事件循环广播写入(eventfd 唤醒回调是排他的, 必须逐个写)
    uint64_t one = 1;
    for (int i = 0; i < g_loopWakeupCount; ++i) {
        ssize_t r = write(g_loopWakeupFds[i], &one, sizeof(one));
        (void)r;
    }
}

TcpServer* TcpServer::GetInstance(std::string ip, int port)
{
    static TcpServer instance(ip, port);
    return &instance;
}

TcpServer::TcpServer(std::string ip, int port) : _ip(ip), _port(port)
{
}

TcpServer::~TcpServer()
{
}

void TcpServer::run()
{
    // 忽略 SIGPIPE, 安装退出信号处理
    Util::handle_for_sigpipe();
    Util::handle_signal(SIGINT, signalHandler);
    Util::handle_signal(SIGTERM, signalHandler);

    // one loop per core: 每个 CPU 核一个事件循环, 各自独立线程
    int loopCount = std::max(1u, std::thread::hardware_concurrency());
    LOG(INFO, "启动 " + std::to_string(loopCount) + " 个事件循环(one loop per core)");

    _loops.reserve(loopCount);
    _loopThreads.reserve(loopCount);
    for (int i = 0; i < loopCount; ++i) {
        std::unique_ptr<EventLoop> loop(new EventLoop(i, _ip, _port));
        EventLoop* raw = loop.get();
        _loops.push_back(std::move(loop));
        _loopThreads.emplace_back([raw]() { raw->loop(); });
    }

    // 主线程等待所有事件循环退出
    for (auto& t : _loopThreads) {
        t.join();
    }
    _loopThreads.clear();
    LOG(INFO, "server shutdown");
}
