#include "cdrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "floppy.h"

/* OSErr values */
#define NO_ERR 0
#define CONTROL_ERR (-17)
#define STATUS_ERR (-18)
#define PARAM_ERR (-50)
#define OFFLINE_ERR (-65)

#define PB_CS_CODE 26
#define PB_CS_PARAM 28

/* AudioStatus */
#define STATUS_PLAYING 0
#define STATUS_PAUSED 1
#define STATUS_MUTED 2
#define STATUS_DONE 3
#define STATUS_IDLE 5

#define DRIVE_TYPE_MULTISPEED 3         /* GetDriveType: an AppleCD 300 or later */
#define SPEED 24u                       /* GetCDFeatures: times an AppleCD 150's (the iMac's drive is 24x) */
#define FEATURES 0x083Cu                /* ... muting, either channel or both to one, stereo volume */
#define MAX_SPEED 0xFF

/* ReadAudio's kinds: the bytes of each sector */
static const unsigned audio_sizes[4] = { CD_SECTOR, CD_SECTOR + 16, CD_SECTOR + CD_SUBCODE, CD_SUBCODE };

struct cdrom *cdrom_new(uint64_t hz, uint32_t device_ident)
{
	struct cdrom *cd = calloc(1, sizeof *cd);
	if (!cd)
		return NULL;
	cdaudio_init(&cd->audio, hz);
	cd->play_mode = CDAUDIO_STEREO;
	cd->block_size = 512;
	cd->device_ident = device_ident;
	return cd;
}

void cdrom_free(struct cdrom *cd)
{
	free(cd);
}

void cdrom_disc(struct cdrom *cd, const struct cdimage *disc)
{
	cdaudio_disc(&cd->audio, disc);
	cd->stop_set = cd->list_play = false;
	cd->list_len = 0;
}

static uint16_t word(struct mem *m, uint32_t pb, unsigned k)
{
	return mem_r16(m, pb + PB_CS_PARAM + 2 * k);
}

static uint32_t lword(struct mem *m, uint32_t pb, unsigned k)
{
	return mem_r32(m, pb + PB_CS_PARAM + 2 * k);
}

static void put_word(struct mem *m, uint32_t pb, unsigned k, unsigned v)
{
	mem_w16(m, pb + PB_CS_PARAM + 2 * k, (uint16_t)v);
}

static bool writable(const struct mem *m, uint32_t addr, uint32_t len)
{
	if (!addr || addr + len < addr)
		return false;
	for (uint32_t a = addr & ~(MEM_PAGE_SIZE - 1); len; a += MEM_PAGE_SIZE) {
		if (!mem_ok(m, a, 1, MEM_W))
			return false;
		if (a >= ((addr + len - 1) & ~(MEM_PAGE_SIZE - 1)))
			break;
	}
	return true;
}

static const struct cd_track *track_numbered(const struct cdimage *disc, unsigned n)
{
	for (unsigned k = 0; k < disc->tracks; k++)
		if (disc->track[k].number == n)
			return &disc->track[k];
	return NULL;
}

/* The track at an address, or the last one before it (in the lead-out) */
static const struct cd_track *track_near(const struct cdimage *disc, uint32_t lba)
{
	int k = cdimage_track_at(disc, lba);
	if (k >= 0)
		return &disc->track[k];
	const struct cd_track *t = &disc->track[0];
	for (unsigned j = 0; j < disc->tracks && disc->track[j].end <= lba; j++)
		t = &disc->track[j];
	return t;
}

static void put_msf(uint8_t *p, uint32_t lba)
{
	cd_msf(lba, &p[0], &p[1], &p[2]);
	for (int k = 0; k < 3; k++)
		p[k] = cd_bcd(p[k]);
}

/*
 * An address as a sector: from where play starts (`end` false) or where it
 * stops (`end`: a track's or a play list entry's is its end). `entry` gets
 * the play list entry of type 3 (-1 otherwise). False if there's no such.
 */
