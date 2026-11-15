#include "net.h"

#include <dlfcn.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "be.h"

/* ---- lanLib -------------------------------------------------------------------- */

enum {
	L_INIT,                 /* InitializeLotsOStuff(void *) -> 1 if ready */
	L_HAL_INIT,             /* hal_init(): the same (network booting's way in) */
	L_HAL_SHUTDOWN,
	L_GET_ADDR,             /* hal_get_enet_addr(UInt8 address[6]) */
	L_WRITE,                /* hal_enet_write(void *frame, UInt16 length) */
	L_READ,                 /* hal_enet_read: network booting's polled receive (none here) */
	L_RESTART,              /* gExportedData[1]: restart receiving */
	L_CONTROL,              /* gExportedData[2](op): shut down */
	L_FILTER,               /* gExportedData[3](addresses, ...): receive filter */
	L_LINK,                 /* gExportedData[4](request): link speed and duplex */
	L_TIMER,                /* receiving without an interrupt: a secondary interrupt handler, on a timer */
	L_INTERRUPT,            /* the receive DMA channel's interrupt handler */
	L_RECEIVE,              /* receiving: the secondary interrupt handler it queues */
	L_COUNT
};

enum { IMP_SET_TIMER, IMP_GET_INT, IMP_INSTALL_INT, IMP_QUEUE_SECONDARY, IMP_PROPERTY_GET, IMP_COUNT };

static const struct ndrv_import lanlib_imports[IMP_COUNT] = {
	[IMP_SET_TIMER] = { "DriverServicesLib", "SetInterruptTimer" },
	[IMP_GET_INT] = { "DriverServicesLib", "GetInterruptFunctions" },
	[IMP_INSTALL_INT] = { "DriverServicesLib", "InstallInterruptFunctions" },
	[IMP_QUEUE_SECONDARY] = { "DriverServicesLib", "QueueSecondaryInterruptHandler" },
	[IMP_PROPERTY_GET] = { "NameRegistryLib", "RegistryPropertyGet" },
};

/* lanLib's own data (after the generated part of its data section) */
enum {
	X_EXPORTED = 0x00,      /* gExportedData: 5 words */
	X_EXPIRATION = 0x18,    /* AbsoluteTime for SetInterruptTimer */
	X_TIMER_ID = 0x20,
	X_IST_SIZE = 0x24,      /* RegistryPropertyGet's size (in and out) */
	X_RX_DESC = 0x30,       /* what the driver's receive routine gets: data, -, length (with FCS), status */
	X_IST = 0x40,           /* the node's driver-ist: (set, member) of each interrupt */
	X_FUNCS = 0x58,         /* the receive member's refCon, handler, enabler, disabler */
	X_IST_NAME = 0x68,
	X_RX_BUF = 0x80,
	X_SIZE = X_RX_BUF + NET_FRAME_MAX + 4 + 12,
};

/* Where lanLib is in a call (state word 0 across callouts; word 1 holds the entry) */
enum { S_TIMER = 1, S_RECEIVING, S_IST, S_FUNCTIONS, S_INSTALL, S_ENABLE, S_QUEUED };

static const struct ndrv_export lanlib_exports[] = {
	{ .name = "InitializeLotsOStuff", .entry = L_INIT },
	{ .name = "hal_init", .entry = L_HAL_INIT },
	{ .name = "hal_shutdown", .entry = L_HAL_SHUTDOWN },
	{ .name = "hal_get_enet_addr", .entry = L_GET_ADDR },
	{ .name = "hal_enet_write", .entry = L_WRITE },
	{ .name = "hal_enet_read", .entry = L_READ },
	{ .name = "gExportedData", .data = true, .offset = X_EXPORTED },
};

