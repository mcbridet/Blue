#include "modem.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { R_OK = 0, R_CONNECT = 1, R_RING = 2, R_NO_CARRIER = 3, R_ERROR = 4, R_NO_DIALTONE = 6, R_BUSY = 7,
       R_NO_ANSWER = 8 };

static const char *const result_text[] = {
	[R_OK] = "OK", [R_CONNECT] = "CONNECT", [R_RING] = "RING", [R_NO_CARRIER] = "NO CARRIER",
	[R_ERROR] = "ERROR", [R_NO_DIALTONE] = "NO DIALTONE", [R_BUSY] = "BUSY", [R_NO_ANSWER] = "NO ANSWER",
};

/* telnet (RFC 854) */
enum { TN_SE = 240, TN_SB = 250, TN_WILL = 251, TN_WONT = 252, TN_DO = 253, TN_DONT = 254, TN_IAC = 255 };
enum { TN_BINARY = 0, TN_ECHO = 1, TN_SGA = 3 };
enum { TS_DATA, TS_IAC, TS_OPTION, TS_SUB, TS_SUB_IAC };

#define RING_SECONDS 6u

static void factory(struct modem *m)
{
	memset(m->s, 0, sizeof m->s);
	m->s[2] = '+';
	m->s[3] = '\r';
	m->s[4] = '\n';
	m->s[5] = 8;
	m->s[6] = 2;
	m->s[7] = 50;
	m->s[8] = 2;
	m->s[10] = 14;
	m->s[12] = 50;
	m->s[25] = 5;
	m->echo = m->verbose = true;
	m->quiet = false;
	m->x = 4;
	m->dcd_mode = 1;
	m->dtr_mode = 2;
	m->speaker = 1;
	m->volume = 2;
}

static void update_lines(struct modem *m)
{
	bool dcd = m->dcd_mode == 0 || (m->call == MODEM_CONNECTED), cts = true;
	if (dcd == m->dcd_out && cts == m->cts_out)
		return;
	m->dcd_out = dcd;
	m->cts_out = cts;
	if (m->dte.lines)
		m->dte.lines(m->dte.ctx, dcd, cts);
}

void modem_init(struct modem *m, uint32_t timebase_hz)
{
	struct modem_line line = m->line;
	struct modem_dte dte = m->dte;
	const struct modem_phonebook *book = m->book;
	bool log = m->log;
	memset(m, 0, sizeof *m);
	m->line = line;
	m->dte = dte;
	m->book = book;
	m->log = log;
	m->timebase_hz = timebase_hz;
	factory(m);
	m->dcd_out = m->cts_out = true;         /* (what the SCC starts with) */
	update_lines(m);
}

/* ---- to the computer ---- */

static void put(struct modem *m, uint8_t b)
{
	if (m->qcount < MODEM_QUEUE) {
		m->q[(m->qhead + m->qcount) % MODEM_QUEUE] = b;
		m->qcount++;
	}
}

static void put_text(struct modem *m, const char *t)
{
	while (*t)
		put(m, (uint8_t)*t++);
}

static void ready(struct modem *m)
{
	if (m->qcount && m->dte.ready)
		m->dte.ready(m->dte.ctx);
}

int modem_to_dte(struct modem *m)
{
	if (!m->qcount)
		return -1;
	uint8_t b = m->q[m->qhead];
	m->qhead = (m->qhead + 1) % MODEM_QUEUE;
	m->qcount--;
	return b;
}

static void line_end(struct modem *m)
{
	put(m, m->s[3]);
	put(m, m->s[4]);
}

static void result(struct modem *m, unsigned code)
{
	if (m->log)
		printf("modem: %s\n", code == R_CONNECT && m->x ? "CONNECT 56000" : result_text[code]);
	if (m->quiet)
		return;
	if (code == R_BUSY && m->x < 3)
		code = R_NO_CARRIER;
	if (code == R_NO_DIALTONE && m->x < 2)
		code = R_NO_CARRIER;
	if (m->verbose) {
		line_end(m);
		put_text(m, result_text[code]);
		if (code == R_CONNECT && m->x) {
			char speed[16];
			snprintf(speed, sizeof speed, " %u", MODEM_SPEED);
			put_text(m, speed);
		}
		line_end(m);
	} else {
		char n[8];
		snprintf(n, sizeof n, "%u", code);
		put_text(m, n);
		put(m, m->s[3]);
	}
	ready(m);
}

