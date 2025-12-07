# Multi-Campus Messaging System (TCP + UDP | Poll-Based Server)

This project is a semester assignment where we built a complete communication system that connects
multiple campuses and departments using both TCP and UDP sockets.

The system includes:
- Authentication
- TCP messaging
- File transfer
- UDP heartbeat monitoring
- UDP server broadcasts
- Routing logs
- Connected client list
- Graceful server shutdown notifications

---

## 🔧 How Concurrency Was Handled
We used **poll()** to manage multiple TCP client sockets at the same time without blocking.
The server runs two sockets:
- TCP socket → for authentication, messaging, and file transfer  
- UDP socket → for heartbeats + admin broadcast  

`poll()` allows the server to:
- Accept multiple clients  
- Handle I/O from any client without blocking  
- Process heartbeats  
- React to admin commands instantly

This removes the need for threads for every client and keeps the server efficient.

---

## 📌 Custom Protocol Format
Messages follow this simple text-based format:

Campus|Department|TargetCampus|TargetDept|Body

arduino
Copy code

For file transfer:
Campus|Department|TargetCampus|TargetDept|FILE|filename.txt|<filedata>

yaml
Copy code

This made it easy to parse incoming messages using `strtok()`.

---

## 🚀 How to Compile
make server
make client

pgsql
Copy code

## ▶ How to Run

**Start Server First**
./server

arduino
Copy code

**Then Start Client**
./client

yaml
Copy code

---

## 🧪 Basic Flow of the System

1. **Server starts** and listens on TCP 9090 & UDP 9091  
2. **Client connects** → sends campus, department, password  
3. **Server authenticates** and stores the client in the routing table  
4. Client menu options:  
   - Send message  
   - Send text file  
   - View inbox  
   - Exit  
5. **Admin menu** allows:  
   - View client list  
   - Send UDP broadcast  
   - View routing logs  
   - View heartbeat logs  
   - Shutdown server  
6. Any **message or file** is routed to the correct campus + department  
7. Any **broadcast** appears instantly on all clients  
8. On **shutdown**, all clients receive a TCP notification and disconnect safely  

---
## Team Members
 **1 Wajahat Ali**
 **2 Laiba Khalid**
 **3 Fatima**

## 📘 License
For educational use only.
