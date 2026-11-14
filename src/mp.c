#include "mp.h"

#include <stdio.h>
#include <string.h>

#define BLOCKED INT32_MIN               /* (a call's status when its task blocks in it) */
#define NAME_BLUE 0x626C7565u           /* 'blue' */
#define NAME_UNNAMED 0x2D2D2D2Du        /* '----' */
#define OPTIONS_VALID 6u                /* kMPCreateTaskTakesAllExceptionsMask, kMPCreateTaskNotDebuggableMask */
#define POLLED_AVAILABLE UINT32_MAX     /* (mp->polled) */
#define DURATION_FOREVER 0x7FFFFFFFu
#define TIMER_KEEP 1u                   /* MPArmTimer's options: kMPPreserveTimerIDMask */
#define TIMER_DELTA 2u                  /* ... kMPTimeIsDeltaMask */
#define TIMER_DURATION 4u               /* ... kMPTimeIsDurationMask */
#define SWI_LEVELS 8u                   /* a software interrupt's 68k levels: 1-8 */
#define AGE_MAX 0x7FFFu                 /* (a page's age: see call 93) */
#define AGE_HELD 0x8000u
#define AREA_PAGED 0x600Cu              /* an area's FlagsAndMinAlign: paged, or contiguous (0x8000), page-aligned (12 bits) */
#define AREA_CONTIGUOUS 0xE00Cu

/* The 68k emulator's context block: software interrupts (EmulatorPublic.a) */
enum {
	ECB_SWI_TABLE = 0x0C8,          /* EDPOffsetSWIRelated: where in EmulatorData its table of event groups by level is (0: none) */
	ECB_SWI = 0x0D0,                /* SWIEventGroupID: the highest level in the table (without one, the event group) */
};

/* Timebase ticks in a Duration (NanoKernel 2's TimebaseTicksPerPeriod): milliseconds, or (negative) microseconds */
static uint64_t duration_ticks(const struct mp *mp, int32_t d)
{
	uint64_t n = d < 0 ? (uint64_t)-(int64_t)d : (uint64_t)d;
	return (d < 0 ? mp->tb_hz / 250000 : mp->tb_hz / 250) * n / 4;
}

void mp_init(struct mp *mp, uint32_t tb_hz)
{
	bool log = mp->log;
	memset(mp, 0, sizeof *mp);
	mp->log = log;
	mp->tb_hz = tb_hz;
	for (unsigned p = MP_CRITICAL; p <= MP_IDLE; p++)
		mp->cake[p] = duration_ticks(mp, -1042) << 3 * p;
	mp->blue_process = mp_make(mp, MP_PROCESS);
	mp->blue_task = mp_make(mp, MP_TASK);
	mp->page_queue = mp_make(mp, MP_QUEUE);
	mp->space = mp_make(mp, MP_ADDRESS_SPACE);
	mp_lookup(mp, mp->blue_process, MP_PROCESS)->proc.space = mp->space;

	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	blue->task.name = NAME_BLUE;
	blue->task.weight = 200;
	blue->task.priority = MP_NOMINAL;
	blue->task.state = MP_TASK_RUNNING;
	blue->task.resume = BLOCKED;
	blue->task.deadline = UINT64_MAX;
	mp->task[0].id = mp->blue_task;
	mp->tasks = 2;          /* (and NanoKernel 2's idle task, which syshm doesn't need; MPLibrary counts on it) */
	mp->current = mp->blue_task;
}

uint32_t mp_add_area(struct mp *mp, uint32_t base, uint32_t length, uint32_t access, bool paged)
{
	for (unsigned k = 0; k < mp->slots; k++) {
		const struct mp_object *o = &mp->obj[k];
		if (o->class == MP_AREA && base - o->area.base < o->area.length)
			return 0;
		if (o->class == MP_AREA && o->area.base - base < length)
			return 0;
		if (o->class == MP_AREA && o->area.paged && paged)
			return 0;
	}
	uint32_t id = mp_make(mp, MP_AREA);
	if (id) {
		struct mp_object *o = mp_lookup(mp, id, MP_AREA);
		o->area.base = base;
		o->area.length = length;
		o->area.pte_config = access;
		o->area.paged = paged;
		for (uint32_t k = 0; paged && k < MP_MAX_PAGES; k++)
			mp->age[k] = AGE_MAX;
	}
	return id;
}

/* The area of the address space (0: the blue task's) holding addr, or NULL. */
static struct mp_object *area_at(struct mp *mp, uint32_t space, uint32_t addr, uint32_t *id)
{
	if (space && !mp_lookup(mp, space, MP_ADDRESS_SPACE))
		return NULL;
	for (unsigned k = 0; k < mp->slots; k++) {
		struct mp_object *o = &mp->obj[k];
		if (o->class == MP_AREA && addr - o->area.base < o->area.length) {
			*id = (uint32_t)k << 16 | o->uses;
			return o;
		}
	}
	return NULL;
}

/*
 * Call 78's record of an area (NanoKernel 2's MPCall_78, from its Area
 * structure): 1, its process, address space, parent (an alias's); its
 * backing provider, access, FlagsAndMinAlign, base; its length, logical
 * separation, page fault count, bytes mapped; its page size, a word, the
 * backing provider's word. Words from `offset` (0x00, 0x10, 0x20, 0x30, 0x3C:
 * the end) go to r6 up, four at most, their byte count to r10.
 */
static int32_t area_record(const struct mp *mp, const struct mp_object *o, uint32_t offset, uint32_t *r)
{
	const uint32_t record[15] = {
		1, o->process, mp->space, 0,
		o->area.provider, o->area.pte_config, o->area.paged ? AREA_PAGED : AREA_CONTIGUOUS, o->area.base,
		o->area.length, 0, 0, o->area.length,
		MEM_PAGE_SIZE, 0, o->area.provider_misc,
	};
	if (offset > 0x3C || (offset % 16 && offset != 0x3C))
		return MP_PARAM_ERR;
	uint32_t words = offset == 0x3C ? 0 : offset == 0x30 ? 3 : 4;
	memcpy(&r[6], &record[offset / 4], words * sizeof record[0]);
	r[10] = words * sizeof record[0];
	return MP_NO_ERR;
}

