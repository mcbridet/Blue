/*
 * Builds native driver (NDRV) fragments in Apple's PEF container format,
 * and other code fragments made the same way (applications, shared
 * libraries). The code is a small loop around a host call, so the logic
 * runs natively but can still call Mac OS services:
 *
 *   entry k:  li r11,k ; b common
 *   common:   save LR, push a frame, save r2
 *   loop:     hostcall          -> the kernel runs the native driver
 *             beq done          (CR0[EQ] set: finished, r3 = result)
 *             call the TVector in r12 with r3..r10 (a "callout")
 *             r11 = -1 ; b loop (resume: r3 = the callout's result)
 *   done:     pop the frame, return r3
 *
 * Native state that must survive a callout lives in the frame
 * (NDRV_FRAME_STATE words at r1 + NDRV_FRAME_STATE_OFFSET).
 *
 * Data section (r2 points at its start): import TVector pointers, the entry
 * TVectors, TheDriverDescription (drivers), then fragment-specific data,
 * then zero-filled data. Drivers export DoDriverIO (entry 0) and
 * TheDriverDescription; applications have entry 0 as their main routine;
 * shared libraries may have initialization and termination routines, export
 * entries and data by name, and have code of their own for some entries
 * (after the common code; see ndrv_code_offset()), which can branch to any
 * entry's stub, as above, to make the host call.
 */
#ifndef SYSHM_NDRV_H
#define SYSHM_NDRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mem.h"

/* DoDriverIO command codes and kinds */
enum {
	NDRV_OPEN = 0, NDRV_CLOSE, NDRV_READ, NDRV_WRITE, NDRV_CONTROL, NDRV_STATUS,
	NDRV_KILL_IO, NDRV_INITIALIZE, NDRV_FINALIZE, NDRV_REPLACE, NDRV_SUPERSEDED,
};
#define NDRV_KIND_SYNC 1u
#define NDRV_KIND_ASYNC 2u
#define NDRV_KIND_IMMEDIATE 4u

/* Runtime options in TheDriverDescription */
#define NDRV_LOADED_UPON_DISCOVERY 0x01u
#define NDRV_OPENED_UPON_LOAD 0x02u
#define NDRV_UNDER_EXPERT_CONTROL 0x04u

#define NDRV_MAX_IMPORTS 128
#define NDRV_MAX_ENTRIES 1024
#define NDRV_ENTRIES 2                  /* drivers: 0: DoDriverIO, 1: a secondary interrupt handler */
#define NDRV_ENTRY_RESUME 0xFFFFFFFFu   /* r11 after a callout */
#define NDRV_FRAME_SIZE 80u             /* the stub's stack frame */
#define NDRV_FRAME_STATE_OFFSET 56u     /* after the linkage and parameter areas */
#define NDRV_FRAME_STATE 6u
#define NDRV_STUB_SIZE 8u               /* each entry's li r11,k ; b common, at NDRV_STUB_SIZE * k */
#define NDRV_COMMON_SIZE 68u            /* the common code, after them */
#define NDRV_STUB 0xFFFFFFFFu           /* (entry_code: the entry's stub) */

/* Where a fragment's own code (ndrv_spec.code) starts in its code section */
static inline uint32_t ndrv_code_offset(unsigned entries)
{
	return NDRV_STUB_SIZE * entries + NDRV_COMMON_SIZE;
}

struct ndrv_import {
	const char *library, *name;     /* consecutive imports must group by library */
};

struct ndrv_export {
	const char *name;
	unsigned entry;
	bool data;                      /* a data symbol, at `offset` in the fragment-specific data */
	uint32_t offset;
};

enum frag_kind { FRAG_DRIVER, FRAG_APPLICATION, FRAG_LIBRARY };

struct ndrv_spec {
	enum frag_kind kind;
	unsigned entries;               /* entry points (0: NDRV_ENTRIES) */
	/* drivers */
	const char *device_name;        /* matched against the node's name/compatible */
	const char *driver_name;        /* unit table name, e.g. ".Display_syshm" */
	uint32_t runtime_options;
	uint32_t category, type;        /* service, e.g. 'ndrv' 'vide' */
	uint32_t version;               /* NumVersion */
	/* shared libraries */
	int init_entry;                 /* the initialization routine, or -1 */
	unsigned term_entry;            /* the termination routine, or 0 for none (entry 0 can't be it) */
	const struct ndrv_export *exports;
	unsigned export_count;
	const uint32_t *code;           /* code of its own, at ndrv_code_offset(entries) */
	size_t code_words;
	const uint32_t *entry_code;     /* where each entry's TVector points: a byte offset in the code section */
					/* (NDRV_STUB, or no entry_code: the entry's stub) */
	uint32_t current_version, old_def_version;     /* (in the container's header: see tools.c's cfrg) */
	/* all */
	uint32_t hostcall;              /* selector the code traps with */
	const struct ndrv_import *imports;
	unsigned import_count;
	uint64_t data_imports;          /* (bit k: import k is a data symbol, its slot its address; else a TVector) */
	const uint8_t *extra;           /* driver data, copied after the description */
	size_t extra_len;
	uint32_t zero_len;              /* zero-filled data after it */
};

