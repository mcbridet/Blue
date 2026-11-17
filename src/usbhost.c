#include "usbhost.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "hostlib.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

/* libusb-1.0's interface, as far as Blue uses it (libusb.h's; its ABI is stable) */
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
typedef int (*libusb_hotplug_callback_fn)(libusb_context *, libusb_device *, int event, void *user_data);

enum {
	LIBUSB_ERROR_ACCESS = -3, LIBUSB_ERROR_NO_DEVICE = -4, LIBUSB_ERROR_NOT_FOUND = -5, LIBUSB_ERROR_BUSY = -6,
	LIBUSB_ERROR_PIPE = -9, LIBUSB_ERROR_NOT_SUPPORTED = -12,
};
enum { XFER_CONTROL, XFER_ISO, XFER_BULK, XFER_INTERRUPT };
enum { ST_COMPLETED, ST_ERROR, ST_TIMED_OUT, ST_CANCELLED, ST_STALL, ST_NO_DEVICE, ST_OVERFLOW };
enum { SPEED_UNKNOWN, SPEED_LOW, SPEED_FULL, SPEED_HIGH, SPEED_SUPER };
enum { HOTPLUG_ARRIVED = 1, HOTPLUG_LEFT = 2, HOTPLUG_ANY = -1, CAP_HAS_HOTPLUG = 1 };

struct libusb {
	int (*init)(libusb_context **);
	void (*exit)(libusb_context *);
	ssize_t (*get_device_list)(libusb_context *, libusb_device ***);
	void (*free_device_list)(libusb_device **, int);
	libusb_device *(*ref_device)(libusb_device *);
	void (*unref_device)(libusb_device *);
	int (*get_device_descriptor)(libusb_device *, struct libusb_device_descriptor *);
	int (*get_config_descriptor_by_value)(libusb_device *, uint8_t, struct libusb_config_descriptor **);
	void (*free_config_descriptor)(struct libusb_config_descriptor *);
	uint8_t (*get_bus_number)(libusb_device *);
	int (*get_port_numbers)(libusb_device *, uint8_t *, int);
	uint8_t (*get_device_address)(libusb_device *);
	int (*get_device_speed)(libusb_device *);
	int (*open)(libusb_device *, libusb_device_handle **);
	void (*close)(libusb_device_handle *);
	int (*kernel_driver_active)(libusb_device_handle *, int);
	int (*detach_kernel_driver)(libusb_device_handle *, int);
	int (*attach_kernel_driver)(libusb_device_handle *, int);
	int (*claim_interface)(libusb_device_handle *, int);
	int (*release_interface)(libusb_device_handle *, int);
	int (*set_configuration)(libusb_device_handle *, int);
	int (*get_configuration)(libusb_device_handle *, int *);
	int (*set_interface_alt_setting)(libusb_device_handle *, int, int);
	int (*clear_halt)(libusb_device_handle *, unsigned char);
	int (*reset_device)(libusb_device_handle *);
	struct libusb_transfer *(*alloc_transfer)(int);
	void (*free_transfer)(struct libusb_transfer *);
	int (*submit_transfer)(struct libusb_transfer *);
	int (*cancel_transfer)(struct libusb_transfer *);
	int (*handle_events_timeout_completed)(libusb_context *, struct timeval *, int *);
	const char *(*error_name)(int);
	/* (not in every version) */
	void (*interrupt_event_handler)(libusb_context *);
	int (*has_capability)(uint32_t);
	int (*hotplug_register_callback)(libusb_context *, int, int, int, int, int, libusb_hotplug_callback_fn, void *,
					 libusb_hotplug_callback_handle *);
	void (*hotplug_deregister_callback)(libusb_context *, libusb_hotplug_callback_handle);
};

#define MAX_OPEN 8
#define MAX_ARRIVALS 16
#define CONTROL_TIMEOUT_MS 5000
#define ISO_XFERS 3                     /* isochronous transfers in flight on an endpoint ... */
#define ISO_PACKETS 8                   /* ... of this many packets (frames) */
#define ISO_RING 32                     /* packets received, waiting for Mac OS */
#define EP_INDEX(addr) (((addr) & 15) | ((addr) & USB_DIR_IN ? 16 : 0))

struct usbhost {
	void *lib;
	struct libusb u;
	libusb_context *ctx;
	pthread_t thread;
	atomic_bool stop;
	pthread_mutex_t lock;           /* the lists below, against libusb's thread */
	struct hostdev *open[MAX_OPEN];
	struct usbhost_info arrived[MAX_ARRIVALS];
	unsigned arrivals;
	bool hotplug;
	libusb_hotplug_callback_handle hotplug_handle;
};

struct hostreq {
	struct libusb_transfer *t;
	struct usb_xfer *x;             /* the controller's (NULL once given up) */
	struct hostdev *hd;
	struct hostreq *next;
};

