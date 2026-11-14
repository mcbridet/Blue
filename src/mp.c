#include "mp.h"

#include <stdio.h>
#include <string.h>

#define BLOCKED INT32_MIN               /* (a call's status when its task blocks in it) */
#define NOMINAL_PRIORITY 2              /* (NanoKernel 2's priorities: critical, latency-protected, nominal, idle) */
#define NAME_BLUE 0x626C7565u           /* 'blue' */
#define NAME_UNNAMED 0x2D2D2D2Du        /* '----' */
#define OPTIONS_VALID 6u                /* kMPCreateTaskTakesAllExceptionsMask, kMPCreateTaskNotDebuggableMask */

void mp_init(struct mp *mp)
{
	bool log = mp->log;
	memset(mp, 0, sizeof *mp);
	mp->log = log;
	mp->blue_process = mp_make(mp, MP_PROCESS);
	mp->blue_task = mp_make(mp, MP_TASK);
	mp->page_queue = mp_make(mp, MP_QUEUE);
	mp->space = mp_make(mp, MP_ADDRESS_SPACE);

	struct mp_object *blue = mp_lookup(mp, mp->blue_task, MP_TASK);
	blue->task.process = mp->blue_process;
	blue->task.name = NAME_BLUE;
	blue->task.weight = 200;
	blue->task.priority = NOMINAL_PRIORITY;
	blue->task.state = MP_TASK_RUNNING;
	mp->task[0].id = mp->blue_task;
	mp->tasks = 1;
	mp->current = mp->blue_task;
}

