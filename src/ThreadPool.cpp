#include "ThreadPool.h"
#include <utility>

ThreadPool::ThreadPool(size_t num_threads) : stop(false), _num_threads(num_threads) {}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(lock_task);
        stop = true;
    }
    cv.notify_all();
    for (auto& work : workers) {
        if (work.joinable()) {
            work.join();
        }
    }
}

void ThreadPool::startThreadPool()
{
    for (size_t i = 0; i < _num_threads; ++i) {
        workers.emplace_back([this]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(lock_task);
                    // 没有任务时挂起等待, 避免空转浪费 CPU
                    cv.wait(lock, [this]() { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) {
                        return; // 退出线程
                    }
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task(); // 锁外执行任务
            }
        });
    }
}

void ThreadPool::addTask(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(lock_task);
        tasks.push(std::move(task));
    }
    cv.notify_one();
}