struct iso_stream {
	uint8_t ep;
	unsigned mps;
	struct libusb_transfer *t[ISO_XFERS];
	bool busy[ISO_XFERS];           /* submitted */
	bool started;                   /* (in: the transfers are going round) */
	bool stopping;
	unsigned filling, filled, fill_len;     /* (out: the transfer being filled, its packets and bytes) */
	unsigned head, count;           /* (in: packets received) */
	uint16_t len[ISO_RING];
	uint8_t data[ISO_RING][1023];
};

struct hostdev {
	struct usb_device dev;
	struct usbhost *h;
	struct usbhost_info info;
	libusb_device *ldev;
	libusb_device_handle *handle;
	uint8_t bus, addr;
	uint32_t claimed;               /* interfaces claimed (bit n: number n) */
	uint32_t detached;              /* ... whose host driver was detached */
	bool configured;                /* Mac OS has configured it (a bus reset goes on to it) */
	uint8_t ep_type[32], ep_iface[32];      /* (index: EP_INDEX) */
	uint16_t ep_mps[32];
	pthread_mutex_t lock;           /* its transfers, against libusb's thread */
	struct hostreq *reqs;           /* in flight */
	struct iso_stream *iso[32];
	atomic_bool gone;
};

static const struct usb_device_ops host_ops;

static struct hostdev *host_of(const struct usb_device *d)
{
	return (struct hostdev *)d;
}

static const char *error_name(const struct usbhost *h, int r)
{
	return h->u.error_name ? h->u.error_name(r) : "error";
}

/* The host's names for it: the maker's and the product's */
static void join_names(char *out, size_t size, const char *maker, const char *product)
{
	if (!*product)
		snprintf(out, size, "%s", maker);
	else if (!*maker || !strncmp(product, maker, strlen(maker)))
		snprintf(out, size, "%s", product);
	else
		snprintf(out, size, "%s %s", maker, product);
}

#ifdef __APPLE__
static long apple_number(io_service_t s, CFStringRef key)
{
	long v = -1;
	CFTypeRef p = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
	if (p && CFGetTypeID(p) == CFNumberGetTypeID())
		CFNumberGetValue(p, kCFNumberLongType, &v);
	if (p)
		CFRelease(p);
	return v;
}

static void apple_string(io_service_t s, const char *const *keys, char *out, size_t size)
{
	*out = 0;
	for (; *keys && !*out; keys++) {
		CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, *keys, kCFStringEncodingUTF8);
		CFTypeRef p = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
		if (p && CFGetTypeID(p) == CFStringGetTypeID() && !CFStringGetCString(p, out, (CFIndex)size, kCFStringEncodingUTF8))
			*out = 0;
		if (p)
			CFRelease(p);
		CFRelease(key);
	}
}

/* (from the I/O Registry: libusb would have to open the device, which takes it from macOS) */
static void host_names(struct usbhost_info *info, uint8_t bus, uint8_t addr)
{
	static const char *const product[] = { "USB Product Name", "kUSBProductString", NULL };
	static const char *const maker[] = { "USB Vendor Name", "kUSBVendorString", NULL };
	io_iterator_t it;
	if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS)
		return;
	io_service_t s;
	while ((s = IOIteratorNext(it))) {
		if ((apple_number(s, CFSTR("locationID")) >> 24) == bus && apple_number(s, CFSTR("USB Address")) == addr &&
		    apple_number(s, CFSTR("idVendor")) == info->vendor && apple_number(s, CFSTR("idProduct")) == info->product) {
			char m[64], p[64];
			apple_string(s, maker, m, sizeof m);
			apple_string(s, product, p, sizeof p);
			join_names(info->name, sizeof info->name, m, p);
		}
		IOObjectRelease(s);
	}
	IOObjectRelease(it);
}
#else
static void sysfs_string(const char *id, const char *what, char *out, size_t size)
{
	char path[96];
	snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/%s", id, what);
	*out = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (!fgets(out, (int)size, f))
		*out = 0;
	fclose(f);
	out[strcspn(out, "\n")] = 0;
}

static void host_names(struct usbhost_info *info, uint8_t bus, uint8_t addr)
{
	(void)bus;
	(void)addr;
	char m[64], p[64];
	sysfs_string(info->id, "manufacturer", m, sizeof m);
	sysfs_string(info->id, "product", p, sizeof p);
	join_names(info->name, sizeof info->name, m, p);
}
#endif

