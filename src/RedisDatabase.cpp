#include "../include/RedisDatabase.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace {

// Glob matching supporting '*' and '?' only.
bool globMatch(const std::string& pattern, const std::string& text){
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while(t < text.size()){
        if(p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])){
            ++p; ++t;
        }else if(p < pattern.size() && pattern[p] == '*'){
            star = p++;
            mark = t;
        }else if(star != std::string::npos){
            p = star + 1;
            t = ++mark;
        }else{
            return false;
        }
    }
    while(p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool parseInteger(const std::string& s, long long& out){
    if(s.empty() || s.size() > 20) return false;
    try{
        size_t pos = 0;
        out = std::stoll(s, &pos);
        return pos == s.size();
    }catch(...){
        return false;
    }
}

// Length-prefixed strings keep the dump file binary-safe: "<len>:<bytes>\n"
void writeStr(std::ostream& os, const std::string& s){
    os << s.size() << ':' << s << '\n';
}

bool readStr(std::istream& is, std::string& s){
    size_t n = 0;
    char c = 0;
    if(!(is >> n)) return false;
    if(!is.get(c) || c != ':') return false;
    if(n > (1ULL << 30)) return false;
    s.assign(n, '\0');
    if(n > 0 && !is.read(&s[0], static_cast<std::streamsize>(n))) return false;
    return true;
}

const char* DUMP_MAGIC = "MYREDIS1";

} // namespace

RedisDatabase& RedisDatabase::getInstance(){
    static RedisDatabase instance;
    return instance;
}

long long RedisDatabase::nowMs(){
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ helpers

RedisDatabase::Entry* RedisDatabase::find(const std::string& key){
    auto exp = expiries.find(key);
    if(exp != expiries.end() && exp->second <= nowMs()){
        erase(key);
        return nullptr;
    }
    auto it = store.find(key);
    return it == store.end() ? nullptr : &it->second;
}

RedisDatabase::Entry* RedisDatabase::findTyped(const std::string& key, ValueType expected){
    Entry* e = find(key);
    if(e && e->type != expected) throw WrongTypeError();
    return e;
}

RedisDatabase::Entry& RedisDatabase::createEntry(const std::string& key, ValueType type){
    Entry& e = store[key];
    e = Entry();
    e.type = type;
    expiries.erase(key);
    return e;
}

void RedisDatabase::erase(const std::string& key){
    store.erase(key);
    expiries.erase(key);
}

// ------------------------------------------------------------------ generic

size_t RedisDatabase::del(const std::vector<std::string>& keysToDelete){
    std::lock_guard<std::mutex> lock(mtx);
    size_t removed = 0;
    for(const auto& k : keysToDelete){
        if(find(k)){
            erase(k);
            ++removed;
        }
    }
    return removed;
}

size_t RedisDatabase::exists(const std::vector<std::string>& keysToCheck){
    std::lock_guard<std::mutex> lock(mtx);
    size_t count = 0;
    for(const auto& k : keysToCheck){
        if(find(k)) ++count;
    }
    return count;
}

std::string RedisDatabase::type(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = find(key);
    if(!e) return "none";
    switch(e->type){
        case ValueType::String: return "string";
        case ValueType::List:   return "list";
        case ValueType::Hash:   return "hash";
    }
    return "none";
}

std::vector<std::string> RedisDatabase::keys(const std::string& pattern){
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> result, expired;
    const long long now = nowMs();
    for(const auto& kv : store){
        auto exp = expiries.find(kv.first);
        if(exp != expiries.end() && exp->second <= now){
            expired.push_back(kv.first);
        }else if(globMatch(pattern, kv.first)){
            result.push_back(kv.first);
        }
    }
    for(const auto& k : expired) erase(k);
    return result;
}

bool RedisDatabase::expire(const std::string& key, long long milliseconds){
    std::lock_guard<std::mutex> lock(mtx);
    if(!find(key)) return false;
    if(milliseconds <= 0){
        erase(key);
    }else{
        expiries[key] = nowMs() + milliseconds;
    }
    return true;
}

long long RedisDatabase::ttl(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    if(!find(key)) return -2;
    auto exp = expiries.find(key);
    if(exp == expiries.end()) return -1;
    long long left = exp->second - nowMs();
    return left < 0 ? 0 : left;
}

bool RedisDatabase::persist(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    if(!find(key)) return false;
    return expiries.erase(key) > 0;
}

size_t RedisDatabase::size(){
    std::lock_guard<std::mutex> lock(mtx);
    size_t count = 0;
    const long long now = nowMs();
    for(const auto& kv : store){
        auto exp = expiries.find(kv.first);
        if(exp == expiries.end() || exp->second > now) ++count;
    }
    return count;
}

void RedisDatabase::flushAll(){
    std::lock_guard<std::mutex> lock(mtx);
    store.clear();
    expiries.clear();
}

// ------------------------------------------------------------------ strings

bool RedisDatabase::set(const std::string& key, const std::string& value, long long ttlMs, bool nx, bool xx){
    std::lock_guard<std::mutex> lock(mtx);
    const bool present = find(key) != nullptr;
    if(nx && present) return false;
    if(xx && !present) return false;
    Entry& e = createEntry(key, ValueType::String);
    e.str = value;
    if(ttlMs > 0) expiries[key] = nowMs() + ttlMs;
    return true;
}

bool RedisDatabase::get(const std::string& key, std::string& value){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::String);
    if(!e) return false;
    value = e->str;
    return true;
}

