#ifndef REDIS_DATABASE_H
#define REDIS_DATABASE_H

#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Thrown when a command is run against a key holding a different data type.
struct WrongTypeError : std::runtime_error{
    WrongTypeError() : std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value"){}
};

// Thrown when INCR/DECR is used on a value that is not a valid 64-bit integer.
struct NotIntegerError : std::runtime_error{
    NotIntegerError() : std::runtime_error("ERR value is not an integer or out of range"){}
};

// Thread-safe in-memory store (Singleton). Every public method takes the mutex.
class RedisDatabase{
public:
    static RedisDatabase& getInstance();
    RedisDatabase(const RedisDatabase&) = delete;
    RedisDatabase& operator=(const RedisDatabase&) = delete;

    // ---- generic key operations ----
    size_t del(const std::vector<std::string>& keys);
    size_t exists(const std::vector<std::string>& keys);
    std::string type(const std::string& key);                  // "string", "list", "hash", "none"
    std::vector<std::string> keys(const std::string& pattern); // glob: * and ?
    bool expire(const std::string& key, long long milliseconds);
    long long ttl(const std::string& key);                     // ms left; -2 = no key, -1 = no expiry
    bool persist(const std::string& key);
    size_t size();
    void flushAll();

    // ---- strings ----
    bool set(const std::string& key, const std::string& value, long long ttlMs, bool nx, bool xx);
    bool get(const std::string& key, std::string& value);
    long long incrBy(const std::string& key, long long delta);
    size_t append(const std::string& key, const std::string& value);
    size_t strlen(const std::string& key);

    // ---- lists ----
    size_t push(const std::string& key, const std::vector<std::string>& values, bool left);
    bool pop(const std::string& key, bool left, std::string& out);
    size_t llen(const std::string& key);
    std::vector<std::string> lrange(const std::string& key, long long start, long long stop);
    bool lindex(const std::string& key, long long index, std::string& out);

    // ---- hashes ----
    size_t hset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields);
    bool hget(const std::string& key, const std::string& field, std::string& out);
    size_t hdel(const std::string& key, const std::vector<std::string>& fields);
    std::vector<std::string> hgetall(const std::string& key);  // flattened: f1, v1, f2, v2...
    size_t hlen(const std::string& key);
    bool hexists(const std::string& key, const std::string& field);

    // ---- persistence (file I/O) ----
    void setDumpFile(const std::string& path);
    bool save();   // atomic: writes <file>.tmp then renames
    bool load();   // false if the file is missing or corrupt (database left untouched)

private:
    RedisDatabase() = default;

    enum class ValueType { String, List, Hash };
    struct Entry{
        ValueType type = ValueType::String;
        std::string str;
        std::deque<std::string> list;
        std::unordered_map<std::string, std::string> hash;
    };

    // helpers below expect mtx to be held
    static long long nowMs();
    Entry* find(const std::string& key);                          // handles lazy expiry
    Entry* findTyped(const std::string& key, ValueType expected); // throws WrongTypeError
    Entry& createEntry(const std::string& key, ValueType type);
    void erase(const std::string& key);

    std::unordered_map<std::string, Entry> store;
    std::unordered_map<std::string, long long> expiries; // key -> unix time in ms
    std::string dumpFile = "dump.my_rdb";
    std::mutex mtx;
};

#endif
