/*
 * The clipboard shared by Mac OS and the host, for text. Blue Enhancements
 * (tools.c) patches the Scrap Manager so that what applications put in the
 * scrap comes here, and the host's text goes into the scrap of the front
 * application when it next looks at it. This is the part in between: the
 * conversions between the host's UTF-8 and Mac OS's 'TEXT' (Mac OS Roman,
 * CR line ends) and 'utxt' (UTF-16, big-endian, CR line ends), and which
 * side's text is newer, so that neither side gets its own text back.
 */
#ifndef BLUE_CLIP_H
#define BLUE_CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLIP_MAX_BYTES (4u << 20)       /* text passed either way, at most (bigger is left alone) */
#define CLIP_HOLD_NS 500000000u         /* keys wait at most this long (see clip_hold_key()) */
#define CLIP_HELD_KEYS 64

#define CLIP_TEXT 0x54455854u           /* 'TEXT' */
#define CLIP_UTXT 0x75747874u           /* 'utxt' */

struct clip {
	bool enabled;                   /* the guest's scrap is kept in step with a host clipboard */
	bool listening;                 /* ... by Blue Enhancements' patches, which are in place */
	char *host;                     /* the host clipboard's text as last seen or set (UTF-8), or NULL */
	/* host to Mac OS: the host's text converted, for the front application's scrap */
	bool pending;                   /* ... not yet put there */
	uint8_t *text, *utxt;
	size_t text_len, utxt_len;
	unsigned long serial;           /* (changes with them) */
	unsigned long told;             /* the text the front application was last told of (a resume event) */
	char *put_from;                 /* the host text last put there (NULL: none) */
	/* Mac OS to host: what applications have put in the scrap since it was last cleared */
	uint8_t *got_text, *got_utxt;
	size_t got_text_len, got_utxt_len;
	bool got;                       /* ... and some of it is new */
	struct { uint8_t key; bool down; } held[CLIP_HELD_KEYS];
	unsigned nheld;
	uint64_t hold_from;
	unsigned long to_mac, to_host;
};

/* Conversions; `out` must hold what the comment says. Each returns the length written. */
size_t clip_mac_to_utf8(const uint8_t *in, size_t len, char *out);     /* 3 * len + 1 (NUL-terminated) */
size_t clip_utxt_to_utf8(const uint8_t *in, size_t len, char *out);    /* 3 * (len / 2) + 1 (NUL-terminated) */
size_t clip_utf8_to_mac(const char *in, size_t len, uint8_t *out);     /* 2 * len */
size_t clip_utf8_to_utxt(const char *in, size_t len, uint8_t *out);    /* 2 * len */

void clip_free(struct clip *c);

/* Mac OS started again: its scrap no longer holds what was put there. */
void clip_guest_reset(struct clip *c);

/* The host clipboard holds this text (NULL or empty: none): if it's new, it goes to Mac OS. */
void clip_host_text(struct clip *c, const char *utf8);

/* The guest's side: ZeroScrap, and PutScrap of 'TEXT' or 'utxt' (other types are ignored) */
void clip_mac_zero(struct clip *c);
void clip_mac_put(struct clip *c, uint32_t type, const uint8_t *data, size_t len);

/* The host text has been put in the front application's scrap */
void clip_delivered(struct clip *c);

/* What Mac OS has put in the scrap, for the host clipboard, if it's new (until the next call); else NULL */
const char *clip_take(struct clip *c);

/*
 * Whether Mac OS's front application has yet to be told that the host has
 * new text: applications that keep a private scrap (TextEdit's) take in
 * the scrap only on a resume event that says it has changed, which Blue
 * Enhancements posts when it next looks (within a sixth of a second). Keys
 * typed in the window meanwhile (Command-V) wait for that, at most
 * CLIP_HOLD_NS: clip_hold_key() is false if a key can go to the guest now,
 * and clip_held_key() gives back those that waited, once they can.
 */
bool clip_telling(const struct clip *c);
bool clip_hold_key(struct clip *c, uint8_t key, bool down, uint64_t now_ns);
bool clip_held_key(struct clip *c, uint64_t now_ns, uint8_t *key, bool *down);

#endif
