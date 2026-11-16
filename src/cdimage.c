#include "cdimage_int.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audiodec.h"
#include "be.h"
#include "dmg.h"
#include "floppy.h"

#define MAX_CUE (1u << 20)              /* a cue sheet's bytes */
#define BUF_SECTORS 32u
#define SESSION_GAP_FIRST 11400u        /* after session 1: its lead-out, the next lead-in and pregap */
#define SESSION_GAP 6900u               /* ... after later sessions */

void cd_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
	uint32_t n = lba + CD_MSF_OFFSET;
	*m = (uint8_t)(n / (60 * CD_SECTORS_PER_SECOND));
	*s = (uint8_t)(n / CD_SECTORS_PER_SECOND % 60);
	*f = (uint8_t)(n % CD_SECTORS_PER_SECOND);
}

uint32_t cd_lba(unsigned m, unsigned s, unsigned f)
{
	uint32_t n = (m * 60 + s) * CD_SECTORS_PER_SECOND + f;
	return n > CD_MSF_OFFSET ? n - CD_MSF_OFFSET : 0;
}

bool cdimage_is_cue(const char *path)
{
	size_t n = strlen(path);
	return n > 4 && strcasecmp(path + n - 4, ".cue") == 0;
}

static struct cdimage *fail(struct cdimage *c, char *why, size_t len, const char *fmt, const char *arg)
{
	snprintf(why, len, fmt, arg);
	cdimage_close(c);
	return NULL;
}

void cdimage_close(struct cdimage *c)
{
	if (!c)
		return;
	for (unsigned k = 0; k < c->nfiles; k++) {
		if (c->files[k].fd >= 0)
			close(c->files[k].fd);
		if (c->files[k].close)
			c->files[k].close(c->files[k].dec);
		free(c->files[k].path);
	}
	free(c->files);
	free(c->buf);
	free(c->path);
	free(c);
}

static struct cdimage *new_image(const char *path)
{
	struct cdimage *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	c->path = strdup(path);
	c->files = calloc(CD_MAX_TRACKS, sizeof *c->files);
	c->buf = malloc(BUF_SECTORS * CD_MAX_STRIDE);
	c->data = -1;
	if (!c->path || !c->files || !c->buf) {
		cdimage_close(c);
		return NULL;
	}
	return c;
}

/* ---- the files a cue sheet names -------------------------------------------- */

/* A file named in a cue sheet: beside it, else by its last component, else the same name in another case */
static int open_beside(const char *cue, const char *name, char **found)
{
	const char *slash = strrchr(cue, '/');
	size_t dir = slash ? (size_t)(slash - cue + 1) : 0;
	const char *base = name;
	for (const char *p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	const char *tries[2] = { name, base };
	char path[4096];
	for (int t = 0; t < 2; t++) {
		if (tries[t][0] == '/')
			snprintf(path, sizeof path, "%s", tries[t]);
		else
			snprintf(path, sizeof path, "%.*s%s", (int)dir, cue, tries[t]);
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			*found = strdup(path);
			return fd;
		}
	}
	snprintf(path, sizeof path, "%.*s", dir ? (int)dir : 1, dir ? cue : ".");
	DIR *d = opendir(path);
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (strcasecmp(e->d_name, base) == 0) {
			snprintf(path, sizeof path, "%.*s%s", (int)dir, cue, e->d_name);
			closedir(d);
			d = NULL;
			int fd = open(path, O_RDONLY);
			if (fd >= 0)
				*found = strdup(path);
			return fd;
		}
	if (d)
		closedir(d);
	return -1;
}

static bool read_at(int fd, void *buf, size_t len, uint64_t at)
{
	return pread(fd, buf, len, (off_t)at) == (ssize_t)len;
}

bool cdimage_file_read(const struct cd_file *f, void *buf, size_t len, uint64_t at)
{
	return f->read ? !f->read(f->dec, buf, len, at) : read_at(f->fd, buf, len, at);
}

int cdimage_add_file(struct cdimage *c, const char *name, char *why, size_t len)
{
	if (c->nfiles >= CD_MAX_TRACKS) {
		snprintf(why, len, "too many files");
		return -1;
	}
	struct cd_file *f = &c->files[c->nfiles];
	struct stat st;
	errno = 0;
	f->fd = open_beside(c->path, name, &f->path);
	if (f->fd < 0) {
		const char *slash = strrchr(c->path, '/');
		snprintf(why, len, "%s (named in %s): %s", name, slash ? slash + 1 : c->path, strerror(errno ? errno : ENOENT));
		return -1;
	}
	c->nfiles++;
	for (unsigned k = 0; k + 1 < c->nfiles; k++)    /* (named again: the one already open) */
		if (c->files[k].path && !strcmp(c->files[k].path, f->path)) {
			close(f->fd);
			free(f->path);
			memset(f, 0, sizeof *f);
			c->nfiles--;
			return (int)k;
		}
	if (fstat(f->fd, &st)) {
		snprintf(why, len, "%s: %s", name, strerror(errno));
		return -1;
	}
	f->size = (uint64_t)st.st_size;
	return (int)c->nfiles - 1;
}

