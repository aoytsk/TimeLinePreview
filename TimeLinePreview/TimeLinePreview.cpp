//----------------------------------------------------------------------------------
//  TimeLinePreview.cpp
//----------------------------------------------------------------------------------
#include "pch.h"
#include "TimeLinePreview.h"
#include "resource.h"
#include <new>
#include <sstream>
#include <algorithm>

using namespace timeline_preview;

// ============================================================
//  内部ユーティリティ
// ============================================================
namespace {

    std::wstring utf8_to_wide(const char* src)
    {
        if (!src || !*src) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring dst(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, src, -1, dst.data(), len);
        return dst;
    }

    std::string trim_str(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
    }

    bool is_numeric(const std::string& s)
    {
        if (s.empty()) return false;
        size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        bool dot = false, digit = false;
        for (; i < s.size(); ++i) {
            if      (s[i] == '.')                 { if (dot) return false; dot = true; }
            else if (s[i] >= '0' && s[i] <= '9') { digit = true; }
            else                                  { return false; }
        }
        return digit;
    }

    // エイリアスデータを全セクション解析する。
    // [Object.0] = メインエフェクト、[Object.1] 以降 = フィルタ効果。
    ObjectParams parse_alias(const char* src, int layer, int fs, int fe)
    {
        ObjectParams op;
        op.layer = layer + 1; op.frame_start = fs; op.frame_end = fe;
        if (!src) return op;

        std::istringstream ss(src);
        std::string line;
        int section = -1; // -1=未到達, 0=Object.0, 1以上=フィルタ

        while (std::getline(ss, line)) {
            line = trim_str(line);
            if (line.empty()) continue;

            if (line.front() == '[') {
                section = -1;
                // [Object.N] を解析
                if (line.size() > 9 && line.substr(0, 8) == "[Object.") {
                    const auto num_str = line.substr(8, line.size() - 9); // ']' を除く
                    bool all_digit = !num_str.empty();
                    for (char c : num_str) if (!isdigit((unsigned char)c)) { all_digit = false; break; }
                    if (all_digit) {
                        section = std::stoi(num_str);
                        if (section > 0) op.filters.push_back({});
                    }
                }
                continue;
            }

            if (section < 0) continue;

            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto key = trim_str(line.substr(0, eq));
            const auto val = trim_str(line.substr(eq + 1));

            if (section == 0) {
                if (key == "effect.name")
                    op.effect_name = utf8_to_wide(val.c_str());
                else if ((int)op.lines.size() < MAX_PARAM_LINES && is_numeric(val))
                    op.lines.push_back({ utf8_to_wide(key.c_str()), utf8_to_wide(val.c_str()) });
            } else {
                auto& f = op.filters.back();
                if (key == "effect.name")
                    f.name = utf8_to_wide(val.c_str());
                else if ((int)f.lines.size() < MAX_PARAM_LINES && is_numeric(val))
                    f.lines.push_back({ utf8_to_wide(key.c_str()), utf8_to_wide(val.c_str()) });
            }
        }

        // effect.name のないフィルタエントリは除去
        op.filters.erase(
            std::remove_if(op.filters.begin(), op.filters.end(),
                           [](const FilterEffect& f) { return f.name.empty(); }),
            op.filters.end());

        op.valid = !op.effect_name.empty();
        return op;
    }

    bool any_button_down()
    {
        return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_XBUTTON1) |
                GetAsyncKeyState(VK_XBUTTON2)) & 0x8000;
    }

    int read_edit_int(HWND hw, int fallback)
    {
        wchar_t buf[32]{};
        GetWindowTextW(hw, buf, 31);
        const int v = _wtoi(buf);
        return v > 0 ? v : fallback;
    }

} // namespace

// ============================================================
//  Settings
// ============================================================
std::wstring Settings::get_ini_path(CONFIG_HANDLE* config) const
{
    if (!config || !config->app_data_path) return {};
    std::wstring dir(config->app_data_path);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
        dir += L'\\';
    dir += INI_SUBDIR;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L'\\' + INI_FILENAME;
}