/* What libusb knows of a device without opening it; false for hubs */
static bool describe(struct usbhost *h, libusb_device *dev, struct usbhost_info *info)
{
	struct libusb_device_descriptor dd;
	if (h->u.get_device_descriptor(dev, &dd) != 0 || dd.bDeviceClass == 9)
		return false;
	memset(info, 0, sizeof *info);
	info->vendor = dd.idVendor;
	info->product = dd.idProduct;
	uint8_t ports[8];
	int n = h->u.get_port_numbers(dev, ports, (int)sizeof ports);
	int at = snprintf(info->id, sizeof info->id, "%u-", h->u.get_bus_number(dev));
	for (int k = 0; k < n && at < (int)sizeof info->id; k++)
		at += snprintf(info->id + at, sizeof info->id - (size_t)at, k ? ".%u" : "%u", ports[k]);
	switch (h->u.get_device_speed(dev)) {
	case SPEED_LOW: info->speed = USB_SPEED_LOW; break;
	case SPEED_HIGH: info->speed = USB_SPEED_HIGH; break;
	case SPEED_UNKNOWN:
	case SPEED_FULL: info->speed = USB_SPEED_FULL; break;
	default: info->speed = USB_SPEED_SUPER; break;
	}
	return true;
}

bool usbhost_matches(const struct usbhost_info *info, const char *spec)
{
	unsigned vid, pid;
	char extra;
	if (strchr(spec, ':') && sscanf(spec, "%x:%x%c", &vid, &pid, &extra) == 2)
		return vid == info->vendor && pid == info->product;
	return !strcmp(spec, info->id);
}

static int on_hotplug(libusb_context *ctx, libusb_device *dev, int event, void *user)
{
	(void)ctx;
	struct usbhost *h = user;
	pthread_mutex_lock(&h->lock);
	if (event == HOTPLUG_LEFT) {
		for (unsigned k = 0; k < MAX_OPEN; k++)
			if (h->open[k] && h->open[k]->ldev == dev)
				atomic_store(&h->open[k]->gone, true);
	} else if (event == HOTPLUG_ARRIVED && h->arrivals < MAX_ARRIVALS) {
		if (describe(h, dev, &h->arrived[h->arrivals]))
			h->arrivals++;
	}
	pthread_mutex_unlock(&h->lock);
	return 0;
}

static void *events(void *arg)
{
	struct usbhost *h = arg;
	while (!atomic_load(&h->stop)) {
		struct timeval tv = { 0, 100000 };
		h->u.handle_events_timeout_completed(h->ctx, &tv, NULL);
	}
	return NULL;
}

struct usbhost *usbhost_start(char *err, size_t size)
{
	struct usbhost *h = calloc(1, sizeof *h);
	if (!h) {
		snprintf(err, size, "out of memory");
		return NULL;
	}
	h->lib = hostlib_open(HOSTLIB_USB);
	if (!h->lib) {
		snprintf(err, size, "libusb-1.0 isn't found, so devices can't be taken from the host (%s)", dlerror());
		free(h);
		return NULL;
	}
	static const char *const names[] = {
		"libusb_init", "libusb_exit", "libusb_get_device_list", "libusb_free_device_list", "libusb_ref_device",
		"libusb_unref_device", "libusb_get_device_descriptor", "libusb_get_config_descriptor_by_value",
		"libusb_free_config_descriptor", "libusb_get_bus_number", "libusb_get_port_numbers",
		"libusb_get_device_address", "libusb_get_device_speed", "libusb_open", "libusb_close",
		"libusb_kernel_driver_active", "libusb_detach_kernel_driver", "libusb_attach_kernel_driver",
		"libusb_claim_interface", "libusb_release_interface", "libusb_set_configuration",
		"libusb_get_configuration", "libusb_set_interface_alt_setting", "libusb_clear_halt", "libusb_reset_device",
		"libusb_alloc_transfer", "libusb_free_transfer", "libusb_submit_transfer", "libusb_cancel_transfer",
		"libusb_handle_events_timeout_completed", "libusb_error_name", "libusb_interrupt_event_handler",
		"libusb_has_capability", "libusb_hotplug_register_callback", "libusb_hotplug_deregister_callback",
	};
	void **fn = (void **)&h->u;
	for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
		fn[k] = dlsym(h->lib, names[k]);
		if (!fn[k] && k < 31) {
			snprintf(err, size, "the libusb found has no %s (version 1.0.16 or later is needed)", names[k]);
			dlclose(h->lib);
			free(h);
			return NULL;
		}
	}
	int r = h->u.init(&h->ctx);
	if (r != 0) {
		snprintf(err, size, "libusb can't start (%s)", error_name(h, r));
		dlclose(h->lib);
		free(h);
		return NULL;
	}
	pthread_mutex_init(&h->lock, NULL);
	if (h->u.has_capability && h->u.hotplug_register_callback && h->u.has_capability(CAP_HAS_HOTPLUG))
		h->hotplug = h->u.hotplug_register_callback(h->ctx, HOTPLUG_ARRIVED | HOTPLUG_LEFT, 0, HOTPLUG_ANY,
							    HOTPLUG_ANY, HOTPLUG_ANY, on_hotplug, h,
							    &h->hotplug_handle) == 0;
	if (pthread_create(&h->thread, NULL, events, h) != 0) {
		snprintf(err, size, "libusb's thread can't start");
		h->u.exit(h->ctx);
		dlclose(h->lib);
		free(h);
		return NULL;
	}
	return h;
}