/* The age of the page at `addr` in a paged area, or NULL (*status: why). */
static uint16_t *page_age(struct mp *mp, uint32_t area, uint32_t addr, int32_t *status)
{
	const struct mp_object *o = mp_lookup(mp, area, MP_AREA);
	uint32_t page = o ? (addr - o->area.base) >> MEM_PAGE_SHIFT : 0;
	*status = !o ? MP_INVALID_ID_ERR : MP_PARAM_ERR;
	if (!o || addr - o->area.base >= o->area.length || !o->area.paged || page >= MP_MAX_PAGES)
		return NULL;
	*status = MP_NO_ERR;
	return &mp->age[page];
}

uint32_t mp_make(struct mp *mp, enum mp_class class)
{
	for (unsigned k = 0; k < MP_MAX_OBJECTS; k++) {
		struct mp_object *o = &mp->obj[k];
		if (o->class != MP_FREE)
			continue;
		uint16_t uses = (uint16_t)(o->uses + 1);
		const struct mp_object *current = mp_lookup(mp, mp->current, MP_TASK);
		memset(o, 0, sizeof *o);
		o->class = (uint8_t)class;
		o->uses = uses ? uses : 1;
		o->process = current ? current->process : mp->blue_process;
		if (k >= mp->slots)
			mp->slots = k + 1;
		return (uint32_t)k << 16 | o->uses;
	}
	return 0;
}

struct mp_object *mp_lookup(struct mp *mp, uint32_t id, enum mp_class class)
{
	uint32_t slot = id >> 16;
	if (slot >= MP_MAX_OBJECTS)
		return NULL;
	struct mp_object *o = &mp->obj[slot];
	return o->class == class && o->uses == (id & 0xFFFF) ? o : NULL;
}

static const struct mp_object *task_at(const struct mp *mp, uint32_t id)
{
	return mp_lookup((struct mp *)mp, id, MP_TASK);
}

/* The ID of the next object after `id` (0: from the first) of a class (0: any), in their slots' order; 0 if none. */
static uint32_t next_id(const struct mp *mp, uint32_t class, uint32_t id)
{
	for (unsigned k = id ? (id >> 16) + 1 : 0; k < mp->slots; k++) {
		const struct mp_object *o = &mp->obj[k];
		if (o->class != MP_FREE && (!class || o->class == class))
			return (uint32_t)k << 16 | o->uses;
	}
	return 0;
}

/* The same for the objects of a class (not 0) that belong to a process. */
static uint32_t next_owned(const struct mp *mp, uint32_t process, uint32_t class, uint32_t id)
{
	while ((id = next_id(mp, class, id)) && mp->obj[id >> 16].process != process)
		;
	return id;
}

/* Makes an object, its ID to *id; returns the status. */
static int32_t create(struct mp *mp, enum mp_class class, uint32_t *id)
{
	*id = mp_make(mp, class);
	return *id ? MP_NO_ERR : MP_INSUFFICIENT_RESOURCES_ERR;
}

/* Puts a task in the ready queue, at the back (or the front), for mp_next() to consider. */
static void make_ready(struct mp *mp, uint32_t task, bool front)
{
	struct mp_object *o = mp_lookup(mp, task, MP_TASK);
	o->task.state = MP_TASK_READY;
	o->task.waiting = 0;
	o->task.deadline = UINT64_MAX;
	if (front) {
		memmove(&mp->ready[1], &mp->ready[0], mp->readies * sizeof mp->ready[0]);
		mp->ready[0] = task;
	} else {
		mp->ready[mp->readies] = task;
	}
	mp->readies++;
	mp->yield = true;
}

static void unready(struct mp *mp, uint32_t task)
{
	for (unsigned k = 0; k < mp->readies; k++)
		if (mp->ready[k] == task) {
			memmove(&mp->ready[k], &mp->ready[k + 1], (--mp->readies - k) * sizeof mp->ready[0]);
			break;
		}
}

/* The current task, if it's running, goes to the back of the ready queue: tasks as urgent run first. */
static void yield(struct mp *mp)
{
	struct mp_object *o = mp_lookup(mp, mp->current, MP_TASK);
	if (o && o->task.state == MP_TASK_RUNNING)
		make_ready(mp, mp->current, false);
}

/*
 * Readies a blocked task: it makes its call again, or (status other than
 * BLOCKED) the call returns `status`. A task woken by what it waited for
 * is latency-protected (`protect`).
 */
static void wake(struct mp *mp, uint32_t task, int32_t status, bool protect)
{
	struct mp_object *o = mp_lookup(mp, task, MP_TASK);
	o->task.resume = status;
	if (protect)
		o->task.priority = MP_LATENCY_PROTECTED;
	make_ready(mp, task, false);
}

/* The task that has waited on `id` longest, or 0. */
static uint32_t first_waiter(struct mp *mp, uint32_t id)
{
	uint32_t first = 0, since = 0;
	for (unsigned k = 0; k < MP_MAX_TASKS; k++) {
		const struct mp_object *o = mp->task[k].id ? mp_lookup(mp, mp->task[k].id, MP_TASK) : NULL;
		if (o && o->task.state == MP_TASK_BLOCKED && o->task.waiting == id &&
		    (!first || (int32_t)(o->task.since - since) < 0)) {
			first = mp->task[k].id;
			since = o->task.since;
		}
	}
	return first;
}

/* Wakes the task that has waited on `id` longest, if any (see wake()). */
static void wake_first(struct mp *mp, uint32_t id, int32_t status)
{
	uint32_t task = first_waiter(mp, id);
	if (task)
		wake(mp, task, status, true);
}

/*
 * `id` (a queue, semaphore, event group or critical region) has something
 * for a waiter. If the blue task polled it last, its nap ends, or (not
 * napping) its next one is skipped (NanoKernel 2's
 * UnblockBlueIfCouldBePolling).
 */
static void available(struct mp *mp, uint32_t id)
{
	if (id != mp->polled)
		return;
	mp->polled = POLLED_AVAILABLE;
	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	if (blue->task.state == MP_TASK_BLOCKED) {
		blue->task.priority = MP_LATENCY_PROTECTED;
		make_ready(mp, mp->blue_task, false);
	}
}

