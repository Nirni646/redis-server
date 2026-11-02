#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>
#include <unordered_map>

using Args = RedisCommandHandler::Args;

namespace {

// ---------------------------------------------------------------- RESP reply builders

std::string simpleString(const std::string& s){ return "+" + s + "\r\n"; }
std::string errorReply(const std::string& s){ return "-" + s + "\r\n"; }
std::string integerReply(long long n){ return ":" + std::to_string(n) + "\r\n"; }
std::string bulkString(const std::string& s){ return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n"; }
std::string nullBulk(){ return "$-1\r\n"; }

std::string arrayReply(const std::vector<std::string>& items){
    std::string out = "*" + std::to_string(items.size()) + "\r\n";
    for(const auto& item : items) out += bulkString(item);
    return out;
}

// ---------------------------------------------------------------- small helpers

// Strict base-10 parse: no spaces, no trailing junk, no overflow.
bool parseLL(const std::string& s, long long& out){
    if(s.empty() || std::isspace(static_cast<unsigned char>(s[0]))) return false;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if(errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

std::string toUpper(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string toLower(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

const std::string NOT_INTEGER_ERR = "ERR value is not an integer or out of range";

// Converts a user supplied amount of time units to milliseconds without overflowing.
bool toMillis(long long amount, long long unitMs, long long& out){
    if(amount <= 0) return false;
    if(amount > LLONG_MAX / unitMs) return false;
    out = amount * unitMs;
    return true;
}

// ---------------------------------------------------------------- commands
// Every command is: std::string fn(const Args& args). args[0] is the command name.
// Arity (including the command name) is checked before the function is called.

std::string cmdPing(const Args& a){
    if(a.size() == 1) return simpleString("PONG");
    if(a.size() == 2) return bulkString(a[1]);
    return errorReply("ERR wrong number of arguments for 'ping' command");
}
std::string cmdEcho(const Args& a){ return bulkString(a[1]); }
std::string cmdCommand(const Args&){ return "*0\r\n"; }   // redis-cli asks for this on connect

// ---- keys
std::string cmdDel(const Args& a){
    return integerReply(static_cast<long long>(RedisDatabase::getInstance().del(Args(a.begin() + 1, a.end()))));
}
std::string cmdExists(const Args& a){
    return integerReply(static_cast<long long>(RedisDatabase::getInstance().exists(Args(a.begin() + 1, a.end()))));
}
std::string cmdKeys(const Args& a){ return arrayReply(RedisDatabase::getInstance().keys(a[1])); }
std::string cmdType(const Args& a){ return simpleString(RedisDatabase::getInstance().type(a[1])); }

std::string expireWith(const Args& a, long long unitMs){
    long long amount = 0, ms = 0;
    if(!parseLL(a[2], amount)) return errorReply(NOT_INTEGER_ERR);
    if(amount <= 0){
        // Redis deletes the key immediately for a non-positive timeout
        return integerReply(RedisDatabase::getInstance().expire(a[1], 0) ? 1 : 0);
    }
    if(!toMillis(amount, unitMs, ms)) return errorReply("ERR invalid expire time in '" + toLower(a[0]) + "' command");
    return integerReply(RedisDatabase::getInstance().expire(a[1], ms) ? 1 : 0);
}
std::string cmdExpire(const Args& a){ return expireWith(a, 1000); }
std::string cmdPExpire(const Args& a){ return expireWith(a, 1); }

std::string cmdTtl(const Args& a){
    long long ms = RedisDatabase::getInstance().ttl(a[1]);
    return integerReply(ms < 0 ? ms : (ms + 500) / 1000);
}
std::string cmdPTtl(const Args& a){ return integerReply(RedisDatabase::getInstance().ttl(a[1])); }
std::string cmdPersist(const Args& a){ return integerReply(RedisDatabase::getInstance().persist(a[1]) ? 1 : 0); }
std::string cmdDbSize(const Args&){ return integerReply(static_cast<long long>(RedisDatabase::getInstance().size())); }
std::string cmdFlushAll(const Args&){ RedisDatabase::getInstance().flushAll(); return simpleString("OK"); }
std::string cmdSave(const Args&){
    return RedisDatabase::getInstance().save() ? simpleString("OK") : errorReply("ERR could not write dump file");
}

// ---- strings
std::string cmdSet(const Args& a){
    bool nx = false, xx = false, haveTtl = false;
    long long ttlMs = -1;
    for(size_t i = 3; i < a.size(); ++i){
        const std::string opt = toUpper(a[i]);
        if(opt == "NX") nx = true;
        else if(opt == "XX") xx = true;
        else if((opt == "EX" || opt == "PX") && i + 1 < a.size() && !haveTtl){
            long long amount = 0;
            if(!parseLL(a[++i], amount)) return errorReply(NOT_INTEGER_ERR);
            if(!toMillis(amount, opt == "EX" ? 1000 : 1, ttlMs)) return errorReply("ERR invalid expire time in 'set' command");
            haveTtl = true;
        }
        else return errorReply("ERR syntax error");
    }
    if(nx && xx) return errorReply("ERR syntax error");
    return RedisDatabase::getInstance().set(a[1], a[2], ttlMs, nx, xx) ? simpleString("OK") : nullBulk();
}

std::string cmdGet(const Args& a){
    std::string value;
    return RedisDatabase::getInstance().get(a[1], value) ? bulkString(value) : nullBulk();
}

std::string cmdMSet(const Args& a){
    if(a.size() % 2 == 0) return errorReply("ERR wrong number of arguments for 'mset' command");
    for(size_t i = 1; i + 1 < a.size(); i += 2) RedisDatabase::getInstance().set(a[i], a[i + 1], -1, false, false);
    return simpleString("OK");
}

std::string cmdMGet(const Args& a){
    std::string out = "*" + std::to_string(a.size() - 1) + "\r\n";
    for(size_t i = 1; i < a.size(); ++i){
        std::string value;
        try{
            out += RedisDatabase::getInstance().get(a[i], value) ? bulkString(value) : nullBulk();
        }catch(const WrongTypeError&){
            out += nullBulk();   // MGET returns nil for keys that are not strings
        }
    }
    return out;
}

std::string cmdIncr(const Args& a){ return integerReply(RedisDatabase::getInstance().incrBy(a[1], 1)); }
std::string cmdDecr(const Args& a){ return integerReply(RedisDatabase::getInstance().incrBy(a[1], -1)); }
std::string cmdIncrBy(const Args& a){
    long long delta = 0;
    if(!parseLL(a[2], delta)) return errorReply(NOT_INTEGER_ERR);
    return integerReply(RedisDatabase::getInstance().incrBy(a[1], delta));
}
std::string cmdDecrBy(const Args& a){
    long long delta = 0;
    if(!parseLL(a[2], delta) || delta == LLONG_MIN) return errorReply(NOT_INTEGER_ERR);
    return integerReply(RedisDatabase::getInstance().incrBy(a[1], -delta));
}
std::string cmdAppend(const Args& a){ return integerReply(static_cast<long long>(RedisDatabase::getInstance().append(a[1], a[2]))); }
std::string cmdStrlen(const Args& a){ return integerReply(static_cast<long long>(RedisDatabase::getInstance().strlen(a[1]))); }

// ---- lists
std::string pushWith(const Args& a, bool left){
    return integerReply(static_cast<long long>(RedisDatabase::getInstance().push(a[1], Args(a.begin() + 2, a.end()), left)));
}
std::string cmdLPush(const Args& a){ return pushWith(a, true); }
std::string cmdRPush(const Args& a){ return pushWith(a, false); }

std::string popWith(const Args& a, bool left){
    std::string value;
    return RedisDatabase::getInstance().pop(a[1], left, value) ? bulkString(value) : nullBulk();
}
std::string cmdLPop(const Args& a){ return popWith(a, true); }
std::string cmdRPop(const Args& a){ return popWith(a, false); }

std::string cmdLLen(const Args& a){ return integerReply(static_cast<long long>(RedisDatabase::getInstance().llen(a[1]))); }

std::string cmdLRange(const Args& a){
    long long start = 0, stop = 0;
    if(!parseLL(a[2], start) || !parseLL(a[3], stop)) return errorReply(NOT_INTEGER_ERR);
    return arrayReply(RedisDatabase::getInstance().lrange(a[1], start, stop));
}

std::string cmdLIndex(const Args& a){
    long long index = 0;
    if(!parseLL(a[2], index)) return errorReply(NOT_INTEGER_ERR);
    std::string value;
    return RedisDatabase::getInstance().lindex(a[1], index, value) ? bulkString(value) : nullBulk();
}

// ---- hashes
std::string cmdHSet(const Args& a){
    if(a.size() % 2 != 0) return errorReply("ERR wrong number of arguments for 'hset' command");
    std::vector<std::pair<std::string, std::string>> fields;
    for(size_t i = 2; i + 1 < a.size(); i += 2) fields.emplace_back(a[i], a[i + 1]);
    return integerReply(static_cast<long long>(RedisDatabase::getInstance().hset(a[1], fields)));
}
std::string cmdHGet(const Args& a){
    std::string value;
    return RedisDatabase::getInstance().hget(a[1], a[2], value) ? bulkString(value) : nullBulk();
}
std::string cmdHDel(const Args& a){
    return integerReply(static_cast<long long>(RedisDatabase::getInstance().hdel(a[1], Args(a.begin() + 2, a.end()))));
}
std::string cmdHGetAll(const Args& a){ return arrayReply(RedisDatabase::getInstance().hgetall(a[1])); }
std::string cmdHLen(const Args& a){ return integerReply(static_cast<long long>(RedisDatabase::getInstance().hlen(a[1]))); }
std::string cmdHExists(const Args& a){ return integerReply(RedisDatabase::getInstance().hexists(a[1], a[2]) ? 1 : 0); }

// ---------------------------------------------------------------- command table
// To add a command: write cmdXxx above and add ONE line here.
// arity > 0 means exactly that many args (including the name); arity < 0 means "at least -arity".

using CommandFn = std::string (*)(const Args&);
struct CommandSpec{
    CommandFn fn;
    int arity;
};

const std::unordered_map<std::string, CommandSpec>& commandTable(){
    static const std::unordered_map<std::string, CommandSpec> table = {
        {"PING",     {cmdPing,    -1}}, {"ECHO",    {cmdEcho,    2}}, {"COMMAND", {cmdCommand, -1}},
        {"DEL",      {cmdDel,     -2}}, {"EXISTS",  {cmdExists, -2}}, {"KEYS",    {cmdKeys,    2}},
        {"TYPE",     {cmdType,     2}}, {"EXPIRE",  {cmdExpire,  3}}, {"PEXPIRE", {cmdPExpire, 3}},
        {"TTL",      {cmdTtl,      2}}, {"PTTL",    {cmdPTtl,    2}}, {"PERSIST", {cmdPersist, 2}},
        {"DBSIZE",   {cmdDbSize,   1}}, {"FLUSHALL",{cmdFlushAll,-1}}, {"FLUSHDB", {cmdFlushAll,-1}},
        {"SAVE",     {cmdSave,     1}},
        {"SET",      {cmdSet,     -3}}, {"GET",     {cmdGet,     2}}, {"MSET",    {cmdMSet,   -3}},
        {"MGET",     {cmdMGet,    -2}}, {"INCR",    {cmdIncr,    2}}, {"DECR",    {cmdDecr,    2}},
        {"INCRBY",   {cmdIncrBy,   3}}, {"DECRBY",  {cmdDecrBy,  3}}, {"APPEND",  {cmdAppend,  3}},
        {"STRLEN",   {cmdStrlen,   2}},
        {"LPUSH",    {cmdLPush,   -3}}, {"RPUSH",   {cmdRPush,  -3}}, {"LPOP",    {cmdLPop,    2}},
        {"RPOP",     {cmdRPop,     2}}, {"LLEN",    {cmdLLen,    2}}, {"LRANGE", {cmdLRange,   4}},
        {"LINDEX",   {cmdLIndex,   3}},
        {"HSET",     {cmdHSet,    -4}}, {"HGET",    {cmdHGet,    3}}, {"HDEL",    {cmdHDel,   -3}},
        {"HGETALL",  {cmdHGetAll,  2}}, {"HLEN",    {cmdHLen,    2}}, {"HEXISTS", {cmdHExists, 3}},
    };
    return table;
}

// Limits protect the server from clients that announce absurd sizes.
const size_t MAX_LINE_LENGTH = 64 * 1024;          // header lines and inline commands
const long long MAX_ARRAY_LENGTH = 1024 * 1024;    // arguments per command
const long long MAX_BULK_LENGTH = 64LL * 1024 * 1024;

} // namespace

// ==================================================================== class

RedisCommandHandler::RedisCommandHandler(){}

RedisCommandHandler::ParseStatus RedisCommandHandler::parseCommand(std::string& buffer, Args& args, std::string& error){
    args.clear();
    if(buffer.empty()) return ParseStatus::INCOMPLETE;

    // ---- inline command ("PING\r\n", "SET a 1\r\n") typed into nc / telnet
    if(buffer[0] != '*'){
        size_t nl = buffer.find('\n');
        if(nl == std::string::npos){
            if(buffer.size() > MAX_LINE_LENGTH){
                error = "too big inline request";
                return ParseStatus::ERROR;
            }
            return ParseStatus::INCOMPLETE;
        }
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if(!line.empty() && line.back() == '\r') line.pop_back();

        size_t i = 0;
        while(i < line.size()){
            while(i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            size_t start = i;
            while(i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            if(i > start) args.push_back(line.substr(start, i - start));
        }
        return ParseStatus::COMPLETE;
    }

    // ---- RESP array: *<count>\r\n  then <count> x ( $<len>\r\n<bytes>\r\n )
    size_t pos = 0;
    size_t eol = buffer.find("\r\n", pos);
    if(eol == std::string::npos){
        if(buffer.size() > MAX_LINE_LENGTH){
            error = "too big mbulk count string";
            return ParseStatus::ERROR;
        }
        return ParseStatus::INCOMPLETE;
    }

    long long count = 0;
    if(!parseLL(buffer.substr(1, eol - 1), count) || count > MAX_ARRAY_LENGTH){
        error = "invalid multibulk length";
        return ParseStatus::ERROR;
    }
    pos = eol + 2;
    if(count <= 0){                      // empty or null array: nothing to run
        buffer.erase(0, pos);
        return ParseStatus::COMPLETE;
    }

    Args parsed;
    parsed.reserve(static_cast<size_t>(count));
    for(long long i = 0; i < count; ++i){
        if(pos >= buffer.size()) return ParseStatus::INCOMPLETE;
        if(buffer[pos] != '$'){
            error = std::string("expected '$', got '") + buffer[pos] + "'";
            return ParseStatus::ERROR;
        }
        eol = buffer.find("\r\n", pos);
        if(eol == std::string::npos){
            if(buffer.size() - pos > MAX_LINE_LENGTH){
                error = "too big bulk count string";
                return ParseStatus::ERROR;
            }
            return ParseStatus::INCOMPLETE;
        }
        long long len = 0;
        if(!parseLL(buffer.substr(pos + 1, eol - pos - 1), len) || len < 0 || len > MAX_BULK_LENGTH){
            error = "invalid bulk length";
            return ParseStatus::ERROR;
        }
        pos = eol + 2;
        const size_t need = static_cast<size_t>(len) + 2;
        if(buffer.size() < pos + need) return ParseStatus::INCOMPLETE;
        if(buffer[pos + len] != '\r' || buffer[pos + len + 1] != '\n'){
            error = "bulk string is not terminated by CRLF";
            return ParseStatus::ERROR;
        }
        parsed.emplace_back(buffer, pos, static_cast<size_t>(len));
        pos += need;
    }

    args = std::move(parsed);
    buffer.erase(0, pos);
    return ParseStatus::COMPLETE;
}

std::string RedisCommandHandler::processCommand(const Args& args, bool& closeConnection){
    if(args.empty()) return "";

    const std::string name = toUpper(args[0]);
    if(name == "QUIT"){
        closeConnection = true;
        return simpleString("OK");
    }

    const auto& table = commandTable();
    auto it = table.find(name);
    if(it == table.end()){
        return errorReply("ERR unknown command '" + args[0] + "'");
    }

    const int argc = static_cast<int>(args.size());
    const int arity = it->second.arity;
    if((arity > 0 && argc != arity) || (arity < 0 && argc < -arity)){
        return errorReply("ERR wrong number of arguments for '" + toLower(name) + "' command");
    }

    // The command name is normalised so handlers never have to care about case.
    Args normalised = args;
    normalised[0] = name;
    try{
        return it->second.fn(normalised);
    }catch(const std::runtime_error& e){      // WrongTypeError / NotIntegerError carry full RESP error text
        return errorReply(e.what());
    }catch(const std::exception& e){
        return errorReply(std::string("ERR ") + e.what());
    }
}
