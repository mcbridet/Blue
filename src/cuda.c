#include "cuda.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Packet types */
#define PKT_ADB 0x00
#define PKT_PSEUDO 0x01
#define PKT_ERROR 0x02

/* ADB reply flags */
#define ADB_FLAG_TIMEOUT 0x02
#define ADB_FLAG_AUTOPOLL 0x40

/* Pseudo commands */
enum {
	CMD_WARM_START = 0x00,
	CMD_AUTOPOLL = 0x01,
	CMD_GET_6805_ADDR = 0x02,
	CMD_GET_TIME = 0x03,
	CMD_GET_PRAM = 0x07,
	CMD_SET_6805_ADDR = 0x08,
	CMD_SET_TIME = 0x09,
	CMD_POWERDOWN = 0x0A,
	CMD_POWERUP_TIME = 0x0B,
	CMD_SET_PRAM = 0x0C,
	CMD_MS_RESET = 0x0D,
	CMD_SEND_DFAC = 0x0E,
	CMD_RESET_SYSTEM = 0x11,
	CMD_SET_IPL = 0x12,
	CMD_FILE_SERVER_FLAG = 0x13,
	CMD_SET_AUTO_RATE = 0x14,
	CMD_GET_AUTO_RATE = 0x16,
	CMD_SET_DEVICE_LIST = 0x19,
	CMD_GET_DEVICE_LIST = 0x1A,
	CMD_SET_ONE_SECOND_MODE = 0x1B,
	CMD_SET_POWER_MESSAGES = 0x21,
	CMD_GET_SET_IIC = 0x22,
	CMD_COMBINED_FORMAT_IIC = 0x25,
};

#define MAC_EPOCH_OFFSET 2082844800     /* 1904-01-01 to 1970-01-01, seconds */

/*
 * Mac OS 9.2.2's power manager dozes after half an hour without user
 * activity (it naps through the kernel's PowerDispatch, and Mac OS hides the
 * cursor), until there is some. Its ADB Manager tells it of the keyboard's
 * key-downs, but not of the mouse's data once the power manager's globals
 * say 'SARA' (PMgrBase + 0x112), as they do here: on the iMac, whose mouse
 * is USB, the mouse's driver told it itself. Blue's mouse is ADB's, so its
 * clicks and moves went unseen (a dialog's button didn't take a click) until
 * a key was pressed. So mouse input while Mac OS dozes goes after a tap of
 * Option, down and up in one packet, which the ADB Manager takes for
 * activity and Mac OS for nothing else; at most one a second.
 */
#define DOZE_MS 250             /* napping through PowerDispatch this recently: dozing */
#define WAKE_TAP_MS 1000
#define WAKE_KEY 0x3A           /* Option (ADB key code) */