/* The current task blocks, waiting on `id` (0: none) until `deadline` (UINT64_MAX: none). */
static int32_t block(struct mp *mp, uint32_t id, uint64_t deadline)
{
	struct mp_object *o = mp_lookup(mp, mp->current, MP_TASK);
	o->task.state = MP_TASK_BLOCKED;
	o->task.waiting = id;
	o->task.since = mp->queued++;
	o->task.deadline = deadline;
	mp->yield = true;
	return BLOCKED;
}

/*
 * What a wait on `id` comes to when what's waited for isn't there: with a
 * timeout (a Duration) of 0, none; the blue task may not block, and polls
 * (and yields to tasks as urgent); others block until woken, or until the
 * timeout from `now` (kDurationForever: none).
 */
static int32_t unavailable(struct mp *mp, uint32_t id, uint32_t timeout, uint64_t now)
{
	if (!timeout)
		return MP_TIMEOUT_ERR;
	if (mp->current == mp->blue_task) {
		mp->polled = id;
		yield(mp);
		return MP_BLUE_BLOCKING_ERR;
	}
	return block(mp, id, timeout == DURATION_FOREVER ? UINT64_MAX : now + duration_ticks(mp, (int32_t)timeout));
}

/* Deletes an object; tasks waiting on it are woken, their calls returning kMPDeletedErr. */
static int32_t delete(struct mp *mp, uint32_t id, enum mp_class class)
{
	struct mp_object *o = mp_lookup(mp, id, class);
	if (!o)
		return MP_INVALID_ID_ERR;
	available(mp, id);
	for (uint32_t task; (task = first_waiter(mp, id));)
		wake(mp, task, MP_DELETED_ERR, false);
	o->class = MP_FREE;
	return MP_NO_ERR;
}

uint32_t mp_next(struct mp *mp, unsigned boost)
{
	struct mp_object *current = mp_lookup(mp, mp->current, MP_TASK);
	if (boost != MP_AS_IS && mp->current == mp->blue_task && current->task.state != MP_TASK_BLOCKED)
		current->task.priority = (uint8_t)boost;
	uint32_t best = 0;
	unsigned urgency = MP_IDLE + 1;
	for (unsigned k = 0; k < mp->readies; k++) {
		const struct mp_object *o = mp_lookup(mp, mp->ready[k], MP_TASK);
		if (o->task.priority < urgency) {
			best = mp->ready[k];
			urgency = o->task.priority;
		}
	}
	if (current && current->task.state == MP_TASK_RUNNING && (!best || urgency >= current->task.priority))
		return mp->current;
	return best ? best : mp->blue_task;
}

bool mp_ready(const struct mp *mp)
{
	for (unsigned k = 0; k < mp->readies; k++)
		if (mp->ready[k] != mp->blue_task)
			return true;
	return false;
}

struct mp_regs *mp_regs(struct mp *mp, uint32_t task)
{
	struct mp_object *o = mp_lookup(mp, task, MP_TASK);
	return o ? &mp->task[o->task.regs].regs : NULL;
}

/* The current task's time slice: its weight's share of its priority's cake, with the ready tasks of that priority. */
static void start_slice(struct mp *mp, uint64_t now)
{
	const struct mp_object *o = mp_lookup(mp, mp->current, MP_TASK);
	uint64_t weights = o->task.weight;
	for (unsigned k = 0; k < mp->readies; k++) {
		const struct mp_object *r = mp_lookup(mp, mp->ready[k], MP_TASK);
		if (r->task.priority == o->task.priority)
			weights += r->task.weight;
	}
	mp->slice_end = now + mp->cake[o->task.priority] * o->task.weight / weights;
	mp->next_tb = 0;
}

void mp_switch(struct mp *mp, uint32_t task, uint64_t now)
{
	struct mp_object *o = mp_lookup(mp, task, MP_TASK);
	mp->yield = false;
	if (task == mp->current && o->task.state == MP_TASK_RUNNING) {
		if (o->task.priority < MP_NOMINAL)
			start_slice(mp, now);
		return;
	}
	yield(mp);
	mp->yield = false;
	unready(mp, task);
	o->task.state = MP_TASK_RUNNING;
	mp->current = task;
	start_slice(mp, now);
}

void mp_interrupt(struct mp *mp, uint64_t now)
{
	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	blue->task.priority = MP_CRITICAL;
	if (blue->task.state == MP_TASK_RUNNING) {
		start_slice(mp, now);
		return;
	}
	unready(mp, mp->blue_task);
	make_ready(mp, mp->blue_task, true);
}

enum mp_nap mp_nap(struct mp *mp, uint64_t until)
{
	if (mp->polled == POLLED_AVAILABLE) {
		mp->polled = 0;
		return MP_NAP_SKIPPED;
	}
	if (!mp_ready(mp))
		return MP_NAP_IDLE;
	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	blue->task.state = MP_TASK_BLOCKED;
	blue->task.since = mp->queued++;
	blue->task.deadline = until;
	mp->yield = true;
	mp->next_tb = 0;
	return MP_NAP_YIELDED;
}

bool mp_napping(const struct mp *mp)
{
	return task_at(mp, mp->blue_task)->task.state == MP_TASK_BLOCKED;
}

/*
 * MPCreateTask (call 7): a task in process r3 whose termination message
 * goes to queue r4, with options r5 and the word r6 for Multiprocessing
 * Services (see call 12). Started (call 8), it runs from r8 with r9 in r2
 * and r7 in r12; its entry point returns to mp->task_return.
 */
