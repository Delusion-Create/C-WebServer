#ifndef _TCPTIMER_H_
#define _TCPTIMER_H_

#include "TimerNode.h"
#include <mutex>
#include <unordered_map>
#include <set>
#include <memory>

// 连接定时器管理器:
// - multiset(内部为红黑树)按过期时间有序, O(1) 获取最早超时的节点
// - unordered_map(哈希表)按 fd 索引, O(1) 定位/删除指定连接的定时器
// - shared_ptr + 惰性删除标记管理节点生命周期
class TcpTimer {
public:
    TcpTimer();
    ~TcpTimer();

    // 为 fd 添加/刷新定时器(已存在时惰性删除旧节点)
    void addTimer(int fd, int timeout, TimerCallback cb);
    void removeTimer(int fd);

    // 触发所有过期定时器, 返回其回调
    void checkExpired();

    // 距下一个定时器超时的剩余毫秒数, 用于 epoll_wait 的超时参数
    int getNextTimeout() const;

    size_t size() const;

private:
    std::multiset<std::shared_ptr<TimerNode>, TimerNodeCompare> _timerSet;
    std::unordered_map<int, std::shared_ptr<TimerNode>> _timerMap;
    mutable std::mutex _mutex;
};

#endif
