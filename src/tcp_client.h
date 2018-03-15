#ifndef _TCP_CLIENT_H_
#define _TCP_CLIENT_H_

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

class ITCPClientNotify {
public:
    virtual ~ITCPClientNotify() {};
    virtual void OnSockRead(bufferevent *bev) = 0;
    virtual void OnSockClose(bufferevent *bev) = 0;
    virtual void OnSockConnected(bufferevent *bev) = 0;
    virtual void OnSockWrote(bufferevent *bev) {};
};

class IPeriodicNotify {
public:
    IPeriodicNotify() {};
    virtual ~IPeriodicNotify() {};
    //invoke when socket has signaled
    virtual void HandlePeriodic() = 0;
};

typedef uint32_t HashType;
enum {
    kHashTypeInvalid = -1
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
};

class TCPClient : public ITCPClientNotify, public IPeriodicNotify {
public:
    TCPClient(event_base * event_loop, string ip, int port);

    bool Init();

    void AppendData(ForwardData & data);

    virtual void OnSockRead(bufferevent * bev);

    virtual void HandlePeriodic();

	void SendToProxy(ForwardData & data);

    bool ForwardToHandler(ForwardData& data);

    virtual void OnSockConnected(bufferevent * bev);

    virtual void OnSockClose(bufferevent * bev);

    void Close();

    void AddHandler(HashType s, ITCPClientNotify * handler);

    void CloseRemoteConnect(HashType s);

    void RemoveHandler(HashType s);

private:
    ~TCPClient();

    event_base* event_loop_;

    bufferevent* socket_;

    event* periodic_event_;

    //远程socket编号到本地编号
    map<HashType, ITCPClientNotify*> socket_handler_;

    int status_;

    string		connect_address_;

    uint16_t	connect_port_;

    vector<char> data_to_send_;

    vector<char> data_to_recv_;

    bool WriteToSock();

    void ParseData();

    int64_t last_heart_time_;

    int heart_;
};

enum CLIENT_STATUS {
    kConstruct = 0,
    kInit,
    kConnected,
    kCloseWait,
    kClosed
};

class SOCK5ClientHandler : public ITCPClientNotify {
public:
    SOCK5ClientHandler(TCPClient * client,
                       HashType hash,
                       event_base * event_loop);

    bool Init();

	void SetCloseWait();

	void AppendData(ForwardData & data);

    virtual void OnSockRead(bufferevent * bev);

    virtual void OnSockConnected(bufferevent *bev);

    virtual void OnSockClose(bufferevent *bev);

    virtual void OnSockWrote(bufferevent* bev);

private:
    void Close();

    ~SOCK5ClientHandler();

    TCPClient* client_;

    int status_;

    event_base* event_loop_;

    bufferevent* socket_;

    HashType hash_;

    vector<char> data_to_send_;

    vector<char> data_to_recv_;

    bool WriteToSock();
};

#endif
