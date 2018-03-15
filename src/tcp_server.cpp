#include "tcp_server.h"

const int kCycleBufferSize = 1024 * 1024;

static void readcb(struct bufferevent *bev, void *ctx) {
    IProxyNotify* pNotify = static_cast<IProxyNotify*>(ctx);
    pNotify->OnSockRead(bev);
}

static void
writecb(struct bufferevent *bev, void *user_data) {
    struct evbuffer *output = bufferevent_get_output(bev);
    if (evbuffer_get_length(output) == 0) {
        IProxyNotify* pNotify = static_cast<IProxyNotify*>(user_data);
        pNotify->OnSockWrote(bev);
    }
}

static void periodiccb(evutil_socket_t fd, short what, void *ctx) {
    IProxyNotify* pNotify = static_cast<IProxyNotify*>(ctx);
    pNotify->HandlePeriodic();
}

static void eventcb(struct bufferevent *bev, short events, void *ptr) {
    IProxyNotify* pNotify = static_cast<IProxyNotify*>(ptr);
    if (events & BEV_EVENT_CONNECTED) {
        pNotify->OnSockConnected(bev);
    } else if( (events | BEV_EVENT_EOF) ||
               (events | BEV_EVENT_ERROR)) {
        pNotify->OnSockClose(bev);
    }
}

static HashType GetHashFromConnectInfo(void* sa, int len) {
    static std::hash<string> pStrHash;
    stringstream ss;
    unsigned char* pAddr = (unsigned char*)sa;
    for(int i = 0; i < len; i++)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)pAddr[i];
    HashType hash = pStrHash(ss.str());
    return hash == kHashTypeInvalid ? 0 : hash;
}

Sock5Client::Sock5Client(TCPServer* server,
                         event_base* event_loop,
                         bufferevent* local_socket,
                         HashType hash):
    server_(server),
    event_loop_(event_loop),
    socket_(local_socket),
    status_(kConnected),
    hash_(hash),
    heart_(0) {
    bufferevent_setcb(socket_, readcb, writecb, eventcb, this);
    bufferevent_enable(socket_, EV_READ | EV_WRITE);
    server_->AddHandler(hash_, this);
}

void Sock5Client::AppendData(ForwardData& data) {
    //sock5 clear header
    assert(status_ == kConnected || status_ == kCloseWait);
    if (data.len_ > 0) {
        if (0 != bufferevent_write(socket_, data.data_, data.len_)) {
            LOGE << "bufferevent_write error\n";
            return;
        }
    }
}

void Sock5Client::OnSockRead(bufferevent *bev) {
    struct evbuffer *input = bufferevent_get_input(socket_);
    size_t input_len = evbuffer_get_length(input);
    shared_ptr<char> recv_buffer(new char[input_len]);
    int recv_size = evbuffer_remove(input, recv_buffer.get(), input_len);
    assert(recv_size == input_len);
    //forward data to proxy
    ForwardData data(hash_, input_len, recv_buffer.get());
    if (!server_->SendToProxy(data)) {
        LOGW << "没有Proxy在线！\n";
        Close();
        return;
    }
}

void Sock5Client::OnSockWrote(bufferevent * bev) {
    if (status_ == kCloseWait) {
        Close();
    }
}

void Sock5Client::OnSockClose(bufferevent * bev) {
    server_->CloseRemoteConnect(hash_);
    Close();
}

void Sock5Client::HandleForward(ForwardData & data) {
    if (data.op_ == ForwardData::kCloseConnect) {
        SetCloseWait();
        return;
    }
    AppendData(data);
}

void Sock5Client::SetCloseWait() {
    status_ = kCloseWait;
    struct evbuffer *output = bufferevent_get_output(socket_);
    if (evbuffer_get_length(output) == 0) {
        LOGI << "Close For Remote\n";
        Close();
    } else {
        LOGI << "Wait For Close\n";
    }
}

void Sock5Client::Close() {
    assert(status_ != kClosed);
    status_ = kClosed;
    server_->RemoveHandler(hash_);
    delete this;
}

Sock5Client::~Sock5Client() {
    if (socket_) {
        bufferevent_free(socket_);
        socket_ = NULL;
    }
}