static bool address(const struct cdrom *cd, const struct cdimage *disc, unsigned type, uint32_t v, bool end,
		    uint32_t *lba, int *entry)
{
	const struct cd_track *t = NULL;
	*entry = -1;
	switch (type) {
	case 0:
		*lba = v;
		break;
	case 1: {
		unsigned s = cd_unbcd((uint8_t)(v >> 8)), f = cd_unbcd((uint8_t)v);
		if (s >= 60 || f >= CD_SECTORS_PER_SECOND)
			return false;
		*lba = cd_lba(cd_unbcd((uint8_t)(v >> 16)), s, f);
		break;
	}
	case 2:
		t = track_numbered(disc, cd_unbcd((uint8_t)v));
		break;
	case 3: {
		unsigned k = cd_unbcd((uint8_t)v);
		if (k >= cd->list_len)
			return false;
		*entry = (int)k;
		t = track_numbered(disc, cd_unbcd(cd->list[k]));
		break;
	}
	default:
		return false;
	}
	if (type >= 2) {
		if (!t)
			return false;
		*lba = end ? t->end : t->start;
	}
	return *lba <= disc->leadout;
}

static void log_play(const struct cdrom *cd, const char *what, uint32_t from, uint32_t to, int entry, unsigned last)
{
	uint8_t a[3], b[3];
	if (!cd->log)
		return;
	cd_msf(from, &a[0], &a[1], &a[2]);
	cd_msf(to, &b[0], &b[1], &b[2]);
	printf("cdrom: %s %02u:%02u:%02u (LBA %u)", what, a[0], a[1], a[2], from);
	if (entry >= 0)
		printf(", the play list's entries %d-%u", entry, last);
	else
		printf(" to %02u:%02u:%02u (LBA %u)", b[0], b[1], b[2], to);
	printf(", play mode %u%s\n", cd->play_mode, cd->repeat ? ", over and over" : "");
}

/* Where the next play stops: an address, or the play list's entry (>= 0) */
static void set_stop(struct cdrom *cd, uint32_t lba, int entry)
{
	cd->stop = entry >= 0 ? (uint32_t)entry : lba;
	cd->stop_set = true;
	cd->stop_list = entry >= 0;
	if (cd->log)
		printf("cdrom: stop at %s %u\n", entry >= 0 ? "play list entry" : "LBA", cd->stop);
}

/* The play list's entry whose track holds `lba` (the one playing, if it does), or -1 */
static int entry_at(const struct cdrom *cd, const struct cdimage *disc, uint32_t lba)
{
	int found = -1;
	for (unsigned k = 0; k < cd->list_len; k++) {
		const struct cd_track *t = track_numbered(disc, cd_unbcd(cd->list[k]));
		if (t && lba >= t->start && lba < t->end && (found < 0 || k == cd->audio.current))
			found = (int)k;
	}
	return found;
}

/*
 * Plays from `from` up to the stop address, or holds there; through the
 * play list, from its entry `entry`, if given, or if the stop address is one
 * of its entries and `from` is in one of its tracks.
 */
static int32_t play(struct cdrom *cd, const struct cdimage *disc, uint64_t tb, uint32_t from, int entry, bool hold)
{
	struct cdaudio_range r[CD_MAX_TRACKS];
	unsigned n = 0, first = 0;
	if (!cdimage_audio(disc, from))
		return PARAM_ERR;       /* (not on a data track) */
	if (entry < 0 && cd->stop_set && cd->stop_list)
		entry = entry_at(cd, disc, from);
	if (entry >= 0) {
		unsigned last = cd->stop_set && cd->stop_list && cd->stop >= (unsigned)entry && cd->stop < cd->list_len ?
					cd->stop : cd->list_len - 1;
		for (unsigned k = 0; k <= last; k++) {
			const struct cd_track *t = track_numbered(disc, cd_unbcd(cd->list[k]));
			r[n++] = (struct cdaudio_range){ t ? t->start : 0, t ? t->end : 0 };
		}
		first = (unsigned)entry;
	} else {
		uint32_t end = cd->stop_set && !cd->stop_list && cd->stop > from ? cd->stop : disc->leadout;
		r[n++] = (struct cdaudio_range){ from, end };
	}
	log_play(cd, hold ? "hold at" : "play from", from, r[n - 1].end, entry, n - 1);
	cdaudio_mode(&cd->audio, tb, cd->play_mode);
	cdaudio_play(&cd->audio, tb, r, n, first, from, cd->repeat, hold);
	cd->list_play = entry >= 0;
	cd->audio_calls++;
	return NO_ERR;
}

