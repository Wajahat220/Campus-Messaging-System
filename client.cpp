// client.cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"

using namespace std;

mutex inbox_mtx;
vector<Message> inbox;
bool server_shutdown_received = false;

string to_lower(const string &s) {
    string out = s;
    transform(out.begin(), out.end(), out.begin(),
              [](unsigned char c){ return tolower(c); });
    return out;
}

// simple base64 encode/decode (for file transfer)
static const string b64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

string base64_encode(const string &in) {
    string out;
    int val=0, valb=-6;
    for (unsigned char c : in) {
        val = (val<<8) + c;
        valb += 8;
        while (valb>=0) {
            out.push_back(b64_chars[(val>>valb)&0x3F]);
            valb -= 6;
        }
    }
    if (valb>-6) out.push_back(b64_chars[((val<<8)>>(valb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}

string base64_decode(const string &in) {
    vector<int> T(256,-1);
    for (int i=0;i<64;i++) T[(unsigned char)b64_chars[i]] = i;
    string out;
    int val=0, valb=-8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val<<6) + T[c];
        valb += 6;
        if (valb>=0) {
            out.push_back(char((val>>valb)&0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Utility to insert new message at top
void push_inbox_top(const Message &m) {
    lock_guard<mutex> lk(inbox_mtx);
    inbox.insert(inbox.begin(), m);
}

// TCP receive thread
void tcp_receive_loop(int tcp_sock, const string &selfCampus, const string &selfDept) {
    char buf[BUFFER_SIZE];
    while (true) {
        ssize_t r = recv(tcp_sock, buf, sizeof(buf)-1, 0);
        if (r <= 0) {
            cout << "[TCP] Disconnected from server." << endl;
            close(tcp_sock);
            exit(0);
        }
        buf[r] = 0;
        string s(buf);

        // parse header token
        vector<string> toks;
        string tmp;
        for (char c : s) {
            if (c=='|') { toks.push_back(tmp); tmp.clear(); } else tmp.push_back(c);
        }
        toks.push_back(tmp);

        Message m;
        if (!toks.empty() && toks[0] == "FROM" && toks.size() >= 4) {
            m.fromCampus = toks[1];
            m.fromDept = toks[2];
            m.content = toks[3];
            m.toCampus = selfCampus;
            m.toDept = selfDept;
            m.read = false;
            push_inbox_top(m);
        } else if (!toks.empty() && toks[0] == "FILEFROM" && toks.size() >= 5) {
            string fromC = toks[1];
            string fromD = toks[2];
            string filename = toks[3];
            // Remaining part is base64 content (in case | in content)
            // find 4th '|' to extract base64 substring
            size_t seen = 0; size_t pos = 0;
            for (pos=0; pos < s.size(); ++pos) {
                if (s[pos] == '|') {
                    seen++;
                    if (seen == 4) { ++pos; break; }
                }
            }
            string b64 = (pos < s.size() ? s.substr(pos) : toks[4]);

            string filedata = base64_decode(b64);
            // save file
            ofstream ofs(filename, ios::binary);
            if (ofs) {
                ofs.write(filedata.data(), filedata.size());
                ofs.close();
            }

            m.fromCampus = fromC;
            m.fromDept = fromD;
            m.toCampus = selfCampus;
            m.toDept = selfDept;
            m.content = "[FILE RECEIVED] " + filename + " (" + to_string(filedata.size()) + " bytes)";
            m.read = false;
            push_inbox_top(m);
            cout << "[INFO] Received file '" << filename << "' saved to current dir.\n";
        } else if (!toks.empty() && toks[0] == "BCAST") {
            Message b;
            b.fromCampus = "ADMIN";
            b.fromDept = "";
            b.toCampus = selfCampus;
            b.toDept = selfDept;
            // rejoin content after first '|'
            size_t p = s.find("|");
            b.content = (p==string::npos ? "" : s.substr(p+1));
            push_inbox_top(b);
        } else if (!toks.empty() && toks[0] == "ERR") {
            Message e;
            e.fromCampus = "SERVER";
            e.fromDept = "";
            e.toCampus = selfCampus;
            e.toDept = selfDept;
            e.content = s;
            push_inbox_top(e);
        } else if (!toks.empty() && toks[0] == "SHUTDOWN") {
            Message sh;
            sh.fromCampus = "SERVER";
            sh.fromDept = "";
            sh.toCampus = selfCampus;
            sh.toDept = selfDept;
            // rejoin the rest
            size_t p = s.find("|");
            sh.content = (p==string::npos ? "Server shutting down" : s.substr(p+1));
            push_inbox_top(sh);
            server_shutdown_received = true;
            cout << "\n[NOTICE] Server sent shutdown message. See inbox. Press Enter to close when ready.\n";
        } else {
            // unknown message: put as raw
            Message raw;
            raw.fromCampus = "SERVER";
            raw.fromDept = "";
            raw.toCampus = selfCampus;
            raw.toDept = selfDept;
            raw.content = s;
            push_inbox_top(raw);
        }
    }
}

// UDP heartbeat sender
void udp_heartbeat_sender(int udp_sock, sockaddr_in server_udp_addr, const string &campus, const string &dept) {
    while (true) {
        string payload = string("HB|") + campus + "|" + dept;
        sendto(udp_sock, payload.c_str(), payload.size(), 0, (sockaddr*)&server_udp_addr, sizeof(server_udp_addr));
        this_thread::sleep_for(chrono::seconds(HEARTBEAT_INTERVAL));
    }
}

// UDP listener for broadcasts
void udp_listener(int udp_sock, const string &selfCampus) {
    char buf[BUFFER_SIZE];
    while (true) {
        sockaddr_in src; socklen_t sl = sizeof(src);
        ssize_t r = recvfrom(udp_sock, buf, sizeof(buf)-1, 0, (sockaddr*)&src, &sl);
        if (r > 0) {
            buf[r] = 0;
            string s(buf);
            if (s.rfind("BCAST|", 0) == 0) {
                Message m;
                m.fromCampus = "ADMIN";
                m.fromDept = "";
                m.toCampus = selfCampus;
                m.toDept = "";
                m.content = s.substr(6);
                push_inbox_top(m);
            }
        } else {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    }
}

int main() {
    cout << "Campus Department Client\nEnter campus name (e.g., Lahore): ";
    string campus; getline(cin, campus);

    cout << "Enter department name (e.g., Admissions): ";
    string dept; getline(cin, dept);

    cout << "Enter password (for demo use matching server credentials): ";
    string pass; getline(cin, pass);

    // --- TCP connect ---
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) { perror("tcp socket"); return 1; }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    if (connect(tcp_sock, (sockaddr*)&srv, sizeof(srv)) < 0) { perror("connect"); return 1; }
    cout << "[TCP] Connected to server." << endl;

    // send AUTH (now includes dept)
    string auth = "AUTH|" + campus + "|" + dept + "|" + pass;
    send(tcp_sock, auth.c_str(), auth.size(), 0);

    char rbuf[256];
    ssize_t r = recv(tcp_sock, rbuf, sizeof(rbuf)-1, 0);
    if (r <= 0) { cout << "No response from server\n"; return 1; }
    rbuf[r] = 0;
    string resp(rbuf);
    if (resp != "AUTH_OK") {
        cout << "Authentication failed: " << resp << endl;
        close(tcp_sock);
        return 1;
    }
    cout << "Authenticated successfully.\n";

    // --- UDP socket ---
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("udp socket"); return 1; }
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = INADDR_ANY; local.sin_port = 0;
    if (bind(udp_sock, (sockaddr*)&local, sizeof(local)) < 0) { perror("bind udp"); return 1; }

    sockaddr_in server_udp_addr{}; server_udp_addr.sin_family = AF_INET; server_udp_addr.sin_port = htons(UDP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_udp_addr.sin_addr);

    // Threads
    thread(tcp_receive_loop, tcp_sock, campus, dept).detach();
    thread(udp_listener, udp_sock, campus).detach();
    thread(udp_heartbeat_sender, udp_sock, server_udp_addr, campus, dept).detach();

    // --- Menu loop ---
    while (true) {
        cout << "\n--- Menu ---\n1) Send message\n2) Send file (text)\n3) View inbox\n4) Exit\nChoose: ";
        string choice; getline(cin, choice);

        if (choice == "1") {
            cout << "Target Campus: "; string target; getline(cin, target);
            cout << "Target Department: "; string tdept; getline(cin, tdept);
            cout << "Message: "; string body; getline(cin, body);
            string msg = "MSG|" + target + "|" + tdept + "|" + body;
            send(tcp_sock, msg.c_str(), msg.size(),0);
            cout << "[Sent]" << endl;
        } else if (choice == "2") {
            cout << "Target Campus: "; string target; getline(cin, target);
            cout << "Target Department: "; string tdept; getline(cin, tdept);
            cout << "Path to text file to send: "; string path; getline(cin, path);
            // read file
            ifstream ifs(path, ios::binary);
            if (!ifs) { cout << "Unable to open file\n"; continue; }
            string content((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());
            string b64 = base64_encode(content);
            // extract filename part
            string filename;
            size_t pos = path.find_last_of("/\\");
            if (pos == string::npos) filename = path;
            else filename = path.substr(pos+1);
            // send
            string out = "FILE|" + target + "|" + tdept + "|" + filename + "|" + b64;
            send(tcp_sock, out.c_str(), out.size(), 0);
            cout << "[File Sent]\n";
        } else if (choice == "3") {
            lock_guard<mutex> lk(inbox_mtx);
            if (inbox.empty()) { cout << "No messages.\n"; continue; }
            cout << "---- Inbox (newest on top) ----\n";
            for (size_t i=0;i<inbox.size();++i) {
                Message &m = inbox[i];
                cout << i+1 << ") FROM: " << m.fromCampus;
                if (!m.fromDept.empty()) cout << " / " << m.fromDept;
                cout << "\n    TO: " << m.toCampus;
                if (!m.toDept.empty()) cout << " / " << m.toDept;
                cout << "\n    MSG: " << m.content;
                if (!m.read) cout << " [NEW]";
                cout << "\n";
                m.read = true;
            }
            cout << "---- End ----\n";
            if (server_shutdown_received) {
                cout << "\nServer shutdown message received. Press Enter to close client.\n";
                string dummy; getline(cin, dummy);
                cout << "Exiting (server requested shutdown)...\n";
                close(tcp_sock); close(udp_sock);
                return 0;
            }
        } else if (choice == "4") {
            cout << "Exiting...\n";
            close(tcp_sock); close(udp_sock);
            return 0;
        } else {
            cout << "Invalid choice\n";
        }
    }

    return 0;
}

