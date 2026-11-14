/*
 * Multiprocessing Services as NanoKernel 2 provides them, for ROM files from
 * Mac OS 9.1 on, which start them themselves: calls through `sc` (or kernel
 * call 8) with their number in r0, arguments from r3 and results in r3 (a
 * status) and the registers after it. There's one processor. Mac OS runs
 * as the blue task (in the blue process); other tasks run while it polls
 * or naps, one at a time, until they block or end. Calls are added as Mac
 * OS makes them.
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
#define MP_TASK_ABORTED_ERR (-29297)
#define MP_TIMEOUT_ERR (-29296)
#define MP_BLUE_BLOCKING_ERR (-29293)           /* the blue task may not wait: it spins instead */

#define MP_MAX_OBJECTS 1024u
#define MP_MAX_MESSAGES 256u
#define MP_MAX_TASKS 64u

enum mp_task_state {
	MP_TASK_NEW,            /* made, not started yet */
	MP_TASK_READY,          /* waiting for the processor */
	MP_TASK_RUNNING,
	MP_TASK_BLOCKED,        /* waiting in a call, its PC on the `sc`: it makes the call again when it runs */
	MP_TASK_TERMINATED,     /* until deleted */
};

/* A task's registers while it's off the processor */
struct mp_regs {
	uint32_t gpr[32];
	uint64_t fpr[32];
	uint32_t cr, xer, lr, ctr, pc, fpscr;
};

struct mp_notify {
	uint32_t queue, word[3], event, flags, sem;
};

struct mp_object {
	uint8_t class;
	uint16_t uses;
	union {
		struct {
			uint32_t label;                 /* a word for Multiprocessing Services (call 63 sets it) */
			uint32_t exceptions;            /* a queue for its exceptions */
			uint32_t process, name, options, weight;
			uint32_t queue, message[3];     /* its termination message, and where it goes */
			uint8_t state, priority;
			uint8_t regs;                   /* its entry in mp->task */
		} task;
		struct { struct mp_notify notify; uint64_t time; bool armed, keep; } timer;    /* (notify: what firing does) */
		struct { uint32_t reserve; } queue;
		struct { uint32_t value, max; } sem;
		struct { uint32_t depth, key; } region;         /* (key: the blue task's, as it enters) */
		struct { uint32_t flags, swi; } event;          /* (swi: 0, or 0x10 | the 68k software interrupt it raises) */
		struct mp_notify note;                          /* what causing it does */
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
	uint32_t debugger;              /* a queue for tasks' exceptions (call 59), or 0 */
	uint32_t swi_raised;            /* an event group set that raises a software interrupt (not delivered yet) */
	struct { uint32_t id; struct mp_regs regs; } task[MP_MAX_TASKS];        /* (id 0: a free entry) */
	unsigned tasks;                 /* tasks there are (NanoKernelInfo's count) */
	uint32_t ready[MP_MAX_TASKS];   /* tasks other than the blue one that are ready, in the order they run */
	unsigned readies;
	uint32_t current;               /* the task on the processor */
	bool yield;                     /* ... gives it up: it blocked or ended, or (the blue task) polled */
	uint32_t task_return;           /* where a new task's entry point returns to: it ends there (nk.c sees to it) */
	bool log;
};

/*
 * Makes the objects a NanoKernel 2 has at startup: the blue process (ID 1),
 * the blue task (on the processor), the page queue and the address space
 * (its areas follow).
 */
void mp_init(struct mp *mp);

/* Adds an area of the address space (startup's, from the page map); returns its ID, or 0 if it overlaps one. */
uint32_t mp_add_area(struct mp *mp, uint32_t base, uint32_t length, bool writable);

uint32_t mp_make(struct mp *mp, enum mp_class class);
struct mp_object *mp_lookup(struct mp *mp, uint32_t id, enum mp_class class);

/*
 * Does call r0 for the current task; returns false if it isn't implemented
 * (nothing changed). If the task blocks, r3 is left as it was, and the
 * call is to be made again (from the `sc`) when it next runs.
 */
bool mp_call(struct mp *mp, struct ppc_cpu *cpu, struct mem *mem);

/* Ends a task other than the blue one as MPTerminateTask (call 9) does, its result in its message's third word. */
int32_t mp_terminate(struct mp *mp, uint32_t task, uint32_t result);

/* The task to run next once the current one yields: the first ready one, else the blue task. */
uint32_t mp_next(const struct mp *mp);

/* Whether a task other than the blue one is ready. */
static inline bool mp_ready(const struct mp *mp)
{
	return mp->readies != 0;
}

/* A task's saved registers. */
struct mp_regs *mp_regs(struct mp *mp, uint32_t task);

/*
 * Makes `task` the current task, once its registers are on the processor.
 * The one before, if it was still running, is ready again (at the end of
 * the ready queue, unless it's the blue task).
 */
void mp_switch(struct mp *mp, uint32_t task);

#endif