static int32_t read_toc(const struct cdimage *disc, struct mem *m, uint32_t pb)
{
	const struct cd_track *t0 = &disc->track[0], *tn = &disc->track[disc->tracks - 1];
	uint8_t msf[3];
	switch (word(m, pb, 0)) {
	case 1:                 /* the first and last tracks */
		put_word(m, pb, 0, cd_bcd(t0->number) << 8 | cd_bcd(tn->number));
		return NO_ERR;
	case 2:                 /* the lead-out */
		put_msf(msf, disc->leadout);
		put_word(m, pb, 0, msf[0] << 8 | msf[1]);
		put_word(m, pb, 1, msf[2] << 8);
		return NO_ERR;
	case 3: {               /* where tracks start, from one on: control, M, S, F each */
		uint32_t buf = lword(m, pb, 1), size = word(m, pb, 3), at = 0;
		unsigned from = cd_unbcd((uint8_t)(word(m, pb, 4) >> 8));
		if (!writable(m, buf, size))
			return PARAM_ERR;
		for (unsigned k = 0; k < disc->tracks && at + 4 <= size; k++) {
			const struct cd_track *t = &disc->track[k];
			if (t->number < from)
				continue;
			mem_w8(m, buf + at, t->control);
			put_msf(msf, t->start);
			for (int j = 0; j < 3; j++)
				mem_w8(m, buf + at + 1 + j, msf[j]);
			at += 4;
		}
		return NO_ERR;
	}
	case 4: {               /* the whole table: points A0, A1, A2, then tracks 1-99, 5 bytes each */
		uint32_t buf = lword(m, pb, 1);
		if (!writable(m, buf, 512))
			return PARAM_ERR;
		uint8_t toc[512] = { 0 };
		uint8_t *p = toc + 1;
		p[0] = t0->control, p[1] = 0xA0, p[2] = cd_bcd(t0->number);
		p += 5;
		p[0] = tn->control, p[1] = 0xA1, p[2] = cd_bcd(tn->number);
		p += 5;
		p[0] = tn->control, p[1] = 0xA2;
		put_msf(p + 2, disc->leadout);
		for (unsigned k = 0; k < disc->tracks; k++) {
			const struct cd_track *t = &disc->track[k];
			p = toc + 16 + 5 * (t->number - 1);
			p[0] = t->control, p[1] = cd_bcd(t->number);
			put_msf(p + 2, t->start);
		}
		memcpy(mem_ptr(m, buf), toc, sizeof toc);
		return NO_ERR;
	}
	case 5: {               /* sessions: the first, the last, and where the last starts */
		const struct cd_track *t = t0;
		for (unsigned k = 0; k < disc->tracks; k++)
			if (disc->track[k].session == disc->sessions) {
				t = &disc->track[k];
				break;
			}
		put_msf(msf, t->start);
		put_word(m, pb, 0, cd_bcd(t0->session));
		put_word(m, pb, 1, cd_bcd(disc->sessions));
		put_word(m, pb, 2, cd_bcd(t->number));
		put_word(m, pb, 3, (t->control & 15u) << 8 | msf[0]);
		put_word(m, pb, 4, msf[1] << 8 | msf[2]);
		return NO_ERR;
	}
	case 6: {               /* the lead-in's Q entries by session: A0, A1, A2 and the tracks, 11 bytes each */
		uint32_t buf = lword(m, pb, 1), size = word(m, pb, 3), at = 4;
		unsigned from = cd_unbcd((uint8_t)(word(m, pb, 4) >> 8));
		if (size < 4 || !writable(m, buf, size))
			return PARAM_ERR;
		for (unsigned s = from ? from : 1; s <= disc->sessions; s++) {
			const struct cd_track *first = NULL, *last = NULL;
			for (unsigned k = 0; k < disc->tracks; k++)
				if (disc->track[k].session == s)
					last = &disc->track[k], first = first ? first : last;
			if (!first)
				continue;
			for (unsigned k = 0; k < 3u + (unsigned)(last - first) + 1; k++) {
				uint8_t e[11] = { (uint8_t)s };
				const struct cd_track *t = k < 3 ? (k == 1 ? last : first) : first + (k - 3);
				e[1] = (uint8_t)(1u << 4 | t->control);
				if (k == 0)
					e[3] = 0xA0, e[8] = cd_bcd(first->number);
				else if (k == 1)
					e[3] = 0xA1, e[8] = cd_bcd(last->number);
				else if (k == 2)
					e[3] = 0xA2, put_msf(e + 8, last->end);
				else
					e[3] = cd_bcd(t->number), put_msf(e + 8, t->start);
				if (at + sizeof e > size)
					break;
				memcpy(mem_ptr(m, buf + at), e, sizeof e);
				at += sizeof e;
			}
		}
		mem_w16(m, buf, (uint16_t)(at - 2));
		mem_w8(m, buf + 2, cd_bcd(from ? from : 1));
		mem_w8(m, buf + 3, cd_bcd(disc->sessions));
		return NO_ERR;
	}
	default:
		return PARAM_ERR;
	}
}