/* A WAVE file's samples: 44.1 kHz 16-bit stereo PCM */
static const char *wave_samples(struct cd_file *f, uint64_t file_size)
{
	uint8_t h[12], ch[8], fmt[40];
	if (!read_at(f->fd, h, 12, 0) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		return "not a WAVE file";
	bool have_fmt = false;
	for (uint64_t at = 12; at + 8 <= file_size;) {
		if (!read_at(f->fd, ch, 8, at))
			break;
		uint64_t n = le32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			if (n < 16 || !read_at(f->fd, fmt, n < sizeof fmt ? (size_t)n : sizeof fmt, at + 8))
				return "a damaged WAVE file";
			unsigned format = fmt[0] | fmt[1] << 8;
			if (format == 0xFFFE && n >= 26)
				format = fmt[24] | fmt[25] << 8;        /* WAVE_FORMAT_EXTENSIBLE: its subformat */
			if (format != 1 || (fmt[2] | fmt[3] << 8) != 2 || le32(fmt + 4) != CD_RATE ||
			    (fmt[14] | fmt[15] << 8) != 16)
				return "a WAVE file that isn't 44.1 kHz 16-bit stereo PCM";
			have_fmt = true;
		} else if (!memcmp(ch, "data", 4) && have_fmt) {
			f->offset = at + 8;
			f->size = n < file_size - f->offset ? n : file_size - f->offset;
			return NULL;
		}
		at += 8 + n + (n & 1);
	}
	return "a WAVE file without its samples";
}

/* An AIFF (or uncompressed AIFF-C) file's samples: 44.1 kHz 16-bit stereo */
static const char *aiff_samples(struct cd_file *f, uint64_t file_size)
{
	static const uint8_t rate44100[10] = { 0x40, 0x0E, 0xAC, 0x44 };
	uint8_t h[12], ch[8], comm[22], ssnd[8];
	if (!read_at(f->fd, h, 12, 0) || memcmp(h, "FORM", 4) || (memcmp(h + 8, "AIFF", 4) && memcmp(h + 8, "AIFC", 4)))
		return "not an AIFF file";
	bool aifc = !memcmp(h + 8, "AIFC", 4), have_comm = false;
	f->swap = true;
	for (uint64_t at = 12; at + 8 <= file_size;) {
		if (!read_at(f->fd, ch, 8, at))
			break;
		uint64_t n = be32(ch + 4);
		if (!memcmp(ch, "COMM", 4)) {
			if (n < 18 || !read_at(f->fd, comm, n < sizeof comm ? (size_t)n : sizeof comm, at + 8))
				return "a damaged AIFF file";
			if (be16(comm) != 2 || be16(comm + 6) != 16 || memcmp(comm + 8, rate44100, 10))
				return "an AIFF file that isn't 44.1 kHz 16-bit stereo";
			if (aifc && n >= 22) {
				if (!memcmp(comm + 18, "sowt", 4))
					f->swap = false;
				else if (memcmp(comm + 18, "NONE", 4) && memcmp(comm + 18, "twos", 4))
					return "a compressed AIFF-C file";
			}
			have_comm = true;
		} else if (!memcmp(ch, "SSND", 4) && have_comm) {
			if (n < 8 || !read_at(f->fd, ssnd, 8, at + 8))
				return "a damaged AIFF file";
			f->offset = at + 16 + be32(ssnd);
			if (f->offset > file_size)
				return "a damaged AIFF file";
			f->size = n - 8 < file_size - f->offset ? n - 8 : file_size - f->offset;
			return NULL;
		}
		at += 8 + n + (n & 1);
	}
	return "an AIFF file without its samples";
}

/* ---- cue sheets --------------------------------------------------------------- */

/* A track as the cue sheet gives it (frames are sectors from the start of its file) */
struct cue_track {
	unsigned number, file, session;
	uint8_t mode, control;
	unsigned sector_size;
	int64_t index0, index1;         /* -1: none */
	uint32_t pregap, postgap;       /* silence (not in the file) */
	char isrc[13];
};

struct cue_parse {
	struct cue_track t[CD_MAX_TRACKS];
	unsigned tracks;
	char why[256];
};