/* ---- the call ---- */

static void end_call(struct modem *m, bool tell_line)
{
	if (m->call != MODEM_IDLE && tell_line && m->line.hangup)
		m->line.hangup(m->line.ctx);
	if (m->log && m->call != MODEM_IDLE)
		printf("modem: call with %s ended\n", m->peer);
	m->call = MODEM_IDLE;
	m->online = m->answering = false;
	m->plus = m->held_len = 0;
	update_lines(m);
}

void modem_hangup(struct modem *m)
{
	bool was_online = m->online;
	end_call(m, true);
	if (was_online)
		result(m, R_NO_CARRIER);
}

static void start_call(struct modem *m)
{
	m->call = MODEM_CONNECTED;
	m->online = true;
	m->calls++;
	m->plus = m->held_len = 0;
	m->sent_any = false;
	m->tn_state = TS_DATA;
	memset(m->tn_us, 0, sizeof m->tn_us);
	memset(m->tn_him, 0, sizeof m->tn_him);
	if (m->log)
		printf("modem: connected to %s%s\n", m->peer, m->telnet == MODEM_TELNET_ON ? " (telnet)" : "");
	result(m, R_CONNECT);
	update_lines(m);
}

void modem_connected(struct modem *m)
{
	if (m->call == MODEM_DIALLING)
		start_call(m);
}

void modem_failed(struct modem *m, bool busy)
{
	if (m->call != MODEM_DIALLING)
		return;
	if (m->log)
		printf("modem: %s %s\n", m->peer, busy ? "is busy" : "doesn't answer");
	end_call(m, false);
	result(m, busy ? R_BUSY : R_NO_CARRIER);
}

void modem_incoming(struct modem *m, const char *peer)
{
	if (m->call != MODEM_IDLE)
		return;
	snprintf(m->peer, sizeof m->peer, "%s", peer);
	m->call = MODEM_RINGING;
	m->answering = true;
	m->rings = 0;
	m->s[1] = 0;
	m->ring_at = m->now;
	m->telnet = MODEM_TELNET_AUTO;
	if (m->log)
		printf("modem: %s is calling\n", peer);
}

void modem_far_hangup(struct modem *m)
{
	enum modem_call call = m->call;
	if (call == MODEM_IDLE)
		return;
	if (m->log)
		printf("modem: %s hung up\n", m->peer);
	end_call(m, false);
	if (call != MODEM_RINGING)
		result(m, R_NO_CARRIER);
}

static void answer(struct modem *m)
{
	if (m->call != MODEM_RINGING) {
		result(m, R_NO_CARRIER);
		return;
	}
	if (m->line.answer && m->line.answer(m->line.ctx)) {
		start_call(m);
		return;
	}
	end_call(m, false);
	result(m, R_NO_CARRIER);
}

/* ---- dialling ---- */

static bool is_address(const char *d)
{
	if (strchr(d, '.') || strchr(d, ':') || strchr(d, '['))
		return true;
	for (; *d; d++)
		if (isalpha((unsigned char)*d) && toupper((unsigned char)*d) != 'W' && toupper((unsigned char)*d) != 'R')
			return true;
	return false;
}

static void digits_of(const char *d, char *out, size_t size)
{
	size_t n = 0;
	for (; *d && n + 1 < size; d++)
		if (isdigit((unsigned char)*d) || *d == '*' || *d == '#')
			out[n++] = *d;
	out[n] = '\0';
}

