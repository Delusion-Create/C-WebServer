#include "TcpTimer.h"
#include <chrono>
#include <vector>
#include <utility>

TcpTimer::TcpTimer() {}

TcpTimer::~TcpTimer()
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& pair : _timerMap) {
        pair.second->markDeleted();
    }
    _timerMap.clear();
    _timerSet.clear();
}

void TcpTimer::addTimer(int fd, int timeout, TimerCallback cb)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _timerMap.find(fd);
    if (it != _timerMap.end()) {
        it->second->markDeleted(); // 惰性删除旧节点
    }
    auto node = std::make_shared<TimerNode>(fd, timeout, std::move(cb));
    _timerSet.insert(node);
    _timerMap[fd] = node;
}

void TcpTimer::removeTimer(int fd)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _timerMap.find(fd);
    if (it != _timerMap.end()) {
        it->second->markDeleted();
        _timerMap.erase(it);
    }
}

void TcpTimer::checkExpired()
{
    std::vector<std::shared_ptr<TimerNode>> expiredNodes;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // multiset 按过期时间升序, 遇到第一个未过期节点即可停止扫描
        for (auto it = _timerSet.begin(); it != _timerSet.end();) {
            auto node = *it;
            if (node->isDeleted()) {
                it = _timerSet.erase(it);   // 顺带清理惰性删除的节点
            } else if (node->isExpired()) {
                expiredNodes.push_back(node);
                it = _timerSet.erase(it);
            } else {
                break;
            }
        }
    }

    // 锁外触发回调, 避免回调内部再次加锁造成死锁
    for (auto& node : expiredNodes) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _timerMap.erase(node->getFd());
        }
        node->triggerCallback();
    }
}

int TcpTimer::getNextTimeout() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_timerSet.empty()) return -1;

    for (auto it = _timerSet.begin(); it != _timerSet.end(); ++it) {
        if ((*it)->isDeleted()) continue;
        auto now = std::chrono::steady_clock::now();
        if ((*it)->getExpireTime() <= now) return 0;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      (*it)->getExpireTime() - now)
                      .count();
        return ms > 0 ? static_cast<int>(ms) : 0;
    }
    return -1; // 全部为已删除节点
}

size_t TcpTimer::size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _timerMap.size();
}