static int32_t create_task(struct mp *mp, const uint32_t *r, uint32_t *id)
{
	unsigned k = 0;
	while (k < MP_MAX_TASKS && mp->task[k].id)
		k++;
	uint32_t made = k < MP_MAX_TASKS ? mp_make(mp, MP_TASK) : 0;
	if (!made)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	struct mp_object *o = mp_lookup(mp, made, MP_TASK);
	o->process = r[3];
	o->task.queue = r[4];
	o->task.options = r[5];
	o->task.label = r[6];
	o->task.name = NAME_UNNAMED;
	o->task.weight = 100;
	o->task.priority = MP_NOMINAL;
	o->task.resume = BLOCKED;
	o->task.deadline = UINT64_MAX;
	o->task.message[2] = (uint32_t)MP_TASK_ABORTED_ERR;
	o->task.regs = (uint8_t)k;
	mp->task[k].id = made;
	struct mp_regs *regs = &mp->task[k].regs;
	memset(regs, 0, sizeof *regs);
	regs->gpr[2] = r[9];
	regs->gpr[12] = r[7];
	regs->pc = r[8];
	regs->lr = mp->task_return;
	mp->tasks++;
	*id = made;
	return MP_NO_ERR;
}

/* Sends a message to a queue; the task that has waited on it longest is woken to take it. */
static int32_t notify_queue(struct mp *mp, uint32_t queue, const uint32_t word[3])
{
	if (!mp_lookup(mp, queue, MP_QUEUE))
		return MP_INVALID_ID_ERR;
	if (mp->messages == MP_MAX_MESSAGES)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	mp->msg[mp->messages++] = (struct mp_message){ queue, { word[0], word[1], word[2] } };
	available(mp, queue);
	wake_first(mp, queue, BLOCKED);
	return MP_NO_ERR;
}

/* Signals a semaphore: it passes straight to the task that has waited on it longest, if any. */
static int32_t signal_semaphore(struct mp *mp, uint32_t sem)
{
	struct mp_object *o = mp_lookup(mp, sem, MP_SEMAPHORE);
	if (!o)
		return MP_INVALID_ID_ERR;
	available(mp, sem);
	if (first_waiter(mp, sem))
		wake_first(mp, sem, MP_NO_ERR);
	else if (o->sem.value >= o->sem.max)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	else
		o->sem.value++;
	return MP_NO_ERR;
}

/* The emulator's table of software interrupts' event groups, by level, or NULL if it has none (or one out of its page). */
static uint8_t *swi_table(const struct mp *mp)
{
	uint32_t at = be32(mp->emulator_data + mp->ecb + ECB_SWI_TABLE);
	return at && at <= MEM_PAGE_SIZE - 4 * (SWI_LEVELS + 1) ? mp->emulator_data + at : NULL;
}

/* The highest level, 1-7, with an event group in the table; 0 if none. */
static uint32_t swi_top(const uint8_t *table)
{
	uint32_t level = 7;
	while (level && !be32(table + 4 * level))
		level--;
	return level;
}

/*
 * Event group `event`, marked to raise software interrupt `swi` (0x10 |
 * its level), has been set (NanoKernel 2's SetEvent): it goes in the
 * emulator's table at its level, unless one is waiting there already, and
 * unless a higher level is waiting, its level is the one for the emulator
 * to take (without a table, the group itself is, unless one is waiting).
 * Then the blue task is to be interrupted (mp->swi): if it's napping, it's
 * critical and runs next; else when it next runs.
 */
static void raise_swi(struct mp *mp, uint32_t event, uint32_t swi)
{
	if (!mp->emulator_data)
		return;
	uint8_t *ecb = mp->emulator_data + mp->ecb, *table = swi_table(mp);
	uint32_t level = swi & 0xF;
	if (!table) {
		if (be32(ecb + ECB_SWI))
			return;
		put_be32(ecb + ECB_SWI, event);
	} else {
		if (be32(table + 4 * level))
			return;
		put_be32(table + 4 * level, event);
		if (level < swi_top(table))
			return;
		put_be32(ecb + ECB_SWI, level);
	}
	if (mp->log)
		printf("mp: event group 0x%08X raises software interrupt %u\n", event, level);
	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	if (blue->task.state == MP_TASK_BLOCKED) {
		blue->task.priority = MP_CRITICAL;
		make_ready(mp, mp->blue_task, true);
	}
	mp->swi = true;
	mp->yield = true;
}

/*
 * The flags of a software interrupt's event group have been taken (by its
 * 68k handler, with MPWaitForEvent): the interrupt no longer waits, and
 * the emulator is to take the highest level still waiting, if any.
 */
static void swi_taken(struct mp *mp, uint32_t event, uint32_t swi)
{
	if (!mp->emulator_data)
		return;
	uint8_t *ecb = mp->emulator_data + mp->ecb, *table = swi_table(mp);
	uint32_t level = swi & 0xF;
	if (!table) {
		if (be32(ecb + ECB_SWI) == event)
			put_be32(ecb + ECB_SWI, 0);
	} else if (be32(table + 4 * level) == event) {
		put_be32(table + 4 * level, 0);
		put_be32(ecb + ECB_SWI, swi_top(table));
	}
}

static int32_t set_event(struct mp *mp, uint32_t event, uint32_t flags)
{
	struct mp_object *o = mp_lookup(mp, event, MP_EVENT_GROUP);
	if (!o)
		return MP_INVALID_ID_ERR;
	o->event.flags |= flags;
	available(mp, event);
	wake_first(mp, event, BLOCKED);
	if (o->event.swi)
		raise_swi(mp, event, o->event.swi);
	return MP_NO_ERR;
}

/* Sets what a notification or timer does to r4: signal it (a semaphore); send it words r5 r6 r7 (a queue); set flags r5 in it (an event group). */
static int32_t set_notify(struct mp *mp, struct mp_notify *n, const uint32_t *r)
{
	const struct mp_object *what = &mp->obj[(r[4] >> 16) % MP_MAX_OBJECTS];
	if (!mp_lookup(mp, r[4], what->class))
		return MP_INVALID_ID_ERR;
	switch (what->class) {
	case MP_SEMAPHORE:
		n->sem = r[4];
		return MP_NO_ERR;
	case MP_QUEUE:
		n->queue = r[4];
		memcpy(n->word, &r[5], sizeof n->word);
		return MP_NO_ERR;
	case MP_EVENT_GROUP:
		n->event = r[4];
		n->flags = r[5];
		return MP_NO_ERR;
	default:
		return MP_INVALID_ID_ERR;
	}
}

