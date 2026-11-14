#include <string.h>

#include "../src/mp.h"
#include "test.h"

#define TB_HZ 16600000u

static struct mp mp;
static struct ppc_cpu cpu;

/* Makes call n with r3 up from `args`; returns r3. */
static int32_t call(unsigned n, unsigned count, const uint32_t *args)
{
	cpu.gpr[0] = n;
	memcpy(&cpu.gpr[3], args, count * sizeof args[0]);
	CHECK(mp_call(&mp, &cpu, NULL));
	return (int32_t)cpu.gpr[3];
}

/* Puts mp_next()'s choice on the processor at timebase `now`; returns it. */
static uint32_t schedule(uint64_t now)
{
	uint32_t next = mp_next(&mp, MP_AS_IS);
	mp_switch(&mp, next, now);
	return next;
}

static uint8_t state(uint32_t task)
{
	return mp_lookup(&mp, task, MP_TASK)->task.state;
}

static uint8_t priority(uint32_t task)
{
	return mp_lookup(&mp, task, MP_TASK)->task.priority;
}

/* Makes and starts a task (termination queue `queue`); returns its ID. */
static uint32_t start_task(uint32_t queue)
{
	uint32_t create[] = { mp.blue_process, queue, 0, 0x64218, 0x35520, 0xFFCE03A0u, 0x35208 };
	CHECK_EQ(call(7, 7, create), MP_NO_ERR);
	uint32_t task = cpu.gpr[10];
	uint32_t start[] = { task, 0x74657374, 0x64218, 0x11, 0x22, 0x64218, 0x63E18 };
	CHECK_EQ(call(8, 7, start), MP_NO_ERR);
	return task;
}

