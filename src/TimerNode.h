#ifndef _TIMERNODE_H_
#define _TIMERNODE_H_

#include <functional>
#include <chrono>
#include <memory>
#include <atomic>

using TimerCallback = std::function<void()>;

// 定时器节点: 绑定 fd 与过期时间
// 采用"惰性删除"策略: 删除时只置标记位, 由管理器在遍历时统一清理,
// 避免并发环境下直接析构仍被引用的节点
class TimerNode {
public:
    TimerNode(int fd, int timeout, TimerCallback cb);
    ~TimerNode();

    std::chrono::steady_clock::time_point getExpireTime() const;
    int getFd() const;
    bool isExpired() const;
    void triggerCallback();
    TimerCallback getCallback() const;
    void markDeleted();
    bool isDeleted() const;

private:
    int _fd;
    std::chrono::steady_clock::time_point _expireTime;
    TimerCallback _callback;
    std::atomic<bool> _deleted;
};

// multiset 排序比较器: 按过期时间升序
struct TimerNodeCompare {
    bool operator()(const std::shared_ptr<TimerNode>& a,
                    const std::shared_ptr<TimerNode>& b) const;
};

#endif
