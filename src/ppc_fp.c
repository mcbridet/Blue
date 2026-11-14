#include "ppc_fp.h"

#include <fenv.h>
#include <math.h>
#include <string.h>

#ifdef __clang__
#pragma STDC FP_CONTRACT OFF    /* (the fast path's error-free transformations need every rounding) */
#endif

/* FPSCR bits (bit 0 = most significant) */
#define FPSCR_FX (1u << 31)
#define FPSCR_FEX (1u << 30)
#define FPSCR_VX (1u << 29)
#define FPSCR_OX (1u << 28)
#define FPSCR_UX (1u << 27)
#define FPSCR_ZX (1u << 26)
#define FPSCR_XX (1u << 25)
#define FPSCR_VXSNAN (1u << 24)
#define FPSCR_VXISI (1u << 23)
#define FPSCR_VXIDI (1u << 22)
#define FPSCR_VXZDZ (1u << 21)
#define FPSCR_VXIMZ (1u << 20)
#define FPSCR_VXVC (1u << 19)
#define FPSCR_FR (1u << 18)
#define FPSCR_FI (1u << 17)
#define FPSCR_FPRF_SHIFT 12
#define FPSCR_FPRF (0x1Fu << FPSCR_FPRF_SHIFT)
#define FPSCR_FPCC_SHIFT 12
#define FPSCR_VXSOFT (1u << 10)
#define FPSCR_VXSQRT (1u << 9)
#define FPSCR_VXCVI (1u << 8)
#define FPSCR_VE (1u << 7)
#define FPSCR_OE (1u << 6)
#define FPSCR_UE (1u << 5)
#define FPSCR_ZE (1u << 4)
#define FPSCR_XE (1u << 3)
#define FPSCR_RN 3u

#define FPSCR_VX_ALL (FPSCR_VXSNAN | FPSCR_VXISI | FPSCR_VXIDI | FPSCR_VXZDZ | FPSCR_VXIMZ | \
		      FPSCR_VXVC | FPSCR_VXSOFT | FPSCR_VXSQRT | FPSCR_VXCVI)
#define FPSCR_EXC_ALL (FPSCR_OX | FPSCR_UX | FPSCR_ZX | FPSCR_XX | FPSCR_VX_ALL)

/* Upper word given to fctiw[z]/mffs results; architecturally undefined */
#define FP_INT_HI 0xFFF8000000000000ull

enum fp_op { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_FMA, OP_SQRT, OP_OTHER };

static inline double f64(uint64_t b)
{
	double d;
	memcpy(&d, &b, sizeof d);
	return d;
}

static inline uint64_t b64(double d)
{
	uint64_t b;
	memcpy(&b, &d, sizeof b);
	return b;
}

/* Keeps the compiler from moving FP arithmetic across the fenv flag accesses. */
static double barrier(double x)
{
	volatile double v = x;
	return v;
}

static inline bool is_snan(uint64_t b)
{
	return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
	       (b & 0x000FFFFFFFFFFFFFull) && !(b & 0x0008000000000000ull);
}

uint64_t ppc_fp_single_to_double(uint32_t s)
{
	uint64_t sign = (uint64_t)(s >> 31) << 63;
	uint32_t exp = (s >> 23) & 0xFF, frac = s & 0x7FFFFF;

	if (exp == 0xFF)
		return sign | 0x7FF0000000000000ull | (uint64_t)frac << 29;
	if (exp == 0) {
		if (frac == 0)
			return sign;
		int e = -126;           /* normalize a single denormal */
		while (!(frac & 0x800000)) {
			frac <<= 1;
			e--;
		}
		frac &= 0x7FFFFF;
		return sign | (uint64_t)(e + 1023) << 52 | (uint64_t)frac << 29;
	}
	return sign | (uint64_t)(exp - 127 + 1023) << 52 | (uint64_t)frac << 29;
}

uint32_t ppc_fp_double_to_single(uint64_t d)
{
	uint32_t sign = (uint32_t)(d >> 63) << 31;
	int exp = (int)((d >> 52) & 0x7FF);
	uint64_t frac = d & 0x000FFFFFFFFFFFFFull;

	if (exp > 896)          /* normal single range, or above: bits 0-1 and 5-34, as the architecture says */
		return (uint32_t)(d >> 32 & 0xC0000000u) | (uint32_t)(d >> 29 & 0x3FFFFFFFu);
	if (exp >= 874) {       /* single denormal: shift in the implicit one, truncating */
		uint64_t m = (frac | 0x0010000000000000ull) >> (896 - exp + 1);
		return sign | (uint32_t)(m >> 29);
	}
	return sign;
}