size_t net_build_lanlib(struct net *n, uint8_t *out, size_t size)
{
	static uint8_t extra[X_SIZE];
	memcpy(extra + X_IST_NAME, "driver-ist", sizeof "driver-ist");
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = L_COUNT,
		.init_entry = -1,
		.exports = lanlib_exports,
		.export_count = sizeof lanlib_exports / sizeof lanlib_exports[0],
		.hostcall = HOSTCALL_LANLIB,
		.imports = lanlib_imports,
		.import_count = IMP_COUNT,
		.extra = extra,
		.extra_len = sizeof extra,
	};
	return ndrv_build(&spec, out, size, &n->layout);
}

void net_init(struct net *n, struct mem *mem, const uint8_t mac[6], uint32_t timebase_hz)
{
	memset(n, 0, sizeof *n);
	n->mem = mem;
	n->timebase_hz = timebase_hz;
	memcpy(n->mac, mac, 6);
}

void net_input(struct net *n, const uint8_t *data, size_t len)
{
	if (len < 14 || len > NET_FRAME_MAX || n->count == NET_QUEUE) {
		n->dropped++;
		return;
	}
	struct net_frame *f = &n->queue[(n->head + n->count++) % NET_QUEUE];
	memcpy(f->data, data, len);
	if (len < 60) {                 /* (the minimum frame, as a controller would deliver it) */
		memset(f->data + len, 0, 60 - len);
		len = 60;
	}
	f->len = (uint16_t)len;
	if (n->irq && n->ready)
		n->kick = true;
}

static void backend_send(struct net_backend *b, const uint8_t *frame, size_t len);

/*
 * Receiving: lanLib's initialization installs an interrupt handler for the
 * receive DMA channel in the Ethernet node's interrupt set (driver-ist), and
 * a frame from the host pulses that interrupt (`kick`). The handler queues a
 * secondary interrupt handler, which hands each queued frame to the driver's
 * receive routine. Without an interrupt set, a timer polls instead: every
 * NET_POLL_FAST_MS while there has been traffic in the last second, and
 * every NET_POLL_SLOW_MS otherwise.
 */
#define NET_POLL_FAST_MS 2u
#define NET_POLL_SLOW_MS 10u

static void arm_timer(struct net *n, struct ndrv_call *c, uint32_t x)
{
	bool busy = c->tb - n->traffic_tb < n->timebase_hz;
	uint64_t ms = busy ? NET_POLL_FAST_MS : NET_POLL_SLOW_MS;
	mem_w64(n->mem, x + X_EXPIRATION, c->tb + n->timebase_hz * ms / 1000);
	ndrv_set_state(c, 0, S_TIMER);
	ndrv_callout(c, ndrv_import(c, &n->layout, IMP_SET_TIMER), x + X_EXPIRATION, c->toc + n->layout.entry[L_TIMER],
		     0, x + X_TIMER_ID);
}

/* Each queued frame to the driver's receive routine; then the timer again, if it's polling */
static void receive_step(struct net *n, struct ndrv_call *c, uint32_t x)
{
	struct mem *m = n->mem;
	uint32_t receive = mem_r32(m, x + X_EXPORTED);
	if (!n->ready) {                /* shut down: the timer stops */
		ndrv_finish(c, 0);
		return;
	}
	while (n->count) {
		struct net_frame *f = &n->queue[n->head];
		n->head = (n->head + 1) % NET_QUEUE;
		n->count--;
		if (!receive || !mem_ok(m, receive, 8, MEM_R)) {
			n->dropped++;
			continue;
		}
		memcpy(mem_ptr(m, x + X_RX_BUF), f->data, f->len);
		memset(mem_ptr(m, x + X_RX_BUF + f->len), 0, 4);       /* (a frame check sequence) */
		mem_w32(m, x + X_RX_DESC, x + X_RX_BUF);
		mem_w32(m, x + X_RX_DESC + 4, 0);
		mem_w32(m, x + X_RX_DESC + 8, f->len + 4u);
		mem_w32(m, x + X_RX_DESC + 12, 0);
		n->received++;
		n->bytes_received += f->len;
		n->traffic_tb = c->tb;
		ndrv_set_state(c, 0, S_RECEIVING);
		ndrv_callout(c, receive, x + X_RX_DESC, 0, 0, 0);
		return;
	}
	if (n->irq)
		ndrv_finish(c, 0);
	else
		arm_timer(n, c, x);
}

