#include "TrayApp.h"
#include <gdiplus.h>
#include <cmath>
#include <cstring>
#include <string>
#include "../../resources/resource.h"

namespace ui {

using namespace Gdiplus;

static const wchar_t* TRAY_CLASS = L"LiveHapticsTray";

// Global pointer used by the static mouse hook (only one TrayApp exists)
static TrayApp* g_trayApp = nullptr;

// ─── Mouse hook ───────────────────────────────────────────────────────────────
LRESULT CALLBACK TrayApp::MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_trayApp && g_trayApp->m_ctrl) {
        auto* mhs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        if (g_trayApp->m_ctrl->isConnected()) {
            HWND popupHwnd = g_trayApp->m_popup ? g_trayApp->m_popup->hwnd() : nullptr;
            HWND under     = WindowFromPoint(mhs->pt);
            if (wParam == WM_MOUSEWHEEL) {
                if (under != popupHwnd)
                    g_trayApp->m_ctrl->playScroll();
            } else if (wParam == WM_MOUSEHWHEEL) {
                // Only use the hook as fallback when HID++ thumb wheel notifications
                // are unavailable.  When they ARE available, the notification thread
                // fires playSideScroll() directly — one event per physical notch,
                // regardless of what Logitech Options+ does for the active app.
                if (under != popupHwnd && !g_trayApp->m_ctrl->supportsThumbWheel())
                    g_trayApp->m_ctrl->playSideScroll();
            } else if (wParam == WM_LBUTTONDOWN) {
                if (under != popupHwnd)
                    g_trayApp->m_ctrl->playLeftClick();
            } else if (wParam == WM_RBUTTONDOWN) {
                if (under != popupHwnd)
                    g_trayApp->m_ctrl->playRightClick();
            } else if (wParam == WM_XBUTTONDOWN) {
                // XBUTTON1 = Back, XBUTTON2 = Forward (side thumb buttons)
                if (under != popupHwnd)
                    g_trayApp->m_ctrl->playSideButton();
            } else if (wParam == WM_MBUTTONDOWN) {
                // Middle button = scroll wheel click
                if (under != popupHwnd)
                    g_trayApp->m_ctrl->playScrollClick();
            }
        }
    }
    return CallNextHookEx(g_trayApp ? g_trayApp->m_mouseHook : nullptr,
                          nCode, wParam, lParam);
}

// ─── Tray icon ────────────────────────────────────────────────────────────────
HICON TrayApp::createDynamicIcon(bool connected, int battery) {
    const int SZ = 32;

    BITMAPINFO bmi     = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = SZ;
    bmi.bmiHeader.biHeight      = -SZ;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   pBits = nullptr;
    HBITMAP hDib  = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HDC     hdc   = CreateCompatibleDC(nullptr);
    HBITMAP hOld  = (HBITMAP)SelectObject(hdc, hDib);
    memset(pBits, 0, SZ * SZ * 4);

    HICON base = (HICON)LoadImageW(m_hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                   IMAGE_ICON, SZ, SZ, LR_DEFAULTCOLOR);
    if (base) {
        DrawIconEx(hdc, 0, 0, base, SZ, SZ, 0, nullptr, DI_NORMAL);
        DestroyIcon(base);
    } else {
        Gdiplus::Graphics gFb(hdc);
        gFb.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush fb(Gdiplus::Color(255, 75, 55, 180));
        gFb.FillEllipse(&fb, Gdiplus::RectF(1, 1, SZ - 2.f, SZ - 2.f));
    }

    {
        Gdiplus::Graphics go(hdc);
        go.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::Color dotClr;
        if (connected) {
            if (battery >= 0 && battery <= 10)
                dotClr = Gdiplus::Color(255, 255, 69, 58);
            else
                dotClr = Gdiplus::Color(255, 48, 209, 88);
        } else {
            dotClr = Gdiplus::Color(220, 120, 120, 120);
        }
        Gdiplus::SolidBrush shadow(Gdiplus::Color(100, 0, 0, 0));
        go.FillEllipse(&shadow, Gdiplus::RectF(SZ - 9.5f, SZ - 9.5f, 9.f, 9.f));
        Gdiplus::SolidBrush dot(dotClr);
        go.FillEllipse(&dot, Gdiplus::RectF(SZ - 10.f, SZ - 10.f, 8.f, 8.f));
    }

    SelectObject(hdc, hOld);
    DeleteDC(hdc);

    ICONINFO ii  = {};
    ii.fIcon     = TRUE;
    ii.hbmMask   = CreateBitmap(SZ, SZ, 1, 1, nullptr);
    ii.hbmColor  = hDib;
    HICON icon   = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    DeleteObject(hDib);
    return icon;
}

