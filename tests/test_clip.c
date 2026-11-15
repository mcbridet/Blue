/*
 * The clipboard shared with Mac OS: conversions between UTF-8 and Mac OS
 * Roman ('TEXT') and UTF-16 ('utxt'), which side's text is newer (so that
 * neither gets its own back), and keys held while the guest hears of it.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/clip.h"
#include "test.h"

static size_t to_mac(const char *utf8, uint8_t *out)
{
	return clip_utf8_to_mac(utf8, strlen(utf8), out);
}

static bool mac_is(const char *utf8, const char *mac)
{
	uint8_t out[256];
	size_t n = to_mac(utf8, out);
	return n == strlen(mac) && memcmp(out, mac, n) == 0;
}

static bool utf8_is(const char *mac, const char *utf8)
{
	char out[3 * 256 + 1];
	size_t n = clip_mac_to_utf8((const uint8_t *)mac, strlen(mac), out);
	return n == strlen(utf8) && strcmp(out, utf8) == 0;
}

static void test_mac_roman(void)
{
	/* accents, curly quotes, ellipsis, dashes, the euro and the Apple logo, and back */
	CHECK(utf8_is("caf\x8E na\x95ve \xD2q\xD3 \xD4s\xD5 \xC9 \xD0\xD1 \xDB \xF0",
		      "café naïve “q” ‘s’ … –— € \xEF\xA3\xBF"));
	CHECK(mac_is("café naïve “q” ‘s’ … –— € \xEF\xA3\xBF",
		     "caf\x8E na\x95ve \xD2q\xD3 \xD4s\xD5 \xC9 \xD0\xD1 \xDB \xF0"));
	for (unsigned b = 1; b < 256; b++) {     /* every character goes there and back (but CR and LF, below) */
		uint8_t mac = (uint8_t)b, back[8];
		char utf8[8];
		size_t n = clip_mac_to_utf8(&mac, 1, utf8);
		if (b == '\r' || b == '\n')
			continue;
		CHECK(clip_utf8_to_mac(utf8, n, back) == 1 && back[0] == b);
	}
	/* line ends: CR on the Mac, LF on the host */
	CHECK(utf8_is("a\rb\r\nc\nd", "a\nb\nc\nd"));
	CHECK(mac_is("a\nb\r\nc\rd\xE2\x80\xA8" "e", "a\rb\rc\rd\re"));
	/* a NUL would end the host's text */
	uint8_t nul[3] = { 'a', 0, 'b' };
	char out[16];
	CHECK_EQ(clip_mac_to_utf8(nul, 3, out), 2);
	CHECK(strcmp(out, "ab") == 0);
}

static void test_precomposed(void)
{
	/* decomposed, as macOS often has it: precomposed */
	CHECK(mac_is("cafe\xCC\x81", "caf\x8E"));
	CHECK(mac_is("A\xCC\x8A" "E\xCC\x80" "n\xCC\x83" "Y\xCC\x88" "c\xCC\xA7", "\x81\xE9\x96\xD9\x8D"));
	/* an accent Mac OS Roman doesn't have on that letter, or on nothing: dropped */
	CHECK(mac_is("q\xCC\x81", "q"));
	CHECK(mac_is("\xCC\x81x", "x"));
	CHECK(mac_is("e\xCC\x81\xCC\x81", "\x8E"));
}

static void test_unmappable(void)
{
	CHECK(mac_is("½ → x", "1/2 -> x"));
	CHECK(mac_is("\xF0\x9F\x98\x80!", "?!"));                /* U+1F600 */
	CHECK(mac_is("\xE4\xB8\xAD", "?"));                       /* U+4E2D */
	CHECK(mac_is("a\xE2\x80\x8B" "b\xC2\xA0" "c\xE2\x80\x83" "d", "ab\xCA" "c d"));  /* ZWSP, NBSP, em space */
	CHECK(mac_is("x\xFFy\xC3", "x?y?"));                      /* not UTF-8 */
	CHECK(mac_is("\xED\xA0\x80", "???"));                     /* an encoded surrogate */
	CHECK(mac_is("\xE2\x80\x9B\xE2\x88\x92", "\xD4-"));       /* reversed quote, minus */
}

static void test_utxt(void)
{
	/* "A", U+1F600 (a surrogate pair), CR LF, "é" */
	static const uint8_t utxt[] = { 0, 'A', 0xD8, 0x3D, 0xDE, 0x00, 0, '\r', 0, '\n', 0, 0xE9 };
	char out[64];
	clip_utxt_to_utf8(utxt, sizeof utxt, out);
	CHECK(strcmp(out, "A\xF0\x9F\x98\x80\n\xC3\xA9") == 0);
	/* byte-order marks, and a high surrogate on its own */
	static const uint8_t bom[] = { 0xFE, 0xFF, 0, 'x', 0xD8, 0x00, 0, 'y' };
	clip_utxt_to_utf8(bom, sizeof bom, out);
	CHECK(strcmp(out, "x\xEF\xBF\xBDy") == 0);
	static const uint8_t little[] = { 0xFF, 0xFE, 'z', 0, 0xE9, 0 };
	clip_utxt_to_utf8(little, sizeof little, out);
	CHECK(strcmp(out, "z\xC3\xA9") == 0);
	/* and the other way, with CR line ends */
	uint8_t back[64];
	const char *text = "A\xF0\x9F\x98\x80\r\n\xC3\xA9\n";
	size_t n = clip_utf8_to_utxt(text, strlen(text), back);
	static const uint8_t want[] = { 0, 'A', 0xD8, 0x3D, 0xDE, 0x00, 0, '\r', 0, 0xE9, 0, '\r' };
	CHECK(n == sizeof want && memcmp(back, want, n) == 0);
}