uint32_t mp_add_area(struct mp *mp, uint32_t base, uint32_t length, bool writable)
{
	for (unsigned k = 0; k < mp->slots; k++) {
		const struct mp_object *o = &mp->obj[k];
		if (o->class == MP_AREA && base - o->area.base < o->area.length)
			return 0;
		if (o->class == MP_AREA && o->area.base - base < length)
			return 0;
	}
	uint32_t id = mp_make(mp, MP_AREA);
	if (id) {
		struct mp_object *o = mp_lookup(mp, id, MP_AREA);
		o->area.base = base;
		o->area.length = length;
		o->area.writable = writable;
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

uint32_t mp_make(struct mp *mp, enum mp_class class)
{
	for (unsigned k = 0; k < MP_MAX_OBJECTS; k++) {
		struct mp_object *o = &mp->obj[k];
		if (o->class != MP_FREE)
			continue;
		uint16_t uses = (uint16_t)(o->uses + 1);
		memset(o, 0, sizeof *o);
		o->class = (uint8_t)class;
		o->uses = uses ? uses : 1;
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

/* Makes an object, its ID to *id; returns the status. */
static int32_t create(struct mp *mp, enum mp_class class, uint32_t *id)
{
	*id = mp_make(mp, class);
	return *id ? MP_NO_ERR : MP_INSUFFICIENT_RESOURCES_ERR;
}

static int32_t delete(struct mp *mp, uint32_t id, enum mp_class class)
{
	struct mp_object *o = mp_lookup(mp, id, class);
	if (!o)
		return MP_INVALID_ID_ERR;
	o->class = MP_FREE;
	return MP_NO_ERR;
}

/*
 * What a wait comes to when what's waited for isn't there: with a timeout
 * of 0, none; the blue task may not block, and polls (and yields to tasks
 * that are ready); others block.
 */
static int32_t unavailable(struct mp *mp, uint32_t timeout)
{
	if (!timeout)
		return MP_TIMEOUT_ERR;
	mp->yield = true;
	if (mp->current == mp->blue_task)
		return MP_BLUE_BLOCKING_ERR;
	mp_lookup(mp, mp->current, MP_TASK)->task.state = MP_TASK_BLOCKED;
	return BLOCKED;
}

static void make_ready(struct mp *mp, uint32_t task)
{
	mp_lookup(mp, task, MP_TASK)->task.state = MP_TASK_READY;
	mp->ready[mp->readies++] = task;
}

static void unready(struct mp *mp, uint32_t task)
{
	for (unsigned k = 0; k < mp->readies; k++)
		if (mp->ready[k] == task) {
			memmove(&mp->ready[k], &mp->ready[k + 1], (--mp->readies - k) * sizeof mp->ready[0]);
			break;
		}
}

uint32_t mp_next(const struct mp *mp)
{
	return mp->readies ? mp->ready[0] : mp->blue_task;
}

struct mp_regs *mp_regs(struct mp *mp, uint32_t task)
{
	struct mp_object *o = mp_lookup(mp, task, MP_TASK);
	return o ? &mp->task[o->task.regs].regs : NULL;
}

void mp_switch(struct mp *mp, uint32_t task)
{
	struct mp_object *from = mp_lookup(mp, mp->current, MP_TASK);
	if (from && from->task.state == MP_TASK_RUNNING) {
		if (mp->current == mp->blue_task)
			from->task.state = MP_TASK_READY;
		else
			make_ready(mp, mp->current);
	}
	unready(mp, task);
	mp_lookup(mp, task, MP_TASK)->task.state = MP_TASK_RUNNING;
	mp->current = task;
	mp->yield = false;
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
	o->task.process = r[3];
	o->task.queue = r[4];
	o->task.options = r[5];
	o->task.label = r[6];
	o->task.name = NAME_UNNAMED;
	o->task.weight = 100;
	o->task.priority = NOMINAL_PRIORITY;
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

static int32_t notify_queue(struct mp *mp, uint32_t queue, const uint32_t word[3])
{
	if (!mp_lookup(mp, queue, MP_QUEUE))
		return MP_INVALID_ID_ERR;
	if (mp->messages == MP_MAX_MESSAGES)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	mp->msg[mp->messages++] = (struct mp_message){ queue, { word[0], word[1], word[2] } };
	return MP_NO_ERR;
}

static int32_t signal_semaphore(struct mp *mp, uint32_t sem)
{
	struct mp_object *o = mp_lookup(mp, sem, MP_SEMAPHORE);
	if (!o)
		return MP_INVALID_ID_ERR;
	if (o->sem.value >= o->sem.max)
		return MP_INSUFFICIENT_RESOURCES_ERR;
	o->sem.value++;
	return MP_NO_ERR;
}

static int32_t set_event(struct mp *mp, uint32_t event, uint32_t flags)
{
	struct mp_object *o = mp_lookup(mp, event, MP_EVENT_GROUP);
	if (!o)
		return MP_INVALID_ID_ERR;
	o->event.flags |= flags;
	if (o->event.swi)
		mp->swi_raised = event;
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
	o->task.message[2] = result;
	if (mp_lookup(mp, o->task.queue, MP_QUEUE))
		notify_queue(mp, o->task.queue, o->task.message);
	return MP_NO_ERR;
}

bool mp_call(struct mp *mp, struct ppc_cpu *cpu, struct mem *mem)
{
	uint32_t *r = cpu->gpr;
	struct mp_object *o;
	int32_t status = MP_NO_ERR;
	(void)mem;

	switch (r[0]) {
	case 7:         /* MPCreateTask(process r3, termination queue r4, options r5, ...: see create_task()) -> r10 */
		if (r[5] & ~OPTIONS_VALID)
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else if (!mp_lookup(mp, r[3], MP_PROCESS))
			status = MP_INVALID_ID_ERR;
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
			make_ready(mp, r[3]);
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
			status = unavailable(mp, r[7]);
		} else {
			memcpy(&r[4], mp->msg[k].word, sizeof mp->msg[k].word);
			memmove(&mp->msg[k], &mp->msg[k + 1], (--mp->messages - k) * sizeof mp->msg[0]);
		}
		break;
	}
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
			status = unavailable(mp, r[4]);
		break;
	case 25:        /* MPCreateCriticalRegion -> r4 */
		status = create(mp, MP_CRITICAL_REGION, &r[4]);
		break;
	case 26:        /* MPDeleteCriticalRegion(r3) */
		status = delete(mp, r[3], MP_CRITICAL_REGION);
		break;
	case 27:        /* MPEnterCriticalRegion(r3, timeout r4, key r5) */
		if (!(o = mp_lookup(mp, r[3], MP_CRITICAL_REGION))) {
			status = MP_INVALID_ID_ERR;
		} else if (!o->region.depth || o->region.key == r[5]) {
			o->region.depth++;
			o->region.key = r[5];
		} else {
			status = unavailable(mp, r[4]);
		}
		break;
	case 28:        /* MPExitCriticalRegion(r3, key r4) */
		if (!(o = mp_lookup(mp, r[3], MP_CRITICAL_REGION)))
			status = MP_INVALID_ID_ERR;
		else if (!o->region.depth || o->region.key != r[4])
			status = MP_INSUFFICIENT_RESOURCES_ERR;
		else
			o->region.depth--;
		break;
	case 40:        /* NKCreateTimer -> r4 */
		status = create(mp, MP_TIMER, &r[4]);
		break;
	case 30:        /* NKSetTimerNotify(timer r3, what r4: see set_notify()) */
		if (!(o = mp_lookup(mp, r[3], MP_TIMER)))
			status = MP_INVALID_ID_ERR;
		else
			status = set_notify(mp, &o->timer.notify, r);
		break;
	case 31:        /* MPArmTimer(r3, time r4:r5, options r6: 1 keeps the timer once fired): when it fires (they don't yet) */
		if (r[6] & ~1u)
			return false;   /* (2: the time is from the last one; 4: it's a Duration, in r4) */
		if (!(o = mp_lookup(mp, r[3], MP_TIMER))) {
			status = MP_INVALID_ID_ERR;
		} else {
			o->timer.time = (uint64_t)r[4] << 32 | r[5];
			o->timer.armed = true;
			o->timer.keep = r[6] & 1;
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
	case 49:        /* MPCreateEvent -> r4 */
		status = create(mp, MP_EVENT_GROUP, &r[4]);
		break;
	case 50:        /* MPDeleteEvent(r3) */
		status = delete(mp, r[3], MP_EVENT_GROUP);
		break;
	case 51:        /* MPSetEvent(r3, flags r4) */
		status = set_event(mp, r[3], r[4]);
		break;
	case 52:        /* MPWaitForEvent(r3, timeout r5) -> flags r4 */
		if (!(o = mp_lookup(mp, r[3], MP_EVENT_GROUP))) {
			status = MP_INVALID_ID_ERR;
		} else if (o->event.flags) {
			r[4] = o->event.flags;
			o->event.flags = 0;
		} else {
			status = unavailable(mp, r[5]);
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
	default:
		return false;
	}
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
	return true;
}