static uint32_t mac_time_now(const struct cuda *c)
{
	time_t t = guest_clock_now(c->clock, NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	return (uint32_t)((int64_t)t + tm.tm_gmtoff + MAC_EPOCH_OFFSET + c->time_offset);
}

/* Queues a packet for the host and signals it: TREQ low plus an attention byte. */
static void send_packet(struct cuda *c, const uint8_t *data, unsigned len)
{
	if (len > sizeof c->in)
		len = sizeof c->in;
	memcpy(c->in, data, len);
	c->in_len = len;
	c->in_pos = 0;
	c->unsolicited = false;
	c->via->in_b &= (uint8_t)~CUDA_TREQ;
	c->via->sr = 0;
	via_raise(c->via, VIA_IRQ_SR);
}

static void pseudo_command(struct cuda *c, const uint8_t *d, unsigned len)
{
	uint8_t r[16] = { PKT_PSEUDO, 0, d[0] };
	unsigned rlen = 3;

	switch (d[0]) {
	case CMD_AUTOPOLL:
		c->autopoll = len > 1 && d[1];
		break;
	case CMD_GET_TIME: {
		uint32_t t = mac_time_now(c);
		r[3] = (uint8_t)(t >> 24);
		r[4] = (uint8_t)(t >> 16);
		r[5] = (uint8_t)(t >> 8);
		r[6] = (uint8_t)t;
		rlen = 7;
		break;
	}
	case CMD_SET_TIME:
		if (len >= 5) {
			uint32_t want = (uint32_t)d[1] << 24 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 8 | d[4];
			c->time_offset += (int64_t)want - (int64_t)mac_time_now(c);
		}
		break;
	case CMD_GET_PRAM:
		if (len >= 3) {
			r[3] = c->pram[d[2]];
			rlen = 4;
		}
		break;
	case CMD_SET_PRAM:
		for (unsigned k = 3; k < len; k++)
			c->pram[(uint8_t)(d[2] + k - 3)] = d[k];
		break;
	case CMD_POWERDOWN:
		c->power_off = true;
		break;
	case CMD_RESET_SYSTEM:
		c->reset = true;
		break;
	case CMD_SET_AUTO_RATE:
		if (len > 1 && d[1])
			c->autopoll_ms = d[1];
		break;
	case CMD_GET_AUTO_RATE:
		r[3] = (uint8_t)c->autopoll_ms;
		rlen = 4;
		break;
	case CMD_SET_DEVICE_LIST:
		if (len > 2)
			c->autopoll_devices = (uint16_t)(d[1] << 8 | d[2]);
		break;
	case CMD_GET_DEVICE_LIST:
		r[3] = (uint8_t)(c->autopoll_devices >> 8);
		r[4] = (uint8_t)c->autopoll_devices;
		rlen = 5;
		break;
	case CMD_GET_6805_ADDR:
		r[3] = r[4] = 0;
		rlen = 5;
		break;
	case CMD_WARM_START:
	case CMD_SET_6805_ADDR:
	case CMD_POWERUP_TIME:
	case CMD_MS_RESET:
	case CMD_SEND_DFAC:
	case CMD_SET_IPL:
	case CMD_FILE_SERVER_FLAG:
	case CMD_SET_ONE_SECOND_MODE:
	case CMD_SET_POWER_MESSAGES:
	case CMD_GET_SET_IIC:
	case CMD_COMBINED_FORMAT_IIC:
		break;
	default:
		if (!c->unknown_logged[d[0] >> 3 & 31] || !(c->unknown_logged[d[0] >> 3 & 31] & (1u << (d[0] & 7)))) {
			c->unknown_logged[d[0] >> 3 & 31] |= (uint8_t)(1u << (d[0] & 7));
			fprintf(stderr, "cuda: unknown pseudo command 0x%02X\n", d[0]);
		}
		r[0] = PKT_ERROR;
		r[1] = 0x02;
		r[2] = PKT_PSEUDO;
		r[3] = d[0];
		rlen = 4;
		break;
	}
	send_packet(c, r, rlen);
}

static void process_packet(struct cuda *c)
{
	c->packets++;
	if (c->out_len < 2) {
		uint8_t r[] = { PKT_ERROR, 0x01, c->out_len ? c->out[0] : 0 };
		send_packet(c, r, sizeof r);
		return;
	}
	switch (c->out[0]) {
	case PKT_ADB: {
		uint8_t cmd = c->out[1];
		uint8_t r[3 + 8] = { PKT_ADB, 0, cmd };
		int n = adb_command(&c->adb, cmd, c->out + 2, c->out_len - 2, r + 3);
		if (n < 0)
			r[1] = ADB_FLAG_TIMEOUT;
		send_packet(c, r, 3u + (n > 0 ? (unsigned)n : 0));
		break;
	}
	case PKT_PSEUDO:
		pseudo_command(c, c->out + 1, c->out_len - 1);
		break;
	default: {
		uint8_t r[] = { PKT_ERROR, 0x01, c->out[0] };
		send_packet(c, r, sizeof r);
		break;
	}
	}
}

/* Hands the host the next byte of the pending packet. */
static void next_byte_to_host(struct cuda *c)
{
	struct via *v = c->via;

	if (c->in_pos < c->in_len) {
		v->sr = c->in[c->in_pos++];
		if (c->in_pos >= c->in_len)
			v->in_b |= CUDA_TREQ;       /* last byte: drop the request */
	} else {
		v->sr = 0;
		v->in_b |= CUDA_TREQ;
	}
	via_raise(v, VIA_IRQ_SR);
}

static void byte_from_host(struct cuda *c)
{
	if (c->out_len < sizeof c->out)
		c->out[c->out_len++] = c->via->sr;
	via_raise(c->via, VIA_IRQ_SR);
}

static void port_b_written(void *ctx, struct via *v)
{
	struct cuda *c = ctx;
	uint8_t b = via_port_b(v);
	bool tip = !(b & CUDA_TIP), was_tip = !(c->last_b & CUDA_TIP);
	bool sending = v->acr & VIA_ACR_SR_OUT;

	if (tip && !was_tip) {                  /* transfer starts */
		if (sending) {
			if (c->unsolicited && c->in_pos == 0 && c->in_len) {
				/* collision: the host goes first, our autopoll data waits */
				memcpy(c->deferred, c->in, c->in_len);
				c->deferred_len = c->in_len;
				c->in_len = 0;
				c->unsolicited = false;
				v->in_b |= CUDA_TREQ;
			}
			c->out_len = 0;
			byte_from_host(c);
		} else {
			next_byte_to_host(c);
		}
	} else if (tip && ((b ^ c->last_b) & CUDA_TACK)) {     /* next byte */
		if (sending)
			byte_from_host(c);
		else
			next_byte_to_host(c);
	} else if (!tip && was_tip) {           /* transfer ends */
		if (c->out_len) {
			process_packet(c);      /* the reply's attention byte interrupts the host */
			c->out_len = 0;
		} else {
			via_raise(v, VIA_IRQ_SR);       /* end of a read: acknowledge it */
			if (c->deferred_len && c->in_pos >= c->in_len) {
				send_packet(c, c->deferred, c->deferred_len);
				c->unsolicited = true;
				c->deferred_len = 0;
			}
		}
	}
	c->last_b = b;
}

void cuda_poll(struct cuda *c, uint64_t now_ms)
{
	bool idle = (c->last_b & CUDA_TIP) && c->in_pos >= c->in_len && !c->deferred_len;
	if (!c->autopoll || !idle || now_ms - c->last_poll_ms < c->autopoll_ms)
		return;
	c->last_poll_ms = now_ms;

	int addr = adb_pending(&c->adb, c->autopoll_devices);
	bool mouse = addr < 0 ? c->mouse_moved : addr == c->adb.mouse.addr;
	c->mouse_moved = false;
	if (mouse && c->dozed && now_ms - c->doze_ms < DOZE_MS && now_ms - c->wake_ms >= WAKE_TAP_MS &&
	    (c->autopoll_devices >> c->adb.kbd.addr & 1)) {
		const uint8_t tap[5] = { PKT_ADB, ADB_FLAG_AUTOPOLL, (uint8_t)(c->adb.kbd.addr << 4 | 0x0C), WAKE_KEY,
					 WAKE_KEY | 0x80 };
		c->wake_ms = now_ms;
		c->wake_taps++;
		send_packet(c, tap, sizeof tap);
		c->unsolicited = true;
		return;                 /* (the mouse's data goes at the next poll) */
	}
	if (addr < 0)
		return;
	uint8_t cmd = (uint8_t)(addr << 4 | 0x0C);      /* Talk R0 */
	uint8_t r[3 + 8] = { PKT_ADB, ADB_FLAG_AUTOPOLL, cmd };
	int n = adb_command(&c->adb, cmd, NULL, 0, r + 3);
	if (n <= 0)
		return;
	send_packet(c, r, 3u + (unsigned)n);
	c->unsolicited = true;
}

void cuda_doze(struct cuda *c, uint64_t now_ms)
{
	c->dozed = true;
	c->doze_ms = now_ms;
}

void cuda_mouse_moved(struct cuda *c)
{
	c->mouse_moved = true;
}

void cuda_init(struct cuda *c, struct via *via)
{
	memset(c, 0, sizeof *c);
	c->via = via;
	c->autopoll_ms = 11;
	c->autopoll_devices = 1u << 2 | 1u << 3;
	adb_init(&c->adb);
	via->hooks = (struct via_hooks){ .ctx = c, .port_b_written = port_b_written };
	via->in_b |= CUDA_TREQ;
	c->last_b = via_port_b(via);
}