/////////////////////////////
ProxyClient::ProxyClient(TCPServer* server,
                         event_base* event_loop,
                         bufferevent* local_socket) :
    periodic_event_(NULL),
    server_(server),
    event_loop_(event_loop),
    socket_(local_socket),
    status_(kConnected),
    heart_(0) {
    bufferevent_setcb(socket_, readcb, writecb, eventcb, this);
    bufferevent_enable(socket_, EV_READ | EV_WRITE);
    data_to_send_.reserve(kCycleBufferSize);
    data_to_recv_.reserve(kCycleBufferSize);
    //增加定时器
    timeval thrity_sec = { 30, 0 };
    periodic_event_ = event_new(event_loop_, -1, EV_PERSIST | EV_TIMEOUT, periodiccb, this);
    event_add(periodic_event_, &thrity_sec);
}

void ProxyClient::AppendData(ForwardData& data) {
    data_to_send_.insert(data_to_send_.end(), (char*)&data.len_, (char*)&data.len_ + sizeof(uint32_t));
    data_to_send_.insert(data_to_send_.end(), (char*)&data.to_, (char*)&data.to_ + sizeof(HashType));
    data_to_send_.insert(data_to_send_.end(), (char*)&data.op_, (char*)&data.op_ + sizeof(uint8_t));
    data_to_send_.insert(data_to_send_.end(), data.data_, data.data_ + data.len_);
    WriteToSock();
}

void ProxyClient::HandleForward(ForwardData & data) {
    AppendData(data);
}

bool ProxyClient::WriteToSock() {
    int send_size = data_to_send_.size();
    if (send_size == 0) return false;
    assert(status_ == kConnected || status_ == kCloseWait);
    if (0 != bufferevent_write(socket_, data_to_send_.data(), send_size)) {
        LOGE << "bufferevent_write error\n";
        return false;
    }
    data_to_send_.clear();
    return true;
}

void ProxyClient::OnSockRead(bufferevent *bev) {
    struct evbuffer *input = bufferevent_get_input(socket_);
    size_t input_len = evbuffer_get_length(input);
    shared_ptr<char> recv_buffer(new char[input_len]);
    int recv_size = evbuffer_remove(input, recv_buffer.get(), input_len);
    assert(recv_size == input_len);
    data_to_recv_.insert(data_to_recv_.end(), recv_buffer.get(), recv_buffer.get() + input_len);
    ParseData();
}

void ProxyClient::OnSockClose(bufferevent * bev) {
    Close();
}

void ProxyClient::ParseData() {
    while (data_to_recv_.size() > 4) {
        size_t size = data_to_recv_.size();
        char* pData = data_to_recv_.data();
        uint32_t datalen = *(uint32_t*)pData;
        uint32_t needlen = datalen + offsetof(ForwardData, data_);
        if (size < needlen) return;
        HashType to = *(HashType*)(pData + offsetof(ForwardData, to_));
        uint8_t op = *(uint8_t*)(pData + offsetof(ForwardData, op_));
        ForwardData data(to, datalen, pData + offsetof(ForwardData, data_), op);
        data_to_recv_.erase(data_to_recv_.begin(), data_to_recv_.begin() + needlen);
        if (op == ForwardData::kHeartBeat) {
            LOGI << "server recieve heart beat" << "\n";
            continue;
        }
        server_->SendToSock5(data);
    }
}

void ProxyClient::HandlePeriodic() {
    //send heart beat
    ForwardData data(kHashTypeInvalid, 4, (char*)&heart_, ForwardData::kHeartBeat);
    heart_++;
    AppendData(data);
}

void ProxyClient::Close() {
    assert(status_ != kClosed);
    status_ = kClosed;
    server_->RemoveProxyHandler();
    delete this;
}

ProxyClient::~ProxyClient() {
    if (periodic_event_)
        event_free(periodic_event_);
    if (socket_) {
        bufferevent_free(socket_);
        socket_ = NULL;
    }
}

//////////////////////////////////////////////
static void
listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
            struct sockaddr *sa, int socklen, void *user_data) {
    ITCPServerNotify* pNotify = static_cast<ITCPServerNotify*>(user_data);
    struct event_base* base = evconnlistener_get_base(listener);
    struct bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        LOGE << "Error constructing bufferevent!\n";
        event_base_loopbreak(base);
        return;
    }
    pNotify->OnSockListen(listener, bev, sa, socklen);
}