void usbhost_stop(struct usbhost *h)
{
	if (!h)
		return;
	if (h->hotplug && h->u.hotplug_deregister_callback)
		h->u.hotplug_deregister_callback(h->ctx, h->hotplug_handle);
	atomic_store(&h->stop, true);
	if (h->u.interrupt_event_handler)
		h->u.interrupt_event_handler(h->ctx);
	pthread_join(h->thread, NULL);
	h->u.exit(h->ctx);
	pthread_mutex_destroy(&h->lock);
	free(h);                        /* (libusb stays loaded) */
}

unsigned usbhost_list(struct usbhost *h, struct usbhost_info *out, unsigned max)
{
	libusb_device **list;
	ssize_t n = h->u.get_device_list(h->ctx, &list);
	unsigned found = 0;
	for (ssize_t k = 0; k < n; k++) {
		struct usbhost_info info;
		if (!describe(h, list[k], &info))
			continue;
		if (found < max) {
			host_names(&info, h->u.get_bus_number(list[k]), h->u.get_device_address(list[k]));
			out[found] = info;
		}
		found++;
	}
	if (n >= 0)
		h->u.free_device_list(list, 1);
	return found;
}

bool usbhost_arrived(struct usbhost *h, struct usbhost_info *info)
{
	pthread_mutex_lock(&h->lock);
	bool any = h->arrivals > 0;
	if (any) {
		*info = h->arrived[0];
		memmove(h->arrived, h->arrived + 1, --h->arrivals * sizeof *h->arrived);
	}
	pthread_mutex_unlock(&h->lock);
	return any;
}

bool usbhost_is(const struct usb_device *d)
{
	return d->ops == &host_ops;
}

const struct usbhost_info *usbhost_device_info(const struct usb_device *d)
{
	return &host_of(d)->info;
}

bool usbhost_gone(const struct usb_device *d)
{
	return atomic_load(&host_of(d)->gone);
}

/* Each endpoint's type and packet size, from configuration `value`'s descriptors (all alternate settings) */
static void learn_endpoints(struct hostdev *hd, int value)
{
	struct usbhost *h = hd->h;
	memset(hd->ep_type, 0, sizeof hd->ep_type);
	memset(hd->ep_mps, 0, sizeof hd->ep_mps);
	struct libusb_config_descriptor *c;
	if (value <= 0 || h->u.get_config_descriptor_by_value(hd->ldev, (uint8_t)value, &c) != 0)
		return;
	for (int i = 0; i < c->bNumInterfaces; i++) {
		for (int a = 0; a < c->interface[i].num_altsetting; a++) {
			const struct libusb_interface_descriptor *id = &c->interface[i].altsetting[a];
			for (int e = 0; e < id->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
				unsigned k = EP_INDEX(ed->bEndpointAddress);
				hd->ep_type[k] = ed->bmAttributes & 3;
				hd->ep_iface[k] = id->bInterfaceNumber;
				if ((ed->wMaxPacketSize & 0x7FF) > hd->ep_mps[k])
					hd->ep_mps[k] = ed->wMaxPacketSize & 0x7FF;
			}
		}
	}
	h->u.free_config_descriptor(c);
}

static void release_all(struct hostdev *hd, bool give_back)
{
	struct usbhost *h = hd->h;
	for (int n = 0; n < 32; n++) {
		if (hd->claimed & (1u << n))
			h->u.release_interface(hd->handle, n);
		if (give_back && (hd->detached & (1u << n)))
			h->u.attach_kernel_driver(hd->handle, n);
	}
	hd->claimed = 0;
	if (give_back)
		hd->detached = 0;
}

/* Claims configuration `value`'s interfaces, detaching the host's drivers from them; false, with `err`, if one can't be */
static bool claim_all(struct hostdev *hd, int value, char *err, size_t size)
{
	struct usbhost *h = hd->h;
	struct libusb_config_descriptor *c;
	learn_endpoints(hd, value);
	if (value <= 0)
		return true;
	if (h->u.get_config_descriptor_by_value(hd->ldev, (uint8_t)value, &c) != 0) {
		snprintf(err, size, "%s's configuration %d can't be read", hd->dev.name, value);
		return false;
	}
	bool ok = true;
	for (int i = 0; i < c->bNumInterfaces && ok; i++) {
		int n = c->interface[i].altsetting[0].bInterfaceNumber;
		if (n >= 32 || (hd->claimed & (1u << n)))
			continue;
		if (h->u.kernel_driver_active(hd->handle, n) == 1) {
			int r = h->u.detach_kernel_driver(hd->handle, n);
			if (r == 0) {
				hd->detached |= 1u << n;
			} else if (r != LIBUSB_ERROR_NOT_FOUND && r != LIBUSB_ERROR_NOT_SUPPORTED) {
#ifdef __APPLE__
				snprintf(err, size, "macOS's own driver has %s: Blue can take a device from macOS only "
					 "when it's run as root (with sudo)%s", hd->dev.name,
					 geteuid() == 0 ? " (and here macOS still won't let it go)" : "");
#else
				snprintf(err, size, "the host's driver has %s and won't let it go (%s)", hd->dev.name,
					 error_name(h, r));
#endif
				ok = false;
				break;
			}
		}
		int r = h->u.claim_interface(hd->handle, n);
		if (r != 0) {
#ifdef __APPLE__
			if (geteuid() != 0)
				snprintf(err, size, "macOS's own driver has %s: Blue can take a device from macOS only when "
					 "it's run as root (with sudo)", hd->dev.name);
			else
#endif
			snprintf(err, size, "%s is in use on the host (interface %d: %s)", hd->dev.name, n, error_name(h, r));
			ok = false;
			break;
		}
		hd->claimed |= 1u << n;
	}
	h->u.free_config_descriptor(c);
	return ok;
}