/* Creating, starting, ending and deleting tasks */
static void test_tasks(void)
{
	mp_init(&mp, TB_HZ);
	mp.task_return = 0x68FFE000u;
	uint32_t blue = mp.blue_task;
	CHECK_EQ(mp.current, blue);
	CHECK_EQ(mp.tasks, 2);          /* (the idle task too) */
	CHECK_EQ(mp_next(&mp, MP_AS_IS), blue);

	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);         /* the termination queue */
	uint32_t queue = cpu.gpr[4];

	/* creating: (process, termination queue, options, label, r12, entry, r2) -> r10 */
	uint32_t bad[] = { mp.blue_process, queue, 1, 0, 0, 0, 0 };
	CHECK_EQ(call(7, 7, bad), MP_INSUFFICIENT_RESOURCES_ERR);
	bad[0] = queue;
	bad[2] = 0;
	CHECK_EQ(call(7, 7, bad), MP_INVALID_ID_ERR);
	uint32_t create[] = { mp.blue_process, queue, 0, 0x64218, 0x35520, 0xFFCE03A0u, 0x35208 };
	CHECK_EQ(call(7, 7, create), MP_NO_ERR);
	uint32_t task = cpu.gpr[10];
	CHECK_EQ(mp.tasks, 3);
	struct mp_regs *regs = mp_regs(&mp, task);
	CHECK(regs != NULL);
	CHECK_EQ(regs->pc, 0xFFCE03A0u);
	CHECK_EQ(regs->gpr[2], 0x35208);
	CHECK_EQ(regs->gpr[12], 0x35520);
	CHECK_EQ(regs->lr, mp.task_return);
	CHECK(!mp_ready(&mp));

	/* starting: (task, name, its r3, termination message words, its r1, its stack's end); it doesn't preempt the blue task */
	uint32_t start[] = { task, 0x64657468, 0x64218, 0x11, 0x22, 0x64218, 0x63E18 };
	CHECK_EQ(call(8, 7, start), MP_NO_ERR);
	CHECK_EQ(regs->gpr[3], 0x64218);
	CHECK_EQ(regs->gpr[1], 0x64218);
	CHECK(mp_ready(&mp));
	CHECK_EQ(priority(task), MP_NOMINAL);
	CHECK_EQ(mp_next(&mp, MP_AS_IS), blue);
	CHECK_EQ(call(8, 7, start), MP_INSUFFICIENT_RESOURCES_ERR);

	/* the blue task polls: it doesn't block, and yields */
	uint32_t wait[] = { queue, 0, 0, 0, 0x7FFFFFFF };
	CHECK_EQ(call(18, 5, wait), MP_BLUE_BLOCKING_ERR);
	CHECK(mp.yield);
	CHECK_EQ(schedule(0), task);
	CHECK(!mp.yield);
	CHECK_EQ(mp.current, task);
	CHECK(!mp_ready(&mp));
	CHECK_EQ(state(blue), MP_TASK_READY);

	uint32_t none[] = { 0 };
	CHECK_EQ(call(11, 1, none), 0);
	CHECK_EQ(call(12, 0, NULL), task);
	CHECK_EQ(cpu.gpr[4], 0x64218);

	/* the task blocks: r3 stays, and the call is made again later */
	cpu.gpr[0] = 18;
	cpu.gpr[3] = queue;
	cpu.gpr[7] = 0x7FFFFFFF;
	CHECK(mp_call(&mp, &cpu, NULL));
	CHECK_EQ(cpu.gpr[3], queue);
	CHECK(mp.yield);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);

	/* terminating it sends its message, with the result, to the queue */
	uint32_t term[] = { task, 0x1234 };
	CHECK_EQ(call(10, 1, term), MP_INSUFFICIENT_RESOURCES_ERR);    /* (not terminated yet) */
	CHECK_EQ(call(9, 2, term), MP_NO_ERR);
	CHECK_EQ(call(9, 2, term), MP_INSUFFICIENT_RESOURCES_ERR);
	CHECK(!mp_ready(&mp));
	CHECK_EQ(call(18, 5, wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 0x11);
	CHECK_EQ(cpu.gpr[5], 0x22);
	CHECK_EQ(cpu.gpr[6], 0x1234);
	CHECK_EQ(call(10, 1, term), MP_NO_ERR);
	CHECK_EQ(mp.tasks, 2);
	CHECK(mp_regs(&mp, task) == NULL);

	/* a task that ends itself gives up the processor */
	task = start_task(queue);
	CHECK_EQ(call(13, 1, none), 0);         /* (MPYield: the blue task to the back) */
	CHECK_EQ(schedule(0), task);
	term[0] = task;
	CHECK_EQ(call(9, 2, term), MP_NO_ERR);
	CHECK(mp.yield);
	CHECK_EQ(state(task), MP_TASK_TERMINATED);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(18, 5, wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[6], 0x1234);

	/* one that never started can end too; the blue task can't */
	CHECK_EQ(call(7, 7, create), MP_NO_ERR);
	CHECK_EQ(mp_terminate(&mp, cpu.gpr[10], 0x55), MP_NO_ERR);
	CHECK_EQ(call(18, 5, wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 0);
	CHECK_EQ(cpu.gpr[6], 0x55);
	CHECK_EQ(mp_terminate(&mp, blue, 0), MP_INSUFFICIENT_RESOURCES_ERR);
	CHECK(!mp.yield);
}

/* Waits block tasks; what they wait for wakes them, latency-protected, ahead of the blue task */
static void test_waking(void)
{
	mp_init(&mp, TB_HZ);
	uint32_t blue = mp.blue_task;
	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);
	uint32_t queue = cpu.gpr[4];
	uint32_t task = start_task(queue);
	uint32_t yield[] = { 0 };
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), task);

	/* a queue: the message wakes the task, which takes it when it makes its call again */
	uint32_t wait[] = { queue, 0, 0, 0, 0x7FFFFFFF };
	CHECK_EQ(call(18, 5, wait), (int32_t)queue);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	CHECK_EQ(schedule(0), blue);
	uint32_t notify[] = { queue, 1, 2, 3 };
	CHECK_EQ(call(17, 4, notify), MP_NO_ERR);
	CHECK(mp.yield);
	CHECK_EQ(state(task), MP_TASK_READY);
	CHECK_EQ(priority(task), MP_LATENCY_PROTECTED);
	CHECK_EQ(schedule(0), task);    /* (it preempts the blue task, nominal) */
	CHECK_EQ(state(blue), MP_TASK_READY);
	CHECK_EQ(call(18, 5, wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 1);
	CHECK_EQ(cpu.gpr[6], 3);

	/* a semaphore: signalled, it passes straight to the waiting task */
	uint32_t sem_args[] = { 1, 0 };
	CHECK_EQ(call(20, 2, sem_args), MP_NO_ERR);
	uint32_t sem = cpu.gpr[5];
	uint32_t sem_wait[] = { sem, 0x7FFFFFFF };
	CHECK_EQ(call(23, 2, sem_wait), (int32_t)sem);
	CHECK_EQ(schedule(0), blue);
	uint32_t signal[] = { sem };
	CHECK_EQ(call(22, 1, signal), MP_NO_ERR);
	CHECK_EQ(mp_lookup(&mp, sem, MP_SEMAPHORE)->sem.value, 0);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(23, 2, sem_wait), MP_NO_ERR);
	CHECK_EQ(mp_lookup(&mp, sem, MP_SEMAPHORE)->sem.value, 0);
	CHECK_EQ(call(23, 2, sem_wait), (int32_t)sem);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(21, 1, signal), MP_NO_ERR);       /* (deleting it wakes the task: its call returns kMPDeletedErr) */
	CHECK_EQ(priority(task), MP_LATENCY_PROTECTED);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(23, 2, sem_wait), MP_DELETED_ERR);

	/* an event group: the task gets the flags set */
	CHECK_EQ(call(49, 0, NULL), MP_NO_ERR);
	uint32_t event = cpu.gpr[4];
	uint32_t event_wait[] = { event, 0, 0x7FFFFFFF };
	CHECK_EQ(call(52, 3, event_wait), (int32_t)event);
	CHECK_EQ(schedule(0), blue);
	uint32_t set[] = { event, 5 };
	CHECK_EQ(call(51, 2, set), MP_NO_ERR);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(52, 3, event_wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 5);
	CHECK_EQ(mp_lookup(&mp, event, MP_EVENT_GROUP)->event.flags, 0);

	/* a critical region: the task gets in once the blue task is out; the blue task polls */
	CHECK_EQ(call(25, 0, NULL), MP_NO_ERR);
	uint32_t region = cpu.gpr[4];
	uint32_t enter[] = { region, 0x7FFFFFFF, mp.blue_process }, leave[] = { region, mp.blue_process };
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(27, 3, enter), MP_NO_ERR);
	CHECK_EQ(call(27, 3, enter), MP_NO_ERR);        /* (again: it's the blue task's) */
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(28, 2, leave), MP_INSUFFICIENT_RESOURCES_ERR);   /* (not the task's) */
	enter[1] = 0;
	CHECK_EQ(call(27, 3, enter), MP_TIMEOUT_ERR);
	enter[1] = 0x7FFFFFFF;
	CHECK_EQ(call(27, 3, enter), (int32_t)region);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(28, 2, leave), MP_NO_ERR);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	CHECK_EQ(call(28, 2, leave), MP_NO_ERR);
	CHECK_EQ(state(task), MP_TASK_READY);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(27, 3, enter), MP_NO_ERR);
	CHECK_EQ(mp_lookup(&mp, region, MP_CRITICAL_REGION)->region.owner, task);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(27, 3, enter), MP_BLUE_BLOCKING_ERR);
	CHECK_EQ(mp.polled, region);

	/* the first to block is the first woken */
	uint32_t second = start_task(queue);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(18, 5, wait), (int32_t)queue);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), second);
	CHECK_EQ(call(18, 5, wait), (int32_t)queue);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(17, 4, notify), MP_NO_ERR);
	CHECK_EQ(state(task), MP_TASK_READY);
	CHECK_EQ(state(second), MP_TASK_BLOCKED);
	uint32_t del[] = { queue };
	CHECK_EQ(call(16, 1, del), MP_NO_ERR);
	CHECK_EQ(state(second), MP_TASK_READY);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(18, 5, wait), MP_INVALID_ID_ERR);         /* (the queue is gone, message and all) */
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), second);
	CHECK_EQ(call(18, 5, wait), MP_DELETED_ERR);
}