void Settings::load(CONFIG_HANDLE* config)
{
    const auto ini = get_ini_path(config);
    if (ini.empty()) return;
    LPCWSTR s = ini.c_str();

    auto get_int  = [&](LPCWSTR key, int def) {
        return (int)GetPrivateProfileIntW(INI_SECTION, key, def, s);
    };
    auto get_bool = [&](LPCWSTR key, bool def) {
        return GetPrivateProfileIntW(INI_SECTION, key, def ? 1 : 0, s) != 0;
    };

    thumb_w            = get_int (ini_key::THUMB_W,            thumb_w);
    timer_ms           = get_int (ini_key::TIMER_MS,           timer_ms);
    show_thumb         = get_bool(ini_key::SHOW_THUMB,         show_thumb);
    show_header        = get_bool(ini_key::SHOW_HEADER,        show_header);
    show_params        = get_bool(ini_key::SHOW_PARAMS,        show_params);
    show_filters       = get_bool(ini_key::SHOW_FILTERS,       show_filters);
    show_filter_params = get_bool(ini_key::SHOW_FILTER_PARAMS, show_filter_params);
}

void Settings::save(CONFIG_HANDLE* config) const
{
    const auto ini = get_ini_path(config);
    if (ini.empty()) return;
    LPCWSTR s = ini.c_str();

    auto put_int  = [&](LPCWSTR key, int v) {
        WritePrivateProfileStringW(INI_SECTION, key, std::to_wstring(v).c_str(), s);
    };
    auto put_bool = [&](LPCWSTR key, bool v) {
        WritePrivateProfileStringW(INI_SECTION, key, v ? L"1" : L"0", s);
    };

    put_int (ini_key::THUMB_W,            thumb_w);
    put_int (ini_key::TIMER_MS,           timer_ms);
    put_bool(ini_key::SHOW_THUMB,         show_thumb);
    put_bool(ini_key::SHOW_HEADER,        show_header);
    put_bool(ini_key::SHOW_PARAMS,        show_params);
    put_bool(ini_key::SHOW_FILTERS,       show_filters);
    put_bool(ini_key::SHOW_FILTER_PARAMS, show_filter_params);
}

// ============================================================
//  PreviewPopup
// ============================================================
bool PreviewPopup::create(HINSTANCE hinst,
    std::function<void()> on_timer,
    std::function<void()> on_thumbnail_ready)
{
    on_timer_           = std::move(on_timer);
    on_thumbnail_ready_ = std::move(on_thumbnail_ready);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = POPUP_WINDOW_CLASS;
    wc.lpfnWndProc   = s_wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // WM_ERASEBKGND を自前で処理するため null
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        POPUP_WINDOW_CLASS, nullptr, WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, SW_HIDE);
    return true;
}

void PreviewPopup::destroy()
{
    if (!hwnd_) return;
    DestroyWindow(hwnd_);
    hwnd_      = nullptr;
    visible_   = false;
    dirty_     = false;
    last_rect_ = {-1, -1, -1, -1};
}

void PreviewPopup::set_thumb(ThumbnailData&& t) { thumb_ = std::move(t); dirty_ = true; }
void PreviewPopup::reset_thumb()                { thumb_ = {};           dirty_ = true; }

void PreviewPopup::set_params(ObjectParams p)
{
    params_ = std::move(p);
    dirty_  = true;
    if (!params_.valid && thumb_.frame < 0) hide();
}

// レイアウト計算（show と on_paint で共有）
PopupLayout PreviewPopup::calc_layout(const Settings& s) const
{
    PopupLayout L;
    L.total_w = s.thumb_w + theme_.pad * 2;

    int y = theme_.pad;

    // サムネイル
    L.has_thumb = s.show_thumb && thumb_.frame >= 0 && thumb_.width > 0;
    if (L.has_thumb) {
        L.thumb_h = s.thumb_w * thumb_.height / thumb_.width;
        y += L.thumb_h + theme_.pad;
    }

    if (params_.valid) {
        // ヘッダー
        L.has_header = s.show_header;
        if (L.has_header) {
            L.header_y = y;
            y += theme_.line_h + theme_.pad;
        }

        // メインエフェクトのパラメータ
        L.has_params = s.show_params && !params_.lines.empty();
        if (L.has_params) {
            L.params_y = y;
            y += (int)params_.lines.size() * (theme_.line_h + 1);
        }

        // フィルタ効果
        L.has_filters = s.show_filters && !params_.filters.empty();
        if (L.has_filters) {
            L.has_separator = true;
            y += theme_.pad / 2;
            L.separator_y = y;
            y += 1 + theme_.pad / 2;

            L.filters_y = y;
            for (const auto& f : params_.filters) {
                y += theme_.line_h + theme_.pad;
                if (s.show_filter_params && !f.lines.empty())
                    y += (int)f.lines.size() * (theme_.line_h + theme_.pad);
            }
        }
    }

    y += theme_.pad;
    L.total_h = y;
    return L;
}