/*
 * The host rounds to nearest, except while an operation in another FPSCR
 * rounding mode runs (the rest of the emulator, the fast path and generated
 * code count on it).
 */
static bool directed_rounding(uint32_t fpscr)
{
	static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
	if (!(fpscr & FPSCR_RN))
		return false;
	fesetround(modes[fpscr & FPSCR_RN]);
	return true;
}

static void nearest_rounding(bool was_directed)
{
	if (was_directed)
		fesetround(FE_TONEAREST);
}

static uint32_t fprf_of(double v)
{
	/* C FL FG FE FU */
	switch (fpclassify(v)) {
	case FP_NAN:       return 0x11;
	case FP_INFINITE:  return signbit(v) ? 0x09 : 0x05;
	case FP_ZERO:      return signbit(v) ? 0x12 : 0x02;
	case FP_SUBNORMAL: return signbit(v) ? 0x18 : 0x14;
	default:           return signbit(v) ? 0x08 : 0x04;
	}
}

static void fpscr_raise(struct ppc_cpu *c, uint32_t bits)
{
	if (bits & ~c->fpscr & FPSCR_EXC_ALL)
		c->fpscr |= FPSCR_FX;
	c->fpscr |= bits;
}

/* Recomputes the summary bits VX and FEX. */
static void fpscr_summarize(struct ppc_cpu *c)
{
	uint32_t f = c->fpscr & ~(FPSCR_VX | FPSCR_FEX);
	if (f & FPSCR_VX_ALL)
		f |= FPSCR_VX;
	if (((f & FPSCR_VX) && (f & FPSCR_VE)) || ((f & FPSCR_OX) && (f & FPSCR_OE)) ||
	    ((f & FPSCR_UX) && (f & FPSCR_UE)) || ((f & FPSCR_ZX) && (f & FPSCR_ZE)) ||
	    ((f & FPSCR_XX) && (f & FPSCR_XE)))
		f |= FPSCR_FEX;
	c->fpscr = f;
}

void ppc_fp_set_fpscr(struct ppc_cpu *c, uint32_t fpscr)
{
	c->fpscr = fpscr;
	fpscr_summarize(c);
}

/* Turns host exception flags from one operation into FPSCR updates. */
static void fp_finish(struct ppc_cpu *c, enum fp_op op, uint64_t a, uint64_t b, uint64_t cc, double r)
{
	int ex = fetestexcept(FE_ALL_EXCEPT);
	uint32_t raise = 0;

	c->fpscr &= ~(FPSCR_FR | FPSCR_FI);
	if (ex & FE_INEXACT) {
		raise |= FPSCR_XX;
		c->fpscr |= FPSCR_FI;
	}
	if (ex & FE_OVERFLOW)
		raise |= FPSCR_OX;
	if (ex & FE_UNDERFLOW)
		raise |= FPSCR_UX;
	if (ex & FE_DIVBYZERO)
		raise |= FPSCR_ZX;
	if (ex & FE_INVALID) {
		double da = f64(a), db = f64(b), dc = f64(cc);
		if (is_snan(a) || is_snan(b) || is_snan(cc))
			raise |= FPSCR_VXSNAN;
		else if (op == OP_DIV)
			raise |= (isinf(da) && isinf(db)) ? FPSCR_VXIDI : FPSCR_VXZDZ;
		else if (op == OP_MUL || (op == OP_FMA && ((isinf(da) && dc == 0) || (da == 0 && isinf(dc)))))
			raise |= FPSCR_VXIMZ;
		else if (op == OP_SQRT)
			raise |= FPSCR_VXSQRT;
		else
			raise |= FPSCR_VXISI;
	}
	fpscr_raise(c, raise);
	c->fpscr = (c->fpscr & ~FPSCR_FPRF) | fprf_of(r) << FPSCR_FPRF_SHIFT;
	fpscr_summarize(c);
}

/*
 * The fast path, for operations whose operands and result lie well inside
 * the normal range (or are zero) in round-to-nearest mode: then the only
 * flag the operation can raise is inexact, and an error-free transformation
 * tells whether it was, without the host's exception flags (clearing which
 * stalls the host FPU for longer than the operation takes). The results are
 * exactly those of the general path. Magnitudes from 2^-900 to 2^1000 keep
 * every intermediate of the transformations clear of underflow and overflow.
 */
static inline bool tame(double x)
{
	double ax = fabs(x);
	return ax == 0 || (ax >= 0x1p-900 && ax <= 0x1p+1000);
}