/* Setting up the receive interrupt, after each callout (or polling, if that fails) */
static void interrupt_step(struct net *n, struct ndrv_call *c, uint32_t x)
{
	struct mem *m = n->mem;
	uint32_t ist = x + X_IST + 8 * NET_IST_RX, set = mem_r32(m, ist), member = mem_r32(m, ist + 4);
	uint32_t status = c->arg[0];

	switch (ndrv_state(c, 0)) {
	case S_IST:
		if (status || mem_r32(m, x + X_IST_SIZE) < 8 * (NET_IST_RX + 1))
			break;
		ndrv_set_state(c, 0, S_FUNCTIONS);
		ndrv_callout(c, ndrv_import(c, &n->layout, IMP_GET_INT), set, member, x + X_FUNCS, x + X_FUNCS + 4);
		c->out[4] = x + X_FUNCS + 8;
		c->out[5] = x + X_FUNCS + 12;
		return;
	case S_FUNCTIONS:               /* ours, keeping the set's refCon, enabler and disabler */
		if (status)
			break;
		ndrv_set_state(c, 0, S_INSTALL);
		ndrv_callout(c, ndrv_import(c, &n->layout, IMP_INSTALL_INT), set, member, mem_r32(m, x + X_FUNCS),
			     c->toc + n->layout.entry[L_INTERRUPT]);
		c->out[4] = mem_r32(m, x + X_FUNCS + 8);
		c->out[5] = mem_r32(m, x + X_FUNCS + 12);
		return;
	case S_INSTALL:
		if (status)
			break;
		if (n->log)
			printf("net: receiving by interrupt\n");
		n->irq = true;
		n->kick = n->count > 0;
		if (!mem_r32(m, x + X_FUNCS + 8)) {
			ndrv_finish(c, 1);
			return;
		}
		ndrv_set_state(c, 0, S_ENABLE);
		ndrv_callout(c, mem_r32(m, x + X_FUNCS + 8), set, member, mem_r32(m, x + X_FUNCS), 0);
		return;
	default:                        /* S_ENABLE */
		ndrv_finish(c, 1);
		return;
	}
	if (n->log)
		printf("net: no receive interrupt (step %u, status %d): polling\n", ndrv_state(c, 0), (int32_t)status);
	arm_timer(n, c, x);
}

