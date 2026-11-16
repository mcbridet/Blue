#include "ui.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "font.h"
#include "icon.h"
}

/* (SDL2's values) */
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u    /* 0xAARRGGBB words, as icon_argb() gives them */
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u    /* RGBA in memory, as Dear ImGui's textures */
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_BLENDMODE_BLEND 1
#define SDL_SCALEMODE_LINEAR 1

/* Mac OS 8's Platinum, and Bondi Blue, as Blue's site has them (site/style.css) */
#define PLATINUM IM_COL32(0xDD, 0xDD, 0xDD, 0xFF)
#define PLATINUM_LIGHT IM_COL32(0xEE, 0xEE, 0xEE, 0xFF)
#define PLATINUM_MID IM_COL32(0xBB, 0xBB, 0xBB, 0xFF)
#define PLATINUM_DARK IM_COL32(0x99, 0x99, 0x99, 0xFF)
#define PLATINUM_DARKER IM_COL32(0x77, 0x77, 0x77, 0xFF)
#define INK IM_COL32(0x11, 0x11, 0x11, 0xFF)
#define MUTED IM_COL32(0x4A, 0x4A, 0x4A, 0xFF)
#define ACCENT IM_COL32(0x00, 0x95, 0xB6, 0xFF)         /* Bondi Blue */
#define ACCENT_LIGHT IM_COL32(0xC4, 0xEC, 0xF4, 0xFF)
#define HIGHLIGHT IM_COL32(0x00, 0x56, 0x6A, 0xFF)
#define WHITE IM_COL32(0xFF, 0xFF, 0xFF, 0xFF)
#define BLACK IM_COL32(0x00, 0x00, 0x00, 0xFF)
#define SHADOW IM_COL32(0x00, 0x00, 0x00, 0x66)

#define FONT 12.0f                      /* the text's size: Chicago 12, as Mac OS had it (see font.h) */
#define TITLE_BAR 20.0f                 /* a box's title bar */
#define BUTTON_PAD 14.0f                /* a button's text from its sides ... */
#define BUTTON_HEIGHT 20.0f             /* ... and its height */
#define BUTTON_ROUNDING 4.0f
#define RING 4.0f                       /* the default button's ring, around it */
#define BOX 12.0f                       /* a checkbox */
#define ICON 32.0f                      /* Blue's icon in prompts, as a Mac OS alert's ... */
#define ICON_PIXELS 64u                 /* ... in pixels: twice that, for high-density displays */

struct ui {
	struct ui_renderer r;
	ImGuiContext *imgui;
	uint8_t *ttf;                   /* the font's file (the atlas reads it while it lives) */
	void *icon;                     /* Blue's icon (NULL: none) */
	bool icon_made;
	bool enter, enter_held;         /* Return was pressed (and is held): the default button's */
	bool broken;                    /* a texture couldn't be made: nothing is drawn */
};

/* Return, for the default button of the box in front */
struct keys {
	bool pressed, held;
};

/* (asked for by ImDrawList callbacks; our renderer has no state to reset) */
static void reset_render_state(const ImDrawList *, const ImDrawCmd *)
{
}

/* Window pixels per unit */
static float density(void)
{
	float k = ImGui::GetIO().DisplayFramebufferScale.x;
	return k > 0 ? k : 1;
}

/* A position at the nearest pixel's edge, so that lines and text land on whole pixels */
static float snap(float v)
{
	return floorf(v * density() + 0.5f) / density();
}

static ImVec2 snap(const ImVec2 &p)
{
	return ImVec2(snap(p.x), snap(p.y));
}

/* A line's width: a unit, in whole pixels */
static float hair(void)
{
	float k = density();
	return (k >= 1 ? floorf(k) : 1) / k;
}

/* A colour, faded as what it's drawn for is (disabled) */
static ImU32 col(ImU32 c)
{
	return ImGui::GetColorU32(c);
}

static ImU32 mix(ImU32 a, ImU32 b, float t)
{
	ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
	return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
						     x.w + (y.w - x.w) * t));
}

/* The vertices added since `from` shaded from `top` at y0 to `bottom` at y1 (their alpha kept) */
static void shade(ImDrawList *dl, int from, float y0, float y1, ImU32 top, ImU32 bottom)
{
	for (int k = from; k < dl->VtxBuffer.Size; k++) {
		ImDrawVert &v = dl->VtxBuffer[k];
		float t = y1 > y0 ? (v.pos.y - y0) / (y1 - y0) : 0;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		v.col = (mix(top, bottom, t) & ~IM_COL32_A_MASK) | (v.col & IM_COL32_A_MASK);
	}
}

/* A rectangle filled from `top` to `bottom` */
static void gradient(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 top, ImU32 bottom, float rounding)
{
	int from = dl->VtxBuffer.Size;
	dl->AddRectFilled(a, b, col(WHITE), rounding);
	shade(dl, from, a.y, b.y, top, bottom);
}

/* Lines inside a rectangle's top and left edges (`light`) and its bottom and right ones (`dark`), short of `r` at the corners */
static void bevel(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 light, ImU32 dark, float r)
{
	const float h = hair();
	dl->AddRectFilled(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y + h), light);
	dl->AddRectFilled(ImVec2(a.x, a.y + r), ImVec2(a.x + h, b.y - r), light);
	dl->AddRectFilled(ImVec2(a.x + r, b.y - h), ImVec2(b.x - r, b.y), dark);
	dl->AddRectFilled(ImVec2(b.x - h, a.y + r), ImVec2(b.x, b.y - r), dark);
}

