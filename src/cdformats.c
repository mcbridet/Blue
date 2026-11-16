#include "cdimage_int.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "be.h"

#include "../third_party/libchdr/include/libchdr/chd.h"

#define MAX_DESCRIPTION (16u << 20)     /* the bytes of an image's description read in (Nero's chunks, an .mds) */
#define CHD_FRAME 2448u                 /* a CHD's frames: a sector and its subchannel */
#define CHD_PADDING 4u                  /* ... each track's padded to a multiple of 4 */
#define CHD_CACHE 4u                    /* hunks kept */

static int fail(char *why, size_t len, const char *what)
{
	snprintf(why, len, "%s", what);
	return -1;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint64_t le64(const uint8_t *p)
{
	return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* A track's sectors as a kind of sector (and the subchannel after each: `stride` more) */
static void set_sectors(struct cd_track *t, uint8_t mode, unsigned size, unsigned stride)
{
	t->mode = mode;
	t->sector_size = size;
	t->stride = stride;
	t->control = (uint8_t)((t->control & ~CD_CONTROL_DATA) | (mode ? CD_CONTROL_DATA : 0));
}

/*
 * Where a track's sectors are in its file, given where the sector at `lba`
 * is: from there on (or from as far before as the file holds, within the
 * track), at most `sectors` of them, up to its end.
 */
static void place(struct cd_track *t, int file, uint32_t lba, uint64_t pos, uint64_t sectors)
{
	t->file = file;
	t->file_lba = lba;
	t->file_pos = pos;
	if (sectors > t->end - lba)
		sectors = t->end - lba;
	t->file_sectors = (uint32_t)sectors;
}

/* The file whose name is `path`'s with another extension, found beside it */
static int add_beside(struct cdimage *c, const char *ext, char *why, size_t len)
{
	const char *base = base_name(c->path), *dot = strrchr(base, '.');
	char name[1024];
	snprintf(name, sizeof name, "%.*s.%s", dot ? (int)(dot - base) : (int)strlen(base), base, ext);
	return cdimage_add_file(c, name, why, len);
}

/* An image's description (the whole file, or `len` from `at`), read in */
static uint8_t *read_description(int fd, uint64_t at, uint64_t len)
{
	if (len > MAX_DESCRIPTION)
		return NULL;
	uint8_t *p = malloc((size_t)len + 1);
	if (p && pread(fd, p, (size_t)len, (off_t)at) != (ssize_t)len) {
		free(p);
		return NULL;
	}
	if (p)
		p[len] = 0;
	return p;
}

/*
 * Lays the tracks of a session out from the start of each (index 1) and of
 * its pregap (index 0), up to the session's lead-out: each ends where the
 * next one's pregap begins.
 */
static void ends(struct cdimage *c, unsigned first, uint32_t leadout)
{
	for (unsigned k = first; k < c->tracks; k++)
		c->track[k].end = k + 1 < c->tracks ? c->track[k + 1].pregap : leadout;
}

/* ---- Nero (.nrg) ---------------------------------------------------------------- */

/*
 * A Nero mode code (audio, mode 1 or mode 2) and the bytes each sector takes
 * in the file: the kind of sector (the data alone, raw, raw with its
 * subchannel after it)
 */
static bool nero_mode(unsigned code, unsigned bytes, uint8_t *mode, unsigned *size, unsigned *stride)
{
	static const uint8_t modes[] = { [0x00] = 1, [0x02] = 2, [0x03] = 2, [0x05] = 1, [0x06] = 2, [0x07] = 0,
					 [0x0F] = 1, [0x10] = 0, [0x11] = 2 };
	if (code >= sizeof modes || (code && !modes[code] && code != 0x07 && code != 0x10))
		return false;
	*mode = modes[code];
	*stride = bytes;
	*size = bytes == CD_MAX_STRIDE ? CD_SECTOR : bytes;
	return (*size == CD_SECTOR || (*mode && (*size == CD_DATA || *size == 2336)));
}

struct nrg_chunk {
	const uint8_t *data;
	uint32_t len;
};

/* A session written disc-at-once: its cue points (CUEX, or the old CUES) and its tracks' places (DAOX, DAOI) */
static int nrg_dao(struct cdimage *c, struct nrg_chunk cue, struct nrg_chunk dao, bool v2, unsigned session,
		   char *why, size_t len)
{
	unsigned block = v2 ? 42 : 30, first_track = c->tracks;
	if (dao.len < 22 || cue.len % 8)
		return fail(why, len, "its table of contents is damaged");
	if (!c->catalog[0] && dao.data[4]) {
		memcpy(c->catalog, dao.data + 4, 13);
		c->catalog[13] = 0;
	}
	int64_t leadout = -1;
	for (uint32_t k = 0; k + 22 + block <= dao.len && c->tracks < CD_MAX_TRACKS; k += block) {
		const uint8_t *b = dao.data + 22 + k;
		struct cd_track *t = &c->track[c->tracks];
		memset(t, 0, sizeof *t);
		t->number = dao.data[20] + k / block;
		t->session = session;
		uint8_t mode;
		unsigned size, stride;
		if (!nero_mode(b[14], be16(b + 12), &mode, &size, &stride)) {
			snprintf(why, len, "track %u is of a kind Blue doesn't read (Nero's mode 0x%02X, %u-byte sectors)",
				 t->number, b[14], be16(b + 12));
			return -1;
		}
		uint64_t pregap_at = v2 ? be64(b + 18) : be32(b + 18), start_at = v2 ? be64(b + 26) : be32(b + 22);
		uint64_t end_at = v2 ? be64(b + 34) : be32(b + 26);
		/* where index 0 and 1 are on the disc: the cue points */
		int64_t index[2] = { -1, -1 };
		uint8_t control = 0;
		for (uint32_t j = 0; j + 8 <= cue.len; j += 8) {
			const uint8_t *e = cue.data + j;
			int64_t lba = v2 ? (int32_t)be32(e + 4) :
				      (int64_t)((e[5] * 60 + e[6]) * CD_SECTORS_PER_SECOND + e[7]) - CD_MSF_OFFSET;
			if (e[1] == 0xAA)
				leadout = lba;
			else if (cd_unbcd(e[1]) == t->number && cd_unbcd(e[2]) <= 1) {
				index[cd_unbcd(e[2])] = lba;
				if (cd_unbcd(e[2]) == 1)
					control = e[0] >> 4;
			}
		}
		if (index[1] < 0 || end_at < start_at || start_at < pregap_at) {
			snprintf(why, len, "its table of contents is damaged (track %u)", t->number);
			return -1;
		}
		t->control = control;
		set_sectors(t, mode, size, stride);
		t->start = (uint32_t)index[1];
		t->pregap = index[0] >= 0 && index[0] < index[1] ? (uint32_t)index[0] : t->start;
		if (c->tracks > first_track && t->pregap < c->track[c->tracks - 1].start)
			t->pregap = t->start;
		/* (the file holds what's before index 1 from pregap_at on) */
		uint64_t before = (start_at - pregap_at) / stride;
		if (before > t->start - t->pregap)
			before = t->start - t->pregap;
		t->file = 0;
		t->file_lba = t->start - (uint32_t)before;
		t->file_pos = start_at - before * stride;
		t->file_sectors = (uint32_t)((end_at - t->file_pos) / stride);
		for (int n = 0; n < 12 && b[n] >= ' ' && b[n] < 0x7F; n++)
			t->isrc[n] = (char)b[n];
		c->tracks++;
	}
	if (c->tracks == first_track || leadout < 0)
		return fail(why, len, "its table of contents is damaged (no tracks, or no lead-out)");
	ends(c, first_track, (uint32_t)leadout);
	for (unsigned k = first_track; k < c->tracks; k++) {
		struct cd_track *t = &c->track[k];
		if (t->file_lba + (uint64_t)t->file_sectors > t->end)
			t->file_sectors = t->end > t->file_lba ? t->end - t->file_lba : 0;
	}
	return 0;
}

/* A session written track at once (ETN2, ETNF): each track's place and kind, a pregap of 2 s before each but the first */
static int nrg_tao(struct cdimage *c, struct nrg_chunk etn, bool v2, unsigned session, char *why, size_t len)
{
	unsigned entry = v2 ? 32 : 20, first_track = c->tracks;
	for (uint32_t k = 0; k + entry <= etn.len && c->tracks < CD_MAX_TRACKS; k += entry) {
		const uint8_t *e = etn.data + k;
		struct cd_track *t = &c->track[c->tracks];
		static const unsigned bytes_of[] = { [0x00] = CD_DATA, [0x02] = CD_DATA, [0x03] = 2336, [0x05] = CD_SECTOR,
						     [0x06] = CD_SECTOR, [0x07] = CD_SECTOR, [0x0F] = CD_MAX_STRIDE,
						     [0x10] = CD_MAX_STRIDE, [0x11] = CD_MAX_STRIDE };
		uint64_t at = v2 ? be64(e) : be32(e), bytes = v2 ? be64(e + 8) : be32(e + 4);
		unsigned code = v2 ? be32(e + 16) : be32(e + 8);
		uint32_t lba = v2 ? be32(e + 20) : be32(e + 12);
		uint8_t mode;
		unsigned size, stride;
		memset(t, 0, sizeof *t);
		t->number = c->tracks + 1;
		t->session = session;
		if (code >= sizeof bytes_of / sizeof bytes_of[0] || !bytes_of[code] ||
		    !nero_mode(code, bytes_of[code], &mode, &size, &stride)) {
			snprintf(why, len, "track %u is of a kind Blue doesn't read (Nero's mode 0x%02X)", t->number, code);
			return -1;
		}
		set_sectors(t, mode, size, stride);
		uint32_t after = c->tracks ? c->track[c->tracks - 1].end : 0;
		t->pregap = after;
		t->start = !c->tracks ? 0 : lba >= after ? lba : after + CD_MSF_OFFSET;
		t->end = t->start + (uint32_t)(bytes / stride);
		place(t, 0, t->start, at, bytes / stride);
		c->tracks++;
	}
	if (c->tracks == first_track)
		return fail(why, len, "its table of contents is damaged (no tracks)");
	return 0;
}

int cdimage_nrg(struct cdimage *c, char *why, size_t len)
{
	if (cdimage_add_file(c, base_name(c->path), why, len) < 0)
		return -1;
	const struct cd_file *f = &c->files[0];
	uint8_t tail[12];
	if (f->size < 12 || pread(f->fd, tail, 12, (off_t)(f->size - 12)) != 12)
		return fail(why, len, "cannot read it");
	bool v2 = !memcmp(tail, "NER5", 4);
	uint64_t at = v2 ? be64(tail + 4) : be32(tail + 8), end = f->size - (v2 ? 12 : 8);
	if (at >= end)
		return fail(why, len, "its table of contents is damaged");
	uint8_t *d = read_description(f->fd, at, end - at);
	if (!d)
		return fail(why, len, "cannot read its table of contents");
	struct nrg_chunk cues[CD_MAX_TRACKS], daos[CD_MAX_TRACKS], etns[CD_MAX_TRACKS];
	unsigned ncue = 0, ndao = 0, netn = 0;
	for (uint64_t k = 0; k + 8 <= end - at;) {
		const uint8_t *ch = d + k;
		uint32_t n = be32(ch + 4);
		if (n > end - at - k - 8)
			break;
		struct nrg_chunk chunk = { ch + 8, n };
		if (!memcmp(ch, "CUEX", 4) || !memcmp(ch, "CUES", 4)) {
			if (ncue < CD_MAX_TRACKS)
				cues[ncue++] = chunk;
		} else if (!memcmp(ch, "DAOX", 4) || !memcmp(ch, "DAOI", 4)) {
			if (ndao < CD_MAX_TRACKS)
				daos[ndao++] = chunk;
		} else if (!memcmp(ch, "ETN2", 4) || !memcmp(ch, "ETNF", 4)) {
			if (netn < CD_MAX_TRACKS)
				etns[netn++] = chunk;
		} else if (!memcmp(ch, "END!", 4)) {
			break;
		}
		k += 8 + (uint64_t)n;
	}
	int rc = 0;
	unsigned session = 0;
	for (unsigned k = 0; k < ndao && k < ncue && !rc; k++)
		rc = nrg_dao(c, cues[k], daos[k], v2, ++session, why, len);
	for (unsigned k = 0; k < netn && !rc; k++)
		rc = nrg_tao(c, etns[k], v2, ++session, why, len);
	free(d);
	if (!rc && !c->tracks)
		rc = fail(why, len, "it has no tracks Blue can find");
	snprintf(c->kind, sizeof c->kind, "a Nero image");
	return rc;
}

/* ---- Alcohol 120% (.mds, .mdf) ------------------------------------------------------ */

int cdimage_mds(struct cdimage *c, char *why, size_t len)
{
	int fd = open(c->path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		snprintf(why, len, "%s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	uint64_t size = (uint64_t)st.st_size;
	uint8_t *d = read_description(fd, 0, size);
	close(fd);
	if (!d || size < 88) {
		free(d);
		return fail(why, len, "cannot read it");
	}
	int rc = 0;
	unsigned medium = le16(d + 18), sessions = le16(d + 20);
	uint32_t blocks = le32(d + 80);
	if (d[16] != 1) {
		snprintf(why, len, "an Alcohol 120%% image of a version Blue doesn't read (%u.%u)", d[16], d[17]);
		rc = -1;
	} else if (medium >= 0x10) {
		rc = fail(why, len, "a DVD's image: the iMac's drive reads CDs");
	}
	for (unsigned s = 0; s < sessions && !rc; s++) {
		if (blocks + 24 * (uint64_t)(s + 1) > size) {
			rc = fail(why, len, "its table of contents is damaged");
			break;
		}
		const uint8_t *sb = d + blocks + 24 * s;
		unsigned n = sb[10], session = le16(sb + 8), first_track = c->tracks;
		uint32_t tracks = le32(sb + 20);
		int64_t leadout = -1;
		if (tracks + 80 * (uint64_t)n > size) {
			rc = fail(why, len, "its table of contents is damaged");
			break;
		}
		for (unsigned k = 0; k < n && !rc; k++) {
			const uint8_t *b = d + tracks + 80 * k;
			unsigned point = b[4];
			if (point == 0xA2)
				leadout = (int64_t)((b[9] * 60 + b[10]) * CD_SECTORS_PER_SECOND + b[11]) - CD_MSF_OFFSET;
			if (point < 1 || point > 99)
				continue;
			if (c->tracks >= CD_MAX_TRACKS)
				break;
			struct cd_track *t = &c->track[c->tracks];
			memset(t, 0, sizeof *t);
			t->number = point;
			t->session = session ? session : s + 1;
			t->control = b[2] & 0x0F;
			unsigned kind = b[0] & 7, stride = le16(b + 16), size_data = stride - (b[1] == 0x08 ? CD_SUBCODE : 0);
			uint8_t mode = kind == 1 ? 0 : kind == 2 ? 1 : 2;
			if (kind == 6 || (size_data != CD_SECTOR && size_data != CD_DATA && size_data != 2336) ||
			    (!mode && size_data != CD_SECTOR) || (b[1] && b[1] != 0x08)) {
				snprintf(why, len, "track %u is of a kind Blue doesn't read (Alcohol's mode 0x%02X, "
					 "%u-byte sectors)", point, b[0], stride);
				rc = -1;
				break;
			}
			set_sectors(t, mode, size_data, stride);
			uint32_t extra = le32(b + 12), pregap = 0, length = 0;
			if (extra && extra + 8 <= size) {
				pregap = le32(d + extra);
				length = le32(d + extra + 4);
			}
			t->start = le32(b + 36);
			uint32_t after = c->tracks > first_track ? c->track[c->tracks - 1].start + 1 : 0;
			t->pregap = t->start - (pregap < t->start - after ? pregap : t->start - after);
			uint32_t footer = le32(b + 52);
			int file = -1;
			if (le32(b + 48) && footer && footer + 16 <= size) {
				uint32_t name_at = le32(d + footer);
				char name[1024] = "";
				if (name_at < size && le32(d + footer + 4)) {           /* UTF-16 */
					size_t o = 0;
					for (uint64_t j = name_at; j + 1 < size && le16(d + j) && o + 1 < sizeof name; j += 2)
						name[o++] = le16(d + j) < 0x80 ? (char)le16(d + j) : '_';
					name[o] = 0;
				} else if (name_at < size) {
					snprintf(name, sizeof name, "%s", (const char *)d + name_at);
				}
				const char *dot = strrchr(name, '.');
				if (name[0] == '*' && dot)      /* "*.mdf": the .mds's name with that extension */
					file = add_beside(c, dot + 1, why, len);
				else
					file = cdimage_add_file(c, name, why, len);
				if (file < 0) {
					rc = -1;
					break;
				}
			}
			t->file = file;
			t->file_lba = t->start;
			t->file_pos = le64(b + 40);
			t->file_sectors = length;
			c->tracks++;
		}
		if (!rc && (c->tracks == first_track || leadout < 0))
			rc = fail(why, len, "its table of contents is damaged (no tracks, or no lead-out)");
		if (!rc) {
			ends(c, first_track, (uint32_t)leadout);
			for (unsigned k = first_track; k < c->tracks; k++)
				if (c->track[k].file_lba + (uint64_t)c->track[k].file_sectors > c->track[k].end)
					c->track[k].file_sectors = c->track[k].end - c->track[k].file_lba;
		}
	}
	free(d);
	if (!rc && !c->tracks)
		rc = fail(why, len, "it has no tracks");
	snprintf(c->kind, sizeof c->kind, "an Alcohol 120%% image");
	return rc;
}

/* ---- CloneCD (.ccd, .img, .sub) ---------------------------------------------------- */

struct ccd_entry {
	int session, point, control, plba;
};

struct ccd_track {
	int mode, index0, index1;
	char isrc[13];
};

int cdimage_ccd(struct cdimage *c, char *why, size_t len)
{
	int fd = open(c->path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		snprintf(why, len, "%s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	char *text = (char *)read_description(fd, 0, (uint64_t)st.st_size);
	close(fd);
	if (!text)
		return fail(why, len, "cannot read it");
	struct ccd_entry entry[3 * CD_MAX_TRACKS];
	struct ccd_track track[CD_MAX_TRACKS + 1];
	unsigned entries = 0;
	int scrambled = 0;
	memset(track, 0, sizeof track);
	for (unsigned k = 0; k <= CD_MAX_TRACKS; k++)
		track[k].mode = track[k].index0 = track[k].index1 = -1;
	enum { OTHER, DISC, ENTRY, TRACK } section = OTHER;
	int n = 0;
	for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		while (isspace((unsigned char)*line))
			line++;
		if (*line == '[') {
			section = OTHER;
			if (!strncasecmp(line, "[Disc]", 6))
				section = DISC;
			else if (sscanf(line, "[Entry %d]", &n) == 1 && entries < 3 * CD_MAX_TRACKS) {
				section = ENTRY;
				entry[entries++] = (struct ccd_entry){ 1, -1, 0, 0 };
			} else if (sscanf(line, "[TRACK %d]", &n) == 1 && n >= 1 && n <= (int)CD_MAX_TRACKS)
				section = TRACK;
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		for (char *e = eq - 1; e >= line && isspace((unsigned char)*e); e--)
			*e = 0;
		char *value = eq + 1;
		while (isspace((unsigned char)*value))
			value++;
		long v = strtol(value, NULL, 0);
		if (section == DISC) {
			if (!strcasecmp(line, "DataTracksScrambled"))
				scrambled = (int)v;
			else if (!strcasecmp(line, "CATALOG"))
				snprintf(c->catalog, sizeof c->catalog, "%.13s", value);
		} else if (section == ENTRY) {
			struct ccd_entry *e = &entry[entries - 1];
			if (!strcasecmp(line, "Session"))
				e->session = (int)v;
			else if (!strcasecmp(line, "Point"))
				e->point = (int)v;
			else if (!strcasecmp(line, "Control"))
				e->control = (int)v;
			else if (!strcasecmp(line, "PLBA"))
				e->plba = (int)v;
		} else if (section == TRACK) {
			if (!strcasecmp(line, "MODE"))
				track[n].mode = (int)v;
			else if (!strcasecmp(line, "INDEX 0"))
				track[n].index0 = (int)v;
			else if (!strcasecmp(line, "INDEX 1"))
				track[n].index1 = (int)v;
			else if (!strcasecmp(line, "ISRC"))
				snprintf(track[n].isrc, sizeof track[n].isrc, "%.12s", value);
		}
	}
	free(text);
	if (scrambled)
		return fail(why, len, "a CloneCD image whose data tracks are scrambled, which Blue doesn't read");
	int file = add_beside(c, "img", why, len);
	if (file < 0)
		return -1;
	/* the tracks, in order on the disc; each session's lead-out after them */
	uint64_t at = 0;
	int last_session = 0;
	for (int s = 1; s <= 99; s++) {
		int leadout = -1;
		unsigned first_track = c->tracks;
		for (unsigned k = 0; k < entries; k++)
			if (entry[k].session == s && entry[k].point == 0xA2)
				leadout = entry[k].plba;
		for (int p = 1; p <= 99; p++)
			for (unsigned k = 0; k < entries; k++) {
				const struct ccd_entry *e = &entry[k];
				if (e->session != s || e->point != p || c->tracks >= CD_MAX_TRACKS || e->plba < 0)
					continue;
				struct cd_track *t = &c->track[c->tracks++];
				const struct ccd_track *tr = &track[p];
				memset(t, 0, sizeof *t);
				t->number = (unsigned)p;
				t->session = (unsigned)s;
				t->control = (uint8_t)(e->control & 0x0F);
				uint8_t mode = !(t->control & CD_CONTROL_DATA) ? 0 : tr->mode == 2 ? 2 : 1;
				set_sectors(t, mode, CD_SECTOR, CD_SECTOR);
				t->start = (uint32_t)e->plba;
				int pregap = tr->index0 >= 0 && tr->index1 > tr->index0 ? tr->index1 - tr->index0 : 0;
				uint32_t low = c->tracks - 1 > first_track ? c->track[c->tracks - 2].start + 1 : 0;
				t->pregap = t->start - ((uint32_t)pregap < t->start - low ? (uint32_t)pregap : t->start - low);
				memcpy(t->isrc, tr->isrc, sizeof t->isrc);
			}
		if (c->tracks == first_track)
			continue;
		if (leadout < 0)
			return fail(why, len, "its table of contents is damaged (a session without its lead-out)");
		ends(c, first_track, (uint32_t)leadout);
		for (unsigned k = first_track; k < c->tracks; k++) {   /* (the .img: each track from its pregap on) */
			struct cd_track *t = &c->track[k];
			place(t, file, t->pregap, at, t->end - t->pregap);
			at += (uint64_t)(t->end - t->pregap) * CD_SECTOR;
		}
		last_session = s;
	}
	if (!last_session)
		return fail(why, len, "it has no tracks");
	/* (a last track cut short: as much as the .img holds) */
	struct cd_track *t = &c->track[c->tracks - 1];
	uint64_t size = c->files[file].size;
	if (t->file_pos + (uint64_t)t->file_sectors * CD_SECTOR > size)
		t->file_sectors = size > t->file_pos ? (uint32_t)((size - t->file_pos) / CD_SECTOR) : 0;
	snprintf(c->kind, sizeof c->kind, "a CloneCD image");
	return 0;
}

/* ---- MAME's CHD -------------------------------------------------------------------- */

struct chd_source {
	chd_file *chd;
	uint32_t hunkbytes;
	uint64_t size;
	uint8_t *hunk[CHD_CACHE];
	uint32_t number[CHD_CACHE];
	unsigned next;
};

static int chd_source_read(void *ctx, void *buf, size_t len, uint64_t at)
{
	struct chd_source *s = ctx;
	uint8_t *out = buf;
	if (at > s->size || len > s->size - at)
		return -1;
	while (len) {
		uint32_t h = (uint32_t)(at / s->hunkbytes), off = (uint32_t)(at % s->hunkbytes);
		unsigned k = 0;
		while (k < CHD_CACHE && s->number[k] != h)
			k++;
		if (k == CHD_CACHE) {
			k = s->next;
			s->next = (s->next + 1) % CHD_CACHE;
			s->number[k] = UINT32_MAX;
			chd_error err = chd_read(s->chd, h, s->hunk[k]);
			if (err != CHDERR_NONE) {
				fprintf(stderr, "CHD: hunk %u: %s\n", h, chd_error_string(err));
				return -1;
			}
			s->number[k] = h;
		}
		size_t n = s->hunkbytes - off < len ? s->hunkbytes - off : len;
		memcpy(out, s->hunk[k] + off, n);
		out += n, at += n, len -= n;
	}
	return 0;
}

static void chd_source_close(void *ctx)
{
	struct chd_source *s = ctx;
	if (!s)
		return;
	for (unsigned k = 0; k < CHD_CACHE; k++)
		free(s->hunk[k]);
	if (s->chd)
		chd_close(s->chd);
	free(s);
}

/* A CHD track type: the kind of sector, and its bytes in each frame */
static bool chd_type(const char *type, uint8_t *mode, unsigned *size)
{
	static const struct { const char *name; uint8_t mode; unsigned size; } types[] = {
		{ "AUDIO", 0, CD_SECTOR }, { "MODE1", 1, CD_DATA }, { "MODE1/2048", 1, CD_DATA },
		{ "MODE1_RAW", 1, CD_SECTOR }, { "MODE1/2352", 1, CD_SECTOR }, { "MODE2", 2, 2336 },
		{ "MODE2/2336", 2, 2336 }, { "MODE2_FORM_MIX", 2, 2336 }, { "MODE2_FORM1", 2, CD_DATA },
		{ "MODE2/2048", 2, CD_DATA }, { "MODE2_RAW", 2, CD_SECTOR }, { "MODE2/2352", 2, CD_SECTOR },
		{ "CDI/2352", 2, CD_SECTOR },
	};
	for (size_t k = 0; k < sizeof types / sizeof types[0]; k++)
		if (!strcmp(type, types[k].name)) {
			*mode = types[k].mode;
			*size = types[k].size;
			return true;
		}
	return false;
}

int cdimage_chd(struct cdimage *c, char *why, size_t len)
{
	struct chd_source *s = calloc(1, sizeof *s);
	if (!s)
		return fail(why, len, "out of memory");
	chd_error err = chd_open(c->path, CHD_OPEN_READ, NULL, &s->chd);
	if (err != CHDERR_NONE) {
		free(s);
		snprintf(why, len, "%s", err == CHDERR_REQUIRES_PARENT ? "a CHD that needs its parent, which Blue doesn't "
			 "read (chdman copy makes one that stands alone)" : chd_error_string(err));
		return -1;
	}
	struct cd_file *f = &c->files[c->nfiles++];
	f->fd = -1;
	f->path = strdup(c->path);
	f->dec = s;
	f->read = chd_source_read;
	f->close = chd_source_close;
	f->swap = true;                 /* (a CHD's audio is big-endian) */
	const chd_header *h = chd_get_header(s->chd);
	s->hunkbytes = h->hunkbytes;
	s->size = f->size = h->logicalbytes;
	for (unsigned k = 0; k < CHD_CACHE; k++) {
		s->number[k] = UINT32_MAX;
		if (s->hunkbytes && !(s->hunk[k] = malloc(s->hunkbytes)))
			return fail(why, len, "out of memory");
	}
	size_t n = (size_t)snprintf(c->kind, sizeof c->kind, "a CHD (v%u", h->version);
	for (unsigned k = 0; k < 4 && h->version >= 5 && n < sizeof c->kind; k++)
		if (h->compression[k])
			n += (size_t)snprintf(c->kind + n, sizeof c->kind - n, "%s%c%c%c%c", k ? ", " : ": ",
					      (char)(h->compression[k] >> 24), (char)(h->compression[k] >> 16),
					      (char)(h->compression[k] >> 8), (char)h->compression[k]);
	if (n < sizeof c->kind)
		snprintf(c->kind + n, sizeof c->kind - n, ")");
	if (!s->hunkbytes || s->hunkbytes % CHD_FRAME)
		return fail(why, len, "a CHD of something other than a CD (a hard disk's, a DVD's, a LaserDisc's?)");
	/* the tracks: one piece of metadata each */
	int64_t disc = 0, origin = -1;
	uint64_t frame = 0;
	for (unsigned k = 0; k < CD_MAX_TRACKS; k++) {
		char meta[256], type[16] = "", sub[16] = "", pgtype[16] = "", pgsub[16] = "";
		int number = 0, frames = 0, pregap = 0, postgap = 0;
		uint32_t got = 0;
		if (chd_get_metadata(s->chd, CDROM_TRACK_METADATA2_TAG, k, meta, sizeof meta - 1, &got, NULL, NULL) ==
		    CHDERR_NONE) {
			meta[got < sizeof meta ? got : sizeof meta - 1] = 0;
			if (sscanf(meta, "TRACK:%d TYPE:%15s SUBTYPE:%15s FRAMES:%d PREGAP:%d PGTYPE:%15s PGSUB:%15s "
				   "POSTGAP:%d", &number, type, sub, &frames, &pregap, pgtype, pgsub, &postgap) != 8)
				return fail(why, len, "a CHD whose table of contents is damaged");
		} else if (chd_get_metadata(s->chd, CDROM_TRACK_METADATA_TAG, k, meta, sizeof meta - 1, &got, NULL,
					    NULL) == CHDERR_NONE) {
			meta[got < sizeof meta ? got : sizeof meta - 1] = 0;
			if (sscanf(meta, "TRACK:%d TYPE:%15s SUBTYPE:%15s FRAMES:%d", &number, type, sub, &frames) != 4)
				return fail(why, len, "a CHD whose table of contents is damaged");
		} else if (k) {
			break;
		} else if (chd_get_metadata(s->chd, GDROM_TRACK_METADATA_TAG, 0, meta, sizeof meta, &got, NULL, NULL) ==
			   CHDERR_NONE || chd_get_metadata(s->chd, GDROM_OLD_METADATA_TAG, 0, meta, sizeof meta, &got,
							   NULL, NULL) == CHDERR_NONE) {
			return fail(why, len, "a Dreamcast GD-ROM's CHD: not a CD a Mac reads");
		} else if (chd_get_metadata(s->chd, CDROM_OLD_METADATA_TAG, 0, meta, sizeof meta, &got, NULL, NULL) ==
			   CHDERR_NONE) {
			return fail(why, len, "a CHD with an old kind of table of contents (chdman copy makes a new one)");
		} else {
			return fail(why, len, "a CHD of something other than a CD (a hard disk's, a DVD's, a LaserDisc's?)");
		}
		struct cd_track *t = &c->track[c->tracks];
		uint8_t mode;
		unsigned size;
		memset(t, 0, sizeof *t);
		bool stored = pgtype[0] == 'V';         /* (the pregap's sectors are in it, before index 1) */
		if (number != (int)c->tracks + 1 || frames < 0 || pregap < 0 || postgap < 0 || (stored && pregap > frames))
			return fail(why, len, "a CHD whose table of contents is damaged");
		if (!chd_type(type, &mode, &size)) {
			snprintf(why, len, "track %d is of a kind Blue doesn't read (%s)", number, type);
			return -1;
		}
		t->number = (unsigned)number;
		t->session = 1;
		set_sectors(t, mode, size, CHD_FRAME);
		int64_t pregap_lba = disc, start = disc + pregap;
		int64_t file_lba = stored ? pregap_lba : start;
		int64_t end = file_lba + frames + postgap;
		if (origin < 0)
			origin = start;
		/* (track 1 starts at LBA 0: what comes before it isn't addressed) */
		uint64_t skip = file_lba < origin ? (uint64_t)(origin - file_lba) : 0;
		if (skip > (uint64_t)frames)
			skip = (uint64_t)frames;
		t->pregap = pregap_lba > origin ? (uint32_t)(pregap_lba - origin) : 0;
		t->start = (uint32_t)(start - origin);
		t->end = (uint32_t)(end - origin);
		t->file = 0;
		t->file_lba = (uint32_t)(file_lba + (int64_t)skip - origin);
		t->file_pos = (frame + skip) * CHD_FRAME;
		t->file_sectors = (uint32_t)((uint64_t)frames - skip);
		c->tracks++;
		disc = end;
		frame += (uint64_t)frames + (CHD_PADDING - (uint64_t)frames % CHD_PADDING) % CHD_PADDING;
	}
	if (!c->tracks)
		return fail(why, len, "a CHD without tracks");
	return 0;
}
