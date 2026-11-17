/*
 * The host's USB devices passed through (usbhost.c), with a stand-in for
 * libusb (tests/libusb_mock.c, build/libusb-mock.so beside the tests) whose
 * device is a USB test disk: its transfers finish on libusb's thread, a
 * millisecond or more later, through the controller as Mac OS would make
 * them; and it's unplugged and plugged in again.
 */
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/hostlib.h"
#include "../src/mem.h"
#include "../src/ohci.h"
#include "../src/usbhost.h"
#include "test.h"

#define RAM 0x100000u
#define HCCA 0x2000u
#define FRAME 12000u

static struct mem mem;
static struct ohci usb;
static uint64_t now;

static void wr(uint32_t reg, uint32_t v)
{
	ohci_mmio_ops.write(&usb, reg, 4, __builtin_bswap32(v));
}

static void put(uint32_t a, uint32_t v)
{
	put_le32(mem_ptr(&mem, a), v);
}

static uint32_t get(uint32_t a)
{
	return le32(mem_ptr(&mem, a));
}

static void ed(uint32_t at, uint32_t flags, uint32_t head, uint32_t tail, uint32_t next)
{
	put(at, flags);
	put(at + 4, tail);
	put(at + 8, head);
	put(at + 12, next);
}

static void td(uint32_t at, uint32_t flags, uint32_t cbp, uint32_t len, uint32_t next)
{
	put(at, flags);
	put(at + 4, len ? cbp : 0);
	put(at + 8, next);
	put(at + 12, len ? cbp + len - 1 : 0);
}

/* Frames, a millisecond of the host's time each, until the ED's head is `head` (at most 2 seconds) */
static bool until_head(uint32_t at, uint32_t head)
{
	for (int k = 0; k < 2000; k++) {
		if ((get(at + 8) & ~2u) == head)
			return true;
		now += FRAME;
		ohci_set_time(&usb, now);
		struct timespec ms = { 0, 1000000 };
		nanosleep(&ms, NULL);
	}
	return false;
}

#define TD_FLAGS(dp, toggle) (0xF0000000u | (dp) << 19 | 7u << 21 | (toggle) << 24)

static char image[64], unplugged[80];

static void remove_image(void)
{
	unlink(image);
	unlink(unplugged);
}

bool usbhost_test_mock(void)
{
	char exe[PATH_MAX], lib[PATH_MAX + 32];
	if (!hostlib_executable(exe, sizeof exe) || !strrchr(exe, '/')) {
		CHECK(!"the tests' own path");
		return false;
	}
	*strrchr(exe, '/') = 0;
	snprintf(lib, sizeof lib, "%s/libusb-mock.so", exe);
	if (access(lib, F_OK) != 0) {
		printf("usbhost skipped (no %s)\n", lib);
		return false;
	}
	if (!image[0]) {                /* (one for all the tests: the stand-in keeps the first it's given) */
		snprintf(image, sizeof image, "/tmp/blue-usbhost-XXXXXX");
		int fd = mkstemp(image);
		static uint8_t blocks[32 * 512];
		for (unsigned k = 0; k < sizeof blocks; k++)
			blocks[k] = (uint8_t)(k / 512 + 0x30);
		CHECK(write(fd, blocks, sizeof blocks) == (ssize_t)sizeof blocks);
		close(fd);
		snprintf(unplugged, sizeof unplugged, "%s.unplugged", image);
		atexit(remove_image);
	}
	setenv("BLUE_LIBUSB", lib, 1);
	setenv("BLUE_LIBUSB_MOCK_IMAGE", image, 1);
	return true;
}

void usbhost_test_unmock(void)
{
	unsetenv("BLUE_LIBUSB");
	unsetenv("BLUE_LIBUSB_MOCK_IMAGE");
}

void usbhost_test_replug(void)
{
	unlink(unplugged);
}

