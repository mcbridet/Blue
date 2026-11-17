#include "usbremote.h"

#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hostlib.h"
#include "usbproto.h"

#define HELLO_MS 15000                  /* how long the helper may take to say hello, once started */
#define OPEN_MS 20000                   /* ... to take a device */
#define QUIT_MS 3000                    /* ... to give its devices back as Blue quits */
#define SOCKET_BUFFER (256 * 1024)
#define ISO_RING 32                     /* isochronous packets received, waiting for Mac OS */
#define ISO_AHEAD 8                     /* ... asked for (or sent) ahead on an endpoint */
#define EP_INDEX(addr) (((addr) & 15) | ((addr) & USB_DIR_IN ? 16 : 0))

extern char **environ;

struct iso_ring {
	unsigned head, count, ahead;
	uint16_t len[ISO_RING];
	uint8_t data[ISO_RING][USBPROTO_ISO];
};

struct remotedev {
	struct usb_device dev;
	struct usbremote *r;
	struct usbhost_info info;
	uint8_t slot;                   /* the helper's number for it */
	atomic_bool gone;
	struct iso_ring *iso[32];       /* (index: EP_INDEX) */
};

/* A transfer with the helper */
struct rreq {
	uint32_t id;
	struct remotedev *rd;
	struct usb_xfer *x;             /* NULL: an isochronous packet's (its answer goes into the ring) */
	uint8_t ep;
	struct rreq *next;
};

struct usbremote {
	int fd;
	pid_t pid;
	char helper[PATH_MAX];
	pthread_t starter, reader;
	bool starting, reading;         /* (the threads started) */
	pthread_mutex_t lock;           /* all below, against the threads */
	pthread_cond_t cond;
	pthread_mutex_t wlock;          /* writing to the socket */
	enum usbremote_state state;
	bool abandoned;                 /* stopped while asking: the starting thread frees it */
	char why[USBPROTO_WHY];
	atomic_bool dead;               /* the helper's end has closed */
	struct remotedev *devs[USBPROTO_DEVICES];
	struct rreq *reqs;
	uint32_t next_id;
	uint32_t open_id;               /* the OPEN waiting for its answer ... */
	bool opened;                    /* ... which has come: */
	struct usbproto_msg open_msg;
	uint8_t open_data[USBPROTO_WHY];
	uint8_t buf[USBPROTO_DATA];     /* (the reader's) */
};

static const struct usb_device_ops remote_ops;

static struct remotedev *remote_of(const struct usb_device *d)
{
	return (struct remotedev *)d;
}

static uint32_t new_id(struct usbremote *r)
{
	if (++r->next_id == 0)
		r->next_id = 1;
	return r->next_id;
}

/* Unlinks a transfer (the lock held) and frees it */
static void forget(struct rreq **p)
{
	struct rreq *q = *p;
	*p = q->next;
	struct iso_ring *s = q->x ? NULL : q->rd->iso[EP_INDEX(q->ep)];
	if (s && s->ahead)
		s->ahead--;
	free(q);
}

/* (the lock held) */
static void forget_all(struct usbremote *r, struct remotedev *rd)
{
	for (struct rreq **p = &r->reqs; *p;) {
		if ((*p)->rd == rd)
			forget(p);
		else
			p = &(*p)->next;
	}
}

/* An answer (the lock held) */
static void answered(struct usbremote *r, const struct usbproto_msg *m)
{
	struct rreq **p = &r->reqs;
	while (*p && ((*p)->id != m->id || (*p)->rd->slot != m->dev))
		p = &(*p)->next;
	if (!*p)
		return;                 /* (given up) */
	struct rreq *q = *p;
	struct usb_xfer *x = q->x;
	if (x) {
		uint32_t n = m->len < x->len ? m->len : x->len;
		if (x->ep & USB_DIR_IN) {
			n = m->data < n ? m->data : n;
			memcpy(x->data, r->buf, n);
		}
		x->actual = n;
		x->status = (int)m->status;
		if (q->rd->dev.log)
			printf("usb: %s: endpoint %02X: %u bytes, status %d (through the helper)\n", q->rd->dev.name,
			       x->ep, n, x->status);
		atomic_store_explicit(&x->done, true, memory_order_release);
	} else if ((q->ep & USB_DIR_IN) && (m->status == USB_OK || m->status == USB_OVERRUN) && m->data) {
		struct iso_ring *s = q->rd->iso[EP_INDEX(q->ep)];
		if (s->count == ISO_RING) {     /* (Mac OS isn't keeping up: the oldest goes) */
			s->head = (s->head + 1) % ISO_RING;
			s->count--;
		}
		unsigned slot = (s->head + s->count++) % ISO_RING;
		s->len[slot] = (uint16_t)(m->data < USBPROTO_ISO ? m->data : USBPROTO_ISO);
		memcpy(s->data[slot], r->buf, s->len[slot]);
	}
	forget(p);
}

