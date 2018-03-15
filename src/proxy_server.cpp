#include "tcp_client.h"

static void
signal_cb(evutil_socket_t sig, short events, void *user_data)
{
	struct event_base *base = (event_base*)user_data;
	struct timeval delay = { 2, 0 };

	LOGW<<"Caught an interrupt signal; exiting cleanly in one seconds.\n";

	event_base_loopexit(base, &delay);
}

Log* Log::instance = NULL;

int main(int argc, char *argv[]) {
	if (argc < 2)
	{
		exit(1);
	}
	char* tcp_addr = argv[1];
	int tcp_port = atoi(argv[2]);
#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(0x0201, &wsa_data);
#else
	pid_t pid = fork();
	if (pid > 0) {
		return 0;
	}
	else if (pid == -1) {
		perror("fork failed");
		return -1;
	}
#endif
	struct event_base *base;
	struct event *signal_event;

	base = event_base_new();
	if (!base) {
		LOGE<<"Could not initialize libevent!\n";
		return 1;
	}

	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);

	if (!signal_event || event_add(signal_event, NULL)<0) {
		LOGE<<"Could not create/add a signal event!\n";
		return 1;
	}

	TCPClient * tcp_client = new TCPClient(base, tcp_addr, tcp_port);
	if (tcp_client->Init()) {
		LOGI << "Init TCPClient Success!\n";
		event_base_dispatch(base);
	}
	event_free(signal_event);
	event_base_free(base);

	Log::GetInstance()->Destory();
	return 0;
}