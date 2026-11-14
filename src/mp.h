/*
 * Multiprocessing Services as NanoKernel 2 provides them, for ROM files from
 * Mac OS 9.1 on, which start them themselves: calls through `sc` (or kernel
 * call 8) with their number in r0, arguments from r3 and results in r3 (a
 * status) and the registers after it. There's one processor. Mac OS runs
 * as the blue task (in the blue process); calls are added as Mac OS makes
 * them.
 *
 * Scheduling follows NanoKernel 2's: the ready task of the most urgent
 * priority runs (critical, latency-protected, nominal, idle), first come
 * first served; a task that's preempted or yields goes to the back. A
 * task that's woken is latency-protected, and so runs before nominal ones
 * (the blue task's priority); the blue task is critical once an interrupt
 * is posted to it, and while its 68k interrupts are masked. Each runs for
 * a time slice, its weight's share of its priority's "time cake" (1 ms for
 * critical, 8 times longer for each priority after): then a critical or
 * latency-protected task becomes nominal, and tasks of its priority get
 * their turn. A task that waits blocks with its PC on the `sc`, and makes
 * the call again once woken: what it waited for is there then, or the call
 * returns the status it was woken with (a deletion, a semaphore handed
 * over). The blue task never blocks: it polls, which yields, or naps,
 * while tasks run. Time is the guest timebase: time slices end at
 * timebase values, checked by mp_tick().
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
#define MP_DELETED_ERR (-29295)
#define MP_BLUE_BLOCKING_ERR (-29293)           /* the blue task may not wait: it spins instead */

#define MP_MAX_OBJECTS 1024u
#define MP_MAX_MESSAGES 256u
#define MP_MAX_TASKS 64u

enum mp_task_state {
	MP_TASK_NEW,            /* made, not started yet */
	MP_TASK_READY,          /* waiting for the processor */
	MP_TASK_RUNNING,
	MP_TASK_BLOCKED,        /* waiting in a call, its PC on the `sc`: it makes the call again when it runs; the blue task napping */
	MP_TASK_TERMINATED,     /* until deleted */
};

enum mp_priority { MP_CRITICAL, MP_LATENCY_PROTECTED, MP_NOMINAL, MP_IDLE };
#define MP_AS_IS 4u                     /* (for mp_next() and mp_tick(): the blue task's priority stays) */

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
			uint32_t waiting;               /* blocked: the queue, semaphore, event group or critical region (0: none) */
			uint32_t since;                 /* ... from this (mp->blocks when it blocked: the first to block is woken first) */
			int32_t resume;                 /* the status the call it makes again returns (BLOCKED: it makes it) */
			uint8_t state, priority;
			uint8_t regs;                   /* its entry in mp->task */
		} task;
		struct { struct mp_notify notify; uint64_t time; bool armed, keep; } timer;    /* (notify: what firing does) */
		struct { uint32_t reserve; } queue;
		struct { uint32_t value, max; } sem;
		struct { uint32_t depth, owner, key; } region;  /* (owner: the task in it; key: its process, as it entered) */
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
	uint32_t ready[MP_MAX_TASKS];   /* tasks that are ready, in the order they came (the running one isn't) */
	unsigned readies;
	uint32_t current;               /* the task on the processor */
	bool yield;                     /* ... may have to give it up (see mp_next()) */
	uint64_t slice_end;             /* ... until this timebase value, its time slice */
	uint64_t next_tb;               /* mp_tick() is due at this timebase value */
	uint32_t blocks;                /* tasks that have blocked so far */
	uint32_t polled;                /* the object the blue task last polled; UINT32_MAX: it has become available since */
	uint64_t nap_until;             /* the blue task's nap (it's blocked) ends at this timebase value, or at an interrupt */
	uint64_t cake[4];               /* each priority's time to share, in timebase ticks */
	uint32_t tb_hz;                 /* the timebase's rate */
	uint32_t free_pages;            /* the system's free page list: pages the kernel has to spare (see nk.c) */
	uint32_t ager;                  /* the page ager's notification, for when they run short (call 82), or 0 */
	uint32_t task_return;           /* where a new task's entry point returns to: it ends there (nk.c sees to it) */
	bool log;
};

/*
 * Makes the objects a NanoKernel 2 has at startup: the blue process (ID 1),
 * the blue task (on the processor), the page queue and the address space
 * (its areas follow). `tb_hz` is the timebase's rate.
 */
void mp_init(struct mp *mp, uint32_t tb_hz);

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

/*
 * The task to run next, once mp->yield is set (NanoKernel 2's SchEval):
 * the current one, if it's still running and no ready one is more urgent;
 * else the most urgent ready one; else (all the others blocked) the blue
 * task, whose nap then ends. The blue task, if current and not napping,
 * first takes the priority `boost` (MP_AS_IS: none), what its 68k
 * interrupt mask calls for (NanoKernel 2's SchFiddlePriorityShifty: see
 * nk.c).
 */
uint32_t mp_next(struct mp *mp, unsigned boost);

/*
 * Makes `task` (mp_next()'s) the current task, once its registers are on
 * the processor, and starts its time slice at `now`: a new one, unless
 * it's the current task, still running at nominal or idle priority. The
 * one before, if it was still running, goes to the back of the ready
 * queue.
 */
void mp_switch(struct mp *mp, uint32_t task, uint64_t now);

/* Whether a task other than the blue one is ready. */
bool mp_ready(const struct mp *mp);

/* A task's saved registers. */
struct mp_regs *mp_regs(struct mp *mp, uint32_t task);

/* An interrupt has been posted to the blue task: it's critical, and runs next. */
void mp_interrupt(struct mp *mp, uint64_t now);

/*
 * The blue task (running) naps until `until` (UINT64_MAX: an interrupt):
 * MP_NAP_SKIPPED, it doesn't, as what it last polled has become available
 * since; MP_NAP_IDLE, there's nothing else to run; MP_NAP_YIELDED, it's
 * blocked, and tasks that are ready run meanwhile.
 */
enum mp_nap { MP_NAP_SKIPPED, MP_NAP_IDLE, MP_NAP_YIELDED };
enum mp_nap mp_nap(struct mp *mp, uint64_t until);

/* Whether the blue task is napping (and not current, unless mp_next() has just chosen it). */
bool mp_napping(const struct mp *mp);

/* Whether mp_tick() has something to do by `now`. */
static inline bool mp_due(const struct mp *mp, uint64_t now)
{
	return now >= mp->next_tb;
}

/*
 * Ends what ends by `now`: the current task's time slice (a critical or
 * latency-protected task becomes nominal, and tasks as urgent get their
 * turn), the blue task's nap. Sets mp->yield if that may change the task
 * to run. Not while the blue task runs with its 68k interrupts masked
 * (`boost` MP_CRITICAL): NanoKernel 2's decrementer then only makes it
 * critical, and comes back a critical time slice later.
 */
void mp_tick(struct mp *mp, uint64_t now, unsigned boost);

#endif