struct usb_device *usbhost_open(struct usbhost *h, const char *spec, char *err, size_t size)
{
	libusb_device **list;
	ssize_t n = h->u.get_device_list(h->ctx, &list);
	libusb_device *dev = NULL;
	struct usbhost_info info;
	for (ssize_t k = 0; k < n && !dev; k++)
		if (describe(h, list[k], &info) && usbhost_matches(&info, spec))
			dev = h->u.ref_device(list[k]);
	if (n >= 0)
		h->u.free_device_list(list, 1);
	if (!dev) {
		snprintf(err, size, "there's no USB device %s on the host", spec);
		return NULL;
	}
	uint8_t bus = h->u.get_bus_number(dev), addr = h->u.get_device_address(dev);
	host_names(&info, bus, addr);
	char name[160];
	if (*info.name)
		snprintf(name, sizeof name, "%s (%04x:%04x)", info.name, info.vendor, info.product);
	else
		snprintf(name, sizeof name, "%04x:%04x", info.vendor, info.product);
	if (info.speed >= USB_SPEED_HIGH) {
		snprintf(err, size, "%s is connected at %s speed: the iMac's USB is 1.1, so it can't be passed through "
			 "(through a USB 1.1 hub it would connect at full speed)", name,
			 info.speed == USB_SPEED_HIGH ? "high (USB 2.0)" : "super (USB 3)");
		h->u.unref_device(dev);
		return NULL;
	}
	libusb_device_handle *handle;
	int r = h->u.open(dev, &handle);
	if (r != 0) {
#ifdef __APPLE__
		snprintf(err, size, "%s can't be opened (%s)", name, error_name(h, r));
#else
		if (r == LIBUSB_ERROR_ACCESS)
			snprintf(err, size, "there's no permission to use %s (/dev/bus/usb/%03u/%03u): a udev rule can "
				 "give it (see the README)", name, bus, addr);
		else
			snprintf(err, size, "%s can't be opened (%s)", name, error_name(h, r));
#endif
		h->u.unref_device(dev);
		return NULL;
	}
	struct hostdev *hd = calloc(1, sizeof *hd);
	if (!hd) {
		h->u.close(handle);
		h->u.unref_device(dev);
		snprintf(err, size, "out of memory");
		return NULL;
	}
	hd->h = h;
	hd->info = info;
	hd->ldev = dev;
	hd->handle = handle;
	hd->bus = bus;
	hd->addr = addr;
	pthread_mutex_init(&hd->lock, NULL);
	atomic_init(&hd->gone, false);
	usb_device_init(&hd->dev, &host_ops, info.speed, name);
	int config = 0;
	if (h->u.get_configuration(handle, &config) != 0)
		config = 0;
	if (!claim_all(hd, config, err, size)) {
		release_all(hd, true);
		h->u.close(handle);
		h->u.unref_device(dev);
		pthread_mutex_destroy(&hd->lock);
		free(hd);
		return NULL;
	}
	pthread_mutex_lock(&h->lock);
	for (unsigned k = 0; k < MAX_OPEN; k++) {
		if (!h->open[k]) {
			h->open[k] = hd;
			break;
		}
	}
	pthread_mutex_unlock(&h->lock);
	return &hd->dev;
}

