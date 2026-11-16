/*
 * The iMac's internal 56K modem, as a Hayes-compatible modem whose calls go
 * over TCP, as DOSBox's and 86Box's do. It sits on the SCC's channel A
 * (scc.h) and answers the AT commands Mac OS's modem script for it ("iMac
 * Internal 56k") sends, and those terminal programs use: ATZ, AT&F, E, V, Q,
 * X, M, L, H, O, A, D, S registers (S0 auto-answer, S2 and S12 for the
 * escape with its guard time), &C (DCD follows the carrier), &D (what DTR
 * dropping does, once it has stayed down for S25's time: a serial driver
 * resetting its channel drops it for a moment), I, &V, with verbose or
 * numeric result codes, and accepts
 * the rest (\N, %C, &K...) with OK.
 *
 * Dialling "host:port" (or a number from the phonebook) calls there, and
 * answers CONNECT 56000; a call coming in rings (RING), for ATA or S0. Where
 * the calls go is the far end's business (struct modem_line): TCP in real
 * time (modemnet.h), or an input script's. Telnet (RFC 854) is spoken on
 * calls to port 23, to phonebook entries marked so, and on calls whose far
 * end opens with a telnet command (as BBSes do): 0xFF is doubled, and option
 * negotiation is answered minimally (binary and suppress-go-ahead accepted,
 * echo accepted from the far end, nothing else).
 *
 * Time is guest time (timebase ticks), so the modem behaves the same for the
 * same input: `now` is what the caller says it is.
 */
#ifndef BLUE_MODEM_H
#define BLUE_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODEM_QUEUE 4096u               /* characters for the computer */
#define MODEM_PHONEBOOK_MAX 64u
#define MODEM_ADDRESS_MAX 256u
#define MODEM_SPEED 56000u              /* what CONNECT says */

/* The far end: where calls go (any may be NULL: then there's nothing there) */
struct modem_line {
	/* starts a call to host:port: modem_connected() or modem_failed() follows; false: no line (NO CARRIER) */
	bool (*dial)(void *ctx, const char *host, unsigned port);
	/* picks up the call ringing in: true if it's connected now */
	bool (*answer)(void *ctx);
	void (*send)(void *ctx, const uint8_t *data, size_t len);
	void (*hangup)(void *ctx);
	void *ctx;
};

/* The computer's side: the serial port */
struct modem_dte {
	void (*ready)(void *ctx);                       /* the modem has characters for it */
	void (*lines)(void *ctx, bool dcd, bool cts);   /* its DCD and CTS outputs changed */
	void *ctx;
};

enum modem_telnet { MODEM_TELNET_AUTO, MODEM_TELNET_ON, MODEM_TELNET_OFF };

struct modem_phonebook {
	unsigned count;
	struct modem_number {
		char number[32];                /* digits (and * #) only */
		char host[MODEM_ADDRESS_MAX];
		unsigned port;
		enum modem_telnet telnet;
	} entry[MODEM_PHONEBOOK_MAX];
};

enum modem_call { MODEM_IDLE, MODEM_DIALLING, MODEM_RINGING, MODEM_CONNECTED };

struct modem {
	struct modem_line line;
	struct modem_dte dte;
	const struct modem_phonebook *book;     /* (NULL: none) */
	uint32_t timebase_hz;
	uint64_t now;                   /* guest time */
	bool log;                       /* print commands, results and calls */

	/* settings */
	uint8_t s[100];                 /* S registers */
	unsigned sreg;                  /* the one last named (for "=" and "?" alone) */
	bool echo, verbose, quiet;
	unsigned x, dcd_mode, dtr_mode, speaker, volume;

	/* the command line (after "AT") */
	char cmd[256];
	unsigned cmd_len;
	unsigned at;                    /* 0: waiting for A, 1: for T, 2: in a command */
	char last[256];                 /* the last command, for A/ */
	char dialled[MODEM_ADDRESS_MAX];        /* digits dialled so far by "ATD...;" */

	/* the call */
	enum modem_call call;
	bool online;                    /* data goes over it (otherwise commands are taken) */
	bool answering;                 /* (the call came in) */
	char peer[MODEM_ADDRESS_MAX];   /* host:port of the call */
	uint64_t deadline;              /* dialling gives up then (S7) */
	unsigned rings;                 /* a call ringing in: rings so far ... */
	uint64_t ring_at;               /* ... and the next */
	bool dtr, rts;
	bool dtr_dropping;              /* DTR has dropped: the modem acts if it stays so (S25) ... */
	uint64_t dtr_dropped_at;        /* ... until then */
	bool dcd_out, cts_out;          /* what the DTE was last told */

	/* the escape: +++ with the guard time (S12) before and after */
	uint64_t last_tx;               /* when the computer last sent a character */
	bool sent_any;
	unsigned plus;                  /* escape characters so far */
	uint64_t plus_at;               /* ... the last one's time */
	char held[260];                 /* what came after them: "AT..." up to a CR runs as a command */
	unsigned held_len;
	uint64_t held_at;               /* ... the last of it came then (data, if no CR comes in the guard time) */

	/* telnet */
	enum modem_telnet telnet;       /* (AUTO: decided by the far end's first character) */
	unsigned tn_state;
	uint8_t tn_cmd;
	bool tn_us[4], tn_him[4];       /* options on at our end and the far end's (binary, echo, -, suppress GA) */

	/* characters for the computer */
	uint8_t q[MODEM_QUEUE];
	size_t qhead, qcount;

	unsigned long calls;
	uint64_t bytes_sent, bytes_received;    /* over calls */
};

void modem_init(struct modem *m, uint32_t timebase_hz);

/* Guest time now: timers (the escape's guard time, rings, dialling's timeout) run to it */
void modem_set_time(struct modem *m, uint64_t tb);

/* The first guest time at which the modem changes by itself (UINT64_MAX: none) */
uint64_t modem_next_event(const struct modem *m);

/* From the computer: a character (at guest time tb), and its DTR and RTS */
void modem_from_dte(struct modem *m, uint8_t byte, uint64_t tb);
void modem_dte_lines(struct modem *m, bool dtr, bool rts, uint64_t tb);

/* The next character for the computer, or -1 */
int modem_to_dte(struct modem *m);

/* From the far end: the call it started is up (or failed: busy, or no answer), or one comes in */
void modem_connected(struct modem *m);
void modem_failed(struct modem *m, bool busy);
void modem_incoming(struct modem *m, const char *peer);

/* Characters from the far end; how many it can take now (the rest waits there) */
size_t modem_room(const struct modem *m);
void modem_received(struct modem *m, const uint8_t *data, size_t len);

/* The far end hung up (or the call ringing in went away) */
void modem_far_hangup(struct modem *m);

/* Hangs up, as ATH does (the window's Hang Up) */
void modem_hangup(struct modem *m);

/* "connected to host:port", "dialling host:port", "ringing (host:port)" or "idle" */
const char *modem_state_text(const struct modem *m, char *buf, size_t size);

/*
 * Phonebook text: lines of "NUMBER HOST:PORT [telnet|raw]" ('#' starts a
 * comment). Returns the entries read, or -1 with the first bad line's
 * number and why in `err`.
 */
int modem_phonebook_parse(struct modem_phonebook *pb, const char *text, char *err, size_t err_size);

/*
 * What a dial string calls: host and port (and telnet) for "host[:port]"
 * (23 without a port) or a phonebook number. Returns false if it calls
 * nowhere (an unknown number, nothing).
 */
bool modem_address(const struct modem_phonebook *pb, const char *dial, char *host, size_t host_size,
		   unsigned *port, enum modem_telnet *telnet);

#endif
