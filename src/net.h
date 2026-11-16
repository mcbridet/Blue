/*
 * Ethernet: the built-in controller's "lanLib", and a user-mode network on
 * the host behind it.
 *
 * On New World Macs, the firmware puts a code fragment in the Ethernet
 * node's device-tree property "lanLib,AAPL,MacOS,PowerPC". The ROM
 * registers it as the shared library lanLib, and Mac OS's Ethernet driver
 * ("Ethernet (Built-In)", whose "paddington" fragment matches the node's
 * "bmac+") is a STREAMS/DLPI wrapper around it for Open Transport: it calls
 * InitializeLotsOStuff, gets the station address with hal_get_enet_addr,
 * sends with hal_enet_write, and puts its receive routine in the first word
 * of gExportedData, whose other words are lanLib's routines for it (restart
 * receiving, shut down, set the receive filter, link control).
 *
 * Our lanLib is generated like the drivers (ndrv.h): its routines are host
 * calls. Frames go to and come from libslirp (loaded at run time), which
 * gives the guest NAT, DHCP and DNS on 10.0.2.0/24. Received frames wait in
 * a queue; each pulses the receive DMA channel's interrupt, whose handler in
 * lanLib (or else a timer) hands them to the driver's receive routine.
 */
#ifndef BLUE_NET_H
#define BLUE_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "ndrv.h"

#define HOSTCALL_LANLIB 0x110u
#define NET_QUEUE 64u
#define NET_FRAME_MAX 1518u             /* (without the frame check sequence) */

struct net_frame {
	uint16_t len;
	uint8_t data[NET_FRAME_MAX];
};

struct net {
	struct mem *mem;
	struct ndrv_layout layout;      /* of lanLib */
	uint8_t mac[6];
	uint32_t timebase_hz;
	bool log;
	bool ready;                     /* lanLib is receiving */
	bool irq;                       /* ... by interrupt (its handler is installed) */
	bool kick;                      /* a frame came: pulse the receive interrupt */
	bool queued;                    /* the handler queued the secondary handler, which hasn't run yet */
	uint64_t traffic_tb;            /* when a frame last came or went */
	/* frames for the guest */
	struct net_frame queue[NET_QUEUE];
	unsigned head, count;
	/* the host side (libslirp), if any */
	struct net_backend *backend;
	unsigned long sent, received, dropped;
	uint64_t bytes_sent, bytes_received;    /* (frames' bytes, without the frame check sequence) */
};

void net_init(struct net *n, struct mem *mem, const uint8_t mac[6], uint32_t timebase_hz);
void net_free(struct net *n);

/* lanLib's code fragment; returns its size (0 if `size` is too small) */
size_t net_build_lanlib(struct net *n, uint8_t *out, size_t size);

/* Connects the guest to the host's network; false (with a message) if libslirp can't be loaded */
bool net_start(struct net *n);

/* Runs a call into lanLib (see ndrv.h) */
void net_call(struct net *n, struct ndrv_call *c);

/* A frame from the host network for the guest (dropped if the queue is full) */
void net_input(struct net *n, const uint8_t *frame, size_t len);

/* Services the host side, waiting up to timeout_ms for something to happen */
void net_poll(struct net *n, int timeout_ms);

#endif