void net_call(struct net *n, struct ndrv_call *c)
{
	struct mem *m = n->mem;
	const uint32_t *a = c->arg;
	uint32_t x = c->toc + n->layout.extra;

	if (c->entry == NDRV_ENTRY_RESUME) {
		switch (ndrv_state(c, 0)) {
		case S_RECEIVING:                       /* the receive routine returned: the next frame */
			receive_step(n, c, x);
			return;
		case S_TIMER:                           /* SetInterruptTimer returned */
			ndrv_finish(c, ndrv_state(c, 1) == L_TIMER ? 0 : 1);
			return;
		case S_QUEUED:                          /* the interrupt is handled (kIsrIsComplete) */
			if (a[0])
				n->queued = false;
			ndrv_finish(c, 0);
			return;
		default:
			interrupt_step(n, c, x);
			return;
		}
	}
	if (n->log && c->entry != L_WRITE && c->entry < L_TIMER)
		printf("net: lanLib %u(%08x %08x %08x %08x)\n", c->entry, a[0], a[1], a[2], a[3]);
	ndrv_set_state(c, 1, c->entry);
	switch (c->entry) {
	case L_INIT: case L_HAL_INIT:
		for (unsigned k = 1; k <= 4; k++)       /* (word 0 is the driver's) */
			mem_w32(m, x + X_EXPORTED + 4 * k, c->toc + n->layout.entry[L_RESTART + k - 1]);
		if (n->ready || n->irq) {               /* (the interrupt handler stays installed) */
			n->ready = true;
			ndrv_finish(c, 1);
			return;
		}
		n->ready = true;
		if (c->entry == L_INIT && mem_ok(m, a[0], 16, MEM_R)) {        /* (the node's RegEntryID) */
			mem_w32(m, x + X_IST_SIZE, 24);
			ndrv_set_state(c, 0, S_IST);
			ndrv_callout(c, ndrv_import(c, &n->layout, IMP_PROPERTY_GET), a[0], x + X_IST_NAME, x + X_IST,
				     x + X_IST_SIZE);
			return;
		}
		arm_timer(n, c, x);
		return;
	case L_GET_ADDR:
		if (mem_ok(m, a[0], 6, MEM_W))
			memcpy(mem_ptr(m, a[0]), n->mac, 6);
		ndrv_finish(c, 0);
		return;
	case L_WRITE: {
		uint32_t len = a[1] & 0xFFFF;
		if (len >= 14 && len <= NET_FRAME_MAX && mem_ok(m, a[0], len, MEM_R)) {
			n->sent++;
			n->bytes_sent += len;
			n->traffic_tb = c->tb;
			if (n->backend)
				backend_send(n->backend, mem_ptr(m, a[0]), len);
		}
		ndrv_finish(c, 0);
		return;
	}
	case L_LINK:                    /* request 1: the modes we have (10/100, half/full duplex) */
		if (mem_ok(m, a[0], 12, MEM_W) && mem_r32(m, a[0]) == 1) {
			mem_w32(m, a[0] + 4, 0xF);
			mem_w32(m, a[0] + 8, 0xF);
		}
		ndrv_finish(c, 0);
		return;
	case L_CONTROL: case L_HAL_SHUTDOWN:
		n->ready = false;
		ndrv_finish(c, 0);
		return;
	case L_TIMER:
		receive_step(n, c, x);
		return;
	case L_INTERRUPT:               /* (at hardware interrupt level) */
		if (n->queued) {
			ndrv_finish(c, 0);
			return;
		}
		n->queued = true;
		ndrv_set_state(c, 0, S_QUEUED);
		ndrv_callout(c, ndrv_import(c, &n->layout, IMP_QUEUE_SECONDARY), c->toc + n->layout.entry[L_RECEIVE], 0, 0, 0);
		return;
	case L_RECEIVE:
		n->queued = false;
		receive_step(n, c, x);
		return;
	default:                        /* restarting, filters: nothing to do */
		ndrv_finish(c, 0);
		return;
	}
}

/* ---- the host side: libslirp ------------------------------------------------------ */

/* libslirp's API (libslirp.h, version 1 of its configuration) */
struct slirp_config {
	uint32_t version;
	int restricted;
	bool in_enabled;
	struct in_addr vnetwork, vnetmask, vhost;
	bool in6_enabled;
	struct in6_addr vprefix_addr6;
	uint8_t vprefix_len;
	struct in6_addr vhost6;
	const char *vhostname, *tftp_server_name, *tftp_path, *bootfile;
	struct in_addr vdhcp_start, vnameserver;
	struct in6_addr vnameserver6;
	const char **vdnssearch;
	const char *vdomainname;
	size_t if_mtu, if_mru;
	bool disable_host_loopback, enable_emu;
};

typedef void (*slirp_timer_cb)(void *opaque);

struct slirp_cb {
	ssize_t (*send_packet)(const void *buf, size_t len, void *opaque);
	void (*guest_error)(const char *msg, void *opaque);
	int64_t (*clock_get_ns)(void *opaque);
	void *(*timer_new)(slirp_timer_cb cb, void *cb_opaque, void *opaque);
	void (*timer_free)(void *timer, void *opaque);
	void (*timer_mod)(void *timer, int64_t expire_ms, void *opaque);
	void (*register_poll_fd)(int fd, void *opaque);
	void (*unregister_poll_fd)(int fd, void *opaque);
	void (*notify)(void *opaque);
};

enum { SLIRP_POLL_IN = 1, SLIRP_POLL_OUT = 2, SLIRP_POLL_PRI = 4, SLIRP_POLL_ERR = 8, SLIRP_POLL_HUP = 16 };