/* ReadAudio: sectors of samples, with their Q or all their subchannel, or the subchannel alone */
static int32_t read_audio(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb)
{
	unsigned kind = word(m, pb, 0), count = word(m, pb, 6);
	uint32_t buf = lword(m, pb, 1), lba;
	int entry;
	if (kind > 3 || !address(cd, disc, word(m, pb, 3), lword(m, pb, 4), false, &lba, &entry) ||
	    !cdimage_audio(disc, lba))
		return PARAM_ERR;
	uint32_t size = audio_sizes[kind];
	if (!writable(m, buf, count * size))
		return PARAM_ERR;
	mem_invalidate(m, buf, count * size);
	uint8_t sector[CD_SECTOR + CD_SUBCODE];
	for (unsigned k = 0; k < count; k++, buf += size) {
		memset(sector, 0, sizeof sector);
		if (kind < 3 && cdimage_read_raw(disc, lba + k, 1, sector))
			return PARAM_ERR;
		if (kind == 1)
			cdimage_q(disc, lba + k, sector + CD_SECTOR);
		else if (kind >= 2)
			cdimage_subcode(disc, lba + k, kind == 2 ? sector + CD_SECTOR : sector);
		memcpy(mem_ptr(m, buf), sector, size);
	}
	cd->audio_calls++;
	return NO_ERR;
}

/* A Pascal string in csParam (ReadMCN, ReadISRC): "" if there's none */
static void put_string(struct mem *m, uint32_t pb, const char *s)
{
	size_t n = strlen(s);
	mem_w8(m, pb + PB_CS_PARAM, (uint8_t)n);
	memcpy(mem_ptr(m, pb + PB_CS_PARAM + 1), s, n);
}

static bool control_call(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb, uint64_t tb,
			 int32_t *err);

/* (with --log-hostcalls: the parameters going in and coming out) */
static void log_params(struct mem *m, uint32_t pb, const char *what)
{
	printf(" %s", what);
	for (unsigned k = 0; k < 7; k++)
		printf(" %04X", word(m, pb, k));
}

