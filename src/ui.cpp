#include "ui.h"

#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"

/* (SDL2's values) */
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u    /* RGBA in memory, as Dear ImGui's textures */
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_BLENDMODE_BLEND 1
#define SDL_SCALEMODE_LINEAR 1

struct ui {
	struct ui_renderer r;
	ImGuiContext *imgui;
	bool broken;                    /* a texture couldn't be made: nothing is drawn */
};

/* (asked for by ImDrawList callbacks; our renderer has no state to reset) */
static void reset_render_state(const ImDrawList *, const ImDrawCmd *)
{
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
	io.BackendPlatformName = "syshm";
	io.BackendRendererName = "syshm (SDL2 renderer)";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.Fonts->AddFontDefaultVector();
	ImGui::StyleColorsDark();
	ImGuiStyle &style = ImGui::GetStyle();
	style.FontSizeBase = 16;
	style.WindowRounding = 8;
	style.FrameRounding = 4;
	style.WindowPadding = ImVec2(16, 14);
	style.ItemSpacing = ImVec2(10, 10);
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

void ui_key(struct ui *u, enum ui_key key, bool down)
{
	static const ImGuiKey keys[] = { ImGuiKey_Tab, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow,
					 ImGuiKey_DownArrow, ImGuiKey_Space, ImGuiKey_Enter };
	ImGui::SetCurrentContext(u->imgui);
	if ((unsigned)key < sizeof keys / sizeof keys[0])
		ImGui::GetIO().AddKeyEvent(keys[key], down);
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

/*
 * The drives with removable media: what's in each, with an Eject button
 * (disabled while Mac OS uses the disc: it's the one to eject it then).
 */
static enum ui_action drives_menu(const struct ui_drives *dr)
{
	enum ui_action action = UI_NONE;
	bool in_use = false;
	ImGui::Separator();
	for (unsigned k = 0; k < dr->count && k < UI_DRIVES; k++) {
		const struct ui_drive *d = &dr->drive[k];
		ImGui::PushID((int)k);
		ImGui::BeginDisabled(!d->in[0] || d->in_use);
		if (ImGui::Button("Eject", ImVec2(70, 0)))
			action = (enum ui_action)(UI_EJECT + k);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text("%s: %s%s", d->name, d->in[0] ? d->in : "empty", d->locked ? " (locked)" : "");
		ImGui::PopID();
		in_use |= d->in_use;
	}
	if (in_use)
		ImGui::TextDisabled("(Mac OS is using a disc: drag it to the Trash there to eject it)");
	if (dr->next[0]) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520);
		ImGui::TextDisabled("Next floppies, as Mac OS ejects: %s", dr->next);
		ImGui::PopTextWrapPos();
	}
	ImGui::TextDisabled("(drop an image on the window to put it in)");
	return action;
}

/* The menu: the guest is paused while it's open */
static enum ui_action menu(const struct ui_state *s)
{
	enum ui_action action = UI_NONE;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
						      IM_COL32(0, 0, 0, 110));
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::Begin("syshm: paused", nullptr,
		     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
			     ImGuiWindowFlags_NoSavedSettings);
	const ImVec2 wide(260, 0);
	if (ImGui::Button("Resume", wide))
		action = UI_RESUME;
	ImGui::SetItemDefaultFocus();
	if (ImGui::Button("Restart", wide))
		action = UI_RESTART;
	if (ImGui::Button("Shut down...", wide))
		action = UI_SHUT_DOWN;
	if (ImGui::Button("Turn off", wide))
		action = UI_TURN_OFF;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("At once, as if the power went: unsaved work is lost");
	if (s->drives) {
		enum ui_action chosen = drives_menu(s->drives);
		if (chosen != UI_NONE)
			action = chosen;
	}

	ImGui::Separator();
	if (ImGui::Button("Screenshot", wide))
		action = UI_SCREENSHOT;
	bool fullscreen = s->fullscreen;
	if (ImGui::Checkbox("Fullscreen", &fullscreen))
		action = UI_FULLSCREEN;
	bool bar = s->status_bar > 0;
	if (ImGui::Checkbox("Status bar", &bar))
		action = UI_STATUS_BAR;
	bool capture = s->mouse == UI_MOUSE_CAPTURED;
	ImGui::BeginDisabled(s->mouse == UI_MOUSE_FOLLOWS);
	if (ImGui::Checkbox("Capture the mouse", &capture))
		action = capture ? UI_CAPTURE_MOUSE : UI_RELEASE_MOUSE;
	ImGui::EndDisabled();
	ImGui::TextDisabled(s->mouse == UI_MOUSE_FOLLOWS ? "(the Mac shows its cursor: the mouse follows the pointer)" :
			    "(while the Mac hides its cursor; Control-Option releases it)");
	if (s->news) {
		ImGui::Separator();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520);  /* (the drives' news can be long) */
		ImGui::TextUnformatted(s->news);
		ImGui::PopTextWrapPos();
	}
	ImGui::Separator();
	ImGui::TextDisabled("F12 or Esc resumes");
	ImGui::End();
	return action;
}