/* The next word of a line, or a "quoted string"; NULL at its end */
static char *word(char **line)
{
	char *p = *line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return NULL;
	char *w = p;
	if (*p == '"') {
		w = ++p;
		while (*p && *p != '"')
			p++;
	} else {
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	if (*p)
		*p++ = 0;
	*line = p;
	return w;
}

static bool msf_frames(const char *s, uint32_t *frames)
{
	unsigned m, sec, f;
	char end;
	if (!s || sscanf(s, "%u:%u:%u%c", &m, &sec, &f, &end) != 3 || sec >= 60 || f >= CD_SECTORS_PER_SECOND)
		return false;
	*frames = (m * 60 + sec) * CD_SECTORS_PER_SECOND + f;
	return true;
}

static bool track_type(struct cue_track *t, const char *type)
{
	static const struct { const char *name; uint8_t mode; unsigned size; } types[] = {
		{ "AUDIO", 0, CD_SECTOR },
		{ "MODE1/2048", 1, 2048 }, { "MODE1/2352", 1, CD_SECTOR },
		{ "MODE2/2048", 2, 2048 }, { "MODE2/2336", 2, 2336 }, { "MODE2/2352", 2, CD_SECTOR },
		{ "CDI/2336", 2, 2336 }, { "CDI/2352", 2, CD_SECTOR },
	};
	for (size_t k = 0; type && k < sizeof types / sizeof types[0]; k++)
		if (strcasecmp(type, types[k].name) == 0) {
			t->mode = types[k].mode;
			t->sector_size = types[k].size;
			t->control = t->mode ? CD_CONTROL_DATA : 0;
			return true;
		}
	return false;
}

static int parse_error(struct cue_parse *p, unsigned line, const char *what)
{
	snprintf(p->why, sizeof p->why, "line %u of the cue sheet: %s", line, what);
	return -1;
}

static void close_audio(void *a)
{
	audiodec_close(a);
}

static int open_file(struct cdimage *c, struct cue_parse *p, unsigned line, const char *name, const char *type)
{
	if (c->nfiles >= CD_MAX_TRACKS)
		return parse_error(p, line, "too many files");
	if (!name || !type)
		return parse_error(p, line, "FILE needs a name and a type");
	struct cd_file *f = &c->files[c->nfiles];
	errno = 0;
	f->fd = open_beside(c->path, name, &f->path);
	if (f->fd < 0) {
		snprintf(p->why, sizeof p->why, "%s (named in the cue sheet): %s", name, strerror(errno ? errno : ENOENT));
		return -1;
	}
	c->nfiles++;
	struct stat st;
	if (fstat(f->fd, &st))
		return parse_error(p, line, "cannot read the file");
	f->size = (uint64_t)st.st_size;
	const char *err = NULL;
	char e[200];
	uint8_t h[12] = { 0 };
	if (strcasecmp(type, "MOTOROLA") == 0) {
		f->swap = true;
	} else if (strcasecmp(type, "BINARY") != 0) {   /* WAVE, AIFF, MP3, FLAC, ...: by what's in it */
		struct audiodec *a = NULL;
		int rc;
		f->audio = true;
		if (!read_at(f->fd, h, sizeof h, 0))
			err = "cannot read it";
		else if (!memcmp(h, "RIFF", 4))
			err = wave_samples(f, f->size);
		else if (!memcmp(h, "FORM", 4))
			err = aiff_samples(f, f->size);
		else if ((rc = audiodec_open(f->path, &a, e, sizeof e)) < 0)
			err = e;
		else if (rc == 0)
			err = !strcasecmp(type, "WAVE") ? "not a WAVE file" : !strcasecmp(type, "AIFF") ? "not an AIFF file" :
			      "not audio Blue reads (WAVE, AIFF, FLAC, MP3 or Ogg Vorbis)";
		else {
			f->dec = a;
			f->read = audiodec_read;
			f->close = close_audio;
			f->size = audiodec_size(a);
		}
	}
	if (err) {
		snprintf(p->why, sizeof p->why, "%s: %s", name, err);
		return -1;
	}
	return 0;
}

static int parse_cue(struct cdimage *c, struct cue_parse *p, char *text)
{
	struct cue_track *t = NULL;
	unsigned session = 1, line = 0;
	for (char *next = text; next && *next;) {
		char *s = next;
		next = strpbrk(s, "\r\n");
		if (next) {
			while (*next == '\r' || *next == '\n')
				line += *next == '\n', *next++ = 0;
		}
		line++;
		if (line == 1 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
			s += 3;                 /* (a UTF-8 byte order mark) */
		char *cmd = word(&s);
		if (!cmd)
			continue;
		if (strcasecmp(cmd, "FILE") == 0) {
			char *name = word(&s);
			if (open_file(c, p, line, name, word(&s)))
				return -1;
		} else if (strcasecmp(cmd, "TRACK") == 0) {
			char *num = word(&s), *type = word(&s);
			if (!c->nfiles)
				return parse_error(p, line, "TRACK before FILE");
			if (p->tracks >= CD_MAX_TRACKS)
				return parse_error(p, line, "more than 99 tracks");
			t = &p->t[p->tracks];
			memset(t, 0, sizeof *t);
			t->number = num ? (unsigned)strtoul(num, NULL, 10) : 0;
			if (t->number < 1 || t->number > 99 || (p->tracks && t->number <= p->t[p->tracks - 1].number))
				return parse_error(p, line, "track numbers go up from 1 to 99");
			if (!track_type(t, type))
				return parse_error(p, line, "a track type that isn't read here (AUDIO, MODE1/2048, MODE1/2352, "
						   "MODE2/2336, MODE2/2352 are)");
			t->file = c->nfiles - 1;
			t->session = session;
			t->index0 = t->index1 = -1;
			p->tracks++;
		} else if (strcasecmp(cmd, "INDEX") == 0) {
			char *num = word(&s);
			uint32_t at;
			if (!t || !num || !msf_frames(word(&s), &at))
				return parse_error(p, line, "INDEX needs a track, a number and mm:ss:ff");
			unsigned n = (unsigned)strtoul(num, NULL, 10);
			if (n == 0)
				t->index0 = at;
			else if (n == 1)
				t->index1 = at;
		} else if (strcasecmp(cmd, "PREGAP") == 0 || strcasecmp(cmd, "POSTGAP") == 0) {
			uint32_t n;
			if (!t || !msf_frames(word(&s), &n))
				return parse_error(p, line, "PREGAP and POSTGAP need a track and mm:ss:ff");
			*(toupper((unsigned char)cmd[1]) == 'R' ? &t->pregap : &t->postgap) = n;
		} else if (strcasecmp(cmd, "FLAGS") == 0 && t) {
			for (char *f; (f = word(&s));) {
				if (strcasecmp(f, "DCP") == 0)
					t->control |= CD_CONTROL_COPY;
				else if (strcasecmp(f, "4CH") == 0)
					t->control |= CD_CONTROL_FOUR;
				else if (strcasecmp(f, "PRE") == 0)
					t->control |= CD_CONTROL_PREEMPHASIS;
			}
		} else if (strcasecmp(cmd, "ISRC") == 0 && t) {
			char *isrc = word(&s);
			snprintf(t->isrc, sizeof t->isrc, "%s", isrc ? isrc : "");
		} else if (strcasecmp(cmd, "CATALOG") == 0) {
			char *n = word(&s);
			snprintf(c->catalog, sizeof c->catalog, "%s", n ? n : "");
		} else if (strcasecmp(cmd, "REM") == 0) {
			char *what = word(&s);
			if (what && strcasecmp(what, "SESSION") == 0) {
				char *n = word(&s);
				unsigned v = n ? (unsigned)strtoul(n, NULL, 10) : 0;
				if (v < session || v > 99)
					return parse_error(p, line, "sessions go up from 1");
				session = v;
			}
		}
		/* (TITLE, PERFORMER, SONGWRITER, CDTEXTFILE and the like don't matter to a drive) */
	}
	if (!p->tracks)
		return parse_error(p, line, "no tracks");
	for (unsigned k = 0; k < p->tracks; k++)
		if (p->t[k].index1 < 0 || (p->t[k].index0 >= 0 && p->t[k].index0 > p->t[k].index1)) {
			snprintf(p->why, sizeof p->why, "track %u has no INDEX 01 (or its INDEX 00 is after it)", p->t[k].number);
			return -1;
		}
	return 0;
}

/*
 * Lays the tracks out on the disc: each file's sectors follow the last
 * file's, with the silence of PREGAP and POSTGAP and the gaps between
 * sessions put in; then everything moves so that track 1 starts at LBA 0.
 */
static int layout(struct cdimage *c, struct cue_parse *p)
{
	int64_t pregap[CD_MAX_TRACKS], start[CD_MAX_TRACKS], file_lba[CD_MAX_TRACKS], end[CD_MAX_TRACKS];
	int64_t disc = 0;               /* where the next file starts */
	unsigned session = 1;
	for (unsigned k = 0; k < p->tracks;) {
		unsigned file = p->t[k].file, last = k;
		while (last + 1 < p->tracks && p->t[last + 1].file == file)
			last++;
		const struct cd_file *f = &c->files[file];
		int64_t shift = 0;          /* silence and gaps put in so far, in this file */
		int64_t frame0 = 0;         /* the first frame of the last track, and its byte offset */
		uint64_t pos = 0;
		for (unsigned j = k; j <= last; j++) {
			const struct cue_track *t = &p->t[j];
			struct cd_track *d = &c->track[j];
			int64_t first = t->index0 >= 0 ? t->index0 : t->index1;
			if (f->audio && t->mode)
				return snprintf(p->why, sizeof p->why, "track %u: data in an audio file", t->number), -1;
			if (j > k && first < frame0)
				return snprintf(p->why, sizeof p->why, "track %u starts before the one before it",
						t->number), -1;
			pos = j > k ? pos + (uint64_t)(first - frame0) * p->t[j - 1].sector_size :
				      (uint64_t)first * t->sector_size;
			frame0 = first;
			if (t->session > session) {
				shift += session == 1 ? SESSION_GAP_FIRST : SESSION_GAP;
				session = t->session;
			}
			pregap[j] = disc + shift + first;       /* (PREGAP's silence first) */
			shift += t->pregap;
			file_lba[j] = disc + shift + first;
			start[j] = disc + shift + t->index1;
			int64_t frames;         /* its sectors in the file */
			if (j < last) {
				frames = (p->t[j + 1].index0 >= 0 ? p->t[j + 1].index0 : p->t[j + 1].index1) - first;
			} else {
				uint64_t left = f->size > pos ? f->size - pos : 0;
				frames = (int64_t)(t->mode ? left / t->sector_size : (left + t->sector_size - 1) / t->sector_size);
			}
			if (t->index1 > first + frames)
				return snprintf(p->why, sizeof p->why, "track %u: its INDEX 01 is past the end of its file",
						t->number), -1;
			end[j] = file_lba[j] + frames + t->postgap;
			shift += t->postgap;
			d->number = t->number;
			d->session = t->session;
			d->control = t->control;
			d->mode = t->mode;
			d->sector_size = t->sector_size;
			d->stride = t->sector_size;
			memcpy(d->isrc, t->isrc, sizeof d->isrc);
			d->file = (int)file;
			d->file_pos = pos;
			d->file_sectors = (uint32_t)frames;
		}
		disc = end[last];
		k = last + 1;
	}

	/* track 1 at LBA 0: what comes before it (a pregap in its file) isn't addressed */
	int64_t origin = start[0];
	for (unsigned j = 0; j < p->tracks; j++) {
		struct cd_track *d = &c->track[j];
		int64_t at = file_lba[j] - origin;
		if (at < 0) {
			uint32_t skip = -at < (int64_t)d->file_sectors ? (uint32_t)-at : d->file_sectors;
			d->file_pos += (uint64_t)skip * d->sector_size;
			d->file_sectors -= skip;
			at = 0;
		}
		if (end[j] - origin > 0xFFFFFF)
			return snprintf(p->why, sizeof p->why, "it's far too long for a CD"), -1;
		d->file_lba = (uint32_t)at;
		d->pregap = pregap[j] > origin ? (uint32_t)(pregap[j] - origin) : 0;
		d->start = (uint32_t)(start[j] - origin);
		d->end = (uint32_t)(end[j] - origin);
	}
	c->tracks = p->tracks;
	c->sessions = p->t[p->tracks - 1].session;
	c->leadout = c->track[c->tracks - 1].end;
	return 0;
}

static struct cdimage *open_cue(struct cdimage *c, char *why, size_t len)
{
	int fd = open(c->path, O_RDONLY);
	if (fd < 0)
		return fail(c, why, len, "%s", strerror(errno));
	struct stat st;
	if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > MAX_CUE) {
		close(fd);
		return fail(c, why, len, "%s", "not a cue sheet");
	}
	char *text = malloc((size_t)st.st_size + 1);
	struct cue_parse *p = calloc(1, sizeof *p);
	bool ok = text && p && read_at(fd, text, (size_t)st.st_size, 0);
	close(fd);
	if (ok) {
		text[st.st_size] = 0;
		ok = !parse_cue(c, p, text) && !layout(c, p);
		if (!ok)
			snprintf(why, len, "%s", p->why);
	} else {
		snprintf(why, len, "cannot read it");
	}
	free(text);
	free(p);
	if (!ok) {
		cdimage_close(c);
		return NULL;
	}
	return c;
}

static void close_dmg(void *d)
{
	dmg_close(d);
}

/* A plain image, or a compressed one (decoded as read): one data track of 2048-byte sectors */
static struct cdimage *open_plain(struct cdimage *c, char *why, size_t len)
{
	struct cd_file *f = &c->files[0];
	c->nfiles = 1;
	f->fd = -1;
	f->path = strdup(c->path);
	if (c->format == CDIMAGE_PACKED) {
		struct dmg *d;
		if (dmg_open(c->path, &d, why, len) != 1) {
			cdimage_close(c);
			return NULL;
		}
		f->dec = d;
		f->read = dmg_read;
		f->close = close_dmg;
		f->size = dmg_size(d);
		snprintf(c->kind, sizeof c->kind, "a %s", dmg_kind(d));
	} else {
		struct stat st;
		f->fd = open(c->path, O_RDONLY);
		if (f->fd < 0)
			return fail(c, why, len, "%s", strerror(errno));
		if (fstat(f->fd, &st))
			return fail(c, why, len, "%s", strerror(errno));
		const char *other = floppy_other_image(f->fd);  /* (an archive, raw sectors without their cue sheet, ...) */
		if (other)
			return fail(c, why, len, "%s", other);
		f->size = (uint64_t)st.st_size;
		snprintf(c->kind, sizeof c->kind, "a plain image");
	}
	if (f->size < CD_DATA)
		return fail(c, why, len, "%s", "not a CD image");
	struct cd_track *t = &c->track[0];
	t->number = 1;
	t->session = 1;
	t->control = CD_CONTROL_DATA;
	t->mode = 1;
	t->sector_size = t->stride = CD_DATA;
	t->file_sectors = (uint32_t)(f->size / CD_DATA);
	t->end = t->file_sectors;
	c->tracks = 1;
	c->sessions = 1;
	c->leadout = t->end;
	return c;
}

int cdimage_check(struct cdimage *c, char *why, size_t len)
{
	if (!c->tracks)
		return snprintf(why, len, "it has no tracks"), -1;
	for (unsigned k = 0; k < c->tracks; k++) {
		const struct cd_track *t = &c->track[k], *next = k + 1 < c->tracks ? t + 1 : NULL;
		if (t->number < 1 || t->number > 99 || (k && t->number <= t[-1].number) || t->pregap > t->start ||
		    t->start > t->end || (next && t->end > next->pregap) || t->end > 0xFFFFFF ||
		    (t->sector_size != CD_SECTOR && t->sector_size != CD_DATA && t->sector_size != 2336) ||
		    t->stride < t->sector_size || t->stride > CD_MAX_STRIDE || (!t->mode && t->sector_size != CD_SECTOR))
			return snprintf(why, len, "its table of contents is damaged (track %u)", t->number), -1;
		if (t->file >= 0) {
			const struct cd_file *f = &c->files[t->file];
			uint64_t need = t->file_sectors ? t->file_pos + (uint64_t)(t->file_sectors - 1) * t->stride +
							  t->sector_size : 0;
			if ((unsigned)t->file >= c->nfiles || t->file_lba < t->pregap ||
			    t->file_lba + (uint64_t)t->file_sectors > t->end)
				return snprintf(why, len, "its table of contents is damaged (track %u)", t->number), -1;
			if (need > f->size)
				return snprintf(why, len, "track %u is past the end of %s (cut short?)", t->number,
						strrchr(f->path, '/') ? strrchr(f->path, '/') + 1 : f->path), -1;
		}
	}
	c->leadout = c->track[c->tracks - 1].end;
	c->sessions = c->track[c->tracks - 1].session;
	return 0;
}

/* ---- what an image is --------------------------------------------------------- */

static bool cue_text(const uint8_t *h, size_t n)
{
	static const char *const words[] = { "FILE ", "REM ", "CATALOG ", "TITLE ", "PERFORMER ", "SONGWRITER ",
					     "CDTEXTFILE " };
	size_t k = n >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF ? 3 : 0;
	for (size_t j = 0; j < n; j++)
		if (h[j] < 0x20 && h[j] != '\r' && h[j] != '\n' && h[j] != '\t')
			return false;
	while (k < n && (h[k] == ' ' || h[k] == '\t' || h[k] == '\r' || h[k] == '\n'))
		k++;
	for (size_t w = 0; w < sizeof words / sizeof words[0]; w++)
		if (n - k >= strlen(words[w]) && !strncasecmp((const char *)h + k, words[w], strlen(words[w])))
			return true;
	return false;
}

/* A file beside `path` with the same name but for its extension `ext` (in either case): true, its path in `out` */
static bool sibling(const char *path, const char *ext, char *out, size_t len)
{
	const char *slash = strrchr(path, '/'), *base = slash ? slash + 1 : path, *dot = strrchr(base, '.');
	size_t stem = dot ? (size_t)(dot - base) : strlen(base);
	int dir = (int)(base - path);
	char dirname[4096];
	snprintf(dirname, sizeof dirname, "%.*s", dir ? dir : 1, dir ? path : ".");
	DIR *d = opendir(dirname);
	struct dirent *e;
	bool found = false;
	while (d && !found && (e = readdir(d)))
		if (strlen(e->d_name) == stem + 1 + strlen(ext) && !strncasecmp(e->d_name, base, stem) &&
		    e->d_name[stem] == '.' && !strcasecmp(e->d_name + stem + 1, ext)) {
			snprintf(out, len, "%.*s%s", dir, path, e->d_name);
			found = true;
		}
	if (d)
		closedir(d);
	return found;
}

int cdimage_detect(const char *path, char *open_path, char *why, size_t len)
{
	static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
	uint8_t h[64] = { 0 }, t[12] = { 0 };
	struct stat st;
	if (open_path)
		snprintf(open_path, 4096, "%s", path);
	int fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		snprintf(why, len, "%s", fd < 0 ? strerror(errno) : "not a file");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	uint64_t size = (uint64_t)st.st_size;
	ssize_t got = pread(fd, h, sizeof h, 0);
	if (size >= sizeof t && pread(fd, t, sizeof t, (off_t)(size - sizeof t)) != (ssize_t)sizeof t)
		memset(t, 0, sizeof t);
	close(fd);
	if (!memcmp(h, "MComprHD", 8))
		return CDIMAGE_CHD;
	if (!memcmp(h, "MEDIA DESCRIPTOR", 16))
		return CDIMAGE_MDS;
	if (!memcmp(t, "NER5", 4) || !memcmp(t + 4, "NERO", 4))
		return CDIMAGE_NRG;
	if (size <= MAX_CUE && got > 0) {
		size_t k = got >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF ? 3 : 0;
		if ((size_t)got - k >= 9 && !strncasecmp((const char *)h + k, "[CloneCD]", 9))
			return CDIMAGE_CCD;
		if (cdimage_is_cue(path) || cue_text(h, (size_t)got))
			return CDIMAGE_CUE;
		if ((size_t)got - k >= 5 && (!strncmp((const char *)h + k, "CD_DA", 5) ||
					     !strncmp((const char *)h + k, "CD_ROM", 6) || !strncmp((const char *)h + k, "CD_I", 4))) {
			char cue[4096];         /* (cdrdao's .toc: the .cue sheet that often comes with it) */
			if (sibling(path, "cue", cue, sizeof cue) && cdimage_detect(cue, NULL, why, len) == CDIMAGE_CUE) {
				if (open_path)
					snprintf(open_path, 4096, "%s", cue);
				return CDIMAGE_CUE;
			}
			snprintf(why, len, "a cdrdao table of contents (.toc), which Blue doesn't read: give it a .cue sheet");
			return -1;
		}
	}
	struct dmg *d;
	int rc = dmg_open(path, &d, why, len);
	if (rc < 0)
		return -1;
	if (rc > 0) {
		dmg_close(d);
		return CDIMAGE_PACKED;
	}
	/* the data of an image whose table of contents is in a file beside it */
	const char *dot = strrchr(path, '.');
	if (!memcmp(h, sync, sizeof sync) || (dot && (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".img") ||
						      !strcasecmp(dot, ".mdf") || !strcasecmp(dot, ".sub")))) {
		static const struct { const char *ext; int format; } desc[] = {
			{ "cue", CDIMAGE_CUE }, { "ccd", CDIMAGE_CCD }, { "mds", CDIMAGE_MDS },
		};
		char other[4096];
		for (size_t k = 0; k < sizeof desc / sizeof desc[0]; k++)
			if (sibling(path, desc[k].ext, other, sizeof other) && strcmp(other, path) &&
			    cdimage_detect(other, NULL, why, len) == desc[k].format) {
				if (open_path)
					snprintf(open_path, 4096, "%s", other);
				return desc[k].format;
			}
	}
	return CDIMAGE_PLAIN;
}

