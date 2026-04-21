#pragma once
#include <Windows.h>
#include <objidl.h>   // IStream for GDI+
#include <shellapi.h>
#include <memory>
#include <string>
#include "haptics/HapticController.h"
#include "config/Config.h"
#include "ui/PopupWindow.h"

namespace ui {

constexpr UINT WM_TRAY_MSG  = WM_APP + 1;
constexpr UINT TRAY_ICON_ID = 1;
constexpr UINT IDM_SHOW     = 100;
constexpr UINT IDM_EXIT     = 101;

class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    bool init(HINSTANCE hInst);
    int  run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM);

    void createTrayIcon();
    void removeTrayIcon();
    void updateTrayIcon();
    HICON createDynamicIcon(bool connected, int battery, bool charging);

    void showPopup();
    void showContextMenu();

    void onConnected(bool connected, const std::wstring& name);
    void onBattery(int pct, bool charging);

    HINSTANCE   m_hInst     = nullptr;
    HWND        m_msgWnd    = nullptr;
    NOTIFYICONDATAW m_nid   = {};

    std::unique_ptr<haptics::HapticController> m_ctrl;
    std::unique_ptr<config::Config>            m_config;
    std::unique_ptr<PopupWindow>               m_popup;

    bool        m_connected  = false;
    int         m_battery    = -1;
    bool        m_charging   = false;
    std::wstring m_devName;

    // Low-level mouse hook for scroll haptic
    HHOOK m_mouseHook = nullptr;
    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);


    // Custom messages from worker thread
    static constexpr UINT WM_CONNECTED  = WM_APP + 10;
    static constexpr UINT WM_BATTERY    = WM_APP + 11;
};

} // namespace ui