/* ... and the single-precision result of an opcode-59 operation */
static inline bool tame_single(double r, double rd)
{
	double ar = fabs(r);
	return ar == 0 ? rd == 0 : ar >= 0x1p-120 && ar <= 0x1p+126;
}

/* s = a + b rounded; is it inexact? (Knuth's TwoSum) */
static inline bool sum_inexact(double a, double b, double s)
{
	double bv = s - a, av = s - bv;
	return (a - av) + (b - bv) != 0;
}

/* r = a * c + b rounded (one rounding); is it inexact? (Boldo and Muller's ErrFma) */
static inline bool fma_inexact(double a, double c, double b, double r)
{
	double u1 = a * c, u2 = fma(a, c, -u1);
	double a1 = b + u2, z = (b - (a1 - (a1 - b))) + (u2 - (a1 - b));     /* TwoSum(b, u2) */
	double b1 = u1 + a1, bv = b1 - u1, b2 = (u1 - (b1 - bv)) + (a1 - bv);   /* TwoSum(u1, a1) */
	double g = (b1 - r) + b2;
	double r2 = g + z, r3 = z - (r2 - g);                                   /* Fast2Sum(g, z) */
	return r2 != 0 || r3 != 0;
}

/* The fast path for an A-form operation; false to take the general one */
static bool fp_fast(struct ppc_cpu *c, unsigned op, unsigned xo5, double a, double b, double cc, double *out)
{
	/*
	 * Division and square roots want tame operands. For sums, products and
	 * multiply-adds a result in the tame range says enough: infinite and NaN
	 * operands give infinite or NaN results, and with such a result the
	 * transformations stay exact even for tiny (denormal) operands.
	 */
	bool checked = xo5 == 18 || xo5 == 22 || xo5 == 24 || xo5 == 26;
	if ((c->fpscr & FPSCR_RN) || (checked && ((xo5 == 18 && !tame(a)) || !tame(b))))
		return false;
	double r;
	bool inexact;
	switch (xo5) {
	case 18:                                                                /* fdiv */
		if (b == 0)
			return false;
		r = a / b;
		if (!tame(r) || (r == 0 && a != 0))
			return false;
		inexact = fma(-r, b, a) != 0;
		break;
	case 20:                                                                /* fsub */
		r = a - b;
		if (!tame(r))
			return false;
		inexact = sum_inexact(a, -b, r);
		break;
	case 21:                                                                /* fadd */
		r = a + b;
		if (!tame(r))
			return false;
		inexact = sum_inexact(a, b, r);
		break;
	case 22:                                                                /* fsqrt */
		if (!(b >= 0))
			return false;
		r = sqrt(b);
		inexact = fma(-r, r, b) != 0;
		break;
	case 24:                                                                /* fres */
		if (b == 0)
			return false;
		r = 1.0 / b;
		if (!tame(r) || r == 0)
			return false;
		inexact = fma(-r, b, 1.0) != 0;
		break;
	case 26: {                                                              /* frsqrte */
		if (!(b > 0))
			return false;
		double s = sqrt(b);
		r = 1.0 / s;
		inexact = fma(-s, s, b) != 0 || fma(-r, s, 1.0) != 0;
		break;
	}
	case 25:                                                                /* fmul */
		r = a * cc;
		if (!tame(r) || (r == 0 && a != 0 && cc != 0))
			return false;
		inexact = fma(a, cc, -r) != 0;
		break;
	case 28: case 29: case 30: case 31: {                                   /* fmsub fmadd fnmsub fnmadd */
		double addend = xo5 & 1 ? b : -b;
		r = fma(a, cc, addend);
		/*
		 * (A zero result is exact here: a * c + b, if not zero, is a multiple
		 * of 2^-1006 or so for tame operands, and a product that underflows
		 * goes the general way.)
		 */
		if (!tame(r) || !tame(a * cc) || (a * cc == 0 && a != 0 && cc != 0))
			return false;
		inexact = fma_inexact(a, cc, addend, r);
		if (xo5 >= 30)
			r = -r;
		break;
	}
	default:
		return false;
	}
	if (op == 59) {
		double rs = (double)(float)r;
		if (!tame_single(rs, r))
			return false;
		inexact |= rs != r;
		r = rs;
	}
	c->fpscr &= ~(FPSCR_FR | FPSCR_FI);
	if (inexact) {
		fpscr_raise(c, FPSCR_XX);
		c->fpscr |= FPSCR_FI;
	}
	c->fpscr = (c->fpscr & ~FPSCR_FPRF) | fprf_of(r) << FPSCR_FPRF_SHIFT;
	fpscr_summarize(c);
	*out = r;
	ppc_fp_fast_ops++;
	return true;
}

