#include "pch.h"
//----------------------------------------------------------------------------------
//	TimeLinePreview - タイムラインサムネイルプレビュープラグイン for AviUtl ExEdit2
//----------------------------------------------------------------------------------
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#define PLUGIN_WINDOW_NAME  L"TimeLinePreview"
#define WM_THUMBNAIL_READY  (WM_USER + 1)

// レイアウト定数
static constexpr UINT_PTR TIMER_ID_HOVER  = 1;
static constexpr UINT     TIMER_INTERVAL  = 1000; // ms
static constexpr int      THUMB_MARGIN    = 4;
static constexpr int      LABEL_H         = 16;  // フレーム番号ラベル高さ
static constexpr int      PARAM_FONT_H    = 14;  // パラメーター行の高さ
static constexpr int      THUMB_RATIO     = 55;  // ウィンドウ高さに対するサムネイル領域の割合(%)

// グローバルハンドル
static EDIT_HANDLE*   g_edit_handle = nullptr;
static LOG_HANDLE*    g_logger      = nullptr;
static CONFIG_HANDLE* g_config      = nullptr;
static HWND           g_hwnd        = nullptr;

// ホバー状態（メインスレッドのみ）
static int  g_hover_frame   = -1;
static int  g_request_frame = -1;
static bool g_rendering     = false;
static POINT g_last_pt      = { -1, -1 };
// ホバー用プレビューウィンドウ
static HWND  g_preview      = nullptr;
static HHOOK g_mouse_hook   = nullptr;
static HHOOK g_callwnd_hook = nullptr;
static bool  g_preview_visible  = false;
static bool  g_contextmenu_hold = false;

struct RenderParam { int frame; };
static void on_rendering_done(
	void* param_ptr, int frame,
	const void* buffer, int width, int height, int pitch);

// サムネイルバッファ（メインスレッドのみ）
struct ThumbnailData {
	std::vector<uint8_t> pixels; // BGRA32
	int width  = 0;
	int height = 0;
	int frame  = -1;
};
static ThumbnailData g_thumb;

// レンダリングスレッド→メインスレッド受け渡し用（mutex保護）
struct PendingThumb {
	std::vector<uint8_t> pixels;
	int  width  = 0;
	int  height = 0;
	int  frame  = -1;
	bool ready  = false;
};
static std::mutex   g_thumb_mutex;
static PendingThumb g_pending;

// オブジェクトパラメーター情報（メインスレッドのみ）
struct ParamLine {
	std::wstring label;
	std::wstring value;
};
struct ObjectParams {
	std::wstring           effect_name;
	int                    layer       = -1;
	int                    frame_start = -1;
	int                    frame_end   = -1;
	std::vector<ParamLine> lines;
	bool                   valid       = false;
};
static ObjectParams g_params;

//----------------------------------------------------------------------------------
//  エイリアス文字列パーサー
//----------------------------------------------------------------------------------

static std::wstring utf8_to_wide(const char* src) {
	if (!src || !*src) return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
	if (len <= 0) return {};
	std::wstring dst(len - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, src, -1, dst.data(), len);
	return dst;
}