void usbhost_test_transfers(struct usb_device *d, bool (*gone)(const struct usb_device *))
{
	CHECK(!gone(d));
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		d->ops->destroy(d);
		return;
	}
	mem_map(&mem, 0, RAM, MEM_RWX);
	ohci_init(&usb, &mem);
	wr(0x18, HCCA);
	wr(0x04, 0x80 | 0x3C);
	CHECK(ohci_connect(&usb, 0, d));
	wr(0x54, 0x10);
	now += 10 * FRAME;
	ohci_set_time(&usb, now);

	/* GET_DESCRIPTOR (it answers later: the data stage waits), then SET_CONFIGURATION (libusb's call) */
	static const uint8_t get_device[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 }, set_config[8] = { 0, 9, 1, 0, 0, 0, 0, 0 };
	memcpy(mem_ptr(&mem, 0x8000), get_device, 8);
	memcpy(mem_ptr(&mem, 0x8008), set_config, 8);
	ed(0x4000, 0 | 64u << 16, 0x5000, 0x5140, 0);
	td(0x5000, TD_FLAGS(0, 2), 0x8000, 8, 0x5040);
	td(0x5040, TD_FLAGS(2, 3) | 1u << 18, 0x9000, 18, 0x5080);
	td(0x5080, TD_FLAGS(1, 3), 0, 0, 0x50C0);
	td(0x50C0, TD_FLAGS(0, 2), 0x8008, 8, 0x5100);
	td(0x5100, TD_FLAGS(2, 3), 0, 0, 0x5140);
	wr(0x20, 0x4000);
	wr(0x08, 2);
	now += FRAME;
	ohci_set_time(&usb, now);
	CHECK_EQ(get(0x4008) & ~2u, 0x5040);                    /* (the data stage NAKed: not yet answered) */
	CHECK(until_head(0x4000, 0x5140));
	CHECK_EQ(get(0x5040) >> 28, 0);
	CHECK_EQ(mem_r8(&mem, 0x9000 + 8), 0x09);               /* idVendor 0x1209 */
	CHECK_EQ(mem_r8(&mem, 0x9000 + 9), 0x12);

	/* READ(10) of block 3 on the bulk endpoints, in transfers on libusb's thread */
	uint8_t cbw[31] = { 'U', 'S', 'B', 'C', 7, 0, 0, 0, 0, 2, 0, 0, 0x80, 0, 10, 0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0 };
	memcpy(mem_ptr(&mem, 0x8100), cbw, 31);
	ed(0x4040, 2u << 7 | 1u << 11 | 64u << 16, 0x5200, 0x5240, 0x4080);
	td(0x5200, TD_FLAGS(1, 0), 0x8100, 31, 0x5240);
	ed(0x4080, 1u << 7 | 2u << 11 | 64u << 16, 0x5300, 0x5380, 0);
	td(0x5300, TD_FLAGS(2, 0), 0xA000, 512, 0x5340);
	td(0x5340, TD_FLAGS(2, 0), 0xA800, 13, 0x5380);
	wr(0x28, 0x4040);
	wr(0x08, 4);
	CHECK(until_head(0x4080, 0x5380));
	CHECK_EQ(get(0x4048) & ~2u, 0x5240);
	CHECK_EQ(mem_r8(&mem, 0xA000 + 511), 0x33);
	CHECK_EQ(le32(mem_ptr(&mem, 0xA800)), 0x53425355u);
	CHECK_EQ(mem_r8(&mem, 0xA800 + 4), 7);                  /* (the tag) */
	CHECK_EQ(mem_r8(&mem, 0xA800 + 12), 0);

	/* an IN with nothing to come waits (in flight); skipping its ED gives it up */
	td(0x5380, TD_FLAGS(2, 0), 0xA800, 13, 0x53C0);
	put(0x4084, 0x53C0);
	wr(0x08, 4);
	for (int k = 0; k < 20; k++) {
		now += FRAME;
		ohci_set_time(&usb, now);
		usleep(1000);
	}
	CHECK(ohci_waiting(&usb));
	put(0x4080, get(0x4080) | 1u << 14);
	now += FRAME;
	ohci_set_time(&usb, now);
	CHECK(!ohci_waiting(&usb));

	/* unplugged from the host */
	int fd = open(unplugged, O_CREAT | O_WRONLY, 0644);
	close(fd);
	for (int k = 0; k < 400 && !gone(d); k++)
		usleep(5000);
	CHECK(gone(d));
	struct usb_device *was = ohci_disconnect(&usb, 0);
	CHECK(was == d);
	d->ops->destroy(d);
	ohci_free(&usb);
	mem_free(&mem);
}

void test_usbhost(void)
{
	if (!usbhost_test_mock())
		return;
	char err[400];
	struct usbhost *h = usbhost_start(err, sizeof err);
	CHECK(h != NULL);
	if (!h)
		goto out;
	struct usbhost_info info[8];
	CHECK_EQ(usbhost_list(h, info, 8), 3);                  /* (the hub isn't listed) */
	CHECK(!strcmp(info[0].id, "1-3"));
	CHECK_EQ(info[0].vendor, 0x1209);
	CHECK_EQ(info[0].speed, USB_SPEED_FULL);
	CHECK_EQ(info[1].speed, USB_SPEED_HIGH);
	CHECK(usbhost_matches(&info[0], "1209:0001"));
	CHECK(!usbhost_matches(&info[0], "1209:0002"));
	CHECK(usbhost_matches(&info[1], "1-4"));
	CHECK(!strcmp(info[2].id, "1-6"));
	CHECK_EQ(info[2].speed, USB_SPEED_LOW);
	struct usb_device *k = usbhost_open(h, "05ac:0201", NULL, err, sizeof err);     /* (a keyboard) */
	CHECK(k != NULL);
	if (k)
		k->ops->destroy(k);
	CHECK(usbhost_open(h, "1209:0002", NULL, err, sizeof err) == NULL);  /* high speed */
	CHECK(strstr(err, "high") != NULL);
	CHECK(usbhost_open(h, "2-1", NULL, err, sizeof err) == NULL);
	struct usb_device *d = usbhost_open(h, "1-3", NULL, err, sizeof err);
	CHECK(d != NULL);
	if (!d)
		goto stop;
	CHECK(usbhost_is(d));
	usbhost_test_transfers(d, usbhost_gone);

	/* plugged in again */
	usbhost_test_replug();
	struct usbhost_info arrived;
	bool back = false;
	for (int k = 0; k < 400 && !back; k++) {
		while (usbhost_arrived(h, &arrived))
			back |= usbhost_matches(&arrived, "1209:0001");
		usleep(5000);
	}
	CHECK(back);
	d = usbhost_open(h, arrived.id, NULL, err, sizeof err);
	CHECK(d != NULL);
	if (d)
		d->ops->destroy(d);

	/* the host's driver has it (as macOS's would): only root, or the USB helper, can take it */
	setenv("BLUE_LIBUSB_MOCK_HELD", "1", 1);
	bool held = false;
	d = usbhost_open(h, "1209:0001", &held, err, sizeof err);
	CHECK(d == NULL);
#ifdef __APPLE__
	CHECK(held);
#endif
	unsetenv("BLUE_LIBUSB_MOCK_HELD");
stop:
	usbhost_stop(h);
out:
	usbhost_test_unmock();
}
