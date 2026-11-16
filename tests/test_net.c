/*
 * Ethernet's lanLib (net.c), driven the way Mac OS's Ethernet driver
 * drives it: its exports, initialization (which installs a receive
 * interrupt handler in the node's interrupt set, or else arms a timer), the
 * station address, sending, link control, and frames from the host handed
 * to the driver's receive routine, padded to the minimum frame with room
 * for a frame check sequence.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/mem.h"
#include "../src/net.h"
#include "test.h"

#define DATA 0x10000u           /* lanLib's data section (r2) */
#define FRAME 0x20000u          /* the stub's stack frame */
#define BUF 0x30000u            /* the driver's buffers */
#define RECEIVE 0x30800u        /* the driver's receive routine (a TVector) */
#define ENTRY_ID 0x30900u       /* the Ethernet node's RegEntryID */
#define TIMEBASE 16600000u

/* lanLib's entries, imports and data, as net.c numbers them */
enum { L_INIT, L_GET_ADDR = 3, L_WRITE, L_RESTART = 6, L_CONTROL, L_LINK = 9, L_TIMER, L_INTERRUPT, L_RECEIVE };
enum { IMP_SET_TIMER, IMP_GET_INT, IMP_INSTALL_INT, IMP_QUEUE_SECONDARY, IMP_PROPERTY_GET, IMP_COUNT };
enum { X_EXPIRATION = 0x18, X_IST_SIZE = 0x24, X_RX_DESC = 0x30, X_IST = 0x40, X_FUNCS = 0x58, X_IST_NAME = 0x68,
       X_RX_BUF = 0x80 };
static const uint32_t import_tvector[IMP_COUNT] = { 0xAAA0, 0xBBB0, 0xCCC0, 0xDDD0, 0xEEE0 };

static struct mem mem;
static struct net n;

static struct ndrv_call call(uint32_t entry, uint32_t a0, uint32_t a1, uint64_t tb)
{
	struct ndrv_call c;
	memset(&c, 0, sizeof c);
	c.mem = &mem;
	c.entry = entry;
	c.arg[0] = a0;
	c.arg[1] = a1;
	c.toc = DATA;
	c.frame = FRAME;
	c.tb = tb;
	net_call(&n, &c);
	return c;
}

/* An export of a PEF container: its class and value (false if there's none) */
static bool export_of(const uint8_t *pef, const char *name, unsigned *cls, uint32_t *value)
{
	const uint8_t *ld = pef + be32(pef + 40 + 2 * 28 + 20);
	const uint8_t *strings = ld + be32(ld + 40);
	uint32_t count = be32(ld + 52);
	const uint8_t *keys = ld + be32(ld + 44) + (4u << be32(ld + 48));
	const uint8_t *syms = keys + 4 * count;
	for (uint32_t k = 0; k < count; k++) {
		uint32_t len = be32(keys + 4 * k) >> 16, w = be32(syms + 10 * k);
		if (len == strlen(name) && !memcmp(strings + (w & 0xFFFFFF), name, len)) {
			*cls = w >> 24;
			*value = be32(syms + 10 * k + 4);
			return true;
		}
	}
	return false;
}