/* libusb's thread: a transfer is finished (or given up). */
static void finished(struct libusb_transfer *t)
{
	struct hostreq *r = t->user_data;
	struct hostdev *hd = r->hd;
	pthread_mutex_lock(&hd->lock);
	struct usb_xfer *x = r->x;
	if (t->status == ST_NO_DEVICE)
		atomic_store(&hd->gone, true);
	if (x) {
		uint32_t got = t->actual_length > 0 ? (uint32_t)t->actual_length : 0;
		const uint8_t *data = t->buffer + (t->type == XFER_CONTROL ? 8 : 0);
		if (got > x->len)
			got = x->len;
		if (x->ep & USB_DIR_IN)
			memcpy(x->data, data, got);
		x->actual = got;
		switch (t->status) {
		case ST_COMPLETED: x->status = USB_OK; break;
		case ST_STALL: x->status = USB_STALL; break;
		case ST_OVERFLOW: x->status = USB_OVERRUN; break;
		case ST_NO_DEVICE: x->status = USB_NODEV; break;
		default: x->status = USB_ERROR; break;
		}
		if (hd->dev.log)
			printf("usb: %s: endpoint %02X: %u bytes, libusb status %d\n", hd->dev.name, x->ep, got, t->status);
		atomic_store_explicit(&x->done, true, memory_order_release);
	}
	for (struct hostreq **p = &hd->reqs; *p; p = &(*p)->next) {
		if (*p == r) {
			*p = r->next;
			break;
		}
	}
	pthread_mutex_unlock(&hd->lock);
	free(t->buffer);
	hd->h->u.free_transfer(t);
	free(r);
}

static int submit(struct hostdev *hd, struct usb_xfer *x, unsigned type, uint8_t *buf, uint32_t len)
{
	struct usbhost *h = hd->h;
	struct hostreq *r = calloc(1, sizeof *r);
	struct libusb_transfer *t = r ? h->u.alloc_transfer(0) : NULL;
	if (!t) {
		free(r);
		free(buf);
		return USB_ERROR;
	}
	t->dev_handle = hd->handle;
	t->endpoint = type == XFER_CONTROL ? 0 : x->ep;
	t->type = (unsigned char)type;
	t->timeout = type == XFER_CONTROL ? CONTROL_TIMEOUT_MS : 0;
	t->buffer = buf;
	t->length = (int)len;
	t->callback = finished;
	t->user_data = r;
	r->t = t;
	r->x = x;
	r->hd = hd;
	pthread_mutex_lock(&hd->lock);
	int e = h->u.submit_transfer(t);
	if (e == 0) {
		r->next = hd->reqs;
		hd->reqs = r;
	}
	pthread_mutex_unlock(&hd->lock);
	if (e != 0) {
		if (e == LIBUSB_ERROR_NO_DEVICE)
			atomic_store(&hd->gone, true);
		h->u.free_transfer(t);
		free(r);
		free(buf);
		return e == LIBUSB_ERROR_NO_DEVICE ? USB_NODEV : e == LIBUSB_ERROR_PIPE ? USB_STALL : USB_ERROR;
	}
	return USB_PENDING;
}

static void stop_iso(struct hostdev *hd, unsigned k);

/* SET_CONFIGURATION, SET_INTERFACE and CLEAR_FEATURE (ENDPOINT_HALT) are carried out with libusb's calls. */
static int control(struct hostdev *hd, struct usb_xfer *x)
{
	struct usbhost *h = hd->h;
	const uint8_t *s = x->setup;
	uint16_t value = usb_w16(s + 2), index = usb_w16(s + 4), length = usb_w16(s + 6);
	x->actual = 0;
	switch (s[0] << 8 | s[1]) {
	case 0x0009: {                                  /* SET_CONFIGURATION */
		for (unsigned k = 0; k < 32; k++)
			stop_iso(hd, k);
		release_all(hd, false);
		int now = 0, r = h->u.set_configuration(hd->handle, value);
		if (r != 0 && (h->u.get_configuration(hd->handle, &now) != 0 || now != value)) {
			if (hd->dev.log)
				printf("usb: %s: configuration %u: %s\n", hd->dev.name, value, error_name(h, r));
			claim_all(hd, now, (char[160]){ 0 }, 160);
			return r == LIBUSB_ERROR_NO_DEVICE ? USB_NODEV : USB_STALL;
		}
		char err[200];
		if (!claim_all(hd, value, err, sizeof err))
			fprintf(stderr, "usb: %s\n", err);
		hd->configured = value != 0;
		return USB_OK;
	}
	case 0x010B: {                                  /* SET_INTERFACE */
		for (unsigned k = 0; k < 32; k++)
			if (hd->ep_iface[k] == index)
				stop_iso(hd, k);
		int r = h->u.set_interface_alt_setting(hd->handle, index, value);
		if (r != 0 && hd->dev.log)
			printf("usb: %s: interface %u setting %u: %s\n", hd->dev.name, index, value, error_name(h, r));
		return r == 0 ? USB_OK : r == LIBUSB_ERROR_NO_DEVICE ? USB_NODEV : USB_STALL;
	}
	case 0x0201:                                    /* CLEAR_FEATURE (ENDPOINT_HALT) */
		if (value == 0) {
			int r = h->u.clear_halt(hd->handle, (unsigned char)index);
			return r == 0 ? USB_OK : r == LIBUSB_ERROR_NO_DEVICE ? USB_NODEV : USB_STALL;
		}
		break;
	}
	uint8_t *buf = malloc(8u + length);
	if (!buf)
		return USB_ERROR;
	memcpy(buf, s, 8);
	if (!(s[0] & USB_DIR_IN))
		memcpy(buf + 8, x->data, x->len < length ? x->len : length);
	return submit(hd, x, XFER_CONTROL, buf, 8u + length);
}