bool cdimage_has_toc(const char *path)
{
	char why[200];
	int format = cdimage_detect(path, NULL, why, sizeof why);
	return format >= CDIMAGE_CUE;
}

struct cdimage *cdimage_open(const char *path, char *why, size_t len)
{
	char target[4096];
	why[0] = 0;
	int format = cdimage_detect(path, target, why, len);
	if (format < 0)
		return NULL;
	struct cdimage *c = new_image(target);
	if (!c) {
		snprintf(why, len, "out of memory");
		return NULL;
	}
	c->format = (enum cdimage_format)format;
	int rc = 0;
	switch (c->format) {
	case CDIMAGE_PLAIN:
	case CDIMAGE_PACKED:
		c = open_plain(c, why, len);
		break;
	case CDIMAGE_CUE:
		snprintf(c->kind, sizeof c->kind, "a cue sheet");
		c = open_cue(c, why, len);
		break;
	case CDIMAGE_NRG:
		rc = cdimage_nrg(c, why, len);
		break;
	case CDIMAGE_MDS:
		rc = cdimage_mds(c, why, len);
		break;
	case CDIMAGE_CCD:
		rc = cdimage_ccd(c, why, len);
		break;
	case CDIMAGE_CHD:
		rc = cdimage_chd(c, why, len);
		break;
	}
	if (c && (rc || (c->format >= CDIMAGE_NRG && cdimage_check(c, why, len)))) {
		cdimage_close(c);
		c = NULL;
	}
	if (!c)
		return NULL;
	for (unsigned k = 0; k < c->tracks; k++)
		if (c->track[k].mode) {
			c->data = (int)k;
			c->data_size = (uint64_t)(c->track[k].end - c->track[k].start) * CD_DATA;
			break;
		}
	return c;
}

