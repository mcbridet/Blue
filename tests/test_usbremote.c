/*
 * Blue's USB helper (macOS): its protocol (usbproto.h); Blue's side of it
 * (usbremote.c) against a stand-in helper over a socket; the helper itself
 * (usbhelper.c), on a thread, taking the stand-in libusb's test disk
 * (tests/libusb_mock.c) through the same transfers as test_usbhost.c's;
 * and blue-usb-helper, run as the user, taking it when the stand-in's
 * "driver" won't let Blue have it.
 */
#include "test.h"

#ifdef __APPLE__

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/usbhelper.h"
#include "../src/usbproto.h"
#include "../src/usbremote.h"

static bool wait_done(struct usb_xfer *x)
{
	for (int k = 0; k < 2000; k++) {
		if (atomic_load_explicit(&x->done, memory_order_acquire))
			return true;
		usleep(1000);
	}
	return false;
}

static void xfer(struct usb_xfer *x, uint8_t ep, uint8_t type, uint8_t *data, uint32_t len)
{
	memset(x, 0, sizeof *x);
	x->ep = ep;
	x->type = type;
	x->data = data;
	x->len = len;
	atomic_init(&x->done, false);
}

/* Framing, and what's refused */
static void protocol(void)
{
	struct usbproto_msg m = { USBPROTO_SUBMIT, 3, 0x81, USB_BULK, USBPROTO_LOG, 0x01020304u, 512, 0, 0,
				  { 1, 2, 3, 4, 5, 6, 7, 8 } }, n;
	uint8_t head[USBPROTO_HEAD];
	usbproto_put(head, &m);
	CHECK_EQ(head[8], 4);                                   /* (little-endian) */
	CHECK_EQ(head[12], 0);
	CHECK_EQ(head[13], 2);
	CHECK(usbproto_get(&n, head));
	CHECK(!memcmp(&m, &n, sizeof m) || (n.type == m.type && n.id == m.id && n.len == m.len && n.ep == m.ep &&
					     !memcmp(n.setup, m.setup, 8)));
	head[6] = 1;
	CHECK(!usbproto_get(&n, head));                         /* (its unused bytes aren't zero) */
	head[6] = 0;
	head[22] = 2;                                           /* (data: 128 KB) */
	CHECK(!usbproto_get(&n, head));

	/* requests Blue may make */
	static const uint8_t no[1];
	uint8_t spec[] = "1209:0001", port[] = "1-3.2", bad[] = "1-3;x", out[2];
	struct usbproto_msg open = { .type = USBPROTO_OPEN, .data = 9 };
	CHECK(!usbproto_check_request(&open, spec));
	open.data = 5;
	CHECK(!usbproto_check_request(&open, port));
	CHECK(usbproto_check_request(&open, bad));
	open.data = 0;
	CHECK(usbproto_check_request(&open, spec));
	open.data = USBPROTO_SPEC;
	CHECK(usbproto_check_request(&open, (const uint8_t *)"1111111111111111111111111111111111111"));
	open.data = 9;
	open.dev = 1;
	CHECK(usbproto_check_request(&open, spec));

	struct usbproto_msg ok[] = {
		{ .type = USBPROTO_SUBMIT, .ep = 0x80, .xfer = USB_CONTROL, .len = 18, .setup = { 0x80, 6, 0, 1, 0, 0, 18 } },
		{ .type = USBPROTO_SUBMIT, .dev = 3, .xfer = USB_CONTROL, .len = 2, .data = 2, .setup = { 0x21, 9, 0, 2, 0, 0, 2 } },
		{ .type = USBPROTO_SUBMIT, .ep = 0x02, .xfer = USB_BULK, .len = 512, .data = 512 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_INTERRUPT, .len = 8, .flags = USBPROTO_LOG },
		{ .type = USBPROTO_SUBMIT, .ep = 0x85, .xfer = USB_ISO, .len = 1023 },
		{ .type = USBPROTO_CANCEL, .dev = 1, .id = 5 },
		{ .type = USBPROTO_RESET, .dev = 2 },
		{ .type = USBPROTO_CLOSE, .dev = 3 },
	};
	for (unsigned k = 0; k < sizeof ok / sizeof *ok; k++) {
		const char *why = usbproto_check_request(&ok[k], out);
		if (why)
			fprintf(stderr, "request %u refused: %s\n", k, why);
		CHECK(!why);
	}
	struct usbproto_msg refused[] = {
		{ .type = 0 },
		{ .type = 99 },
		{ .type = USBPROTO_HELLO },                                             /* (an answer) */
		{ .type = USBPROTO_SUBMIT, .dev = USBPROTO_DEVICES, .ep = 0x81, .xfer = USB_BULK, .len = 8 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x91, .xfer = USB_BULK, .len = 8 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = 4, .len = 8 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_BULK, .len = 8, .flags = 2 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_BULK, .len = 8, .status = 1 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_BULK, .len = 8, .data = 8 },     /* (in, with data) */
		{ .type = USBPROTO_SUBMIT, .ep = 0x02, .xfer = USB_BULK, .len = 8, .data = 4 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_BULK, .len = USBPROTO_DATA + 1 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x80, .xfer = USB_BULK, .len = 8 },     /* (endpoint 0) */
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_BULK, .len = 8, .setup = { 1 } },
		{ .type = USBPROTO_SUBMIT, .ep = 0x85, .xfer = USB_ISO, .len = 1024 },
		{ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_CONTROL, .len = 18, .setup = { 0x80, 6, 0, 1, 0, 0, 18 } },
		{ .type = USBPROTO_SUBMIT, .ep = 0x00, .xfer = USB_CONTROL, .len = 18, .setup = { 0x80, 6, 0, 1, 0, 0, 18 } },
		{ .type = USBPROTO_SUBMIT, .ep = 0x80, .xfer = USB_CONTROL, .len = 64, .setup = { 0x80, 6, 0, 1, 0, 0, 18 } },
		{ .type = USBPROTO_SUBMIT, .xfer = USB_CONTROL, .len = 4, .data = 4, .setup = { 0x21, 9, 0, 2, 0, 0, 2 } },
		{ .type = USBPROTO_CANCEL, .dev = 1, .id = 5, .data = 1 },
		{ .type = USBPROTO_RESET, .dev = 2, .id = 1 },
		{ .type = USBPROTO_CLOSE, .dev = 7 },
	};
	for (unsigned k = 0; k < sizeof refused / sizeof *refused; k++) {
		bool r = usbproto_check_request(&refused[k], no) != NULL;
		if (!r)
			fprintf(stderr, "request %u not refused\n", k);
		CHECK(r);
	}

	/* answers the helper may give */
	struct usbproto_msg a = { .type = USBPROTO_DONE, .dev = 1, .status = USB_OK, .len = 8, .data = 8 };
	CHECK(!usbproto_check_answer(&a));
	a.data = 9;
	CHECK(usbproto_check_answer(&a));
	a.data = 0;
	a.status = USB_PENDING;
	CHECK(usbproto_check_answer(&a));
	a = (struct usbproto_msg){ .type = USBPROTO_OPENED, .dev = 1, .data = USBPROTO_INFO };
	CHECK(!usbproto_check_answer(&a));
	a.data = 12;
	CHECK(usbproto_check_answer(&a));
	a.status = 1;
	CHECK(!usbproto_check_answer(&a));
	a = (struct usbproto_msg){ .type = USBPROTO_GONE, .dev = USBPROTO_DEVICES };
	CHECK(usbproto_check_answer(&a));
	a.type = USBPROTO_OPEN;
	CHECK(usbproto_check_answer(&a));

	/* a device's description, its strings made printable */
	struct usbhost_info info = { "1-3.1", 0x05AC, 0x0201, USB_SPEED_LOW, "Apple\001Keyboard", true }, got;
	uint8_t data[USBPROTO_INFO];
	char name[96];
	unsigned speed;
	usbproto_put_info(data, &info, USB_SPEED_LOW, "Apple Keyboard (05ac:0201)");
	usbproto_get_info(data, &got, &speed, name, sizeof name);
	CHECK(!strcmp(got.id, "1-3.1"));
	CHECK_EQ(got.vendor, 0x05AC);
	CHECK_EQ(got.product, 0x0201);
	CHECK_EQ(speed, USB_SPEED_LOW);
	CHECK(got.held);
	CHECK(!strcmp(got.name, "Apple?Keyboard"));
	CHECK(!strcmp(name, "Apple Keyboard (05ac:0201)"));
}