bool ppc_fp_fast_path = true;
unsigned long ppc_fp_fast_ops;

static void set_cr1(struct ppc_cpu *c)
{
	c->cr = (c->cr & ~0x0F000000u) | (c->fpscr >> 4 & 0x0F000000u);
}

static void fp_compare(struct ppc_cpu *c, unsigned crf, uint64_t a, uint64_t b, bool ordered)
{
	double da = f64(a), db = f64(b);
	uint32_t res = da < db ? 8 : da > db ? 4 : da == db ? 2 : 1;

	c->cr = (c->cr & ~(0xFu << (28 - 4 * crf))) | res << (28 - 4 * crf);
	c->fpscr = (c->fpscr & ~(0xFu << FPSCR_FPCC_SHIFT)) | res << FPSCR_FPCC_SHIFT;
	if (res == 1) {
		uint32_t raise = 0;
		if (is_snan(a) || is_snan(b))
			raise |= FPSCR_VXSNAN;
		if (ordered)
			raise |= FPSCR_VXVC;
		fpscr_raise(c, raise);
		fpscr_summarize(c);
	}
}

static uint64_t fp_to_int(struct ppc_cpu *c, uint64_t b, bool toward_zero)
{
	double v = f64(b);
	uint32_t res, raise = 0;

	c->fpscr &= ~(FPSCR_FR | FPSCR_FI);
	if (isnan(v)) {
		res = 0x80000000u;
		raise = FPSCR_VXCVI | (is_snan(b) ? FPSCR_VXSNAN : 0);
	} else {
		double r = barrier(toward_zero ? trunc(barrier(v)) : nearbyint(barrier(v)));
		if (r > 2147483647.0) {
			res = 0x7FFFFFFFu;
			raise = FPSCR_VXCVI;
		} else if (r < -2147483648.0) {
			res = 0x80000000u;
			raise = FPSCR_VXCVI;
		} else {
			res = (uint32_t)(int32_t)r;
			if (r != v) {
				raise = FPSCR_XX;
				c->fpscr |= FPSCR_FI;
			}
		}
	}
	fpscr_raise(c, raise);
	fpscr_summarize(c);
	return FP_INT_HI | res;
}

