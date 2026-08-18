#include "RateLimiter.h"
#include <algorithm>

RateLimiter& RateLimiter::instance()
{
    static RateLimiter limiter;
    return limiter;
}

RateLimiter::RateLimiter(size_t capacity, double refillPerSec)
    : _capacity(capacity), _refillPerSec(refillPerSec)
{
}

bool RateLimiter::allow(const std::string& key)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto now = std::chrono::steady_clock::now();

    auto it = _buckets.find(key);
    if (it == _buckets.end()) {
        // 新 IP: 桶满(允许一次突发)
        _buckets[key] = Bucket{static_cast<double>(_capacity) - 1.0, now};
        if (_buckets.size() > 10000) cleanup();
        return true;
    }

    Bucket& b = it->second;
    double elapsed = std::chrono::duration<double>(now - b.lastRefill).count();
    b.tokens = std::min(static_cast<double>(_capacity), b.tokens + elapsed * _refillPerSec);
    b.lastRefill = now;

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    return false;
}

void RateLimiter::cleanup()
{
    auto now = std::chrono::steady_clock::now();
    for (auto it = _buckets.begin(); it != _buckets.end();) {
        if (now - it->second.lastRefill > std::chrono::minutes(10)) {
            it = _buckets.erase(it);
        } else {
            ++it;
        }
    }
}