#define PROMPT_WIDTH 440.0f             /* (its text's) */
#define PROMPT_BUTTON 110.0f

/*
 * A question (or news) in a box over the screen, the guest running on
 * underneath; the default button last, on the right, as Mac OS has it.
 * While the menu is open, it waits.
 */
static enum ui_action prompt(const struct ui_state *s)
{
	enum ui_action action = UI_NONE;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::Begin("syshm##prompt", nullptr,
		     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
			     ImGuiWindowFlags_NoSavedSettings);
	ImGui::BeginDisabled(s->menu);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + PROMPT_WIDTH);
	ImGui::TextUnformatted(s->prompt);
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(PROMPT_WIDTH, 4));
	/* (as wide as the wider label needs: "Put Back and Restart") */
	const char *const labels[] = { s->prompt_no, s->prompt_yes };
	float wide = PROMPT_BUTTON, pad = 2 * ImGui::GetStyle().FramePadding.x;
	for (const char *label : labels)
		if (label && ImGui::CalcTextSize(label).x + pad > wide)
			wide = ImGui::CalcTextSize(label).x + pad;
	float buttons = s->prompt_no ? 2 * wide + ImGui::GetStyle().ItemSpacing.x : wide;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PROMPT_WIDTH - buttons);
	if (s->prompt_no) {
		if (ImGui::Button(s->prompt_no, ImVec2(wide, 0)))
			action = UI_PROMPT_NO;
		ImGui::SameLine();
	}
	if (ImGui::Button(s->prompt_yes, ImVec2(wide, 0)))
		action = UI_PROMPT_YES;
	if (!s->menu)
		ImGui::SetItemDefaultFocus();
	ImGui::EndDisabled();
	ImGui::End();
	return action;
}

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

#define BAR_TEXT 14.0f                  /* the status bar's text size, at its largest */
#define BAR_TEXT_MIN 10.0f              /* ... and its smallest, in narrow windows */

static const ImU32 bar_label = IM_COL32(205, 205, 210, 255), bar_dim = IM_COL32(110, 110, 116, 255);
static const ImVec4 bar_off(0.22f, 0.22f, 0.24f, 1), bar_green(0.25f, 0.92f, 0.35f, 1), bar_amber(1.0f, 0.66f, 0.10f, 1);
static const ImVec4 bar_blue(0.32f, 0.56f, 1.0f, 1);
#define BAR_NAP 0.7f                    /* (napping, the processor's light goes this far towards blue: softly) */

static ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t)
{
	return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

/* Text in a slot as wide as the widest of `widest` (so that what follows stays put); returns its width */
static float bar_text(ImDrawList *dl, ImFont *font, float size, float x, float mid, ImU32 col, const char *text,
		      const char *const *widest)
{
	if (dl)
		dl->AddText(font, size, ImVec2(x, mid - size / 2), col, text);
	float w = font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
	for (; widest && *widest; widest++) {
		float v = font->CalcTextSizeA(size, FLT_MAX, 0, *widest).x;
		w = v > w ? v : w;
	}
	return w;
}

/* A rate after an arrow (down: read or received, green while it's going; up: written or sent, amber); returns its width */
static float bar_rate(ImDrawList *dl, ImFont *font, float k, float x, float mid, uint64_t bytes, bool out, bool bits)
{
	if (dl) {
		ImU32 col = ImGui::ColorConvertFloat4ToU32(!bytes ? ImVec4(0.43f, 0.43f, 0.45f, 1) :
							     out ? bar_amber : bar_green);
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
	return w + bar_text(dl, font, BAR_TEXT * k, x + w, mid, bytes ? bar_label : bar_dim, text, nullptr);
}

/* ... and the widest one */
static float bar_rate_widest(ImFont *font, float k)
{
	static const char *const widest[] = { "000 KB/s", "0.0 MB/s", "000 kbps", "0.0 Mbps", nullptr };
	return 7 * k + 4 * k + bar_text(nullptr, font, BAR_TEXT * k, 0, 0, 0, "", widest);
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
	const float x0 = x, size = BAR_TEXT * k;
	for (int n = 0; n < UI_LEDS; n++) {
		const struct ui_light *l = &s->bar.light[n];
		if (l->hidden)
			continue;
		if (n)
			x += 20 * k;
		if (dl) {
			/* (off, then softly blue as much as it naps, then lit as much as it's busy) */
			const float t = l->level / 255.0f, nap = l->nap / 255.0f * BAR_NAP;
			const ImVec4 on = l->write ? bar_amber : bar_green;
			const ImVec4 c = mix(mix(bar_off, bar_blue, nap), on, t);
			const ImVec2 centre(x + 5 * k, mid);
			if (nap > 0)
				dl->AddCircleFilled(centre, 8 * k, ImGui::ColorConvertFloat4ToU32(ImVec4(bar_blue.x, bar_blue.y,
													   bar_blue.z, 0.16f * nap)));
			if (t > 0)
				dl->AddCircleFilled(centre, 8 * k, ImGui::ColorConvertFloat4ToU32(ImVec4(on.x, on.y, on.z, 0.22f * t)));
			dl->AddCircleFilled(centre, 4.5f * k, ImGui::ColorConvertFloat4ToU32(c));
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
			x += bar_text(dl, font, size, x, mid, bar_dim, speed, widest_speed);
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
 * The status bar, along the bottom of the window: a light for each device
 * (green, or amber while written to; dimmed if there's no such device, or
 * no disk in the floppy drive, whose light is there only with one),
 * with what it reads and writes a second, and the processor's lit as
 * brightly as it's busy, with its speed alongside. In narrow windows, the
 * hint goes, then the names are shortened, then it's all made smaller.
 */
static void status_bar(const struct ui_state *s)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImFont *font = ImGui::GetFont();
	const float left = vp->Pos.x + 12, right = vp->Pos.x + vp->Size.x - 12, bottom = vp->Pos.y + vp->Size.y;
	const float top = bottom - s->status_bar, mid = top + s->status_bar / 2;
	dl->AddRectFilled(ImVec2(vp->Pos.x, top), ImVec2(vp->Pos.x + vp->Size.x, bottom), IM_COL32(28, 28, 30, 255));
	dl->AddLine(ImVec2(vp->Pos.x, top + 0.5f), ImVec2(vp->Pos.x + vp->Size.x, top + 0.5f), IM_COL32(58, 58, 62, 255));

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
		dl->AddText(font, BAR_TEXT, ImVec2(right - w, mid - BAR_TEXT / 2), bar_dim, hint);
}

enum ui_action ui_draw(struct ui *u, const struct ui_state *s, int w, int h, float density, double seconds)
{
	ImGui::SetCurrentContext(u->imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)w, (float)h);
	io.DisplayFramebufferScale = ImVec2(density, density);
	io.DeltaTime = seconds > 0 && seconds < 1 ? (float)seconds : 1.0f / 60;
	ImGui::NewFrame();
	if (s->status_bar > 0)
		status_bar(s);
	enum ui_action action = s->prompt ? prompt(s) : UI_NONE;
	if (s->menu)
		action = menu(s);
	ImGui::Render();
	render(u, ImGui::GetDrawData());
	return action;
}
