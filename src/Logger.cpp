#include "Logger.h"

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <utility>

AsyncLogger* AsyncLogger::_instance = nullptr;
std::mutex AsyncLogger::_instanceMutex;

AsyncLogger* AsyncLogger::getInstance()
{
    // 双重检查锁定: 兼顾线程安全与单例获取性能
    if (_instance == nullptr) {
        std::lock_guard<std::mutex> lock(_instanceMutex);
        if (_instance == nullptr) {
            _instance = new AsyncLogger();
        }
    }
    return _instance;
}

AsyncLogger::AsyncLogger()
    : _level(static_cast<int>(INFO)), _running(false), _maxQueueSize(10000)
{
}

AsyncLogger::~AsyncLogger()
{
    stop();
}

void AsyncLogger::init(const std::string& filepath, LogLevel level, size_t maxQueueSize)
{
    _filepath = filepath;
    _level.store(static_cast<int>(level));
    _maxQueueSize = maxQueueSize;

    _fileStream.open(_filepath, std::ios::out | std::ios::app);
    if (!_fileStream.is_open()) {
        std::cerr << "Failed to open log file: " << _filepath << std::endl;
        return;
    }

    _running = true;
    _writeThread = std::thread(&AsyncLogger::writeThreadFunc, this);
}

void AsyncLogger::stop()
{
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) {
        return; // 已停止
    }

    _cv.notify_all();
    if (_writeThread.joinable()) {
        _writeThread.join();
    }

    // 兜底: 把缓冲区中剩余的日志写盘
    std::lock_guard<std::mutex> lock(_mutex);
    while (!_frontBuffer.empty()) {
        writeToFile(_frontBuffer.front());
        _frontBuffer.pop();
    }
    while (!_backBuffer.empty()) {
        writeToFile(_backBuffer.front());
        _backBuffer.pop();
    }
    if (_fileStream.is_open()) {
        _fileStream.close();
    }
}

std::string AsyncLogger::getCurrentTimeMicros()
{
    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);
    auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                     now.time_since_epoch()) %
                 1000000;

    std::tm* nowTm = std::localtime(&nowTime);
    std::ostringstream oss;
    oss << std::put_time(nowTm, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setfill('0') << std::setw(6) << nowUs.count();
    return oss.str();
}

std::string AsyncLogger::levelName(LogLevel level)
{
    switch (level) {
        case DEBUG:   return "DEBUG";
        case INFO:    return "INFO";
        case WARNING: return "WARNING";
        case ERROR:   return "ERROR";
        case FATAL:   return "FATAL";
    }
    return "UNKNOWN";
}

void AsyncLogger::log(LogLevel level, const std::string& message, const char* file, int line)
{
    // 等级过滤: 低于阈值的日志直接丢弃
    if (static_cast<int>(level) < _level.load()) return;
    if (!_running.load()) return;

    std::ostringstream oss;
    oss << "[" << levelName(level) << "][" << getCurrentTimeMicros() << "]["
        << message << "][" << file << ":" << line << "]";
    std::string logMsg = oss.str();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_frontBuffer.size() >= _maxQueueSize) {
            _frontBuffer.pop(); // 队列满时丢弃最旧日志, 保证不阻塞业务线程
        }
        _frontBuffer.push(std::move(logMsg));
    }
    _cv.notify_one();
}

void AsyncLogger::writeThreadFunc()
{
    std::unique_lock<std::mutex> lock(_mutex);
    while (_running.load()) {
        _cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return !_running.load() || !_frontBuffer.empty();
        });

        if (!_running.load() && _frontBuffer.empty()) break;

        // 交换前后台缓冲: 拿到一批日志后解锁落盘, 减少持锁时间
        std::swap(_frontBuffer, _backBuffer);
        lock.unlock();

        while (!_backBuffer.empty()) {
            writeToFile(_backBuffer.front());
            _backBuffer.pop();
        }
        if (_fileStream.is_open()) {
            _fileStream.flush();
        }

        lock.lock();
    }
}

void AsyncLogger::writeToFile(const std::string& logMsg)
{
    if (_fileStream.is_open()) {
        _fileStream << logMsg << '\n';
    }
}