/* ---- reading ------------------------------------------------------------------- */

int cdimage_track_at(const struct cdimage *c, uint32_t lba)
{
	for (unsigned k = 0; k < c->tracks; k++) {
		if (lba < c->track[k].pregap)
			return -1;      /* (between sessions) */
		if (lba < c->track[k].end)
			return (int)k;
	}
	return -1;
}

bool cdimage_audio(const struct cdimage *c, uint32_t lba)
{
	int t = cdimage_track_at(c, lba);
	return t >= 0 && !c->track[t].mode;
}

/* Where sector `lba` of a track is in its file (false: it isn't) */
static bool in_file(const struct cd_track *t, uint32_t lba, uint64_t *at)
{
	if (t->file < 0 || lba < t->file_lba || lba - t->file_lba >= t->file_sectors)
		return false;
	*at = t->file_pos + (uint64_t)(lba - t->file_lba) * t->stride;
	return true;
}

/* How many sectors from `lba` on lie one after the other in the file (at most `max`) */
static unsigned run_in_file(const struct cd_track *t, uint32_t lba, unsigned max)
{
	uint32_t left = t->file_lba + t->file_sectors - lba;
	return left < max ? left : max;
}

static unsigned user_offset(const struct cd_track *t)
{
	if (t->sector_size == CD_SECTOR)
		return t->mode == 2 ? 24 : 16;
	return t->sector_size == 2336 ? 8 : 0;
}