#define MAX_TIMERS 8
#define MAX_FDS 256

struct net_timer {
	bool used, armed;
	int64_t expire_ms;
	slirp_timer_cb cb;
	void *cb_opaque;
};

struct net_backend {
	struct net *net;
	void *lib, *slirp;
	void *(*slirp_new)(const struct slirp_config *, const struct slirp_cb *, void *);
	void (*slirp_input)(void *, const uint8_t *, int);
	void (*pollfds_fill)(void *, uint32_t *, int (*)(int, int, void *), void *);
	void (*pollfds_poll)(void *, int, int (*)(int, void *), void *);
	void (*slirp_cleanup)(void *);
	struct slirp_cb cb;
	struct net_timer timers[MAX_TIMERS];
	struct pollfd fds[MAX_FDS];
	unsigned nfds;
};

static int64_t clock_ns(void *opaque)
{
	(void)opaque;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
	struct net_backend *b = opaque;
	net_input(b->net, buf, len);
	return (ssize_t)len;
}

static void cb_guest_error(const char *msg, void *opaque)
{
	(void)opaque;
	fprintf(stderr, "net: %s\n", msg);
}

static void *cb_timer_new(slirp_timer_cb cb, void *cb_opaque, void *opaque)
{
	struct net_backend *b = opaque;
	for (unsigned k = 0; k < MAX_TIMERS; k++)
		if (!b->timers[k].used) {
			b->timers[k] = (struct net_timer){ true, false, 0, cb, cb_opaque };
			return &b->timers[k];
		}
	return NULL;
}

static void cb_timer_free(void *timer, void *opaque)
{
	(void)opaque;
	if (timer)
		((struct net_timer *)timer)->used = false;
}

static void cb_timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
	(void)opaque;
	if (timer) {
		((struct net_timer *)timer)->expire_ms = expire_ms;
		((struct net_timer *)timer)->armed = true;
	}
}

static void cb_poll_fd(int fd, void *opaque)
{
	(void)fd;
	(void)opaque;
}

static void cb_notify(void *opaque)
{
	(void)opaque;
}

static int cb_add_poll(int fd, int events, void *opaque)
{
	struct net_backend *b = opaque;
	if (b->nfds == MAX_FDS)
		return -1;
	short ev = 0;
	if (events & SLIRP_POLL_IN)
		ev |= POLLIN;
	if (events & SLIRP_POLL_OUT)
		ev |= POLLOUT;
	if (events & SLIRP_POLL_PRI)
		ev |= POLLPRI;
	b->fds[b->nfds] = (struct pollfd){ .fd = fd, .events = ev };
	return (int)b->nfds++;
}

static int cb_get_revents(int idx, void *opaque)
{
	struct net_backend *b = opaque;
	if (idx < 0 || (unsigned)idx >= b->nfds)
		return 0;
	short r = b->fds[idx].revents;
	return (r & POLLIN ? SLIRP_POLL_IN : 0) | (r & POLLOUT ? SLIRP_POLL_OUT : 0) |
	       (r & POLLPRI ? SLIRP_POLL_PRI : 0) | (r & POLLERR ? SLIRP_POLL_ERR : 0) |
	       (r & POLLHUP ? SLIRP_POLL_HUP : 0);
}

static void backend_send(struct net_backend *b, const uint8_t *frame, size_t len)
{
	b->slirp_input(b->slirp, frame, (int)len);
}

static struct in_addr ipv4(uint32_t a)
{
	return (struct in_addr){ htonl(a) };
}