struct ui *ui_open(const struct ui_renderer *r)
{
	struct ui *u = (struct ui *)calloc(1, sizeof *u);
	if (!u)
		return nullptr;
	u->r = *r;
	IMGUI_CHECKVERSION();
	u->imgui = ImGui::CreateContext();
	ImGui::GetPlatformIO().DrawCallback_ResetRenderState = reset_render_state;
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;       /* (no imgui.ini wherever we were started) */
	io.LogFilename = nullptr;
	io.BackendPlatformName = "Blue";
	io.BackendRendererName = "Blue (SDL2 renderer)";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	/*
	 * Chicago, with Dear ImGui's own font for what it hasn't, rasterized at
	 * the display's density: whole pixels to its grid's squares, and to
	 * its glyphs' advances, so that it's as sharp as Mac OS's
	 */
	size_t size = 0;
	u->ttf = font_ttf(&size);
	ImFontConfig chicago;
	chicago.FontDataOwnedByAtlas = false;
	chicago.ExtraSizeScale = font_em_scale;
	chicago.PixelSnapH = true;
	chicago.OversampleH = chicago.OversampleV = 1;
	ImFont *font = u->ttf ? io.Fonts->AddFontFromMemoryTTF(u->ttf, (int)size, 0, &chicago) : nullptr;
	ImFontConfig rest;
	rest.MergeMode = font != nullptr;
	io.Fonts->AddFontDefaultVector(&rest);

