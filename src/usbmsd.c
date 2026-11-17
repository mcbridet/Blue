#include "usbmsd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "be.h"

#define BLOCK 512u
#define EP_IN 0x81u
#define EP_OUT 0x02u
#define MPS 64u
#define CBW_SIGNATURE 0x43425355u       /* "USBC" */
#define CSW_SIGNATURE 0x53425355u       /* "USBS" */

enum { BOT_CBW, BOT_DATA_IN, BOT_DATA_OUT, BOT_CSW };
enum { CSW_GOOD, CSW_FAILED, CSW_PHASE_ERROR };

/* Sense keys */
enum { SENSE_NONE = 0, SENSE_NOT_READY = 2, SENSE_MEDIUM_ERROR = 3, SENSE_ILLEGAL_REQUEST = 5, SENSE_DATA_PROTECT = 7 };

struct usbmsd {
	struct usb_device dev;
	int fd;
	bool read_only;
	uint64_t blocks;
	uint8_t config;                 /* SET_CONFIGURATION's (0: unconfigured) */
	bool halt_in, halt_out;
	bool ejected, prevent;
	/* the bulk-only transport's command in progress */
	unsigned state;
	uint32_t tag;
	uint32_t host_len;              /* dCBWDataTransferLength: what the host moves */
	uint32_t moved;                 /* ... so far */
	uint32_t data_len;              /* what the command has to move (data in; data out it takes) */
	uint8_t status;                 /* the CSW's */
	uint8_t cdb[16];
	bool to_file;                   /* READ/WRITE: the data is the image's, from `offset` */
	uint64_t offset;
	uint8_t buf[256];               /* other commands' data */
	uint8_t sense_key, asc, ascq;
};

static const uint8_t device_descriptor[18] = {
	18, USB_DT_DEVICE, 0x10, 0x01,  /* USB 1.1 */
	0, 0, 0, MPS,                   /* class in the interface; endpoint 0's packets */
	USBMSD_VENDOR & 0xFF, USBMSD_VENDOR >> 8, USBMSD_PRODUCT & 0xFF, USBMSD_PRODUCT >> 8,
	0x00, 0x01,                     /* release 1.00 */
	1, 2, 3,                        /* manufacturer, product, serial number strings */
	1,                              /* configurations */
};

static const uint8_t config_descriptor[32] = {
	9, USB_DT_CONFIG, 32, 0, 1, 1, 0, 0x80, 50,     /* bus powered, 100 mA */
	9, USB_DT_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,      /* mass storage, SCSI, bulk-only */
	7, USB_DT_ENDPOINT, EP_IN, USB_BULK, MPS, 0, 0,
	7, USB_DT_ENDPOINT, EP_OUT, USB_BULK, MPS, 0, 0,
};

static const char *const strings[] = { NULL, "Blue", "USB test disk", "000000000001" };

static struct usbmsd *msd_of(struct usb_device *d)
{
	return (struct usbmsd *)d;
}

static const struct usb_device_ops msd_ops;

bool usbmsd_is(const struct usb_device *d)
{
	return d->ops == &msd_ops;
}

bool usbmsd_ejected(const struct usb_device *d)
{
	return ((const struct usbmsd *)d)->ejected;
}

static void sense(struct usbmsd *m, uint8_t key, uint8_t asc, uint8_t ascq)
{
	m->sense_key = key;
	m->asc = asc;
	m->ascq = ascq;
	if (key != SENSE_NONE)
		m->status = CSW_FAILED;
}

static uint32_t copy_reply(struct usbmsd *m, const uint8_t *data, uint32_t len, uint32_t room)
{
	uint32_t n = len < room ? len : room;
	memcpy(m->buf, data, n);
	return n;
}

static bool ready(struct usbmsd *m)
{
	if (m->ejected) {
		sense(m, SENSE_NOT_READY, 0x3A, 0);     /* medium not present */
		return false;
	}
	return true;
}

