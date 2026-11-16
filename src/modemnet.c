#include "modemnet.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0                  /* (macOS: SO_NOSIGPIPE instead) */
#endif

#define CONNECT_TIMEOUT_MS 60000
#define OUT_MAX (1u << 20)              /* what waits to be sent, at most */

/* A call going out: the thread resolves the name and connects; whoever finishes with it last frees it */
struct modemnet_dial {
	char host[MODEM_ADDRESS_MAX];
	unsigned port;
	int fd;                         /* the connection (-1: none) ... */
	bool refused;                   /* ... refused: the line's busy */
	atomic_int done;
	atomic_int refs;
	atomic_int cancelled;
};

static void dial_release(struct modemnet_dial *d)
{
	if (atomic_fetch_sub(&d->refs, 1) == 1) {
		if (d->fd >= 0)
			close(d->fd);
		free(d);
	}
}

static void no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
	(void)fd;
#endif
}

static int connect_to(const struct addrinfo *a, bool *refused)
{
	int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
	if (fd < 0)
		return -1;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
		return fd;
	if (errno == EINPROGRESS) {
		struct pollfd p = { .fd = fd, .events = POLLOUT };
		int err = 0;
		socklen_t len = sizeof err;
		if (poll(&p, 1, CONNECT_TIMEOUT_MS) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0) {
			if (!err)
				return fd;
			*refused |= err == ECONNREFUSED;
		}
	} else {
		*refused |= errno == ECONNREFUSED;
	}
	close(fd);
	return -1;
}

static void *dial_thread(void *arg)
{
	struct modemnet_dial *d = arg;
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
	char port[8];
	snprintf(port, sizeof port, "%u", d->port);
	if (getaddrinfo(d->host, port, &hints, &res) == 0) {
		for (const struct addrinfo *a = res; a && d->fd < 0 && !atomic_load(&d->cancelled); a = a->ai_next)
			d->fd = connect_to(a, &d->refused);
		freeaddrinfo(res);
	}
	atomic_store(&d->done, 1);
	dial_release(d);
	return NULL;
}

void modemnet_init(struct modemnet *n, struct modem *m)
{
	memset(n, 0, sizeof *n);
	n->modem = m;
	n->listen_fd = n->fd = -1;
}