/* Reads the helper's answers, until its end closes */
static void *reader(void *arg)
{
	struct usbremote *r = arg;
	struct usbproto_msg m;
	while (usbproto_recv(r->fd, &m, r->buf) > 0) {
		const char *bad = usbproto_check_answer(&m);
		if (bad) {
			fprintf(stderr, "usb: Blue's USB helper said %s\n", bad);
			break;
		}
		pthread_mutex_lock(&r->lock);
		if (m.type == USBPROTO_DONE) {
			answered(r, &m);
		} else if (m.type == USBPROTO_OPENED && m.id == r->open_id && !r->opened) {
			r->open_msg = m;
			memcpy(r->open_data, r->buf, m.data);
			r->opened = true;
			pthread_cond_broadcast(&r->cond);
		} else if (m.type == USBPROTO_GONE && r->devs[m.dev]) {
			atomic_store(&r->devs[m.dev]->gone, true);
		}
		pthread_mutex_unlock(&r->lock);
	}
	pthread_mutex_lock(&r->lock);
	atomic_store(&r->dead, true);
	for (unsigned k = 0; k < USBPROTO_DEVICES; k++)
		if (r->devs[k])
			atomic_store(&r->devs[k]->gone, true);
	while (r->reqs) {
		struct usb_xfer *x = r->reqs->x;
		if (x) {
			x->actual = 0;
			x->status = USB_NODEV;
			atomic_store_explicit(&x->done, true, memory_order_release);
		}
		forget(&r->reqs);
	}
	pthread_cond_broadcast(&r->cond);
	pthread_mutex_unlock(&r->lock);
	return NULL;
}

/* A request, whole (or not at all: false) */
static bool send_msg(struct usbremote *r, const struct usbproto_msg *m, const void *data)
{
	if (atomic_load(&r->dead) || usbproto_check_request(m, data))
		return false;
	pthread_mutex_lock(&r->wlock);
	bool ok = usbproto_send(r->fd, m, data);
	pthread_mutex_unlock(&r->wlock);
	return ok;
}

/* Gives up transfer `id` that couldn't be sent; false if it's been answered all the same (the helper gone) */
static bool withdraw(struct usbremote *r, uint32_t id)
{
	pthread_mutex_lock(&r->lock);
	struct rreq **p = &r->reqs;
	while (*p && (*p)->id != id)
		p = &(*p)->next;
	bool found = *p != NULL;
	if (found)
		forget(p);
	pthread_mutex_unlock(&r->lock);
	return found;
}