/* libusb's thread: an isochronous transfer is finished. */
static void iso_finished(struct libusb_transfer *t)
{
	struct hostdev *hd = t->user_data;
	unsigned k = EP_INDEX(t->endpoint);
	pthread_mutex_lock(&hd->lock);
	struct iso_stream *s = hd->iso[k];
	unsigned n = 0;
	while (n < ISO_XFERS && s->t[n] != t)
		n++;
	if (t->status == ST_NO_DEVICE)
		atomic_store(&hd->gone, true);
	if ((t->endpoint & USB_DIR_IN) && t->status == ST_COMPLETED) {
		unsigned at = 0;
		for (int p = 0; p < t->num_iso_packets; p++) {
			const struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[p];
			if (d->status == ST_COMPLETED) {
				if (s->count == ISO_RING) {     /* (Mac OS isn't keeping up: the oldest goes) */
					s->head = (s->head + 1) % ISO_RING;
					s->count--;
				}
				unsigned slot = (s->head + s->count++) % ISO_RING;
				s->len[slot] = (uint16_t)(d->actual_length < 1023 ? d->actual_length : 1023);
				memcpy(s->data[slot], t->buffer + at, s->len[slot]);
			}
			at += d->length;
		}
	}
	bool again = (t->endpoint & USB_DIR_IN) && !s->stopping && !atomic_load(&hd->gone) &&
		     t->status != ST_CANCELLED && hd->h->u.submit_transfer(t) == 0;
	if (n < ISO_XFERS)
		s->busy[n] = again;
	pthread_mutex_unlock(&hd->lock);
}

static struct iso_stream *iso_stream(struct hostdev *hd, uint8_t ep)
{
	unsigned k = EP_INDEX(ep);
	if (hd->iso[k])
		return hd->iso[k];
	struct iso_stream *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->ep = ep;
	s->mps = hd->ep_mps[k] ? hd->ep_mps[k] : 1023;
	for (unsigned n = 0; n < ISO_XFERS; n++) {
		struct libusb_transfer *t = hd->h->u.alloc_transfer(ISO_PACKETS);
		uint8_t *buf = t ? malloc(s->mps * ISO_PACKETS) : NULL;
		if (!buf) {
			if (t)
				hd->h->u.free_transfer(t);
			for (unsigned m = 0; m < n; m++) {
				free(s->t[m]->buffer);
				hd->h->u.free_transfer(s->t[m]);
			}
			free(s);
			return NULL;
		}
		t->dev_handle = hd->handle;
		t->endpoint = ep;
		t->type = XFER_ISO;
		t->buffer = buf;
		t->callback = iso_finished;
		t->user_data = hd;
		s->t[n] = t;
	}
	pthread_mutex_lock(&hd->lock);
	hd->iso[k] = s;
	pthread_mutex_unlock(&hd->lock);
	return s;
}