/* Time slices, interrupts and the 68k's interrupt mask */
static void test_slices(void)
{
	mp_init(&mp, TB_HZ);
	uint32_t blue = mp.blue_task;
	CHECK_EQ(mp.cake[MP_CRITICAL], 17193);          /* (NanoKernel 2's, about 1 ms) */
	CHECK_EQ(mp.cake[MP_NOMINAL], 17193 * 64);
	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);
	uint32_t queue = cpu.gpr[4];

	/* the blue task alone: a new time slice when one ends */
	CHECK(mp_due(&mp, 0));
	mp_tick(&mp, 0, MP_AS_IS);
	CHECK(mp.yield);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(mp.slice_end, mp.cake[MP_NOMINAL]);
	mp_tick(&mp, 0, MP_AS_IS);
	CHECK(!mp.yield);
	CHECK_EQ(mp.next_tb, mp.slice_end);

	/* tasks as urgent share the processor by weight (the blue task 200, others 100) */
	uint32_t one = start_task(queue), two = start_task(queue);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(mp.slice_end, mp.cake[MP_NOMINAL]);
	uint64_t now = mp.slice_end;
	CHECK(mp_due(&mp, now));
	mp_tick(&mp, now, MP_AS_IS);
	CHECK_EQ(schedule(now), one);
	CHECK_EQ(mp.slice_end, now + mp.cake[MP_NOMINAL] * 100 / 400);
	now = mp.slice_end;
	mp_tick(&mp, now, MP_AS_IS);
	CHECK_EQ(schedule(now), two);
	now = mp.slice_end;
	mp_tick(&mp, now, MP_AS_IS);
	CHECK_EQ(schedule(now), blue);
	CHECK_EQ(mp.slice_end, now + mp.cake[MP_NOMINAL] * 200 / 400);

	/* while the blue task runs with its 68k interrupts masked, nothing ends */
	now = mp.slice_end;
	mp_tick(&mp, now, MP_CRITICAL);
	CHECK(!mp.yield);
	CHECK_EQ(priority(blue), MP_NOMINAL);
	CHECK_EQ(mp.next_tb, now + mp.cake[MP_CRITICAL]);
	CHECK_EQ(mp_next(&mp, MP_CRITICAL), blue);      /* (but it's critical when the task to run is chosen) */
	CHECK_EQ(priority(blue), MP_CRITICAL);
	mp_switch(&mp, blue, now);
	CHECK_EQ(mp.slice_end, now + mp.cake[MP_CRITICAL]);
	now += 1000;
	mp_tick(&mp, now, MP_AS_IS);    /* (unmasked: critical for the rest of its time slice) */
	CHECK(!mp.yield);
	now = mp.slice_end;
	mp_tick(&mp, now, MP_AS_IS);
	CHECK(mp.yield);
	CHECK_EQ(priority(blue), MP_NOMINAL);
	CHECK_EQ(schedule(now), one);

	/* an interrupt for the blue task: it's critical, and runs next */
	mp_interrupt(&mp, now);
	CHECK_EQ(priority(blue), MP_CRITICAL);
	CHECK_EQ(schedule(now), blue);
	CHECK_EQ(mp.slice_end, now + mp.cake[MP_CRITICAL]);
	CHECK_EQ(mp_next(&mp, MP_LATENCY_PROTECTED), blue);     /* (its 68k mask makes it latency-protected) */
	CHECK_EQ(priority(blue), MP_LATENCY_PROTECTED);
	CHECK_EQ(mp_next(&mp, MP_AS_IS), blue);
	CHECK_EQ(priority(blue), MP_LATENCY_PROTECTED);
	now = mp.slice_end;
	mp_tick(&mp, now, MP_AS_IS);
	CHECK_EQ(priority(blue), MP_NOMINAL);
	CHECK_EQ(schedule(now), two);
}

