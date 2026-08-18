#include "TimerNode.h"
#include <utility>

TimerNode::TimerNode(int fd, int timeout, TimerCallback cb)
    : _fd(fd), _callback(std::move(cb)), _deleted(false)
{
    _expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
}

TimerNode::~TimerNode() {}

std::chrono::steady_clock::time_point TimerNode::getExpireTime() const
{
    return _expireTime;
}

int TimerNode::getFd() const
{
    return _fd;
}

bool TimerNode::isExpired() const
{
    return std::chrono::steady_clock::now() > _expireTime;
}

void TimerNode::triggerCallback()
{
    if (_callback && !_deleted.load()) {
        _callback();
    }
}

TimerCallback TimerNode::getCallback() const
{
    return _callback;
}

void TimerNode::markDeleted()
{
    _deleted.store(true);
}

bool TimerNode::isDeleted() const
{
    return _deleted.load();
}

bool TimerNodeCompare::operator()(const std::shared_ptr<TimerNode>& a,
                                  const std::shared_ptr<TimerNode>& b) const
{
    return a->getExpireTime() < b->getExpireTime();
}