/* A READ or WRITE of `count` blocks from `lba`: whether it's in range */
static bool blocks_ok(struct usbmsd *m, uint64_t lba, uint32_t count, bool write)
{
	if (!ready(m))
		return false;
	if (lba > m->blocks || count > m->blocks - lba) {
		sense(m, SENSE_ILLEGAL_REQUEST, 0x21, 0);       /* logical block address out of range */
		return false;
	}
	if (write && m->read_only) {
		sense(m, SENSE_DATA_PROTECT, 0x27, 0);          /* write protected */
		return false;
	}
	m->to_file = true;
	m->offset = lba * BLOCK;
	m->data_len = count * BLOCK;
	return true;
}

/* Carries out the CBW's command (data out: sets up for it); the data in or out it has (data_len) */
static void command(struct usbmsd *m, bool in)
{
	const uint8_t *c = m->cdb;
	m->status = CSW_GOOD;
	m->to_file = false;
	m->data_len = 0;
	if (c[0] != 0x03)
		sense(m, SENSE_NONE, 0, 0);
	if (m->dev.log)
		printf("usb: %s: SCSI %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X (%u bytes %s)\n", m->dev.name,
		       c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], m->host_len, in ? "in" : "out");
	switch (c[0]) {
	case 0x00:                                      /* TEST UNIT READY */
		ready(m);
		break;
	case 0x03: {                                    /* REQUEST SENSE */
		uint8_t s[18] = { 0x70, 0, m->sense_key, 0, 0, 0, 0, 10, 0, 0, 0, 0, m->asc, m->ascq };
		m->data_len = copy_reply(m, s, sizeof s, c[4]);
		sense(m, SENSE_NONE, 0, 0);
		break;
	}
	case 0x12: {                                    /* INQUIRY */
		if (c[1] & 1) {                         /* (no vital product data) */
			sense(m, SENSE_ILLEGAL_REQUEST, 0x24, 0);
			break;
		}
		uint8_t s[36] = { 0x00, 0x80, 0x02, 0x02, 31 };      /* direct access, removable, SCSI-2 */
		memcpy(s + 8, "Blue    USB test disk   1.00", 28);
		m->data_len = copy_reply(m, s, sizeof s, c[4]);
		break;
	}
	case 0x1A:                                      /* MODE SENSE (6) */
	case 0x5A: {                                    /* MODE SENSE (10) */
		uint8_t wp = m->read_only ? 0x80 : 0;
		if (c[0] == 0x1A) {
			uint8_t s[4] = { 3, 0, wp, 0 };
			m->data_len = copy_reply(m, s, sizeof s, c[4]);
		} else {
			uint8_t s[8] = { 0, 6, 0, wp };
			m->data_len = copy_reply(m, s, sizeof s, be16(c + 7));
		}
		break;
	}
	case 0x1B:                                      /* START STOP UNIT */
		if ((c[4] & 3) == 2 && !m->prevent)     /* (eject) */
			m->ejected = true;
		else if ((c[4] & 3) == 2)
			sense(m, SENSE_ILLEGAL_REQUEST, 0x53, 2);       /* medium removal prevented */
		break;
	case 0x1E:                                      /* PREVENT ALLOW MEDIUM REMOVAL */
		m->prevent = c[4] & 1;
		break;
	case 0x23: {                                    /* READ FORMAT CAPACITIES */
		if (!ready(m))
			break;
		uint8_t s[12] = { 0, 0, 0, 8 };
		put_be32(s + 4, (uint32_t)m->blocks);
		put_be32(s + 8, 0x02000000u | BLOCK);   /* formatted */
		m->data_len = copy_reply(m, s, sizeof s, be16(c + 7));
		break;
	}
	case 0x25: {                                    /* READ CAPACITY (10) */
		if (!ready(m))
			break;
		uint8_t s[8];
		put_be32(s, (uint32_t)(m->blocks - 1));
		put_be32(s + 4, BLOCK);
		m->data_len = copy_reply(m, s, sizeof s, 8);
		break;
	}
	case 0x08:                                      /* READ (6) */
	case 0x0A:                                      /* WRITE (6) */
		blocks_ok(m, (uint64_t)(c[1] & 0x1F) << 16 | c[2] << 8 | c[3], c[4] ? c[4] : 256, c[0] == 0x0A);
		break;
	case 0x28:                                      /* READ (10) */
	case 0x2A:                                      /* WRITE (10) */
	case 0x2E:                                      /* WRITE AND VERIFY (10) */
		blocks_ok(m, be32(c + 2), be16(c + 7), c[0] != 0x28);
		break;
	case 0x2F:                                      /* VERIFY (10) */
		if (ready(m) && (be32(c + 2) > m->blocks || be16(c + 7) > m->blocks - be32(c + 2)))
			sense(m, SENSE_ILLEGAL_REQUEST, 0x21, 0);
		break;
	case 0x35:                                      /* SYNCHRONIZE CACHE */
		if (ready(m))
			fsync(m->fd);
		break;
	default:
		sense(m, SENSE_ILLEGAL_REQUEST, 0x20, 0);       /* invalid command operation code */
		break;
	}
	if (m->dev.log && m->sense_key)
		printf("usb: %s: -> sense %X/%02X/%02X\n", m->dev.name, m->sense_key, m->asc, m->ascq);
}