static void put_text(struct clip *c, const char *mac)
{
	clip_mac_put(c, CLIP_TEXT, (const uint8_t *)mac, strlen(mac));
}

static void test_echo(void)
{
	struct clip c = { .enabled = true };

	/* the host's text goes to Mac OS once */
	clip_host_text(&c, "");
	CHECK(!c.pending);
	clip_host_text(&c, "caf\xC3\xA9 \xE2\x86\x92");          /* "café →" */
	CHECK(c.pending);
	CHECK(c.text_len == 7 && memcmp(c.text, "caf\x8E ->", 7) == 0);
	CHECK(c.utxt_len == 12);
	clip_delivered(&c);
	CHECK(!c.pending && c.to_mac == 1);
	clip_host_text(&c, "caf\xC3\xA9 \xE2\x86\x92");          /* (the window got the focus again) */
	CHECK(!c.pending);

	/* an application hands it back (its private scrap, at a suspend): not the host's again, lossy as it is */
	clip_mac_zero(&c);
	put_text(&c, "caf\x8E ->");
	CHECK(clip_take(&c) == NULL);
	CHECK(strcmp(c.host, "caf\xC3\xA9 \xE2\x86\x92") == 0);

	/* a copy in Mac OS goes to the host, and coming back from it is nothing new */
	clip_mac_zero(&c);
	put_text(&c, "new\rline");
	const char *t = clip_take(&c);
	CHECK(t && strcmp(t, "new\nline") == 0);
	CHECK(clip_take(&c) == NULL);
	clip_host_text(&c, "new\nline");
	CHECK(!c.pending && c.to_host == 1);

	/* the same text as was given to Mac OS, copied there after the host has moved on, is new to the host */
	clip_host_text(&c, "one");
	clip_delivered(&c);
	clip_host_text(&c, "two");
	clip_mac_zero(&c);
	put_text(&c, "one");
	t = clip_take(&c);
	CHECK(t && strcmp(t, "one") == 0);
	CHECK(!c.pending);                      /* (and "two" no longer waits for Mac OS) */

	/* 'utxt' rather than 'TEXT', when an application puts both; other types don't count */
	static const uint8_t utxt[] = { 0x21, 0x92, 0, '!' };   /* U+2192 */
	clip_mac_zero(&c);
	put_text(&c, "->!");
	clip_mac_put(&c, CLIP_UTXT, utxt, sizeof utxt);
	clip_mac_put(&c, 0x7374796Cu, (const uint8_t *)"xx", 2);        /* 'styl' */
	t = clip_take(&c);
	CHECK(t && strcmp(t, "\xE2\x86\x92!") == 0);
	clip_mac_zero(&c);
	CHECK(clip_take(&c) == NULL);
	put_text(&c, "");                       /* (nothing to give) */
	CHECK(clip_take(&c) == NULL);

	/* a restarted Mac OS has none of it: the host's text goes again */
	clip_host_text(&c, "again");
	clip_delivered(&c);
	clip_guest_reset(&c);
	CHECK(c.pending);
	clip_free(&c);
}

static void test_limit(void)
{
	struct clip c = { .enabled = true };
	char *big = malloc(CLIP_MAX_BYTES + 2);
	if (!big)
		return;
	memset(big, 'x', CLIP_MAX_BYTES + 1);
	big[CLIP_MAX_BYTES + 1] = '\0';
	clip_host_text(&c, big);                /* (says so) */
	CHECK(!c.pending && !c.text);
	big[CLIP_MAX_BYTES] = '\0';
	clip_host_text(&c, big);
	CHECK(c.pending && c.text_len == CLIP_MAX_BYTES);
	clip_mac_zero(&c);
	clip_mac_put(&c, CLIP_TEXT, (const uint8_t *)big, CLIP_MAX_BYTES + 1);
	CHECK(!c.got);
	free(big);
	clip_free(&c);
}

static void test_hold(void)
{
	struct clip c = { .enabled = true };
	uint8_t key;
	bool down;

	clip_host_text(&c, "text");
	CHECK(!clip_telling(&c));               /* (no Blue Enhancements to tell it) */
	CHECK(!clip_hold_key(&c, 9, true, 0));
	c.listening = true;
	CHECK(clip_telling(&c));
	CHECK(clip_hold_key(&c, 0x37, true, 1000));
	CHECK(clip_hold_key(&c, 9, true, 2000));
	CHECK(!clip_held_key(&c, 3000, &key, &down));
	c.told = c.serial;                      /* the front application has been told */
	CHECK(!clip_telling(&c));
	CHECK(clip_hold_key(&c, 9, false, 4000));        /* (behind those waiting) */
	CHECK(clip_held_key(&c, 5000, &key, &down) && key == 0x37 && down);
	CHECK(clip_held_key(&c, 5000, &key, &down) && key == 9 && down);
	CHECK(clip_held_key(&c, 5000, &key, &down) && key == 9 && !down);
	CHECK(!clip_held_key(&c, 5000, &key, &down));
	CHECK(!clip_hold_key(&c, 1, true, 6000));

	/* keys wait no longer than CLIP_HOLD_NS */
	clip_host_text(&c, "more");
	CHECK(clip_hold_key(&c, 2, true, 10000));
	CHECK(!clip_held_key(&c, 10000 + CLIP_HOLD_NS - 1, &key, &down));
	CHECK(clip_held_key(&c, 10000 + CLIP_HOLD_NS, &key, &down) && key == 2);
	clip_free(&c);
}

void test_clip(void)
{
	test_mac_roman();
	test_precomposed();
	test_unmappable();
	test_utxt();
	test_echo();
	test_limit();
	test_hold();
}
