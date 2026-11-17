#include "usbhelper.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "usb.h"
#include "usbhost.h"
#include "usbproto.h"

volatile sig_atomic_t usbhelper_quit;

/* A transfer in flight */
struct req {
	struct usb_xfer x;
	uint32_t id;
	struct req *next;
	uint8_t buf[];
};

struct helper {
	int fd;
	int wake[2];                    /* (libusb's thread wakes the loop: an answer has come, or a device has gone) */
	struct usbhost *host;           /* libusb, once a device is wanted */
	char why[USBPROTO_WHY];         /* ... why it can't be had */
	struct usb_device *dev[USBPROTO_DEVICES];
	bool told_gone[USBPROTO_DEVICES];
	struct req *reqs[USBPROTO_DEVICES];
	unsigned count[USBPROTO_DEVICES];
	uint8_t data[USBPROTO_DATA];
};

static void wake(void *arg)
{
	struct helper *s = arg;
	char c = 0;
	ssize_t w = write(s->wake[1], &c, 1);  /* (the pipe full: it's awake already) */
	(void)w;
}

static bool done(struct helper *s, unsigned k, const struct req *r)
{
	struct usbproto_msg m = { .type = USBPROTO_DONE, .dev = (uint8_t)k, .id = r->id, .status = (uint32_t)r->x.status };
	m.len = r->x.actual < r->x.len ? r->x.actual : r->x.len;
	m.data = r->x.ep & USB_DIR_IN ? m.len : 0;
	return usbproto_send(s->fd, &m, r->buf);
}

/* Answers the transfers finished, and says which devices have gone; false if Blue has gone */
static bool answer(struct helper *s)
{
	for (unsigned k = 0; k < USBPROTO_DEVICES; k++) {
		for (struct req **p = &s->reqs[k]; *p;) {
			struct req *r = *p;
			if (!atomic_load_explicit(&r->x.done, memory_order_acquire)) {
				p = &r->next;
				continue;
			}
			*p = r->next;
			s->count[k]--;
			bool ok = done(s, k, r);
			free(r);
			if (!ok)
				return false;
		}
		if (s->dev[k] && !s->told_gone[k] && usbhost_gone(s->dev[k])) {
			s->told_gone[k] = true;
			struct usbproto_msg m = { .type = USBPROTO_GONE, .dev = (uint8_t)k };
			if (!usbproto_send(s->fd, &m, NULL))
				return false;
		}
	}
	return true;
}

/* (once the device has given them up: a reset, or closed) */
static void drop_all(struct helper *s, unsigned k)
{
	while (s->reqs[k]) {
		struct req *r = s->reqs[k];
		s->reqs[k] = r->next;
		free(r);
	}
	s->count[k] = 0;
}

static bool open_device(struct helper *s, const struct usbproto_msg *m)
{
	char spec[USBPROTO_SPEC], why[USBPROTO_WHY] = "";
	memcpy(spec, s->data, m->data);
	spec[m->data] = 0;
	unsigned k = 0;
	while (k < USBPROTO_DEVICES && s->dev[k])
		k++;
	if (!s->host && !s->why[0]) {
		s->host = usbhost_start(s->why, sizeof s->why);
		if (s->host) {
			usbhost_notify(s->host, wake, s);
			usbhost_refuse_keyboards(s->host);
		}
	}
	struct usb_device *d = NULL;
	if (!s->host)
		snprintf(why, sizeof why, "%s", s->why);
	else if (k == USBPROTO_DEVICES)
		snprintf(why, sizeof why, "Blue's USB helper has as many devices as it takes");
	else
		d = usbhost_open(s->host, spec, NULL, why, sizeof why);
	struct usbproto_msg a = { .type = USBPROTO_OPENED, .id = m->id };
	if (!d) {
		a.status = 1;
		a.data = (uint32_t)strlen(why);
		return usbproto_send(s->fd, &a, why);
	}
	s->dev[k] = d;
	s->told_gone[k] = false;
	uint8_t info[USBPROTO_INFO];
	usbproto_put_info(info, usbhost_device_info(d), d->speed, d->name);
	a.dev = (uint8_t)k;
	a.data = USBPROTO_INFO;
	return usbproto_send(s->fd, &a, info);
}