/* Offsets within the data section (i.e. from r2) */
struct ndrv_layout {
	uint32_t import[NDRV_MAX_IMPORTS];      /* TVector pointer of each import */
	uint32_t entry[NDRV_MAX_ENTRIES];       /* TVector of each entry point */
	uint32_t extra;
};

/*
 * Returns the container size, or 0 if `size` is too small or the spec is
 * invalid. Exports are found through a hash table as PEF linkers make them.
 */
size_t ndrv_build(const struct ndrv_spec *spec, uint8_t *out, size_t size, struct ndrv_layout *layout);

/* PEF export hash word for a symbol name. */
uint32_t pef_hash_word(const char *name, size_t len);

/*
 * One step of a native driver, as seen by the kernel. On input: the entry
 * number (or NDRV_ENTRY_RESUME), r3..r10, r2 and r1. On output: either done
 * with a result, or a callout to `tvector` with `out` as r3..r10.
 */
struct ndrv_call {
	struct mem *mem;
	uint32_t entry;
	uint32_t arg[8];
	uint32_t toc, frame;
	uint64_t tb;                    /* current timebase (AbsoluteTime on PowerPC) */
	uint64_t f[13];                 /* f1 to f13 (floating-point arguments: see ndrv_args) */
	bool done;
	uint32_t result;
	bool set_f1;                    /* ... and a floating-point result in f1 */
	uint64_t f1_out;
	uint32_t tvector;
	uint32_t out[8];
	bool set_out_f;                 /* ... and f1 to f13 (floating-point arguments) */
	uint64_t out_f[13];
};

static inline void ndrv_finish(struct ndrv_call *c, uint32_t result)
{
	c->done = true;
	c->result = result;
}

static inline void ndrv_callout(struct ndrv_call *c, uint32_t tvector, uint32_t a0, uint32_t a1,
				uint32_t a2, uint32_t a3)
{
	c->done = false;
	c->tvector = tvector;
	c->out[0] = a0;
	c->out[1] = a1;
	c->out[2] = a2;
	c->out[3] = a3;
	c->set_out_f = false;
}

/* A callout's floating-point argument k (f1 + k, a double), after ndrv_callout() */
static inline void ndrv_callout_double(struct ndrv_call *c, unsigned k, double v)
{
	memcpy(&c->out_f[k], &v, sizeof v);
	c->set_out_f = true;
}

/* State words kept in the stub's stack frame across callouts */
static inline uint32_t ndrv_state(const struct ndrv_call *c, unsigned k)
{
	return mem_r32(c->mem, c->frame + NDRV_FRAME_STATE_OFFSET + 4 * k);
}

static inline void ndrv_set_state(struct ndrv_call *c, unsigned k, uint32_t v)
{
	mem_w32(c->mem, c->frame + NDRV_FRAME_STATE_OFFSET + 4 * k, v);
}

/*
 * A call's arguments in order, where the PowerPC calling convention puts
 * them: integers and pointers in the next word (r3 to r10, then the
 * caller's parameter area, 24 bytes into its frame), floating-point values
 * in the next of f1 to f13, each skipping a word (a double, two), or in its
 * words once f13 is taken. So glBitmap(w, h, xorig, yorig, xmove, ymove,
 * bitmap) has bitmap in r9, and glTexImage2D's ninth argument is in the
 * parameter area.
 */
struct ndrv_args {
	const struct ndrv_call *c;
	unsigned word, fpr;
};

static inline struct ndrv_args ndrv_args(const struct ndrv_call *c)
{
	return (struct ndrv_args){ c, 0, 0 };
}

/* Word k of the arguments (0 if the parameter area can't be read) */
uint32_t ndrv_arg(const struct ndrv_call *c, unsigned k);

uint32_t ndrv_next_word(struct ndrv_args *a);
uint32_t ndrv_next_float(struct ndrv_args *a);  /* (its bits, as stfs stores the register) */
uint64_t ndrv_next_double(struct ndrv_args *a); /* (its bits) */

/* Where the call came from (the return address the stub saved in its caller's frame) */
static inline uint32_t ndrv_caller(const struct ndrv_call *c)
{
	return mem_r32(c->mem, c->frame + NDRV_FRAME_SIZE + 8);
}

/* TVector of import `k` (a pointer the Code Fragment Manager filled in) */
static inline uint32_t ndrv_import(const struct ndrv_call *c, const struct ndrv_layout *l, unsigned k)
{
	return mem_r32(c->mem, c->toc + l->import[k]);
}

/*
 * Ends a DoDriverIO request as drivers must: immediate requests return the
 * status; others call IOCommandIsComplete(cmdID, status) (import
 * `io_complete`) and return its result. Uses state word 0 (set to
 * NDRV_STEP_COMPLETING) so the caller's resume path can finish.
 */
#define NDRV_STEP_COMPLETING 0x434F4D50u        /* 'COMP' */

static inline void ndrv_complete(struct ndrv_call *c, const struct ndrv_layout *l, unsigned io_complete,
				 uint32_t cmd_id, uint32_t kind, int32_t status)
{
	if (kind & NDRV_KIND_IMMEDIATE) {
		ndrv_finish(c, (uint32_t)status);
		return;
	}
	ndrv_set_state(c, 0, NDRV_STEP_COMPLETING);
	ndrv_callout(c, ndrv_import(c, l, io_complete), cmd_id, (uint32_t)status, 0, 0);
}

#endif