bool modem_address(const struct modem_phonebook *pb, const char *dial, char *host, size_t host_size,
		   unsigned *port, enum modem_telnet *telnet)
{
	char d[MODEM_ADDRESS_MAX];
	while (*dial == ' ')
		dial++;
	snprintf(d, sizeof d, "%s", dial);
	size_t n = strlen(d);
	while (n && (d[n - 1] == ' ' || d[n - 1] == ';'))
		d[--n] = '\0';
	if (!n)
		return false;
	if (is_address(d)) {
		char *h = d, *p = NULL;
		if (*h == '[') {                /* [IPv6]:port */
			char *close = strchr(h, ']');
			if (!close)
				return false;
			*close = '\0';
			h++;
			p = close[1] == ':' ? close + 2 : NULL;
		} else if ((p = strrchr(h, ':'))) {
			*p++ = '\0';
		}
		unsigned v = 23;
		if (p) {
			char *end;
			unsigned long pv = strtoul(p, &end, 10);
			if (!*p || *end || pv == 0 || pv > 65535)
				return false;
			v = (unsigned)pv;
		}
		while (*h == ' ')
			h++;
		for (size_t k = strlen(h); k && h[k - 1] == ' '; k--)
			h[k - 1] = '\0';
		if (!*h)
			return false;
		snprintf(host, host_size, "%s", h);
		*port = v;
		*telnet = v == 23 ? MODEM_TELNET_ON : MODEM_TELNET_AUTO;
		return true;
	}
	char number[32];
	digits_of(d, number, sizeof number);
	for (unsigned k = 0; pb && number[0] && k < pb->count; k++) {
		const struct modem_number *e = &pb->entry[k];
		if (strcmp(e->number, number) == 0) {
			snprintf(host, host_size, "%s", e->host);
			*port = e->port;
			*telnet = e->telnet != MODEM_TELNET_AUTO ? e->telnet : e->port == 23 ? MODEM_TELNET_ON :
				  MODEM_TELNET_AUTO;
			return true;
		}
	}
	return false;
}

static void dial(struct modem *m, const char *d)
{
	while (*d == ' ')
		d++;
	if (toupper((unsigned char)*d) == 'T' || toupper((unsigned char)*d) == 'P')    /* (tone or pulse) */
		d++;
	size_t n = strlen(d);
	bool stay = n && d[n - 1] == ';';       /* dial, and stay in command mode: the number goes on */
	char full[MODEM_ADDRESS_MAX];
	size_t have = strnlen(m->dialled, sizeof full - 1), add = stay ? n - 1 : n;
	if (add > sizeof full - 1 - have)
		add = sizeof full - 1 - have;
	memcpy(full, m->dialled, have);
	memcpy(full + have, d, add);
	full[have + add] = '\0';
	if (stay) {
		snprintf(m->dialled, sizeof m->dialled, "%s", full);
		result(m, R_OK);
		return;
	}
	m->dialled[0] = '\0';
	char host[MODEM_ADDRESS_MAX];
	unsigned port;
	enum modem_telnet telnet;
	if (m->call != MODEM_IDLE) {
		result(m, R_ERROR);
		return;
	}
	if (!modem_address(m->book, full, host, sizeof host, &port, &telnet)) {
		if (m->log)
			printf("modem: \"%s\" calls nowhere\n", full);
		result(m, R_NO_CARRIER);
		return;
	}
	snprintf(m->peer, sizeof m->peer, strchr(host, ':') ? "[%s]:%u" : "%s:%u", host, port);
	m->telnet = telnet;
	m->answering = false;
	m->call = MODEM_DIALLING;
	m->deadline = m->now + (uint64_t)(m->s[7] ? m->s[7] : 50) * m->timebase_hz;
	if (m->log)
		printf("modem: dialling %s\n", m->peer);
	if (!m->line.dial || !m->line.dial(m->line.ctx, host, port)) {
		end_call(m, false);
		result(m, R_NO_CARRIER);
	}
}

/* ---- commands ---- */

static unsigned number(const char **p)
{
	unsigned v = 0;
	while (isdigit((unsigned char)**p))
		v = v * 10 + (unsigned)(*(*p)++ - '0');
	return v;
}

static void show_settings(struct modem *m)
{
	char t[160];
	snprintf(t, sizeof t, "E%u L%u M%u Q%u V%u X%u &C%u &D%u", m->echo, m->volume, m->speaker, m->quiet, m->verbose,
		 m->x, m->dcd_mode, m->dtr_mode);
	line_end(m);
	put_text(m, t);
	line_end(m);
	static const unsigned regs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12 };
	size_t n = 0;
	for (unsigned k = 0; k < sizeof regs / sizeof regs[0]; k++)
		n += (size_t)snprintf(t + n, sizeof t - n, "%sS%02u:%03u", k ? " " : "", regs[k], m->s[regs[k]]);
	put_text(m, t);
	line_end(m);
}