	ImGui::StyleColorsLight();
	ImGuiStyle &style = ImGui::GetStyle();
	style.FontSizeBase = FONT;
	style.WindowRounding = 0;
	style.WindowBorderSize = 1;
	style.PopupRounding = 0;
	style.PopupBorderSize = 1;
	style.FrameRounding = BUTTON_ROUNDING;
	style.WindowPadding = ImVec2(16, 12);
	style.FramePadding = ImVec2(BUTTON_PAD, (BUTTON_HEIGHT - FONT) / 2);
	style.ItemSpacing = ImVec2(8, 6);
	style.DisabledAlpha = 0.4f;     /* (black on Platinum: Mac OS's grey) */
	ImVec4 *c = style.Colors;
	c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(INK);
	c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(MUTED);
	c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(PLATINUM);
	c[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(WHITE);
	c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(BLACK);
	c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(ACCENT);
	c[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(ACCENT_LIGHT);
	c[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(PLATINUM_DARK);
	c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);     /* (the widgets below draw their own, in Bondi Blue) */
	return u;
}

void ui_close(struct ui *u)
{
	if (!u)
		return;
	ImGui::SetCurrentContext(u->imgui);
	for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
		if (tex->TexID != ImTextureID_Invalid)
			u->r.DestroyTexture((void *)(intptr_t)tex->TexID);
	ImGui::DestroyContext(u->imgui);
	if (u->icon)
		u->r.DestroyTexture(u->icon);
	free(u->ttf);
	free(u);
}

void ui_mouse_move(struct ui *u, float x, float y)
{
	ImGui::SetCurrentContext(u->imgui);
	ImGui::GetIO().AddMousePosEvent(x, y);
}

void ui_mouse_button(struct ui *u, int button, bool down)
{
	ImGui::SetCurrentContext(u->imgui);
	if (button >= 0 && button < ImGuiMouseButton_COUNT)
		ImGui::GetIO().AddMouseButtonEvent(button, down);
}

void ui_mouse_wheel(struct ui *u, float dx, float dy)
{
	ImGui::SetCurrentContext(u->imgui);
	ImGui::GetIO().AddMouseWheelEvent(dx, dy);
}

/* (Return is the default button's, not the focused one's: Space is that one's) */
void ui_key(struct ui *u, enum ui_key key, bool down)
{
	static const ImGuiKey keys[] = { ImGuiKey_Tab, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow,
					 ImGuiKey_DownArrow, ImGuiKey_Space };
	ImGui::SetCurrentContext(u->imgui);
	if (key == UI_KEY_ENTER) {
		u->enter |= down && !u->enter_held;
		u->enter_held = down;
	} else if ((unsigned)key < sizeof keys / sizeof keys[0]) {
		ImGui::GetIO().AddKeyEvent(keys[key], down);
	}
}

/* Dear ImGui's requests for its textures (the font's glyphs): made, added to, let go of */
static void update_texture(struct ui *u, ImTextureData *tex)
{
	void *t = (void *)(intptr_t)tex->TexID;
	if (tex->Status == ImTextureStatus_WantCreate) {
		t = u->r.CreateTexture(u->r.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, tex->Width,
				       tex->Height);
		if (!t) {
			u->broken = true;
			return;
		}
		u->r.UpdateTexture(t, nullptr, tex->GetPixels(), tex->GetPitch());
		u->r.SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
		if (u->r.SetTextureScaleMode)
			u->r.SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
		tex->SetTexID((ImTextureID)(intptr_t)t);
		tex->SetStatus(ImTextureStatus_OK);
	} else if (tex->Status == ImTextureStatus_WantUpdates) {
		for (const ImTextureRect &r : tex->Updates) {
			struct ui_rect rect = { r.x, r.y, r.w, r.h };
			u->r.UpdateTexture(t, &rect, tex->GetPixelsAt(r.x, r.y), tex->GetPitch());
		}
		tex->SetStatus(ImTextureStatus_OK);
	} else if (tex->Status == ImTextureStatus_WantDestroy) {
		if (t)
			u->r.DestroyTexture(t);
		tex->SetTexID(ImTextureID_Invalid);
		tex->SetStatus(ImTextureStatus_Destroyed);
	}
}

/* Blue's icon, for prompts: made the first time (ICON_PIXELS square, each pixel the average of those it covers) */
static void *icon(struct ui *u)
{
	if (u->icon_made)
		return u->icon;
	u->icon_made = true;
	const unsigned n = ICON_SIZE / ICON_PIXELS;
	uint32_t *big = (uint32_t *)malloc(ICON_SIZE * ICON_SIZE * sizeof *big);
	uint32_t *small = (uint32_t *)malloc(ICON_PIXELS * ICON_PIXELS * sizeof *small);
	if (big && small && icon_argb(big)) {
		for (unsigned y = 0; y < ICON_PIXELS; y++) {
			for (unsigned x = 0; x < ICON_PIXELS; x++) {
				uint32_t sum[4] = { 0, 0, 0, 0 };          /* (colours weighted by their alpha) */
				for (unsigned dy = 0; dy < n; dy++) {
					for (unsigned dx = 0; dx < n; dx++) {
						uint32_t p = big[(y * n + dy) * ICON_SIZE + x * n + dx], a = p >> 24;
						sum[0] += a;
						for (unsigned c = 1; c < 4; c++)
							sum[c] += (p >> (24 - 8 * c) & 0xFF) * a;
					}
				}
				uint32_t px = (sum[0] + n * n / 2) / (n * n) << 24;
				for (unsigned c = 1; c < 4 && sum[0]; c++)
					px |= (sum[c] + sum[0] / 2) / sum[0] << (24 - 8 * c);
				small[y * ICON_PIXELS + x] = px;
			}
		}
		u->icon = u->r.CreateTexture(u->r.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
					     ICON_PIXELS, ICON_PIXELS);
		if (u->icon) {
			u->r.UpdateTexture(u->icon, nullptr, small, ICON_PIXELS * sizeof *small);
			u->r.SetTextureBlendMode(u->icon, SDL_BLENDMODE_BLEND);
			if (u->r.SetTextureScaleMode)
				u->r.SetTextureScaleMode(u->icon, SDL_SCALEMODE_LINEAR);
		}
	}
	free(big);
	free(small);
	return u->icon;
}

/*
 * The draw lists, as triangles in the window's units: the renderer is
 * scaled to pixels meanwhile (and so are the clip rectangles).
 */
static void render(struct ui *u, ImDrawData *dd)
{
	if (dd->Textures)
		for (ImTextureData *tex : *dd->Textures)
			if (tex->Status != ImTextureStatus_OK)
				update_texture(u, tex);
	if (u->broken)
		return;
	u->r.RenderSetScale(u->r.renderer, dd->FramebufferScale.x, dd->FramebufferScale.y);
	for (const ImDrawList *list : dd->CmdLists) {
		const ImDrawVert *vtx = list->VtxBuffer.Data;
		const ImDrawIdx *idx = list->IdxBuffer.Data;
		for (const ImDrawCmd &cmd : list->CmdBuffer) {
			if (cmd.UserCallback) {
				if (cmd.UserCallback != ImGui::GetPlatformIO().DrawCallback_ResetRenderState)
					cmd.UserCallback(list, &cmd);
				continue;
			}
			float x0 = cmd.ClipRect.x - dd->DisplayPos.x, y0 = cmd.ClipRect.y - dd->DisplayPos.y;
			float x1 = cmd.ClipRect.z - dd->DisplayPos.x, y1 = cmd.ClipRect.w - dd->DisplayPos.y;
			x0 = x0 > 0 ? x0 : 0;
			y0 = y0 > 0 ? y0 : 0;
			x1 = x1 < dd->DisplaySize.x ? x1 : dd->DisplaySize.x;
			y1 = y1 < dd->DisplaySize.y ? y1 : dd->DisplaySize.y;
			if (x1 <= x0 || y1 <= y0)
				continue;
			struct ui_rect clip = { (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0) };
			u->r.RenderSetClipRect(u->r.renderer, &clip);
			const ImDrawVert *v = vtx + cmd.VtxOffset;
			/* (colors are RGBA bytes, as SDL_Color) */
			u->r.RenderGeometryRaw(u->r.renderer, (void *)(intptr_t)cmd.GetTexID(), &v->pos.x, (int)sizeof *v,
					       &v->col, (int)sizeof *v, &v->uv.x, (int)sizeof *v,
					       list->VtxBuffer.Size - (int)cmd.VtxOffset, idx + cmd.IdxOffset,
					       (int)cmd.ElemCount, (int)sizeof *idx);
		}
	}
	u->r.RenderSetClipRect(u->r.renderer, nullptr);
	u->r.RenderSetScale(u->r.renderer, 1, 1);
}

#define LINE_GAP 3.0f                   /* between lines of text (Mac OS's Chicago 12 had 15 or 16 to a line) */

/*
 * Text wrapped to `width`, breaking lines only at spaces: Dear ImGui's own
 * wrapping also breaks after punctuation, quotes among it, which could leave
 * an opening quote at the end of a line, apart from the word it opens. (A
 * word wider than `width` is wrapped as Dear ImGui wraps it.)
 */
static void text_wrapped(const char *text, float width)
{
	char buf[2048];
	size_t n = strlen(text);
	if (n < sizeof buf) {
		memcpy(buf, text, n + 1);
		for (char *line = buf, *p = buf; *p;) {         /* (p: after a word, or at the start) */
			char *next = p;
			while (*next == ' ')
				next++;
			while (*next && *next != ' ' && *next != '\n')
				next++;
			if (p != line && *p == ' ' && ImGui::CalcTextSize(line, next).x > width) {
				*p = '\n';
				line = ++p;
			} else if (*next == '\n') {
				line = p = next + 1;
			} else {
				p = next;
			}
		}
		text = buf;
	}
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width + 1);
	ImGui::BeginGroup();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, LINE_GAP));
	for (const char *line = text;;) {               /* (a line at a time, LINE_GAP apart) */
		const char *end = strchr(line, '\n');
		ImGui::TextUnformatted(line, end ? end : line + strlen(line));
		if (!end)
			break;
		line = end + 1;
	}
	ImGui::PopStyleVar();
	ImGui::EndGroup();
	ImGui::PopTextWrapPos();
}

/* ---- Platinum: boxes and their controls -------------------------------------------- */

/* (whole units, so that its edges land on whole pixels: see snap()) */
static void whole_size(ImGuiSizeCallbackData *data)
{
	data->DesiredSize = ImVec2(ceilf(data->DesiredSize.x), ceilf(data->DesiredSize.y));
}

/*
 * A box in the middle of the window, as a Mac OS 8 dialog: Platinum,
 * bevelled inside a black frame, with a shadow, and its title centred in
 * a striped title bar. Its contents follow, below the bar.
 */
