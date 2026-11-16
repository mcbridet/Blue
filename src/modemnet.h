/*
 * The modem's calls over TCP (real time only, as for Ethernet): dialling
 * connects to host:port (resolving and connecting on a thread of its own,
 * so a slow name server doesn't hold the guest up), and with a port to
 * listen on (--modem-listen), a connection coming in rings. One call at a
 * time: a connection coming in during a call is closed at once.
 */
#ifndef BLUE_MODEMNET_H
#define BLUE_MODEMNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "modem.h"

struct modemnet_dial;

struct modemnet {
	struct modem *modem;
	int listen_fd;                  /* (-1: not listening) */
	char listening[80];             /* ... on this, as said to the user */
	unsigned port;                  /* ... port */
	int fd;                         /* the call's connection (-1: none) */
	bool incoming;                  /* ... which came in (ringing until answered) */
	bool up;                        /* ... and the call's on */
	struct modemnet_dial *dial;     /* a call going out, not connected yet */
	uint8_t *out;                   /* what waits to be sent */
	size_t out_len, out_cap;
	bool log;
};

void modemnet_init(struct modemnet *n, struct modem *m);

/*
 * Listens for calls on "[ADDRESS:]PORT" (all the host's addresses without
 * one; port 0: one the system picks); false with why in err
 */
bool modemnet_listen(struct modemnet *n, const char *spec, char *err, size_t err_size);

/* The modem's line (struct modem_line's functions, ctx being the modemnet) */
bool modemnet_dial(void *ctx, const char *host, unsigned port);
bool modemnet_answer(void *ctx);
void modemnet_send(void *ctx, const uint8_t *data, size_t len);
void modemnet_hangup(void *ctx);

/* Services the connections without waiting; true if the modem heard something */
bool modemnet_poll(struct modemnet *n);

/* Listening, or with a call (or one being made): worth polling */
bool modemnet_active(const struct modemnet *n);

void modemnet_free(struct modemnet *n);

#endif
