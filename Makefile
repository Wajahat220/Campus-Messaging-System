all: server client

server: server.cpp
	g++ server.cpp -o server -std=c++17

client: client.cpp
	g++ client.cpp -o client -std=c++17 -pthread

clean:
	rm -f server client