static void cbw(struct usbmsd *m, const uint8_t *p, uint32_t len)
{
	if (len != 31 || le32(p) != CBW_SIGNATURE || (p[14] & 0x1F) == 0 || (p[14] & 0x1F) > 16 || p[13] != 0) {
		if (m->dev.log)
			printf("usb: %s: not a command block wrapper (%u bytes: %08X ... LUN %u, %u bytes)\n", m->dev.name,
			       len, len >= 4 ? le32(p) : 0, len > 13 ? p[13] : 0, len > 14 ? p[14] : 0);
		m->halt_in = m->halt_out = true;        /* (until a reset recovers it) */
		return;
	}
	m->tag = le32(p + 4);
	m->host_len = le32(p + 8);
	m->moved = 0;
	memset(m->cdb, 0, sizeof m->cdb);
	memcpy(m->cdb, p + 15, p[14] & 0x1F);
	bool in = (p[12] & 0x80) != 0;
	command(m, in);
	if (m->host_len == 0) {
		if (m->data_len)
			m->status = CSW_PHASE_ERROR;
		m->state = BOT_CSW;
	} else if (m->data_len && in != (m->cdb[0] != 0x0A && m->cdb[0] != 0x2A && m->cdb[0] != 0x2E)) {
		m->status = CSW_PHASE_ERROR;            /* (the host would move it the other way) */
		m->data_len = 0;
		m->state = in ? BOT_DATA_IN : BOT_DATA_OUT;
	} else {
		if (m->data_len > m->host_len)
			m->data_len = m->host_len;
		m->state = in ? BOT_DATA_IN : BOT_DATA_OUT;
	}
}

static void csw(struct usbmsd *m, uint8_t *p)
{
	put_le32(p, CSW_SIGNATURE);
	put_le32(p + 4, m->tag);
	put_le32(p + 8, m->host_len - m->data_len);     /* (data_len is at most host_len) */
	p[12] = m->status;
	m->state = BOT_CBW;
}

/* Data in: the command's, then zeros to the length the host asked for */
static int data_in(struct usbmsd *m, struct usb_xfer *x)
{
	uint32_t n = m->host_len - m->moved < x->len ? m->host_len - m->moved : x->len;
	uint32_t have = m->moved < m->data_len ? m->data_len - m->moved : 0;
	if (have > n)
		have = n;
	if (have && m->to_file) {
		ssize_t got = pread(m->fd, x->data, have, (off_t)(m->offset + m->moved));
		if (got != (ssize_t)have) {
			memset(x->data, 0, have);
			sense(m, SENSE_MEDIUM_ERROR, 0x11, 0);  /* unrecovered read error */
		}
	} else if (have) {
		memcpy(x->data, m->buf + m->moved, have);
	}
	memset(x->data + have, 0, n - have);
	m->moved += n;
	x->actual = n;
	if (m->moved == m->host_len)
		m->state = BOT_CSW;
	return USB_OK;
}

