#include "density.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#else
#include <dirent.h>
#endif

#define MIN_INCHES 10.0                 /* a display's diagonal, at least ... */
#define MAX_INCHES 150.0                /* ... and at most */

double density_from_size(unsigned px_w, unsigned px_h, double mm_w, double mm_h)
{
	if (!px_w || !px_h || !(mm_w >= 1) || !(mm_h >= 1))
		return 0;
	/* (turned, the size may be given as it was) */
	double square = (px_w * mm_h) / (px_h * mm_w), turned = (px_w * mm_w) / (px_h * mm_h);
	if (fabs(log(turned)) < fabs(log(square))) {
		double t = mm_w;
		mm_w = mm_h;
		mm_h = t;
		square = turned;
	}
	double inches = hypot(mm_w, mm_h) / 25.4;
	if (fabs(square - 1) > 0.1 || inches < MIN_INCHES || inches > MAX_INCHES)
		return 0;
	return px_w * 25.4 / mm_w;
}

bool density_edid(const uint8_t *edid, size_t size, unsigned *px_w, unsigned *px_h, double *mm_w, double *mm_h)
{
	static const uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
	*px_w = *px_h = 0;
	*mm_w = *mm_h = 0;
	if (size < 128 || memcmp(edid, header, sizeof header) != 0)
		return false;
	const uint8_t *t = edid + 54;           /* the first detailed timing, if it is one (a pixel clock) */
	if (t[0] | t[1]) {
		*px_w = t[2] | (t[4] & 0xF0u) << 4;
		*px_h = (t[5] | (t[7] & 0xF0u) << 4) << (t[17] & 0x80 ? 1 : 0);    /* (interlaced: a field's) */
		*mm_w = t[12] | (t[14] & 0xF0u) << 4;
		*mm_h = t[13] | (t[14] & 0x0Fu) << 8;
	}
	if ((!*mm_w || !*mm_h) && edid[21] && edid[22]) {
		*mm_w = edid[21] * 10.0;
		*mm_h = edid[22] * 10.0;
	}
	return *mm_w && *mm_h;
}

#ifdef __APPLE__

double density_display(int x, int y, unsigned px_w, unsigned px_h)
{
	CGDirectDisplayID id;
	uint32_t n = 0;
	if (CGGetDisplaysWithPoint(CGPointMake(x, y), 1, &id, &n) != kCGErrorSuccess || !n)
		return 0;
	CGSize mm = CGDisplayScreenSize(id);
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
	if (mode) {
		px_w = (unsigned)CGDisplayModeGetPixelWidth(mode);
		px_h = (unsigned)CGDisplayModeGetPixelHeight(mode);
		CGDisplayModeRelease(mode);
	}
	return density_from_size(px_w, px_h, mm.width, mm.height);
}

#else

static size_t read_file(const char *path, void *buf, size_t size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, size, f);
	fclose(f);
	return n;
}

double density_display(int x, int y, unsigned px_w, unsigned px_h)
{
	(void)x;
	(void)y;
	DIR *dir = opendir("/sys/class/drm");
	if (!dir)
		return 0;
	unsigned connected = 0, same = 0;
	double any[2] = { 0, 0 }, sized[2] = { 0, 0 };  /* (the millimetres of one connected, and of one that size) */
	struct dirent *e;
	while ((e = readdir(dir))) {
		/* connectors: card0-HDMI-A-1, card1-eDP-1 */
		if (strncmp(e->d_name, "card", 4) != 0 || !strchr(e->d_name, '-') || strlen(e->d_name) > 200)
			continue;
		char path[256], text[16] = "";
		uint8_t edid[256];
		snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
		if (read_file(path, text, sizeof text - 1) < 9 || strncmp(text, "connected", 9) != 0)
			continue;
		memset(text, 0, sizeof text);
		snprintf(path, sizeof path, "/sys/class/drm/%s/enabled", e->d_name);
		if (read_file(path, text, sizeof text - 1) && strncmp(text, "disabled", 8) == 0)
			continue;
		snprintf(path, sizeof path, "/sys/class/drm/%s/edid", e->d_name);
		unsigned w, h;
		double mm_w, mm_h;
		if (!density_edid(edid, read_file(path, edid, sizeof edid), &w, &h, &mm_w, &mm_h))
			continue;
		connected++;
		any[0] = mm_w;
		any[1] = mm_h;
		if ((w == px_w && h == px_h) || (w == px_h && h == px_w)) {
			same++;
			sized[0] = mm_w;
			sized[1] = mm_h;
		}
	}
	closedir(dir);
	if (same == 1)
		return density_from_size(px_w, px_h, sized[0], sized[1]);
	return connected == 1 ? density_from_size(px_w, px_h, any[0], any[1]) : 0;
}

#endif
