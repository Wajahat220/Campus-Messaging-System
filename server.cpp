// server.cpp
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"

using namespace std;

// Hard-coded credentials: campus -> password (display case preserved)
map<string, string> credentials = {
    {"Lahore", "NU-LHR-123"},
    {"Karachi", "NU-KHI-123"},
    {"Peshawar", "NU-PES-123"},
    {"CFD", "NU-CFD-123"},
    {"Multan", "NU-MUL-123"},
    {"Islamabad", "NU-ISB-123"}
};

// Map lowercase campus -> proper display name
map<string, string> campus_display_name;

// Data per connected client (each client represents a single department)
struct ClientInfo {
    int sockfd;
    string campusLower;     // lowercase campus
    string campusDisplay;   // display campus (preserve case)
    string deptLower;       // lowercase department
    string deptDisplay;     // display department (preserve case)
    sockaddr_in udpAddr;    // last known UDP address for broadcast
    bool has_udp_addr = false;
};

struct CampusStatus {
    time_t lastHeartbeat = 0;
    int missedCount = 0;
    bool online = false;
};

mutex global_mutex; // for shared access
vector<ClientInfo> clients;
map<string, int> routing_map;        // key = campusLower + "|" + deptLower -> index in clients
map<string, CampusStatus> campusStatus;  // lowercase campus -> status
vector<string> routing_log;

// Heartbeat internal storage (lowercase campus -> heartbeat info)
struct HeartbeatInfo {
    string dept; // stored as-received (preserve formatting)
    chrono::system_clock::time_point ts;
};
mutex hb_mtx;
map<string, HeartbeatInfo> heartbeats; // lowercase campus -> info

static string make_log(const string& s) {
    return "[" + now_str() + "] " + s;
}

vector<string> split_tokens(const string &s, char sep='|') {
    vector<string> out;
    string tmp;
    for (char c : s) {
        if (c == sep) { out.push_back(tmp); tmp.clear(); }
        else tmp.push_back(c);
    }
    out.push_back(tmp);
    return out;
}