static bool submit(struct helper *s, const struct usbproto_msg *m)
{
	unsigned k = m->dev;
	struct usb_device *d = s->dev[k];
	struct req *r = s->count[k] < USBPROTO_REQUESTS ? calloc(1, sizeof *r + (m->len ? m->len : 1)) : NULL;
	if (!r) {
		struct usbproto_msg a = { .type = USBPROTO_DONE, .dev = m->dev, .id = m->id, .status = USB_ERROR };
		return usbproto_send(s->fd, &a, NULL);
	}
	r->id = m->id;
	r->x.ep = m->ep;
	r->x.type = m->xfer;
	memcpy(r->x.setup, m->setup, 8);
	r->x.data = r->buf;
	r->x.len = m->len;
	if (!(m->ep & USB_DIR_IN))
		memcpy(r->buf, s->data, m->len);
	atomic_init(&r->x.done, false);
	d->log = m->flags & USBPROTO_LOG;
	int st = d->ops->submit(d, &r->x);
	if (st == USB_PENDING) {
		r->next = s->reqs[k];
		s->reqs[k] = r;
		s->count[k]++;
		return true;
	}
	r->x.status = st;
	bool ok = done(s, k, r);
	free(r);
	return ok;
}

static void cancel(struct helper *s, const struct usbproto_msg *m)
{
	for (struct req **p = &s->reqs[m->dev]; *p; p = &(*p)->next) {
		struct req *r = *p;
		if (r->id == m->id) {
			s->dev[m->dev]->ops->cancel(s->dev[m->dev], &r->x);
			*p = r->next;
			s->count[m->dev]--;
			free(r);
			return;
		}
	}
}

int usbhelper_serve(int fd)
{
	struct helper *s = calloc(1, sizeof *s);
	if (!s)
		return 1;
	if (pipe(s->wake) != 0) {
		free(s);
		return 1;
	}
	for (int k = 0; k < 2; k++) {
		fcntl(s->wake[k], F_SETFL, O_NONBLOCK);
		fcntl(s->wake[k], F_SETFD, FD_CLOEXEC);
	}
	s->fd = fd;
	int buffer = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof buffer);
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
	int result = 0;
	struct usbproto_msg hello = { .type = USBPROTO_HELLO, .status = USBPROTO_VERSION, .id = (uint32_t)getpid() };
	bool ok = usbproto_send(fd, &hello, NULL);
	while (ok && !usbhelper_quit) {
		struct pollfd p[2] = { { fd, POLLIN, 0 }, { s->wake[0], POLLIN, 0 } };
		int n = poll(p, 2, 100);
		if (n < 0 && errno != EINTR)
			break;
		if (n > 0 && p[1].revents) {
			char b[64];
			while (read(s->wake[0], b, sizeof b) > 0) {
			}
		}
		ok = answer(s);
		if (!ok || n <= 0 || !(p[0].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		struct usbproto_msg m;
		int got = usbproto_recv(fd, &m, s->data);
		const char *why = got < 0 ? "a message that isn't one" : got ? usbproto_check_request(&m, s->data) : NULL;
		if (!why && got > 0 && m.type != USBPROTO_OPEN && !s->dev[m.dev])
			why = "a request for a device it hasn't";
		if (why) {
			fprintf(stderr, "blue-usb-helper: refused %s\n", why);
			result = 1;
		}
		if (got <= 0 || why)
			break;
		struct usb_device *d = s->dev[m.dev];
		switch (m.type) {
		case USBPROTO_OPEN:
			ok = open_device(s, &m);
			break;
		case USBPROTO_SUBMIT:
			ok = submit(s, &m);
			break;
		case USBPROTO_CANCEL:
			cancel(s, &m);
			break;
		case USBPROTO_RESET:
			d->ops->reset(d);
			drop_all(s, m.dev);
			break;
		case USBPROTO_CLOSE:
			d->ops->destroy(d);
			drop_all(s, m.dev);
			s->dev[m.dev] = NULL;
			break;
		}
		ok = ok && answer(s);
	}
	for (unsigned k = 0; k < USBPROTO_DEVICES; k++) {
		if (s->dev[k])
			s->dev[k]->ops->destroy(s->dev[k]);
		drop_all(s, k);
	}
	usbhost_stop(s->host);
	close(s->wake[0]);
	close(s->wake[1]);
	free(s);
	return result;
}