int cdimage_read_data(void *ctx, void *out, size_t len, uint64_t at)
{
	const struct cdimage *c = ctx;
	uint8_t *buf = out;
	if (c->data < 0 && at <= (uint64_t)c->leadout * CD_DATA && len <= (uint64_t)c->leadout * CD_DATA - at) {
		memset(buf, 0, len);    /* (a disc of audio alone: no data, nothing but zeros) */
		return 0;
	}
	if (c->data < 0 || at > c->data_size || len > c->data_size - at)
		return -1;
	const struct cd_track *t = &c->track[c->data];
	const struct cd_file *f = &c->files[t->file >= 0 ? t->file : 0];
	unsigned skip = user_offset(t);
	while (len) {
		uint32_t lba = t->start + (uint32_t)(at / CD_DATA);
		unsigned off = (unsigned)(at % CD_DATA);
		uint64_t pos;
		if (!in_file(t, lba, &pos)) {
			size_t n = CD_DATA - off < len ? CD_DATA - off : len;
			memset(buf, 0, n);
			buf += n, at += n, len -= n;
			continue;
		}
		if (t->sector_size == CD_DATA && t->stride == CD_DATA) {        /* the data alone: as much as lies together */
			uint64_t room = (uint64_t)run_in_file(t, lba, UINT32_MAX) * CD_DATA - off;
			size_t n = room < len ? (size_t)room : len;
			if (!cdimage_file_read(f, buf, n, f->offset + pos + off))
				return -1;
			buf += n, at += n, len -= n;
			continue;
		}
		size_t want = (off + len + CD_DATA - 1) / CD_DATA;
		unsigned n = run_in_file(t, lba, want < BUF_SECTORS ? (unsigned)want : BUF_SECTORS);
		if (!cdimage_file_read(f, c->buf, (size_t)(n - 1) * t->stride + t->sector_size, f->offset + pos))
			return -1;
		for (unsigned k = 0; k < n && len; k++) {
			size_t m = CD_DATA - off < len ? CD_DATA - off : len;
			memcpy(buf, c->buf + (size_t)k * t->stride + skip + off, m);
			buf += m, at += m, len -= m;
			off = 0;
		}
	}
	return 0;
}