/* Isochronous packets are answered at once: in, from those received (none yet: empty), each asking for another */
static int iso_packet(struct remotedev *rd, struct usb_xfer *x)
{
	struct usbremote *r = rd->r;
	bool in = x->ep & USB_DIR_IN;
	int status = USB_OK;
	if (!in)
		x->actual = x->len;     /* (sent, or dropped if too many are on their way) */
	if (!(x->ep & 15) || (!in && x->len > USBPROTO_ISO))
		return status;
	struct usbproto_msg m = { .type = USBPROTO_SUBMIT, .dev = rd->slot, .ep = x->ep, .xfer = USB_ISO };
	m.len = x->len < USBPROTO_ISO ? x->len : USBPROTO_ISO;
	m.data = in ? 0 : m.len;
	m.flags = rd->dev.log ? USBPROTO_LOG : 0;
	unsigned k = EP_INDEX(x->ep);
	pthread_mutex_lock(&r->lock);
	struct iso_ring *s = rd->iso[k];
	if (!s)
		s = rd->iso[k] = calloc(1, sizeof *s);
	if (s && in && s->count) {
		unsigned n = s->len[s->head];
		if (n > x->len) {
			n = x->len;
			status = USB_OVERRUN;
		}
		memcpy(x->data, s->data[s->head], n);
		x->actual = n;
		s->head = (s->head + 1) % ISO_RING;
		s->count--;
	}
	struct rreq *q = s && s->ahead < ISO_AHEAD ? calloc(1, sizeof *q) : NULL;
	if (q) {
		s->ahead++;
		q->id = m.id = new_id(r);
		q->rd = rd;
		q->ep = x->ep;
		q->next = r->reqs;
		r->reqs = q;
	}
	pthread_mutex_unlock(&r->lock);
	if (q && !send_msg(r, &m, x->data))
		withdraw(r, m.id);
	return status;
}

static void forget_iso(struct remotedev *rd)
{
	pthread_mutex_lock(&rd->r->lock);
	for (unsigned k = 0; k < 32; k++)
		if (rd->iso[k])
			rd->iso[k]->head = rd->iso[k]->count = 0;
	pthread_mutex_unlock(&rd->r->lock);
}

static int remote_submit(struct usb_device *d, struct usb_xfer *x)
{
	struct remotedev *rd = remote_of(d);
	struct usbremote *r = rd->r;
	x->actual = 0;
	if (atomic_load(&rd->gone) || atomic_load(&r->dead))
		return USB_NODEV;
	if (x->type == USB_ISO)
		return iso_packet(rd, x);
	struct usbproto_msg m = { .type = USBPROTO_SUBMIT, .dev = rd->slot, .ep = x->ep, .xfer = x->type, .len = x->len };
	m.flags = d->log ? USBPROTO_LOG : 0;
	if (x->type == USB_CONTROL) {
		if (x->ep & 15)
			return USB_STALL;
		uint16_t length = usb_w16(x->setup + 6);
		memcpy(m.setup, x->setup, 8);
		m.ep = x->setup[0] & USB_DIR_IN;
		m.len = m.ep || x->len > length ? length : x->len;
		unsigned req = x->setup[0] << 8 | x->setup[1];
		if (req == 0x0009 || req == 0x010B)     /* (SET_CONFIGURATION, SET_INTERFACE: streams start again) */
			forget_iso(rd);
	} else if (!(x->ep & 15) || x->len > USBPROTO_DATA) {
		return USB_STALL;
	}
	m.data = m.ep & USB_DIR_IN ? 0 : m.len;
	struct rreq *q = calloc(1, sizeof *q);
	if (!q)
		return USB_ERROR;
	pthread_mutex_lock(&r->lock);
	q->id = m.id = new_id(r);
	q->rd = rd;
	q->x = x;
	q->ep = x->ep;
	q->next = r->reqs;
	r->reqs = q;
	pthread_mutex_unlock(&r->lock);
	if (send_msg(r, &m, x->data))
		return USB_PENDING;
	return withdraw(r, m.id) ? USB_NODEV : USB_PENDING;
}

static void remote_cancel(struct usb_device *d, struct usb_xfer *x)
{
	struct remotedev *rd = remote_of(d);
	struct usbremote *r = rd->r;
	struct usbproto_msg m = { .type = USBPROTO_CANCEL, .dev = rd->slot };
	pthread_mutex_lock(&r->lock);
	for (struct rreq **p = &r->reqs; *p; p = &(*p)->next) {
		if ((*p)->x == x) {
			m.id = (*p)->id;
			forget(p);
			break;
		}
	}
	pthread_mutex_unlock(&r->lock);
	if (m.id)
		send_msg(r, &m, NULL);
}

static void remote_reset(struct usb_device *d)
{
	struct remotedev *rd = remote_of(d);
	struct usbremote *r = rd->r;
	pthread_mutex_lock(&r->lock);
	forget_all(r, rd);
	pthread_mutex_unlock(&r->lock);
	forget_iso(rd);
	struct usbproto_msg m = { .type = USBPROTO_RESET, .dev = rd->slot };
	send_msg(r, &m, NULL);
}

