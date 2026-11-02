#ifndef REDIS_COMMAND_HANDLER_H
#define REDIS_COMMAND_HANDLER_H

#include <string>
#include <vector>

class RedisCommandHandler{
public:
    using Args = std::vector<std::string>;

    enum class ParseStatus { COMPLETE, INCOMPLETE, ERROR };

    RedisCommandHandler();

    // RESP parser: tries to read ONE command from the front of `buffer`.
    //  COMPLETE   -> command removed from buffer and stored in args (args is empty for blank lines)
    //  INCOMPLETE -> not enough bytes yet, buffer untouched
    //  ERROR      -> malformed input, `error` says why (caller should close the connection)
    // Understands RESP arrays of bulk strings (redis-cli, client libraries) and inline commands (nc/telnet).
    ParseStatus parseCommand(std::string& buffer, Args& args, std::string& error);

    // Execute a parsed command and return the RESP-formatted reply.
    // Sets closeConnection when the client sent QUIT.
    std::string processCommand(const Args& args, bool& closeConnection);
};

#endif