/* A message written a byte at a time, then a head that isn't one */
static void *dribble(void *arg)
{
	int fd = *(int *)arg;
	struct usbproto_msg m = { .type = USBPROTO_DONE, .dev = 2, .id = 77, .len = 5, .data = 5 };
	uint8_t b[USBPROTO_HEAD + 5];
	usbproto_put(b, &m);
	memcpy(b + USBPROTO_HEAD, "hello", 5);
	for (unsigned k = 0; k < sizeof b; k++) {
		CHECK(write(fd, b + k, 1) == 1);
		usleep(200);
	}
	b[7] = 1;
	CHECK(write(fd, b, USBPROTO_HEAD) == USBPROTO_HEAD);
	return NULL;
}

static void partial_reads(void)
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	pthread_t t;
	pthread_create(&t, NULL, dribble, &sv[1]);
	static uint8_t data[USBPROTO_DATA];
	struct usbproto_msg m;
	CHECK_EQ(usbproto_recv(sv[0], &m, data), 1);
	CHECK_EQ(m.type, USBPROTO_DONE);
	CHECK_EQ(m.id, 77);
	CHECK(!memcmp(data, "hello", 5));
	CHECK_EQ(usbproto_recv(sv[0], &m, data), -1);
	pthread_join(t, NULL);
	close(sv[1]);
	CHECK_EQ(usbproto_recv(sv[0], &m, data), 0);
	close(sv[0]);
}