// ─── TrayApp lifecycle ────────────────────────────────────────────────────────
TrayApp::TrayApp() = default;

TrayApp::~TrayApp() {
    if (m_mouseHook) { UnhookWindowsHookEx(m_mouseHook); m_mouseHook = nullptr; }
    g_trayApp = nullptr;
    removeTrayIcon();
    if (m_ctrl) m_ctrl->stop();
}

bool TrayApp::init(HINSTANCE hInst) {
    m_hInst = hInst;

    WNDCLASSEXW wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.lpszClassName= TRAY_CLASS;
    RegisterClassExW(&wc);

    m_msgWnd = CreateWindowExW(0, TRAY_CLASS, L"", 0,
                                0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, this);
    if (!m_msgWnd) return false;

    m_config = std::make_unique<config::Config>();

    // Popup
    m_popup = std::make_unique<PopupWindow>();
    if (!m_popup->create(hInst)) return false;

    // Sync initial popup state from config
    m_popup->setScrollWaveform      (m_config->haptic.scrollWaveform);
    m_popup->setSideScrollWaveform  (m_config->haptic.sideScrollWaveform);
    m_popup->setClickWaveform       (m_config->haptic.clickWaveform);
    m_popup->setRightClickWaveform  (m_config->haptic.rightClickWaveform);
    m_popup->setScrollClickWaveform (m_config->haptic.scrollClickWaveform);
    m_popup->setHoverWaveform       (m_config->haptic.hoverWaveform);
    m_popup->setScrollEnabled       (m_config->haptic.scrollEnabled);
    m_popup->setSideScrollEnabled   (m_config->haptic.sideScrollEnabled);
    m_popup->setClickEnabled        (m_config->haptic.clickEnabled);
    m_popup->setRightClickEnabled   (m_config->haptic.rightClickEnabled);
    m_popup->setSideButtonEnabled   (m_config->haptic.sideButtonEnabled);
    m_popup->setScrollClickEnabled  (m_config->haptic.scrollClickEnabled);
    m_popup->setHoverEnabled        (m_config->haptic.hoverEnabled);
    m_popup->setSideButtonWaveform  (m_config->haptic.sideButtonWaveform);
    m_popup->setHoverMode           (m_config->haptic.hoverMode);
    m_popup->setHoverOnlyFocused    (m_config->haptic.hoverOnlyFocused);
    m_popup->setDelay(0, m_config->haptic.scrollCooldownMs);
    m_popup->setDelay(1, m_config->haptic.sideScrollCooldownMs);
    m_popup->setDelay(2, m_config->haptic.clickCooldownMs);
    m_popup->setDelay(3, m_config->haptic.rightClickCooldownMs);
    m_popup->setDelay(4, m_config->haptic.sideButtonCooldownMs);
    m_popup->setDelay(5, m_config->haptic.scrollClickCooldownMs);
    m_popup->setDelay(6, m_config->haptic.hoverCooldownMs);

    // Waveform callbacks – update config + save
    m_popup->setScrollWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.scrollWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().scrollWaveform = wf;
    });
    m_popup->setSideScrollWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.sideScrollWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().sideScrollWaveform = wf;
    });
    m_popup->setClickWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.clickWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().clickWaveform = wf;
    });
    m_popup->setRightClickWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.rightClickWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().rightClickWaveform = wf;
    });
    m_popup->setHoverWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.hoverWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().hoverWaveform = wf;
    });
    m_popup->setSideButtonWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.sideButtonWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().sideButtonWaveform = wf;
    });
    m_popup->setScrollClickWaveformCallback([this](hidpp::Waveform wf) {
        m_config->haptic.scrollClickWaveform = wf; m_config->save();
        if (m_ctrl) m_ctrl->config().scrollClickWaveform = wf;
    });

    // Enable/disable callbacks
    m_popup->setScrollEnabledCallback([this](bool e) {
        m_config->haptic.scrollEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().scrollEnabled = e;
    });
    m_popup->setSideScrollEnabledCallback([this](bool e) {
        m_config->haptic.sideScrollEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().sideScrollEnabled = e;
    });
    m_popup->setClickEnabledCallback([this](bool e) {
        m_config->haptic.clickEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().clickEnabled = e;
    });
    m_popup->setRightClickEnabledCallback([this](bool e) {
        m_config->haptic.rightClickEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().rightClickEnabled = e;
    });
    m_popup->setHoverEnabledCallback([this](bool e) {
        m_config->haptic.hoverEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().hoverEnabled = e;
    });
    m_popup->setSideButtonEnabledCallback([this](bool e) {
        m_config->haptic.sideButtonEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().sideButtonEnabled = e;
    });
    m_popup->setScrollClickEnabledCallback([this](bool e) {
        m_config->haptic.scrollClickEnabled = e; m_config->save();
        if (m_ctrl) m_ctrl->config().scrollClickEnabled = e;
    });

    // Mode + cooldown callbacks
    m_popup->setHoverModeCallback([this](haptics::HoverMode mode) {
        m_config->haptic.hoverMode = mode; m_config->save();
        if (m_ctrl) m_ctrl->config().hoverMode = mode;
    });
    m_popup->setHoverOnlyFocusedCallback([this](bool e) {
        m_config->haptic.hoverOnlyFocused = e; m_config->save();
        if (m_ctrl) m_ctrl->config().hoverOnlyFocused = e;
    });
    m_popup->setDelayCallback([this](int type, uint32_t ms) {
        auto& h = m_config->haptic;
        switch (type) {
        case 0: h.scrollCooldownMs      = ms; if (m_ctrl) m_ctrl->config().scrollCooldownMs      = ms; break;
        case 1: h.sideScrollCooldownMs  = ms; if (m_ctrl) m_ctrl->config().sideScrollCooldownMs  = ms; break;
        case 2: h.clickCooldownMs       = ms; if (m_ctrl) m_ctrl->config().clickCooldownMs       = ms; break;
        case 3: h.rightClickCooldownMs  = ms; if (m_ctrl) m_ctrl->config().rightClickCooldownMs  = ms; break;
        case 4: h.sideButtonCooldownMs  = ms; if (m_ctrl) m_ctrl->config().sideButtonCooldownMs  = ms; break;
        case 5: h.scrollClickCooldownMs = ms; if (m_ctrl) m_ctrl->config().scrollClickCooldownMs = ms; break;
        case 6: h.hoverCooldownMs       = ms; if (m_ctrl) m_ctrl->config().hoverCooldownMs       = ms; break;
        }
        m_config->save();
    });
    m_popup->setPreviewCallback([this](hidpp::Waveform wf) {
        if (m_ctrl) m_ctrl->play(wf);
    });

    m_popup->setCloseCallback([this]() { m_popup->hide(); });

    // Haptic controller
    m_ctrl = std::make_unique<haptics::HapticController>();
    m_ctrl->config() = m_config->haptic;

    m_ctrl->setConnectCallback([this](bool connected, const std::wstring& name) {
        std::wstring* pName = new std::wstring(name);
        PostMessageW(m_msgWnd, WM_CONNECTED, connected ? 1 : 0,
                     reinterpret_cast<LPARAM>(pName));
    });
    m_ctrl->setBatteryCallback([this](int pct) {
        PostMessageW(m_msgWnd, WM_BATTERY, static_cast<WPARAM>(pct), 0);
    });

    createTrayIcon();

    m_ctrl->start();

    // Install global mouse hook
    g_trayApp   = this;
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, nullptr, 0);

    return true;
}

