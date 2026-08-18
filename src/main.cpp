#include "TcpServer.h"
#include "Logger.h"
#include "Util.h"

#include <iostream>
#include <cstdlib>

int main(int argc, const char* argv[])
{
    // 初始化异步日志系统
    AsyncLogger::getInstance()->init("server.log", INFO, 10000);

    // 忽略 SIGPIPE, 防止向已关闭连接写数据时进程被信号杀死
    Util::handle_for_sigpipe();

    std::string ip = "127.0.0.1";
    int port = 8848;

    if (argc == 3) {
        ip = argv[1];
        port = std::atoi(argv[2]);
    } else if (argc != 1) {
        std::cout << "用法: " << argv[0] << " [ip] [port]" << std::endl;
        std::cout << "示例: " << argv[0] << "             # 默认 127.0.0.1:8848" << std::endl;
        std::cout << "      " << argv[0] << " 0.0.0.0 8080" << std::endl;
        return 1;
    }

    TcpServer* server = TcpServer::GetInstance(ip, port);
    server->run();

    AsyncLogger::getInstance()->stop();
    return 0;
}