static int data_out(struct usbmsd *m, struct usb_xfer *x)
{
	uint32_t n = m->host_len - m->moved < x->len ? m->host_len - m->moved : x->len;
	uint32_t take = m->moved < m->data_len ? m->data_len - m->moved : 0;
	if (take > n)
		take = n;
	if (take && m->to_file && m->status == CSW_GOOD &&
	    pwrite(m->fd, x->data, take, (off_t)(m->offset + m->moved)) != (ssize_t)take)
		sense(m, SENSE_MEDIUM_ERROR, 0x0C, 0);  /* write error */
	m->moved += n;
	x->actual = n;
	if (m->moved == m->host_len)
		m->state = BOT_CSW;
	return n < x->len ? USB_STALL : USB_OK;
}

static int control(struct usbmsd *m, struct usb_xfer *x)
{
	const uint8_t *s = x->setup;
	uint16_t value = usb_w16(s + 2), index = usb_w16(s + 4);
	uint32_t room = x->len;
	x->actual = 0;
	switch (s[0] << 8 | s[1]) {
	case 0x8006:                                    /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case USB_DT_DEVICE:
			x->actual = room < 18 ? room : 18;
			memcpy(x->data, device_descriptor, x->actual);
			return USB_OK;
		case USB_DT_CONFIG:
			if ((value & 0xFF) != 0)
				return USB_STALL;
			x->actual = room < sizeof config_descriptor ? room : sizeof config_descriptor;
			memcpy(x->data, config_descriptor, x->actual);
			return USB_OK;
		case USB_DT_STRING: {
			uint8_t d[256];
			uint32_t n;
			if ((value & 0xFF) == 0) {
				d[0] = 4, d[1] = USB_DT_STRING, d[2] = 0x09, d[3] = 0x04;      /* US English */
				n = 4;
			} else if ((value & 0xFF) < sizeof strings / sizeof *strings) {
				n = usb_string_descriptor(strings[value & 0xFF], d, sizeof d);
			} else {
				return USB_STALL;
			}
			x->actual = room < n ? room : n;
			memcpy(x->data, d, x->actual);
			return USB_OK;
		}
		}
		return USB_STALL;
	case 0x8008:                                    /* GET_CONFIGURATION */
		if (room)
			x->data[0] = m->config, x->actual = 1;
		return USB_OK;
	case 0x0009:                                    /* SET_CONFIGURATION */
		if (value > 1)
			return USB_STALL;
		m->config = (uint8_t)value;
		m->halt_in = m->halt_out = false;
		m->state = BOT_CBW;
		return USB_OK;
	case 0x8000:                                    /* GET_STATUS: device */
	case 0x8100:                                    /* ... interface */
	case 0x8200:                                    /* ... endpoint */
		if (room < 2)
			return USB_STALL;
		x->data[0] = x->data[1] = 0;
		if (s[0] == 0x82)
			x->data[0] = (index & 0xFF) == EP_IN ? m->halt_in : (index & 0xFF) == EP_OUT ? m->halt_out : 0;
		x->actual = 2;
		return USB_OK;
	case 0x0201:                                    /* CLEAR_FEATURE (ENDPOINT_HALT) */
	case 0x0203:                                    /* SET_FEATURE (ENDPOINT_HALT) */
		if (value != 0)
			return USB_STALL;
		if ((index & 0xFF) == EP_IN)
			m->halt_in = s[1] == USB_REQ_SET_FEATURE;
		else if ((index & 0xFF) == EP_OUT)
			m->halt_out = s[1] == USB_REQ_SET_FEATURE;
		else if ((index & 0x7F) != 0)
			return USB_STALL;
		return USB_OK;
	case 0x810A:                                    /* GET_INTERFACE */
		if (room)
			x->data[0] = 0, x->actual = 1;
		return USB_OK;
	case 0x010B:                                    /* SET_INTERFACE */
		return value == 0 && index == 0 ? USB_OK : USB_STALL;
	case 0x21FF:                                    /* Bulk-Only Mass Storage Reset */
		m->state = BOT_CBW;
		return USB_OK;
	case 0xA1FE:                                    /* Get Max LUN */
		if (room)
			x->data[0] = 0, x->actual = 1;
		return USB_OK;
	}
	return USB_STALL;
}