/* Runs a command line (what followed "AT") */
static void execute(struct modem *m, const char *cmd)
{
	snprintf(m->last, sizeof m->last, "%s", cmd);
	if (m->log)
		printf("modem: AT%s\n", cmd);
	const char *p = cmd;
	while (*p) {
		char c = (char)toupper((unsigned char)*p++);
		unsigned v;
		switch (c) {
		case ' ':
			break;
		case 'A':
			answer(m);
			return;
		case 'D':
			dial(m, p);
			return;
		case 'E':
			m->echo = number(&p) != 0;
			break;
		case 'H':
			if (number(&p) == 0)
				end_call(m, true);
			break;
		case 'I':                       /* (Mac OS 9's modem script picks its settings by I12's answer) */
			v = number(&p);
			line_end(m);
			put_text(m, v == 12 ? "D1" : v == 3 ? "Blue 56K Modem (calls over TCP)" : "Blue 56K Modem");
			line_end(m);
			break;
		case 'L':
			m->volume = number(&p);
			break;
		case 'M':
			m->speaker = number(&p);
			break;
		case 'O':
			number(&p);
			if (m->call == MODEM_CONNECTED) {
				m->online = true;
				m->plus = m->held_len = 0;
				result(m, R_CONNECT);
			} else {
				result(m, R_NO_CARRIER);
			}
			return;
		case 'Q':
			m->quiet = number(&p) != 0;
			break;
		case 'S':
			if (isdigit((unsigned char)*p))
				m->sreg = number(&p);
			if (m->sreg >= sizeof m->s) {
				result(m, R_ERROR);
				return;
			}
			if (*p == '=') {
				p++;
				v = number(&p);
				m->s[m->sreg] = (uint8_t)(v > 255 ? 255 : v);
			} else if (*p == '?') {
				char t[8];
				p++;
				snprintf(t, sizeof t, "%03u", m->s[m->sreg]);
				line_end(m);
				put_text(m, t);
				line_end(m);
			}
			break;
		case 'V':
			m->verbose = number(&p) != 0;
			break;
		case 'X':
			v = number(&p);
			m->x = v > 4 ? 4 : v;
			break;
		case 'Z':
			number(&p);
			end_call(m, true);
			factory(m);
			update_lines(m);
			break;
		case 'B': case 'C': case 'F': case 'G': case 'K': case 'N': case 'P': case 'R': case 'T': case 'U':
		case 'W': case 'Y':
			number(&p);
			break;
		case '&':
			c = (char)toupper((unsigned char)*p);
			if (!c) {
				result(m, R_ERROR);
				return;
			}
			p++;
			v = number(&p);
			if (c == 'C') {
				m->dcd_mode = v ? 1 : 0;
				update_lines(m);
			} else if (c == 'D') {
				m->dtr_mode = v > 3 ? 2 : v;
			} else if (c == 'F') {
				factory(m);
				update_lines(m);
			} else if (c == 'V') {
				show_settings(m);
			} else if (c == 'Z') {          /* (&Zn=number: stored numbers aren't kept) */
				while (*p && *p != ';')
					p++;
			}
			break;
		case '\\': case '%': case '*': case '#': case '$': case '"': case ')':
			if (!*p) {
				result(m, R_ERROR);
				return;
			}
			p++;
			number(&p);
			if (*p == '=') {
				p++;
				number(&p);
			}
			break;
		case '+':                       /* (+MS=..., +GCI=...: to the end of the line, or a ';') */
			while (*p && *p != ';')
				p++;
			if (*p)
				p++;
			break;
		default:
			result(m, R_ERROR);
			return;
		}
	}
	result(m, R_OK);
}

