#include "Util.h"
#include <cstdio>
#include <cstring>
#include <signal.h>

void Util::handle_for_sigpipe()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction(SIGPIPE) failed");
    }
}

void Util::handle_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(sig, &sa, NULL) == -1) {
        perror("sigaction failed");
    }
}