long long RedisDatabase::incrBy(const std::string& key, long long delta){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::String);
    long long current = 0;
    if(e && !parseInteger(e->str, current)) throw NotIntegerError();
    long long result = 0;
    if(__builtin_add_overflow(current, delta, &result)) throw NotIntegerError();
    if(!e) e = &createEntry(key, ValueType::String);
    e->str = std::to_string(result);
    return result;
}

size_t RedisDatabase::append(const std::string& key, const std::string& value){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::String);
    if(!e) e = &createEntry(key, ValueType::String);
    e->str += value;
    return e->str.size();
}

size_t RedisDatabase::strlen(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::String);
    return e ? e->str.size() : 0;
}

// ------------------------------------------------------------------ lists

size_t RedisDatabase::push(const std::string& key, const std::vector<std::string>& values, bool left){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::List);
    if(!e) e = &createEntry(key, ValueType::List);
    for(const auto& v : values){
        if(left) e->list.push_front(v);
        else     e->list.push_back(v);
    }
    return e->list.size();
}

bool RedisDatabase::pop(const std::string& key, bool left, std::string& out){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::List);
    if(!e || e->list.empty()) return false;
    if(left){
        out = std::move(e->list.front());
        e->list.pop_front();
    }else{
        out = std::move(e->list.back());
        e->list.pop_back();
    }
    if(e->list.empty()) erase(key);
    return true;
}

size_t RedisDatabase::llen(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::List);
    return e ? e->list.size() : 0;
}

std::vector<std::string> RedisDatabase::lrange(const std::string& key, long long start, long long stop){
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> result;
    Entry* e = findTyped(key, ValueType::List);
    if(!e) return result;
    const long long len = static_cast<long long>(e->list.size());
    if(start < 0) start += len;
    if(stop < 0)  stop += len;
    if(start < 0) start = 0;
    if(stop >= len) stop = len - 1;
    if(start > stop || start >= len) return result;
    for(long long i = start; i <= stop; ++i) result.push_back(e->list[static_cast<size_t>(i)]);
    return result;
}

bool RedisDatabase::lindex(const std::string& key, long long index, std::string& out){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::List);
    if(!e) return false;
    const long long len = static_cast<long long>(e->list.size());
    if(index < 0) index += len;
    if(index < 0 || index >= len) return false;
    out = e->list[static_cast<size_t>(index)];
    return true;
}

// ------------------------------------------------------------------ hashes

size_t RedisDatabase::hset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::Hash);
    if(!e) e = &createEntry(key, ValueType::Hash);
    size_t added = 0;
    for(const auto& f : fields){
        if(e->hash.find(f.first) == e->hash.end()) ++added;
        e->hash[f.first] = f.second;
    }
    return added;
}

bool RedisDatabase::hget(const std::string& key, const std::string& field, std::string& out){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::Hash);
    if(!e) return false;
    auto it = e->hash.find(field);
    if(it == e->hash.end()) return false;
    out = it->second;
    return true;
}

size_t RedisDatabase::hdel(const std::string& key, const std::vector<std::string>& fields){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::Hash);
    if(!e) return 0;
    size_t removed = 0;
    for(const auto& f : fields) removed += e->hash.erase(f);
    if(e->hash.empty()) erase(key);
    return removed;
}