bool net_start(struct net *n)
{
	struct net_backend *b = calloc(1, sizeof *b);
	if (!b)
		return false;
	b->net = n;
	/* (bare names aren't looked for in Homebrew's or MacPorts' directories on macOS) */
	static const char *const names[] = { "libslirp.so.0", "libslirp.dylib", "/opt/homebrew/lib/libslirp.dylib",
					     "/opt/local/lib/libslirp.dylib" };
	for (size_t k = 0; k < sizeof names / sizeof names[0] && !b->lib; k++)
		b->lib = dlopen(names[k], RTLD_NOW | RTLD_LOCAL);
	if (b->lib) {
		*(void **)&b->slirp_new = dlsym(b->lib, "slirp_new");
		*(void **)&b->slirp_input = dlsym(b->lib, "slirp_input");
		*(void **)&b->pollfds_fill = dlsym(b->lib, "slirp_pollfds_fill_socket");
		if (!b->pollfds_fill)
			*(void **)&b->pollfds_fill = dlsym(b->lib, "slirp_pollfds_fill");
		*(void **)&b->pollfds_poll = dlsym(b->lib, "slirp_pollfds_poll");
		*(void **)&b->slirp_cleanup = dlsym(b->lib, "slirp_cleanup");
	}
	if (!b->lib || !b->slirp_new || !b->slirp_input || !b->pollfds_fill || !b->pollfds_poll || !b->slirp_cleanup) {
		fprintf(stderr, "net: libslirp isn't available (%s); the guest's network has nothing behind it\n",
			b->lib ? "missing functions" : dlerror());
		if (b->lib)
			dlclose(b->lib);
		free(b);
		return false;
	}
	b->cb = (struct slirp_cb){
		.send_packet = cb_send_packet,
		.guest_error = cb_guest_error,
		.clock_get_ns = clock_ns,
		.timer_new = cb_timer_new,
		.timer_free = cb_timer_free,
		.timer_mod = cb_timer_mod,
		.register_poll_fd = cb_poll_fd,
		.unregister_poll_fd = cb_poll_fd,
		.notify = cb_notify,
	};
	struct slirp_config cfg = {
		.version = 1,
		.in_enabled = true,
		.vnetwork = ipv4(0x0A000200),           /* 10.0.2.0/24 */
		.vnetmask = ipv4(0xFFFFFF00),
		.vhost = ipv4(0x0A000202),              /* the gateway, 10.0.2.2 */
		.vhostname = "blue",
		.vdhcp_start = ipv4(0x0A00020F),        /* the guest, 10.0.2.15 */
		.vnameserver = ipv4(0x0A000203),        /* DNS, 10.0.2.3 */
	};
	b->slirp = b->slirp_new(&cfg, &b->cb, b);
	if (!b->slirp) {
		fprintf(stderr, "net: libslirp wouldn't start\n");
		dlclose(b->lib);
		free(b);
		return false;
	}
	n->backend = b;
	return true;
}

void net_poll(struct net *n, int timeout_ms)
{
	struct net_backend *b = n->backend;
	if (!b)
		return;
	uint32_t timeout = timeout_ms < 0 ? UINT32_MAX : (uint32_t)timeout_ms;
	b->nfds = 0;
	b->pollfds_fill(b->slirp, &timeout, cb_add_poll, b);           /* (which may shorten it) */
	int64_t now_ms = clock_ns(NULL) / 1000000;
	for (unsigned k = 0; k < MAX_TIMERS; k++) {
		const struct net_timer *t = &b->timers[k];
		if (t->used && t->armed && t->expire_ms - now_ms < (int64_t)timeout)
			timeout = t->expire_ms > now_ms ? (uint32_t)(t->expire_ms - now_ms) : 0;
	}
	int r = poll(b->fds, b->nfds, timeout == UINT32_MAX ? -1 : (int)timeout);
	b->pollfds_poll(b->slirp, r < 0, cb_get_revents, b);
	now_ms = clock_ns(NULL) / 1000000;
	for (unsigned k = 0; k < MAX_TIMERS; k++) {
		struct net_timer *t = &b->timers[k];
		if (t->used && t->armed && t->expire_ms <= now_ms) {
			t->armed = false;
			t->cb(t->cb_opaque);
		}
	}
}

void net_free(struct net *n)
{
	struct net_backend *b = n->backend;
	if (b) {
		b->slirp_cleanup(b->slirp);
		dlclose(b->lib);
		free(b);
		n->backend = NULL;
	}
}