/* Isochronous packets are answered at once: in, from those received (none yet: empty); out, sent in batches. */
static int iso_packet(struct hostdev *hd, struct usb_xfer *x)
{
	struct iso_stream *s = iso_stream(hd, x->ep);
	if (!s)
		return USB_ERROR;
	pthread_mutex_lock(&hd->lock);
	int status = USB_OK;
	if (x->ep & USB_DIR_IN) {
		if (!s->started) {
			s->started = true;
			for (unsigned n = 0; n < ISO_XFERS; n++) {
				struct libusb_transfer *t = s->t[n];
				t->num_iso_packets = ISO_PACKETS;
				t->length = (int)(s->mps * ISO_PACKETS);
				for (unsigned p = 0; p < ISO_PACKETS; p++)
					t->iso_packet_desc[p].length = s->mps;
				s->busy[n] = hd->h->u.submit_transfer(t) == 0;
			}
		}
		if (s->count) {
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
	} else {
		struct libusb_transfer *t = s->t[s->filling];
		if (!s->busy[s->filling] && x->len <= s->mps) {
			memcpy(t->buffer + s->fill_len, x->data, x->len);
			t->iso_packet_desc[s->filled].length = x->len;
			s->fill_len += x->len;
			x->actual = x->len;
			if (++s->filled == ISO_PACKETS) {
				t->num_iso_packets = ISO_PACKETS;
				t->length = (int)s->fill_len;
				s->busy[s->filling] = hd->h->u.submit_transfer(t) == 0;
				s->filling = (s->filling + 1) % ISO_XFERS;
				s->filled = s->fill_len = 0;
			}
		} else {
			x->actual = x->len;     /* (they're all in flight: this one's dropped) */
		}
	}
	pthread_mutex_unlock(&hd->lock);
	return status;
}

/* Waits for libusb's thread to finish (or give up) what's in flight: up to 2 seconds */
static bool settled(struct hostdev *hd, unsigned k)
{
	for (int tries = 0; tries < 2000; tries++) {
		pthread_mutex_lock(&hd->lock);
		bool busy = k < 32 ? false : hd->reqs != NULL;
		if (k < 32 && hd->iso[k])
			for (unsigned n = 0; n < ISO_XFERS; n++)
				busy |= hd->iso[k]->busy[n];
		pthread_mutex_unlock(&hd->lock);
		if (!busy)
			return true;
		usleep(1000);
	}
	return false;
}

static void stop_iso(struct hostdev *hd, unsigned k)
{
	struct iso_stream *s = hd->iso[k];
	if (!s)
		return;
	pthread_mutex_lock(&hd->lock);
	s->stopping = true;
	for (unsigned n = 0; n < ISO_XFERS; n++)
		if (s->busy[n])
			hd->h->u.cancel_transfer(s->t[n]);
	pthread_mutex_unlock(&hd->lock);
	if (!settled(hd, k))
		return;                 /* (left: libusb may yet touch it) */
	pthread_mutex_lock(&hd->lock);
	hd->iso[k] = NULL;
	pthread_mutex_unlock(&hd->lock);
	for (unsigned n = 0; n < ISO_XFERS; n++) {
		free(s->t[n]->buffer);
		hd->h->u.free_transfer(s->t[n]);
	}
	free(s);
}

static int host_submit(struct usb_device *d, struct usb_xfer *x)
{
	struct hostdev *hd = host_of(d);
	x->actual = 0;
	if (atomic_load(&hd->gone))
		return USB_NODEV;
	if (x->type == USB_CONTROL)
		return control(hd, x);
	unsigned type = hd->ep_type[EP_INDEX(x->ep)];
	if (type == USB_CONTROL)
		type = x->type;         /* (not in its descriptors: as the controller found it) */
	if (type == USB_ISO)
		return iso_packet(hd, x);
	uint8_t *buf = malloc(x->len ? x->len : 1);
	if (!buf)
		return USB_ERROR;
	if (!(x->ep & USB_DIR_IN))
		memcpy(buf, x->data, x->len);
	return submit(hd, x, type == USB_INTERRUPT ? XFER_INTERRUPT : XFER_BULK, buf, x->len);
}

static void host_cancel(struct usb_device *d, struct usb_xfer *x)
{
	struct hostdev *hd = host_of(d);
	pthread_mutex_lock(&hd->lock);
	for (struct hostreq *r = hd->reqs; r; r = r->next) {
		if (r->x == x) {
			r->x = NULL;
			hd->h->u.cancel_transfer(r->t);
		}
	}
	pthread_mutex_unlock(&hd->lock);
}

static void give_up_all(struct hostdev *hd)
{
	pthread_mutex_lock(&hd->lock);
	for (struct hostreq *r = hd->reqs; r; r = r->next) {
		r->x = NULL;
		hd->h->u.cancel_transfer(r->t);
	}
	pthread_mutex_unlock(&hd->lock);
	for (unsigned k = 0; k < 32; k++)
		stop_iso(hd, k);
}

/* A bus reset: passed on once Mac OS has configured the device (until then it's as the host left it) */
static void host_reset(struct usb_device *d)
{
	struct hostdev *hd = host_of(d);
	give_up_all(hd);
	if (!hd->configured || atomic_load(&hd->gone))
		return;
	hd->configured = false;
	int r = hd->h->u.reset_device(hd->handle);
	if (hd->dev.log)
		printf("usb: %s: reset (%s)\n", hd->dev.name, r ? error_name(hd->h, r) : "done");
	if (r == LIBUSB_ERROR_NOT_FOUND || r == LIBUSB_ERROR_NO_DEVICE)
		atomic_store(&hd->gone, true);  /* (it came back as another device: taken again, as if plugged in) */
}

static void host_destroy(struct usb_device *d)
{
	struct hostdev *hd = host_of(d);
	struct usbhost *h = hd->h;
	give_up_all(hd);
	bool clean = settled(hd, 32);
	pthread_mutex_lock(&h->lock);
	for (unsigned k = 0; k < MAX_OPEN; k++)
		if (h->open[k] == hd)
			h->open[k] = NULL;
	pthread_mutex_unlock(&h->lock);
	release_all(hd, true);
	h->u.close(hd->handle);
	h->u.unref_device(hd->ldev);
	if (!clean)
		return;                 /* (libusb still has transfers that point here) */
	for (unsigned k = 0; k < 32; k++)
		if (hd->iso[k])
			return;
	pthread_mutex_destroy(&hd->lock);
	free(hd);
}

static const struct usb_device_ops host_ops = { host_submit, host_cancel, host_reset, host_destroy };