/* A data sector the file has only the data of, as a drive would read it raw: sync, header, data, zeros */
static void make_raw(const struct cd_track *t, uint32_t lba, uint8_t *s)
{
	unsigned at = t->mode == 1 || t->sector_size == 2336 ? 16 : 24;      /* (mode 2 form 1: after its subheader) */
	memmove(s + at, s, t->sector_size);
	memset(s, 0, at);
	memset(s + 1, 0xFF, 10);
	uint8_t m, sec, f;
	cd_msf(lba, &m, &sec, &f);
	s[12] = cd_bcd(m), s[13] = cd_bcd(sec), s[14] = cd_bcd(f);
	s[15] = t->mode;
	memset(s + at + t->sector_size, 0, CD_SECTOR - at - t->sector_size);
}

int cdimage_read_raw(const struct cdimage *c, uint32_t lba, unsigned count, uint8_t *buf)
{
	while (count) {
		int k = cdimage_track_at(c, lba);
		uint64_t pos;
		if (k < 0 || !in_file(&c->track[k], lba, &pos)) {
			memset(buf, 0, CD_SECTOR);
			buf += CD_SECTOR, lba++, count--;
			continue;
		}
		const struct cd_track *t = &c->track[k];
		const struct cd_file *f = &c->files[t->file];
		unsigned n = run_in_file(t, lba, count);
		if (t->sector_size == CD_SECTOR && t->stride == CD_SECTOR) {
			uint64_t room = f->size > pos ? f->size - pos : 0;      /* (a last sector cut short) */
			size_t want = (size_t)n * CD_SECTOR, got = room < want ? (size_t)room : want;
			if (got && !cdimage_file_read(f, buf, got, f->offset + pos))
				return -1;
			memset(buf + got, 0, want - got);
		} else {
			for (unsigned j = 0; j < n; j++) {
				uint8_t *s = buf + (size_t)j * CD_SECTOR;
				if (!cdimage_file_read(f, s, t->sector_size, f->offset + pos + (uint64_t)j * t->stride))
					return -1;
				if (t->sector_size != CD_SECTOR)
					make_raw(t, lba + j, s);
			}
		}
		if (f->swap && !t->mode)
			for (size_t b = 0; b + 1 < (size_t)n * CD_SECTOR; b += 2) {
				uint8_t x = buf[b];
				buf[b] = buf[b + 1];
				buf[b + 1] = x;
			}
		buf += (size_t)n * CD_SECTOR, lba += n, count -= n;
	}
	return 0;
}

