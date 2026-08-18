#ifndef _RATELIMITER_H_
#define _RATELIMITER_H_

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>

// 基于令牌桶算法的 IP 限流器(单例):
// - 桶容量 = 允许的瞬时突发请求数
// - 令牌按固定速率补充(refillPerSec)
// - 有令牌则放行并消耗一个, 否则拒绝(429)
class RateLimiter {
public:
    static RateLimiter& instance();

    // key(客户端 IP)是否允许通过
    bool allow(const std::string& key);

private:
    RateLimiter(size_t capacity = 1000, double refillPerSec = 500);
    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    void cleanup();

    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point lastRefill;
    };

    std::unordered_map<std::string, Bucket> _buckets;
    std::mutex _mutex;
    size_t _capacity;
    double _refillPerSec;
};

#endif