/* A character in command mode */
static void command_char(struct modem *m, uint8_t b)
{
	if (m->call == MODEM_DIALLING) {        /* a key pressed while dialling: the call is given up */
		end_call(m, true);
		result(m, R_NO_CARRIER);
		return;
	}
	if (m->echo) {
		put(m, b);
		ready(m);
	}
	char c = (char)toupper(b & 0x7F);
	if (b == m->s[3]) {
		if (m->at == 2) {
			m->cmd[m->cmd_len] = '\0';
			m->at = 0;
			execute(m, m->cmd);
		}
		m->at = 0;
		return;
	}
	if (b == m->s[5] || b == 0x7F) {
		if (m->at == 2 && m->cmd_len)
			m->cmd_len--;
		return;
	}
	switch (m->at) {
	case 0:
		m->at = c == 'A';
		break;
	case 1:
		if (c == 'T') {
			m->at = 2;
			m->cmd_len = 0;
		} else if (c == '/') {
			m->at = 0;
			execute(m, m->last);
		} else {
			m->at = c == 'A';
		}
		break;
	default:
		if (m->cmd_len + 1 < sizeof m->cmd)
			m->cmd[m->cmd_len++] = (char)b;
		break;
	}
}

/* ---- data over the call ---- */

static void send_line(struct modem *m, const uint8_t *data, size_t len)
{
	if (m->line.send && len)
		m->line.send(m->line.ctx, data, len);
}

static void send_data(struct modem *m, const uint8_t *data, size_t len)
{
	m->bytes_sent += len;
	if (m->telnet != MODEM_TELNET_ON) {
		send_line(m, data, len);
		return;
	}
	for (size_t k = 0; k < len; k++) {
		static const uint8_t iac2[2] = { TN_IAC, TN_IAC };
		if (data[k] == TN_IAC)
			send_line(m, iac2, 2);
		else
			send_line(m, &data[k], 1);
	}
}

static uint64_t guard_time(const struct modem *m)
{
	return (uint64_t)m->s[12] * m->timebase_hz / 50;
}

/* Back to command mode, the call kept (the escape) */
static void escape(struct modem *m)
{
	m->online = false;
	m->plus = 0;
	m->at = 0;
	if (m->log)
		printf("modem: escape to command mode\n");
}

void modem_from_dte(struct modem *m, uint8_t b, uint64_t tb)
{
	modem_set_time(m, tb);
	if (!m->online) {
		command_char(m, b);
		return;
	}
	uint64_t guard = guard_time(m);
	if (m->plus == 3) {
		/* after +++ but before its guard time: "AT...<CR>" is a command (the escape), anything else data */
		m->held[m->held_len++] = (char)b;
		m->held_at = tb;
		bool at = toupper((unsigned char)m->held[0]) == 'A' &&
			  (m->held_len < 2 || toupper((unsigned char)m->held[1]) == 'T');
		if (at && b == m->s[3] && m->held_len > 2) {
			char cmd[sizeof m->held];
			snprintf(cmd, sizeof cmd, "%.*s", (int)m->held_len - 3, m->held + 2);
			m->held_len = 0;
			escape(m);
			execute(m, cmd);
			return;
		}
		if (at && m->held_len + 1 < sizeof m->held)
			return;
		m->plus = 0;
		send_data(m, (const uint8_t *)m->held, m->held_len);
		m->held_len = 0;
		m->last_tx = tb;
		return;
	}
	if (b == m->s[2] && m->s[2] < 128 &&
	    ((m->plus == 0 && (!m->sent_any || tb - m->last_tx >= guard)) || (m->plus && tb - m->plus_at < guard))) {
		m->plus++;
		m->plus_at = tb;
	} else {
		m->plus = 0;
	}
	send_data(m, &b, 1);
	m->last_tx = tb;
	m->sent_any = true;
}

static uint64_t dtr_time(const struct modem *m)
{
	return (uint64_t)m->s[25] * m->timebase_hz / 100;
}

void modem_dte_lines(struct modem *m, bool dtr, bool rts, uint64_t tb)
{
	modem_set_time(m, tb);
	if (m->dtr && !dtr) {
		m->dtr_dropping = true;
		m->dtr_dropped_at = m->now;
	} else if (dtr) {
		m->dtr_dropping = false;
	}
	m->dtr = dtr;
	m->rts = rts;
	modem_set_time(m, tb);
}

/* DTR has stayed down for S25's time */
static void dtr_dropped(struct modem *m)
{
	m->dtr_dropping = false;
	switch (m->dtr_mode) {
	case 1:                                 /* &D1: to command mode */
		if (m->online) {
			escape(m);
			result(m, R_OK);
		}
		break;
	case 2:                                 /* &D2: hang up */
		if (m->call != MODEM_IDLE && m->call != MODEM_RINGING) {
			if (m->log)
				printf("modem: DTR dropped: hanging up\n");
			end_call(m, true);
			result(m, R_NO_CARRIER);
		}
		break;
	case 3:                                 /* &D3: hang up and reset */
		if (m->call != MODEM_RINGING)
			end_call(m, true);
		factory(m);
		update_lines(m);
		break;
	}
}