bool cdrom_control_call(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb, uint64_t tb,
			int32_t *err)
{
	uint16_t in[7];
	for (unsigned k = 0; k < 7; k++)
		in[k] = word(m, pb, k);
	bool handled = control_call(cd, disc, m, pb, tb, err);
	if (cd->log && handled) {
		printf("cdrom: control %u, in", mem_r16(m, pb + PB_CS_CODE));
		for (unsigned k = 0; k < 7; k++)
			printf(" %04X", in[k]);
		log_params(m, pb, "out");
		printf(" -> %d\n", *err);
	}
	return handled;
}

static bool control_call(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb, uint64_t tb,
			 int32_t *err)
{
	unsigned code = mem_r16(m, pb + PB_CS_CODE);
	uint32_t lba;
	int entry;
	*err = NO_ERR;
	switch (code) {
	case 65:                /* accRun */
	case 70:                /* SetPowerMode */
	case 76:                /* ModifyPostEvent */
	case 80:                /* SetUserEject */
	case 81:                /* SetPollFreq */
	case 1079:              /* Quiescence */
	case 0xFFFF:            /* GoodBye */
		return true;
	case 79:                /* SetBlockSize: the data's, 512 or 2048 bytes */
		if (word(m, pb, 0) != 512 && word(m, pb, 0) != 2048)
			*err = PARAM_ERR;
		else
			cd->block_size = word(m, pb, 0);
		return true;
	case 109:               /* AudioControl: the volume of each channel */
		cdaudio_volume(&cd->audio, tb, (uint8_t)(word(m, pb, 0) >> 8), (uint8_t)word(m, pb, 0));
		return true;
	case 112:               /* ReadAudioVolume */
		put_word(m, pb, 0, cd->audio.volume[0] << 8 | cd->audio.volume[1]);
		return true;
	case 113:               /* GetSpindleSpeed */
		put_word(m, pb, 0, cd->speed);
		return true;
	case 114:               /* SetSpindleSpeed: normal or the fastest */
		if (word(m, pb, 0) != 0 && word(m, pb, 0) != MAX_SPEED)
			*err = PARAM_ERR;
		else
			cd->speed = (uint8_t)word(m, pb, 0);
		return true;
	case 116:               /* ReadAllSubcodes, as most ATAPI drives: not had */
		*err = CONTROL_ERR;
		return true;
	case 123: {             /* GetTrackList */
		uint32_t buf = lword(m, pb, 1);
		if (!writable(m, buf, cd->list_len ? cd->list_len : 1)) {
			*err = PARAM_ERR;
			return true;
		}
		put_word(m, pb, 0, cd->list_len);
		memcpy(mem_ptr(m, buf), cd->list, cd->list_len);
		return true;
	}
	case 124:               /* GetTrackIndex: the play list's entry playing (-1: none) */
		cdaudio_run(&cd->audio, tb);
		put_word(m, pb, 0, cd->list_play && cd->audio.state != CDAUDIO_IDLE ? cd->audio.current : 0xFFFFu);
		return true;
	case 125:               /* SetPlayMode: repeat, and normal, shuffle or programmed */
		cd->repeat = (word(m, pb, 0) >> 8) != 0;
		cd->list_mode = (uint8_t)word(m, pb, 0);
		cdaudio_run(&cd->audio, tb);
		cd->audio.repeat = cd->repeat;
		return true;
	case 126:               /* GetPlayMode */
		put_word(m, pb, 0, (cd->repeat ? 0x100u : 0) | cd->list_mode);
		return true;
	}

	if (code < 100 || code > 122)
		return false;
	if (!disc) {            /* (what's left needs a disc) */
		*err = OFFLINE_ERR;
		return true;
	}
	switch (code) {
	case 100:               /* ReadTOC */
		*err = read_toc(disc, m, pb);
		return true;
	case 101: {             /* ReadTheQSubcode: where the drive is */
		uint8_t q[10];
		cdaudio_status(&cd->audio, tb, &lba);
		cdimage_q(disc, lba < disc->leadout ? lba : disc->leadout - 1, q);
		put_word(m, pb, 0, (q[0] >> 4) << 8 | q[1]);
		put_word(m, pb, 1, q[2] << 8 | q[3]);
		put_word(m, pb, 2, q[4] << 8 | q[5]);
		put_word(m, pb, 3, q[7] << 8 | q[8]);
		put_word(m, pb, 4, q[9] << 8);
		return true;
	}
	case 102: {             /* ReadHeader: a data sector's address and mode */
		uint8_t msf[3];
		lba = lword(m, pb, 0);
		int k = cdimage_track_at(disc, lba);
		if (k < 0 || !disc->track[k].mode) {
			*err = PARAM_ERR;
			return true;
		}
		put_msf(msf, lba);
		put_word(m, pb, 0, msf[0] << 8 | msf[1]);
		put_word(m, pb, 1, msf[2] << 8 | disc->track[k].mode);
		return true;
	}
	case 103:               /* AudioTrackSearch: to an address, then play or hold */
	case 104:               /* AudioPlay: from an address, or (csParam[3]) where to stop */
		if (!address(cd, disc, word(m, pb, 0), lword(m, pb, 1), code == 104 && word(m, pb, 3), &lba, &entry)) {
			*err = PARAM_ERR;
			return true;
		}
		cd->play_mode = word(m, pb, 4) & 15;
		if (code == 104 && word(m, pb, 3)) {
			set_stop(cd, lba, entry);
			return true;
		}
		*err = play(cd, disc, tb, lba, entry, code == 103 && !word(m, pb, 3));
		return true;
	case 105:               /* AudioPause: 1 pause, 0 go on */
		cdaudio_pause(&cd->audio, tb, lword(m, pb, 0) != 0);
		return true;
	case 106:               /* AudioStop: at once (address 0), or where (for this play and the next) */
		if (word(m, pb, 0) == 0 && lword(m, pb, 1) == 0) {
			cdaudio_stop(&cd->audio, tb);
			cd->list_play = false;
			if (cd->log)
				printf("cdrom: stop\n");
		} else if (address(cd, disc, word(m, pb, 0), lword(m, pb, 1), true, &lba, &entry)) {
			if (entry < 0)
				cdaudio_stop_at(&cd->audio, tb, lba);
			else if (cd->list_play)
				cdaudio_stop_after(&cd->audio, tb, (unsigned)entry);
			set_stop(cd, lba, entry);
		} else {
			cd->stop_set = false;   /* (AppleCD Audio Player starts with track 0: no stop, the lead-out) */
		}
		return true;
	case 107: {             /* AudioStatus: state, play mode, control, where (absolute) */
		uint8_t msf[3];
		enum cdaudio_state s = cdaudio_status(&cd->audio, tb, &lba);
		unsigned status = s == CDAUDIO_PLAYING ? (cd->play_mode ? STATUS_PLAYING : STATUS_MUTED) :
				  s == CDAUDIO_PAUSED ? STATUS_PAUSED : s == CDAUDIO_DONE ? STATUS_DONE : STATUS_IDLE;
		put_msf(msf, lba);
		put_word(m, pb, 0, status << 8 | cd->play_mode);
		put_word(m, pb, 1, (track_near(disc, lba)->control & 15u) << 8 | msf[0]);
		put_word(m, pb, 2, msf[1] << 8 | msf[2]);
		return true;
	}
	case 108:               /* AudioScan: from an address, forward (0) or back */
		if (!address(cd, disc, word(m, pb, 0), lword(m, pb, 1), false, &lba, &entry) || !cdimage_audio(disc, lba)) {
			*err = PARAM_ERR;
			return true;
		}
		cdaudio_scan(&cd->audio, tb, lba, word(m, pb, 3) ? -1 : 1);
		return true;
	case 110:               /* ReadMCN */
		put_string(m, pb, disc->catalog);
		return true;
	case 111: {             /* ReadISRC */
		const struct cd_track *t = track_numbered(disc, cd_unbcd((uint8_t)word(m, pb, 0)));
		put_string(m, pb, t ? t->isrc : "");
		return true;
	}
	case 115:               /* ReadAudio */
		*err = read_audio(cd, disc, m, pb);
		return true;
	case 122: {             /* SetTrackList: track numbers (BCD), in the order to play */
		unsigned n = word(m, pb, 0);
		uint32_t list = lword(m, pb, 1);
		if (n >= 100 || !list || !mem_ok(m, list, n ? n : 1, MEM_R)) {
			*err = PARAM_ERR;
			return true;
		}
		for (unsigned k = 0; k < n; k++)
			if (!track_numbered(disc, cd_unbcd(mem_r8(m, list + k)))) {
				*err = PARAM_ERR;
				return true;
			}
		memcpy(cd->list, mem_ptr(m, list), n);
		cd->list_len = n;
		return true;
	}
	default:
		*err = CONTROL_ERR;
		return true;
	}
}