static std::string trim_str(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

static bool is_numeric(const std::string& s) {
	if (s.empty()) return false;
	size_t i = 0;
	if (s[i] == '-' || s[i] == '+') ++i;
	bool has_dot = false, has_digit = false;
	for (; i < s.size(); ++i) {
		if (s[i] == '.') {
			if (has_dot) return false;
			has_dot = true;
		} else if (s[i] >= '0' && s[i] <= '9') {
			has_digit = true;
		} else {
			return false;
		}
	}
	return has_digit;
}

// [Object.0] セクションの effect.name と数値パラメーターを抽出（最大 8 件）
static ObjectParams parse_alias(const char* alias_utf8,
	int layer, int frame_start, int frame_end)
{
	ObjectParams op;
	op.layer       = layer + 1; // UI表示は1始まり
	op.frame_start = frame_start;
	op.frame_end   = frame_end;
	if (!alias_utf8) return op;

	std::istringstream ss(alias_utf8);
	std::string line;
	bool in_target = false;
	static constexpr int MAX_PARAMS = 8;

	while (std::getline(ss, line)) {
		line = trim_str(line);
		if (line.empty()) continue;

		if (line.front() == '[') {
			in_target = (line == "[Object.0]");
			continue;
		}
		if (!in_target) continue;

		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = trim_str(line.substr(0, eq));
		std::string val = trim_str(line.substr(eq + 1));

		if (key == "effect.name") {
			op.effect_name = utf8_to_wide(val.c_str());
		} else if ((int)op.lines.size() < MAX_PARAMS && is_numeric(val)) {
			ParamLine pl;
			pl.label = utf8_to_wide(key.c_str());
			pl.value = utf8_to_wide(val.c_str());
			op.lines.push_back(std::move(pl));
		}
	}

	op.valid = !op.effect_name.empty();
	return op;
}

// プレビューの表示サイズ
static constexpr int TIP_THUMB_W  = 240;
static constexpr int TIP_THUMB_H  = 135; // 16:9基準、シーン比率に廊じてWM_DRAWITEMで再計算
static constexpr int TIP_PAD      = 6;
static constexpr int TIP_LINE_H   = 14;
static POINT         g_tip_pt     = {}; // 表示位置（スクリーン座標）

// ツールチップの必要サイズを計算
static SIZE calc_tip_size()
{
	int lines = g_params.valid ? (int)g_params.lines.size() : 0;
	int text_h = TIP_LINE_H + (lines > 0 ? TIP_PAD / 2 + lines * (TIP_LINE_H + 1) : 0);
	int thumb_h = g_thumb.frame >= 0 ? TIP_THUMB_H + TIP_PAD : 0;
	// シーンのアスペクト比でサムネイル高さを計算
	if (g_thumb.frame >= 0 && g_thumb.width > 0 && g_thumb.height > 0) {
		thumb_h = TIP_THUMB_W * g_thumb.height / g_thumb.width + TIP_PAD;
	}
	return { TIP_THUMB_W + TIP_PAD * 2,
		     TIP_PAD + thumb_h + text_h + TIP_PAD };
}

static void paint_preview_content(HDC hdc, const RECT& rc)
{
	int cw = rc.right - rc.left;
	int ch = rc.bottom - rc.top;

	HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	int py = TIP_PAD;

	if (g_thumb.frame >= 0 && !g_thumb.pixels.empty()) {
		int tw = g_thumb.width, th = g_thumb.height;
		int dw = cw - TIP_PAD * 2;
		int dh = (tw > 0) ? dw * th / tw : 0;
		if (dw > 0 && dh > 0) {
			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth       = tw;
			bmi.bmiHeader.biHeight      = -th;
			bmi.bmiHeader.biPlanes      = 1;
			bmi.bmiHeader.biBitCount    = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			SetStretchBltMode(hdc, HALFTONE);
			StretchDIBits(hdc, TIP_PAD, py, dw, dh, 0, 0, tw, th,
				g_thumb.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
			py += dh + TIP_PAD;
		}
	}

	if (g_params.valid) {
		SetBkMode(hdc, TRANSPARENT);

		wchar_t h[128];
		swprintf_s(h, L"[L%d] %s (%d-%d)",
			g_params.layer, g_params.effect_name.c_str(),
			g_params.frame_start, g_params.frame_end);
		RECT hr = { TIP_PAD, py, cw - TIP_PAD, py + TIP_LINE_H };
		SetTextColor(hdc, RGB(120, 200, 255));
		DrawText(hdc, h, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		py += TIP_LINE_H + TIP_PAD / 2;

		int label_col = cw * 55 / 100;
		for (const auto& pl : g_params.lines) {
			if (py + TIP_LINE_H > ch) break;
			RECT lr = { TIP_PAD + 2, py, label_col, py + TIP_LINE_H };
			SetTextColor(hdc, RGB(200, 200, 200));
			DrawText(hdc, pl.label.c_str(), -1, &lr,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
			RECT vr = { label_col, py, cw - TIP_PAD, py + TIP_LINE_H };
			SetTextColor(hdc, RGB(255, 220, 120));
			DrawText(hdc, pl.value.c_str(), -1, &vr,
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
			py += TIP_LINE_H + 1;
		}
	}
}

// プレビューを指定スクリーン座標に表示
static void show_tooltip(POINT pt_screen)
{
	if (!g_preview) return;
	if (!g_params.valid && g_thumb.frame < 0) {
		if (g_preview_visible) {
			ShowWindow(g_preview, SW_HIDE);
			g_preview_visible = false;
		}
		return;
	}

	g_tip_pt = pt_screen;

	SIZE sz = calc_tip_size();
	int mx = GetSystemMetrics(SM_CXSCREEN);
	int my = GetSystemMetrics(SM_CYSCREEN);
	int tx = pt_screen.x;
	if (tx + sz.cx > mx) tx = mx - sz.cx;
	if (tx < 0) tx = 0;

	int ty = pt_screen.y - sz.cy;
	if (ty < 0) ty = pt_screen.y;
	if (ty + sz.cy > my) ty = my - sz.cy;
	if (ty < 0) ty = 0;

	SetWindowPos(g_preview, HWND_TOPMOST, tx, ty, sz.cx, sz.cy,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(g_preview, nullptr, TRUE);
	if (!g_preview_visible) {
		ShowWindow(g_preview, SW_SHOWNOACTIVATE);
		g_preview_visible = true;
	}
}

static void hide_tooltip()
{
	if (g_preview && g_preview_visible) {
		ShowWindow(g_preview, SW_HIDE);
		g_preview_visible = false;
	}
}

static void toggle_contextmenu_hold()
{
	g_contextmenu_hold = !g_contextmenu_hold;
	hide_tooltip();
}

static bool is_any_mouse_button_down()
{
	return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
		(GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
		(GetAsyncKeyState(VK_MBUTTON) & 0x8000) ||
		(GetAsyncKeyState(VK_XBUTTON1) & 0x8000) ||
		(GetAsyncKeyState(VK_XBUTTON2) & 0x8000);
}

static bool is_own_process_foreground()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return false;

	DWORD fg_pid = 0;
	GetWindowThreadProcessId(fg, &fg_pid);
	return fg_pid == GetCurrentProcessId();
}

struct HoverParam {
	POINT       pt;
	int         frame     = -1;
	int         layer     = -1;
	bool        valid     = false;
	LOG_HANDLE* logger    = nullptr;
	char        alias_buf[4096] = {};
	int         obj_layer = -1;
	int         obj_start = -1;
	int         obj_end   = -1;
	bool        obj_found = false;
};

static void update_hover_at_point(POINT pt)
{
	if (g_rendering) return;
	if (g_edit_handle->get_edit_state() == EDIT_HANDLE::EDIT_STATE_SAVE) return;
	if (g_contextmenu_hold) {
		hide_tooltip();
		return;
	}
	if (is_any_mouse_button_down()) {
		hide_tooltip();
		return;
	}

	HoverParam hp{};
	hp.pt = pt;
	hp.logger = g_logger;

	g_edit_handle->call_edit_section_param(&hp, [](void* p, EDIT_SECTION* edit) {
		HoverParam* hp = static_cast<HoverParam*>(p);
		int layer, frame;
		if (!edit->pos_to_layer_frame(hp->pt.x, hp->pt.y, &layer, &frame)) return;

		hp->frame = frame;
		hp->layer = layer;
		hp->valid = true;

		if (hp->logger) {
			wchar_t buf[128];
			swprintf_s(buf, L"[TimeLinePreview] hover layer=%d frame=%d", layer + 1, frame);
			hp->logger->verbose(hp->logger, buf);
		}

		OBJECT_HANDLE obj = edit->find_object(layer, frame);
		if (!obj) return;

		OBJECT_LAYER_FRAME olf = edit->get_object_layer_frame(obj);
		if (frame < olf.start || frame > olf.end) return;

		LPCSTR alias = edit->get_object_alias(obj);
		if (!alias) return;

		strncpy_s(hp->alias_buf, alias, _TRUNCATE);
		hp->obj_layer = olf.layer;
		hp->obj_start = olf.start;
		hp->obj_end   = olf.end;
		hp->obj_found = true;
	});

	if (!hp.valid) {
		if (g_hover_frame != -1 || g_params.valid) {
			g_hover_frame = -1;
			g_params      = ObjectParams{};
			hide_tooltip();
			InvalidateRect(g_hwnd, nullptr, TRUE);
		}
		return;
	}

	g_params = hp.obj_found
		? parse_alias(hp.alias_buf, hp.obj_layer, hp.obj_start, hp.obj_end)
		: ObjectParams{};

	int hover_frame = hp.frame;
	if (hover_frame != g_request_frame) {
		auto* rp = new RenderParam{ hover_frame };
		if (g_edit_handle->rendering_scene_video(hover_frame, rp, on_rendering_done)) {
			g_rendering     = true;
			g_request_frame = hover_frame;
		} else {
			delete rp;
		}
	}

	InvalidateRect(g_hwnd, nullptr, FALSE);
	show_tooltip(pt);
}

static void on_timer()
{
	if (g_rendering) return;
	if (g_contextmenu_hold) {
		hide_tooltip();
		return;
	}
	if (is_any_mouse_button_down()) {
		hide_tooltip();
		return;
	}
	if (!is_own_process_foreground()) {
		hide_tooltip();
		return;
	}

	POINT pt{};
	GetCursorPos(&pt);
	if (pt.x == g_last_pt.x && pt.y == g_last_pt.y) return;
	g_last_pt = pt;
	update_hover_at_point(pt);
}

// マウスフックコールバック
// タイムライン上でのマウス移動を捕捉し、最新のg_paramsでプレビューを更新
static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION) {
		if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN ||
			wp == WM_XBUTTONDOWN || wp == WM_NCLBUTTONDOWN || wp == WM_NCRBUTTONDOWN ||
			wp == WM_NCMBUTTONDOWN || wp == WM_NCXBUTTONDOWN) {
			hide_tooltip();
			return CallNextHookEx(g_mouse_hook, code, wp, lp);
		}

		if (wp == WM_MOUSEMOVE) {
			const auto* ms = reinterpret_cast<MOUSEHOOKSTRUCT*>(lp);
			if (g_contextmenu_hold || is_any_mouse_button_down()) {
				hide_tooltip();
				return CallNextHookEx(g_mouse_hook, code, wp, lp);
			}
			g_last_pt = ms->pt;
			update_hover_at_point(ms->pt);
		}
	}
	return CallNextHookEx(g_mouse_hook, code, wp, lp);
}

static LRESULT CALLBACK callwnd_hook_proc(int code, WPARAM, LPARAM lp)
{
	if (code == HC_ACTION) {
		const auto* cwp = reinterpret_cast<CWPSTRUCT*>(lp);
		if (cwp && cwp->message == WM_CONTEXTMENU) {
			toggle_contextmenu_hold();
		}
	}
	return CallNextHookEx(g_callwnd_hook, code, 0, lp);
}

//----------------------------------------------------------------------------------
//  汎用プラグイン定義・初期化系
//----------------------------------------------------------------------------------

static COMMON_PLUGIN_TABLE common_plugin_table = {
	L"TimeLinePreview",
	L"TimeLinePreview version 0.2",
};

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2003300; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* h) { g_logger = h; }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* h) { g_config = h; }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_mouse_hook) {
		UnhookWindowsHookEx(g_mouse_hook);
		g_mouse_hook = nullptr;
	}
	if (g_callwnd_hook) {
		UnhookWindowsHookEx(g_callwnd_hook);
		g_callwnd_hook = nullptr;
	}
	if (g_preview) {
		DestroyWindow(g_preview);
		g_preview = nullptr;
		g_preview_visible = false;
	}
}
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
	return &common_plugin_table;
}