size_t modem_room(const struct modem *m)
{
	if (m->call != MODEM_CONNECTED || !m->online)
		return 0;
	return MODEM_QUEUE - m->qcount;
}

static void negotiate(struct modem *m, uint8_t cmd, uint8_t opt)
{
	uint8_t reply[3] = { TN_IAC, 0, opt };
	bool known_him = opt == TN_BINARY || opt == TN_ECHO || opt == TN_SGA;
	bool known_us = opt == TN_BINARY || opt == TN_SGA;
	switch (cmd) {
	case TN_WILL:
		if (!known_him)
			reply[1] = TN_DONT;
		else if (!m->tn_him[opt])
			reply[1] = TN_DO;
		if (known_him)
			m->tn_him[opt] = true;
		break;
	case TN_WONT:
		if (known_him && m->tn_him[opt]) {
			m->tn_him[opt] = false;
			reply[1] = TN_DONT;
		}
		break;
	case TN_DO:
		if (!known_us)
			reply[1] = TN_WONT;
		else if (!m->tn_us[opt])
			reply[1] = TN_WILL;
		if (known_us)
			m->tn_us[opt] = true;
		break;
	case TN_DONT:
		if (known_us && m->tn_us[opt]) {
			m->tn_us[opt] = false;
			reply[1] = TN_WONT;
		}
		break;
	}
	if (reply[1])
		send_line(m, reply, 3);
}

void modem_received(struct modem *m, const uint8_t *data, size_t len)
{
	if (m->call != MODEM_CONNECTED || !m->online)
		return;
	m->bytes_received += len;
	for (size_t k = 0; k < len; k++) {
		uint8_t b = data[k];
		if (m->telnet == MODEM_TELNET_AUTO) {   /* the far end's first character says */
			if (m->tn_state == TS_DATA && b == TN_IAC) {
				m->tn_state = TS_IAC;
				continue;
			}
			if (m->tn_state == TS_IAC && b >= TN_SB && b <= TN_DONT) {
				m->telnet = MODEM_TELNET_ON;
				if (m->log)
					printf("modem: the far end speaks telnet\n");
			} else {
				if (m->tn_state == TS_IAC)
					put(m, TN_IAC);
				m->tn_state = TS_DATA;
				m->telnet = MODEM_TELNET_OFF;
				put(m, b);
				continue;
			}
		}
		if (m->telnet == MODEM_TELNET_OFF) {
			put(m, b);
			continue;
		}
		switch (m->tn_state) {
		case TS_DATA:
			if (b == TN_IAC)
				m->tn_state = TS_IAC;
			else
				put(m, b);
			break;
		case TS_IAC:
			if (b == TN_IAC) {
				put(m, b);
				m->tn_state = TS_DATA;
			} else if (b >= TN_WILL) {
				m->tn_cmd = b;
				m->tn_state = TS_OPTION;
			} else {
				m->tn_state = b == TN_SB ? TS_SUB : TS_DATA;
			}
			break;
		case TS_OPTION:
			negotiate(m, m->tn_cmd, b);
			m->tn_state = TS_DATA;
			break;
		case TS_SUB:
			if (b == TN_IAC)
				m->tn_state = TS_SUB_IAC;
			break;
		default:                        /* TS_SUB_IAC */
			m->tn_state = b == TN_SE ? TS_DATA : TS_SUB;
			break;
		}
	}
	ready(m);
}

/* ---- time ---- */