bool modemnet_listen(struct modemnet *n, const char *spec, char *err, size_t err_size)
{
	char host[MODEM_ADDRESS_MAX] = "";
	const char *colon = strrchr(spec, ':');
	const char *port = colon ? colon + 1 : spec;
	if (colon) {
		snprintf(host, sizeof host, "%.*s", (int)(colon - spec), spec);
		if (host[0] == '[' && host[strlen(host) - 1] == ']') {
			memmove(host, host + 1, strlen(host));
			host[strlen(host) - 1] = '\0';
		}
	}
	char *end;
	unsigned long p = strtoul(port, &end, 10);
	if (!*port || *end || p > 65535) {
		snprintf(err, err_size, "\"%s\" isn't a port (or ADDRESS:PORT)", spec);
		return false;
	}
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
				  .ai_flags = AI_PASSIVE }, *res = NULL;
	int e = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
	if (e) {
		snprintf(err, err_size, "%s: %s", host, gai_strerror(e));
		return false;
	}
	int fd = -1;
	for (const struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
			continue;
		int one = 1, zero = 0;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (a->ai_family == AF_INET6 && !host[0])      /* (IPv4 too) */
			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
		if (bind(fd, a->ai_addr, a->ai_addrlen) || listen(fd, 4)) {
			snprintf(err, err_size, "port %s: %s", port, strerror(errno));
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	if (fd < 0)
		return false;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	n->listen_fd = fd;
	struct sockaddr_storage a;
	socklen_t len = sizeof a;
	if (getsockname(fd, (struct sockaddr *)&a, &len) == 0)       /* (port 0: the one the system chose) */
		p = ntohs(a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port :
			  ((struct sockaddr_in *)&a)->sin_port);
	n->port = (unsigned)p;
	if (host[0])
		snprintf(n->listening, sizeof n->listening, "%s port %lu", host, p);
	else
		snprintf(n->listening, sizeof n->listening, "port %lu", p);
	return true;
}

static void close_call(struct modemnet *n)
{
	if (n->fd >= 0)
		close(n->fd);
	n->fd = -1;
	n->incoming = n->up = false;
	n->out_len = 0;
}

static void cancel_dial(struct modemnet *n)
{
	if (n->dial) {
		atomic_store(&n->dial->cancelled, 1);
		dial_release(n->dial);
		n->dial = NULL;
	}
}

bool modemnet_dial(void *ctx, const char *host, unsigned port)
{
	struct modemnet *n = ctx;
	cancel_dial(n);
	close_call(n);
	struct modemnet_dial *d = calloc(1, sizeof *d);
	if (!d)
		return false;
	snprintf(d->host, sizeof d->host, "%s", host);
	d->port = port;
	d->fd = -1;
	atomic_init(&d->done, 0);
	atomic_init(&d->refs, 2);
	atomic_init(&d->cancelled, 0);
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	bool ok = pthread_create(&t, &attr, dial_thread, d) == 0;
	pthread_attr_destroy(&attr);
	if (!ok) {
		free(d);
		return false;
	}
	n->dial = d;
	return true;
}

bool modemnet_answer(void *ctx)
{
	struct modemnet *n = ctx;
	if (n->fd < 0 || !n->incoming)
		return false;
	n->up = true;
	return true;
}

static void flush_out(struct modemnet *n)
{
	while (n->fd >= 0 && n->out_len) {
		ssize_t w = send(n->fd, n->out, n->out_len, MSG_NOSIGNAL);
		if (w <= 0)
			return;
		memmove(n->out, n->out + w, n->out_len - (size_t)w);
		n->out_len -= (size_t)w;
	}
}

void modemnet_send(void *ctx, const uint8_t *data, size_t len)
{
	struct modemnet *n = ctx;
	if (n->fd < 0 || !n->up)
		return;
	if (n->out_len + len > n->out_cap) {
		size_t cap = n->out_cap ? n->out_cap : 4096;
		while (cap < n->out_len + len)
			cap *= 2;
		if (cap > OUT_MAX)
			return;                 /* (the far end isn't taking it: it's lost) */
		uint8_t *o = realloc(n->out, cap);
		if (!o)
			return;
		n->out = o;
		n->out_cap = cap;
	}
	memcpy(n->out + n->out_len, data, len);
	n->out_len += len;              /* (sent at the next poll: a millisecond's characters at a time) */
}

void modemnet_hangup(void *ctx)
{
	struct modemnet *n = ctx;
	cancel_dial(n);
	flush_out(n);
	close_call(n);
}

static void peer_name(int fd, char *buf, size_t size)
{
	struct sockaddr_storage a;
	socklen_t len = sizeof a;
	char host[NI_MAXHOST], port[NI_MAXSERV];
	if (getpeername(fd, (struct sockaddr *)&a, &len) == 0 &&
	    getnameinfo((struct sockaddr *)&a, len, host, sizeof host, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
		const char *h = strncmp(host, "::ffff:", 7) == 0 ? host + 7 : host;
		snprintf(buf, size, strchr(h, ':') ? "[%s]:%s" : "%s:%s", h, port);
	} else {
		snprintf(buf, size, "somebody");
	}
}

static void call_fd(int fd)
{
	int one = 1;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	no_sigpipe(fd);
}

bool modemnet_poll(struct modemnet *n)
{
	struct modem *m = n->modem;
	bool heard = false;

	if (n->dial && atomic_load(&n->dial->done)) {
		struct modemnet_dial *d = n->dial;
		n->dial = NULL;
		if (d->fd >= 0) {
			n->fd = d->fd;
			d->fd = -1;
			call_fd(n->fd);
			n->up = true;
			n->incoming = false;
			modem_connected(m);
		} else {
			modem_failed(m, d->refused);
		}
		dial_release(d);
		heard = true;
	}

	if (n->listen_fd >= 0) {
		int fd;
		while ((fd = accept(n->listen_fd, NULL, NULL)) >= 0) {
			if (n->fd >= 0 || n->dial || m->call != MODEM_IDLE) {
				close(fd);              /* (a call's on: the line's busy) */
				continue;
			}
			call_fd(fd);
			n->fd = fd;
			n->incoming = true;
			n->up = false;
			char peer[MODEM_ADDRESS_MAX];
			peer_name(fd, peer, sizeof peer);
			modem_incoming(m, peer);
			heard = true;
		}
	}

	if (n->fd < 0)
		return heard;
	flush_out(n);
	if (!n->up) {                           /* ringing: did the caller give up? */
		uint8_t b;
		ssize_t r = recv(n->fd, &b, 1, MSG_PEEK);
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			close_call(n);
			modem_far_hangup(m);
			heard = true;
		}
		return heard;
	}
	uint8_t buf[4096];
	for (;;) {
		size_t room = modem_room(m);
		if (!room)
			break;
		ssize_t r = recv(n->fd, buf, room < sizeof buf ? room : sizeof buf, 0);
		if (r > 0) {
			modem_received(m, buf, (size_t)r);
			heard = true;
			continue;
		}
		if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			close_call(n);
			modem_far_hangup(m);
			heard = true;
		}
		break;
	}
	return heard;
}

bool modemnet_active(const struct modemnet *n)
{
	return n->listen_fd >= 0 || n->fd >= 0 || n->dial;
}

void modemnet_free(struct modemnet *n)
{
	cancel_dial(n);
	close_call(n);
	if (n->listen_fd >= 0)
		close(n->listen_fd);
	n->listen_fd = -1;
	free(n->out);
	n->out = NULL;
	n->out_len = n->out_cap = 0;
}
