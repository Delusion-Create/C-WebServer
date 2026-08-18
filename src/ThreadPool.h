#ifndef _THREADPOOL_H_
#define _THREADPOOL_H_

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

// 固定大小线程池:
// 主线程只负责事件分发, 耗时的请求处理(读写/解析/响应)提交到线程池执行
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    void startThreadPool();
    void addTask(std::function<void()> task);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex lock_task;
    std::condition_variable cv;
    bool stop;
    size_t _num_threads;
};

#endif