static void begin_box(const char *name, const char *title)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, FLT_MAX), whole_size);
	ImGui::Begin(name, nullptr,
		     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
			     ImGuiWindowFlags_NoScrollbar);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	const ImVec2 b(a.x + size.x, a.y + size.y);
	const float h = hair();
	dl->PushClipRect(ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4), false);
	dl->AddRectFilled(ImVec2(a.x + 2, b.y), ImVec2(b.x + 2, b.y + 2), SHADOW);
	dl->AddRectFilled(ImVec2(b.x, a.y + 2), ImVec2(b.x + 2, b.y), SHADOW);
	bevel(dl, ImVec2(a.x + h, a.y + h), ImVec2(b.x - h, b.y - h), WHITE, PLATINUM_DARK, 0);

	/* the title bar's stripes (6 light lines, 5 dark ones), with the title over them */
	const float top = a.y + floorf((TITLE_BAR - 11) / 2);
	for (int k = 0; k < 11; k++)
		dl->AddRectFilled(ImVec2(a.x + 8, top + k), ImVec2(b.x - 8, top + k + h), k & 1 ? PLATINUM_DARK : WHITE);
	const ImVec2 t = ImGui::CalcTextSize(title);
	const float x = snap(a.x + (size.x - t.x) / 2);
	dl->AddRectFilled(ImVec2(x - 8, top), ImVec2(x + t.x + 8, top + 11), PLATINUM);
	dl->AddText(ImVec2(x, snap(a.y + (TITLE_BAR - ImGui::GetFontSize()) / 2)), INK, title);
	dl->PopClipRect();
	ImGui::SetCursorPosY(TITLE_BAR + 6);
}

static void end_box(void)
{
	ImGui::End();
}

/* Draws into the whole box (a default button's ring and focus rings go outside its contents) */
static ImDrawList *box_draw_list(void)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	dl->PushClipRect(a, ImVec2(a.x + size.x, a.y + size.y), false);
	return dl;
}

/* The keyboard's focus (Tab, the arrows; Space presses), as Mac OS 8 draws it: a ring in the highlight colour */
static void focus_ring(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, float out, float rounding)
{
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
		dl->AddRect(ImVec2(a.x - out, a.y - out), ImVec2(b.x + out, b.y + out), col(ACCENT), rounding + out, 2);
}

/* A Mac OS 8 line between parts of a box: etched, a dark line over a light one */
static void separator(void)
{
	ImGui::Dummy(ImVec2(0, 1));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float h = hair(), y = snap(ImGui::GetCursorScreenPos().y);
	const float x0 = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x;
	const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
	dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + h), PLATINUM_DARK);
	dl->AddRectFilled(ImVec2(x0, y + h), ImVec2(x1, y + 2 * h), WHITE);
	ImGui::Dummy(ImVec2(0, 2 * h + 1));
}

/*
 * A push button (`width` 0: as wide as its label needs): bevelled, black
 * framed and rounded, dark while pressed. The default one has Mac OS's
 * thick ring around it, and Return presses it (`keys`).
 */
static bool button(const char *label, float width, const struct keys *keys = nullptr)
{
	const ImVec2 t = ImGui::CalcTextSize(label);
	const ImVec2 size(snap(width > 0 ? width : t.x + 2 * BUTTON_PAD), BUTTON_HEIGHT);
	const ImVec2 a = snap(ImGui::GetCursorScreenPos()), b(a.x + size.x, a.y + size.y);
	bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
	bool down = ImGui::IsItemActive() && ImGui::IsItemHovered(), hovered = ImGui::IsItemHovered();
	if (keys) {
		pressed |= keys->pressed;
		down |= keys->held;
	}
	ImDrawList *dl = box_draw_list();
	const float h = hair(), r = BUTTON_ROUNDING;
	if (keys) {
		dl->AddRectFilled(ImVec2(a.x - RING, a.y - RING), ImVec2(b.x + RING, b.y + RING), col(BLACK), r + RING);
		gradient(dl, ImVec2(a.x - RING + h, a.y - RING + h), ImVec2(b.x + RING - h, b.y + RING - h), PLATINUM_MID,
			 PLATINUM_DARK, r + RING - h);
		dl->AddRectFilled(ImVec2(a.x - h, a.y - h), ImVec2(b.x + h, b.y + h), col(WHITE), r + h);
	}
	dl->AddRectFilled(a, b, col(BLACK), r);
	const ImVec2 ia(a.x + h, a.y + h), ib(b.x - h, b.y - h);
	if (down) {
		dl->AddRectFilled(ia, ib, col(IM_COL32(0x6B, 0x6B, 0x6B, 0xFF)), r - h);
		bevel(dl, ia, ib, col(IM_COL32(0x3A, 0x3A, 0x3A, 0xFF)), col(PLATINUM_DARKER), r - h);
	} else {
		gradient(dl, ia, ib, hovered ? WHITE : IM_COL32(0xF6, 0xF6, 0xF6, 0xFF),
			 hovered ? IM_COL32(0xDC, 0xDC, 0xDC, 0xFF) : IM_COL32(0xD2, 0xD2, 0xD2, 0xFF), r - h);
		bevel(dl, ia, ib, col(WHITE), col(PLATINUM_DARK), r - h);
	}
	dl->AddText(snap(ImVec2(a.x + (size.x - t.x) / 2, a.y + (size.y - ImGui::GetFontSize()) / 2)),
		    col(down ? WHITE : INK), label);
	focus_ring(dl, a, b, keys ? RING + 2 : 2, r);
	dl->PopClipRect();
	return pressed;
}