static bool status_call(struct cdrom *cd, struct mem *m, uint32_t pb, int32_t *err);

bool cdrom_status_call(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb, uint64_t tb,
		       int32_t *err)
{
	(void)disc;
	(void)tb;
	bool handled = status_call(cd, m, pb, err);
	if (cd->log && handled) {
		printf("cdrom: status %u,", mem_r16(m, pb + PB_CS_CODE));
		log_params(m, pb, "out");
		printf(" -> %d\n", *err);
	}
	return handled;
}

static bool status_call(struct cdrom *cd, struct mem *m, uint32_t pb, int32_t *err)
{
	*err = NO_ERR;
	switch (mem_r16(m, pb + PB_CS_CODE)) {
	case 70:                /* GetPowerMode: active */
		put_word(m, pb, 0, 0);
		return true;
	case 95:                /* Get2KOffset: of the last read in its 2K block */
		if (!cd->read_once)
			*err = STATUS_ERR;
		else
			mem_w32(m, pb + PB_CS_PARAM, cd->last_read % CD_DATA & ~511u);
		return true;
	case 96:                /* GetDriveType */
		put_word(m, pb, 0, DRIVE_TYPE_MULTISPEED);
		return true;
	case 98:                /* GetBlockSize */
		put_word(m, pb, 0, cd->block_size);
		return true;
	case 120:               /* ReturnDeviceIdent */
		mem_w32(m, pb + PB_CS_PARAM, cd->device_ident);
		return true;
	case 121:               /* GetCDFeatures: speed, features */
		put_word(m, pb, 0, SPEED << 8);
		put_word(m, pb, 1, FEATURES);
		return true;
	default:
		return false;
	}
}