/* A stand-in for the helper: one device, 05ac:0201, its answers made up */
static struct {
	int fd;
	pthread_mutex_t lock;
	unsigned refused, resets, closes, iso_out, iso_in;
	uint32_t held_id, cancelled;
	uint8_t out[16];
	uint8_t setup[8];
} fake;

static void fake_send(struct usbproto_msg *m, const void *data)
{
	pthread_mutex_lock(&fake.lock);
	usbproto_send(fake.fd, m, data);
	pthread_mutex_unlock(&fake.lock);
}

static void *fake_helper(void *arg)
{
	(void)arg;
	static uint8_t data[USBPROTO_DATA];
	struct usbproto_msg m = { .type = USBPROTO_HELLO, .status = USBPROTO_VERSION, .id = 1 };
	fake_send(&m, NULL);
	while (usbproto_recv(fake.fd, &m, data) > 0) {
		if (usbproto_check_request(&m, data)) {
			fake.refused++;
			continue;
		}
		struct usbproto_msg a = { .type = USBPROTO_DONE, .dev = m.dev, .id = m.id, .status = USB_OK };
		switch (m.type) {
		case USBPROTO_OPEN:
			a.type = USBPROTO_OPENED;
			if (m.data == 9 && !memcmp(data, "05ac:0201", 9)) {
				struct usbhost_info info = { "1-2", 0x05AC, 0x0201, USB_SPEED_FULL, "Fake", true };
				uint8_t d[USBPROTO_INFO];
				usbproto_put_info(d, &info, USB_SPEED_FULL, "Fake (05ac:0201)");
				a.dev = 2;
				a.data = USBPROTO_INFO;
				fake_send(&a, d);
			} else {
				a.status = 1;
				a.data = 26;
				fake_send(&a, "there's no such USB device");
			}
			break;
		case USBPROTO_SUBMIT:
			if (m.xfer == USB_CONTROL) {
				memcpy(fake.setup, m.setup, 8);
				for (unsigned k = 0; k < m.len; k++)
					data[k] = (uint8_t)k;
				a.len = a.data = m.len;
				fake_send(&a, data);
			} else if (m.ep == 0x02) {
				memcpy(fake.out, data, m.len < 16 ? m.len : 16);
				a.len = m.len;
				fake_send(&a, NULL);
			} else if (m.ep == 0x83) {
				a.status = USB_STALL;
				fake_send(&a, NULL);
			} else if (m.ep == 0x81) {
				fake.held_id = m.id;    /* (answered only once given up, which Blue mustn't see) */
			} else if (m.ep == 0x85) {
				fake.iso_in++;
				a.len = a.data = 3;
				fake_send(&a, "iso");
			} else if (m.ep == 0x06) {
				fake.iso_out++;
				a.len = m.len;
				fake_send(&a, NULL);
			}
			break;
		case USBPROTO_CANCEL:
			fake.cancelled = m.id;
			a.len = a.data = 4;
			fake_send(&a, "late");
			break;
		case USBPROTO_RESET:
			fake.resets++;
			break;
		case USBPROTO_CLOSE:
			fake.closes++;
			break;
		}
	}
	return NULL;
}

