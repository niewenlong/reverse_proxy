#include "tcp_client.h"

TCPClient::TCPClient(event_base* event_loop, string ip, int port):
    event_loop_(event_loop),
    connect_address_(ip),
    connect_port_(port),
    heart_(0),
    periodic_event_(NULL),
    status_(kConstruct) {
}

static void readcb(struct bufferevent *bev, void *ctx) {
    ITCPClientNotify* pNotify = static_cast<ITCPClientNotify*>(ctx);
    pNotify->OnSockRead(bev);
}

static void
writecb(struct bufferevent *bev, void *user_data) {
    struct evbuffer *output = bufferevent_get_output(bev);
    if (evbuffer_get_length(output) == 0) {
        ITCPClientNotify* pNotify = static_cast<ITCPClientNotify*>(user_data);
        pNotify->OnSockWrote(bev);
    }
}

static void periodiccb(evutil_socket_t fd, short what, void *ctx) {
    IPeriodicNotify* pNotify = static_cast<IPeriodicNotify*>(static_cast<TCPClient*>(ctx));
    pNotify->HandlePeriodic();
}

static void eventcb(struct bufferevent *bev, short events, void *ptr) {
    ITCPClientNotify* pNotify = static_cast<ITCPClientNotify*>(ptr);
    if (events & BEV_EVENT_CONNECTED) {
        pNotify->OnSockConnected(bev);
    } else if ((events | BEV_EVENT_EOF) ||
               (events | BEV_EVENT_ERROR)) {
        pNotify->OnSockClose(bev);
    }
}


bufferevent* CreateConnectSocket(event_base* base, string ip, int port, void* ctx) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr.s_addr);
    sin.sin_port = htons(port);
    struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(bev, readcb, writecb, eventcb, ctx);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        /* Error starting connection */
        bufferevent_free(bev);
        return NULL;
    }
    return bev;
}