/* A checkbox, as Mac OS 8's (its check overhanging the box), checked in Bondi Blue */
static bool checkbox(const char *label, bool *on)
{
	const float font = ImGui::GetFontSize(), height = font + 6;
	const ImVec2 t = ImGui::CalcTextSize(label), at = ImGui::GetCursorScreenPos();
	bool pressed = ImGui::InvisibleButton(label, ImVec2(BOX + 7 + t.x, height), ImGuiButtonFlags_EnableNav);
	if (pressed)
		*on = !*on;
	bool down = ImGui::IsItemActive() && ImGui::IsItemHovered(), hovered = ImGui::IsItemHovered();
	ImDrawList *dl = box_draw_list();
	const float h = hair();
	const ImVec2 a = snap(ImVec2(at.x, at.y + (height - BOX) / 2)), b(a.x + BOX, a.y + BOX);
	dl->AddRectFilled(a, b, col(BLACK));
	const ImVec2 ia(a.x + h, a.y + h), ib(b.x - h, b.y - h);
	if (down) {
		dl->AddRectFilled(ia, ib, col(PLATINUM_DARK));
		bevel(dl, ia, ib, col(PLATINUM_DARKER), col(PLATINUM_MID), 0);
	} else {
		gradient(dl, ia, ib, hovered ? WHITE : PLATINUM_LIGHT, hovered ? PLATINUM_LIGHT : PLATINUM, 0);
		bevel(dl, ia, ib, col(WHITE), col(PLATINUM_DARK), 0);
	}
	if (*on) {
		const float k = BOX / 12;
		const ImVec2 p[] = { ImVec2(a.x + 3 * k, a.y + 6 * k), ImVec2(a.x + 5.5f * k, a.y + 9 * k),
				     ImVec2(a.x + 13.5f * k, a.y - 1.5f * k) };
		dl->AddPolyline(p, 3, col(HIGHLIGHT), 3.6f * k);
		dl->AddPolyline(p, 3, col(ACCENT), 2 * k);
	}
	dl->AddText(snap(ImVec2(b.x + 7, at.y + (height - font) / 2)), col(INK), label);
	focus_ring(dl, a, b, 2, 1);
	dl->PopClipRect();
	return pressed;
}

/* A radio button, as Mac OS 8's (round, shaded, a dot when it's the one on), its dot in Bondi Blue */
static bool radio(const char *id, const char *label, bool on)
{
	const float font = ImGui::GetFontSize(), height = font + 6;
	const ImVec2 t = ImGui::CalcTextSize(label), at = ImGui::GetCursorScreenPos();
	bool pressed = ImGui::InvisibleButton(id, ImVec2(BOX + 6 + t.x, height), ImGuiButtonFlags_EnableNav);
	bool down = ImGui::IsItemActive() && ImGui::IsItemHovered(), hovered = ImGui::IsItemHovered();
	ImDrawList *dl = box_draw_list();
	const float h = hair(), r = BOX / 2;
	const ImVec2 c = snap(ImVec2(at.x + r, at.y + height / 2));
	dl->AddCircleFilled(c, r, col(BLACK));
	int from = dl->VtxBuffer.Size;
	dl->AddCircleFilled(c, r - h, col(WHITE));
	if (down)
		shade(dl, from, c.y - r, c.y + r, PLATINUM_DARKER, PLATINUM_MID);
	else
		shade(dl, from, c.y - r, c.y + r, hovered ? WHITE : PLATINUM_LIGHT, hovered ? PLATINUM : PLATINUM_MID);
	if (on) {
		dl->AddCircleFilled(c, r / 2, col(HIGHLIGHT));
		dl->AddCircleFilled(c, r / 2 - h, col(ACCENT));
	}
	dl->AddText(snap(ImVec2(at.x + BOX + 6, at.y + (height - font) / 2)), col(INK), label);
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
		dl->AddCircle(c, r + 2, col(ACCENT), 0, 2);
	dl->PopClipRect();
	return pressed;
}

/* What an item does, in a tooltip while the pointer rests on it (Mac OS 8's Balloon Help, plainer) */
static void tip(const char *text)
{
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
		return;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 5));
	ImGui::BeginTooltip();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	dl->PushClipRect(a, ImVec2(a.x + size.x + 2, a.y + size.y + 2), false);
	dl->AddRectFilled(ImVec2(a.x + 2, a.y + size.y), ImVec2(a.x + size.x + 2, a.y + size.y + 2), SHADOW);
	dl->AddRectFilled(ImVec2(a.x + size.x, a.y + 2), ImVec2(a.x + size.x + 2, a.y + size.y), SHADOW);
	dl->PopClipRect();
	text_wrapped(text, 300);
	ImGui::EndTooltip();
	ImGui::PopStyleVar();
}

/* Text beside buttons, in the middle of their height */
static void beside_buttons(void)
{
	ImGui::SameLine();
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (BUTTON_HEIGHT - ImGui::GetFontSize()) / 2);
}

/* ---- the menu and prompts ---------------------------------------------------------- */

#define ELLIPSIS "\xE2\x80\xA6"

/*
 * The drives with removable media: what's in each, with an Eject button
 * (disabled while Mac OS uses the disc: it's the one to eject it then) and
 * an Insert... button (the host's file chooser: the image chosen goes in
 * that drive, as if dropped on the window).
 */
static enum ui_action drives_menu(const struct ui_drives *dr)
{
	enum ui_action action = UI_NONE;
	bool in_use = false;
	separator();
	for (unsigned k = 0; k < dr->count && k < UI_DRIVES; k++) {
		const struct ui_drive *d = &dr->drive[k];
		ImGui::PushID((int)k);
		ImGui::BeginDisabled(!d->in[0] || d->in_use);
		if (button("Eject", 64))
			action = (enum ui_action)(UI_EJECT + k);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (button("Insert" ELLIPSIS, 0))
			action = (enum ui_action)(UI_INSERT + k);
		beside_buttons();
		ImGui::Text("%s: %s%s", d->name, d->in[0] ? d->in : "empty", d->locked ? " (locked)" : "");
		ImGui::PopID();
		in_use |= d->in_use;
	}
	if (in_use)
		ImGui::TextDisabled("(Mac OS is using a disc: drag it to the Trash there to eject it)");
	if (dr->next[0]) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480);
		ImGui::TextDisabled("Next floppies, as Mac OS ejects: %s", dr->next);
		ImGui::PopTextWrapPos();
	}
	ImGui::TextDisabled("(or drop an image on the window to put it in)");
	return action;
}

#define MENU_BUTTON 240.0f
#define TIMES "\xC3\x97"