std::vector<std::string> RedisDatabase::hgetall(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> result;
    Entry* e = findTyped(key, ValueType::Hash);
    if(!e) return result;
    for(const auto& kv : e->hash){
        result.push_back(kv.first);
        result.push_back(kv.second);
    }
    return result;
}

size_t RedisDatabase::hlen(const std::string& key){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::Hash);
    return e ? e->hash.size() : 0;
}

bool RedisDatabase::hexists(const std::string& key, const std::string& field){
    std::lock_guard<std::mutex> lock(mtx);
    Entry* e = findTyped(key, ValueType::Hash);
    return e && e->hash.find(field) != e->hash.end();
}

// ------------------------------------------------------------------ persistence

void RedisDatabase::setDumpFile(const std::string& path){
    std::lock_guard<std::mutex> lock(mtx);
    dumpFile = path;
}

bool RedisDatabase::save(){
    std::lock_guard<std::mutex> lock(mtx);
    const std::string tmpPath = dumpFile + ".tmp";
    const long long now = nowMs();

    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if(!out) return false;

    size_t live = 0;
    for(const auto& kv : store){
        auto exp = expiries.find(kv.first);
        if(exp == expiries.end() || exp->second > now) ++live;
    }

    out << DUMP_MAGIC << '\n' << live << '\n';
    for(const auto& kv : store){
        auto exp = expiries.find(kv.first);
        if(exp != expiries.end() && exp->second <= now) continue;
        const Entry& e = kv.second;
        const long long expiresAt = (exp == expiries.end()) ? 0 : exp->second;
        const char typeChar = e.type == ValueType::String ? 'S' : e.type == ValueType::List ? 'L' : 'H';

        out << typeChar << ' ' << expiresAt << '\n';
        writeStr(out, kv.first);
        if(e.type == ValueType::String){
            writeStr(out, e.str);
        }else if(e.type == ValueType::List){
            out << e.list.size() << '\n';
            for(const auto& item : e.list) writeStr(out, item);
        }else{
            out << e.hash.size() << '\n';
            for(const auto& f : e.hash){
                writeStr(out, f.first);
                writeStr(out, f.second);
            }
        }
    }
    out.flush();
    if(!out){
        out.close();
        std::remove(tmpPath.c_str());
        return false;
    }
    out.close();
    return std::rename(tmpPath.c_str(), dumpFile.c_str()) == 0;
}

bool RedisDatabase::load(){
    std::lock_guard<std::mutex> lock(mtx);
    std::ifstream in(dumpFile, std::ios::binary);
    if(!in) return false;

    std::string magic;
    if(!std::getline(in, magic) || magic != DUMP_MAGIC) return false;
    size_t count = 0;
    if(!(in >> count)) return false;

    std::unordered_map<std::string, Entry> newStore;
    std::unordered_map<std::string, long long> newExpiries;
    const long long now = nowMs();

    for(size_t i = 0; i < count; ++i){
        char typeChar = 0;
        long long expiresAt = 0;
        std::string key;
        if(!(in >> typeChar >> expiresAt)) return false;
        if(!readStr(in, key)) return false;

        Entry e;
        if(typeChar == 'S'){
            e.type = ValueType::String;
            if(!readStr(in, e.str)) return false;
        }else if(typeChar == 'L'){
            e.type = ValueType::List;
            size_t n = 0;
            if(!(in >> n)) return false;
            for(size_t j = 0; j < n; ++j){
                std::string item;
                if(!readStr(in, item)) return false;
                e.list.push_back(std::move(item));
            }
        }else if(typeChar == 'H'){
            e.type = ValueType::Hash;
            size_t n = 0;
            if(!(in >> n)) return false;
            for(size_t j = 0; j < n; ++j){
                std::string field, value;
                if(!readStr(in, field) || !readStr(in, value)) return false;
                e.hash[field] = std::move(value);
            }
        }else{
            return false;
        }

        if(expiresAt != 0 && expiresAt <= now) continue; // already expired while the server was down
        if(expiresAt != 0) newExpiries[key] = expiresAt;
        newStore[key] = std::move(e);
    }

    store.swap(newStore);
    expiries.swap(newExpiries);
    return true;
}
