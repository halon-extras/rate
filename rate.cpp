#include <HalonMTA.h>
#include <netdb.h>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <vector>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <syslog.h>
#include <math.h>
#include <mutex>
#include <algorithm>
#if defined(__linux__)
#include <bsd/bsd.h>
#endif

using namespace std::chrono_literals;

#define RATE_NS_LEN 64
#define RATE_ENTRY_LEN 512

enum rate_paket_type
{
	RATE_HIT = 1,
	RATE_COUNT,
	SYNC,
	SYNC_HIT,
	RATE_HIT_NOSYNC,
};

struct rate_packet_hit
{
	uint8_t version;
	uint8_t type;
	uint32_t cookie;
	uint32_t time;
	uint32_t count;
	char ns[RATE_NS_LEN];
	char entry[RATE_ENTRY_LEN];
} __attribute__((packed));

struct rate_packet_hit_reply
{
	uint32_t cookie;
	bool allowed;
	bool exceeded;
	uint32_t nextexpire;
} __attribute__((packed));

struct rate_packet_count
{
	uint8_t version;
	uint8_t type;
	uint32_t cookie;
	uint32_t time;
	char ns[RATE_NS_LEN];
	char entry[RATE_ENTRY_LEN];
} __attribute__((packed));

struct rate_packet_count_reply
{
	uint32_t cookie;
	uint32_t count;
} __attribute__((packed));

std::mutex m;
std::thread p;
std::condition_variable cv;
std::mutex cv_m;
bool stop = false;
std::vector<int> sockets;
unsigned long reresolve = 0;
std::string path;
std::string port;
std::string address;
bool badhost = false;
int timeout = 5000;

HALON_EXPORT
int Halon_version()
{
	return HALONMTA_PLUGIN_VERSION;
}

bool create_sockets(std::string path_, std::string port_, std::string address_, std::vector<int> &sockets_)
{
	std::vector<std::pair<sockaddr_storage, socklen_t>> addrs;

	if (!port_.empty())
	{
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;

		struct addrinfo* result;
		int error = getaddrinfo((!address_.empty()) ? address_.c_str() : "localhost", port_.c_str(), &hints, &result);
		if (error != 0)
		{
			if (error == EAI_SYSTEM)
				syslog(LOG_ERR, "rate(getaddrinfo): %m");
			else
				syslog(LOG_ERR, "rate(getaddrinfo): %s", gai_strerror(error));
			return false;
		}

		for (addrinfo *r = result; r != NULL; r = r->ai_next)
		{
			struct sockaddr_storage dst;
			memset(&dst, 0, sizeof dst);
			socklen_t len = 0;
			memcpy(&dst, r->ai_addr, r->ai_addrlen);
			len = r->ai_addrlen;
			addrs.push_back({ dst, len });
		}
		freeaddrinfo(result);
	}
	else
	{
		struct sockaddr_storage dst;
		memset(&dst, 0, sizeof dst);
		socklen_t len = 0;
		struct sockaddr_un* d = (struct sockaddr_un*)&dst;
		d->sun_family = PF_LOCAL;
		strlcpy(d->sun_path, (!path_.empty()) ? path_.c_str() : "/var/run/halon/rated.sock", sizeof d->sun_path);
		len = sizeof(struct sockaddr_un);
		addrs.push_back({ dst, len });
	}

	for (const auto &a : addrs)
	{
		sockaddr_storage dst = a.first;
		socklen_t len = a.second;

		int udp_internal = socket(dst.ss_family, SOCK_DGRAM, 0);
		if (udp_internal == -1)
		{
			syslog(LOG_CRIT, "rate(socket): %m");
			continue;
		}

		if (port_.empty())
		{
			struct sockaddr_un addr;
			memset(&addr, 0, sizeof addr);
			addr.sun_family = PF_LOCAL;
			if (bind(udp_internal, (struct sockaddr *)&addr, sizeof(sa_family_t)) == -1)
			{
				close(udp_internal);
				syslog(LOG_CRIT, "rate(bind): %m");
				continue;
			}
		}

		if (connect(udp_internal, (struct sockaddr *)&dst, len) == -1)
		{
			close(udp_internal);
			syslog(LOG_CRIT, "rate(connect): %m");
			continue;
		}

		sockets_.push_back(udp_internal);
	}

	return !sockets_.empty();
}

