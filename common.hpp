#ifndef COMMON_HPP
#define COMMON_HPP

#include <string>
#include <chrono>

// Ports and sizes
static const int TCP_PORT = 9090;   // server TCP port
static const int UDP_PORT = 9091;   // server UDP port (heartbeats & broadcasts)
static const int BUFFER_SIZE = 8192;

// Heartbeat settings
static const int HEARTBEAT_INTERVAL = 10; // client sends heartbeat every 10s
static const int MAX_MISSED_HEARTBEATS = 3; // mark offline after missing 3 heartbeats

// Helper: current timestamp string
inline std::string now_str() {
    using namespace std::chrono;
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[64];
    std::strftime(buf, sizeof(buf), "%F %T", std::localtime(&t));
    return std::string(buf);
}

// Message structure
struct Message {
    std::string fromCampus;
    std::string fromDept;
    std::string toCampus;
    std::string toDept;
    std::string content;
    bool read = false; // true if read by client
};

// Campus information for heartbeat monitoring
struct CampusInfo {
    std::string name;
    time_t lastHeartbeat = 0; // last received heartbeat timestamp
    int missedCount = 0;      // missed heartbeat counter
    bool online = false;       // online/offline status
};

#endif // COMMON_HPP