/*
 * The window's scale: Auto (saying what it chose) or a number of the
 * display's pixels to a Mac pixel, as radio buttons on a line
 */
static enum ui_action scale_menu(const struct ui_state *s)
{
	enum ui_action action = UI_NONE;
	const float y = ImGui::GetCursorPosY();
	ImGui::SetCursorPosY(y + 3);
	ImGui::TextUnformatted("Scale:");
	for (unsigned k = 0; k < UI_SCALES; k++) {
		char id[16], label[32], why[400];
		snprintf(id, sizeof id, "##scale%u", k);
		if (k) {
			snprintf(label, sizeof label, "%u" TIMES, k);
			snprintf(why, sizeof why, "Each Mac pixel %u " TIMES " %u of the display's pixels", k, k);
		} else {
			snprintf(label, sizeof label, "Auto (%u" TIMES ")", s->scale_auto);
			snprintf(why, sizeof why,
				 "The whole number of the display's pixels to a Mac pixel that makes Mac OS about 80 pixels "
				 "to the inch, as a 1999 Mac's screen was: %u on this display. With Blue Enhancements, Mac "
				 "OS's resolution changes to fill the window (as Mac OS resumes); without, the window's size "
				 "changes", s->scale_auto);
		}
		ImGui::SameLine(0, k ? 12 : 10);
		ImGui::SetCursorPosY(y);
		if (radio(id, label, s->scale == k))
			action = (enum ui_action)(UI_SCALE + k);
		tip(why);
	}
	return action;
}

/* The menu: the guest is paused while it's open */
static enum ui_action menu(const struct ui_state *s, const struct keys *keys)
{
	enum ui_action action = UI_NONE;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
						      IM_COL32(0, 0, 0, 110));
	begin_box("Blue: paused", "Blue: paused");
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + RING);
	if (button("Resume", MENU_BUTTON, keys))
		action = UI_RESUME;
	ImGui::SetItemDefaultFocus();
	ImGui::Dummy(ImVec2(0, RING - ImGui::GetStyle().ItemSpacing.y + 4));
	if (button("Restart", MENU_BUTTON))
		action = UI_RESTART;
	if (button("Shut down" ELLIPSIS, MENU_BUTTON))
		action = UI_SHUT_DOWN;
	if (button("Turn off", MENU_BUTTON))
		action = UI_TURN_OFF;
	tip("At once, as if the power went: unsaved work is lost");
	if (s->tools) {
		separator();
		ImGui::BeginDisabled(s->tools_why_not != nullptr);
		if (button("Install Guest Tools" ELLIPSIS, MENU_BUTTON))
			action = UI_INSTALL_TOOLS;
		ImGui::EndDisabled();
		tip(s->tools_why_not ? s->tools_why_not :
				       "Offers to install Blue's guest tools on the startup disk, as Blue does by itself "
				       "unless told not to (Don't Ask Again)");
	}
	if (s->drives) {
		enum ui_action chosen = drives_menu(s->drives);
		if (chosen != UI_NONE)
			action = chosen;
	}

	separator();
	if (button("Screenshot", MENU_BUTTON))
		action = UI_SCREENSHOT;
	bool fullscreen = s->fullscreen;
	if (checkbox("Fullscreen", &fullscreen))
		action = UI_FULLSCREEN;
	enum ui_action scale = scale_menu(s);
	if (scale != UI_NONE)
		action = scale;
	bool bar = s->status_bar > 0;
	if (checkbox("Status bar", &bar))
		action = UI_STATUS_BAR;
	bool capture = s->mouse == UI_MOUSE_CAPTURED;
	ImGui::BeginDisabled(s->mouse == UI_MOUSE_FOLLOWS);
	if (checkbox("Capture the mouse", &capture))
		action = capture ? UI_CAPTURE_MOUSE : UI_RELEASE_MOUSE;
	ImGui::EndDisabled();
	ImGui::TextDisabled(s->mouse == UI_MOUSE_FOLLOWS ? "(the Mac shows its cursor: the mouse follows the pointer)" :
			    "(while the Mac hides its cursor; Control-Option releases it)");
	if (s->news) {
		separator();
		text_wrapped(s->news, 480);     /* (the drives' news can be long) */
	}
	separator();
	ImGui::TextDisabled("F12 or Esc resumes");
	end_box();
	return action;
}

#define PROMPT_WIDTH 380.0f             /* (its text's) */
#define PROMPT_BUTTON 86.0f             /* (Mac OS's buttons were at least 58 wide; ours as wide as their labels need) */
#define PROMPT_GAP 12.0f                /* (between buttons; the third, on the left, further) */

/*
 * A question (or news) in a box over the screen, the guest running on
 * underneath, as a Mac OS alert: Blue's icon, the text beside it, and the
 * buttons below, the default last, on the right, and a third on the left.
 * While the menu is open, it waits.
 */
static enum ui_action prompt(struct ui *u, const struct ui_state *s, const struct keys *keys)
{
	enum ui_action action = UI_NONE;
	begin_box("Blue##prompt", "Blue");
	ImGui::BeginDisabled(s->menu);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
	void *tex = icon(u);
	float x = ImGui::GetCursorPosX();
	if (tex) {
		const ImVec2 a = snap(ImGui::GetCursorScreenPos());
		ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)tex, a, ImVec2(a.x + ICON, a.y + ICON),
						     ImVec2(0, 0), ImVec2(1, 1), col(WHITE));
		ImGui::Dummy(ImVec2(ICON, ICON));
		ImGui::SameLine(0, 16);
		x = ImGui::GetCursorPosX();
	}
	text_wrapped(s->prompt, PROMPT_WIDTH);
	ImGui::Dummy(ImVec2(0, 8));
	/* (as wide as the wider label needs: "Put Back and Restart"; a third button on the left, apart) */
	const char *const labels[] = { s->prompt_no, s->prompt_yes };
	float wide = PROMPT_BUTTON;
	for (const char *label : labels)
		if (label && ImGui::CalcTextSize(label).x + 2 * BUTTON_PAD > wide)
			wide = ImGui::CalcTextSize(label).x + 2 * BUTTON_PAD;
	float buttons = s->prompt_no ? 2 * wide + PROMPT_GAP : wide;
	ImGui::SetCursorPosX(x);
	if (s->prompt_alt) {
		float alt = ImGui::CalcTextSize(s->prompt_alt).x + 2 * BUTTON_PAD;
		if (button(s->prompt_alt, alt > PROMPT_BUTTON ? alt : PROMPT_BUTTON))
			action = UI_PROMPT_ALT;
		ImGui::SameLine(0, 2 * PROMPT_GAP);
	}
	if (x + PROMPT_WIDTH - buttons > ImGui::GetCursorPosX())
		ImGui::SetCursorPosX(x + PROMPT_WIDTH - buttons);
	if (s->prompt_no) {
		if (button(s->prompt_no, wide))
			action = UI_PROMPT_NO;
		ImGui::SameLine(0, PROMPT_GAP);
	}
	if (button(s->prompt_yes, wide, keys))
		action = UI_PROMPT_YES;
	if (!s->menu)
		ImGui::SetItemDefaultFocus();
	ImGui::EndDisabled();
	end_box();
	return action;
}