bool TCPServer::Init() {
    return InitProxyServer() && InitSock5Server();
}

bool TCPServer::InitSock5Server() {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, sock5_address_.c_str(), &sin.sin_addr.s_addr);
    sin.sin_port = htons(sock5_port_);

    struct evconnlistener *listener;
    listener = evconnlistener_new_bind(event_loop_, listener_cb, this,
                                       LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                       (struct sockaddr*)&sin,
                                       sizeof(sin));

    if (!listener) {
        LOGE << "Could not create a listener!\n";
        return false;
    }
    this->sock5_socket_ = listener;
    return true;
}

bool TCPServer::InitProxyServer() {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, proxy_address_.c_str(), &sin.sin_addr.s_addr);
    sin.sin_port = htons(proxy_port_);

    struct evconnlistener *listener;
    listener = evconnlistener_new_bind(event_loop_, listener_cb, this,
                                       LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                       (struct sockaddr*)&sin,
                                       sizeof(sin));

    if (!listener) {
        LOGE << "Could not create a listener!\n";
        return false;
    }
    this->proxy_socket_ = listener;
    return true;
}

TCPServer::TCPServer(
    event_base* event_loop,
    string proxy_address,
    int proxy_port,
    string sock5_address,
    int sock5_port):
    is_closed_(false),
    event_loop_(event_loop),
    proxy_socket_(NULL),
    sock5_socket_(NULL),
    proxy_address_(proxy_address),
    proxy_port_(proxy_port),
    sock5_address_(sock5_address),
    sock5_port_(sock5_port),
    proxy_handler_(NULL) {
}

void TCPServer::AddHandler(HashType hash, ISock5Notify * handler) {
    if (sock5_handler_.count(hash) > 0) {
        LOGW << "this socket has bind a handler,it may cause memory leak" << "\n";
    }
    sock5_handler_[hash] = handler;
}

void TCPServer::CloseRemoteConnect(HashType s) {
    char flag = 0x1;
    ForwardData data(s, 1, &flag, ForwardData::kCloseConnect);
    if(proxy_handler_)
        proxy_handler_->HandleForward(data);
}

void TCPServer::RemoveHandler(HashType s) {
    //关闭远程socket
    sock5_handler_.erase(s);
    LOGI << "剩余" << sock5_handler_.size() << "\n";
}

void TCPServer::RemoveProxyHandler() {
    proxy_handler_ = NULL;
}

void TCPServer::OnSockListen(struct evconnlistener *listener,
                             bufferevent* bev,
                             struct sockaddr *sa,
                             int socklen) {
    assert(bev && listener);
    if (listener == proxy_socket_) {
        LOGI << "Handle Proxy Socket" << "\n";
        if (proxy_handler_) {
            delete proxy_handler_;
        }
        proxy_handler_ = new ProxyClient(this, event_loop_, bev);
    } else if (listener == sock5_socket_) {
        LOGI << "Handle Sock5 Socket" << "\n";
        HashType hash = GetHashFromConnectInfo(sa, socklen);
        new Sock5Client(this, event_loop_, bev, hash);
    } else {
        assert(false);
    }
}

void TCPServer::Close() {
    LOGI << "TCP Server close" << "\n";
    is_closed_ = true;
    if (event_loop_) {
        event_base_loopbreak(event_loop_);
    }
    if (proxy_socket_)
        evconnlistener_free(proxy_socket_);
    if (sock5_socket_)
        evconnlistener_free(sock5_socket_);
    for (auto& iter : sock5_handler_) {
        delete iter.second;
    }
}

bool TCPServer::SendToSock5(ForwardData & data) {
    HashType s = data.to_;
    if (s == kHashTypeInvalid) {
        LOGW << "Invalid HashType" << "\n";
        return false;
    }
    if (sock5_handler_.count(s) == 0) {
        if(data.op_ != ForwardData::kCloseConnect)
            LOGW << "No This HashType" << "\n";
        return false;
    }
    sock5_handler_[s]->HandleForward(data);
    return true;
}

bool TCPServer::SendToProxy(ForwardData & data) {
    if (proxy_handler_ == NULL) {
        return false;
    }
    proxy_handler_->HandleForward(data);
    return true;
}