int TrayApp::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// ─── Tray icon ────────────────────────────────────────────────────────────────
void TrayApp::createTrayIcon() {
    memset(&m_nid, 0, sizeof(m_nid));
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = m_msgWnd;
    m_nid.uID              = TRAY_ICON_ID;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    m_nid.uCallbackMessage = WM_TRAY_MSG;
    m_nid.hIcon            = createDynamicIcon(false, -1);
    wcscpy_s(m_nid.szTip, L"Live Haptics");
    Shell_NotifyIconW(NIM_ADD, &m_nid);

    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
}

void TrayApp::removeTrayIcon() {
    if (m_nid.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        if (m_nid.hIcon) { DestroyIcon(m_nid.hIcon); m_nid.hIcon = nullptr; }
    }
}

void TrayApp::updateTrayIcon() {
    if (m_nid.hIcon) DestroyIcon(m_nid.hIcon);
    m_nid.hIcon  = createDynamicIcon(m_connected, m_battery);
    m_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;

    if (m_connected) {
        if (m_battery >= 0)
            swprintf_s(m_nid.szTip, L"Live Haptics \u2022 %s \u2022 %d%%",
                       m_devName.c_str(), m_battery);
        else
            swprintf_s(m_nid.szTip, L"Live Haptics \u2022 %s", m_devName.c_str());
    } else {
        wcscpy_s(m_nid.szTip, L"Live Haptics");
    }
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

// ─── Popup ────────────────────────────────────────────────────────────────────
void TrayApp::showPopup() {
    if (m_popup->isVisible()) {
        m_popup->hide();
        return;
    }

    SetForegroundWindow(m_msgWnd);

    RECT iconRect = {};
    NOTIFYICONIDENTIFIER nii = {};
    nii.cbSize = sizeof(nii);
    nii.hWnd   = m_msgWnd;
    nii.uID    = TRAY_ICON_ID;
    Shell_NotifyIconGetRect(&nii, &iconRect);

    POINT anchor = { (iconRect.left + iconRect.right) / 2,
                     (iconRect.top  + iconRect.bottom) / 2 };
    if (anchor.x == 0 && anchor.y == 0)
        GetCursorPos(&anchor);

    m_popup->setConnected(m_connected, m_devName);
    m_popup->setBattery(m_battery);
    m_popup->setHapticsSupported(m_ctrl && m_ctrl->supportsHaptics());
    m_popup->show(anchor);
}

void TrayApp::showContextMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Open Live Haptics");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(m_msgWnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, m_msgWnd, nullptr);
    DestroyMenu(menu);
}