static void against_fake(void)
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	memset(&fake, 0, sizeof fake);
	pthread_mutex_init(&fake.lock, NULL);
	fake.fd = sv[1];
	pthread_t t;
	pthread_create(&t, NULL, fake_helper, NULL);
	char err[400];
	struct usbremote *r = usbremote_connect(sv[0], err, sizeof err);
	CHECK(r != NULL);
	if (!r) {
		shutdown(sv[1], SHUT_RDWR);
		pthread_join(t, NULL);
		close(sv[1]);
		return;
	}
	CHECK_EQ(usbremote_state(r, NULL, 0), USBREMOTE_READY);
	CHECK(usbremote_open(r, "1-9", err, sizeof err) == NULL);
	CHECK(!strcmp(err, "there's no such USB device"));
	CHECK(usbremote_open(r, "1-3; reboot", err, sizeof err) == NULL);     /* (not even asked) */
	struct usb_device *d = usbremote_open(r, "05ac:0201", err, sizeof err);
	CHECK(d != NULL);
	if (!d)
		goto stop;
	CHECK(usbremote_is(d));
	CHECK(!strcmp(d->name, "Fake (05ac:0201)"));
	CHECK_EQ(d->speed, USB_SPEED_FULL);
	CHECK_EQ(usbremote_device_info(d)->vendor, 0x05AC);
	CHECK(!strcmp(usbremote_device_info(d)->id, "1-2"));

	/* a control request in, a bulk transfer out, a stall */
	uint8_t buf[64];
	struct usb_xfer x;
	xfer(&x, 0x80, USB_CONTROL, buf, 18);
	memcpy(x.setup, (uint8_t[8]){ 0x80, 6, 0, 1, 0, 0, 18, 0 }, 8);
	CHECK_EQ(d->ops->submit(d, &x), USB_PENDING);
	CHECK(wait_done(&x));
	CHECK_EQ(x.status, USB_OK);
	CHECK_EQ(x.actual, 18);
	CHECK_EQ(buf[17], 17);
	CHECK_EQ(fake.setup[6], 18);
	xfer(&x, 0x02, USB_BULK, (uint8_t *)"hello", 5);
	CHECK_EQ(d->ops->submit(d, &x), USB_PENDING);
	CHECK(wait_done(&x));
	CHECK_EQ(x.actual, 5);
	CHECK(!memcmp(fake.out, "hello", 5));
	xfer(&x, 0x83, USB_INTERRUPT, buf, 8);
	CHECK_EQ(d->ops->submit(d, &x), USB_PENDING);
	CHECK(wait_done(&x));
	CHECK_EQ(x.status, USB_STALL);

	/* given up: its late answer isn't seen */
	xfer(&x, 0x81, USB_BULK, buf, 64);
	CHECK_EQ(d->ops->submit(d, &x), USB_PENDING);
	for (int k = 0; k < 500 && !fake.held_id; k++)
		usleep(1000);
	d->ops->cancel(d, &x);
	for (int k = 0; k < 500 && !fake.cancelled; k++)
		usleep(1000);
	CHECK_EQ(fake.cancelled, fake.held_id);
	usleep(20000);
	CHECK(!atomic_load(&x.done));

	/* isochronous: answered at once, in from those received */
	xfer(&x, 0x85, USB_ISO, buf, 192);
	CHECK_EQ(d->ops->submit(d, &x), USB_OK);
	CHECK_EQ(x.actual, 0);
	for (int k = 0; k < 500 && fake.iso_in == 0; k++)
		usleep(1000);
	usleep(20000);
	xfer(&x, 0x85, USB_ISO, buf, 192);
	CHECK_EQ(d->ops->submit(d, &x), USB_OK);
	CHECK_EQ(x.actual, 3);
	CHECK(!memcmp(buf, "iso", 3));
	xfer(&x, 0x06, USB_ISO, buf, 10);
	CHECK_EQ(d->ops->submit(d, &x), USB_OK);
	CHECK_EQ(x.actual, 10);
	for (int k = 0; k < 500 && fake.iso_out == 0; k++)
		usleep(1000);
	CHECK_EQ(fake.iso_out, 1);

	/* a bus reset; unplugged from the host; unplugged from the Mac */
	d->ops->reset(d);
	struct usbproto_msg gone = { .type = USBPROTO_GONE, .dev = 2 };
	fake_send(&gone, NULL);
	for (int k = 0; k < 500 && !usbremote_gone(d); k++)
		usleep(1000);
	CHECK(usbremote_gone(d));
	xfer(&x, 0x02, USB_BULK, buf, 4);
	CHECK_EQ(d->ops->submit(d, &x), USB_NODEV);
	d->ops->destroy(d);
	for (int k = 0; k < 500 && !fake.closes; k++)
		usleep(1000);
	CHECK_EQ(fake.resets, 1);
	CHECK_EQ(fake.closes, 1);

	/* the helper stops: what's in flight ends, and the device has gone */
	d = usbremote_open(r, "05ac:0201", err, sizeof err);
	CHECK(d != NULL);
	if (d) {
		fake.held_id = 0;
		xfer(&x, 0x81, USB_BULK, buf, 64);
		CHECK_EQ(d->ops->submit(d, &x), USB_PENDING);
		for (int k = 0; k < 500 && !fake.held_id; k++)
			usleep(1000);
		shutdown(sv[1], SHUT_RDWR);
		CHECK(wait_done(&x));
		CHECK_EQ(x.status, USB_NODEV);
		CHECK(usbremote_gone(d));
		CHECK_EQ(usbremote_state(r, err, sizeof err), USBREMOTE_FAILED);
		CHECK(strstr(err, "stopped") != NULL);
		d->ops->destroy(d);
	}
	CHECK_EQ(fake.refused, 0);