/* The blue task's naps, and MPYieldWithHint */
static void test_naps(void)
{
	mp_init(&mp, TB_HZ);
	uint32_t blue = mp.blue_task;
	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);
	uint32_t queue = cpu.gpr[4];
	uint32_t one = start_task(queue), two = start_task(queue);

	/* tasks run while the blue task naps, until they're all blocked */
	CHECK_EQ(mp_nap(&mp, 1000000), MP_NAP_YIELDED);
	CHECK(mp_napping(&mp));
	CHECK_EQ(schedule(0), one);
	uint32_t wait[] = { queue, 0, 0, 0, 0x7FFFFFFF };
	CHECK_EQ(call(18, 5, wait), (int32_t)queue);
	CHECK_EQ(schedule(0), two);
	CHECK_EQ(call(18, 5, wait), (int32_t)queue);
	CHECK_EQ(mp_next(&mp, MP_AS_IS), blue);
	CHECK(mp_napping(&mp));
	CHECK_EQ(schedule(0), blue);
	CHECK(!mp_napping(&mp));
	CHECK_EQ(mp_nap(&mp, UINT64_MAX), MP_NAP_IDLE);         /* (nothing else to run) */

	/* a nap ends at its time (the blue task latency-protected, ahead of nominal tasks) */
	uint32_t three = start_task(queue);
	CHECK_EQ(mp_nap(&mp, 1000), MP_NAP_YIELDED);
	CHECK_EQ(schedule(0), three);
	CHECK_EQ(mp.next_tb, 0);
	mp_tick(&mp, 0, MP_AS_IS);
	CHECK_EQ(mp.next_tb, 1000);
	mp_tick(&mp, 1000, MP_AS_IS);
	CHECK_EQ(state(blue), MP_TASK_READY);
	CHECK_EQ(priority(blue), MP_LATENCY_PROTECTED);
	CHECK_EQ(schedule(1000), blue);

	/* ... at an interrupt */
	CHECK_EQ(mp_nap(&mp, UINT64_MAX), MP_NAP_YIELDED);
	CHECK_EQ(schedule(1000), three);
	mp_interrupt(&mp, 1000);
	CHECK_EQ(schedule(1000), blue);

	/* ... or when what it polled last is there */
	CHECK_EQ(call(18, 5, wait), MP_BLUE_BLOCKING_ERR);
	CHECK_EQ(schedule(1000), blue);         /* (critical) */
	CHECK_EQ(mp_nap(&mp, UINT64_MAX), MP_NAP_YIELDED);
	CHECK_EQ(schedule(1000), three);
	uint32_t notify[] = { queue, 1, 2, 3 };
	CHECK_EQ(call(17, 4, notify), MP_NO_ERR);
	CHECK_EQ(state(blue), MP_TASK_READY);
	CHECK_EQ(priority(blue), MP_LATENCY_PROTECTED);
	CHECK_EQ(state(one), MP_TASK_READY);
	CHECK_EQ(schedule(1000), blue);

	/* if it's there before the nap, there's none */
	mp_init(&mp, TB_HZ);
	blue = mp.blue_task;
	uint32_t sem_args[] = { 1, 0 };
	CHECK_EQ(call(20, 2, sem_args), MP_NO_ERR);
	uint32_t sem = cpu.gpr[5];
	uint32_t task = start_task(0);
	uint32_t sem_wait[] = { sem, 0x7FFFFFFF }, signal[] = { sem }, yield[] = { 0 };
	CHECK_EQ(call(23, 2, sem_wait), MP_BLUE_BLOCKING_ERR);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(22, 1, signal), MP_NO_ERR);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(mp_nap(&mp, UINT64_MAX), MP_NAP_SKIPPED);
	CHECK_EQ(mp_nap(&mp, UINT64_MAX), MP_NAP_YIELDED);

	/* MPYieldWithHint: the blue task, more urgent, becomes nominal at the front */
	CHECK_EQ(schedule(0), task);
	mp_interrupt(&mp, 0);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(mp_next(&mp, MP_LATENCY_PROTECTED), blue);
	uint32_t hint[] = { 1 };
	CHECK_EQ(call(13, 1, hint), 1);
	CHECK_EQ(priority(blue), MP_NOMINAL);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(13, 1, hint), 1);
	CHECK(!mp.yield);
}