static int32_t cause_notification(struct mp *mp, uint32_t id)
{
	struct mp_object *o = mp_lookup(mp, id, MP_NOTIFICATION);
	int32_t status = MP_NO_ERR;
	if (!o)
		return MP_INVALID_ID_ERR;
	if (!o->note.queue && !o->note.sem && !o->note.event)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	if (o->note.queue)
		status = notify_queue(mp, o->note.queue, o->note.word);
	if (!status && o->note.sem && signal_semaphore(mp, o->note.sem) == MP_INVALID_ID_ERR)
		status = MP_INVALID_ID_ERR;     /* (a semaphore at its maximum just stays there) */
	if (!status && o->note.event)
		status = set_event(mp, o->note.event, o->note.flags);
	return status;
}

/*
 * The timer or blocked task to end first: the earliest, and among those
 * due at once, the first armed or blocked. NULL if none; its time to
 * *time (UINT64_MAX: never), its ID to *id.
 */
static struct mp_object *earliest(struct mp *mp, uint64_t *time, uint32_t *id)
{
	struct mp_object *first = NULL;
	uint32_t since = 0;
	*time = UINT64_MAX;
	for (unsigned k = 0; k < mp->slots; k++) {
		struct mp_object *o = &mp->obj[k];
		uint64_t t;
		uint32_t s;
		if (o->class == MP_TIMER && o->timer.armed)
			t = o->timer.time, s = o->timer.since;
		else if (o->class == MP_TASK && o->task.state == MP_TASK_BLOCKED && o->task.deadline != UINT64_MAX)
			t = o->task.deadline, s = o->task.since;
		else
			continue;
		if (first && (t > *time || (t == *time && (int32_t)(s - since) >= 0)))
			continue;
		first = o;
		*time = t;
		since = s;
		*id = (uint32_t)k << 16 | o->uses;
	}
	return first;
}

/* A timer fires: it notifies as NKSetTimerNotify (call 30) set, and is deleted unless armed to be kept. */
static void fire(struct mp *mp, struct mp_object *timer, uint32_t id)
{
	const struct mp_notify *n = &timer->timer.notify;
	if (mp->log)
		printf("mp: timer 0x%08X fires\n", id);
	timer->timer.armed = false;
	if (mp_lookup(mp, n->queue, MP_QUEUE))
		notify_queue(mp, n->queue, n->word);
	if (mp_lookup(mp, n->sem, MP_SEMAPHORE))
		signal_semaphore(mp, n->sem);
	if (mp_lookup(mp, n->event, MP_EVENT_GROUP))
		set_event(mp, n->event, n->flags);
	if (!timer->timer.keep)
		timer->class = MP_FREE;
}

void mp_tick(struct mp *mp, uint64_t now, unsigned boost)
{
	struct mp_object *current = mp_lookup(mp, mp->current, MP_TASK);
	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	if (boost == MP_CRITICAL && current == blue && blue->task.state == MP_TASK_RUNNING) {
		mp->next_tb = now + mp->cake[MP_CRITICAL];
		return;
	}
	if (current->task.state == MP_TASK_RUNNING && now >= mp->slice_end) {
		if (current->task.priority < MP_NOMINAL)
			current->task.priority = MP_NOMINAL;
		yield(mp);
	}
	uint64_t time;
	uint32_t id;
	struct mp_object *o;
	while ((o = earliest(mp, &time, &id)) && time <= now) {
		if (o->class == MP_TIMER)
			fire(mp, o, id);
		else if (o->task.waiting)
			wake(mp, id, MP_TIMEOUT_ERR, false);
		else            /* (a delay, or the blue task's nap: it made no call to make again) */
			wake(mp, id, id == mp->blue_task ? BLOCKED : MP_NO_ERR, true);
	}
	mp->next_tb = current->task.state == MP_TASK_RUNNING ? mp->slice_end : UINT64_MAX;   /* (else mp_switch() is next) */
	if (time < mp->next_tb)
		mp->next_tb = time;
}

uint64_t mp_next_due(const struct mp *mp)
{
	uint64_t time;
	uint32_t id;
	earliest((struct mp *)mp, &time, &id);
	return time;
}

bool mp_fires(const struct mp *mp, uint64_t now)
{
	return (task_at(mp, mp->current)->task.state == MP_TASK_RUNNING && now >= mp->slice_end) || mp_next_due(mp) <= now;
}

int32_t mp_terminate(struct mp *mp, uint32_t task, uint32_t result)
{
	struct mp_object *o = mp_lookup(mp, task, MP_TASK);
	if (!o)
		return MP_INVALID_ID_ERR;
	if (o->task.state == MP_TASK_TERMINATED || task == mp->blue_task)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	unready(mp, task);
	if (task == mp->current)
		mp->yield = true;
	o->task.state = MP_TASK_TERMINATED;
	o->task.waiting = 0;
	o->task.deadline = UINT64_MAX;
	o->task.message[2] = result;
	if (mp_lookup(mp, o->task.queue, MP_QUEUE))
		notify_queue(mp, o->task.queue, o->task.message);
	return MP_NO_ERR;
}

