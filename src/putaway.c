#include "putaway.h"

#include <stdio.h>
#include <string.h>

#include "install.h"

#define WAIT_TB(hz) ((hz) * 10)         /* for an idle call */

enum { I_UNMOUNT_VOL, I_EJECT, I_COUNT };

static const struct ndrv_import imports[I_COUNT] = {
	[I_UNMOUNT_VOL] = { "InterfaceLib", "UnmountVol" },
	[I_EJECT] = { "InterfaceLib", "Eject" },
};

void putaway_ask(struct putaway *p, uint16_t drive, uint64_t tb)
{
	memset(p, 0, sizeof *p);
	p->phase = PUTAWAY_WAIT;
	p->drive = drive;
	p->asked_tb = tb;
}

bool putaway_waiting(const struct putaway *p)
{
	return p->phase == PUTAWAY_WAIT;
}

static void done(struct putaway *p)
{
	p->phase = PUTAWAY_DONE;
}

bool putaway_idle(struct putaway *p, struct mem *m, uint16_t trap, bool region_free, uint64_t tb, uint64_t tb_hz)
{
	if (p->phase == PUTAWAY_INJECTED &&
	    install_diverted(m, p->trap_entry, p->trap_routine, "Blue's code", p->why, sizeof p->why)) {
		done(p);
		return false;
	}
	if (p->phase != PUTAWAY_WAIT)
		return false;
	if (trap && region_free) {
		const struct ndrv_spec spec = {
			.kind = FRAG_APPLICATION,
			.entries = 1,
			.hostcall = HOSTCALL_PUTAWAY,
			.imports = imports,
			.import_count = I_COUNT,
		};
		if (install_divert(m, trap, &spec, "Blue Put Away", &p->layout, &p->trap_entry, &p->trap_routine)) {
			p->phase = PUTAWAY_INJECTED;
			return true;
		}
		snprintf(p->why, sizeof p->why, "Blue's code couldn't be made");
		done(p);
	} else if (tb - p->asked_tb >= WAIT_TB(tb_hz)) {
		snprintf(p->why, sizeof p->why, "Mac OS is busy (it hasn't idled for 10 seconds)");
		done(p);
	}
	return false;
}

/* UnmountVol(NULL, drive), then Eject(NULL, drive): a drive number does for a volume's reference number */
void putaway_call(struct putaway *p, struct ndrv_call *c)
{
	if (c->entry == 0) {                    /* main() */
		if (p->phase != PUTAWAY_INJECTED) {
			ndrv_finish(c, 0);
			return;
		}
		p->phase = PUTAWAY_RUNNING;
		p->ran = true;
		ndrv_callout(c, ndrv_import(c, &p->layout, I_UNMOUNT_VOL), 0, p->drive, 0, 0);
		memset(c->out + 4, 0, 4 * sizeof c->out[0]);
		return;
	}
	if (p->phase != PUTAWAY_RUNNING) {
		ndrv_finish(c, 0);
		return;
	}
	int16_t err = (int16_t)c->arg[0];
	if (!p->ejecting && !err) {
		p->unmounted = p->ejecting = true;
		ndrv_callout(c, ndrv_import(c, &p->layout, I_EJECT), 0, p->drive, 0, 0);
		memset(c->out + 4, 0, 4 * sizeof c->out[0]);
		return;
	}
	p->error = err;
	done(p);
	ndrv_finish(c, 0);
}