/* Timeouts on waits, delays, and timers, on the timebase */
static void test_timers(void)
{
	mp_init(&mp, TB_HZ);
	uint32_t blue = mp.blue_task;
	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);
	uint32_t queue = cpu.gpr[4];
	uint32_t task = start_task(queue);
	uint32_t yield[] = { 0 };
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(0), task);
	CHECK_EQ(mp_next_due(&mp), UINT64_MAX);

	/* a wait times out (Durations: milliseconds, or negative, microseconds), its call returning kMPTimeoutErr */
	cpu.tb = 1000;
	uint32_t wait[] = { queue, 0, 0, 0, 10 };
	CHECK_EQ(call(18, 5, wait), (int32_t)queue);
	CHECK_EQ(mp_lookup(&mp, task, MP_TASK)->task.deadline, 1000 + 166000);
	CHECK_EQ(mp_next_due(&mp), 167000);
	CHECK_EQ(schedule(1000), blue);
	mp_tick(&mp, 166999, MP_AS_IS);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	CHECK_EQ(mp.next_tb, 167000);
	mp_tick(&mp, 167000, MP_AS_IS);
	CHECK_EQ(state(task), MP_TASK_READY);
	CHECK_EQ(priority(task), MP_NOMINAL);
	CHECK_EQ(mp_next_due(&mp), UINT64_MAX);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(167000), task);
	wait[1] = 0x44;
	CHECK_EQ(call(18, 5, wait), MP_TIMEOUT_ERR);
	CHECK_EQ(cpu.gpr[4], 0x44);

	/* ... unless what it waits for comes first */
	cpu.tb = 200000;
	uint32_t sem_args[] = { 1, 0 };
	CHECK_EQ(call(20, 2, sem_args), MP_NO_ERR);
	uint32_t sem = cpu.gpr[5];
	uint32_t sem_wait[] = { sem, (uint32_t)-500 };  /* (500 us: 66 * 500 / 4 ticks) */
	CHECK_EQ(call(23, 2, sem_wait), (int32_t)sem);
	CHECK_EQ(mp_next_due(&mp), 200000 + 8250);
	CHECK_EQ(schedule(200000), blue);
	uint32_t signal[] = { sem };
	CHECK_EQ(call(22, 1, signal), MP_NO_ERR);
	CHECK_EQ(mp_next_due(&mp), UINT64_MAX);
	mp_tick(&mp, 300000, MP_AS_IS);
	CHECK_EQ(schedule(300000), task);
	CHECK_EQ(call(23, 2, sem_wait), MP_NO_ERR);

	/* event groups and critical regions time out too */
	cpu.tb = 300000;
	CHECK_EQ(call(49, 0, NULL), MP_NO_ERR);
	uint32_t event_wait[] = { cpu.gpr[4], 0, 1 };
	CHECK_EQ(call(52, 3, event_wait), (int32_t)event_wait[0]);
	CHECK_EQ(mp_next_due(&mp), 300000 + 16600);
	CHECK_EQ(schedule(300000), blue);
	mp_tick(&mp, 400000, MP_AS_IS);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(400000), task);
	CHECK_EQ(call(52, 3, event_wait), MP_TIMEOUT_ERR);
	CHECK_EQ(call(25, 0, NULL), MP_NO_ERR);
	uint32_t region = cpu.gpr[4];
	uint32_t enter[] = { region, 1, mp.blue_process };
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(400000), blue);
	CHECK_EQ(call(27, 3, enter), MP_NO_ERR);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(400000), task);
	cpu.tb = 400000;
	CHECK_EQ(call(27, 3, enter), (int32_t)region);
	CHECK_EQ(schedule(400000), blue);
	mp_tick(&mp, 416600, MP_AS_IS);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(416600), task);
	CHECK_EQ(call(27, 3, enter), MP_TIMEOUT_ERR);

	/* MPDelayUntil (and MPDelayUntilSys, for tasks): until then, then latency-protected */
	uint32_t delay[] = { 0x80000000u, 500000 };     /* (the top bit is ignored) */
	CHECK_EQ(call(33, 2, delay), (int32_t)0x80000000u);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	CHECK_EQ(mp_next_due(&mp), 500000);
	CHECK_EQ(schedule(416600), blue);
	CHECK_EQ(call(33, 2, delay), MP_BLUE_BLOCKING_ERR);     /* (the blue task may not; it naps with call 55) */
	mp_tick(&mp, 499999, MP_AS_IS);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	mp_tick(&mp, 500000, MP_AS_IS);
	CHECK_EQ(priority(task), MP_LATENCY_PROTECTED);
	CHECK_EQ(schedule(500000), task);
	CHECK_EQ(call(33, 2, delay), MP_NO_ERR);
	delay[1] = 600000;
	CHECK_EQ(call(55, 2, delay), (int32_t)delay[0]);
	CHECK_EQ(mp_next_due(&mp), 600000);
	CHECK_EQ(schedule(500000), blue);

	/* while the blue task runs with its 68k interrupts masked, nothing ends */
	mp_tick(&mp, 600000, MP_CRITICAL);
	CHECK_EQ(state(task), MP_TASK_BLOCKED);
	mp_tick(&mp, 600000 + mp.cake[MP_CRITICAL], MP_AS_IS);
	CHECK_EQ(state(task), MP_TASK_READY);
	CHECK_EQ(schedule(600000), task);
	CHECK_EQ(call(55, 2, delay), MP_NO_ERR);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(600000), blue);

	/* a timer: made at a time (for deltas); armed, it notifies as set and is deleted */
	cpu.tb = 700000;
	CHECK_EQ(call(40, 0, NULL), MP_NO_ERR);
	uint32_t timer = cpu.gpr[4];
	CHECK_EQ(mp_lookup(&mp, timer, MP_TIMER)->timer.time, 700000);
	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);
	uint32_t timer_queue = cpu.gpr[4];
	CHECK_EQ(call(49, 0, NULL), MP_NO_ERR);
	uint32_t event = cpu.gpr[4];
	uint32_t to_queue[] = { timer, timer_queue, 1, 2, 3 }, to_sem[] = { timer, sem }, to_event[] = { timer, event, 8 };
	CHECK_EQ(call(30, 5, to_queue), MP_NO_ERR);
	CHECK_EQ(call(30, 2, to_sem), MP_NO_ERR);
	CHECK_EQ(call(30, 3, to_event), MP_NO_ERR);
	uint32_t arm[] = { timer, 0, 800000, 0 };
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	CHECK_EQ(mp_next_due(&mp), 800000);
	mp_tick(&mp, 799999, MP_AS_IS);
	CHECK(mp_lookup(&mp, timer, MP_TIMER) != NULL);
	mp_tick(&mp, 800000, MP_AS_IS);
	CHECK(mp_lookup(&mp, timer, MP_TIMER) == NULL);
	CHECK_EQ(mp_lookup(&mp, sem, MP_SEMAPHORE)->sem.value, 1);
	CHECK_EQ(mp_lookup(&mp, event, MP_EVENT_GROUP)->event.flags, 8);
	uint32_t timer_wait[] = { timer_queue, 0, 0, 0, 0 };
	CHECK_EQ(call(18, 5, timer_wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 1);
	CHECK_EQ(cpu.gpr[6], 3);
	CHECK_EQ(call(31, 4, arm), MP_INVALID_ID_ERR);

	/* kept (option 1): armed again from its last time (2), or for a Duration (4) */
	CHECK_EQ(call(40, 0, NULL), MP_NO_ERR);
	timer = arm[0] = to_queue[0] = cpu.gpr[4];
	CHECK_EQ(call(30, 5, to_queue), MP_NO_ERR);
	arm[3] = 1;
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	mp_tick(&mp, 800000, MP_AS_IS);
	CHECK(mp_lookup(&mp, timer, MP_TIMER) != NULL);
	CHECK_EQ(mp_next_due(&mp), UINT64_MAX);
	timer_wait[4] = 0;
	CHECK_EQ(call(18, 5, timer_wait), MP_NO_ERR);
	arm[2] = 1000;
	arm[3] = 3;
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	CHECK_EQ(mp_next_due(&mp), 801000);
	arm[1] = 2;
	arm[3] = 7;
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	CHECK_EQ(mp_next_due(&mp), 801000 + 33200);
	arm[3] = 5;
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	CHECK_EQ(mp_next_due(&mp), 33200);

	/* cancelled, it doesn't fire, and says how long it had left */
	cpu.tb = 33000;
	uint32_t cancel[] = { timer };
	CHECK_EQ(call(32, 1, cancel), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 0);
	CHECK_EQ(cpu.gpr[5], 200);
	CHECK_EQ(mp_next_due(&mp), UINT64_MAX);
	CHECK_EQ(call(32, 1, cancel), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[5], 0);

	/* timers due together fire in the order they were armed, and wake who waits */
	CHECK_EQ(call(40, 0, NULL), MP_NO_ERR);
	uint32_t other = cpu.gpr[4], to_queue2[] = { other, timer_queue, 4, 5, 6 };
	CHECK_EQ(call(30, 5, to_queue2), MP_NO_ERR);
	uint32_t arm2[] = { other, 0, 900000, 0 };
	CHECK_EQ(call(31, 4, arm2), MP_NO_ERR);
	arm[1] = 0;
	arm[2] = 900000;
	arm[3] = 1;
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	CHECK_EQ(call(13, 1, yield), 0);
	CHECK_EQ(schedule(800000), task);
	timer_wait[4] = 0x7FFFFFFF;
	CHECK_EQ(call(18, 5, timer_wait), (int32_t)timer_queue);
	CHECK_EQ(schedule(800000), blue);
	mp_tick(&mp, 900000, MP_AS_IS);
	CHECK_EQ(priority(task), MP_LATENCY_PROTECTED);
	CHECK_EQ(schedule(900000), task);
	CHECK_EQ(call(18, 5, timer_wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 4);
	CHECK_EQ(call(18, 5, timer_wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 1);
	uint32_t del[] = { timer };
	CHECK_EQ(call(41, 1, del), MP_NO_ERR);
	CHECK_EQ(call(41, 1, del), MP_INVALID_ID_ERR);

	/* one that sets an event group raising a software interrupt raises it */
	static uint8_t edp[4096];
	mp.emulator_data = edp;
	mp.ecb = 0x100;
	put_be32(edp + 0x100 + 0xC8, 0xAE0);
	uint32_t swi[] = { event, 1 };
	CHECK_EQ(call(54, 2, swi), MP_NO_ERR);
	CHECK_EQ(call(40, 0, NULL), MP_NO_ERR);
	to_event[0] = arm[0] = cpu.gpr[4];
	CHECK_EQ(call(30, 3, to_event), MP_NO_ERR);
	arm[2] = 950000;
	CHECK_EQ(call(31, 4, arm), MP_NO_ERR);
	CHECK(!mp.swi);
	mp_tick(&mp, 950000, MP_AS_IS);
	CHECK(mp.swi);
	CHECK_EQ(be32(edp + 0xAE0 + 4), event);
	CHECK_EQ(be32(edp + 0x100 + 0xD0), 1);
}

/* Event groups raising the 68k emulator's software interrupts (EmulatorData: the context block at 0x100, the table at 0xAE0) */
static void test_swi(void)
{
	static uint8_t edp[4096];
	memset(edp, 0, sizeof edp);
	mp_init(&mp, TB_HZ);
	mp.emulator_data = edp;
	mp.ecb = 0x100;
	put_be32(edp + 0x100 + 0xC8, 0xAE0);
	uint32_t blue = mp.blue_task;
	uint32_t group[3];
	for (unsigned k = 0; k < 3; k++) {
		CHECK_EQ(call(49, 0, NULL), MP_NO_ERR);
		group[k] = cpu.gpr[4];
	}
	uint32_t level1[] = { group[0], 1 }, level7[] = { group[1], 7 }, plain[] = { group[2], 0 };
	CHECK_EQ(call(54, 2, level1), MP_NO_ERR);
	CHECK_EQ(call(54, 2, level7), MP_NO_ERR);
	CHECK_EQ(call(54, 2, plain), MP_NO_ERR);       /* (0, or above 8: level 1) */
	CHECK_EQ(mp_lookup(&mp, group[2], MP_EVENT_GROUP)->event.swi, 0x11);

	/* set, it goes in the table at its level, which is the one to take; the blue task is to be interrupted */
	uint32_t set1[] = { group[0], 8 }, set7[] = { group[1], 0x80000000u }, set_plain[] = { group[2], 2 };
	CHECK_EQ(call(51, 2, set1), MP_NO_ERR);
	CHECK(mp.swi);
	CHECK(mp.yield);
	CHECK_EQ(be32(edp + 0xAE0 + 4), group[0]);
	CHECK_EQ(be32(edp + 0x1D0), 1);
	CHECK_EQ(priority(blue), MP_NOMINAL);           /* (running: its priority stays) */
	CHECK_EQ(schedule(0), blue);

	/* only one waits at each level */
	mp.swi = false;
	CHECK_EQ(call(51, 2, set_plain), MP_NO_ERR);
	CHECK(!mp.swi);
	CHECK_EQ(be32(edp + 0xAE0 + 4), group[0]);

	/* a higher level is taken first; a lower one set meanwhile waits in the table */
	CHECK_EQ(call(51, 2, set7), MP_NO_ERR);
	CHECK(mp.swi);
	CHECK_EQ(be32(edp + 0xAE0 + 28), group[1]);
	CHECK_EQ(be32(edp + 0x1D0), 7);

	/* the handler takes the group's flags: the level comes out of the table, and the next is to be taken */
	uint32_t take7[] = { group[1], 0, 0 }, take1[] = { group[0], 0, 0 };
	CHECK_EQ(call(52, 3, take7), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 0x80000000u);
	CHECK_EQ(be32(edp + 0xAE0 + 28), 0);
	CHECK_EQ(be32(edp + 0x1D0), 1);
	CHECK_EQ(call(52, 3, take7), MP_TIMEOUT_ERR);   /* (no flags: nothing changes) */
	CHECK_EQ(be32(edp + 0x1D0), 1);
	CHECK_EQ(call(52, 3, take1), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 8);
	CHECK_EQ(be32(edp + 0xAE0 + 4), 0);
	CHECK_EQ(be32(edp + 0x1D0), 0);

	/* the blue task napping: its nap ends, and it's critical, ahead of tasks woken */
	CHECK_EQ(call(15, 0, NULL), MP_NO_ERR);
	uint32_t queue = cpu.gpr[4];
	uint32_t task = start_task(queue);
	CHECK_EQ(mp_nap(&mp, UINT64_MAX), MP_NAP_YIELDED);
	CHECK_EQ(schedule(0), task);
	mp.swi = false;
	CHECK_EQ(call(51, 2, set1), MP_NO_ERR);
	CHECK(mp.swi);
	CHECK_EQ(state(blue), MP_TASK_READY);
	CHECK_EQ(priority(blue), MP_CRITICAL);
	CHECK_EQ(mp.ready[0], blue);
	CHECK_EQ(schedule(0), blue);
	CHECK_EQ(call(52, 3, take1), MP_NO_ERR);

	/* without the table, the context block holds the group to take, one at a time */
	put_be32(edp + 0x100 + 0xC8, 0);
	mp.swi = false;
	CHECK_EQ(call(51, 2, set7), MP_NO_ERR);
	CHECK(mp.swi);
	CHECK_EQ(be32(edp + 0x1D0), group[1]);
	mp.swi = false;
	CHECK_EQ(call(51, 2, set1), MP_NO_ERR);
	CHECK(!mp.swi);
	CHECK_EQ(call(52, 3, take1), MP_NO_ERR);
	CHECK_EQ(be32(edp + 0x1D0), group[1]);
	CHECK_EQ(call(52, 3, take7), MP_NO_ERR);
	CHECK_EQ(be32(edp + 0x1D0), 0);

	/* MPGetNextID: the objects of a class (0: any) in turn */
	uint32_t first[] = { MP_EVENT_GROUP, 0 };
	CHECK_EQ(call(37, 2, first), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], group[0]);
	uint32_t next[] = { MP_EVENT_GROUP, group[2] };
	CHECK_EQ(call(37, 2, next), MP_INVALID_ID_ERR);
	uint32_t any[] = { 0, 0 };
	CHECK_EQ(call(37, 2, any), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], mp.blue_process);
	any[1] = mp.blue_process;
	CHECK_EQ(call(37, 2, any), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], blue);
}

void test_mp(void)
{
	test_tasks();
	test_waking();
	test_slices();
	test_naps();
	test_timers();
	test_swi();
}
