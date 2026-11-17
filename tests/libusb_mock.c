/*
 * A stand-in for libusb-1.0, for testing the passthrough of the host's USB
 * devices (usbhost.c) without real ones: BLUE_LIBUSB names it instead of
 * the real one. Its host has four devices on bus 1: a USB test disk
 * (usbmsd.h; 1209:0001, full speed, at 1-3) whose image is
 * BLUE_LIBUSB_MOCK_IMAGE, a high-speed device (1209:0002, 1-4), a hub,
 * and a low-speed keyboard (05ac:0201, 1-6) whose transfers just finish.
 * Transfers finish on the thread that handles events, a millisecond or
 * more after they're submitted (bulk in that the disk NAKs waits for data,
 * as on a bus). While a file named as the image with ".unplugged" added is
 * there, the disk is unplugged (hot-plug's callbacks say so). With
 * BLUE_LIBUSB_MOCK_HELD, the host's driver has the disk, as macOS's would,
 * and only Blue's USB helper (blue-usb-helper) can take it from it, as
 * root could.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../src/usb.h"
#include "../src/usbmsd.h"

#define API __attribute__((visibility("default")))

struct libusb_context { int unused; };
struct libusb_device { int n; };
struct libusb_device_handle { struct libusb_device *dev; };
typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_device_descriptor {
	uint8_t bLength, bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
	uint16_t idVendor, idProduct, bcdDevice;
	uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
struct libusb_endpoint_descriptor {
	uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval, bRefresh, bSynchAddress;
	const unsigned char *extra;
	int extra_length;
};
struct libusb_interface_descriptor {
	uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
	uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
	const struct libusb_endpoint_descriptor *endpoint;
	const unsigned char *extra;
	int extra_length;
};
struct libusb_interface {
	const struct libusb_interface_descriptor *altsetting;
	int num_altsetting;
};
struct libusb_config_descriptor {
	uint8_t bLength, bDescriptorType;
	uint16_t wTotalLength;
	uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
	const struct libusb_interface *interface;
	const unsigned char *extra;
	int extra_length;
};
struct libusb_iso_packet_descriptor {
	unsigned int length, actual_length;
	int status;
};
struct libusb_transfer;
typedef void (*libusb_transfer_cb_fn)(struct libusb_transfer *);
struct libusb_transfer {
	libusb_device_handle *dev_handle;
	uint8_t flags;
	unsigned char endpoint, type;
	unsigned int timeout;
	int status;
	int length, actual_length;
	libusb_transfer_cb_fn callback;
	void *user_data;
	unsigned char *buffer;
	int num_iso_packets;
	struct libusb_iso_packet_descriptor iso_packet_desc[];
};
typedef int libusb_hotplug_callback_handle;
typedef int (*libusb_hotplug_callback_fn)(libusb_context *, libusb_device *, int, void *);
struct libusb_version { uint16_t major, minor, micro, nano; const char *rc, *describe; };

enum { ST_COMPLETED, ST_ERROR, ST_TIMED_OUT, ST_CANCELLED, ST_STALL, ST_NO_DEVICE, ST_OVERFLOW };
#define ERROR_ACCESS (-3)
#define ERROR_NO_DEVICE (-4)
#define ERROR_NOT_FOUND (-5)
#define ERROR_PIPE (-9)
#define QUEUE 64

static libusb_context context;
static libusb_device devs[4] = { { 0 }, { 1 }, { 2 }, { 3 } };
static libusb_device_handle handles[4] = { { &devs[0] }, { &devs[1] }, { &devs[2] }, { &devs[3] } };
static struct usb_device *disk;
static char unplug_path[1100];
static bool unplugged;
static int config;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static struct {
	struct libusb_transfer *t;
	uint64_t at;                    /* when it can finish (ns) */
	bool cancelled;
} queue[QUEUE];
static unsigned queued;
static libusb_hotplug_callback_fn hotplug;
static void *hotplug_user;

static uint64_t ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

