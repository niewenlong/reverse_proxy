#include "tcp_server.h"

void usage() {
    LOGE << "usage:rproxy.exe [tcp port] [sock port]" << "\n";
    exit(1);
}
static void
signal_cb(evutil_socket_t sig, short events, void *user_data) {
    struct event_base *base = (event_base*)user_data;
    struct timeval delay = { 2, 0 };

    LOGW << "Caught an interrupt signal; exiting cleanly in one seconds.\n";

    event_base_loopexit(base, &delay);
}

Log* Log::instance = NULL;

int main(int argc, char *argv[]) {
    int tcp_port = 1587, sock_port = 1589;
    if (argc > 2) {
        tcp_port = atoi(argv[1]);
        sock_port = atoi(argv[2]);
        if (tcp_port == 0 || sock_port == 0) {
            usage();
        }
    }
#ifdef _WIN32
    WSADATA wsaData;
    if (0 != WSAStartup(MAKEWORD(2, 2), &wsaData)) {
        return 1;
    }
#endif
    struct event_base *base;
    struct event *signal_event;

    base = event_base_new();
    if (!base) {
        LOGE << "Could not initialize libevent!\n";
        return 1;
    }

    signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);

    if (!signal_event || event_add(signal_event, NULL) < 0) {
        LOGE << "Could not create/add a signal event!\n";
        return 1;
    }

    TCPServer * tcp_server = new TCPServer(base, "0.0.0.0", tcp_port, "0.0.0.0", sock_port);
    if (tcp_server->Init()) {
        LOGI << "tcp server listen on " << tcp_port << "    sock5 server listen on " << sock_port << "\n";
        event_base_dispatch(base);
    }
	tcp_server->Close();
	delete tcp_server;
    event_free(signal_event);
    event_base_free(base);

    Log::GetInstance()->Destory();

    return 0;
}
