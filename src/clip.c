#include "clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"

/* Mac OS Roman's upper half in Unicode (since Mac OS 8.5, 0xDB is the euro sign) */
static const uint16_t mac_roman[128] = {
	0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
	0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
	0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
	0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
	0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
	0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
	0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
	0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
	0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
	0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
	0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
	0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
	0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
	0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
	0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/* A base letter and a combining mark, precomposed (as far as Mac OS Roman has them) */
static const struct {
	uint16_t mark;
	const char *bases;
	uint16_t composed[11];
} compositions[] = {
	{ 0x0300, "AEIOUaeiou", { 0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9 } },
	{ 0x0301, "AEIOUaeiou", { 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xE1, 0xE9, 0xED, 0xF3, 0xFA } },
	{ 0x0302, "AEIOUaeiou", { 0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB } },
	{ 0x0303, "ANOano", { 0xC3, 0xD1, 0xD5, 0xE3, 0xF1, 0xF5 } },
	{ 0x0308, "AEIOUYaeiouy", { 0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0x178, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC } },
	{ 0x030A, "Aa", { 0xC5, 0xE5 } },
	{ 0x0327, "Cc", { 0xC7, 0xE7 } },
};

/* What stands in for some characters Mac OS Roman lacks ("" drops them; others become '?') */
static const struct {
	uint32_t cp;
	const char *mac;
} fallbacks[] = {
	{ 0x00A6, "|" }, { 0x00AD, "" }, { 0x00B2, "2" }, { 0x00B3, "3" }, { 0x00B9, "1" },
	{ 0x00BC, "1/4" }, { 0x00BD, "1/2" }, { 0x00BE, "3/4" }, { 0x00D0, "D" }, { 0x00D7, "x" },
	{ 0x00DD, "Y" }, { 0x00DE, "Th" }, { 0x00F0, "d" }, { 0x00FD, "y" }, { 0x00FE, "th" },
	{ 0x200B, "" }, { 0x200C, "" }, { 0x200D, "" }, { 0x2010, "-" }, { 0x2011, "-" },
	{ 0x2012, "\xD0" }, { 0x2015, "\xD1" }, { 0x201B, "\xD4" }, { 0x201F, "\xD2" }, { 0x2024, "." },
	{ 0x2025, ".." }, { 0x2032, "'" }, { 0x2033, "\"" }, { 0x2060, "" }, { 0x2190, "<-" },
	{ 0x2192, "->" }, { 0x2212, "-" }, { 0xFEFF, "" },
};

static size_t put_utf8(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* The code point at `s` (U+FFFD for a byte that doesn't start a well-formed one); its length in *n */
static uint32_t get_utf8(const unsigned char *s, size_t len, size_t *n)
{
	unsigned b = s[0], more;
	uint32_t cp;
	*n = 1;
	if (b < 0x80)
		return b;
	if (b >= 0xC2 && b <= 0xDF) {
		more = 1;
		cp = b & 0x1F;
	} else if (b >= 0xE0 && b <= 0xEF) {
		more = 2;
		cp = b & 0x0F;
	} else if (b >= 0xF0 && b <= 0xF4) {
		more = 3;
		cp = b & 0x07;
	} else {
		return 0xFFFD;
	}
	if (len <= more)
		return 0xFFFD;
	for (unsigned k = 1; k <= more; k++) {
		if ((s[k] & 0xC0) != 0x80)
			return 0xFFFD;
		cp = cp << 6 | (s[k] & 0x3F);
	}
	if ((more == 2 && cp < 0x800) || (more == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
	    (cp >= 0xD800 && cp <= 0xDFFF))
		return 0xFFFD;
	*n = more + 1;
	return cp;
}

static bool combining(uint32_t cp)
{
	return cp >= 0x0300 && cp <= 0x036F;
}

/* Mac OS Roman for a code point, or -1 */
static int to_mac(uint32_t cp)
{
	if (cp < 0x80)
		return (int)cp;
	for (unsigned k = 0; k < 128; k++)
		if (mac_roman[k] == cp)
			return (int)(0x80 + k);
	return -1;
}

static uint32_t compose(uint32_t base, uint32_t mark)
{
	for (size_t k = 0; base && base < 0x80 && k < sizeof compositions / sizeof compositions[0]; k++) {
		const char *b = compositions[k].mark == mark ? strchr(compositions[k].bases, (int)base) : NULL;
		if (b)
			return compositions[k].composed[b - compositions[k].bases];
	}
	return 0;
}

size_t clip_mac_to_utf8(const uint8_t *in, size_t len, char *out)
{
	size_t o = 0;
	for (size_t k = 0; k < len; k++) {
		uint8_t b = in[k];
		if (b == '\r') {
			out[o++] = '\n';
			if (k + 1 < len && in[k + 1] == '\n')
				k++;
		} else if (b) {         /* (a NUL would end the host's text) */
			o += put_utf8(out + o, b < 0x80 ? b : mac_roman[b - 0x80]);
		}
	}
	out[o] = '\0';
	return o;
}

size_t clip_utxt_to_utf8(const uint8_t *in, size_t len, char *out)
{
	size_t units = len / 2, o = 0, k = 0;
	bool little = false;
	if (units && (be16(in) == 0xFEFF || be16(in) == 0xFFFE)) {      /* a byte-order mark */
		little = be16(in) == 0xFFFE;
		k = 1;
	}
#define UNIT(i) (little ? (uint32_t)(in[2 * (i)] | in[2 * (i) + 1] << 8) : (uint32_t)be16(in + 2 * (i)))
	for (; k < units; k++) {
		uint32_t u = UNIT(k);
		if (u >= 0xD800 && u <= 0xDBFF && k + 1 < units && UNIT(k + 1) >= 0xDC00 && UNIT(k + 1) <= 0xDFFF) {
			u = 0x10000 + ((u - 0xD800) << 10) + (UNIT(k + 1) - 0xDC00);
			k++;
		} else if (u >= 0xD800 && u <= 0xDFFF) {
			u = 0xFFFD;
		} else if (u == '\r') {
			u = '\n';
			if (k + 1 < units && UNIT(k + 1) == '\n')
				k++;
		} else if (!u) {
			continue;
		}
		o += put_utf8(out + o, u);
	}
#undef UNIT
	out[o] = '\0';
	return o;
}

size_t clip_utf8_to_mac(const char *text, size_t len, uint8_t *out)
{
	const unsigned char *in = (const unsigned char *)text;
	size_t o = 0, n;
	for (size_t k = 0; k < len; k += n) {
		uint32_t cp = get_utf8(in + k, len - k, &n);
		if (cp == '\r' || cp == '\n' || cp == 0x2028 || cp == 0x2029) {
			out[o++] = '\r';
			if (cp == '\r' && k + 1 < len && in[k + 1] == '\n')
				n++;
			continue;
		}
		/* a letter and its accent, decomposed as on macOS: as one character */
		size_t m;
		uint32_t next = k + n < len ? get_utf8(in + k + n, len - k - n, &m) : 0;
		uint32_t composed = combining(next) ? compose(cp, next) : 0;
		if (composed && to_mac(composed) >= 0) {
			cp = composed;
			n += m;
		}
		int mac = to_mac(cp);
		if (mac >= 0) {
			out[o++] = (uint8_t)mac;
			continue;
		}
		if (combining(cp))              /* (an accent with nothing to go on) */
			continue;
		bool space = (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
		const char *s = space ? " " : "?";
		for (size_t f = 0; f < sizeof fallbacks / sizeof fallbacks[0]; f++)
			if (fallbacks[f].cp == cp)
				s = fallbacks[f].mac;
		while (*s)
			out[o++] = (uint8_t)*s++;
	}
	return o;
}

size_t clip_utf8_to_utxt(const char *text, size_t len, uint8_t *out)
{
	const unsigned char *in = (const unsigned char *)text;
	size_t o = 0, n;
	for (size_t k = 0; k < len; k += n) {
		uint32_t cp = get_utf8(in + k, len - k, &n);
		if (cp == '\r' || cp == '\n') {
			cp = '\r';
			if (in[k] == '\r' && k + 1 < len && in[k + 1] == '\n')
				n++;
		}
		if (cp >= 0x10000) {
			put_be16(out + o, (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)));
			put_be16(out + o + 2, (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)));
			o += 4;
		} else {
			put_be16(out + o, (uint16_t)cp);
			o += 2;
		}
	}
	return o;
}

static void forget_got(struct clip *c)
{
	free(c->got_text);
	free(c->got_utxt);
	c->got_text = c->got_utxt = NULL;
	c->got_text_len = c->got_utxt_len = 0;
	c->got = false;
}

static void forget_text(struct clip *c)
{
	free(c->text);
	free(c->utxt);
	c->text = c->utxt = NULL;
	c->text_len = c->utxt_len = 0;
	c->pending = false;
	c->serial++;
}

void clip_free(struct clip *c)
{
	forget_got(c);
	forget_text(c);
	free(c->host);
	free(c->put_from);
	c->host = c->put_from = NULL;
}

void clip_guest_reset(struct clip *c)
{
	forget_got(c);
	c->listening = false;
	free(c->put_from);
	c->put_from = NULL;
	c->pending = c->text != NULL;
}

void clip_host_text(struct clip *c, const char *utf8)
{
	if (!utf8 || !utf8[0] || (c->host && strcmp(c->host, utf8) == 0))
		return;
	size_t len = strlen(utf8);
	char *host = strdup(utf8);
	if (!host)
		return;
	free(c->host);
	c->host = host;
	forget_text(c);
	if (len > CLIP_MAX_BYTES) {
		printf("clipboard: the host's text (%zu KB) is more than Mac OS is given (%u KB)\n", (len + 1023) >> 10,
		       CLIP_MAX_BYTES >> 10);
		return;
	}
	c->text = malloc(2 * len);
	c->utxt = malloc(2 * len);
	if (!c->text || !c->utxt) {
		forget_text(c);
		return;
	}
	c->text_len = clip_utf8_to_mac(host, len, c->text);
	c->utxt_len = clip_utf8_to_utxt(host, len, c->utxt);
	c->pending = true;
}

void clip_mac_zero(struct clip *c)
{
	forget_got(c);
}

void clip_mac_put(struct clip *c, uint32_t type, const uint8_t *data, size_t len)
{
	if (type != CLIP_TEXT && type != CLIP_UTXT)
		return;
	if (len > CLIP_MAX_BYTES) {
		printf("clipboard: Mac OS's text (%zu KB) is more than the host is given (%u KB)\n", (len + 1023) >> 10,
		       CLIP_MAX_BYTES >> 10);
		return;
	}
	uint8_t *copy = malloc(len ? len : 1);
	if (!copy)
		return;
	memcpy(copy, data, len);
	if (type == CLIP_TEXT) {
		free(c->got_text);
		c->got_text = copy;
		c->got_text_len = len;
	} else {
		free(c->got_utxt);
		c->got_utxt = copy;
		c->got_utxt_len = len;
	}
	c->got = true;
}

void clip_delivered(struct clip *c)
{
	c->pending = false;
	free(c->put_from);
	c->put_from = c->host ? strdup(c->host) : NULL;
	c->to_mac++;
}

static bool same(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
	return a && b && alen == blen && memcmp(a, b, alen) == 0;
}

const char *clip_take(struct clip *c)
{
	if (!c->got)
		return NULL;
	c->got = false;
	/* the text we put there, handed back (from a private scrap, at a suspend), while the host still has it */
	if (c->put_from && c->host && strcmp(c->put_from, c->host) == 0 &&
	    (c->got_text ? same(c->got_text, c->got_text_len, c->text, c->text_len)
			 : same(c->got_utxt, c->got_utxt_len, c->utxt, c->utxt_len)))
		return NULL;
	char *s = c->got_utxt ? malloc(3 * (c->got_utxt_len / 2) + 1) : malloc(3 * c->got_text_len + 1);
	if (!s)
		return NULL;
	if (c->got_utxt)
		clip_utxt_to_utf8(c->got_utxt, c->got_utxt_len, s);
	else
		clip_mac_to_utf8(c->got_text, c->got_text_len, s);
	if (!s[0] || (c->host && strcmp(s, c->host) == 0)) {
		free(s);
		return NULL;
	}
	free(c->host);
	c->host = s;
	forget_text(c);
	c->to_host++;
	return c->host;
}

bool clip_telling(const struct clip *c)
{
	return c->listening && c->text && c->told != c->serial;
}

bool clip_hold_key(struct clip *c, uint8_t key, bool down, uint64_t now_ns)
{
	if ((!c->nheld && !clip_telling(c)) || c->nheld == CLIP_HELD_KEYS)
		return false;
	if (!c->nheld)
		c->hold_from = now_ns;
	c->held[c->nheld].key = key;
	c->held[c->nheld].down = down;
	c->nheld++;
	return true;
}

bool clip_held_key(struct clip *c, uint64_t now_ns, uint8_t *key, bool *down)
{
	if (!c->nheld || (clip_telling(c) && now_ns - c->hold_from < CLIP_HOLD_NS))
		return false;
	*key = c->held[0].key;
	*down = c->held[0].down;
	memmove(c->held, c->held + 1, --c->nheld * sizeof c->held[0]);
	return true;
}
