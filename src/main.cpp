#include "../include/RedisServer.h"
#include "../include/RedisDatabase.h"

#include <iostream>
#include <string>
#include <unistd.h>

// Usage: ./my_redis_server [port] [dump_file] [save_interval_seconds]
//   defaults:              6379   dump.my_rdb   300 (0 disables the periodic save)
int main(int argc, char* argv[]){
    int port = 6379;
    std::string dumpFile = "dump.my_rdb";
    int saveInterval = 300;   // background persistence: dump the database every 5 * 60 seconds

    try{
        if(argc >= 2) port = std::stoi(argv[1]);
        if(argc >= 3) dumpFile = argv[2];
        if(argc >= 4) saveInterval = std::stoi(argv[3]);
    }catch(const std::exception&){
        std::cerr << "Usage: " << argv[0] << " [port] [dump_file] [save_interval_seconds]\n";
        return 1;
    }
    if(port < 1 || port > 65535){
        std::cerr << "Port must be between 1 and 65535\n";
        return 1;
    }

    RedisDatabase& db = RedisDatabase::getInstance();
    db.setDumpFile(dumpFile);
    if(access(dumpFile.c_str(), F_OK) == 0){
        if(db.load()) std::cout << "Database loaded from " << dumpFile << " (" << db.size() << " keys)\n";
        else std::cerr << "Warning: could not read " << dumpFile << ", starting with an empty database\n";
    }else{
        std::cout << "No dump file found, starting with an empty database\n";
    }

    RedisServer server(port, saveInterval);
    return server.run() ? 0 : 1;
}