stop:
	shutdown(sv[1], SHUT_RDWR);
	usbremote_stop(r);
	pthread_join(t, NULL);
	close(sv[1]);
}

static int served;

static void *serve(void *arg)
{
	served = usbhelper_serve(*(int *)arg);
	return NULL;
}

/* The helper on a thread: refusing what isn't a request, and taking the stand-in's disk */
static void helper_thread(void)
{
	int sv[2];
	pthread_t t;
	static uint8_t data[USBPROTO_DATA];
	struct usbproto_msg m;
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	pthread_create(&t, NULL, serve, &sv[1]);
	CHECK_EQ(usbproto_recv(sv[0], &m, data), 1);
	CHECK_EQ(m.type, USBPROTO_HELLO);
	CHECK_EQ(m.status, USBPROTO_VERSION);
	m = (struct usbproto_msg){ .type = USBPROTO_SUBMIT, .ep = 0x81, .xfer = USB_BULK, .len = 8 };
	CHECK(usbproto_send(sv[0], &m, NULL));                  /* (for a device it hasn't) */
	pthread_join(t, NULL);
	CHECK_EQ(served, 1);
	close(sv[1]);
	CHECK_EQ(usbproto_recv(sv[0], &m, data), 0);
	close(sv[0]);

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	pthread_create(&t, NULL, serve, &sv[1]);
	CHECK_EQ(usbproto_recv(sv[0], &m, data), 1);
	m = (struct usbproto_msg){ .type = 42 };
	CHECK(usbproto_send(sv[0], &m, NULL));
	pthread_join(t, NULL);
	CHECK_EQ(served, 1);
	close(sv[0]);
	close(sv[1]);

	if (!usbhost_test_mock())
		return;
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	served = -1;
	pthread_create(&t, NULL, serve, &sv[1]);
	char err[400];
	struct usbremote *r = usbremote_connect(sv[0], err, sizeof err);
	CHECK(r != NULL);
	if (r) {
		CHECK(usbremote_open(r, "1209:0002", err, sizeof err) == NULL);        /* (high speed) */
		CHECK(strstr(err, "high") != NULL);
		struct usb_device *d = usbremote_open(r, "1-3", err, sizeof err);
		CHECK(d != NULL);
		if (d) {
			CHECK(strstr(d->name, "1209:0001") != NULL);
			usbhost_test_transfers(d, usbremote_gone);
		}
		usbhost_test_replug();
		d = NULL;
		for (int k = 0; k < 400 && !d; k++) {
			d = usbremote_open(r, "1209:0001", err, sizeof err);
			if (!d)
				usleep(5000);
		}
		CHECK(d != NULL);
		if (d)
			d->ops->destroy(d);
		usbremote_stop(r);
	} else {
		shutdown(sv[0], SHUT_RDWR);
		close(sv[0]);
	}
	pthread_join(t, NULL);
	CHECK_EQ(served, 0);
	close(sv[1]);
	usbhost_test_unmock();
}

