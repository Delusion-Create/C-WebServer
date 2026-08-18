#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <queue>

// 日志级别: 数值越大越重要
enum LogLevel {
    DEBUG = 0,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

// 异步日志系统(单例):
// 生产线程只把日志写入前台缓冲, 后台线程定时交换缓冲并批量落盘,
// 将"打日志"从请求处理的关键路径上剥离, 避免磁盘 IO 阻塞业务线程
class AsyncLogger {
public:
    static AsyncLogger* getInstance();

    void init(const std::string& filepath, LogLevel level = INFO, size_t maxQueueSize = 10000);
    void stop();

    // level 低于阈值(如 DEBUG 低于 INFO)时直接丢弃, 减少无效开销
    void log(LogLevel level, const std::string& message, const char* file, int line);

    void setLevel(LogLevel level) { _level.store(static_cast<int>(level)); }
    LogLevel getLevel() const { return static_cast<LogLevel>(_level.load()); }

    ~AsyncLogger();

private:
    AsyncLogger();
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void writeThreadFunc();
    void writeToFile(const std::string& logMsg);
    std::string getCurrentTimeMicros();
    static std::string levelName(LogLevel level);

    std::string _filepath;
    std::atomic<int> _level;                 // 当前日志阈值
    std::atomic<bool> _running;

    std::queue<std::string> _frontBuffer;    // 生产线程写入
    std::queue<std::string> _backBuffer;     // 后台线程批量写出

    std::mutex _mutex;
    std::condition_variable _cv;
    std::thread _writeThread;

    size_t _maxQueueSize;                    // 前台缓冲上限, 超出丢弃最旧日志
    std::ofstream _fileStream;

    static AsyncLogger* _instance;
    static std::mutex _instanceMutex;
};

#define LOG(level, message) do { \
    AsyncLogger::getInstance()->log(level, message, __FILE__, __LINE__); \
} while (0)

#endif