// ─── Callbacks from worker ────────────────────────────────────────────────────
void TrayApp::onConnected(bool connected, const std::wstring& name) {
    m_connected = connected;
    m_devName   = name.empty() ? L"MX Master 4" : name;
    updateTrayIcon();
    if (m_popup->isVisible()) {
        m_popup->setConnected(m_connected, m_devName);
        m_popup->setHapticsSupported(m_ctrl && m_ctrl->supportsHaptics());
    }
}

void TrayApp::onBattery(int pct) {
    m_battery = pct;
    updateTrayIcon();
    if (m_popup->isVisible()) m_popup->setBattery(pct);
}

// ─── WndProc ──────────────────────────────────────────────────────────────────
LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayApp* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<TrayApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_TRAY_MSG:
        switch (LOWORD(lp)) {
        case NIN_SELECT:        // left-click / keyboard select – fires ONCE
            showPopup();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showContextMenu();
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SHOW: showPopup();         break;
        case IDM_EXIT:
            removeTrayIcon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_CONNECTED: {
        bool         connected = (wp != 0);
        std::wstring* pName   = reinterpret_cast<std::wstring*>(lp);
        std::wstring  name    = pName ? *pName : L"";
        delete pName;
        onConnected(connected, name);
        return 0;
    }

    case WM_BATTERY:
        onBattery(static_cast<int>(wp));
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace ui
