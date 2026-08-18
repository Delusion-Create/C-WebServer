#ifndef _UTIL_H_
#define _UTIL_H_

class Util
{
public:
    // 忽略 SIGPIPE: 防止向已关闭连接写数据时进程被信号杀死
    static void handle_for_sigpipe();

    // 安装指定信号的处理函数
    static void handle_signal(int sig, void (*handler)(int));
};

#endif
