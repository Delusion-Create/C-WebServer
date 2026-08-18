#include "KVStore.h"

KVStore& KVStore::instance()
{
    static KVStore store;
    return store;
}

bool KVStore::get(const std::string& key, std::string& value) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _data.find(key);
    if (it == _data.end()) return false;
    value = it->second;
    return true;
}

void KVStore::put(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _data[key] = value;
}

bool KVStore::del(const std::string& key)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.erase(key) > 0;
}

size_t KVStore::size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.size();
}

std::string KVStore::dump() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string out;
    for (const auto& kv : _data) {
        out += kv.first;
        out += "=";
        out += kv.second;
        out += "\n";
    }
    return out;
}