/* Does call r0 for the current task, its status to *result (BLOCKED: the task blocks); false if it isn't implemented. */
static bool call(struct mp *mp, struct ppc_cpu *cpu, int32_t *result)
{
	uint32_t *r = cpu->gpr;
	struct mp_object *o;
	int32_t status = MP_NO_ERR;

	switch (r[0]) {
	case 3:         /* MPProcessors(r3: 0 all, else those scheduled) -> r3 (not a status): there's one */
		status = 1;
		break;
	case 4:         /* MPCreateProcess(address space r3, 0: the system's) -> r5 */
		if (r[3] && !mp_lookup(mp, r[3], MP_ADDRESS_SPACE)) {
			status = MP_INVALID_ID_ERR;
		} else if (!(status = create(mp, MP_PROCESS, &r[5]))) {
			mp_lookup(mp, r[5], MP_PROCESS)->proc.space = r[3] ? r[3] : mp->space;
		}
		break;
	case 5:         /* MPTerminateProcess(r3): marks it, so it gets no more tasks (its tasks carry on) */
		if (!(o = mp_lookup(mp, r[3], MP_PROCESS)))
			status = MP_INVALID_ID_ERR;
		else if (o->proc.terminated)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else
			o->proc.terminated = true;
		break;
	case 6:         /* MPDeleteProcess(r3): once terminated, with no tasks left */
		if (!(o = mp_lookup(mp, r[3], MP_PROCESS)))
			status = MP_INVALID_ID_ERR;
		else if (!o->proc.terminated || next_owned(mp, r[3], MP_TASK, 0))
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else
			status = delete(mp, r[3], MP_PROCESS);
		break;
	case 99:        /* MPSetBlueProcessID(process r3, word r4): the blue task's process and its call-63 word */
		if (mp->current != mp->blue_task)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else if (!mp_lookup(mp, r[3], MP_PROCESS))
			status = MP_INVALID_ID_ERR;
		else {
			o = mp_lookup(mp, mp->current, MP_TASK);
			o->process = r[3];
			o->task.label = r[4];
		}
		break;
	case 7:         /* MPCreateTask(process r3, termination queue r4, options r5, ...: see create_task()) -> r10 */
		if (r[5] & ~OPTIONS_VALID)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else if (!(o = mp_lookup(mp, r[3], MP_PROCESS)))
			status = MP_INVALID_ID_ERR;
		else if (o->proc.terminated)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else
			status = create_task(mp, r, &r[10]);
		break;
	case 8:         /* (task r3, name r4, its r3 r5, termination message words r6 r7, its r1 r8; r9: its stack's end) starts it */
		if (!(o = mp_lookup(mp, r[3], MP_TASK))) {
			status = MP_INVALID_ID_ERR;
		} else if (o->task.state != MP_TASK_NEW) {
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		} else {
			struct mp_regs *regs = &mp->task[o->task.regs].regs;
			o->task.name = r[4];
			o->task.message[0] = r[6];
			o->task.message[1] = r[7];
			regs->gpr[3] = r[5];
			regs->gpr[1] = r[8];
			make_ready(mp, r[3], false);
		}
		break;
	case 9:         /* MPTerminateTask(r3, result r4) */
		status = mp_terminate(mp, r[3], r[4]);
		break;
	case 10:        /* (r3): deletes a terminated task */
		if (!(o = mp_lookup(mp, r[3], MP_TASK))) {
			status = MP_INVALID_ID_ERR;
		} else if (o->task.state != MP_TASK_TERMINATED) {
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		} else {
			mp->task[o->task.regs].id = 0;
			o->class = MP_FREE;
			mp->tasks--;
		}
		break;
	case 11:        /* MPIsTaskBlue(task r3, 0: the current) -> r3 (not a status) */
		status = (r[3] ? r[3] : mp->current) == mp->blue_task;
		break;
	case 12:        /* -> the current task r3 (not a status), and the word call 63 set for it r4 */
		status = (int32_t)mp->current;
		r[4] = mp_lookup(mp, mp->current, MP_TASK)->task.label;
		break;
	case 13:        /* MPYieldWithHint(hint r3) -> r3 as it was: nominal, it yields; the blue task with a hint only if it was more urgent, to the front */
		o = mp_lookup(mp, mp->current, MP_TASK);
		status = (int32_t)r[3];
		if (mp->current != mp->blue_task || !(r[3] & 1)) {
			o->task.priority = MP_NOMINAL;
			yield(mp);
		} else if (o->task.priority < MP_NOMINAL) {
			o->task.priority = MP_NOMINAL;
			make_ready(mp, mp->current, true);
		}
		break;
	case 14:        /* MPSetTaskWeight(r3, weight r4 1-10000) */
		if (r[4] < 1 || r[4] > 10000 || !(o = mp_lookup(mp, r[3], MP_TASK)))
			status = MP_INVALID_ID_ERR;
		else
			o->task.weight = r[4];
		break;
	case 15:        /* MPCreateQueue -> r4 */
		status = create(mp, MP_QUEUE, &r[4]);
		break;
	case 16:        /* MPDeleteQueue(r3) */
		status = delete(mp, r[3], MP_QUEUE);
		for (unsigned k = 0; !status && k < mp->messages;)
			if (mp->msg[k].queue == r[3])
				memmove(&mp->msg[k], &mp->msg[k + 1], (--mp->messages - k) * sizeof mp->msg[0]);
			else
				k++;
		break;
	case 17:        /* MPNotifyQueue(r3, words r4 r5 r6) */
		status = notify_queue(mp, r[3], &r[4]);
		break;
	case 18: {      /* MPWaitOnQueue(r3, timeout r7) -> words r4 r5 r6 */
		unsigned k = 0;
		while (k < mp->messages && mp->msg[k].queue != r[3])
			k++;
		if (!mp_lookup(mp, r[3], MP_QUEUE)) {
			status = MP_INVALID_ID_ERR;
		} else if (k == mp->messages) {
			status = unavailable(mp, r[3], r[7], cpu->tb);
		} else {
			memcpy(&r[4], mp->msg[k].word, sizeof mp->msg[k].word);
			memmove(&mp->msg[k], &mp->msg[k + 1], (--mp->messages - k) * sizeof mp->msg[0]);
		}
		break;
	}
	case 37:        /* MPGetNextID(class r3, 0: any; ID r4, 0: from the first) -> the next r4 */
		r[4] = next_id(mp, r[3], r[4]);
		if (!r[4])
			status = MP_INVALID_ID_ERR;
		break;
	case 39:        /* MPSetQueueReserve(r3, count r4): notifications that can't fail for want of memory */
		if (!(o = mp_lookup(mp, r[3], MP_QUEUE)))
			status = MP_INVALID_ID_ERR;
		else if ((int32_t)r[4] < 0)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else
			o->queue.reserve = r[4];
		break;
	case 20:        /* MPCreateSemaphore(maximum r3, initial r4) -> r5 */
		if (r[4] > r[3]) {
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		} else if (!(status = create(mp, MP_SEMAPHORE, &r[5]))) {
			o = mp_lookup(mp, r[5], MP_SEMAPHORE);
			o->sem.max = r[3];
			o->sem.value = r[4];
		}
		break;
	case 21:        /* MPDeleteSemaphore(r3) */
		status = delete(mp, r[3], MP_SEMAPHORE);
		break;
	case 22:        /* MPSignalSemaphore(r3) */
		status = signal_semaphore(mp, r[3]);
		break;
	case 23:        /* MPWaitOnSemaphore(r3, timeout r4) */
		if (!(o = mp_lookup(mp, r[3], MP_SEMAPHORE)))
			status = MP_INVALID_ID_ERR;
		else if (o->sem.value)
			o->sem.value--;
		else
			status = unavailable(mp, r[3], r[4], cpu->tb);
		break;
	case 25:        /* MPCreateCriticalRegion -> r4 */
		status = create(mp, MP_CRITICAL_REGION, &r[4]);
		break;
	case 26:        /* MPDeleteCriticalRegion(r3) */
		status = delete(mp, r[3], MP_CRITICAL_REGION);
		break;
	case 27:        /* MPEnterCriticalRegion(r3, timeout r4, key r5: the task's process) */
		if (!(o = mp_lookup(mp, r[3], MP_CRITICAL_REGION))) {
			status = MP_INVALID_ID_ERR;
		} else if (!o->region.depth || (o->region.owner == mp->current && o->region.key == r[5])) {
			o->region.depth++;
			o->region.owner = mp->current;
			o->region.key = r[5];
		} else if (!mp_lookup(mp, o->region.owner, MP_TASK) || !mp_lookup(mp, o->region.key, MP_PROCESS)) {
			status = MP_TASK_ABORTED_ERR;
		} else {
			status = unavailable(mp, r[3], r[4], cpu->tb);
		}
		break;
	case 28:        /* MPExitCriticalRegion(r3, key r4): the first waiting enters it once it's free */
		if (!(o = mp_lookup(mp, r[3], MP_CRITICAL_REGION))) {
			status = MP_INVALID_ID_ERR;
		} else if (!o->region.depth || o->region.owner != mp->current || o->region.key != r[4]) {
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		} else if (!--o->region.depth) {
			o->region.owner = o->region.key = 0;
			available(mp, r[3]);
			wake_first(mp, r[3], BLOCKED);
		}
		break;
	case 40:        /* NKCreateTimer -> r4: its time (for MPArmTimer's deltas) is now */
		if (!(status = create(mp, MP_TIMER, &r[4])))
			mp_lookup(mp, r[4], MP_TIMER)->timer.time = cpu->tb;
		break;
	case 41:        /* NKDeleteTimer(r3) */
		status = delete(mp, r[3], MP_TIMER);
		break;
	case 30:        /* NKSetTimerNotify(timer r3, what r4: see set_notify()) */
		if (!(o = mp_lookup(mp, r[3], MP_TIMER)))
			status = MP_INVALID_ID_ERR;
		else
			status = set_notify(mp, &o->timer.notify, r);
		break;
	case 31:        /* MPArmTimer(r3, time r4:r5, options r6): it fires then (see TIMER_KEEP) */
		if (!(o = mp_lookup(mp, r[3], MP_TIMER))) {
			status = MP_INVALID_ID_ERR;
		} else {
			uint64_t time = r[6] & TIMER_DURATION ? duration_ticks(mp, (int32_t)r[4]) : (uint64_t)r[4] << 32 | r[5];
			o->timer.time = r[6] & TIMER_DELTA ? o->timer.time + time : time;
			o->timer.since = mp->queued++;
			o->timer.armed = true;
			o->timer.keep = r[6] & TIMER_KEEP;
		}
		break;
	case 32:        /* MPCancelTimer(r3) -> the time it had left r4:r5 (0 if it wasn't armed) */
		if (!(o = mp_lookup(mp, r[3], MP_TIMER))) {
			status = MP_INVALID_ID_ERR;
		} else {
			uint64_t left = o->timer.armed && o->timer.time > cpu->tb ? o->timer.time - cpu->tb : 0;
			o->timer.armed = false;
			r[4] = (uint32_t)(left >> 32);
			r[5] = (uint32_t)left;
		}
		break;
	case 33:        /* MPDelayUntil(time r3:r4, its top bit ignored): the task blocks until then (the blue task may not) */
	case 55:        /* (MPDelayUntilSys: the same for tasks; the blue task's is a nap, see nk.c) */
		if (mp->current == mp->blue_task)
			status = MP_BLUE_BLOCKING_ERR;
		else
			status = block(mp, 0, (uint64_t)(r[3] & 0x7FFFFFFFu) << 32 | r[4]);
		break;
	case 49:        /* MPCreateEvent -> r4 */
		status = create(mp, MP_EVENT_GROUP, &r[4]);
		break;
	case 50:        /* MPDeleteEvent(r3) */
		status = delete(mp, r[3], MP_EVENT_GROUP);
		break;
	case 51:        /* MPSetEvent(r3, flags r4) */
		status = set_event(mp, r[3], r[4]);
		break;
	case 52:        /* MPWaitForEvent(r3, timeout r5) -> flags r4; a software interrupt's handler takes it so */
		if (!(o = mp_lookup(mp, r[3], MP_EVENT_GROUP))) {
			status = MP_INVALID_ID_ERR;
		} else if (o->event.flags) {
			r[4] = o->event.flags;
			o->event.flags = 0;
			if (o->event.swi)
				swi_taken(mp, r[3], o->event.swi);
		} else {
			status = unavailable(mp, r[3], r[5], cpu->tb);
		}
		break;
	case 54:        /* MPSetSWIEvent(r3, level r4 1-8, else 1): setting the group raises that software interrupt */
		if (!(o = mp_lookup(mp, r[3], MP_EVENT_GROUP)))
			status = MP_INVALID_ID_ERR;
		else
			o->event.swi = 0x10 | (r[4] >= 1 && r[4] <= 8 ? r[4] : 1);
		break;
	case 56:        /* MPSetExceptionHandler(task r3, queue r4 or 0): where its exceptions are reported */
		if (!(o = mp_lookup(mp, r[3], MP_TASK)) || (r[4] && !mp_lookup(mp, r[4], MP_QUEUE)))
			status = MP_INVALID_ID_ERR;
		else
			o->task.exceptions = r[4];
		break;
	case 59:        /* (queue r3 or 0): the debugger's, for tasks' exceptions (NanoKernel 2's ThrowTaskToDebugger) */
		if (r[3] && !mp_lookup(mp, r[3], MP_QUEUE))
			status = MP_INVALID_ID_ERR;
		else
			mp->debugger = r[3];
		break;
	case 63:        /* (task r3, value r4): a word NanoKernel 2 keeps for the task */
		if ((o = mp_lookup(mp, r[3], MP_TASK)))
			o->task.label = r[4];
		else
			status = MP_INVALID_ID_ERR;
		break;
	case 126:       /* KCSetTaskType(r3, name r4) */
		if ((o = mp_lookup(mp, r[3], MP_TASK)))
			o->task.name = r[4];
		else
			status = MP_INVALID_ID_ERR;
		break;
	case 64:        /* MPCreateNotification -> r4 */
		status = create(mp, MP_NOTIFICATION, &r[4]);
		break;
	case 65:        /* MPDeleteNotification(r3) */
		status = delete(mp, r[3], MP_NOTIFICATION);
		break;
	case 66:        /* MPModifyNotification(r3, what r4: see set_notify()) */
		if (!(o = mp_lookup(mp, r[3], MP_NOTIFICATION)))
			status = MP_INVALID_ID_ERR;
		else
			status = set_notify(mp, &o->note, r);
		break;
	case 67:        /* MPCauseNotification(r3) */
		status = cause_notification(mp, r[3]);
		break;
	case 128:       /* (notification r3, kind r4: 4, words r5 r6 r7 for its queue; 9, flags r5 for its event group) */
		if (!(o = mp_lookup(mp, r[3], MP_NOTIFICATION)))
			status = MP_INVALID_ID_ERR;
		else if (r[4] == MP_QUEUE)
			memcpy(o->note.word, &r[5], sizeof o->note.word);
		else if (r[4] == MP_EVENT_GROUP)
			o->note.flags = r[5];
		else
			status = MP_PARAM_ERR;
		break;
	case 77:        /* MPSetAreaBackingProvider(area r3, notification r4, r5) */
		if (!(o = mp_lookup(mp, r[3], MP_AREA)) || (r[4] && !mp_lookup(mp, r[4], MP_NOTIFICATION))) {
			status = MP_INVALID_ID_ERR;
		} else {
			o->area.provider = r[4];
			o->area.provider_misc = r[5];
		}
		break;
	case 80:        /* MPGetAreaFromAddress(space r3, 0: the current; address r4) -> r5 */
		if (r[3] && !mp_lookup(mp, r[3], MP_ADDRESS_SPACE))
			status = MP_INVALID_ID_ERR;
		else if (!area_at(mp, r[3], r[4], &r[5]))
			status = MP_PARAM_ERR, r[5] = 0;
		break;
	case 78:        /* (area r3, 1 r4, offset r5) -> words of its record r6 up, their byte count r10: see area_record() */
		if (!(o = mp_lookup(mp, r[3], MP_AREA)))
			status = MP_INVALID_ID_ERR;
		else if (r[4] != 1)
			status = MP_PARAM_ERR;
		else
			status = area_record(mp, o, r[5], r);
		break;
	case 93: {      /* MPGetPageAge(area r3, address r4) -> r5 (0 while it's held) */
		const uint16_t *age = page_age(mp, r[3], r[4], &status);
		if (age)
			r[5] = *age & AGE_HELD ? 0 : *age;
		break;
	}
	case 94: {      /* MPSetPageAge(area r3, address r4, age r5) (NanoKernel 2 then takes it out of the hash table, for its next use to halve its age; syshm has none) */
		uint16_t *age = page_age(mp, r[3], r[4], &status);
		if (age && r[5] > AGE_MAX)
			status = MP_PARAM_ERR;
		else if (age && *age & AGE_HELD)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else if (age)
			*age = (uint16_t)r[5];
		break;
	}
	case 100:       /* MPGetFreePageCount -> r3 (not a status) */
		status = (int32_t)mp->free_pages;
		break;
	case 82:        /* MPRegisterAger(notification r3): once */
		if (mp->ager)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else if (!mp_lookup(mp, r[3], MP_NOTIFICATION))
			status = MP_INVALID_ID_ERR;
		else
			mp->ager = r[3];
		break;
	case 116: {     /* MPGetNextIDOwnedByProcess(process r3, class r4, ID r5, 0: from the first) -> the next r5 of the class that belongs to it */
		uint32_t class = r[4];
		bool owned = class != MP_FREE && class != MP_PROCESS && class != MP_CPU && class != MP_COHERENCE_GROUP &&
			     class <= MP_CONSOLE_LOG;
		if (!mp_lookup(mp, r[3], MP_PROCESS))
			status = MP_INVALID_ID_ERR;
		else if (!owned)
			status = (r[5] = next_id(mp, class, r[5])) ? MP_PARAM_ERR : MP_INVALID_ID_ERR;
		else if (!(r[5] = next_owned(mp, r[3], class, r[5])))
			status = MP_INVALID_ID_ERR;
		break;
	}
	default:
		return false;
	}
	*result = status;
	return true;
}

bool mp_call(struct mp *mp, struct ppc_cpu *cpu, struct mem *mem)
{
	uint32_t *r = cpu->gpr;
	struct mp_object *self = mp_lookup(mp, mp->current, MP_TASK);
	int32_t status = self->task.resume;
	(void)mem;

	if (status != BLOCKED)          /* (woken with a status: the call made again returns it) */
		self->task.resume = BLOCKED;
	else if (!call(mp, cpu, &status))
		return false;
	if (mp->log) {
		printf("mp: call %u (r3=0x%08X r4=0x%08X r5=0x%08X) -> ", r[0], r[3], r[4], r[5]);
		if (status == BLOCKED)
			printf("blocks");
		else
			printf("%d", status);
		if (r[0] == 7 && status == MP_NO_ERR)
			printf(", task 0x%08X", r[10]);
		if (mp->current != mp->blue_task)
			printf(" (task 0x%08X)", mp->current);
		printf("\n");
	}
	if (status != BLOCKED)
		r[3] = (uint32_t)status;
	mp->next_tb = 0;
	return true;
}
