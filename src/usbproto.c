#include "usbproto.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "usb.h"

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void usbproto_put(uint8_t out[USBPROTO_HEAD], const struct usbproto_msg *m)
{
	memset(out, 0, USBPROTO_HEAD);
	out[0] = m->type;
	out[1] = m->dev;
	out[2] = m->ep;
	out[3] = m->xfer;
	out[4] = m->flags;
	put32(out + 8, m->id);
	put32(out + 12, m->len);
	put32(out + 16, m->status);
	put32(out + 20, m->data);
	memcpy(out + 24, m->setup, 8);
}

bool usbproto_get(struct usbproto_msg *m, const uint8_t in[USBPROTO_HEAD])
{
	m->type = in[0];
	m->dev = in[1];
	m->ep = in[2];
	m->xfer = in[3];
	m->flags = in[4];
	m->id = get32(in + 8);
	m->len = get32(in + 12);
	m->status = get32(in + 16);
	m->data = get32(in + 20);
	memcpy(m->setup, in + 24, 8);
	return !in[5] && !in[6] && !in[7] && m->data <= USBPROTO_DATA;
}

static bool zero(const uint8_t *p, size_t n)
{
	while (n--)
		if (*p++)
			return false;
	return true;
}

/* (VID:PID in hex, or BUS-PORT[.PORT...]) */
static bool spec_chars(const uint8_t *s, uint32_t n)
{
	if (n == 0 || n >= USBPROTO_SPEC)
		return false;
	for (uint32_t k = 0; k < n; k++)
		if (!s[k] || !strchr("0123456789abcdefABCDEF:-.", s[k]))
			return false;
	return true;
}

const char *usbproto_check_request(const struct usbproto_msg *m, const uint8_t *data)
{
	if (m->status)
		return "a request with a status";
	switch (m->type) {
	case USBPROTO_OPEN:
		if (m->dev || m->ep || m->xfer || m->flags || m->len || !zero(m->setup, 8))
			return "OPEN with more than a device";
		return spec_chars(data, m->data) ? NULL : "OPEN without a device's VID:PID or BUS-PORT";
	case USBPROTO_SUBMIT: {
		if (m->dev >= USBPROTO_DEVICES)
			return "a transfer for no device";
		if (m->flags & ~USBPROTO_LOG)
			return "a transfer with unknown flags";
		if (m->ep & 0x70)
			return "a transfer on no endpoint";
		bool in = m->ep & USB_DIR_IN;
		if (m->data != (in ? 0 : m->len))
			return "a transfer whose data isn't its length";
		if (m->xfer == USB_CONTROL) {
			if ((m->ep & 15) || in != !!(m->setup[0] & USB_DIR_IN))
				return "a control request not on endpoint 0's way";
			uint16_t length = usb_w16(m->setup + 6);
			if (in ? m->len != length : m->len > length)
				return "a control request whose length isn't its own";
			return NULL;
		}
		if (m->xfer > USB_INTERRUPT)
			return "a transfer of no type";
		if (!(m->ep & 15))
			return "endpoint 0 without a control request";
		if (!zero(m->setup, 8))
			return "a setup packet without a control request";
		if (m->len > (m->xfer == USB_ISO ? USBPROTO_ISO : USBPROTO_DATA))
			return "a transfer too long";
		return NULL;
	}
	case USBPROTO_CANCEL:
	case USBPROTO_RESET:
	case USBPROTO_CLOSE:
		if (m->dev >= USBPROTO_DEVICES)
			return "a request for no device";
		if (m->ep || m->xfer || m->flags || m->len || m->data || !zero(m->setup, 8) ||
		    (m->type != USBPROTO_CANCEL && m->id))
			return "a request with more than its device";
		return NULL;
	default:
		return "an unknown request";
	}
}

const char *usbproto_check_answer(const struct usbproto_msg *m)
{
	switch (m->type) {
	case USBPROTO_HELLO:
		return m->data ? "HELLO with data" : NULL;
	case USBPROTO_OPENED:
		if (m->status == 0)
			return m->dev < USBPROTO_DEVICES && m->data == USBPROTO_INFO ? NULL : "OPENED for no device";
		return m->data < USBPROTO_WHY ? NULL : "OPENED with too long a reason";
	case USBPROTO_DONE:
		if (m->dev >= USBPROTO_DEVICES || m->status > USB_ERROR || m->status == USB_PENDING || m->data > m->len)
			return "DONE that isn't one";
		return NULL;
	case USBPROTO_GONE:
		return m->dev < USBPROTO_DEVICES && !m->data ? NULL : "GONE for no device";
	default:
		return "an unknown answer";
	}
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		n -= (size_t)w;
	}
	return true;
}

static bool read_all(int fd, uint8_t *p, size_t n)
{
	while (n) {
		ssize_t r = read(fd, p, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

bool usbproto_send(int fd, const struct usbproto_msg *m, const void *data)
{
	uint8_t head[USBPROTO_HEAD];
	usbproto_put(head, m);
	return write_all(fd, head, sizeof head) && (!m->data || write_all(fd, data, m->data));
}

int usbproto_recv(int fd, struct usbproto_msg *m, uint8_t *data)
{
	uint8_t head[USBPROTO_HEAD];
	if (!read_all(fd, head, sizeof head))
		return 0;
	if (!usbproto_get(m, head))
		return -1;
	return read_all(fd, data, m->data) ? 1 : 0;
}

/* (vendor, product, speed, held; then the host's ID, its name and Blue's, each NUL-padded) */
#define INFO_ID 8
#define INFO_NAME (INFO_ID + 24)
#define INFO_DEVNAME (INFO_NAME + 128)

static void put_string(uint8_t *out, const char *s, size_t room)
{
	size_t n = strlen(s);
	memcpy(out, s, n < room ? n : room - 1);
}

void usbproto_put_info(uint8_t out[USBPROTO_INFO], const struct usbhost_info *info, unsigned speed, const char *name)
{
	memset(out, 0, USBPROTO_INFO);
	out[0] = (uint8_t)info->vendor;
	out[1] = (uint8_t)(info->vendor >> 8);
	out[2] = (uint8_t)info->product;
	out[3] = (uint8_t)(info->product >> 8);
	out[4] = (uint8_t)speed;
	out[5] = info->held;
	put_string(out + INFO_ID, info->id, 24);
	put_string(out + INFO_NAME, info->name, 128);
	put_string(out + INFO_DEVNAME, name, USBPROTO_INFO - INFO_DEVNAME);
}

/* A string of `in`, printable ASCII or UTF-8 (anything else a '?') */
static void get_string(char *out, size_t size, const uint8_t *in, size_t n)
{
	size_t k = 0;
	for (; k + 1 < size && k < n && in[k]; k++)
		out[k] = in[k] < 0x20 || in[k] == 0x7F ? '?' : (char)in[k];
	if (size)
		out[k] = 0;
}

void usbproto_get_info(const uint8_t in[USBPROTO_INFO], struct usbhost_info *info, unsigned *speed, char *name,
		       size_t size)
{
	memset(info, 0, sizeof *info);
	info->vendor = (uint16_t)(in[0] | in[1] << 8);
	info->product = (uint16_t)(in[2] | in[3] << 8);
	*speed = in[4] <= USB_SPEED_SUPER ? in[4] : USB_SPEED_FULL;
	info->speed = *speed;
	info->held = in[5] != 0;
	get_string(info->id, sizeof info->id, in + INFO_ID, 24);
	get_string(info->name, sizeof info->name, in + INFO_NAME, 128);
	get_string(name, size, in + INFO_DEVNAME, USBPROTO_INFO - INFO_DEVNAME);
}