//----------------------------------------------------------------------------------
//  レンダリングコールバック（レンダリングスレッドから呼ばれる）
//----------------------------------------------------------------------------------

static void on_rendering_done(
	void* param_ptr, int frame,
	const void* buffer, int width, int height, int pitch)
{
	delete static_cast<RenderParam*>(param_ptr);

	if (!buffer || width <= 0 || height <= 0) {
		g_rendering = false;
		return;
	}

	std::vector<uint8_t> bgra(width * height * 4);
	const uint8_t* src = static_cast<const uint8_t*>(buffer);
	for (int y = 0; y < height; ++y) {
		const uint8_t* row = src + y * pitch;
		uint8_t*       dst = bgra.data() + y * width * 4;
		for (int x = 0; x < width; ++x) {
			dst[0] = row[2]; dst[1] = row[1];
			dst[2] = row[0]; dst[3] = row[3];
			row += 4; dst += 4;
		}
	}

	{
		std::lock_guard<std::mutex> lk(g_thumb_mutex);
		g_pending.pixels = std::move(bgra);
		g_pending.width  = width;
		g_pending.height = height;
		g_pending.frame  = frame;
		g_pending.ready  = true;
	}
	if (g_hwnd) PostMessage(g_hwnd, WM_THUMBNAIL_READY, 0, 0);
	g_rendering = false;
}

