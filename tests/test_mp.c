#include <string.h>

#include "../src/mp.h"
#include "test.h"

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

void test_mp(void)
{
	mp_init(&mp);
	mp.task_return = 0x68FFE000u;
	uint32_t blue = mp.blue_task;
	CHECK_EQ(mp.current, blue);
	CHECK_EQ(mp.tasks, 1);
	CHECK_EQ(mp_next(&mp), blue);

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
	CHECK_EQ(mp.tasks, 2);
	struct mp_regs *regs = mp_regs(&mp, task);
	CHECK(regs != NULL);
	CHECK_EQ(regs->pc, 0xFFCE03A0u);
	CHECK_EQ(regs->gpr[2], 0x35208);
	CHECK_EQ(regs->gpr[12], 0x35520);
	CHECK_EQ(regs->lr, mp.task_return);
	CHECK(!mp_ready(&mp));

	/* starting: (task, name, its r3, termination message words, its r1, its stack's end) */
	uint32_t start[] = { task, 0x64657468, 0x64218, 0x11, 0x22, 0x64218, 0x63E18 };
	CHECK_EQ(call(8, 7, start), MP_NO_ERR);
	CHECK_EQ(regs->gpr[3], 0x64218);
	CHECK_EQ(regs->gpr[1], 0x64218);
	CHECK(mp_ready(&mp));
	CHECK_EQ(mp_next(&mp), task);
	CHECK_EQ(call(8, 7, start), MP_INSUFFICIENT_RESOURCES_ERR);

	/* the blue task polls: it yields, and doesn't block */
	uint32_t wait[] = { queue, 0, 0, 0, 0x7FFFFFFF };
	CHECK_EQ(call(18, 5, wait), MP_BLUE_BLOCKING_ERR);
	CHECK(mp.yield);
	mp_switch(&mp, task);
	CHECK(!mp.yield);
	CHECK_EQ(mp.current, task);
	CHECK(!mp_ready(&mp));
	CHECK_EQ(mp_next(&mp), blue);

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
	CHECK_EQ(mp_lookup(&mp, task, MP_TASK)->task.state, MP_TASK_BLOCKED);
	CHECK_EQ(mp_next(&mp), blue);
	mp_switch(&mp, blue);
	CHECK_EQ(mp_lookup(&mp, task, MP_TASK)->task.state, MP_TASK_BLOCKED);

	/* terminating it sends its message, with the result, to the queue */
	uint32_t term[] = { task, 0x1234 };
	CHECK_EQ(call(10, 1, term), MP_INSUFFICIENT_RESOURCES_ERR);    /* (not terminated yet) */
	CHECK_EQ(call(9, 2, term), MP_NO_ERR);
	CHECK_EQ(call(9, 2, term), MP_INSUFFICIENT_RESOURCES_ERR);
	CHECK_EQ(call(18, 5, wait), MP_NO_ERR);
	CHECK_EQ(cpu.gpr[4], 0x11);
	CHECK_EQ(cpu.gpr[5], 0x22);
	CHECK_EQ(cpu.gpr[6], 0x1234);
	CHECK_EQ(call(10, 1, term), MP_NO_ERR);
	CHECK_EQ(mp.tasks, 1);
	CHECK(mp_regs(&mp, task) == NULL);

	/* a task that ends itself gives up the processor */
	CHECK_EQ(call(7, 7, create), MP_NO_ERR);
	task = cpu.gpr[10];
	start[0] = task;
	CHECK_EQ(call(8, 7, start), MP_NO_ERR);
	mp_switch(&mp, task);
	term[0] = task;
	CHECK_EQ(call(9, 2, term), MP_NO_ERR);
	CHECK(mp.yield);
	CHECK_EQ(mp_lookup(&mp, task, MP_TASK)->task.state, MP_TASK_TERMINATED);
	CHECK_EQ(mp_next(&mp), blue);
	mp_switch(&mp, blue);
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
