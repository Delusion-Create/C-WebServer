#ifndef _KVSTORE_H_
#define _KVSTORE_H_

#include <string>
#include <unordered_map>
#include <mutex>

// 内存键值存储(单例), 作为业务层示例:
// 互斥锁保护哈希表, 读写均为 O(1)
class KVStore {
public:
    static KVStore& instance();

    bool get(const std::string& key, std::string& value) const;
    void put(const std::string& key, const std::string& value);
    bool del(const std::string& key);
    size_t size() const;
    std::string dump() const; // 所有键值对, 格式 "key=value\n"

private:
    KVStore() {}
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::string> _data;
};

#endif