int64_t GetTimeStamp() {
#ifdef _WIN32
    _timeb timebuffer;
    _ftime64_s(&timebuffer);
    return timebuffer.time * 1000 + timebuffer.millitm;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return  tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

bool TCPClient::Init() {
    socket_ = CreateConnectSocket(event_loop_, connect_address_, connect_port_, this);
    if (!socket_) {
        return false;
    }
    //增加定时器
    timeval thrity_sec = { 30, 0 };
    periodic_event_ = event_new(event_loop_, -1, EV_PERSIST | EV_TIMEOUT, periodiccb, this);
    event_add(periodic_event_, &thrity_sec);
    status_ = kInit;
    return true;
}

void TCPClient::AppendData(ForwardData& data) {
    data_to_send_.insert(data_to_send_.end(), (char*)&data.len_, (char*)&data.len_ + sizeof(uint32_t));
    data_to_send_.insert(data_to_send_.end(), (char*)&data.to_, (char*)&data.to_ + sizeof(HashType));
    data_to_send_.insert(data_to_send_.end(), (char*)&data.op_, (char*)&data.op_ + sizeof(uint8_t));
    data_to_send_.insert(data_to_send_.end(), data.data_, data.data_ + data.len_);
    WriteToSock();
}

bool TCPClient::WriteToSock() {
    int send_size = data_to_send_.size();
    if (status_ <= kInit || send_size == 0) return false;
    assert(status_ == kConnected || status_ == kCloseWait);
    if (0 != bufferevent_write(socket_, data_to_send_.data(), send_size)) {
        LOGE << "bufferevent_write error\n";
        return false;
    }
    data_to_send_.clear();
    return true;
}

void TCPClient::OnSockRead(bufferevent *bev) {
    struct evbuffer *input = bufferevent_get_input(socket_);
    size_t input_len = evbuffer_get_length(input);
    shared_ptr<char> recv_buffer(new char[input_len]);
    int recv_size = evbuffer_remove(input, recv_buffer.get(), input_len);
    assert(recv_size == input_len);
    data_to_recv_.insert(data_to_recv_.end(), recv_buffer.get(), recv_buffer.get() + input_len);
    ParseData();
}

void TCPClient::ParseData() {
    while (data_to_recv_.size() > 4) {
        size_t size = data_to_recv_.size();
        char* pData = data_to_recv_.data();
        int datalen = *(int*)pData;
        uint32_t needlen = datalen + offsetof(ForwardData, data_);
        if (size < needlen) return;
        HashType to = *(HashType*)(pData + offsetof(ForwardData, to_));
        uint8_t op = *(uint8_t*)(pData + offsetof(ForwardData, op_));
        ForwardData data(to, datalen, pData + offsetof(ForwardData, data_), op);
        data_to_recv_.erase(data_to_recv_.begin(), data_to_recv_.begin() + needlen);
        if (op == ForwardData::kHeartBeat) {
            last_heart_time_ = GetTimeStamp();
            LOGI << "client recieve heart beat" << "\n";
            continue;
        }
        if (op == ForwardData::kCloseConnect) {
            if (socket_handler_.count(to) > 0) {
                static_cast<SOCK5ClientHandler*>(socket_handler_[to])->SetCloseWait();
                continue;
            }
        }
        ForwardToHandler(data);
    }
}

void TCPClient::AddHandler(HashType s, ITCPClientNotify * handler) {
    if (socket_handler_.count(s) > 0) {
        LOGW << "this socket has bind a handler,it may cause memory leak" << "\n";
    }
    socket_handler_[s] = handler;
}

void TCPClient::CloseRemoteConnect(HashType s) {
    char flag = 0x1;
    ForwardData data(s, 1, &flag, ForwardData::kCloseConnect);
    AppendData(data);
}

void TCPClient::RemoveHandler(HashType s) {
    //通知远程关闭socket
    assert(socket_handler_.count(s) > 0);
    socket_handler_.erase(s);
    LOGI << "剩余" << socket_handler_.size() << "\n";
}

bool TCPClient::ForwardToHandler(ForwardData & data) {
    if (data.op_ == ForwardData::kCloseConnect) {
        if (socket_handler_.count(data.to_) == 0) {
            //可能已经被删了，无所谓 返回就行
            assert(data.len_ == 1);
            return true;
        }
        AppendData(data);
        return true;
    } else {
        if (socket_handler_.count(data.to_) == 0) {
            SOCK5ClientHandler* pSocketClient = new SOCK5ClientHandler(this, data.to_, event_loop_);
            if (!pSocketClient->Init()) {
                pSocketClient->OnSockClose(NULL);
                LOGE << "致命error!\n";
                return false;
            }
        }
        static_cast<SOCK5ClientHandler*>(socket_handler_[data.to_])->AppendData(data);
        return true;
    }
}

void TCPClient::HandlePeriodic() {
    if (last_heart_time_ - GetTimeStamp() > 1000 * 120) {
        LOGE << "long time don't recieve heartbeat!\n";
        Close();
        return;
    }
    //send heart beat
    ForwardData data(kHashTypeInvalid, 4, (char*)&heart_, ForwardData::kHeartBeat);
    heart_++;
    AppendData(data);
}

void TCPClient::SendToProxy(ForwardData & data) {
    AppendData(data);
}

void TCPClient::OnSockConnected(bufferevent* bev) {
    status_ = kConnected;
    WriteToSock();
}

void TCPClient::OnSockClose(bufferevent* bev) {
    Close();
}

void TCPClient::Close() {
    assert(status_ != kClosed);
    status_ = kClosed;
    delete this;
}

TCPClient::~TCPClient() {
    if (bufferevent_getfd( socket_ ) != INVALID_SOCKET) {
        bufferevent_free(socket_);
        socket_ = NULL;
    }
    if (periodic_event_) {
        event_free(periodic_event_);
    }
    for (auto& it : socket_handler_) {
        delete it.second;
    }
    struct timeval delay = { 1, 0 };
    LOGE << "TCPClient 结束" << "\n";

    event_base_loopexit(event_loop_, &delay);
}

////////////////
SOCK5ClientHandler::SOCK5ClientHandler(TCPClient * client, HashType hash, event_base * event_loop) {
    hash_ = hash;
    status_ = kConstruct;
    client_ = client;
    event_loop_ = event_loop;
}

bool SOCK5ClientHandler::Init() {
    socket_ = CreateConnectSocket(event_loop_, "127.0.0.1", 1081, this);
    if (!socket_) {
        return false;
    }
    client_->AddHandler(hash_, this);
    status_ = kInit;
    return true;
}

void SOCK5ClientHandler::AppendData(ForwardData& data) {
    data_to_send_.insert(data_to_send_.end(), data.data_, data.data_ + data.len_);
    WriteToSock();
}

bool SOCK5ClientHandler::WriteToSock() {
    int send_size = data_to_send_.size();
    if (status_ <= kInit || send_size == 0) return false;
    assert(status_ == kConnected || status_ == kCloseWait);
    if (0 != bufferevent_write(socket_, data_to_send_.data(), send_size)) {
        LOGE << "bufferevent_write error\n";
        return false;
    }
    data_to_send_.clear();
    return true;
}

void SOCK5ClientHandler::OnSockRead(bufferevent *bev) {
    struct evbuffer *input = bufferevent_get_input(socket_);
    size_t input_len = evbuffer_get_length(input);
    shared_ptr<char> recv_buffer(new char[input_len]);
    int recv_size = evbuffer_remove(input, recv_buffer.get(), input_len);
    assert(recv_size == input_len);
    ForwardData data(hash_, input_len, recv_buffer.get());
    client_->SendToProxy(data);
}

void SOCK5ClientHandler::SetCloseWait() {
    status_ = kCloseWait;
    struct evbuffer *output = bufferevent_get_output(socket_);
    if (evbuffer_get_length(output) == 0) {
        LOGI << "Close For Remote\n";
        Close();
    } else {
        LOGI << "Wait For Close\n";
    }
}

void SOCK5ClientHandler::Close() {
    assert(status_ != kClosed);
    status_ = kClosed;
    delete this;
}

void SOCK5ClientHandler::OnSockConnected(bufferevent *bev) {
    status_ = kConnected;
    WriteToSock();
}

void SOCK5ClientHandler::OnSockClose(bufferevent *bev) {
    client_->CloseRemoteConnect(hash_);
    Close();
}

void SOCK5ClientHandler::OnSockWrote(bufferevent * bev) {
    if (status_ == kCloseWait) {
        Close();
    }
}

SOCK5ClientHandler::~SOCK5ClientHandler() {
    if (socket_) {
        client_->RemoveHandler(hash_);
        bufferevent_free(socket_);
        socket_ = NULL;
    }
}