static int msd_submit(struct usb_device *d, struct usb_xfer *x)
{
	struct usbmsd *m = msd_of(d);
	x->actual = 0;
	if (x->type == USB_CONTROL)
		return control(m, x);
	if (!m->config || x->type != USB_BULK)
		return USB_STALL;
	if (x->ep == EP_OUT) {
		if (m->halt_out)
			return USB_STALL;
		if (m->state == BOT_CBW) {
			cbw(m, x->data, x->len);
			if (m->halt_out)
				return USB_STALL;
			x->actual = x->len;
			return USB_OK;
		}
		if (m->state == BOT_DATA_OUT)
			return data_out(m, x);
		m->halt_out = true;
		return USB_STALL;
	}
	if (x->ep == EP_IN) {
		if (m->halt_in)
			return USB_STALL;
		if (m->state == BOT_DATA_IN)
			return data_in(m, x);
		if (m->state == BOT_CSW) {
			if (x->len < 13) {
				m->halt_in = true;
				return USB_STALL;
			}
			csw(m, x->data);
			x->actual = 13;
			return USB_OK;
		}
		if (m->state == BOT_DATA_OUT) {         /* (the host gave up sending: the status, with a stall first) */
			m->halt_in = true;
			m->state = BOT_CSW;
			return USB_STALL;
		}
		return USB_NAK;
	}
	return USB_STALL;
}

static void msd_cancel(struct usb_device *d, struct usb_xfer *x)
{
	(void)d;
	(void)x;
}

static void msd_reset(struct usb_device *d)
{
	struct usbmsd *m = msd_of(d);
	m->config = 0;
	m->halt_in = m->halt_out = false;
	m->state = BOT_CBW;
}

static void msd_destroy(struct usb_device *d)
{
	struct usbmsd *m = msd_of(d);
	close(m->fd);
	free(m);
}

static const struct usb_device_ops msd_ops = { msd_submit, msd_cancel, msd_reset, msd_destroy };

struct usb_device *usbmsd_open(const char *path, bool read_only, char *err, size_t size)
{
	int fd = read_only ? -1 : open(path, O_RDWR);
	bool ro = read_only;
	if (fd < 0) {
		fd = open(path, O_RDONLY);
		ro = true;
	}
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)BLOCK) {
		snprintf(err, size, "%s: %s", path, fd < 0 ? strerror(errno) : "not an image of at least one 512-byte block");
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	struct usbmsd *m = calloc(1, sizeof *m);
	if (!m) {
		close(fd);
		snprintf(err, size, "out of memory");
		return NULL;
	}
	m->fd = fd;
	m->read_only = ro;
	m->blocks = (uint64_t)st.st_size / BLOCK;
	if (m->blocks > 0xFFFFFFFFu)
		m->blocks = 0xFFFFFFFFu;
	const char *base = strrchr(path, '/');
	char name[96];
	snprintf(name, sizeof name, "test disk %s", base ? base + 1 : path);
	usb_device_init(&m->dev, &msd_ops, USB_SPEED_FULL, name);
	return &m->dev;
}
