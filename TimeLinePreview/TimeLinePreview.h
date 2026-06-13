#pragma once
//----------------------------------------------------------------------------------
//  TimeLinePreview.h
//----------------------------------------------------------------------------------
#include "pch.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <string>

// ============================================================
//  定数
// ============================================================
namespace timeline_preview {

    // プラグイン識別名（文字列リテラル連結のベースとして使用）
#define TIMELINE_PREVIEW_NAME L"タイムラインプレビュー"
    inline constexpr LPCWSTR PLUGIN_NAME        = TIMELINE_PREVIEW_NAME;
    inline constexpr LPCWSTR PLUGIN_VERSION_STR = TIMELINE_PREVIEW_NAME L" version 1.0.0";

    // ウィンドウクラス名
    inline constexpr LPCWSTR POPUP_WINDOW_CLASS = L"TimeLinePreview" L"Popup";

    // カスタムメッセージ
    inline constexpr UINT WM_THUMBNAIL_READY = WM_USER + 1;

    // テーマ設定
    struct Theme {
        int      pad                 = 6;
        int      line_h              = 16;
        COLORREF bg_color            = RGB(30, 30, 30);
        COLORREF header_color        = RGB(120, 200, 255);
        COLORREF param_label_color   = RGB(200, 200, 200);
        COLORREF param_value_color   = RGB(255, 220, 120);
        COLORREF separator_color     = RGB(70, 70, 70);
        COLORREF filter_name_color   = RGB(160, 220, 160);
        COLORREF filter_label_color  = RGB(180, 180, 180);
        COLORREF filter_value_color  = RGB(220, 200, 100);
        int      checker_size        = 16;
        int      checker_color1      = 32;
        int      checker_color2      = 48;
    };

    // パラメータ表示上限行数
    inline constexpr int MAX_PARAM_LINES = 8;

    // INI セクション名・ファイル名（TIMELINE_PREVIEW_NAME を共有）
    inline constexpr LPCWSTR INI_SECTION  = TIMELINE_PREVIEW_NAME;
    inline constexpr LPCWSTR INI_SUBDIR   = L"Plugin";
    inline constexpr LPCWSTR INI_FILENAME = TIMELINE_PREVIEW_NAME L".ini";

    // INI キー名
    namespace ini_key {
        inline constexpr LPCWSTR THUMB_W            = L"thumb_w";
        inline constexpr LPCWSTR TIMER_MS           = L"timer_ms";
        inline constexpr LPCWSTR SHOW_THUMB         = L"show_thumb";
        inline constexpr LPCWSTR SHOW_HEADER        = L"show_header";
        inline constexpr LPCWSTR SHOW_PARAMS        = L"show_params";
        inline constexpr LPCWSTR SHOW_FILTERS       = L"show_filters";
        inline constexpr LPCWSTR SHOW_FILTER_PARAMS = L"show_filter_params";
    }

    // 翻訳ヘルパー
    // config が nullptr または未定義の場合は key をそのまま返す
    inline LPCWSTR tr(CONFIG_HANDLE* config, LPCWSTR key)
    {
        if (!config || !config->translate) return key;
        return config->translate(config, key);
    }

} // namespace timeline_preview

// ============================================================
//  データ型
// ============================================================
class Settings {
public:
    int  thumb_w             = 320;
    int  timer_ms            = 1000;
    bool show_thumb          = true;
    bool show_header         = true;
    bool show_params         = true;
    bool show_filters        = true;
    bool show_filter_params  = false;

    void load(CONFIG_HANDLE* config);
    void save(CONFIG_HANDLE* config) const;

private:
    std::wstring get_ini_path(CONFIG_HANDLE* config) const;
};

struct ThumbnailData {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0, frame = -1;
};

struct PendingThumb {
    std::vector<uint8_t> pixels;
    int  width = 0, height = 0, frame = -1;
    bool ready = false;
};

struct ParamLine {
    std::wstring label, value;
};

struct FilterEffect {
    std::wstring           name;
    std::vector<ParamLine> lines;
};