/* blue-usb-helper as a program, run as the user, where the stand-in's "driver" won't let Blue have its disk */
static void helper_program(void)
{
	if (!usbhost_test_mock())
		return;
	setenv("BLUE_LIBUSB_MOCK_HELD", "1", 1);
	setenv("BLUE_USB_HELPER_TEST", "1", 1);
	char err[400];
	struct usbhost *h = usbhost_start(err, sizeof err);
	CHECK(h != NULL);
	bool held = false;
	CHECK(h && usbhost_open(h, "1209:0001", &held, err, sizeof err) == NULL);
	CHECK(held);
	struct usbremote *r = usbremote_start(err, sizeof err);
	CHECK(r != NULL);
	enum usbremote_state s = USBREMOTE_ASKING;
	for (int k = 0; r && k < 1000 && (s = usbremote_state(r, err, sizeof err)) == USBREMOTE_ASKING; k++)
		usleep(5000);
	CHECK_EQ(s, USBREMOTE_READY);
	if (s != USBREMOTE_READY)
		fprintf(stderr, "the USB helper: %s\n", err);
	struct usb_device *d = s == USBREMOTE_READY ? usbremote_open(r, "1209:0001", err, sizeof err) : NULL;
	CHECK(d != NULL);
	if (!d)
		fprintf(stderr, "through the USB helper: %s\n", err);
	if (d)
		usbhost_test_transfers(d, usbremote_gone);
	usbhost_test_replug();
	usbremote_stop(r);
	if (h)
		usbhost_stop(h);
	unsetenv("BLUE_USB_HELPER_TEST");
	unsetenv("BLUE_LIBUSB_MOCK_HELD");
	usbhost_test_unmock();
}

void test_usbremote(void)
{
	protocol();
	partial_reads();
	against_fake();
	helper_thread();
	helper_program();
}

#else

void test_usbremote(void)
{
	printf("usbremote skipped (macOS's)\n");
}

#endif