HALON_EXPORT
bool Halon_init(HalonInitContext* hic)
{
	HalonConfig *cfg, *app;

	HalonMTA_init_getinfo(hic, HALONMTA_INIT_CONFIG, nullptr, 0, &cfg, nullptr);
	const char* reresolve_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "reresolve"), nullptr);
 	if (reresolve_) reresolve = strtoul(reresolve_, nullptr, 0);

	HalonMTA_init_getinfo(hic, HALONMTA_INIT_APPCONFIG, nullptr, 0, &app, nullptr);
	const char* path_ = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "path"), nullptr);
	if (path_) path = path_;
	const char* port_ = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "port"), nullptr);
	if (port_) port = port_;
	const char* address_ = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "address"), nullptr);
	if (address_) address = address_;
	const char* badhost_ = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "badhost"), nullptr);
	if (badhost_) badhost = (strcmp(badhost_, "true") == 0 || strcmp(badhost_, "TRUE") == 0) ? true : false;
	const char* timeout_ = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "timeout"), nullptr);
	if (timeout_) timeout = strtoul(timeout_, nullptr, 10);

	if (!port.empty())
		create_sockets(path, port, address, sockets);

	if (reresolve)
	{
		p = std::thread([]{
			pthread_setname_np(pthread_self(), "p/rate/resolve");
			while (true)
			{
				std::unique_lock<std::mutex> lk(cv_m);
				if (cv.wait_for(lk, reresolve * 1s, []{ return stop; }))
					break;
				else
				{
					m.lock();
					std::string path__ = path;
					std::string port__ = port;
					std::string address__ = address;
					m.unlock();
					std::vector<int> sockets_;
					if (create_sockets(path__, port__, address__, sockets_))
					{
						std::lock_guard<std::mutex> lg(m);
						for (int socket : sockets)
							close(socket);
						sockets = sockets_;
					}
				}
			}
		});
	}

	return true;
}

HALON_EXPORT
void Halon_config_reload(HalonConfig* app)
{
	std::string path_;
	std::string port_;
	std::string address_;
	bool badhost_ = false;
	int timeout_ = 5000;

	const char* a = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "path"), nullptr);
	if (a) path_ = a;
	const char* b = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "port"), nullptr);
	if (b) port_ = b;
	const char* c = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "address"), nullptr);
	if (c) address_ = c;
	const char* d = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "badhost"), nullptr);
	if (d) badhost_ = (strcmp(d, "true") == 0 || strcmp(d, "TRUE") == 0) ? true : false;
	const char* e = HalonMTA_config_string_get(HalonMTA_config_object_get(app, "timeout"), nullptr);
	if (e) timeout_ = strtoul(e, nullptr, 10);

	std::vector<int> sockets_;
	if (create_sockets(path_, port_, address_, sockets_))
	{
		std::lock_guard<std::mutex> lg(m);
		for (int socket : sockets)
			close(socket);
		sockets = sockets_;
		path = path_;
		port = port_;
		address = address_;
		badhost = badhost_;
		timeout = timeout_;
	}
}

HALON_EXPORT
void Halon_cleanup()
{
	for (int socket : sockets)
		close(socket);
	if (reresolve)
	{
		cv_m.lock();
		stop = true;
		cv_m.unlock();
		cv.notify_all();
		p.join();
	}
}

