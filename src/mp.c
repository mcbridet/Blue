#include "mp.h"

#include <stdio.h>
#include <string.h>

void mp_init(struct mp *mp)
{
	bool log = mp->log;
	memset(mp, 0, sizeof *mp);
	mp->log = log;
	mp->blue_process = mp_make(mp, MP_PROCESS);
	mp->blue_task = mp_make(mp, MP_TASK);
	mp->page_queue = mp_make(mp, MP_QUEUE);
	mp->space = mp_make(mp, MP_ADDRESS_SPACE);
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

/* What waiting comes to when what's waited for isn't there: there's no other task to provide it. */
static int32_t would_wait(uint32_t timeout)
{
	return timeout ? MP_BLUE_BLOCKING_ERR : MP_TIMEOUT_ERR;
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

bool mp_call(struct mp *mp, struct ppc_cpu *cpu, struct mem *mem)
{
	uint32_t *r = cpu->gpr;
	struct mp_object *o;
	int32_t status = MP_NO_ERR;
	(void)mem;

	switch (r[0]) {
	case 11:        /* MPIsTaskBlue(task r3, 0: the current) -> r3 (not a status) */
		status = !r[3] || r[3] == mp->blue_task;
		break;
	case 12:        /* -> the current task r3 (not a status), and the word call 63 set for it r4 */
		status = (int32_t)mp->blue_task;
		r[4] = mp_lookup(mp, mp->blue_task, MP_TASK)->task.label;
		break;
	case 14:        /* MPSetTaskWeight(r3, weight r4 1-10000) */
		if (r[4] < 1 || r[4] > 10000 || !mp_lookup(mp, r[3], MP_TASK))
			status = MP_INVALID_ID_ERR;
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
			status = would_wait(r[7]);
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
			status = would_wait(r[4]);
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
			status = would_wait(r[4]);
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
			status = would_wait(r[5]);
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
	case 63:        /* (task r3, value r4): a word NanoKernel 2 keeps for the task */
		if ((o = mp_lookup(mp, r[3], MP_TASK)))
			o->task.label = r[4];
		else
			status = MP_INVALID_ID_ERR;
		break;
	case 64:        /* MPCreateNotification -> r4 */
		status = create(mp, MP_NOTIFICATION, &r[4]);
		break;
	case 65:        /* MPDeleteNotification(r3) */
		status = delete(mp, r[3], MP_NOTIFICATION);
		break;
	case 66: {      /* MPModifyNotification(r3, what r4: a semaphore; a queue, words r5 r6 r7; an event group, flags r5) */
		struct mp_object *what = &mp->obj[(r[4] >> 16) % MP_MAX_OBJECTS];
		if (!(o = mp_lookup(mp, r[3], MP_NOTIFICATION)) || !mp_lookup(mp, r[4], what->class)) {
			status = MP_INVALID_ID_ERR;
		} else if (what->class == MP_SEMAPHORE) {
			o->note.sem = r[4];
		} else if (what->class == MP_QUEUE) {
			o->note.queue = r[4];
			memcpy(o->note.word, &r[5], sizeof o->note.word);
		} else if (what->class == MP_EVENT_GROUP) {
			o->note.event = r[4];
			o->note.flags = r[5];
		} else {
			status = MP_INVALID_ID_ERR;
		}
		break;
	}
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
	if (mp->log)
		printf("mp: call %u (r3=0x%08X r4=0x%08X r5=0x%08X) -> %d\n", r[0], r[3], r[4], r[5], status);
	r[3] = (uint32_t)status;
	return true;
}