/* ---- icons -------------------------------------------------------------------- */

static const char *const cd32[32] = {
	"............KKKKKKKK............",
	".........KKKKLLLLLLKKKK.........",
	"........KKLLLLLLLLLLLyKK........",
	"......KKWLLLLLLLLLLLyyymKK......",
	".....KKWWLLLLLLLLLLLyymmmKK.....",
	"....KWWWWWLLLLLLLLLLyymmmccK....",
	"...KKWWWWWWLLLLLLLLyymmmcccKK...",
	"...KWWWWWWWLLLLLLLLyymmcccccK...",
	"..KWWWWWWWWWLLLLLLLymmmccccLLK..",
	".KKLLWWWWWWLGGGGGGGGmmcccLLLLKK.",
	".KLLLLWWWWLGGHHHHHHGGcccLLLLLLK.",
	".KLLLLLLWLGHHggggggHHGLLLLLLLLK.",
	"KKLLLLLLLGGHggKKKKggHGGLLLLLLLKK",
	"KLLLLLLLLGHggKK..KKggHGLLLLLLLLK",
	"KLLLLLLLLGHgKK....KKgHGLLLLLLLLK",
	"KLLLLLLLLGHgK......KgHGLLLLLLLLK",
	"KLLLLLLLLGHgK......KgHGLLLLLLLLK",
	"KLLLLLLLLGHgKK....KKgHGLLLLLLLLK",
	"KLLLLLLLLGHggKK..KKggHGLLLLLLLLK",
	"KKLLLLLLLGGHggKKKKggHGGLLLLLLLKK",
	".KLLLLLLLLGHHggggggHHGLLLLLLLLK.",
	".KLLLLLLcccGGHHHHHHGGLLLLLLLLLK.",
	".KKLLLLcccmmGGGGGGGGLLLLLLLLLKK.",
	"..KLLccccmmmyLLLLLLLLLLLLLLLLK..",
	"...KcccccmmyyLLLLLLLLLLLLLLLK...",
	"...KKcccmmmyyLLLLLLLLLLLLLLKK...",
	"....KccmmmyyLLLLLLLLLLLLLLLK....",
	".....KKmmmyyLLLLLLLLLLLLLKK.....",
	"......KKmyyyLLLLLLLLLLLLKK......",
	"........KKyLLLLLLLLLLLKK........",
	".........KKKKLLLLLLKKKK.........",
	"............KKKKKKKK............",
};