void rate(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	HalonHSLValue* a;

	char* namespace_ = nullptr;
	a = HalonMTA_hsl_argument_get(args, 0);
	if (a && HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_STRING)
		HalonMTA_hsl_value_get(a, HALONMTA_HSL_TYPE_STRING, &namespace_, nullptr);
	else
	{
		HalonHSLValue* x = HalonMTA_hsl_throw(hhc);
 		HalonMTA_hsl_value_set(x, HALONMTA_HSL_TYPE_EXCEPTION, "Invalid or missing namespace parameter", 0);
		return;
	}

	char* entry = nullptr;
	a = HalonMTA_hsl_argument_get(args, 1);
	if (a && HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_STRING)
		HalonMTA_hsl_value_get(a, HALONMTA_HSL_TYPE_STRING, &entry, nullptr);
	else
	{
		HalonHSLValue* x = HalonMTA_hsl_throw(hhc);
 		HalonMTA_hsl_value_set(x, HALONMTA_HSL_TYPE_EXCEPTION, "Invalid or missing entry parameter", 0);
		return;
	}

	double count_;
	char* count_char = nullptr;
	a = HalonMTA_hsl_argument_get(args, 2);
	if (a && HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_NUMBER)
		HalonMTA_hsl_value_get(a, HALONMTA_HSL_TYPE_NUMBER, &count_, nullptr);
	else if (a && HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_STRING)
	{
		HalonMTA_hsl_value_get(a, HALONMTA_HSL_TYPE_STRING, &count_char, nullptr);
		count_ = atof(count_char);
	}
	else
	{
		HalonHSLValue* x = HalonMTA_hsl_throw(hhc);
 		HalonMTA_hsl_value_set(x, HALONMTA_HSL_TYPE_EXCEPTION, "Invalid or missing count parameter", 0);
		return;
	}

	double interval;
	char* interval_char = nullptr;
	a = HalonMTA_hsl_argument_get(args, 3);
	if (a && HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_NUMBER)
		HalonMTA_hsl_value_get(a, HALONMTA_HSL_TYPE_NUMBER, &interval, nullptr);
	else if (a && HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_STRING)
	{
		HalonMTA_hsl_value_get(a, HALONMTA_HSL_TYPE_STRING, &interval_char, nullptr);
		interval = atof(interval_char);
	}
	else
	{
		HalonHSLValue* x = HalonMTA_hsl_throw(hhc);
 		HalonMTA_hsl_value_set(x, HALONMTA_HSL_TYPE_EXCEPTION, "Invalid or missing interval parameter", 0);
		return;
	}

	bool sync = true;
	a = HalonMTA_hsl_argument_get(args, 4);
	if (a)
	{
		if (HalonMTA_hsl_value_type(a) == HALONMTA_HSL_TYPE_ARRAY)
		{
			HalonHSLValue *v = HalonMTA_hsl_value_array_find(a, "sync");
			if (v)
			{
				if (HalonMTA_hsl_value_type(v) == HALONMTA_HSL_TYPE_BOOLEAN)
					HalonMTA_hsl_value_get(v, HALONMTA_HSL_TYPE_BOOLEAN, &sync, nullptr);
				else
				{
					HalonHSLValue* x = HalonMTA_hsl_throw(hhc);
					HalonMTA_hsl_value_set(x, HALONMTA_HSL_TYPE_EXCEPTION, "Invalid sync option", 0);
					return;
				}
			}
		}
		else
		{
			HalonHSLValue* x = HalonMTA_hsl_throw(hhc);
			HalonMTA_hsl_value_set(x, HALONMTA_HSL_TYPE_EXCEPTION, "Invalid options parameter", 0);
			return;
		}
	}

	std::lock_guard<std::mutex> lg(m);

	if (sockets.empty() && port.empty())
		create_sockets(path, port, address, sockets);

	if (sockets.empty())
	{
		syslog(LOG_CRIT, "rate(socket): bad path or address");
		return;
	}

	static size_t i = 0;
	int udp_internal = sockets[++i % sockets.size()]; 

	size_t c = (int)round(count_);
	time_t x = (int)round(interval);
	static uint32_t cookie = 0;
	if (c > 0)
	{
		struct rate_packet_hit hit;
		hit.version = 1;
		hit.type = sync ? RATE_HIT : RATE_HIT_NOSYNC;
		hit.cookie = cookie++;
		hit.time = htonl(x);
		hit.count = htonl(c);
		strncpy(hit.ns, namespace_, sizeof hit.ns);
		strncpy(hit.entry, entry, sizeof hit.entry);
		ssize_t r = send(udp_internal, &hit, sizeof hit, 0);
		if (r != sizeof hit)
		{
			syslog(LOG_ERR, "rate(send): %m");
			if (port.empty()) {
				for (int socket : sockets)
					close(socket);
				sockets.clear();
			}
			if (!port.empty() && badhost && reresolve && sockets.size() > 1)
			{
				sockets.erase(std::remove(sockets.begin(), sockets.end(), udp_internal), sockets.end());
				close(udp_internal);
			}
			return;
		}

		do {
			pollfd pfd;
			pfd.fd = udp_internal;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, timeout) != 1 || !(pfd.revents & POLLIN))
			{
				syslog(LOG_ERR, "rate(recv): timed out");
				if (!port.empty() && badhost && reresolve && sockets.size() > 1)
				{
					sockets.erase(std::remove(sockets.begin(), sockets.end(), udp_internal), sockets.end());
					close(udp_internal);
				}
				return;
			}

			struct rate_packet_hit_reply reply;
			r = recv(udp_internal, (void*)&reply, sizeof reply, 0);
			if (r != sizeof reply)
			{
				syslog(LOG_ERR, "rate(recv): %m");
				if (!port.empty() && badhost && reresolve && sockets.size() > 1)
				{
					sockets.erase(std::remove(sockets.begin(), sockets.end(), udp_internal), sockets.end());
					close(udp_internal);
				}
				return;
			}

			if (reply.cookie == hit.cookie)
			{
				HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_BOOLEAN, &reply.allowed, 0);
				return;
			}
		} while (true);
	}
	else
	{
		struct rate_packet_count count;
		count.version = 1;
		count.type = RATE_COUNT;
		count.cookie = cookie++;
		count.time = htonl(x);
		strncpy(count.ns, namespace_, sizeof count.ns);
		strncpy(count.entry, entry, sizeof count.entry);
		ssize_t r = send(udp_internal, &count, sizeof count, 0);
		if (r != sizeof count)
		{
			syslog(LOG_ERR, "rate(send): failed");
			if (port.empty()) {
				for (int socket : sockets)
					close(socket);
				sockets.clear();
			}
			if (!port.empty() && badhost && reresolve && sockets.size() > 1)
			{
				sockets.erase(std::remove(sockets.begin(), sockets.end(), udp_internal), sockets.end());
				close(udp_internal);
			}
			return;
		}

		do {
			pollfd pfd;
			pfd.fd = udp_internal;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, timeout) != 1 || !(pfd.revents & POLLIN))
			{
				syslog(LOG_ERR, "rate(recv): timed out");
				if (!port.empty() && badhost && reresolve && sockets.size() > 1)
				{
					sockets.erase(std::remove(sockets.begin(), sockets.end(), udp_internal), sockets.end());
					close(udp_internal);
				}
				return;
			}

			struct rate_packet_count_reply reply;
			r = recv(udp_internal, (void*)&reply, sizeof reply, 0);
			if (r != sizeof reply)
			{
				syslog(LOG_ERR, "rate(recv): %m");
				if (!port.empty() && badhost && reresolve && sockets.size() > 1)
				{
					sockets.erase(std::remove(sockets.begin(), sockets.end(), udp_internal), sockets.end());
					close(udp_internal);
				}
				return;
			}

			if (reply.cookie == count.cookie)
			{
				double d = reply.count;
				HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_NUMBER, &d, 0);
				return;
			}
		} while (true);
	}
}

HALON_EXPORT
bool Halon_hsl_register(HalonHSLRegisterContext* ptr)
{
	HalonMTA_hsl_register_function(ptr, "rate", &rate);
	HalonMTA_hsl_module_register_function(ptr, "rate", &rate);
	return true;
}