API int libusb_init(libusb_context **ctx)
{
	const char *image = getenv("BLUE_LIBUSB_MOCK_IMAGE");
	char err[200];
	if (image && !disk) {
		disk = usbmsd_open(image, false, err, sizeof err);
		if (disk)
			disk->log = getenv("BLUE_LOG_USB") != NULL;
		snprintf(unplug_path, sizeof unplug_path, "%s.unplugged", image);
	}
	*ctx = &context;
	return 0;
}

API void libusb_exit(libusb_context *ctx)
{
	(void)ctx;
}

static bool present(int n)
{
	return n != 0 || (disk && !unplugged);
}

API ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	(void)ctx;
	libusb_device **l = calloc(5, sizeof *l);
	ssize_t n = 0;
	pthread_mutex_lock(&lock);
	for (int k = 0; k < 4; k++)
		if (present(k))
			l[n++] = &devs[k];
	pthread_mutex_unlock(&lock);
	*list = l;
	return n;
}

API void libusb_free_device_list(libusb_device **list, int unref)
{
	(void)unref;
	free(list);
}

API libusb_device *libusb_ref_device(libusb_device *d) { return d; }
API void libusb_unref_device(libusb_device *d) { (void)d; }

API int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *dd)
{
	memset(dd, 0, sizeof *dd);
	dd->bLength = 18;
	dd->bDescriptorType = 1;
	dd->bcdUSB = d->n == 1 ? 0x200 : 0x110;
	dd->bDeviceClass = d->n == 2 ? 9 : 0;
	dd->bMaxPacketSize0 = 64;
	dd->idVendor = d->n == 2 ? 0x05E3 : d->n == 3 ? 0x05AC : 0x1209;
	dd->idProduct = (uint16_t)(d->n == 2 ? 0x0608 : d->n == 3 ? 0x0201 : d->n + 1);
	dd->bNumConfigurations = 1;
	return 0;
}

static const struct libusb_endpoint_descriptor endpoints[2] = {
	{ 7, 5, 0x81, 2, 64, 0, 0, 0, NULL, 0 },
	{ 7, 5, 0x02, 2, 64, 0, 0, 0, NULL, 0 },
};
static const struct libusb_interface_descriptor interface0 = { 9, 4, 0, 0, 2, 8, 6, 0x50, 0, endpoints, NULL, 0 };
static const struct libusb_interface interfaces = { &interface0, 1 };
static struct libusb_config_descriptor config1 = { 9, 2, 32, 1, 1, 0, 0x80, 50, &interfaces, NULL, 0 };
static const struct libusb_endpoint_descriptor keyboard_endpoint = { 7, 5, 0x81, 3, 8, 10, 0, 0, NULL, 0 };
static const struct libusb_interface_descriptor keyboard_interface = { 9, 4, 0, 0, 1, 3, 1, 1, 0, &keyboard_endpoint,
								       NULL, 0 };
static const struct libusb_interface keyboard_interfaces = { &keyboard_interface, 1 };
static struct libusb_config_descriptor keyboard_config = { 9, 2, 34, 1, 1, 0, 0xA0, 50, &keyboard_interfaces, NULL, 0 };

API int libusb_get_config_descriptor_by_value(libusb_device *d, uint8_t value, struct libusb_config_descriptor **c)
{
	if ((d->n != 0 && d->n != 3) || value != 1)
		return ERROR_NOT_FOUND;
	*c = d->n == 3 ? &keyboard_config : &config1;
	return 0;
}

API void libusb_free_config_descriptor(struct libusb_config_descriptor *c) { (void)c; }
API uint8_t libusb_get_bus_number(libusb_device *d) { (void)d; return 1; }
API uint8_t libusb_get_device_address(libusb_device *d) { return (uint8_t)(5 + d->n); }
API int libusb_get_device_speed(libusb_device *d) { return d->n == 1 ? 3 : d->n == 3 ? 1 : 2; }

API int libusb_get_port_numbers(libusb_device *d, uint8_t *ports, int len)
{
	if (len < 1)
		return -2;
	ports[0] = (uint8_t)(d->n == 2 ? 1 : 3 + d->n);
	return 1;
}