void PreviewPopup::show(POINT pt, const Settings& s)
{
    if (!hwnd_) return;

    const PopupLayout L = calc_layout(s);
    if (!L.has_thumb && !L.has_header && !L.has_params && !L.has_filters) {
        hide();
        return;
    }

    settings_ = s;

    const int mx = GetSystemMetrics(SM_CXSCREEN);
    const int my = GetSystemMetrics(SM_CYSCREEN);
    int tx = pt.x;
    int ty = pt.y - L.total_h;
    if (tx + L.total_w > mx) tx = mx - L.total_w;
    if (tx < 0)              tx = 0;
    if (ty < 0)              ty = pt.y;
    if (ty + L.total_h > my) ty = my - L.total_h;
    if (ty < 0)              ty = 0;

    const RECT next = { tx, ty, tx + L.total_w, ty + L.total_h };

    constexpr int MOVE_THRESHOLD = 4;
    const bool moved =
        std::abs(next.left - last_rect_.left) > MOVE_THRESHOLD ||
        std::abs(next.top  - last_rect_.top)  > MOVE_THRESHOLD ||
        (next.right  - next.left) != (last_rect_.right  - last_rect_.left) ||
        (next.bottom - next.top)  != (last_rect_.bottom - last_rect_.top);

    if (!visible_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, tx, ty, L.total_w, L.total_h, SWP_NOACTIVATE);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        last_rect_ = next;
        visible_   = true;
        dirty_     = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        if (moved) {
            SetWindowPos(hwnd_, HWND_TOPMOST, tx, ty, L.total_w, L.total_h, SWP_NOACTIVATE);
            last_rect_ = next;
        }
        if (moved || dirty_) {
            dirty_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

void PreviewPopup::hide()
{
    if (hwnd_ && visible_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_   = false;
        last_rect_ = {-1, -1, -1, -1};
    }
}

LRESULT CALLBACK PreviewPopup::s_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PreviewPopup* self = nullptr;
    if (msg == WM_CREATE) {
        self = static_cast<PreviewPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<PreviewPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_TIMER:
        if (self && self->on_timer_) self->on_timer_();
        return 0;
    case WM_THUMBNAIL_READY:
        if (self && self->on_thumbnail_ready_) self->on_thumbnail_ready_();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (self) self->on_paint(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
}

void PreviewPopup::on_paint(HDC hdc, const RECT& rc)
{
    const int cw = rc.right  - rc.left;
    const int ch = rc.bottom - rc.top;

    // ダブルバッファリング（スコープ終了時に自動解放）
    wil::unique_hdc     mem_dc{ CreateCompatibleDC(hdc) };
    wil::unique_hbitmap bmp   { CreateCompatibleBitmap(hdc, cw, ch) };
    HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc.get(), bmp.get()));
    auto restore_bmp = wil::scope_exit([&] { SelectObject(mem_dc.get(), old_bmp); });

    // 背景
    const RECT full = {0, 0, cw, ch};
    wil::unique_hbrush bg_brush{ CreateSolidBrush(theme_.bg_color) };
    FillRect(mem_dc.get(), &full, bg_brush.get());

    const PopupLayout L = calc_layout(settings_);

    // ── サムネイル ───────────────────────────────────────────
    if (L.has_thumb && !thumb_.pixels.empty()) {
        const int tw = thumb_.width, th = thumb_.height;
        const int dw = cw - theme_.pad * 2;
        const int dh = L.thumb_h;
        if (dw > 0 && dh > 0) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = tw;
            bmi.bmiHeader.biHeight      = -th;
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(mem_dc.get(), HALFTONE);
            StretchDIBits(mem_dc.get(), theme_.pad, theme_.pad, dw, dh,
                          0, 0, tw, th,
                          thumb_.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
    }

    if (!params_.valid) {
        BitBlt(hdc, 0, 0, cw, ch, mem_dc.get(), 0, 0, SRCCOPY);
        return;
    }

    SetBkMode(mem_dc.get(), TRANSPARENT);

    // ── ヘッダー ─────────────────────────────────────────────
    if (L.has_header) {
        wchar_t hdr[128];
        swprintf_s(hdr, L"[L%d] %s (%d-%d)",
                   params_.layer, params_.effect_name.c_str(),
                   params_.frame_start, params_.frame_end);
        RECT hr = { theme_.pad, L.header_y, cw - theme_.pad, L.header_y + theme_.line_h };
        SetTextColor(mem_dc.get(), theme_.header_color);
        DrawText(mem_dc.get(), hdr, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // ── メインエフェクトのパラメータ ─────────────────────────
    if (L.has_params) {
        const int lc = cw * 55 / 100;
        int py = L.params_y;
        for (const auto& pl : params_.lines) {
            RECT lr = { theme_.pad + 2, py, lc,              py + theme_.line_h };
            RECT vr = { lc,             py, cw - theme_.pad, py + theme_.line_h };
            SetTextColor(mem_dc.get(), theme_.param_label_color);
            DrawText(mem_dc.get(), pl.label.c_str(), -1, &lr, DT_LEFT  | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(mem_dc.get(), theme_.param_value_color);
            DrawText(mem_dc.get(), pl.value.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            py += theme_.line_h + 1;
        }
    }

    // ── フィルタ効果 ─────────────────────────────────────────
    if (L.has_filters) {
        // 区切り線（CPen でスコープ終了時に自動解放）
        if (L.has_separator) {
            wil::unique_hpen sep_pen{ CreatePen(PS_SOLID, 1, theme_.separator_color) };
            HPEN old_pen = static_cast<HPEN>(SelectObject(mem_dc.get(), sep_pen.get()));
            MoveToEx(mem_dc.get(), theme_.pad, L.separator_y, nullptr);
            LineTo(mem_dc.get(), cw - theme_.pad, L.separator_y);
            SelectObject(mem_dc.get(), old_pen);
        }

        int py = L.filters_y;
        for (const auto& f : params_.filters) {
            // フィルタ名（先頭に "▸ " を付けて視覚的に区別）
            const std::wstring fname = L"\u25b8 " + f.name;
            RECT fr = { theme_.pad + 2, py, cw - theme_.pad, py + theme_.line_h };
            SetTextColor(mem_dc.get(), theme_.filter_name_color);
            DrawText(mem_dc.get(), fname.c_str(), -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            py += theme_.line_h + theme_.pad;

            if (settings_.show_filter_params && !f.lines.empty()) {
                const int lc = cw * 55 / 100;
                for (const auto& pl : f.lines) {
                    RECT lr = { theme_.pad + 12, py, lc,              py + theme_.line_h };
                    RECT vr = { lc,              py, cw - theme_.pad, py + theme_.line_h };
                    SetTextColor(mem_dc.get(), theme_.filter_label_color);
                    DrawText(mem_dc.get(), pl.label.c_str(), -1, &lr, DT_LEFT  | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SetTextColor(mem_dc.get(), theme_.filter_value_color);
                    DrawText(mem_dc.get(), pl.value.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    py += theme_.line_h + theme_.pad;
                }
            }
        }
    }

    BitBlt(hdc, 0, 0, cw, ch, mem_dc.get(), 0, 0, SRCCOPY);
    // wil::unique_hdc/hbitmap/hbrush のデストラクタが DeleteDC/DeleteObject を処理
    // restore_bmp の scope_exit が SelectObject を復元
}

// ============================================================
//  TimelineInspector
// ============================================================
void TimelineInspector::init(EDIT_HANDLE* edit, LOG_HANDLE* log,
    PreviewPopup* popup, const Settings* settings,
    HWND notify_hwnd, HWND host_app_window)
{
    edit_            = edit;
    log_             = log;
    popup_           = popup;
    settings_        = settings;
    notify_hwnd_     = notify_hwnd;
    host_app_window_ = host_app_window;
}

bool TimelineInspector::is_host_foreground() const
{
    return host_app_window_ && GetForegroundWindow() == host_app_window_;
}

void TimelineInspector::on_mouse_move(POINT pt)
{
    if (contextmenu_hold_ || any_button_down()) { popup_->hide(); return; }
    last_pt_ = pt;
    update_at(pt);
}

void TimelineInspector::on_button_down()
{
    popup_->hide();
}

void TimelineInspector::on_context_menu_open()
{
    contextmenu_hold_ = true;
    popup_->hide();
}

void TimelineInspector::on_context_menu_close()
{
    contextmenu_hold_ = false;
}

void TimelineInspector::on_timer()
{
    if (rendering_) return;
    if (contextmenu_hold_ || any_button_down()) { popup_->hide(); return; }
    if (!is_host_foreground())                  { popup_->hide(); return; }

    POINT pt{};
    GetCursorPos(&pt);
    if (pt.x == last_pt_.x && pt.y == last_pt_.y) return;
    last_pt_ = pt;
    update_at(pt);
}

void TimelineInspector::on_thumbnail_ready()
{
    PendingThumb tmp;
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        if (!pending_.ready) return;
        tmp = std::move(pending_);
        pending_.ready = false;
    }
    ThumbnailData td;
    td.pixels = std::move(tmp.pixels);
    td.width  = tmp.width;
    td.height = tmp.height;
    td.frame  = tmp.frame;
    popup_->set_thumb(std::move(td));
    popup_->show(last_pt_, *settings_);
}

void TimelineInspector::update_at(POINT pt)
{
    if (rendering_ || !edit_) return;
    if (!is_host_foreground() || !IsWindowEnabled(host_app_window_)) { popup_->hide(); return; }
    { const int st = edit_->get_edit_state();
      if (st == EDIT_HANDLE::EDIT_STATE_PLAY || st == EDIT_HANDLE::EDIT_STATE_SAVE)
          { popup_->hide(); return; }
    }
    if (!is_host_foreground()) { popup_->hide(); return; }

    struct HoverParam {
        POINT       pt        = {};
        int         frame     = -1;
        int         layer     = -1;
        bool        valid     = false;
        std::string alias;
        int         obj_layer = -1;
        int         obj_start = -1;
        int         obj_end   = -1;
        bool        found     = false;
    } hp;
    hp.pt = pt;

    edit_->call_edit_section_param(&hp, [](void* p, EDIT_SECTION* e) {
        auto* h = static_cast<HoverParam*>(p);
        int la, fr;
        if (!e->pos_to_layer_frame(h->pt.x, h->pt.y, &la, &fr)) return;
        h->frame = fr; h->layer = la; h->valid = true;
        auto obj = e->find_object(la, fr);
        if (!obj) return;
        auto olf = e->get_object_layer_frame(obj);
        if (fr < olf.start || fr > olf.end) return;
        auto al = e->get_object_alias(obj);
        if (!al) return;
        h->alias     = al;
        h->obj_layer = olf.layer;
        h->obj_start = olf.start;
        h->obj_end   = olf.end;
        h->found     = true;
    });

    if (!hp.valid) {
        popup_->reset_thumb();
        popup_->set_params({});
        return;
    }

    popup_->set_params(hp.found
        ? parse_alias(hp.alias.c_str(), hp.obj_layer, hp.obj_start, hp.obj_end)
        : ObjectParams{});

    if (log_) {
        wchar_t buf[128];
        swprintf_s(buf, L"[TLP] hover L%d F%d", hp.layer + 1, hp.frame);
        log_->verbose(log_, buf);
    }

    if (hp.frame != request_frame_) request_render(hp.frame);
    popup_->show(pt, *settings_);
}

void TimelineInspector::request_render(int frame)
{
    auto rp = std::unique_ptr<RenderParam>(new (std::nothrow) RenderParam{ frame, this });
    if (!rp) return;
    if (edit_->rendering_scene_video(frame, rp.get(), s_render_done)) {
        rendering_     = true;
        request_frame_ = frame;
        rp.release(); // 所有権をコールバックへ移譲
    }
    // 失敗時は unique_ptr のデストラクタが delete
}

void CALLBACK TimelineInspector::s_render_done(
    void* param, int /*frame*/, const void* buf, int w, int h, int pitch)
{
    auto* rp   = static_cast<RenderParam*>(param);
    auto* self = rp->self;
    const int  frame = rp->frame;
    delete rp;

    if (!buf || w <= 0 || h <= 0) { self->rendering_ = false; return; }

    // BGRA に変換しつつアルファ合成（チェッカー背景）
    std::vector<uint8_t> bgra(w * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + y * pitch;
        uint8_t*       dst = bgra.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint8_t r = row[0], g = row[1], b = row[2], a = row[3];
            const int     bg = ((x / self->theme_.checker_size ^ y / self->theme_.checker_size) & 1)
                               ? self->theme_.checker_color2 : self->theme_.checker_color1;
            dst[0] = (uint8_t)((b * a + bg * (255 - a) + 127) / 255);
            dst[1] = (uint8_t)((g * a + bg * (255 - a) + 127) / 255);
            dst[2] = (uint8_t)((r * a + bg * (255 - a) + 127) / 255);
            dst[3] = 255;
            row += 4; dst += 4;
        }
    }

    {
        std::lock_guard<std::mutex> lk(self->pending_mutex_);
        self->pending_ = { std::move(bgra), w, h, frame, true };
    }
    self->rendering_ = false;
    if (self->notify_hwnd_) PostMessage(self->notify_hwnd_, WM_THUMBNAIL_READY, 0, 0);
}

// ============================================================
//  SettingsDialog
// ============================================================
void SettingsDialog::init(Settings* settings, HWND host_app_window, CONFIG_HANDLE* config,
    std::function<void()> on_ok)
{
    settings_ = settings;
    host_wnd_ = host_app_window;
    config_   = config;
    on_ok_    = std::move(on_ok);
}

void SettingsDialog::show(HWND parent)
{
    backup_ = *settings_;

    HINSTANCE hinst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&s_dlg_proc), &hinst);

    DialogBoxParamW(hinst, MAKEINTRESOURCEW(IDD_SETTINGS_DIALOG), parent, s_dlg_proc, reinterpret_cast<LPARAM>(this));
}

void SettingsDialog::apply_translations(HWND hwnd)
{
    // ダイアログ自体のキャプション
    SetWindowTextW(hwnd, tr(config_, L"タイムラインプレビュー 設定"));

    // ラベル・チェックボックスを翻訳
    struct { int id; LPCWSTR key; } items[] = {
        { IDC_LABEL_THUMB_W,      L"サムネイル幅"                     },
        { IDC_LABEL_TIMER_MS,     L"タイマー間隔 (ms)"                 },
        { IDC_SHOW_THUMB,         L"サムネイルを表示"                   },
        { IDC_SHOW_HEAD,          L"ヘッダーを表示"                     },
        { IDC_SHOW_PARAM,         L"メインエフェクトのパラメータを表示"  },
        { IDC_SHOW_FILTERS,       L"フィルタ効果を表示"                  },
        { IDC_SHOW_FILTER_PARAMS, L"フィルタ効果のパラメータを表示"      },
        { IDCANCEL,               L"キャンセル"                          },
    };
    for (const auto& item : items) {
        HWND hc = GetDlgItem(hwnd, item.id);
        if (hc) SetWindowTextW(hc, tr(config_, item.key));
    }
}

void SettingsDialog::sync_ui_to_settings(HWND hwnd)
{
    auto set_check = [hwnd](int id, bool v) {
        CheckDlgButton(hwnd, id, v ? BST_CHECKED : BST_UNCHECKED);
    };
    SetDlgItemInt(hwnd, IDC_THUMB_W,  settings_->thumb_w, FALSE);
    SetDlgItemInt(hwnd, IDC_TIMER_MS, settings_->timer_ms, FALSE);
    set_check(IDC_SHOW_THUMB,         settings_->show_thumb);
    set_check(IDC_SHOW_HEAD,          settings_->show_header);
    set_check(IDC_SHOW_PARAM,         settings_->show_params);
    set_check(IDC_SHOW_FILTERS,       settings_->show_filters);
    set_check(IDC_SHOW_FILTER_PARAMS, settings_->show_filter_params);
}

bool SettingsDialog::sync_settings_from_ui(HWND hwnd)
{
    auto is_checked = [hwnd](int id) {
        return IsDlgButtonChecked(hwnd, id) == BST_CHECKED;
    };

    BOOL translated = FALSE;
    UINT val = GetDlgItemInt(hwnd, IDC_THUMB_W, &translated, FALSE);
    if (translated && val > 0) settings_->thumb_w = static_cast<int>(val);

    val = GetDlgItemInt(hwnd, IDC_TIMER_MS, &translated, FALSE);
    if (translated && val > 0) settings_->timer_ms = static_cast<int>(val);

    settings_->show_thumb         = is_checked(IDC_SHOW_THUMB);
    settings_->show_header        = is_checked(IDC_SHOW_HEAD);
    settings_->show_params        = is_checked(IDC_SHOW_PARAM);
    settings_->show_filters       = is_checked(IDC_SHOW_FILTERS);
    settings_->show_filter_params = is_checked(IDC_SHOW_FILTER_PARAMS);
    return true;
}

INT_PTR CALLBACK SettingsDialog::s_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsDialog* self = nullptr;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<SettingsDialog*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->apply_translations(hwnd);
        self->sync_ui_to_settings(hwnd);
        return TRUE;
    }
    self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND:
        if (!self) break;
        if (LOWORD(wp) == IDOK) {
            self->sync_settings_from_ui(hwnd);
            if (self->on_ok_) self->on_ok_();
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            *self->settings_ = self->backup_;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