/* ---- the subchannel -------------------------------------------------------------- */

void cdimage_q(const struct cdimage *c, uint32_t lba, uint8_t q[10])
{
	int k = cdimage_track_at(c, lba);
	const struct cd_track *t = &c->track[k >= 0 ? k : 0];
	uint32_t rel;
	uint8_t m, s, f;
	if (k < 0) {                    /* a lead-out: after the last track before it */
		for (unsigned j = 0; j < c->tracks && c->track[j].end <= lba; j++)
			t = &c->track[j];
		q[1] = 0xAA;
		q[2] = 1;
		rel = lba - t->end;
	} else {
		q[1] = cd_bcd(t->number);
		q[2] = lba < t->start ? 0 : 1;
		rel = lba < t->start ? t->start - lba : lba - t->start;
	}
	q[0] = (uint8_t)(t->control << 4 | 1);
	cd_msf(rel - CD_MSF_OFFSET, &m, &s, &f);        /* (no 2 s offset in the track's time) */
	q[3] = cd_bcd(m), q[4] = cd_bcd(s), q[5] = cd_bcd(f);
	q[6] = 0;
	cd_msf(lba, &m, &s, &f);
	q[7] = cd_bcd(m), q[8] = cd_bcd(s), q[9] = cd_bcd(f);
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t crc = 0;
	for (size_t k = 0; k < n; k++) {
		crc ^= (uint16_t)(p[k] << 8);
		for (int b = 0; b < 8; b++)
			crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

void cdimage_subcode(const struct cdimage *c, uint32_t lba, uint8_t sub[CD_SUBCODE])
{
	uint8_t q[12];
	cdimage_q(c, lba, q);
	put_be16(q + 10, (uint16_t)~crc16(q, 10));
	bool pause = q[2] == 0 && q[1] != 0xAA;         /* P: on in pregaps */
	for (unsigned k = 0; k < CD_SUBCODE; k++)
		sub[k] = (uint8_t)((pause ? 0x80 : 0) | ((q[k / 8] >> (7 - k % 8)) & 1) << 6);
}