/* ---- the status bar ---------------------------------------------------------------- */

/* A speed in millions of instructions a second, as a clock's (one instruction a cycle) */
static void format_speed(char *out, size_t size, uint32_t mhz)
{
	if (!mhz)
		snprintf(out, size, " ");
	else if (mhz < 1000)
		snprintf(out, size, "%u MHz", (unsigned)mhz);
	else
		snprintf(out, size, "%.2f GHz", mhz / 1000.0);
}

/* A rate, in bytes a second (or bits, for networks), in thousands and so on */
static void format_rate(char *out, size_t size, uint64_t bytes, bool bits)
{
	static const char *const byte_units[] = { "B/s", "KB/s", "MB/s", "GB/s" };
	static const char *const bit_units[] = { "bps", "kbps", "Mbps", "Gbps" };
	double v = (double)bytes * (bits ? 8 : 1);
	int u = 0;
	for (; u < 3 && v >= 999.5; u++)
		v /= 1000;
	snprintf(out, size, u && v < 9.95 ? "%.1f %s" : "%.0f %s", v, (bits ? bit_units : byte_units)[u]);
}

#define BAR_TEXT 12.0f                  /* the status bar's text size, at its largest */
#define BAR_TEXT_MIN 10.0f              /* ... and its smallest, in narrow windows */

/* (on Platinum: lights as an LED's, the arrows' colours darker to read on it) */
static const ImU32 bar_label = INK, bar_dim = IM_COL32(0x8C, 0x8C, 0x8C, 0xFF), bar_quiet = MUTED;
static const ImU32 bar_off = IM_COL32(0xC4, 0xC4, 0xC4, 0xFF), bar_green = IM_COL32(0x2B, 0xD1, 0x48, 0xFF);
static const ImU32 bar_amber = IM_COL32(0xF5, 0xA6, 0x23, 0xFF), bar_blue = ACCENT;
static const ImU32 arrow_green = IM_COL32(0x1C, 0x9A, 0x37, 0xFF), arrow_amber = IM_COL32(0xC8, 0x80, 0x00, 0xFF);
static const ImU32 arrow_idle = IM_COL32(0xA8, 0xA8, 0xA8, 0xFF);
#define BAR_NAP 0.7f                    /* (napping, the processor's light goes this far towards blue: softly) */

/* Text in a slot as wide as the widest of `widest` (so that what follows stays put); returns its width */
static float bar_text(ImDrawList *dl, ImFont *font, float size, float x, float mid, ImU32 col, const char *text,
		      const char *const *widest)
{
	if (dl)
		dl->AddText(font, size, snap(ImVec2(x, mid - size / 2)), col, text);
	float w = font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
	for (; widest && *widest; widest++) {
		float v = font->CalcTextSizeA(size, FLT_MAX, 0, *widest).x;
		w = v > w ? v : w;
	}
	return w;
}

/* The text's size at k times its largest (in whole units: each size is rasterized anew) */
static float bar_size(float k)
{
	return floorf(BAR_TEXT * k + 0.25f);
}

/* A rate after an arrow (down: read or received, green while it's going; up: written or sent, amber); returns its width */
static float bar_rate(ImDrawList *dl, ImFont *font, float k, float x, float mid, uint64_t bytes, bool out, bool bits)
{
	if (dl) {
		ImU32 col = !bytes ? arrow_idle : out ? arrow_amber : arrow_green;
		float cx = x + 3.5f * k, w = 3.5f * k, h = 3 * k;
		/* (clockwise, for Dear ImGui's anti-aliasing) */
		if (out)
			dl->AddTriangleFilled(ImVec2(cx, mid - h), ImVec2(cx + w, mid + h), ImVec2(cx - w, mid + h), col);
		else
			dl->AddTriangleFilled(ImVec2(cx - w, mid - h), ImVec2(cx + w, mid - h), ImVec2(cx, mid + h), col);
	}
	char text[32];
	format_rate(text, sizeof text, bytes, bits);
	float w = 7 * k + 4 * k;
	return w + bar_text(dl, font, bar_size(k), x + w, mid, bytes ? bar_label : bar_dim, text, nullptr);
}

/* ... and the widest one */
static float bar_rate_widest(ImFont *font, float k)
{
	static const char *const widest[] = { "000 KB/s", "0.0 MB/s", "000 kbps", "0.0 Mbps", nullptr };
	return 7 * k + 4 * k + bar_text(nullptr, font, bar_size(k), 0, 0, 0, "", widest);
}

