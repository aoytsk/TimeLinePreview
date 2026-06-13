//----------------------------------------------------------------------------------
//  dllmain.cpp
//  TimeLinePreviewPlugin — SDK エントリポイント
//----------------------------------------------------------------------------------
#include "pch.h"
#include "TimeLinePreview.h"

using namespace timeline_preview;

// ============================================================
//  プラグイン定数
// ============================================================
namespace {
    inline constexpr DWORD    REQUIRED_VERSION = 2004800;
    inline constexpr UINT_PTR TIMER_ID         = 1;
}

// ============================================================
//  TimeLinePreviewPlugin
// ============================================================
class TimeLinePreviewPlugin {
public:
    TimeLinePreviewPlugin()  { g_plugin_ptr = this; }
    ~TimeLinePreviewPlugin() { g_plugin_ptr = nullptr; }

    void set_logger(LOG_HANDLE* h)    { logger_ = h; }
    void set_config(CONFIG_HANDLE* h) { config_ = h; }

    bool initialize(DWORD version)
    {
        host_version_ = version;
        if (logger_) {
            wchar_t buf[128];
            swprintf_s(buf, L"[TLP] host version: %lu", version);
            logger_->log(logger_, buf);
        }
        return true;
    }

    void uninitialize()
    {
        remove_hooks();
        stop_timer();
        popup_.destroy();
    }

    bool register_plugin(HOST_APP_TABLE* host)
    {
        HINSTANCE hinst = GetModuleHandle(nullptr);

        if (!popup_.create(hinst,
                [this] { inspector_.on_timer(); },
                [this] { inspector_.on_thumbnail_ready(); }))
            return false;

        edit_handle_ = host->create_edit_handle();
        HWND host_wnd = edit_handle_->get_host_app_window();

        inspector_.init(
            edit_handle_,
            logger_,
            &popup_,
            &settings_,
            popup_.hwnd(),
            host_wnd);

        settings_.load(config_);

        settings_dlg_.init(&settings_, host_wnd, config_,
            [this] {
                settings_.save(config_);
                if (popup_.hwnd()) {
                    KillTimer(popup_.hwnd(), TIMER_ID);
                    SetTimer(popup_.hwnd(), TIMER_ID, settings_.timer_ms, nullptr);
                }
            });

        host->register_config_menu(
            tr(config_, PLUGIN_NAME),
            [](HWND parent, HINSTANCE) {
                if (g_plugin_ptr) g_plugin_ptr->settings_dlg_.show(parent);
            });

        install_hooks();
        start_timer();
        return true;
    }

    void start_timer()
    {
        if (popup_.hwnd()) SetTimer(popup_.hwnd(), TIMER_ID, settings_.timer_ms, nullptr);
    }

    void stop_timer()
    {
        if (popup_.hwnd()) KillTimer(popup_.hwnd(), TIMER_ID);
    }

private:
    // --------------------------------------------------------
    //  メンバ変数
    // --------------------------------------------------------
    Settings          settings_;
    PreviewPopup      popup_;
    TimelineInspector inspector_;
    SettingsDialog    settings_dlg_;
    EDIT_HANDLE*      edit_handle_ = nullptr;
    LOG_HANDLE*       logger_      = nullptr;
    DWORD             host_version_= 0;
    CONFIG_HANDLE*    config_      = nullptr;

    wil::unique_hhook mouse_hook_;
    wil::unique_hhook callwnd_hook_;
    wil::unique_hhook callwnd_ret_hook_;

    // g_plugin_ptr への参照は static コールバックからのみ使用
    static TimeLinePreviewPlugin* g_plugin_ptr;

    // --------------------------------------------------------
    //  フック
    // --------------------------------------------------------
    void install_hooks()
    {
        const DWORD tid = GetCurrentThreadId();
        mouse_hook_       .reset(SetWindowsHookEx(WH_MOUSE,          s_mouse_hook,       nullptr, tid));
        callwnd_hook_     .reset(SetWindowsHookEx(WH_CALLWNDPROC,    s_callwnd_hook,     nullptr, tid));
        callwnd_ret_hook_ .reset(SetWindowsHookEx(WH_CALLWNDPROCRET, s_callwnd_ret_hook, nullptr, tid));
    }

    void remove_hooks()
    {
        mouse_hook_.reset();
        callwnd_hook_.reset();
        callwnd_ret_hook_.reset();
    }

    static LRESULT CALLBACK s_mouse_hook(int code, WPARAM wp, LPARAM lp)
    {
        if (code == HC_ACTION && g_plugin_ptr) {
            const auto* ms = reinterpret_cast<MOUSEHOOKSTRUCT*>(lp);
            switch (wp) {
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN: case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
            case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
                g_plugin_ptr->inspector_.on_button_down();
                break;
            case WM_MOUSEMOVE:
                g_plugin_ptr->inspector_.on_mouse_move(ms->pt);
                break;
            }
        }
        return CallNextHookEx(g_plugin_ptr ? g_plugin_ptr->mouse_hook_.get() : nullptr, code, wp, lp);
    }

    static LRESULT CALLBACK s_callwnd_hook(int code, WPARAM, LPARAM lp)
    {
        if (code == HC_ACTION && g_plugin_ptr) {
            const auto* cwp = reinterpret_cast<CWPSTRUCT*>(lp);
            if (cwp && cwp->message == WM_CONTEXTMENU)
                g_plugin_ptr->inspector_.on_context_menu_open();
        }
        return CallNextHookEx(g_plugin_ptr ? g_plugin_ptr->callwnd_hook_.get() : nullptr, code, 0, lp);
    }

    static LRESULT CALLBACK s_callwnd_ret_hook(int code, WPARAM, LPARAM lp)
    {
        if (code == HC_ACTION && g_plugin_ptr) {
            const auto* cwpr = reinterpret_cast<CWPRETSTRUCT*>(lp);
            if (cwpr && cwpr->message == WM_CONTEXTMENU)
                g_plugin_ptr->inspector_.on_context_menu_close();
        }
        return CallNextHookEx(g_plugin_ptr ? g_plugin_ptr->callwnd_ret_hook_.get() : nullptr, code, 0, lp);
    }
};

TimeLinePreviewPlugin* TimeLinePreviewPlugin::g_plugin_ptr = nullptr;

// ============================================================
//  SDK エクスポート
// ============================================================
namespace {
    TimeLinePreviewPlugin g_plugin;

    COMMON_PLUGIN_TABLE common_plugin_table = {
        PLUGIN_NAME,
        PLUGIN_VERSION_STR,
    };
}

EXTERN_C __declspec(dllexport) DWORD               RequiredVersion()        { return REQUIRED_VERSION; }
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable()  { return &common_plugin_table; }
EXTERN_C __declspec(dllexport) void                 InitializeLogger(LOG_HANDLE* h)    { g_plugin.set_logger(h); }
EXTERN_C __declspec(dllexport) void                 InitializeConfig(CONFIG_HANDLE* h) { g_plugin.set_config(h); }
EXTERN_C __declspec(dllexport) bool                 InitializePlugin(DWORD v)          { return g_plugin.initialize(v); }
EXTERN_C __declspec(dllexport) void                 UninitializePlugin()               { g_plugin.uninitialize(); }
EXTERN_C __declspec(dllexport) void                 RegisterPlugin(HOST_APP_TABLE* h)  { g_plugin.register_plugin(h); }