API int libusb_open(libusb_device *d, libusb_device_handle **h)
{
	*h = &handles[d->n];
	return 0;
}

API void libusb_close(libusb_device_handle *h) { (void)h; }
static bool detached;

static bool held(libusb_device_handle *h)
{
	return h->dev->n == 0 && getenv("BLUE_LIBUSB_MOCK_HELD") && !detached;
}

API int libusb_kernel_driver_active(libusb_device_handle *h, int n)
{
	(void)n;
	return held(h);
}

API int libusb_detach_kernel_driver(libusb_device_handle *h, int n)
{
	(void)n;
	if (!held(h))
		return ERROR_NOT_FOUND;
#ifdef __APPLE__
	if (strcmp(getprogname(), "blue-usb-helper") != 0)
#endif
		return ERROR_ACCESS;
	detached = true;
	return 0;
}

API int libusb_attach_kernel_driver(libusb_device_handle *h, int n)
{
	(void)n;
	if (h->dev->n == 0)
		detached = false;
	return 0;
}
API int libusb_claim_interface(libusb_device_handle *h, int n) { (void)h; return n == 0 ? 0 : ERROR_NOT_FOUND; }
API int libusb_release_interface(libusb_device_handle *h, int n) { (void)h; (void)n; return 0; }

/* A request carried out at once on the disk (the caller holds the lock) */
static int request(uint8_t type, uint8_t req, uint16_t value, uint16_t index)
{
	struct usb_xfer x = { .type = USB_CONTROL, .setup = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
							      (uint8_t)index, (uint8_t)(index >> 8), 0, 0 } };
	return disk->ops->submit(disk, &x);
}

API int libusb_set_configuration(libusb_device_handle *h, int value)
{
	if (h->dev->n != 0)
		return 0;
	pthread_mutex_lock(&lock);
	int r = unplugged ? ERROR_NO_DEVICE : request(0, 9, (uint16_t)value, 0) == USB_OK ? 0 : ERROR_PIPE;
	if (r == 0)
		config = value;
	pthread_mutex_unlock(&lock);
	return r;
}

API int libusb_get_configuration(libusb_device_handle *h, int *value)
{
	*value = h->dev->n == 0 && !held(h) ? config : 1;      /* (the host's driver configured it) */
	return 0;
}

API int libusb_set_interface_alt_setting(libusb_device_handle *h, int n, int alt)
{
	(void)h;
	return n == 0 && alt == 0 ? 0 : ERROR_NOT_FOUND;
}

API int libusb_clear_halt(libusb_device_handle *h, unsigned char ep)
{
	if (h->dev->n != 0)
		return 0;
	pthread_mutex_lock(&lock);
	int r = unplugged ? ERROR_NO_DEVICE : request(2, 1, 0, ep) == USB_OK ? 0 : ERROR_PIPE;
	pthread_mutex_unlock(&lock);
	return r;
}

API int libusb_reset_device(libusb_device_handle *h)
{
	if (h->dev->n == 0) {
		pthread_mutex_lock(&lock);
		disk->ops->reset(disk);
		request(0, 9, (uint16_t)config, 0);     /* (as the host's reset leaves it: configured again) */
		pthread_mutex_unlock(&lock);
	}
	return 0;
}

API struct libusb_transfer *libusb_alloc_transfer(int iso)
{
	return calloc(1, sizeof(struct libusb_transfer) + (size_t)iso * sizeof(struct libusb_iso_packet_descriptor));
}

API void libusb_free_transfer(struct libusb_transfer *t)
{
	free(t);
}

API int libusb_submit_transfer(struct libusb_transfer *t)
{
	pthread_mutex_lock(&lock);
	int r = 0;
	if (unplugged && t->dev_handle->dev->n == 0)
		r = ERROR_NO_DEVICE;
	else if (queued == QUEUE)
		r = -11;
	else {
		queue[queued].t = t;
		queue[queued].at = ns() + 1000000u;
		queue[queued++].cancelled = false;
		pthread_cond_signal(&cond);
	}
	pthread_mutex_unlock(&lock);
	return r;
}