static const char *const cd16[16] = {
	"......KKKK......",
	"....KKLLLLKK....",
	"..KKWLLLLLymKK..",
	"..KWWLLLLLmmcK..",
	".KWWLLLLLymccLK.",
	".KLLLLHHHHccLLK.",
	"KLLLLHKKKKHLLLLK",
	"KLLLLHK..KHLLLLK",
	"KLLLLHK..KHLLLLK",
	"KLLLLHKKKKHLLLLK",
	".KLLccHHHHLLLLK.",
	".KLccmyLLLLLLLK.",
	"..KcmmLLLLLLLK..",
	"..KKmyLLLLLLKK..",
	"....KKLLLLKK....",
	"......KKKK......",
};

static const char *const drive32[32] = {
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
	".KWWWWWWWWWWWWWWWWWWWWWWWWWWWWK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLKKKKKKKKKKKKKKKKKKKKKKKKLGK.",
	".KWLKWWWWWWWWWWWWWWWWWWWWWWKLGK.",
	".KWLKWgggggggggggggggggggggKLGK.",
	".KWLKWgggggggggggggggggggggKLGK.",
	".KWLKGGGGGGGGGGGGGGGGGGGGGGKLGK.",
	".KWLKKKKKKKKKKKKKKKKKKKKKKKKLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLZZLLLLLLLLLLLLLLLLLLLKLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLKKKLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLKKKKKLGK.",
	".KGGGGGGGGGGGGGGGGGGGGGGGGGGGGK.",
	".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
};

static const char *const drive16[16] = {
	"................",
	"................",
	"................",
	"................",
	".KKKKKKKKKKKKKK.",
	".KWWWWWWWWWWWWK.",
	".KWKKKKKKKKKKGK.",
	".KWKWgggggggKGK.",
	".KWKGGGGGGGGKGK.",
	".KWKKKKKKKKKKGK.",
	".KWLLLLLLLLLLGK.",
	".KWZLLLLLLLKLGK.",
	".KGGGGGGGGGGGGK.",
	".KKKKKKKKKKKKKK.",
	"................",
	"................",
};

void cdrom_icons(uint8_t *media_icn, uint8_t *drive_icn, uint8_t *media_icns, uint8_t *drive_icns)
{
	floppy_icon_family(cd32, cd16, media_icn, media_icns);
	floppy_icon_family(drive32, drive16, drive_icn, drive_icns);
}