struct ObjectParams {
    std::wstring              effect_name;
    int                       layer       = -1;
    int                       frame_start = -1;
    int                       frame_end   = -1;
    std::vector<ParamLine>    lines;
    std::vector<FilterEffect> filters;
    bool                      valid = false;
};

// ============================================================
//  レイアウト計算結果（PreviewPopup::calc_layout が返す）
// ============================================================
struct PopupLayout {
    int total_w = 0;
    int total_h = 0;
    int thumb_h = 0;    // サムネイル描画高さ（0 = 非表示）
    int header_y = 0;
    int params_y = 0;
    int separator_y = 0;
    int filters_y = 0;
    bool has_thumb    = false;
    bool has_header   = false;
    bool has_params   = false;
    bool has_separator = false;
    bool has_filters  = false;
};

// ============================================================
//  PreviewPopup
// ============================================================
class PreviewPopup {
public:
    bool create(HINSTANCE hinst,
                std::function<void()> on_timer,
                std::function<void()> on_thumbnail_ready);
    void destroy();

    void set_thumb(ThumbnailData&& thumb);
    void reset_thumb();
    void set_params(ObjectParams params);
    void mark_dirty() { dirty_ = true; }

    void show(POINT screen_pt, const Settings& s);
    void hide();

    bool is_visible() const { return visible_; }
    HWND hwnd()       const { return hwnd_; }

private:
    PopupLayout calc_layout(const Settings& s) const;

    static LRESULT CALLBACK s_wnd_proc(HWND, UINT, WPARAM, LPARAM);
    void on_paint(HDC hdc, const RECT& rc);

    HWND          hwnd_     = nullptr;
    bool          visible_  = false;
    bool          dirty_    = false;
    RECT          last_rect_= {-1, -1, -1, -1};

    ThumbnailData thumb_;
    ObjectParams  params_;
    Settings      settings_;
    timeline_preview::Theme         theme_;

    std::function<void()> on_timer_;
    std::function<void()> on_thumbnail_ready_;
};

// ============================================================
//  TimelineInspector
// ============================================================
class TimelineInspector {
public:
    void init(EDIT_HANDLE*    edit,
              LOG_HANDLE*     log,
              PreviewPopup*   popup,
              const Settings* settings,
              HWND            notify_hwnd,
              HWND            host_app_window);

    void on_mouse_move(POINT pt);
    void on_button_down();
    void on_context_menu_open();
    void on_context_menu_close();
    void on_timer();
    void on_thumbnail_ready();

    bool  is_rendering() const { return rendering_.load(); }
    POINT last_pt()      const { return last_pt_; }

private:
    bool is_host_foreground() const;
    void update_at(POINT pt);
    void request_render(int frame);

    // C コールバック（rendering_scene_video 用）
    struct RenderParam { int frame; TimelineInspector* self; };
    static void CALLBACK s_render_done(void*, int, const void*, int, int, int);

    EDIT_HANDLE*    edit_             = nullptr;
    LOG_HANDLE*     log_              = nullptr;
    PreviewPopup*   popup_            = nullptr;
    const Settings* settings_         = nullptr;
    HWND            notify_hwnd_      = nullptr;
    HWND            host_app_window_  = nullptr;
    timeline_preview::Theme           theme_;

    std::atomic<bool> rendering_{ false };
    int               request_frame_   = -1;
    bool              contextmenu_hold_= false;
    POINT             last_pt_         = {-1, -1};

    std::mutex   pending_mutex_;
    PendingThumb pending_;
};

// ============================================================
//  SettingsDialog
// ============================================================
class SettingsDialog {
public:
    void init(Settings* settings, HWND host_app_window, CONFIG_HANDLE* config,
              std::function<void()> on_ok = nullptr);
    void show(HWND parent);

private:
    static INT_PTR CALLBACK s_dlg_proc(HWND, UINT, WPARAM, LPARAM);
    void sync_ui_to_settings(HWND hwnd);
    bool sync_settings_from_ui(HWND hwnd);
    void apply_translations(HWND hwnd);

    Settings*      settings_ = nullptr;
    HWND           host_wnd_ = nullptr;
    CONFIG_HANDLE* config_   = nullptr;
    Settings       backup_;
    std::function<void()> on_ok_;
};
