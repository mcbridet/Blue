/*
 * Multiprocessing Services as NanoKernel 2 provides them, for ROM files from
 * Mac OS 9.1 on, which start them themselves: calls through `sc` (or kernel
 * call 8) with their number in r0, arguments from r3 and results in r3 (a
 * status) and the registers after it. There's one processor and one task
 * that runs, the blue task (Mac OS, in the blue process); calls are added
 * as Mac OS makes them.
 *
 * Objects have NanoKernel 2's opaque IDs (a slot number << 16 | the count
 * of the slot's uses), and its classes. After the reverse-engineered
 * NanoKernel 2 (github.com/elliotnunn/powermac-rom).
 */
#ifndef SYSHM_MP_H
#define SYSHM_MP_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "ppc.h"

enum mp_class {
	MP_FREE, MP_PROCESS, MP_TASK, MP_TIMER, MP_QUEUE, MP_SEMAPHORE, MP_CRITICAL_REGION, MP_CPU,
	MP_ADDRESS_SPACE, MP_EVENT_GROUP, MP_COHERENCE_GROUP, MP_AREA, MP_NOTIFICATION, MP_CONSOLE_LOG,
};

/* Status codes */
#define MP_NO_ERR 0
#define MP_UNIMPLEMENTED (-4)                   /* (NanoKernel 2's for calls it doesn't have) */
#define MP_PARAM_ERR (-50)
#define MP_INVALID_ID_ERR (-29299)
#define MP_INSUFFICIENT_RESOURCES_ERR (-29298)
#define MP_TIMEOUT_ERR (-29296)
#define MP_BLUE_BLOCKING_ERR (-29293)           /* the blue task may not wait: it spins instead */

#define MP_MAX_OBJECTS 1024u
#define MP_MAX_MESSAGES 256u

struct mp_object {
	uint8_t class;
	uint16_t uses;
	union {
		struct { uint32_t label, exceptions; } task;    /* (label: a word call 63 sets; exceptions: a queue) */
		struct { uint32_t reserve; } queue;
		struct { uint32_t value, max; } sem;
		struct { uint32_t depth, key; } region;         /* (key: the blue task's, as it enters) */
		struct { uint32_t flags, swi; } event;          /* (swi: 0, or 0x10 | the 68k software interrupt it raises) */
		struct { uint32_t queue, word[3], event, flags, sem; } note;    /* what causing it does */
		struct { uint32_t base, length, provider, provider_misc; bool writable; } area;  /* (provider: a notification for its page faults) */
	};
};

struct mp_message {
	uint32_t queue, word[3];
};

struct mp {
	struct mp_object obj[MP_MAX_OBJECTS];
	unsigned slots;                 /* slots used so far */
	struct mp_message msg[MP_MAX_MESSAGES];         /* waiting on all the queues, oldest first */
	unsigned messages;
	uint32_t blue_process, blue_task, page_queue, space;
	uint32_t swi_raised;            /* an event group set that raises a software interrupt (not delivered yet) */
	bool log;
};

/*
 * Makes the objects a NanoKernel 2 has at startup: the blue process (ID 1),
 * the blue task, the page queue and the address space (its areas follow).
 */
void mp_init(struct mp *mp);

/* Adds an area of the address space (startup's, from the page map); returns its ID, or 0 if it overlaps one. */
uint32_t mp_add_area(struct mp *mp, uint32_t base, uint32_t length, bool writable);

uint32_t mp_make(struct mp *mp, enum mp_class class);
struct mp_object *mp_lookup(struct mp *mp, uint32_t id, enum mp_class class);

/* Does call r0 for the blue task; returns false if it isn't implemented (nothing changed). */
bool mp_call(struct mp *mp, struct ppc_cpu *cpu, struct mem *mem);

#endif