void modem_set_time(struct modem *m, uint64_t tb)
{
	if (tb > m->now)
		m->now = tb;
	if (m->online && m->plus == 3 && !m->held_len && m->now - m->plus_at >= guard_time(m)) {
		escape(m);
		result(m, R_OK);
	} else if (m->online && m->plus == 3 && m->held_len && m->now - m->held_at >= guard_time(m)) {
		m->plus = 0;                    /* ("+++AT..." without its CR: data after all) */
		send_data(m, (const uint8_t *)m->held, m->held_len);
		m->held_len = 0;
		m->last_tx = m->held_at;
	}
	while (m->call == MODEM_RINGING && m->now >= m->ring_at) {
		m->rings++;
		m->s[1] = (uint8_t)(m->rings > 255 ? 255 : m->rings);
		m->ring_at += (uint64_t)RING_SECONDS * m->timebase_hz;
		result(m, R_RING);
		if (m->s[0] && m->rings >= m->s[0] && (m->dtr || m->dtr_mode == 0))
			answer(m);
	}
	if (m->dtr_dropping && m->now - m->dtr_dropped_at >= dtr_time(m))
		dtr_dropped(m);
	if (m->call == MODEM_DIALLING && m->now >= m->deadline) {
		if (m->log)
			printf("modem: no carrier from %s in S7's %u seconds\n", m->peer, m->s[7]);
		end_call(m, true);
		result(m, R_NO_CARRIER);
	}
}

uint64_t modem_next_event(const struct modem *m)
{
	uint64_t t = UINT64_MAX;
	if (m->online && m->plus == 3)
		t = (m->held_len ? m->held_at : m->plus_at) + guard_time(m);
	if (m->call == MODEM_RINGING && m->ring_at < t)
		t = m->ring_at;
	if (m->call == MODEM_DIALLING && m->deadline < t)
		t = m->deadline;
	if (m->dtr_dropping && m->dtr_dropped_at + dtr_time(m) < t)
		t = m->dtr_dropped_at + dtr_time(m);
	return t;
}

const char *modem_state_text(const struct modem *m, char *buf, size_t size)
{
	switch (m->call) {
	case MODEM_DIALLING:
		snprintf(buf, size, "dialling %s", m->peer);
		break;
	case MODEM_RINGING:
		snprintf(buf, size, "ringing: %s calling", m->peer);
		break;
	case MODEM_CONNECTED:
		snprintf(buf, size, "connected %s %s%s", m->answering ? "from" : "to", m->peer,
			 m->online ? "" : " (in command mode)");
		break;
	default:
		snprintf(buf, size, "idle");
		break;
	}
	return buf;
}

/* ---- the phonebook ---- */

int modem_phonebook_parse(struct modem_phonebook *pb, const char *text, char *err, size_t err_size)
{
	memset(pb, 0, sizeof *pb);
	unsigned line = 0;
	while (*text) {
		const char *end = strchr(text, '\n');
		size_t len = end ? (size_t)(end - text) : strlen(text);
		char l[512];
		snprintf(l, sizeof l, "%.*s", (int)(len < sizeof l ? len : sizeof l - 1), text);
		text += len + (end != NULL);
		line++;
		char *hash = strchr(l, '#');
		if (hash)
			*hash = '\0';
		char number[64], address[MODEM_ADDRESS_MAX + 8], kind[16] = "", extra[8];
		int n = sscanf(l, "%63s %263s %15s %7s", number, address, kind, extra);
		if (n <= 0)
			continue;
		struct modem_number e = { .telnet = MODEM_TELNET_AUTO };
		digits_of(number, e.number, sizeof e.number);
		bool ok = n >= 2 && n <= 3 && e.number[0] && strlen(number) < sizeof e.number;
		if (ok && n == 3) {
			if (!strcmp(kind, "telnet"))
				e.telnet = MODEM_TELNET_ON;
			else if (!strcmp(kind, "raw"))
				e.telnet = MODEM_TELNET_OFF;
			else
				ok = false;
		}
		enum modem_telnet ignored;
		if (ok)
			ok = is_address(address) && modem_address(NULL, address, e.host, sizeof e.host, &e.port, &ignored);
		if (!ok) {
			snprintf(err, err_size, "line %u: wants \"NUMBER HOST:PORT\", with \"telnet\" or \"raw\" after it "
				 "if need be", line);
			return -1;
		}
		if (pb->count == MODEM_PHONEBOOK_MAX) {
			snprintf(err, err_size, "line %u: more than %u numbers", line, MODEM_PHONEBOOK_MAX);
			return -1;
		}
		pb->entry[pb->count++] = e;
	}
	return (int)pb->count;
}