//----------------------------------------------------------------------------------
//  描画
//----------------------------------------------------------------------------------

static void paint_window(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	int cw = rc.right;
	int ch = rc.bottom;

	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);

	// 背景
	HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	// ---- サムネイル領域 ----
	int thumb_area_h = ch * THUMB_RATIO / 100;

	if (g_thumb.frame >= 0 && !g_thumb.pixels.empty()) {
		int tw = g_thumb.width, th = g_thumb.height;
		int avail_w = cw - THUMB_MARGIN * 2;
		int avail_h = thumb_area_h - THUMB_MARGIN * 2 - LABEL_H;
		if (avail_w > 0 && avail_h > 0 && tw > 0 && th > 0) {
			float scale = (std::min)(float(avail_w) / tw, float(avail_h) / th);
			int dw = int(tw * scale), dh = int(th * scale);
			int dx = THUMB_MARGIN + (avail_w - dw) / 2;
			int dy = THUMB_MARGIN;

			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth       = tw;
			bmi.bmiHeader.biHeight      = -th;
			bmi.bmiHeader.biPlanes      = 1;
			bmi.bmiHeader.biBitCount    = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			SetStretchBltMode(hdc, HALFTONE);
			StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, tw, th,
				g_thumb.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

			// フレーム番号ラベル
			RECT lr = { 0, thumb_area_h - LABEL_H, cw, thumb_area_h };
			SetBkColor(hdc, RGB(30, 30, 30));
			SetTextColor(hdc, RGB(180, 180, 180));
			wchar_t buf[64];
			swprintf_s(buf, L"Frame %d", g_thumb.frame);
			DrawText(hdc, buf, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}

	// ---- 区切り線 ----
	HPEN sep = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
	HPEN old = (HPEN)SelectObject(hdc, sep);
	MoveToEx(hdc, 0, thumb_area_h, nullptr);
	LineTo(hdc, cw, thumb_area_h);
	SelectObject(hdc, old);
	DeleteObject(sep);

	// ---- パラメーター領域 ----
	int py = thumb_area_h + 4;
	SetBkMode(hdc, TRANSPARENT);

	if (g_params.valid) {
		// エフェクト名ヘッダー（レイヤー・フレーム範囲付き）
		{
			wchar_t hdr[256];
			swprintf_s(hdr, L"[L%d] %s  (%d - %d)",
				g_params.layer, g_params.effect_name.c_str(),
				g_params.frame_start, g_params.frame_end);
			RECT hr = { 4, py, cw - 4, py + PARAM_FONT_H + 2 };
			SetTextColor(hdc, RGB(120, 200, 255));
			DrawText(hdc, hdr, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
			py += PARAM_FONT_H + 4;

			HPEN hl = CreatePen(PS_SOLID, 1, RGB(60, 100, 140));
			old = (HPEN)SelectObject(hdc, hl);
			MoveToEx(hdc, 4, py - 2, nullptr);
			LineTo(hdc, cw - 4, py - 2);
			SelectObject(hdc, old);
			DeleteObject(hl);
		}

		// パラメーター行（ラベル左・値右）
		int label_col = cw * 55 / 100;
		for (const auto& pl : g_params.lines) {
			if (py + PARAM_FONT_H > ch) break;

			RECT lbr = { 6, py, label_col, py + PARAM_FONT_H };
			SetTextColor(hdc, RGB(200, 200, 200));
			DrawText(hdc, pl.label.c_str(), -1, &lbr,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			RECT vlr = { label_col, py, cw - 6, py + PARAM_FONT_H };
			SetTextColor(hdc, RGB(255, 220, 120));
			DrawText(hdc, pl.value.c_str(), -1, &vlr,
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

			py += PARAM_FONT_H + 1;
		}
	} else if (g_hover_frame == -1) {
		RECT mr = { 0, py, cw, ch };
		SetTextColor(hdc, RGB(90, 90, 90));
		DrawText(hdc, L"タイムライン上にカーソルを移動してください",
			-1, &mr, DT_CENTER | DT_TOP | DT_WORDBREAK);
	}

	EndPaint(hwnd, &ps);
}

//----------------------------------------------------------------------------------
//  ウィンドウプロシージャ
//----------------------------------------------------------------------------------

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		SetTimer(hwnd, TIMER_ID_HOVER, TIMER_INTERVAL, nullptr);
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, TIMER_ID_HOVER);
		return 0;

	case WM_TIMER:
		if (wp == TIMER_ID_HOVER) on_timer();
		return 0;

	case WM_ACTIVATEAPP:
		if (!wp) hide_tooltip();
		return 0;

	case WM_THUMBNAIL_READY:
		{
			std::lock_guard<std::mutex> lk(g_thumb_mutex);
			if (g_pending.ready) {
				g_thumb.pixels = std::move(g_pending.pixels);
				g_thumb.width  = g_pending.width;
				g_thumb.height = g_pending.height;
				g_thumb.frame  = g_pending.frame;
				g_pending.ready = false;
				g_hover_frame   = g_thumb.frame;
			}
		}
		if (g_preview_visible && g_preview) {
			InvalidateRect(g_preview, nullptr, TRUE);
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_PAINT:
		paint_window(hwnd);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_NOTIFY:
		break;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK preview_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		paint_preview_content(hdc, rc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

//----------------------------------------------------------------------------------
//  プラグイン登録
//----------------------------------------------------------------------------------

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host)
{
	WNDCLASSEXW wcex = {};
	wcex.cbSize        = sizeof(WNDCLASSEX);
	wcex.lpszClassName = PLUGIN_WINDOW_NAME;
	wcex.lpfnWndProc   = wnd_proc;
	wcex.hInstance     = GetModuleHandle(nullptr);
	wcex.hbrBackground = nullptr;
	wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	if (!RegisterClassEx(&wcex)) return;

	WNDCLASSEXW pwcex = {};
	pwcex.cbSize        = sizeof(WNDCLASSEXW);
	pwcex.lpszClassName = L"TimeLinePreviewPopup";
	pwcex.lpfnWndProc   = preview_wnd_proc;
	pwcex.hInstance     = GetModuleHandle(nullptr);
	pwcex.hbrBackground = nullptr;
	pwcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	if (!RegisterClassExW(&pwcex)) return;

	g_hwnd = CreateWindowEx(0, PLUGIN_WINDOW_NAME, PLUGIN_WINDOW_NAME,
		WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 320, 400,
		nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	if (!g_hwnd) return;

	g_preview = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		L"TimeLinePreviewPopup", nullptr,
		WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	if (g_preview) {
		ShowWindow(g_preview, SW_HIDE);
	}

	// プロセス内マウスフックを設置
	g_mouse_hook = SetWindowsHookEx(
		WH_MOUSE, mouse_hook_proc,
		nullptr, GetCurrentThreadId());
	g_callwnd_hook = SetWindowsHookEx(
		WH_CALLWNDPROC, callwnd_hook_proc,
		nullptr, GetCurrentThreadId());

	host->register_window_client(PLUGIN_WINDOW_NAME, g_hwnd);
	g_edit_handle = host->create_edit_handle();
}