static void remote_destroy(struct usb_device *d)
{
	struct remotedev *rd = remote_of(d);
	struct usbremote *r = rd->r;
	pthread_mutex_lock(&r->lock);
	forget_all(r, rd);
	if (r->devs[rd->slot] == rd)
		r->devs[rd->slot] = NULL;
	pthread_mutex_unlock(&r->lock);
	struct usbproto_msg m = { .type = USBPROTO_CLOSE, .dev = rd->slot };
	send_msg(r, &m, NULL);
	for (unsigned k = 0; k < 32; k++)
		free(rd->iso[k]);
	free(rd);
}

static const struct usb_device_ops remote_ops = { remote_submit, remote_cancel, remote_reset, remote_destroy };

bool usbremote_is(const struct usb_device *d)
{
	return d->ops == &remote_ops;
}

const struct usbhost_info *usbremote_device_info(const struct usb_device *d)
{
	return &remote_of(d)->info;
}

bool usbremote_gone(const struct usb_device *d)
{
	return atomic_load(&remote_of(d)->gone);
}

struct usb_device *usbremote_open(struct usbremote *r, const char *spec, char *err, size_t size)
{
	if (usbremote_state(r, err, size) != USBREMOTE_READY)
		return NULL;
	struct usbproto_msg m = { .type = USBPROTO_OPEN, .data = (uint32_t)strlen(spec) };
	if (usbproto_check_request(&m, (const uint8_t *)spec)) {
		snprintf(err, size, "there's no USB device %s on the host", spec);
		return NULL;
	}
	struct remotedev *rd = calloc(1, sizeof *rd);
	if (!rd) {
		snprintf(err, size, "out of memory");
		return NULL;
	}
	pthread_mutex_lock(&r->lock);
	m.id = r->open_id = new_id(r);
	r->opened = false;
	pthread_mutex_unlock(&r->lock);
	bool sent = send_msg(r, &m, spec);
	struct timeval now;
	gettimeofday(&now, NULL);
	struct timespec until = { now.tv_sec + OPEN_MS / 1000, (long)now.tv_usec * 1000 };
	pthread_mutex_lock(&r->lock);
	while (sent && !r->opened && !atomic_load(&r->dead))
		if (pthread_cond_timedwait(&r->cond, &r->lock, &until) == ETIMEDOUT)
			break;
	struct usbproto_msg a = r->open_msg;
	bool taken = r->opened && a.status == 0 && !r->devs[a.dev];
	if (taken) {
		char name[sizeof rd->dev.name];
		unsigned speed;
		usbproto_get_info(r->open_data, &rd->info, &speed, name, sizeof name);
		rd->r = r;
		rd->slot = a.dev;
		atomic_init(&rd->gone, false);
		usb_device_init(&rd->dev, &remote_ops, speed, name);
		r->devs[a.dev] = rd;
	} else if (r->opened && a.status != 0) {
		char why[USBPROTO_WHY];
		memcpy(why, r->open_data, a.data);
		why[a.data] = 0;
		snprintf(err, size, "%s", why);
	} else {
		snprintf(err, size, "Blue's USB helper didn't answer (taking %s)", spec);
	}
	r->open_id = 0;
	r->opened = false;
	pthread_mutex_unlock(&r->lock);
	if (!taken) {
		free(rd);
		return NULL;
	}
	return &rd->dev;
}

/* blue-usb-helper, beside Blue's executable (in Blue.app's Contents/MacOS, or the build's directory) */
static bool helper_path(char *out, size_t size)
{
	char exe[PATH_MAX];
	if (!hostlib_executable(exe, sizeof exe) || !strrchr(exe, '/'))
		return false;
	*strrchr(exe, '/') = 0;
	return snprintf(out, size, "%s/blue-usb-helper", exe) < (int)size && access(out, X_OK) == 0;
}