string to_lower(const string &s) {
    string out = s;
    transform(out.begin(), out.end(), out.begin(),
              [](unsigned char c){ return tolower(c); });
    return out;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Called when heartbeat is received (campusLower expected)
void on_heartbeat(const string &campusLower, const string &dept) {
    lock_guard<mutex> lk(hb_mtx);
    heartbeats[campusLower] = { dept, chrono::system_clock::now() };
}

// Menu option to view heartbeats
void show_heartbeat_log() {
    system("clear"); // clear console
    cout << "---- Heartbeat Records ----\n";
    lock_guard<mutex> lk(hb_mtx);
    for (auto &p : heartbeats) {
        auto t = chrono::system_clock::to_time_t(p.second.ts);
        string display = p.first;
        if (campus_display_name.count(p.first)) display = campus_display_name[p.first];
        cout << display << " (" << p.second.dept << ") : " << ctime(&t);
    }
    cout << "---------------------------\n";
    cout << "Press Enter to return to main menu...";
    cin.ignore();
}

// send a TCP framed message (no newline expected)
void send_tcp_msg(int sockfd, const string &msg) {
    if (send(sockfd, msg.c_str(), msg.size(), 0) < 0) {
        // ignore send errors for now (client might have disconnected)
    }
}

// ---------------- Admin Menu Thread ----------------
void admin_menu(int udp_fd) {
    while (true) {
        cout << "\n--- Admin Menu ---\n";
        cout << "1) LIST          - Show connected departments & status\n";
        cout << "2) BROADCAST     - Send message to active clients (UDP)\n";
        cout << "3) LOG           - Show message routing log\n";
        cout << "4) HEARTBEAT LOG - Show heartbeat records\n";
        cout << "5) EXIT          - Shutdown server (notify clients)\n";
        cout << "Choose: ";
        string choice;
        getline(cin, choice);

        if (choice == "1") {
            lock_guard<mutex> lock(global_mutex);
            cout << "---- Connected department clients ----\n";
            for (auto &c : clients) {
                string name = (c.campusDisplay.empty() ? "(unauthenticated)" : c.campusDisplay);
                cout << "fd=" << c.sockfd << " : " << name << " / " << c.deptDisplay;
                if (c.has_udp_addr) cout << " (udp-known)";
                cout << "\n";
            }
            cout << "---- Heartbeat Status ----\n";
            for (auto &cs : campusStatus) {
                string display = cs.first;
                if (campus_display_name.count(cs.first)) display = campus_display_name[cs.first];
                cout << display << " : last HB " 
                     << cs.second.lastHeartbeat 
                     << "s ago, "
                     << (cs.second.online ? "ONLINE" : "OFFLINE") 
                     << "\n";
            }
        } else if (choice == "2") {
            cout << "Enter broadcast message: ";
            string msg;
            getline(cin, msg);
            lock_guard<mutex> lock(global_mutex);
            for (auto &ci : clients) {
                if (ci.has_udp_addr) {
                    string payload = "BCAST|" + msg;
                    ssize_t sent = sendto(udp_fd, payload.c_str(), payload.size(), 0,
                                          (sockaddr*)&ci.udpAddr, sizeof(ci.udpAddr));
                    if (sent < 0) perror("sendto");
                }
            }
            cout << make_log("Admin broadcast sent: " + msg) << endl;
        } else if (choice == "3") {
            lock_guard<mutex> lock(global_mutex);
            cout << "---- Routing Log ----\n";
            for (auto &l : routing_log) cout << l << "\n";
        } else if (choice == "4") {
            show_heartbeat_log();
        } else if (choice == "5") {
            // Notify all clients via TCP and then exit
            lock_guard<mutex> lock(global_mutex);
            string shutdown_msg = "SHUTDOWN|Server is shutting down";
            for (auto &ci : clients) {
                send_tcp_msg(ci.sockfd, shutdown_msg);
            }
            cout << make_log("Server shutting down (admin triggered). Notified clients.") << endl;
            // Give a short moment for messages to be sent
            this_thread::sleep_for(chrono::milliseconds(200));
            exit(0);
        } else {
            cout << "Invalid option.\n";
        }
    }
}

int main() {
    cout << make_log("Starting Central Server (poll-based)") << endl;

    // build lowercase -> proper case map and initialize campusStatus with lowercase keys
    for (auto &p : credentials) {
        string lc = to_lower(p.first);
        campus_display_name[lc] = p.first;
        campusStatus[lc] = CampusStatus();
    }

    // TCP socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in srvAddr{};
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(TCP_PORT);
    srvAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&srvAddr, sizeof(srvAddr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 50) < 0) { perror("listen"); return 1; }

    set_nonblocking(listen_fd);

    // UDP socket
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("udp socket"); return 1; }

    sockaddr_in udpAddr{};
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_port = htons(UDP_PORT);
    udpAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_fd, (sockaddr*)&udpAddr, sizeof(udpAddr)) < 0) { perror("udp bind"); return 1; }
    set_nonblocking(udp_fd);

    cout << make_log("TCP port: " + to_string(TCP_PORT) + ", UDP port: " + to_string(UDP_PORT)) << endl;

    // Start admin thread
    thread(admin_menu, udp_fd).detach();

    // Main poll loop
    while (true) {
        vector<pollfd> pfds;

        pfds.push_back({listen_fd, POLLIN, 0});
        pfds.push_back({udp_fd, POLLIN, 0});

        {
            lock_guard<mutex> lock(global_mutex);
            for (auto &ci : clients)
                pfds.push_back({ci.sockfd, POLLIN, 0});
        }

        int n = poll(pfds.data(), pfds.size(), 1000);
        if (n < 0) { perror("poll"); continue; }

        int idx = 0;

        // --- TCP listen ---
        if (pfds[idx].revents & POLLIN) {
            sockaddr_in cliAddr; socklen_t len = sizeof(cliAddr);
            int clientfd = accept(listen_fd, (sockaddr*)&cliAddr, &len);
            if (clientfd >= 0) {
                set_nonblocking(clientfd);
                ClientInfo ci; ci.sockfd = clientfd;
                lock_guard<mutex> lock(global_mutex);
                clients.push_back(ci);
                routing_log.push_back(make_log("Client connected fd=" + to_string(clientfd)));
                cout << make_log("New TCP client connected (fd=" + to_string(clientfd) + ")") << endl;
            }
        }
        idx++;

        // --- UDP (heartbeat) ---
        if (pfds[idx].revents & POLLIN) {
            char buf[BUFFER_SIZE]; sockaddr_in src; socklen_t sl = sizeof(src);
            ssize_t r = recvfrom(udp_fd, buf, sizeof(buf)-1, 0, (sockaddr*)&src, &sl);
            if (r > 0) {
                buf[r] = 0;
                string s(buf);
                auto toks = split_tokens(s,'|');
                if (!toks.empty() && toks[0]=="HB" && toks.size()>=2) {
                    string campusLower = to_lower(toks[1]);
                    string dept = (toks.size()>=3 ? toks[2] : "");
                    on_heartbeat(campusLower, dept);

                    lock_guard<mutex> lock(global_mutex);
                    if (campusStatus.count(campusLower)) {
                        campusStatus[campusLower].lastHeartbeat = time(nullptr);
                        campusStatus[campusLower].missedCount = 0;
                        campusStatus[campusLower].online = true;
                    }
                    // update udp addr for any clients that match campus (we don't have dept in HB reliably)
                    for (size_t i=0;i<clients.size();++i) {
                        if (clients[i].campusLower == campusLower) {
                            clients[i].udpAddr = src;
                            clients[i].has_udp_addr = true;
                        }
                    }
                }
            }
        }
        idx++;

        // --- Client sockets ---
        lock_guard<mutex> lock(global_mutex);
        for (size_t ci_idx=0; ci_idx<clients.size(); ++ci_idx, ++idx) {
            auto &ci = clients[ci_idx];
            if (!(pfds[idx].revents & POLLIN)) continue;
            char buf[BUFFER_SIZE]; ssize_t r = recv(ci.sockfd, buf, sizeof(buf)-1, 0);
            if (r <= 0) {
                cout << make_log("Client fd=" + to_string(ci.sockfd)+" disconnected") << endl;
                routing_log.push_back(make_log("fd "+to_string(ci.sockfd)+" disconnected"));
                close(ci.sockfd);
                // remove routing_map entry for this client
                if (!ci.campusLower.empty() && !ci.deptLower.empty()) {
                    string key = ci.campusLower + "|" + ci.deptLower;
                    if (routing_map.count(key)) routing_map.erase(key);
                }
                clients.erase(clients.begin()+ci_idx);
                ci_idx--;
                idx--;
                continue;
            }
            buf[r] = 0;
            string msg(buf);
            auto toks = split_tokens(msg,'|');
            if (toks.empty()) continue;

            // AUTH handling (AUTH|Campus|Dept|Pass)
            if (toks[0]=="AUTH" && toks.size()>=4) {
                string inputCamp = toks[1];
                string inputDept = toks[2];
                string pass = toks[3];
                string inputLower = to_lower(inputCamp);
                string deptLower = to_lower(inputDept);

                if (campus_display_name.count(inputLower) && credentials[campus_display_name[inputLower]] == pass) {
                    // success
                    ci.campusLower = inputLower;
                    ci.campusDisplay = campus_display_name[inputLower];
                    ci.deptLower = deptLower;
                    ci.deptDisplay = inputDept;
                    string key = ci.campusLower + "|" + ci.deptLower;
                    routing_map[key] = ci_idx;
                    string reply = "AUTH_OK";
                    if (send(ci.sockfd, reply.c_str(), reply.size(),0)<0) perror("send");
                    routing_log.push_back(make_log("AUTH " + ci.campusDisplay + " / " + ci.deptDisplay));
                    cout << make_log("Authenticated: " + ci.campusDisplay + " / " + ci.deptDisplay + " (fd="+to_string(ci.sockfd)+")") << endl;
                } else {
                    string reply = "AUTH_FAIL";
                    if (send(ci.sockfd, reply.c_str(), reply.size(),0)<0) perror("send");
                    routing_log.push_back(make_log("AUTH_FAIL fd="+to_string(ci.sockfd)));
                    cout << make_log("Authentication failed for fd=" + to_string(ci.sockfd)) << endl;
                    close(ci.sockfd);
                    clients.erase(clients.begin()+ci_idx);
                    ci_idx--;
                    idx--;
                    continue;
                }
            }
            // MSG handling: MSG|TargetCampus|TargetDept|DeptForFrom|Body
            else if (toks[0]=="MSG" && toks.size()>=4) {
                string targetRaw = toks[1];
                string targetDeptRaw = toks[2];
                string body = toks[3];

                string targetLower = to_lower(targetRaw);
                string targetDeptLower = to_lower(targetDeptRaw);

                string fromDisplay = "(Unknown)";
                string fromDeptDisplay = "";
                if (!ci.campusLower.empty()) {
                    fromDisplay = ci.campusDisplay;
                    fromDeptDisplay = ci.deptDisplay;
                }

                string forward = "FROM|" + fromDisplay + "|" + fromDeptDisplay + "|" + body;

                string key = targetLower + "|" + targetDeptLower;
                if (routing_map.count(key)) {
                    int tidx = routing_map[key];
                    int sock_target = clients[tidx].sockfd;
                    if (send(sock_target, forward.c_str(), forward.size(),0)<0) perror("send");
                    string routedMsg = "Routed " + fromDisplay + "-" + fromDeptDisplay + " -> " +
                                        (campus_display_name.count(targetLower)?campus_display_name[targetLower]:targetRaw) +
                                        "-" + targetDeptRaw + " : " + body;
                    routing_log.push_back(make_log(routedMsg));
                    cout << make_log(routedMsg) << endl;
                } else {
                    string err = "ERR|Target offline or unknown: " + targetRaw + "-" + targetDeptRaw;
                    if (send(ci.sockfd, err.c_str(), err.size(),0)<0) perror("send");
                }
            }
            // FILE handling: FILE|TargetCampus|TargetDept|Filename|Base64Content
            else if (toks[0]=="FILE" && toks.size()>=5) {
                string targetRaw = toks[1];
                string targetDeptRaw = toks[2];
                string filename = toks[3];
                // remaining tokens after 4th are part of base64 content (in case '|' inside)
                string b64;
                // re-join all remaining tokens into b64
                size_t pos = msg.find("|", 0);
                // We need to find the position of 4th '|' to extract exact b64 substring easily
                int seen = 0;
                size_t i;
                for (i = 0; i < msg.size(); ++i) {
                    if (msg[i] == '|') {
                        seen++;
                        if (seen == 4) { ++i; break; }
                    }
                }
                if (i < msg.size()) b64 = msg.substr(i);
                else if (toks.size()>=5) b64 = toks[4];

                string targetLower = to_lower(targetRaw);
                string targetDeptLower = to_lower(targetDeptRaw);
                string fromDisplay = ci.campusDisplay;
                string fromDeptDisplay = ci.deptDisplay;

                string forward = "FILEFROM|" + fromDisplay + "|" + fromDeptDisplay + "|" + filename + "|" + b64;

                string key = targetLower + "|" + targetDeptLower;
                if (routing_map.count(key)) {
                    int tidx = routing_map[key];
                    int sock_target = clients[tidx].sockfd;
                    if (send(sock_target, forward.c_str(), forward.size(),0)<0) perror("send");
                    string routedMsg = "File routed " + fromDisplay + "-" + fromDeptDisplay + " -> " +
                                        (campus_display_name.count(targetLower)?campus_display_name[targetLower]:targetRaw) +
                                        "-" + targetDeptRaw + " : " + filename;
                    routing_log.push_back(make_log(routedMsg));
                    cout << make_log(routedMsg) << endl;
                } else {
                    string err = "ERR|Target offline or unknown: " + targetRaw + "-" + targetDeptRaw;
                    if (send(ci.sockfd, err.c_str(), err.size(),0)<0) perror("send");
                }
            }
            else {
                cout << make_log("Unknown TCP payload from fd="+to_string(ci.sockfd)+" -> "+msg) << endl;
            }
        }

        // --- Heartbeat monitoring (mark offline if missed MAX_MISSED_HEARTBEATS) ---
        time_t now = time(nullptr);
        for (auto &kv : campusStatus) {
            auto &key = kv.first;
            auto &cs = kv.second;
            if (cs.online) {
                double diff = difftime(now, cs.lastHeartbeat);
                if (diff > HEARTBEAT_INTERVAL) {
                    cs.missedCount++;
                    if (cs.missedCount >= MAX_MISSED_HEARTBEATS) {
                        cs.online = false;
                        string disp = key;
                        if (campus_display_name.count(key)) disp = campus_display_name[key];
                        cout << make_log(disp + " marked OFFLINE due to missed heartbeats") << endl;
                    }
                }
            }
        }

    } // main loop

    close(listen_fd);
    close(udp_fd);
    return 0;
}