bool ppc_fp_exec(struct ppc_cpu *c, uint32_t i)
{
	unsigned op = i >> 26, frt = (i >> 21) & 31, fra = (i >> 16) & 31, frb = (i >> 11) & 31;
	unsigned frc = (i >> 6) & 31, xo5 = (i >> 1) & 31, xo10 = (i >> 1) & 0x3FF;
	bool rc = i & 1;
	uint64_t *f = c->fpr;
	uint64_t a = f[fra], b = f[frb], cc = f[frc];
	double r;
	enum fp_op kind = OP_OTHER;

	if (op == 63 && !(xo5 & 0x10)) {
		/* X-form: moves, compares, conversions, FPSCR access */
		switch (xo10) {
		case 0:                         /* fcmpu */
		case 32:                        /* fcmpo */
			fp_compare(c, (i >> 23) & 7, a, b, xo10 == 32);
			return true;
		case 12: {                      /* frsp */
			if (ppc_fp_fast_path && !(c->fpscr & FPSCR_RN) && tame(f64(b))) {
				double rs = (double)(float)f64(b);
				if (tame_single(rs, f64(b))) {
					c->fpscr &= ~(FPSCR_FR | FPSCR_FI);
					if (rs != f64(b)) {
						fpscr_raise(c, FPSCR_XX);
						c->fpscr |= FPSCR_FI;
					}
					c->fpscr = (c->fpscr & ~FPSCR_FPRF) | fprf_of(rs) << FPSCR_FPRF_SHIFT;
					fpscr_summarize(c);
					f[frt] = b64(rs);
					break;
				}
			}
			bool directed = directed_rounding(c->fpscr);
			feclearexcept(FE_ALL_EXCEPT);
			r = barrier((double)(float)barrier(f64(b)));
			f[frt] = is_snan(b) ? b | 0x0008000000000000ull : b64(r);
			fp_finish(c, OP_OTHER, b, 0, 0, f64(f[frt]));
			nearest_rounding(directed);
			break;
		}
		case 14:                        /* fctiw */
		case 15: {                      /* fctiwz */
			bool directed = xo10 == 14 && directed_rounding(c->fpscr);
			f[frt] = fp_to_int(c, b, xo10 == 15);
			nearest_rounding(directed);
			break;
		}
		case 40: f[frt] = b ^ 0x8000000000000000ull; break;                     /* fneg */
		case 72: f[frt] = b; break;                                             /* fmr */
		case 136: f[frt] = b | 0x8000000000000000ull; break;                    /* fnabs */
		case 264: f[frt] = b & ~0x8000000000000000ull; break;                   /* fabs */
		case 583: f[frt] = c->fpscr; break;                                     /* mffs */
		case 38:                        /* mtfsb1 */
		case 70: {                      /* mtfsb0 */
			uint32_t bit = 0x80000000u >> frt;
			if (bit != FPSCR_FEX && bit != FPSCR_VX) {
				if (xo10 == 38)
					fpscr_raise(c, bit);
				else
					c->fpscr &= ~bit;
			}
			fpscr_summarize(c);
			break;
		}
		case 64: {                      /* mcrfs */
			unsigned crfd = (i >> 23) & 7, crfs = (i >> 18) & 7;
			uint32_t field = (c->fpscr >> (28 - 4 * crfs)) & 0xF;
			c->cr = (c->cr & ~(0xFu << (28 - 4 * crfd))) | field << (28 - 4 * crfd);
			c->fpscr &= ~((0xFu << (28 - 4 * crfs)) & (FPSCR_FX | FPSCR_EXC_ALL));
			fpscr_summarize(c);
			return true;
		}
		case 134: {                     /* mtfsfi */
			unsigned crfd = (i >> 23) & 7;
			uint32_t mask = 0xFu << (28 - 4 * crfd);
			uint32_t val = ((i >> 12) & 0xF) << (28 - 4 * crfd);
			c->fpscr = (c->fpscr & ~mask) | (val & mask);
			fpscr_summarize(c);
			break;
		}
		case 711: {                     /* mtfsf */
			unsigned flm = (i >> 17) & 0xFF;
			uint32_t mask = 0;
			for (unsigned k = 0; k < 8; k++)
				if (flm & (0x80u >> k))
					mask |= 0xFu << (28 - 4 * k);
			c->fpscr = (c->fpscr & ~mask) | ((uint32_t)b & mask);
			fpscr_summarize(c);
			break;
		}
		default:
			return false;
		}
		if (rc)
			set_cr1(c);
		return true;
	}

	/* A-form arithmetic (opcode 59: single precision, 63: double precision) */
	static const uint32_t valid59 = 1u << 18 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 24 | 1u << 25 |
					1u << 28 | 1u << 29 | 1u << 30 | 1u << 31;
	static const uint32_t valid63 = 1u << 18 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 23 | 1u << 25 |
					1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 | 1u << 31;
	if (!((op == 59 ? valid59 : valid63) & (1u << xo5)))
		return false;

	if (ppc_fp_fast_path && xo5 != 23 && fp_fast(c, op, xo5, f64(a), f64(b), f64(cc), &r)) {
		f[frt] = b64(r);
		if (rc)
			set_cr1(c);
		return true;
	}
	bool directed = directed_rounding(c->fpscr);
	feclearexcept(FE_ALL_EXCEPT);
	double da = barrier(f64(a)), db = barrier(f64(b)), dc = barrier(f64(cc));
	switch (xo5) {
	case 18: r = da / db; kind = OP_DIV; break;                             /* fdiv */
	case 20: r = da - db; kind = OP_SUB; break;                             /* fsub */
	case 21: r = da + db; kind = OP_ADD; break;                             /* fadd */
	case 22: r = sqrt(db); kind = OP_SQRT; break;                           /* fsqrt */
	case 23:                                                                /* fsel */
		f[frt] = (da >= 0.0) ? cc : b;
		nearest_rounding(directed);
		if (rc)
			set_cr1(c);
		return true;
	case 24: r = 1.0 / db; kind = OP_DIV; break;                            /* fres */
	case 25: r = da * dc; kind = OP_MUL; break;                             /* fmul */
	case 26: r = 1.0 / sqrt(db); kind = OP_SQRT; break;                     /* frsqrte */
	case 28: r = fma(da, dc, -db); kind = OP_FMA; break;                    /* fmsub */
	case 29: r = fma(da, dc, db); kind = OP_FMA; break;                     /* fmadd */
	case 30: r = -barrier(fma(da, dc, -db)); kind = OP_FMA; break;          /* fnmsub */
	case 31: r = -barrier(fma(da, dc, db)); kind = OP_FMA; break;           /* fnmadd */
	default:
		nearest_rounding(directed);
		return false;
	}
	if (op == 59 || xo5 == 24)
		r = (double)(float)barrier(r);
	r = barrier(r);
	f[frt] = b64(r);
	fp_finish(c, kind, a, b, cc, r);
	nearest_rounding(directed);
	if (rc)
		set_cr1(c);
	return true;
}