/* For tests: as the user, without asking */
static int spawn(struct usbremote *r, char *why, size_t size)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		snprintf(why, size, "Blue's USB helper can't be started (%s)", strerror(errno));
		return -1;
	}
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
	posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
	posix_spawn_file_actions_addinherit_np(&fa, 2);
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
	char *args[] = { r->helper, (char *)"--serve", NULL };
	pid_t pid;
	int e = posix_spawn(&pid, r->helper, &fa, &at, args, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	close(sv[1]);
	if (e != 0) {
		close(sv[0]);
		snprintf(why, size, "Blue's USB helper can't be started (%s)", strerror(e));
		return -1;
	}
	fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	return sv[0];
}

typedef OSStatus (*execute_fn)(AuthorizationRef, const char *, AuthorizationFlags, char *const *, FILE **);

/*
 * As root, once an administrator's password is given in macOS's own dialog:
 * the helper's standard input and output are a socket, whose other end is
 * Blue's. (AuthorizationExecuteWithPrivileges() is found at run time: it's
 * deprecated, though it's still there, and still the way for an app without
 * a Developer ID to run something as root for as long as it runs.)
 */
static int ask(struct usbremote *r, char *why, size_t size)
{
	execute_fn execute;
	*(void **)&execute = dlsym(RTLD_DEFAULT, "AuthorizationExecuteWithPrivileges");
	AuthorizationRef auth;
	if (!execute || AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults, &auth) !=
			errAuthorizationSuccess) {
		snprintf(why, size, "this macOS can't start Blue's USB helper as root: running Blue with sudo is the "
			 "way left");
		return -1;
	}
	AuthorizationItem item = { kAuthorizationRightExecute, strlen(r->helper), r->helper, 0 };
	AuthorizationRights rights = { 1, &item };
	static const char prompt[] = "Blue wants to take USB devices from macOS's own drivers, for Mac OS, until it "
				     "quits.";
	AuthorizationItem said = { kAuthorizationEnvironmentPrompt, sizeof prompt - 1, (void *)prompt, 0 };
	AuthorizationEnvironment env = { 1, &said };
	OSStatus st = AuthorizationCopyRights(auth, &rights, &env, kAuthorizationFlagInteractionAllowed |
					      kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights, NULL);
	FILE *pipe = NULL;
	if (st == errAuthorizationSuccess) {
		char *args[] = { (char *)"--serve", NULL };
		st = execute(auth, r->helper, kAuthorizationFlagDefaults, args, &pipe);
	}
	AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
	if (st == errAuthorizationCanceled) {
		snprintf(why, size, "the administrator's password wasn't given");
		return -1;
	}
	if (st != errAuthorizationSuccess || !pipe) {
		if (pipe)
			fclose(pipe);
		snprintf(why, size, st == errAuthorizationDenied ? "that password isn't an administrator's" :
			 "Blue's USB helper couldn't be started as root (error %d)", (int)st);
		return -1;
	}
	int fd = fcntl(fileno(pipe), F_DUPFD_CLOEXEC, 3);
	fclose(pipe);
	if (fd < 0)
		snprintf(why, size, "Blue's USB helper couldn't be started (%s)", strerror(errno));
	return fd;
}

/* Waits for the helper's hello on `fd` */
static bool hello(struct usbremote *r, int fd, char *why, size_t size)
{
	int one = 1, buffer = SOCKET_BUFFER;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof buffer);
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
	struct pollfd p = { fd, POLLIN, 0 };
	struct usbproto_msg m;
	if (poll(&p, 1, HELLO_MS) != 1 || usbproto_recv(fd, &m, r->buf) <= 0 || m.type != USBPROTO_HELLO ||
	    usbproto_check_answer(&m)) {
		snprintf(why, size, "Blue's USB helper didn't start");
		return false;
	}
	if (m.status != USBPROTO_VERSION) {
		snprintf(why, size, "Blue's USB helper is another version's (Blue needs installing again)");
		return false;
	}
	r->pid = (pid_t)m.id;
	return true;
}

static struct usbremote *remote_new(void)
{
	struct usbremote *r = calloc(1, sizeof *r);
	if (!r)
		return NULL;
	r->fd = -1;
	pthread_mutex_init(&r->lock, NULL);
	pthread_mutex_init(&r->wlock, NULL);
	pthread_cond_init(&r->cond, NULL);
	atomic_init(&r->dead, false);
	return r;
}