/* A light: a round LED, rimmed, with a gleam; a glow around it as it's lit */
static void bar_light(ImDrawList *dl, const ImVec2 &centre, float k, ImU32 c, ImU32 glow, float lit)
{
	const float r = 4.5f * k;
	if (lit > 0)
		dl->AddCircleFilled(centre, 8 * k, (glow & ~IM_COL32_A_MASK) | (ImU32)(0x48 * lit) << IM_COL32_A_SHIFT);
	dl->AddCircleFilled(centre, r + hair(), mix(c, BLACK, 0.45f));
	dl->AddCircleFilled(centre, r, c);
	dl->AddCircleFilled(ImVec2(centre.x - 1.4f * k, centre.y - 1.4f * k), 1.6f * k, IM_COL32(0xFF, 0xFF, 0xFF, 0x90));
}

/*
 * Lays out (and draws, with a draw list) the status bar's contents from x
 * at k times their largest size, with brief names or not; returns their
 * width.
 */
static float bar_contents(const struct ui_state *s, ImDrawList *dl, ImFont *font, float x, float mid, float k,
			  bool brief)
{
	static const char *const names[UI_LEDS] = { "CPU", "Disk", "CD-ROM", "Floppy", "Network" };
	static const char *const brief_names[UI_LEDS] = { "CPU", "Disk", "CD", "FD", "Net" };
	static const char *const widest_speed[] = { "000 MHz", "00.00 GHz", nullptr };
	const float x0 = x, size = bar_size(k);
	for (int n = 0; n < UI_LEDS; n++) {
		const struct ui_light *l = &s->bar.light[n];
		if (l->hidden)
			continue;
		if (n)
			x += 20 * k;
		if (dl) {
			/* (off, then softly blue as much as it naps, then lit as much as it's busy) */
			const float t = l->level / 255.0f, nap = l->nap / 255.0f * BAR_NAP;
			const ImU32 on = l->write ? bar_amber : bar_green;
			const ImU32 c = mix(mix(bar_off, bar_blue, nap), on, t);
			bar_light(dl, ImVec2(snap(x + 5 * k), mid), k, c, t >= nap ? on : bar_blue, t > nap ? t : nap);
		}
		x += 17 * k;
		x += bar_text(dl, font, size, x, mid, l->absent ? bar_dim : bar_label, (brief ? brief_names : names)[n],
			      nullptr);
		if (l->absent)
			continue;
		x += 8 * k;
		if (n == UI_LED_CPU) {
			char speed[32];
			format_speed(speed, sizeof speed, s->bar.mhz);
			x += bar_text(dl, font, size, x, mid, bar_quiet, speed, widest_speed);
			continue;
		}
		/* (each rate as wide as it is; the pair takes the room the widest would, so that what follows stays put) */
		const float from = x;
		float room = bar_rate_widest(font, k);
		x += bar_rate(dl, font, k, x, mid, s->bar.in[n], false, n == UI_LED_NET);
		if (n != UI_LED_CDROM) {
			x += 7 * k;
			x += bar_rate(dl, font, k, x, mid, s->bar.out[n], true, n == UI_LED_NET);
			room = 2 * room + 7 * k;
		}
		x = x - from > room ? x : from + room;
	}
	return x - x0;
}

/*
 * The status bar, along the bottom of the window, Platinum as a Mac OS 9
 * window's edge: a light for each device (green, or amber while written
 * to; dimmed if there's no such device, or no disk in the floppy drive,
 * whose light is there only with one), with what it reads and writes a
 * second, and the processor's lit as brightly as it's busy (Bondi Blue as
 * it naps), with its speed alongside. In narrow windows, the hint goes,
 * then the names are shortened, then it's all made smaller.
 */
static void status_bar(const struct ui_state *s)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImFont *font = ImGui::GetFont();
	const float left = vp->Pos.x + 12, right = vp->Pos.x + vp->Size.x - 12, bottom = vp->Pos.y + vp->Size.y;
	const float top = bottom - s->status_bar, mid = snap(top + 1 + (s->status_bar - 1) / 2), h = hair();
	gradient(dl, ImVec2(vp->Pos.x, top), ImVec2(vp->Pos.x + vp->Size.x, bottom), PLATINUM_LIGHT, PLATINUM, 0);
	dl->AddRectFilled(ImVec2(vp->Pos.x, top), ImVec2(vp->Pos.x + vp->Size.x, top + h), PLATINUM_DARKER);
	dl->AddRectFilled(ImVec2(vp->Pos.x, top + h), ImVec2(vp->Pos.x + vp->Size.x, top + 2 * h), WHITE);

	float k = 1;
	bool brief = bar_contents(s, nullptr, font, left, mid, 1, false) > right - left;
	if (brief) {
		/* (twice: text doesn't quite shrink in proportion) */
		for (int pass = 0; pass < 2; pass++) {
			float w = bar_contents(s, nullptr, font, left, mid, k, true);
			if (w > right - left)
				k *= (right - left) / w;
		}
		k = k < BAR_TEXT_MIN / BAR_TEXT ? BAR_TEXT_MIN / BAR_TEXT : k;
	}
	float end = left + bar_contents(s, dl, font, left, mid, k, brief);
	const char *hint = "F12: menu";
	float w = font->CalcTextSizeA(BAR_TEXT, FLT_MAX, 0, hint).x;
	if (right - w >= end + 20)
		dl->AddText(font, BAR_TEXT, snap(ImVec2(right - w, mid - BAR_TEXT / 2)), bar_quiet, hint);
}

enum ui_action ui_draw(struct ui *u, const struct ui_state *s, int w, int h, float pixels, double seconds)
{
	ImGui::SetCurrentContext(u->imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)w, (float)h);
	io.DisplayFramebufferScale = ImVec2(pixels, pixels);
	io.DeltaTime = seconds > 0 && seconds < 1 ? (float)seconds : 1.0f / 60;
	ImGui::NewFrame();
	/* (Return is for the box in front: the menu, over a prompt) */
	const struct keys none = { false, false }, enter = { u->enter, u->enter_held };
	u->enter = false;
	if (s->status_bar > 0)
		status_bar(s);
	enum ui_action action = s->prompt ? prompt(u, s, s->menu ? &none : &enter) : UI_NONE;
	if (s->menu)
		action = menu(s, &enter);
	ImGui::Render();
	render(u, ImGui::GetDrawData());
	return action;
}