void test_net(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init failed");
		return;
	}
	mem_map(&mem, 0, 0x40000, MEM_RW);
	static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
	net_init(&n, &mem, mac, TIMEBASE);

	static uint8_t lib[8192];
	size_t size = net_build_lanlib(&n, lib, sizeof lib);
	CHECK(size > 0);
	unsigned cls;
	uint32_t value;
	CHECK(export_of(lib, "InitializeLotsOStuff", &cls, &value) && cls == 2 && value == n.layout.entry[L_INIT]);
	CHECK(export_of(lib, "hal_enet_write", &cls, &value) && cls == 2);
	CHECK(export_of(lib, "gExportedData", &cls, &value) && cls == 1 && value == n.layout.extra);

	/* the data section, with made-up TVectors where the imports go */
	const uint8_t *data = lib + be32(lib + 40 + 28 + 20);
	memcpy(mem_ptr(&mem, DATA), data, be32(lib + 40 + 28 + 16));
	for (unsigned k = 0; k < IMP_COUNT; k++)
		mem_w32(&mem, DATA + n.layout.import[k], import_tvector[k]);
	const uint32_t x = DATA + n.layout.extra, exported = x;

	/*
	 * Initializing looks for the node's interrupt set; without one, it arms
	 * the timer, then says yes. gExportedData gets lanLib's routines.
	 */
	uint64_t tb = 1000000000u;
	mem_w32(&mem, exported, RECEIVE);               /* (the driver's, set before) */
	struct ndrv_call c = call(L_INIT, ENTRY_ID, 0, tb);
	CHECK(!c.done && c.tvector == import_tvector[IMP_PROPERTY_GET]);
	CHECK(c.out[0] == ENTRY_ID && c.out[2] == x + X_IST && c.out[3] == x + X_IST_SIZE);
	CHECK(!strcmp((const char *)mem_ptr(&mem, c.out[1]), "driver-ist"));
	CHECK_EQ(mem_r32(&mem, x + X_IST_SIZE), 24);
	c = call(NDRV_ENTRY_RESUME, 0xFFFFF615u, 0, tb);        /* nrNotFoundErr */
	CHECK(!c.done && c.tvector == import_tvector[IMP_SET_TIMER]);
	CHECK_EQ(c.out[0], x + X_EXPIRATION);
	CHECK_EQ(c.out[1], DATA + n.layout.entry[L_TIMER]);
	CHECK_EQ(mem_r64(&mem, x + X_EXPIRATION), tb + TIMEBASE / 100);    /* 10 ms: no traffic yet */
	CHECK_EQ(mem_r32(&mem, exported), RECEIVE);
	for (unsigned k = 1; k <= 4; k++)
		CHECK_EQ(mem_r32(&mem, exported + 4 * k), DATA + n.layout.entry[L_RESTART + k - 1]);
	c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
	CHECK(c.done && c.result == 1);

	c = call(L_GET_ADDR, BUF, 0, tb);
	CHECK(c.done && !memcmp(mem_ptr(&mem, BUF), mac, 6));

	c = call(L_WRITE, BUF, 60, tb);                 /* (no host network: counted, dropped) */
	CHECK(c.done);
	CHECK_EQ(n.sent, 1);

	mem_w32(&mem, BUF + 0x100, 1);                  /* link control: which modes? all four */
	c = call(L_LINK, BUF + 0x100, 0, tb);
	CHECK(c.done && mem_r32(&mem, BUF + 0x104) == 0xF && mem_r32(&mem, BUF + 0x108) == 0xF);

	/* frames from the host: each to the receive routine (short ones padded), then the timer again */
	uint8_t arp[42], big[100];
	for (unsigned k = 0; k < sizeof arp; k++)
		arp[k] = (uint8_t)(k + 1);
	for (unsigned k = 0; k < sizeof big; k++)
		big[k] = (uint8_t)(200 - k);
	net_input(&n, arp, sizeof arp);
	net_input(&n, big, sizeof big);
	tb += TIMEBASE / 100;
	c = call(L_TIMER, 0, 0, tb);
	CHECK(!c.done && c.tvector == RECEIVE && c.out[0] == x + X_RX_DESC);
	CHECK_EQ(mem_r32(&mem, x + X_RX_DESC), x + X_RX_BUF);
	CHECK_EQ(mem_r32(&mem, x + X_RX_DESC + 8), 64);         /* 60 bytes and a frame check sequence */
	CHECK(!memcmp(mem_ptr(&mem, x + X_RX_BUF), arp, sizeof arp));
	CHECK_EQ(mem_r32(&mem, x + X_RX_BUF + 56), 0);
	c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
	CHECK(!c.done && c.tvector == RECEIVE);
	CHECK_EQ(mem_r32(&mem, x + X_RX_DESC + 8), 104);
	CHECK(!memcmp(mem_ptr(&mem, x + X_RX_BUF), big, sizeof big));
	c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
	CHECK(!c.done && c.tvector == 0x0000AAA0u);
	CHECK_EQ(mem_r64(&mem, x + X_EXPIRATION), tb + TIMEBASE / 500);     /* 2 ms while there's traffic */
	c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
	CHECK(c.done && c.result == 0);
	CHECK_EQ(n.received, 2);

	/* shut down: the timer stops */
	c = call(L_CONTROL, 4, 0, tb);
	CHECK(c.done);
	net_input(&n, arp, sizeof arp);
	c = call(L_TIMER, 0, 0, tb);
	CHECK(c.done && c.result == 0);
	CHECK_EQ(n.received, 2);
	net_free(&n);

	/*
	 * With an interrupt set: lanLib's handler for its receive DMA member,
	 * enabled. That's the set's last: the iMac's has all three of the node's
	 * interrupts (the MAC's, then the transmit and receive channels'),
	 * Blue's just the last.
	 */
	static const uint32_t set_members[] = { 3, 1 };
	for (unsigned s = 0; s < 2; s++) {
		uint32_t members = set_members[s];
		net_init(&n, &mem, mac, TIMEBASE);
		CHECK_EQ(net_build_lanlib(&n, lib, sizeof lib), size);
		memcpy(mem_ptr(&mem, DATA), data, be32(lib + 40 + 28 + 16));
		for (unsigned k = 0; k < IMP_COUNT; k++)
			mem_w32(&mem, DATA + n.layout.import[k], import_tvector[k]);
		mem_w32(&mem, exported, RECEIVE);
		memset(mem_ptr(&mem, x + X_IST), 0xEE, 24);    /* (left from before) */
		c = call(L_INIT, ENTRY_ID, 0, tb);
		CHECK(!c.done && c.tvector == import_tvector[IMP_PROPERTY_GET]);
		for (unsigned k = 0; k < 6; k++)                /* cleared (Mac OS leaves the size at 24) */
			CHECK_EQ(mem_r32(&mem, x + X_IST + 4 * k), 0);
		for (unsigned k = 0; k < members; k++) {
			mem_w32(&mem, x + X_IST + 8 * k, 0x1234);
			mem_w32(&mem, x + X_IST + 8 * k + 4, k + 1);
		}
		c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
		CHECK(!c.done && c.tvector == import_tvector[IMP_GET_INT]);
		CHECK(c.out[0] == 0x1234 && c.out[1] == members && c.out[2] == x + X_FUNCS &&
		      c.out[5] == x + X_FUNCS + 12);
		mem_w32(&mem, x + X_FUNCS, 0x5555);             /* refCon, handler, enabler, disabler */
		mem_w32(&mem, x + X_FUNCS + 4, 0x6666);
		mem_w32(&mem, x + X_FUNCS + 8, 0x7777);
		mem_w32(&mem, x + X_FUNCS + 12, 0x8888);
		c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
		CHECK(!c.done && c.tvector == import_tvector[IMP_INSTALL_INT]);
		CHECK(c.out[0] == 0x1234 && c.out[1] == members && c.out[2] == 0x5555);
		CHECK(c.out[3] == DATA + n.layout.entry[L_INTERRUPT] && c.out[4] == 0x7777 && c.out[5] == 0x8888);
		c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
		CHECK(!c.done && c.tvector == 0x7777 && c.out[0] == 0x1234 && c.out[1] == members &&
		      c.out[2] == 0x5555);
		c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
		CHECK(c.done && c.result == 1);
		CHECK(n.irq && !n.kick);
		if (s == 0)
			net_free(&n);
	}

	/* a frame pulses the interrupt; the handler queues the secondary handler (once), which receives */
	net_input(&n, arp, sizeof arp);
	CHECK(n.kick);
	c = call(L_INTERRUPT, 0x1234, 1, tb);
	CHECK(!c.done && c.tvector == import_tvector[IMP_QUEUE_SECONDARY]);
	CHECK_EQ(c.out[0], DATA + n.layout.entry[L_RECEIVE]);
	c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
	CHECK(c.done && c.result == 0);                 /* kIsrIsComplete */
	c = call(L_INTERRUPT, 0x1234, 1, tb);
	CHECK(c.done && c.result == 0);
	c = call(L_RECEIVE, 0, 0, tb);
	CHECK(!c.done && c.tvector == RECEIVE && c.out[0] == x + X_RX_DESC);
	CHECK(!memcmp(mem_ptr(&mem, x + X_RX_BUF), arp, sizeof arp));
	c = call(NDRV_ENTRY_RESUME, 0, 0, tb);
	CHECK(c.done && c.result == 0);                 /* (no timer) */
	CHECK_EQ(n.received, 1);
	CHECK(!n.queued);

	net_free(&n);
	mem_free(&mem);
}