static void remote_free(struct usbremote *r)
{
	if (r->fd >= 0)
		shutdown(r->fd, SHUT_RDWR);     /* (the helper sees its end, and ends) */
	if (r->reading)
		pthread_join(r->reader, NULL);
	if (r->fd >= 0)
		close(r->fd);
	for (int k = 0; r->pid > 0 && k < QUIT_MS / 10; k++) {
		pid_t w = waitpid(r->pid, NULL, WNOHANG);
		if (w != 0)
			break;
		usleep(10000);
	}
	pthread_mutex_destroy(&r->lock);
	pthread_mutex_destroy(&r->wlock);
	pthread_cond_destroy(&r->cond);
	free(r);
}

static void *starter(void *arg)
{
	struct usbremote *r = arg;
	char why[USBPROTO_WHY] = "";
	int fd = getenv("BLUE_USB_HELPER_TEST") ? spawn(r, why, sizeof why) : ask(r, why, sizeof why);
	bool ok = fd >= 0 && hello(r, fd, why, sizeof why);
	pthread_mutex_lock(&r->lock);
	r->fd = fd;
	bool abandoned = r->abandoned;
	if (ok && !abandoned) {
		r->reading = pthread_create(&r->reader, NULL, reader, r) == 0;
		ok = r->reading;
		if (!ok)
			snprintf(why, sizeof why, "Blue's USB helper's thread can't start");
	}
	r->state = ok ? USBREMOTE_READY : USBREMOTE_FAILED;
	snprintf(r->why, sizeof r->why, "%s", why);
	pthread_mutex_unlock(&r->lock);
	if (abandoned)
		remote_free(r);
	return NULL;
}

struct usbremote *usbremote_start(char *err, size_t size)
{
	struct usbremote *r = remote_new();
	if (!r) {
		snprintf(err, size, "out of memory");
		return NULL;
	}
	if (!helper_path(r->helper, sizeof r->helper)) {
		snprintf(err, size, "Blue's USB helper (blue-usb-helper) isn't beside Blue, so devices macOS's own drivers "
			 "have can't be taken from them");
		remote_free(r);
		return NULL;
	}
	r->state = USBREMOTE_ASKING;
	r->starting = pthread_create(&r->starter, NULL, starter, r) == 0;
	if (!r->starting) {
		snprintf(err, size, "Blue's USB helper's thread can't start");
		remote_free(r);
		return NULL;
	}
	return r;
}

struct usbremote *usbremote_connect(int fd, char *err, size_t size)
{
	struct usbremote *r = remote_new();
	if (!r) {
		snprintf(err, size, "out of memory");
		return NULL;
	}
	if (!hello(r, fd, err, size)) {
		remote_free(r);
		return NULL;
	}
	r->fd = fd;
	r->reading = pthread_create(&r->reader, NULL, reader, r) == 0;
	if (!r->reading) {
		snprintf(err, size, "Blue's USB helper's thread can't start");
		r->fd = -1;
		remote_free(r);
		return NULL;
	}
	r->state = USBREMOTE_READY;
	return r;
}

enum usbremote_state usbremote_state(struct usbremote *r, char *why, size_t size)
{
	pthread_mutex_lock(&r->lock);
	enum usbremote_state s = r->state;
	if (s == USBREMOTE_READY && atomic_load(&r->dead)) {
		s = USBREMOTE_FAILED;
		snprintf(r->why, sizeof r->why, "Blue's USB helper has stopped");
	}
	if (why && size)
		snprintf(why, size, "%s", s == USBREMOTE_FAILED ? r->why : "");
	pthread_mutex_unlock(&r->lock);
	return s;
}

void usbremote_stop(struct usbremote *r)
{
	if (!r)
		return;
	pthread_mutex_lock(&r->lock);
	bool asking = r->state == USBREMOTE_ASKING;
	r->abandoned = asking;
	pthread_mutex_unlock(&r->lock);
	if (r->starting && asking) {
		pthread_detach(r->starter);     /* (the dialog can't be taken back: what's started then ends at once) */
		return;
	}
	if (r->starting)
		pthread_join(r->starter, NULL);
	remote_free(r);
}