API int libusb_cancel_transfer(struct libusb_transfer *t)
{
	pthread_mutex_lock(&lock);
	int r = ERROR_NOT_FOUND;
	for (unsigned k = 0; k < queued; k++) {
		if (queue[k].t == t) {
			queue[k].cancelled = true;
			queue[k].at = 0;
			r = 0;
		}
	}
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
	return r;
}

/* Carries out a transfer on the disk (the lock held); false if it waits (NAKed) */
static bool run(struct libusb_transfer *t)
{
	if (unplugged) {
		t->status = ST_NO_DEVICE;
		return true;
	}
	struct usb_xfer x = { .ep = t->endpoint, .type = t->type == 0 ? USB_CONTROL : t->type == 3 ? USB_INTERRUPT :
							    USB_BULK };
	if (t->type == 0) {
		memcpy(x.setup, t->buffer, 8);
		x.ep = t->buffer[0] & 0x80;
		x.data = t->buffer + 8;
		x.len = (uint32_t)t->length - 8;
	} else {
		x.data = t->buffer;
		x.len = (uint32_t)t->length;
	}
	int st = disk->ops->submit(disk, &x);
	if (st == USB_NAK)
		return false;
	t->actual_length = (int)x.actual;
	t->status = st == USB_OK ? ST_COMPLETED : st == USB_STALL ? ST_STALL : st == USB_OVERRUN ? ST_OVERFLOW : ST_ERROR;
	return true;
}

API int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
	(void)ctx;
	(void)completed;
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_nsec += (long)tv->tv_usec * 1000 + 1000000;    /* (a millisecond at least: transfers take one) */
	until.tv_sec += tv->tv_sec + until.tv_nsec / 1000000000;
	until.tv_nsec %= 1000000000;
	struct libusb_transfer *done[QUEUE];
	unsigned n = 0;
	int event = 0;
	pthread_mutex_lock(&lock);
	pthread_cond_timedwait(&cond, &lock, &until);
	bool gone = disk && access(unplug_path, F_OK) == 0;
	if (disk && gone != unplugged) {
		unplugged = gone;
		event = gone ? 2 : 1;
		if (!gone) {
			disk->ops->reset(disk);
			config = 0;
		}
	}
	uint64_t now = ns();
	for (unsigned k = 0; k < queued;) {
		struct libusb_transfer *t = queue[k].t;
		bool finished = false;
		if (queue[k].cancelled) {
			t->status = ST_CANCELLED;
			finished = true;
		} else if (now >= queue[k].at) {
			finished = t->dev_handle->dev->n != 0 ? (t->status = ST_COMPLETED, true) : run(t);
		}
		if (finished) {
			done[n++] = t;
			queue[k] = queue[--queued];
		} else {
			k++;
		}
	}
	pthread_mutex_unlock(&lock);
	if (event && hotplug)
		hotplug(&context, &devs[0], event, hotplug_user);
	for (unsigned k = 0; k < n; k++)
		done[k]->callback(done[k]);
	return 0;
}

API const char *libusb_error_name(int r)
{
	return r == ERROR_NO_DEVICE ? "LIBUSB_ERROR_NO_DEVICE" : r == ERROR_PIPE ? "LIBUSB_ERROR_PIPE" : "LIBUSB_ERROR";
}

API void libusb_interrupt_event_handler(libusb_context *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&lock);
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
}

API int libusb_has_capability(uint32_t cap)
{
	return cap == 1;
}

API int libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags, int vendor, int product,
					 int cls, libusb_hotplug_callback_fn cb, void *user,
					 libusb_hotplug_callback_handle *handle)
{
	(void)ctx, (void)events, (void)flags, (void)vendor, (void)product, (void)cls;
	hotplug = cb;
	hotplug_user = user;
	*handle = 1;
	return 0;
}

API void libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle handle)
{
	(void)ctx, (void)handle;
	hotplug = NULL;
}

API const struct libusb_version *libusb_get_version(void)
{
	static const struct libusb_version v = { 1, 0, 99, 0, "", "Blue's stand-in for tests" };
	return &v;
}
