#ifndef _TCP_SERVER_H_
#define _TCP_SERVER_H_

#include <list>
#include <iostream>
#include <set>
#include <map>
#include <vector>
#include <cassert>
#include <string>
#include <sstream>
#include <stdint.h>
#include <memory>
#include <sys/timeb.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <iomanip>
#ifndef _WIN32
#include <netinet/in.h>
# ifdef _XOPEN_SOURCE_EXTENDED
#  include <arpa/inet.h>
# endif
#include <sys/socket.h>
#endif

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>

using namespace std;

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib,"ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET -1
#endif

#include "log.hpp"


typedef uint32_t HashType;
enum {
    kHashTypeInvalid = -1
};

class ITCPServerNotify {
public:
    virtual ~ITCPServerNotify() {};
    virtual void OnSockListen(struct evconnlistener *listener, bufferevent* bev, struct sockaddr *sa, int socklen) = 0;
};

class ITCPClientNotify {
public:
    virtual ~ITCPClientNotify() {};
    virtual void OnSockRead(bufferevent *bev) = 0;
    virtual void OnSockClose(bufferevent *bev) = 0;
	virtual void OnSockConnected(bufferevent *bev) {};
    virtual void OnSockWrote(bufferevent *bev) {};
};

class IPeriodicNotify {
public:
    IPeriodicNotify() {};
    virtual ~IPeriodicNotify() {};
    //invoke when socket has signaled
    virtual void HandlePeriodic() = 0;
};

#pragma pack(1)
struct ForwardData {
    enum {
        kHeartBeat = 0,
        kSendData,
        kCloseConnect
    };
    ForwardData(HashType to, uint32_t len, char* data, uint8_t op = kSendData) {
        len_ = len;
        data_ = new char[len_];
        op_ = op;
        to_ = to;
        memcpy(data_, data, len_);
    }
    ~ForwardData() {
        if (data_) delete data_;
    }
    uint32_t len_;
    HashType to_;
    uint8_t op_;
    char* data_;
} ;

class IProxyNotify: public ITCPClientNotify, public IPeriodicNotify {
public:
    virtual ~IProxyNotify() {};
    virtual void HandleForward(ForwardData& data) = 0;
};

class ISock5Notify : public ITCPClientNotify {
public:
    virtual ~ISock5Notify() {};
    virtual void HandleForward(ForwardData& data) = 0;
};

//tcp socket server
class TCPServer : public ITCPServerNotify {
public:
    bool Init();
    bool InitSock5Server();
    bool InitProxyServer();
    TCPServer(event_base* event_loop, string proxy_address, int proxy_port, string sock5_address, int sock5_port);
    void AddHandler(HashType s, ISock5Notify* handler) ;
    void CloseRemoteConnect(HashType s);
    void RemoveHandler(HashType s);
    void RemoveProxyHandler();
    virtual void OnSockListen(struct evconnlistener *listener, bufferevent* bev, struct sockaddr *sa, int socklen);
    void Close();
    bool SendToSock5(ForwardData& data);
    bool SendToProxy(ForwardData& data);
private:
    bool is_closed_;
    event_base* event_loop_;
    evconnlistener* proxy_socket_;
    evconnlistener* sock5_socket_;
    string proxy_address_;
    int proxy_port_;
    string sock5_address_;
    int sock5_port_;
    map<HashType, ISock5Notify*> sock5_handler_;
    IProxyNotify* proxy_handler_;
};


enum CLIENT_STATUS {
    kConstruct = 0,
    kInit,
    kConnected,
    kCloseWait,
    kClosed
};

class Sock5Client : public ISock5Notify {
public:
    Sock5Client(TCPServer* server,
                event_base* event_loop,
                bufferevent* local_socket,
                HashType hash);
    void SetCloseWait();

    virtual void HandleForward(ForwardData& data);

    virtual void OnSockRead(bufferevent *bev);

    virtual void OnSockWrote(bufferevent *bev);

    virtual void OnSockClose(bufferevent *bev);

private:
    ~Sock5Client();

    TCPServer* server_;

    event_base* event_loop_;

    bufferevent* socket_;

    int status_;

    void Close();

    void AppendData(ForwardData & data);

    int heart_;

    HashType hash_;
};

class ProxyClient : public IProxyNotify {
public:
    ProxyClient(TCPServer * server,
                event_base * event_loop,
                bufferevent* local_socket);

    virtual void HandleForward(ForwardData& data);

    virtual void HandlePeriodic();

    virtual void OnSockRead(bufferevent *bev);

    virtual void OnSockClose(bufferevent *bev);

private:
    ~ProxyClient();

    TCPServer* server_;

    event_base* event_loop_;

    bufferevent* socket_;

    int status_;

    vector<char> data_to_send_;

    vector<char> data_to_recv_;

    void AppendData(ForwardData & data);

    bool WriteToSock();

    void ParseData();

    void Close();

    int heart_;

    event* periodic_event_;
};


#endif
