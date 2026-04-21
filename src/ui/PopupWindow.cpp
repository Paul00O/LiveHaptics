#include "PopupWindow.h"
#include "hidpp/HidppDevice.h"
#include "../../resources/resource.h"
#include <dwmapi.h>
#include <objidl.h>
#include <combaseapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "dwmapi.lib")

namespace ui {

using namespace Gdiplus;

static const wchar_t* CLASS_NAME = L"LiveHapticsPopup2";

// ─── Per-feature accent colors (COLORREF 0xBBGGRR) ───────────────────────────
static const COLORREF TYPE_COLORS[NUM_TYPES] = {
    0x6FC926,  // Scroll
    0x6FC926,  // Side Scroll
    0x6FC926,  // L. Click
    0x6FC926,  // R. Click
    0x6FC926,  // Side Buttons
    0x6FC926,  // Mid Click
    0x6FC926,  // Hover
};

static const wchar_t* TYPE_LABELS[NUM_TYPES] = {
    L"SCROLL", L"SIDE SCROLL", L"L. CLICK",
    L"R. CLICK", L"SIDE BUTTONS", L"MID CLICK", L"HOVER"
};

static const wchar_t* HOVER_MODE_LABELS[] = { L"ENTER", L"EXIT", L"BOTH" };

// ─── Hotspot positions ────────────────────────────────────────────────────────
// Coordinates are in pixels within a IMG_REF_W × IMG_REF_H reference frame
// (the actual drawn image is scaled to fit 380×490, same proportions).
// To reposition a hotspot: change px/py. ldx/ldy is the annotation line direction.
constexpr float IMG_REF_W = 380.f;
constexpr float IMG_REF_H = 490.f;

struct HotspotDef {
    float px, py;         // center in reference pixels
    const wchar_t* label;
    float ldx, ldy;       // annotation line direction vector
};

static const HotspotDef HOTSPOTS[5] = {
    // ── edit px/py to reposition ──────────────────────────────── ldx   ldy
    /* 0  Scroll wheel      */ { 282.f,  140.f, L"SCROLL",       -1.0f,  0.5f },
    /* 1  Side scroll       */ {  209.f, 250.f, L"SIDE SCROLL",   1.0f,  0.5f },
    /* 2  Left click        */ { 185.f, 40.f, L"L. CLICK",      -1.0f,  0.0f },
    /* 3  Right click       */ { 300.f, 70.f, L"R. CLICK",      0.9f, -0.4f },
    /* 4  Side buttons      */ { 160.f, 218.f, L"SIDE BUTTONS", -1.0f,  0.0f },
};

// ─── Panel layout (all y values are absolute window coords) ──────────────────
constexpr float PP          = 16.f;    // panel padding
constexpr float FEAT_Y      = float(HEADER_H) + 8.f;
constexpr float FEAT_H      = 52.f;
constexpr float WF_LABEL_Y  = FEAT_Y + FEAT_H + 10.f;
constexpr int   WF_COLS     = 3;
constexpr float WF_CELL_W   = (PANEL_W - 2*PP - (WF_COLS-1)*6.f) / WF_COLS;  // ~72
constexpr float WF_CELL_H   = 48.f;
constexpr float WF_CELL_GAP = 6.f;
constexpr float WF_GRID_Y   = WF_LABEL_Y + 18.f;
constexpr int   WF_ROWS     = (hidpp::SHOWN_WF_COUNT + WF_COLS - 1) / WF_COLS; // 4
constexpr float WF_GRID_H   = WF_ROWS * WF_CELL_H + (WF_ROWS-1) * WF_CELL_GAP;

constexpr float DLY_Y       = WF_GRID_Y + WF_GRID_H + 14.f;
constexpr float DLY_H       = 40.f;
constexpr float HOVER_CTL_Y  = DLY_Y + DLY_H + 12.f;
constexpr float MC_SECT_Y    = DLY_Y + DLY_H + 18.f;
constexpr float MC_WF_GRID_Y = MC_SECT_Y + FEAT_H + 28.f;
constexpr float MC_DLY_Y     = MC_WF_GRID_Y + WF_GRID_H + 14.f;

constexpr float    HS_RADIUS    = 14.f;  // uniform hotspot circle radius (px)
constexpr uint32_t DELAY_MAX_MS = 2000;
constexpr int ANIM_TIMER_ID = 42;
constexpr int ANIM_TIMER_MS = 14;

// ─── Helpers ─────────────────────────────────────────────────────────────────
static inline float clamp01(float v) { return v<0.f?0.f:(v>1.f?1.f:v); }
static inline float lerpf(float a, float b, float t) { return a + (b-a)*t; }

static float easeInOutCubic(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4*t*t*t : 1 - powf(-2*t+2, 3)/2;
}

static GraphicsPath* makeRoundRectPath(const RectF& r, float rad) {
    auto* p = new GraphicsPath();
    if (rad <= 0.f) { p->AddRectangle(r); return p; }
    float d = rad * 2.f;
    p->AddArc(r.X,           r.Y,            d, d, 180, 90);
    p->AddArc(r.X+r.Width-d, r.Y,            d, d, 270, 90);
    p->AddArc(r.X+r.Width-d, r.Y+r.Height-d, d, d,   0, 90);
    p->AddArc(r.X,           r.Y+r.Height-d, d, d,  90, 90);
    p->CloseFigure();
    return p;
}

// Load PNG from embedded Win32 resource into a GDI+ Image
static Image* loadImageFromResource(HINSTANCE hInst, int resId) {
    HRSRC hrsrc = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hrsrc) return nullptr;
    HGLOBAL hg = LoadResource(hInst, hrsrc);
    if (!hg) return nullptr;
    DWORD sz = SizeofResource(hInst, hrsrc);
    void* data = LockResource(hg);
    if (!data || !sz) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (!hMem) return nullptr;
    void* pMem = GlobalLock(hMem);
    if (!pMem) { GlobalFree(hMem); return nullptr; }
    memcpy(pMem, data, sz);
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream))) {
        GlobalFree(hMem);
        return nullptr;
    }
    Image* img = Image::FromStream(pStream);
    pStream->Release();
    return (img && img->GetLastStatus() == Ok) ? img : (delete img, nullptr);
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────
PopupWindow::PopupWindow() {
    GdiplusStartupInput gsi;
    GdiplusStartup(&m_gdiplusToken, &gsi, nullptr);
}

PopupWindow::~PopupWindow() {
    if (m_animTimer) { KillTimer(m_hwnd, ANIM_TIMER_ID); m_animTimer = 0; }
    for (int i = 0; i < NUM_TYPES; i++) {
        delete m_panelCache[i];
    }
    delete m_bgCache;
    delete m_mouseImg;
    if (m_hwnd) DestroyWindow(m_hwnd);
    GdiplusShutdown(m_gdiplusToken);
}

// ─── create() ────────────────────────────────────────────────────────────────
bool PopupWindow::create(HINSTANCE hInst) {
    m_hInst = hInst;
    m_mouseImg = loadImageFromResource(hInst, IDR_MOUSE_PNG);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.style         = CS_DROPSHADOW;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        CLASS_NAME, L"LiveHaptics",
        WS_POPUP,
        0, 0, POPUP_W, POPUP_H,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd) return false;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
    int backdrop = 3;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    SetLayeredWindowAttributes(m_hwnd, 0, 252, LWA_ALPHA);
    
    for (int i = 0; i < NUM_TYPES; i++) {
        updatePanelCache(i);
    }
    
    return true;
}

// ─── show / hide ─────────────────────────────────────────────────────────────
void PopupWindow::show(POINT anchor) {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = anchor.x - POPUP_W/2 - 80;
    int y = anchor.y - POPUP_H - 14 - 30;
    x = std::max(x, (int)wa.left + 8);
    x = std::min(x, (int)wa.right  - POPUP_W - 8);
    y = std::max(y, (int)wa.top  + 8);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, POPUP_W, POPUP_H, SWP_SHOWWINDOW);
    SetForegroundWindow(m_hwnd);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}
void PopupWindow::hide()            { ShowWindow(m_hwnd, SW_HIDE); }
bool PopupWindow::isVisible() const { return m_hwnd && IsWindowVisible(m_hwnd); }

void PopupWindow::setConnected(bool c, const std::wstring& name) {
    m_connected  = c;
    m_deviceName = name.empty() ? L"MX Master 4" : name;
    invalidate();
}
void PopupWindow::setBattery(int pct, bool charging) {
    m_batteryPct  = pct;
    m_isCharging  = charging;
    invalidate();
}

// ─── WndProc ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK PopupWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PopupWindow* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<PopupWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PopupWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:       onPaint(); return 0;
    case WM_MOUSEMOVE:   onMouseMove((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONDOWN: onLButtonDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONUP:   onLButtonUp((short)LOWORD(lp), (short)HIWORD(lp));   return 0;
    case WM_TIMER:       if (wp == ANIM_TIMER_ID) onTimer(); return 0;
    case WM_ERASEBKGND:  return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool hand = (m_hoveredHotspot != -1 || m_hoveredHoverPill || m_hoveredWf != -1 || 
                         m_hoveredMode != -1 || m_hoveredFocusBtn || m_hoveredToggle || 
                         m_hovDelaySlider || m_hoveredMcWf != -1 || m_hovDelayMcSlider || m_hoveredMcToggle);
            if (hand) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !m_draggingDelay)
            hide();
        return 0;
    case WM_NCHITTEST: {
        POINT pt = {LOWORD(lp), HIWORD(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < HEADER_H) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSELEAVE:
        m_hoveredHotspot = -1; m_hoveredHoverPill = false;
        m_hoveredWf = -1; m_hoveredMode = -1;
        m_hoveredFocusBtn = false; m_hoveredToggle = false;
        m_hovDelaySlider = false;
        m_hoveredMcWf = -1; m_hovDelayMcSlider = false; m_hoveredMcToggle = false;
        invalidate();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── Animation ───────────────────────────────────────────────────────────────
void PopupWindow::startAnim(float target) {
    m_animTarget = target;
    if (m_displayType >= 0 && m_displayType < NUM_TYPES) {
        updatePanelCache(m_displayType);
    }
    if (m_animTimer == 0) {
        m_animLastTick = GetTickCount64();
        m_animTimer = SetTimer(m_hwnd, ANIM_TIMER_ID, 16, nullptr);
    }
}

void PopupWindow::onTimer() {
    ULONGLONG now = GetTickCount64();
    float dt = float(now - m_animLastTick) / 1000.f;
    m_animLastTick = now;

    float speed = 5.0f; // roughly 200ms
    float diff = m_animTarget - m_animT;
    float step = dt * speed;

    if (fabsf(diff) <= step || step <= 0.001f) {
        m_animT = m_animTarget;
        KillTimer(m_hwnd, ANIM_TIMER_ID);
        m_animTimer = 0;
    } else {
        m_animT += (diff > 0.f ? step : -step);
    }
    invalidate();
}

float PopupWindow::easedT() const { return easeInOutCubic(m_animT); }

void PopupWindow::selectFeature(int idx) {
    if (m_activeType == idx) {
        // Toggle off
        m_activeType = -1;
        startAnim(0.f);
    } else {
        bool wasOpen = (m_activeType >= 0);
        m_activeType = idx;
        if (idx >= 0) m_displayType = idx;
        if (!wasOpen) startAnim(1.f);
        else          invalidate();  // switch features: no slide animation needed
    }
}

// ─── Geometry ────────────────────────────────────────────────────────────────
RectF PopupWindow::mouseImageRect() const {
    float t = easedT();
    // Center shifts right as panel opens
    float cx = lerpf(float(POPUP_W)/2.f, float(POPUP_W)/2.f + float(PANEL_W)*0.52f, t);
    float cy = float(HEADER_H) + (float(POPUP_H - HEADER_H))/2.f - 24.5f;

    constexpr float MAX_W = 455.f, MAX_H = 526.f;
    if (!m_mouseImg) return RectF(cx-MAX_W/2, cy-MAX_H/2, MAX_W, MAX_H);

    float iw = float(m_mouseImg->GetWidth());
    float ih = float(m_mouseImg->GetHeight());
    float sc = std::min(MAX_W/iw, MAX_H/ih);
    float dw = iw * sc, dh = ih * sc;
    return RectF(cx - dw/2.f, cy - dh/2.f, dw, dh);
}

RectF PopupWindow::hoverPillRect() const {
    float t  = easedT();
    float cx = lerpf(float(POPUP_W) / 2.f, float(POPUP_W) / 2.f + float(PANEL_W) * 0.52f, t);
    return RectF(cx - 34.f + 16.f, float(POPUP_H) - 68.f, 68.f, 34.f);
}

float PopupWindow::panelX() const {
    return lerpf(float(-PANEL_W), 0.f, easedT());
}

RectF PopupWindow::wfCellRect(int si, float px) const {
    int col = si % WF_COLS, row = si / WF_COLS;
    return RectF(px + PP + col*(WF_CELL_W + WF_CELL_GAP),
                 WF_GRID_Y + row*(WF_CELL_H + WF_CELL_GAP),
                 WF_CELL_W, WF_CELL_H);
}

RectF PopupWindow::delayTrackRect(float px) const {
    float lw = 54.f, vw = 54.f;
    float tx = px + PP + lw;
    float tw = float(PANEL_W) - 2*PP - lw - vw;
    float ty = DLY_Y + DLY_H/2.f - 2.f;
    return RectF(tx, ty, tw, 3.f);
}

RectF PopupWindow::delayKnobRect(float px) const {
    auto tr = delayTrackRect(px);
    int  dt = m_displayType >= 0 && m_displayType < NUM_TYPES ? m_displayType : 0;
    float p = clamp01(float(m_delayMs[dt]) / float(DELAY_MAX_MS));
    constexpr float KR = 9.f;
    return RectF(tr.X + p*tr.Width - KR, tr.Y + tr.Height/2.f - KR, KR*2, KR*2);
}

RectF PopupWindow::modeBtnRect(int mi, float px) const {
    constexpr float GAP = 8.f, BH = 28.f;
    float avail = float(PANEL_W) - 2*PP - 2*GAP;
    float BW    = avail / 3.f;
    return RectF(px + PP + mi*(BW+GAP), HOVER_CTL_Y + 4.f, BW, BH);
}

RectF PopupWindow::focusBtnRect(float px) const {
    return RectF(px + PP, HOVER_CTL_Y + 50.f, float(PANEL_W) - 2*PP, 28.f);
}


RectF PopupWindow::enableToggleRect(float px) const {
    constexpr float TW = 40.f, TH = 22.f;
    return RectF(px + float(PANEL_W) - PP - TW,
                 FEAT_Y + (FEAT_H - TH)/2.f, TW, TH);
}

// ─── Hit testing ─────────────────────────────────────────────────────────────
int PopupWindow::hotspotAt(int x, int y) const {
    auto ir = mouseImageRect();
    for (int i = 0; i < 5; i++) {
        const auto& h = HOTSPOTS[i];
        float cx = ir.X + (h.px / IMG_REF_W) * ir.Width;
        float cy = ir.Y + (h.py / IMG_REF_H) * ir.Height;
        float hr = HS_RADIUS + 4.f;  // slightly larger hit area
        float dx = float(x) - cx, dy = float(y) - cy;
        if (dx*dx + dy*dy <= hr*hr) return i;
    }
    return -1;
}

bool PopupWindow::hoverPillAt(int x, int y) const {
    auto r = hoverPillRect();
    return x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height;
}

int PopupWindow::wfAt(int x, int y, float px) const {
    for (int i = 0; i < hidpp::SHOWN_WF_COUNT; i++) {
        auto r = wfCellRect(i, px);
        if (x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height) return i;
    }
    return -1;
}

bool PopupWindow::delaySliderAt(int x, int y, float px) const {
    auto tr = delayTrackRect(px);
    float ex = 12.f;
    return x>=tr.X-ex && x<=tr.X+tr.Width+ex && y>=tr.Y-ex && y<=tr.Y+tr.Height+ex;
}

int PopupWindow::modeAt(int x, int y, float px) const {
    for (int i = 0; i < 3; i++) {
        auto r = modeBtnRect(i, px);
        if (x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height) return i;
    }
    return -1;
}

bool PopupWindow::focusBtnAt(int x, int y, float px) const {
    auto r = focusBtnRect(px);
    return x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height;
}


bool PopupWindow::enableToggleAt(int x, int y, float px) const {
    auto r = enableToggleRect(px);
    return x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height;
}

void PopupWindow::updateDelayFromX(int x, float px) {
    auto tr = delayTrackRect(px);
    float p = clamp01((float(x) - tr.X) / tr.Width);
    uint32_t ms = uint32_t(p * float(DELAY_MAX_MS) + 0.5f);
    int dt = m_displayType >= 0 && m_displayType < NUM_TYPES ? m_displayType : 0;
    if (ms != m_delayMs[dt]) {
        m_delayMs[dt] = ms;
        if (m_onDelay) m_onDelay(dt, ms);
        invalidate();
    }
}

// ─── Interaction ─────────────────────────────────────────────────────────────
void PopupWindow::onMouseMove(int x, int y) {
    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, m_hwnd, 0};
    TrackMouseEvent(&tme);

    float px = panelX();
    if (m_draggingDelay)   { updateDelayFromX(x, px); return; }
    if (m_draggingMcDelay) {
        float tx = px + PP + 54.f, tw = float(PANEL_W) - 2*PP - 54.f - 44.f;
        uint32_t ms = uint32_t(clamp01((float(x)-tx)/tw) * float(DELAY_MAX_MS) + 0.5f);
        if (ms != m_delayMs[5]) { m_delayMs[5]=ms; if(m_onDelay) m_onDelay(5,ms); invalidate(); }
        return;
    }

    bool panelVisible = (m_animT > 0.02f);
    bool mcVisible    = panelVisible && (m_displayType == 0);

    int  hHS  = hotspotAt(x, y);
    bool hPil = hoverPillAt(x, y);
    int  hWf  = panelVisible ? wfAt(x, y, px)  : -1;
    int  hMod = panelVisible ? modeAt(x, y, px) : -1;
    bool hFoc = panelVisible && focusBtnAt(x, y, px);
    bool hTog = panelVisible && enableToggleAt(x, y, px);
    bool hDly = panelVisible && delaySliderAt(x, y, px);

    // MC section hovers
    int  hMcWf  = -1;
    bool hMcDly = false, hMcTog = false;
    if (mcVisible) {
        constexpr float TW2=40.f, TH2=22.f;
        RectF mcTog(px+float(PANEL_W)-PP-TW2, MC_SECT_Y+(FEAT_H-TH2)/2.f, TW2, TH2);
        hMcTog = (x>=mcTog.X && x<mcTog.X+mcTog.Width && y>=mcTog.Y && y<mcTog.Y+mcTog.Height);
        float tx=px+PP+54.f, tw=float(PANEL_W)-2*PP-54.f-44.f, ty=MC_DLY_Y+DLY_H/2.f-2.f;
        hMcDly = (x>=tx-12.f && x<=tx+tw+12.f && y>=ty-12.f && y<=ty+15.f);
        for (int si=0; si<hidpp::SHOWN_WF_COUNT && hMcWf<0; si++) {
            int col=si%WF_COLS, row=si/WF_COLS;
            RectF r(px+PP+col*(WF_CELL_W+WF_CELL_GAP), MC_WF_GRID_Y+row*(WF_CELL_H+WF_CELL_GAP), WF_CELL_W, WF_CELL_H);
            if (x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height) hMcWf=si;
        }
    }

    if (hHS!=m_hoveredHotspot || hPil!=m_hoveredHoverPill ||
        hWf!=m_hoveredWf || hMod!=m_hoveredMode ||
        hFoc!=m_hoveredFocusBtn || hTog!=m_hoveredToggle || hDly!=m_hovDelaySlider ||
        hMcWf!=m_hoveredMcWf || hMcDly!=m_hovDelayMcSlider || hMcTog!=m_hoveredMcToggle) {
        m_hoveredHotspot=hHS; m_hoveredHoverPill=hPil;
        m_hoveredWf=hWf; m_hoveredMode=hMod;
        m_hoveredFocusBtn=hFoc; m_hoveredToggle=hTog; m_hovDelaySlider=hDly;
        m_hoveredMcWf=hMcWf; m_hovDelayMcSlider=hMcDly; m_hoveredMcToggle=hMcTog;
        invalidate();
    }
}

void PopupWindow::onLButtonDown(int x, int y) {
    float px = panelX();
    bool panelVisible = (m_animT > 0.1f);

    // MC section delay drag (check before scroll delay — MC section is lower)
    if (panelVisible && m_displayType == 0) {
        float tx=px+PP+54.f, tw=float(PANEL_W)-2*PP-54.f-44.f, ty=MC_DLY_Y+DLY_H/2.f-2.f;
        if (x>=tx-12.f && x<=tx+tw+12.f && y>=ty-12.f && y<=ty+15.f) {
            m_draggingMcDelay=true; SetCapture(m_hwnd);
            uint32_t ms=uint32_t(clamp01((float(x)-tx)/tw)*float(DELAY_MAX_MS)+0.5f);
            if(ms!=m_delayMs[5]){m_delayMs[5]=ms;if(m_onDelay)m_onDelay(5,ms);} invalidate(); return;
        }
    }

    // Delay slider drag
    if (panelVisible && delaySliderAt(x, y, px)) {
        m_draggingDelay = true; SetCapture(m_hwnd);
        updateDelayFromX(x, px); return;
    }

    // MC section toggle
    if (panelVisible && m_displayType == 0) {
        constexpr float TW2=40.f, TH2=22.f;
        RectF mcTog(px+float(PANEL_W)-PP-TW2, MC_SECT_Y+(FEAT_H-TH2)/2.f, TW2, TH2);
        if (x>=mcTog.X && x<mcTog.X+mcTog.Width && y>=mcTog.Y && y<mcTog.Y+mcTog.Height) {
            m_enabled[5]=!m_enabled[5];
            if(m_onEnabled[5]) m_onEnabled[5](m_enabled[5]);
            invalidate(); return;
        }
    }

    // MC section waveform cells
    if (panelVisible && m_displayType == 0) {
        for (int si=0; si<hidpp::SHOWN_WF_COUNT; si++) {
            int col=si%WF_COLS, row=si/WF_COLS;
            RectF r(px+PP+col*(WF_CELL_W+WF_CELL_GAP), MC_WF_GRID_Y+row*(WF_CELL_H+WF_CELL_GAP), WF_CELL_W, WF_CELL_H);
            if (x>=r.X && x<r.X+r.Width && y>=r.Y && y<r.Y+r.Height) {
                int wfId=hidpp::SHOWN_WAVEFORMS[si]; m_wf[5]=wfId;
                if(m_onWaveform[5]) m_onWaveform[5](hidpp::Waveform(wfId));
                if(m_onPreview)     m_onPreview(hidpp::Waveform(wfId));
                invalidate(); return;
            }
        }
    }

    // Enable toggle
    if (panelVisible && enableToggleAt(x, y, px)) {
        int dt = m_displayType >= 0 && m_displayType < NUM_TYPES ? m_displayType : 0;
        m_enabled[dt] = !m_enabled[dt];
        if (m_onEnabled[dt]) m_onEnabled[dt](m_enabled[dt]);
        invalidate(); return;
    }

    // Focus button (hover-only)
    if (panelVisible && focusBtnAt(x, y, px)) {
        m_hoverOnlyFocused = !m_hoverOnlyFocused;
        if (m_onHoverOnlyFocused) m_onHoverOnlyFocused(m_hoverOnlyFocused);
        invalidate(); return;
    }

    // Hover mode buttons
    if (panelVisible) {
        int mi = modeAt(x, y, px);
        if (mi >= 0 && mi < 3) {
            m_hoverMode = haptics::HoverMode(mi);
            if (m_onHoverMode) m_onHoverMode(m_hoverMode);
            invalidate(); return;
        }
    }

    // Waveform cells
    if (panelVisible) {
        int si = wfAt(x, y, px);
        if (si >= 0) {
            int dt = m_displayType >= 0 && m_displayType < NUM_TYPES ? m_displayType : 0;
            int wfId = hidpp::SHOWN_WAVEFORMS[si];
            m_wf[dt] = wfId;
            if (m_onWaveform[dt]) m_onWaveform[dt](hidpp::Waveform(wfId));
            if (m_onPreview)      m_onPreview(hidpp::Waveform(wfId));
            invalidate(); return;
        }
    }

    // Hover pill → select type 6
    if (hoverPillAt(x, y)) { selectFeature(6); return; }

    // Physical hotspots → select type 0..5
    int hs = hotspotAt(x, y);
    if (hs >= 0) { selectFeature(hs); return; }

    // Click on empty space while panel is open → deselect
    if (m_activeType >= 0 && m_animTarget > 0.f) {
        selectFeature(m_activeType);  // toggle off
    }
}

void PopupWindow::onLButtonUp(int x, int y) {
    (void)x; (void)y;
    if (m_draggingDelay)   { m_draggingDelay=false;   ReleaseCapture(); }
    if (m_draggingMcDelay) { m_draggingMcDelay=false; ReleaseCapture(); }
}

// ─── Drawing primitives ───────────────────────────────────────────────────────
void PopupWindow::drawRoundRect(Graphics& g, RectF r, float rad, const Brush& fill) {
    auto* p = makeRoundRectPath(r, rad);
    g.FillPath(&fill, p);
    delete p;
}

void PopupWindow::drawRoundRectStroke(Graphics& g, RectF r, float rad, const Pen& pen) {
    auto* p = makeRoundRectPath(r, rad);
    g.DrawPath(&pen, p);
    delete p;
}

static void fillRoundRectColor(Graphics& g, RectF r, float rad, Color col) {
    SolidBrush b(col); auto* p = makeRoundRectPath(r, rad);
    g.FillPath(&b, p); delete p;
}

void PopupWindow::drawPremiumToggle(Graphics& g, RectF tog, bool on, bool hov, COLORREF accent) {
    float r = tog.Height / 2.f;
    if (on) {
        LinearGradientBrush tr(PointF(tog.X, tog.Y), PointF(tog.X, tog.Y+tog.Height),
                               toGdi(accent,255), toGdi(accent,210));
        GraphicsPath* p = makeRoundRectPath(tog, r); g.FillPath(&tr, p); delete p;
        LinearGradientBrush sh(PointF(tog.X, tog.Y), PointF(tog.X, tog.Y+tog.Height*0.5f),
                               Color(30,255,255,255), Color(0,255,255,255));
        GraphicsPath* sp = makeRoundRectPath(tog, r); g.FillPath(&sh, sp); delete sp;
        if (hov) {
            Pen gp(toGdi(accent, 55), 1.5f);
            auto* gp2 = makeRoundRectPath(RectF(tog.X-2,tog.Y-2,tog.Width+4,tog.Height+4), r+2.f);
            g.DrawPath(&gp, gp2); delete gp2;
        }
    } else {
        fillRoundRectColor(g, tog, r, Color(255,38,36,40));
        GraphicsPath* cl = makeRoundRectPath(tog, r);
        Region cr(cl); g.SetClip(&cr); delete cl;
        LinearGradientBrush sh(PointF(tog.X,tog.Y), PointF(tog.X,tog.Y+8.f), Color(55,0,0,0), Color(0,0,0,0));
        g.FillRectangle(&sh, tog.X, tog.Y, tog.Width, 8.f);
        g.ResetClip();
        Pen border(Color(22,255,255,255), 1.f);
        auto* bp = makeRoundRectPath(tog, r); g.DrawPath(&border, bp); delete bp;
    }
    float kd = tog.Height - 4.f;
    float kx = on ? tog.X+tog.Width-kd-2.f : tog.X+2.f;
    RectF kr(kx, tog.Y+2.f, kd, kd);
    for (int i = 3; i >= 1; i--) {
        float o = float(i)*0.8f;
        SolidBrush sh(Color(BYTE(25+i*10),0,0,0));
        g.FillEllipse(&sh, RectF(kr.X+o,kr.Y+o,kr.Width,kr.Height));
    }
    SolidBrush kf(Color(255,248,247,252)); g.FillEllipse(&kf, kr);
    LinearGradientBrush ks(PointF(kr.X,kr.Y), PointF(kr.X,kr.Y+kr.Height*0.5f),
                           Color(60,255,255,255), Color(0,255,255,255));
    g.FillEllipse(&ks, kr);
}

// ─── Waveform shape visualization ────────────────────────────────────────────
void PopupWindow::drawWaveformViz(Graphics& g, int wfId, RectF area, Color col) {
    float x0 = area.X + area.Width*0.08f;
    float x1 = area.X + area.Width*0.92f;
    float ym = area.Y + area.Height*0.5f;
    float ya = ym - area.Height*0.32f;
    float yb = ym + area.Height*0.32f;
    float xm = (x0+x1)*0.5f;

    Pen pen(col, 1.6f);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);

    GraphicsPath path;
    switch (wfId) {
    case 0x00: { // S. Change — hard step
        path.AddLine(x0, yb, xm-2.f, yb);
        path.StartFigure(); path.AddLine(xm-2.f, yb, xm-2.f, ya);
        path.StartFigure(); path.AddLine(xm-2.f, ya, x1, ya);
        break; }
    case 0x01: // D. Change — smooth S-curve
        path.AddBezier(x0, yb, x0+(x1-x0)*0.35f, yb, xm-(x1-x0)*0.1f, ya, x1, ya);
        break;
    case 0x02: { // Sharp Hit — sharp triangle spike
        PointF pts[] = { PointF(x0,ym), PointF(xm,ya), PointF(x1,ym) };
        path.AddLines(pts, 3);
        break; }
    case 0x03: // Damp Hit — decaying bumps
        path.AddBezier(x0, ym, x0+(xm-x0)*0.4f, ya, xm-(xm-x0)*0.1f, ym, xm+4.f, ym);
        path.StartFigure();
        path.AddBezier(xm+4.f, ym, xm+(x1-xm)*0.3f, ya+(yb-ya)*0.55f,
                       x1-(x1-xm)*0.2f, ym, x1, ym);
        break;
    case 0x04: // Subtle — tiny gentle arch
        path.AddBezier(x0, ym, xm-(xm-x0)*0.4f, ym-(yb-ya)*0.3f,
                       xm+(x1-xm)*0.4f, ym-(yb-ya)*0.3f, x1, ym);
        break;
    case 0x05: // Happy — smooth full arch
        path.AddBezier(x0, ym, x0+(xm-x0)*0.5f, ya-(ya-ym)*0.1f,
                       x1-(x1-xm)*0.5f, ya-(ya-ym)*0.1f, x1, ym);
        break;
    case 0x06: { // Angry — sharp zigzag
        float s = (x1-x0)/4.f;
        PointF pts[] = { PointF(x0,ym), PointF(x0+s,ya),
                         PointF(x0+2*s,yb), PointF(x0+3*s,ya), PointF(x1,ym) };
        path.AddLines(pts, 5);
        break; }
    case 0x07: // Done — rising smooth curve
        path.AddBezier(x0, yb, x0+(x1-x0)*0.2f, yb,
                       x0+(x1-x0)*0.6f, ya, x1, ya-(ya-ym)*0.2f);
        break;
    case 0x08: { // Square — square wave
        float s = (x1-x0)/4.f;
        PointF pts[] = { PointF(x0,ym), PointF(x0,ya), PointF(x0+2*s,ya),
                         PointF(x0+2*s,yb), PointF(x1,yb), PointF(x1,ym) };
        path.AddLines(pts, 6);
        break; }
    case 0x09: // Wave — smooth sine
        path.AddBezier(x0, ym, x0+(xm-x0)*0.5f, ya, xm-(xm-x0)*0.5f, ya, xm, ym);
        path.AddBezier(xm, ym, xm+(x1-xm)*0.5f, yb, x1-(x1-xm)*0.5f, yb, x1, ym);
        break;
    case 0x0B: { // Mad — rapid tight zigzag
        float s = (x1-x0)/6.f;
        PointF pts[8];
        pts[0] = PointF(x0, ym);
        for (int i = 0; i < 3; i++) {
            pts[1+i*2] = PointF(x0+(2*i+1)*s, ya);
            pts[2+i*2] = PointF(x0+(2*i+2)*s, yb);
        }
        pts[7] = PointF(x1, ym);
        path.AddLines(pts, 8);
        break; }
    case 0x1B: // Whisper — barely visible ripple
        path.AddBezier(x0, ym, x0+(xm-x0)*0.5f, ym-(yb-ya)*0.15f,
                       xm-(xm-x0)*0.5f, ym-(yb-ya)*0.15f, xm, ym);
        path.AddBezier(xm, ym, xm+(x1-xm)*0.5f, ym+(yb-ya)*0.15f,
                       x1-(x1-xm)*0.5f, ym+(yb-ya)*0.15f, x1, ym);
        break;
    default:
        path.AddLine(x0, ym, x1, ym);
        break;
    }
    g.DrawPath(&pen, &path);
}

// ─── onPaint ─────────────────────────────────────────────────────────────────
void PopupWindow::onPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);
    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, POPUP_W, POPUP_H);
    SelectObject(mdc, bmp);
    {
        Graphics g(mdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // Update background cache if animation state changed or missing
        if (!m_bgCache || fabsf(m_lastBgAnimT - m_animT) > 0.001f) {
            if (!m_bgCache) m_bgCache = new Gdiplus::Bitmap(POPUP_W, POPUP_H, PixelFormat32bppARGB);
            Graphics bgG(m_bgCache);
            bgG.SetSmoothingMode(SmoothingModeAntiAlias);
            bgG.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            bgG.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            
            drawBackground(bgG);
            
            // Draw static mouse section (grid + mouse image)
            auto ir = mouseImageRect();
            float startX = std::max(0.f, panelX() + float(PANEL_W));
            Region oldClip; bgG.GetClip(&oldClip);
            bgG.SetClip(RectF(startX, float(HEADER_H), float(POPUP_W) - startX, float(POPUP_H - HEADER_H)));

            float cx = ir.X + ir.Width / 2.f;
            float cy = ir.Y + ir.Height / 2.f;
            GraphicsPath gp;
            float r = 420.f;
            gp.AddEllipse(cx - r, cy - r, r * 2.f, r * 2.f);
            PathGradientBrush pgb(&gp);
            pgb.SetCenterColor(Color(26, 255, 255, 255));
            Color edgeC(0, 255, 255, 255);
            int cnt = 1;
            pgb.SetSurroundColors(&edgeC, &cnt);
            Pen gridPen(&pgb, 1.0f);
            float step = 32.f;
            float shiftX = cx - float(POPUP_W) / 2.f;
            float offsetX = fmodf(shiftX, step);
            if (offsetX < 0.f) offsetX += step;

            for (float x = offsetX - step; x < float(POPUP_W) + step; x += step) {
                bgG.DrawLine(&gridPen, x, float(HEADER_H), x, float(POPUP_H));
            }
            for (float y = float(HEADER_H); y < float(POPUP_H); y += step) {
                bgG.DrawLine(&gridPen, 0.f, y, float(POPUP_W), y);
            }
            bgG.SetClip(&oldClip);

            if (m_mouseImg) {
                ImageAttributes ia;
                bgG.DrawImage(m_mouseImg,
                            Rect(int(ir.X), int(ir.Y), int(ir.Width), int(ir.Height)),
                            0, 0, int(m_mouseImg->GetWidth()), int(m_mouseImg->GetHeight()),
                            UnitPixel, &ia);
            }
            
            m_lastBgAnimT = m_animT;
        }

        // Draw cached background (O(1) rendering)
        g.DrawImage(m_bgCache, 0, 0);

        // Draw interactive elements only
        drawMouseSection(g);
        drawPanel(g);
        drawHeader(g);   // header drawn last so it layers on top
    }
    BitBlt(hdc, 0, 0, POPUP_W, POPUP_H, mdc, 0, 0, SRCCOPY);
    DeleteObject(bmp); DeleteDC(mdc);
    EndPaint(m_hwnd, &ps);
}

// ─── drawBackground ──────────────────────────────────────────────────────────
void PopupWindow::drawBackground(Graphics& g) {
    Color top(255, 14, 13, 18), bot(255, 9, 8, 12);
    LinearGradientBrush bg(RectF(0,0,float(POPUP_W),float(POPUP_H)), top, bot, 90.f);
    g.FillRectangle(&bg, 0, 0, POPUP_W, POPUP_H);

    // Soft top-center glow for depth
    GraphicsPath gp; gp.AddEllipse(float(POPUP_W/2-180), -90.f, 360.f, 220.f);
    PathGradientBrush pgb(&gp);
    pgb.SetCenterColor(Color(20, 130, 100, 255));
    Color edgeC(0,0,0,0); int cnt=1;
    pgb.SetSurroundColors(&edgeC, &cnt);
    g.FillPath(&pgb, &gp);
}

// ─── drawHeader ──────────────────────────────────────────────────────────────
void PopupWindow::drawHeader(Graphics& g) {
    // Subtle header background
    LinearGradientBrush hbg(RectF(0,0,float(POPUP_W),float(HEADER_H+9)),
                             Color(28,255,255,255), Color(0,255,255,255), 90.f);
    g.FillRectangle(&hbg, 0, 0, POPUP_W, HEADER_H+9);

    StringFormat sfL; sfL.SetAlignment(StringAlignmentNear); sfL.SetLineAlignment(StringAlignmentCenter);

    // Brand micro-label
    { Font f(L"Segoe UI", 7.f, FontStyleBold);
      SolidBrush b(Color(50,245,245,247));
      g.DrawString(L"L I V E  H A P T I C S", -1, &f, RectF(22.f,10.f,220.f,12.f), &sfL, &b); }

    // Device name
    { std::wstring dn = m_deviceName.empty() ? L"MX Master 4" : m_deviceName;
      Font f(L"Segoe UI", 13.f, FontStyleBold);
      SolidBrush b(Color(255,245,245,247));
      g.DrawString(dn.c_str(),-1,&f,RectF(20.f,20.f,260.f,24.f),&sfL,&b); }

    // Status
    { Font sf(L"Segoe UI", 8.f, FontStyleRegular);
      if (!m_connected) {
          SolidBrush sb(toGdi(Colors::TEXT_DIM,90));
          g.DrawString(L"Searching for device...",-1,&sf,RectF(22.f,44.f,200.f,14.f),&sfL,&sb);
      } else if (!m_hapticsSupported) {
          SolidBrush sb(toGdi(Colors::STATUS_WARN,200));
          g.DrawString(L"Haptics not detected",-1,&sf,RectF(22.f,44.f,200.f,14.f),&sfL,&sb);
      } else {
          SolidBrush dot(toGdi(Colors::STATUS_OK,220));
          g.FillEllipse(&dot,RectF(22.f,45.5f,6.f,6.f));
          SolidBrush glow(toGdi(Colors::STATUS_OK,30));
          g.FillEllipse(&glow,RectF(20.f,45.f,10.f,10.f));
          SolidBrush sb(toGdi(Colors::STATUS_OK,185));
          g.DrawString(L"Connected",-1,&sf,RectF(34.f,43.f,100.f,14.f),&sfL,&sb);
      }
    }

    // Battery widget — right-aligned, vertically centered in header
    if (m_connected && m_batteryPct >= 0) {
        // Color: amber when charging, green/yellow/red based on level otherwise
        COLORREF bc = m_isCharging        ? Colors::STATUS_CHARGE :
                      m_batteryPct > 20   ? Colors::STATUS_OK     :
                      m_batteryPct > 10   ? Colors::STATUS_WARN   : Colors::STATUS_ERR;

        constexpr float BW=27.6f, BH=13.8f, NW=3.45f, NH=6.9f;
        float bx = float(POPUP_W) - 22.f - NW - BW - 43.f;
        float by = (float(HEADER_H) - BH) / 2.f + 4.f;

        // Battery shell
        Pen bp(toGdi(bc,170),1.f);
        drawRoundRectStroke(g,RectF(bx,by,BW,BH),3.45f,bp);
        SolidBrush nub(toGdi(bc,150));
        drawRoundRect(g,RectF(bx+BW,by+(BH-NH)/2.f,NW,NH),1.75f,nub);

        // Fill bar
        float fw = std::max(2.3f,(float(m_batteryPct)/100.f)*(BW-4.6f));
        LinearGradientBrush fill(PointF(bx+2.3f,by+2.3f),PointF(bx+2.3f,by+BH-2.3f),
                                 toGdi(bc,235),toGdi(bc,175));
        drawRoundRect(g,RectF(bx+2.3f,by+2.3f,fw,BH-4.6f),2.3f,fill);

        // Label: "⚡ 75%" when charging, "75%" otherwise
        wchar_t bs[16];
        if (m_isCharging)
            swprintf_s(bs, L"\u26a1 %d%%", m_batteryPct);
        else
            swprintf_s(bs, L"%d%%", m_batteryPct);

        Font bf(L"Segoe UI",10.35f,FontStyleBold);
        SolidBrush bt(toGdi(bc,220));
        StringFormat sfR2;
        sfR2.SetAlignment(StringAlignmentNear);
        sfR2.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(bs,-1,&bf,RectF(bx+BW+NW+4.f,by,52.f,BH),&sfR2,&bt);
    }

    // Separator
    { LinearGradientBrush sep(PointF(22.f,float(HEADER_H+8)),PointF(float(POPUP_W-22),float(HEADER_H+8)),
                               Color(0,255,255,255),Color(0,255,255,255));
      Color cols[]={Color(0,255,255,255),Color(35,255,255,255),Color(35,255,255,255),Color(0,255,255,255)};
      REAL pos[]={0.f,0.28f,0.72f,1.f}; sep.SetInterpolationColors(cols,pos,4);
      g.FillRectangle(&sep,22.f,float(HEADER_H+8),float(POPUP_W-44),1.f); }
}

// ─── drawMouseSection ────────────────────────────────────────────────────────
void PopupWindow::drawMouseSection(Graphics& g) {
    auto ir = mouseImageRect();
    drawHotspots(g, ir);
    drawHoverPill(g);
}

// ─── drawHotspots ────────────────────────────────────────────────────────────
void PopupWindow::drawHotspots(Graphics& g, RectF ir) {
    for (int i = 0; i < 5; i++) {
        const auto& h = HOTSPOTS[i];
        COLORREF ac = TYPE_COLORS[i];

        float cx = ir.X + (h.px / IMG_REF_W) * ir.Width;
        float cy = ir.Y + (h.py / IMG_REF_H) * ir.Height;

        bool sel = (m_activeType == i);
        bool hov = (m_hoveredHotspot == i && !sel);

        float r = HS_RADIUS * (hov ? 1.15f : 1.f);
        RectF er(cx-r, cy-r, r*2, r*2);

        // Real radial glow
        float glowExt = sel ? 46.f : (hov ? 40.f : 40.f);
        GraphicsPath gpGlow;
        gpGlow.AddEllipse(er.X - glowExt, er.Y - glowExt, er.Width + glowExt*2, er.Height + glowExt*2);
        PathGradientBrush pgb(&gpGlow);
        if (sel) {
            pgb.SetCenterColor(toGdi(ac, 230));
            Color edgeC = toGdi(ac, 0);
            int cnt = 1;
            pgb.SetSurroundColors(&edgeC, &cnt);
        } else {
            pgb.SetCenterColor(toGdi(ac, hov ? 190 : 180));
            Color edgeC = toGdi(ac, 0);
            int cnt = 1;
            pgb.SetSurroundColors(&edgeC, &cnt);
        }
        g.FillPath(&pgb, &gpGlow);

        // Transparent background
        int opacity = sel ? 170 : (hov ? 60 : 35);
        if (i == 0 || i == 1) { // Scroll and Side Scroll
            opacity = sel ? 200 : (hov ? 100 : 75);
        }
        SolidBrush bg(toGdi(ac, (BYTE)opacity));
        auto* pBg = makeRoundRectPath(er, r);
        g.FillPath(&bg, pBg);
        delete pBg;
    }
}

// ─── drawHoverPill ───────────────────────────────────────────────────────────
void PopupWindow::drawHoverPill(Graphics& g) {
    COLORREF ac  = TYPE_COLORS[6];  // green
    bool     sel = (m_activeType == 6);
    bool     hov = (m_hoveredHoverPill && !sel);
    RectF    r   = hoverPillRect();
    float    rad = r.Height / 2.f;

    // Real radial glow
    float glowExt = sel ? 18.f : (hov ? 16.f : 12.f);
    RectF gr(r.X - glowExt, r.Y - glowExt, r.Width + glowExt*2, r.Height + glowExt*2);
    GraphicsPath* pGlow = makeRoundRectPath(gr, rad + glowExt);
    PathGradientBrush pgb(pGlow);
    if (sel) {
        pgb.SetCenterColor(toGdi(ac, 140));
        Color edgeC = toGdi(ac, 0);
        int cnt = 1;
        pgb.SetSurroundColors(&edgeC, &cnt);
    } else {
        pgb.SetCenterColor(toGdi(ac, hov ? 90 : 50));
        Color edgeC = toGdi(ac, 0);
        int cnt = 1;
        pgb.SetSurroundColors(&edgeC, &cnt);
    }
    g.FillPath(&pgb, pGlow);
    delete pGlow;

    // Transparent background
    SolidBrush bg(sel ? toGdi(ac, 220) : toGdi(ac, hov ? 80 : 45));
    drawRoundRect(g, r, rad, bg);

    // Contour
    Pen bp(sel ? toGdi(ac, 255) : toGdi(ac, hov ? 180 : 130), 1.5f);
    drawRoundRectStroke(g, r, rad, bp);

    // ── Label ─────────────────────────────────────────────────────────────
    Font lf(L"Segoe UI", 9.5f, FontStyleBold);
    SolidBrush lb(sel ? Color(255, 255, 255, 255) : Color(255, BYTE(std::max(0, (int)GetRValue(ac) - 15)), BYTE(std::max(0, (int)GetGValue(ac) - 15)), BYTE(std::max(0, (int)GetBValue(ac) - 15))));
    StringFormat sfL3; sfL3.SetAlignment(StringAlignmentCenter); sfL3.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"HOVER", -1, &lf, RectF(r.X, r.Y+1.f, r.Width, r.Height), &sfL3, &lb);
}

// ─── drawPanel ───────────────────────────────────────────────────────────────
void PopupWindow::drawPanel(Graphics& g) {
    if (m_animT < 0.01f) return;

    float px    = panelX();
    float alpha = clamp01(m_animT * 2.f);  // fade in faster than slide
    int   dt    = (m_displayType >= 0 && m_displayType < NUM_TYPES) ? m_displayType : 0;
    COLORREF ac = TYPE_COLORS[dt];

    if (m_animTimer != 0 && m_animT < 1.0f && m_panelCache[dt]) {
        ImageAttributes ia;
        ColorMatrix cm = {
            1.f, 0.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 0.f, alpha, 0.f,
            0.f, 0.f, 0.f, 0.f, 1.f
        };
        ia.SetColorMatrix(&cm);
        g.DrawImage(m_panelCache[dt], RectF(px, 0.f, float(PANEL_W), float(POPUP_H)),
                    0.f, 0.f, float(PANEL_W), float(POPUP_H), UnitPixel, &ia);
        return;
    }

    // ── Panel background ────────────────────────────────────────────────────
    {   RectF panelRect(px, float(HEADER_H) + 8.f, float(PANEL_W), float(POPUP_H - HEADER_H) - 8.f);
        SolidBrush bg(Color(BYTE(245*alpha), 11, 10, 15));
        drawRoundRect(g, panelRect, 0.f, bg);  // flush left, square left edges

        // Subtle gradient sheen
        LinearGradientBrush sheen(PointF(px,float(HEADER_H)+8.f), PointF(px+float(PANEL_W),float(HEADER_H)+8.f),
                                  Color(BYTE(12*alpha),255,255,255), Color(0,255,255,255));
        g.FillRectangle(&sheen, px, float(HEADER_H) + 8.f, float(PANEL_W), float(POPUP_H-HEADER_H) - 8.f);

        // Right-edge separator
        Pen sep(Color(BYTE(30*alpha),255,255,255), 1.f);
        g.DrawLine(&sep, PointF(px+float(PANEL_W)-0.5f, float(HEADER_H) + 8.f),
                         PointF(px+float(PANEL_W)-0.5f, float(POPUP_H)));
    }

    if (alpha < 0.05f) return;
    BYTE ba = BYTE(255 * alpha);

    // ── Feature header ──────────────────────────────────────────────────────
    {   RectF fr(px, FEAT_Y, float(PANEL_W), FEAT_H);

        // Accent strip on left
        SolidBrush ab(toGdi(ac, BYTE(200*alpha)));
        g.FillRectangle(&ab, px, FEAT_Y+8.f, 3.f, FEAT_H-16.f);

        // Feature name
        Font fn(L"Segoe UI", 14.f, FontStyleBold);
        SolidBrush fb(toGdi(ac, ba));
        StringFormat sfL4; sfL4.SetAlignment(StringAlignmentNear); sfL4.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(TYPE_LABELS[dt], -1, &fn,
                     RectF(px+10.f, FEAT_Y, float(PANEL_W)-60.f, FEAT_H), &sfL4, &fb);

        // Enable toggle
        auto tr = enableToggleRect(px);
        bool ten = m_enabled[dt];
        bool tHov = m_hoveredToggle;
        // Apply alpha to toggle: draw at full coords but with blended color
        drawPremiumToggle(g, tr, ten, tHov, ac);
    }

    // ── Separator ───────────────────────────────────────────────────────────
    {   LinearGradientBrush sep(PointF(px+PP, FEAT_Y+FEAT_H+4.f),
                                 PointF(px+float(PANEL_W)-PP, FEAT_Y+FEAT_H+4.f),
                                 Color(0,255,255,255), Color(0,255,255,255));
        Color sc[]={Color(0,255,255,255),Color(BYTE(28*alpha),255,255,255),Color(0,255,255,255)};
        REAL sp[]={0.f,0.5f,1.f}; sep.SetInterpolationColors(sc,sp,3);
        g.FillRectangle(&sep, px+PP, FEAT_Y+FEAT_H+4.f, float(PANEL_W)-2*PP, 1.f); }

    // ── Waveforms label ─────────────────────────────────────────────────────
    {   Font wlf(L"Segoe UI", 7.f, FontStyleBold);
        SolidBrush wlb(Color(BYTE(55*alpha), 245,245,247));
        StringFormat sfL5; sfL5.SetAlignment(StringAlignmentNear);
        g.DrawString(L"W A V E F O R M", -1, &wlf,
                     PointF(px+PP, WF_LABEL_Y), &sfL5, &wlb); }

    drawWaveformGrid(g, px);
    drawDelaySlider(g, px);
    if (dt == 0) drawMidClickSection(g, px);
    if (dt == 6) drawHoverControls(g, px);
}

// ─── drawWaveformGrid ────────────────────────────────────────────────────────
void PopupWindow::drawWaveformGrid(Graphics& g, float px) {
    float alpha = clamp01(m_animT * 2.f);
    int   dt    = (m_displayType >= 0 && m_displayType < NUM_TYPES) ? m_displayType : 0;
    COLORREF ac = TYPE_COLORS[dt];

    int activeSel = -1;
    for (int si = 0; si < hidpp::SHOWN_WF_COUNT; si++)
        if (hidpp::SHOWN_WAVEFORMS[si] == m_wf[dt]) { activeSel = si; break; }

    Font nameFont(L"Segoe UI", 7.f, FontStyleRegular);
    Font nameFontSel(L"Segoe UI", 7.f, FontStyleBold);
    StringFormat sfC2; sfC2.SetAlignment(StringAlignmentCenter); sfC2.SetLineAlignment(StringAlignmentFar);
    sfC2.SetTrimming(StringTrimmingEllipsisCharacter);

    for (int si = 0; si < hidpp::SHOWN_WF_COUNT; si++) {
        RectF r   = wfCellRect(si, px);
        bool  sel = (si == activeSel);
        bool  hov = (si == m_hoveredWf && !sel);
        int   wfId= hidpp::SHOWN_WAVEFORMS[si];
        BYTE  ba  = BYTE(255*alpha);

        if (sel) {
            // Outer glow
            RectF gr(r.X-3,r.Y-3,r.Width+6,r.Height+6);
            SolidBrush gb(toGdi(ac, BYTE(18*alpha)));
            drawRoundRect(g, gr, 12.f, gb);
            // Glass fill
            LinearGradientBrush fill(PointF(r.X,r.Y),PointF(r.X,r.Y+r.Height),
                                     toGdi(ac,BYTE(55*alpha)),toGdi(ac,BYTE(28*alpha)));
            drawRoundRect(g, r, 9.f, fill);
            // Top shimmer
            {   auto* cl = makeRoundRectPath(r, 9.f); Region cr(cl); g.SetClip(&cr); delete cl;
                LinearGradientBrush sh(PointF(r.X,r.Y),PointF(r.X,r.Y+r.Height*0.45f),
                                       Color(BYTE(28*alpha),255,255,255),Color(0,255,255,255));
                g.FillRectangle(&sh,r.X,r.Y,r.Width,r.Height*0.45f); g.ResetClip(); }
            Pen bp(toGdi(ac,BYTE(195*alpha)),1.f); drawRoundRectStroke(g,r,9.f,bp);

            // Viz + name
            RectF vizR(r.X+4,r.Y+4, r.Width-8, r.Height-18);
            drawWaveformViz(g, wfId, vizR, toGdi(ac, ba));
            RectF nameR(r.X, r.Y+r.Height-16.f, r.Width, 14.f);
            SolidBrush tb(toGdi(ac, ba));
            g.DrawString(hidpp::waveformName(wfId),-1,&nameFontSel,nameR,&sfC2,&tb);
        } else if (hov) {
            SolidBrush fill(Color(BYTE(38*alpha),255,255,255));
            drawRoundRect(g,r,9.f,fill);
            Pen bp(Color(BYTE(22*alpha),255,255,255),1.f); drawRoundRectStroke(g,r,9.f,bp);
            RectF vizR(r.X+4,r.Y+4,r.Width-8,r.Height-18);
            drawWaveformViz(g, wfId, vizR, Color(BYTE(180*alpha),210,210,220));
            RectF nameR(r.X, r.Y+r.Height-16.f, r.Width, 14.f);
            SolidBrush tb(Color(BYTE(210*alpha),245,245,247));
            g.DrawString(hidpp::waveformName(wfId),-1,&nameFont,nameR,&sfC2,&tb);
        } else {
            SolidBrush fill(Color(BYTE(14*alpha),255,255,255));
            drawRoundRect(g,r,9.f,fill);
            Pen bp(Color(BYTE(9*alpha),255,255,255),1.f); drawRoundRectStroke(g,r,9.f,bp);
            RectF vizR(r.X+4,r.Y+4,r.Width-8,r.Height-18);
            drawWaveformViz(g, wfId, vizR, Color(BYTE(70*alpha),140,138,155));
            RectF nameR(r.X, r.Y+r.Height-16.f, r.Width, 14.f);
            SolidBrush tb(Color(BYTE(120*alpha),140,138,155));
            g.DrawString(hidpp::waveformName(wfId),-1,&nameFont,nameR,&sfC2,&tb);
        }
    }
}

// ─── drawDelaySlider ─────────────────────────────────────────────────────────
void PopupWindow::drawDelaySlider(Graphics& g, float px) {
    float alpha = clamp01(m_animT * 2.f);
    int   dt    = (m_displayType>=0 && m_displayType<NUM_TYPES) ? m_displayType : 0;
    COLORREF ac = TYPE_COLORS[dt];
    auto   tr   = delayTrackRect(px);
    float  pct  = clamp01(float(m_delayMs[dt]) / float(DELAY_MAX_MS));

    StringFormat sfL6; sfL6.SetAlignment(StringAlignmentNear); sfL6.SetLineAlignment(StringAlignmentCenter);
    StringFormat sfR3; sfR3.SetAlignment(StringAlignmentFar);  sfR3.SetLineAlignment(StringAlignmentCenter);

    // Label
    { Font lf(L"Segoe UI",7.f,FontStyleBold);
      SolidBrush lb(Color(BYTE(100*alpha),245,245,247));
      g.DrawString(L"DELAY",-1,&lf,RectF(px+PP,DLY_Y,52.f,DLY_H),&sfL6,&lb); }

    // Track
    { RectF trE(tr.X,tr.Y-1.f,tr.Width,tr.Height+2.f);
      SolidBrush trbg(Color(BYTE(255*alpha),13,12,16));
      drawRoundRect(g,trE,2.f,trbg);
      auto* cl = makeRoundRectPath(trE,2.f); Region cr2(cl); g.SetClip(&cr2); delete cl;
      LinearGradientBrush ts(PointF(tr.X,tr.Y-1.f),PointF(tr.X,tr.Y+4.f),
                              Color(BYTE(55*alpha),0,0,0),Color(0,0,0,0));
      g.FillRectangle(&ts,tr.X,tr.Y-1.f,tr.Width,5.f); g.ResetClip();
      Pen tbp(Color(BYTE(18*alpha),255,255,255),1.f); drawRoundRectStroke(g,trE,2.f,tbp);
      if (pct > 0.001f) {
          RectF filled(tr.X,tr.Y-1.f,tr.Width*pct,tr.Height+2.f);
          LinearGradientBrush fill(PointF(tr.X,tr.Y),PointF(tr.X+tr.Width*pct,tr.Y),
                                   toGdi(ac,BYTE(90*alpha)),toGdi(ac,BYTE(220*alpha)));
          drawRoundRect(g,filled,2.f,fill);
      }
    }

    // Knob
    { auto kr = delayKnobRect(px);
      float kx = kr.X+kr.Width/2, ky = kr.Y+kr.Height/2, kR = kr.Width/2;
      bool act = m_hovDelaySlider || m_draggingDelay;
      for (int i=4;i>=1;i--) {
          float o=float(i)*0.7f;
          SolidBrush sh(Color(BYTE((18+i*12)*alpha),0,0,0));
          g.FillEllipse(&sh,RectF(kr.X+o,kr.Y+o,kr.Width,kr.Height));
      }
      LinearGradientBrush kf(PointF(kx-kR,ky-kR),PointF(kx-kR,ky+kR),
                              Color(BYTE(255*alpha),250,249,255),Color(BYTE(255*alpha),220,218,230));
      g.FillEllipse(&kf,kr);
      LinearGradientBrush ks(PointF(kx-kR,ky-kR),PointF(kx-kR,ky),Color(BYTE(50*alpha),255,255,255),Color(0,255,255,255));
      g.FillEllipse(&ks,kr);
      if (act) {
          Pen gr(toGdi(ac,BYTE(180*alpha)),1.5f);
          g.DrawEllipse(&gr,RectF(kr.X-1,kr.Y-1,kr.Width+2,kr.Height+2));
      } else {
          Pen ep(Color(BYTE(20*alpha),255,255,255),1.f); g.DrawEllipse(&ep,kr);
      }
    }

    // Value text
    { wchar_t vs[16]; swprintf_s(vs, L"%ums", m_delayMs[dt]);
      Font vf(L"Segoe UI",8.5f,FontStyleBold);
      SolidBrush vb(toGdi(ac,BYTE(225*alpha)));
      g.DrawString(vs,-1,&vf,RectF(tr.X+tr.Width+4.f,DLY_Y,52.f,DLY_H),&sfR3,&vb); }
}

// ─── drawMidClickSection ─────────────────────────────────────────────────────
void PopupWindow::drawMidClickSection(Graphics& g, float px) {
    float alpha = clamp01(m_animT * 2.f);
    COLORREF ac  = TYPE_COLORS[5];
    BYTE     ba  = BYTE(255*alpha);

    // ── Divider separator ───────────────────────────────────────────────────
    { LinearGradientBrush sep(PointF(px+PP, MC_SECT_Y-8.f),
                               PointF(px+float(PANEL_W)-PP, MC_SECT_Y-8.f),
                               Color(0,255,255,255), Color(0,255,255,255));
      Color sc[]={Color(0,255,255,255),Color(BYTE(35*alpha),255,255,255),Color(0,255,255,255)};
      REAL sp[]={0.f,0.5f,1.f}; sep.SetInterpolationColors(sc,sp,3);
      g.FillRectangle(&sep, px+PP, MC_SECT_Y-8.f, float(PANEL_W)-2*PP, 1.f); }

    // ── Feature header ──────────────────────────────────────────────────────
    SolidBrush abr(toGdi(ac, BYTE(200*alpha)));
    g.FillRectangle(&abr, px, MC_SECT_Y+8.f, 3.f, FEAT_H-16.f);

    { Font fn(L"Segoe UI", 14.f, FontStyleBold);
      SolidBrush fb(toGdi(ac, ba));
      StringFormat sfN; sfN.SetAlignment(StringAlignmentNear); sfN.SetLineAlignment(StringAlignmentCenter);
      g.DrawString(L"MID CLICK", -1, &fn, RectF(px+10.f, MC_SECT_Y, float(PANEL_W)-60.f, FEAT_H), &sfN, &fb); }

    // Toggle
    { constexpr float TW=40.f, TH=22.f;
      RectF tr(px+float(PANEL_W)-PP-TW, MC_SECT_Y+(FEAT_H-TH)/2.f, TW, TH);
      drawPremiumToggle(g, tr, m_enabled[5], m_hoveredMcToggle, ac); }

    // ── Inner separator ─────────────────────────────────────────────────────
    { LinearGradientBrush sep2(PointF(px+PP, MC_SECT_Y+FEAT_H+4.f),
                                PointF(px+float(PANEL_W)-PP, MC_SECT_Y+FEAT_H+4.f),
                                Color(0,255,255,255), Color(0,255,255,255));
      Color sc2[]={Color(0,255,255,255),Color(BYTE(28*alpha),255,255,255),Color(0,255,255,255)};
      REAL sp2[]={0.f,0.5f,1.f}; sep2.SetInterpolationColors(sc2,sp2,3);
      g.FillRectangle(&sep2, px+PP, MC_SECT_Y+FEAT_H+4.f, float(PANEL_W)-2*PP, 1.f); }

    // ── Waveform label ──────────────────────────────────────────────────────
    { Font wlf(L"Segoe UI", 7.f, FontStyleBold);
      SolidBrush wlb(Color(BYTE(55*alpha), 245,245,247));
      StringFormat sfWL; sfWL.SetAlignment(StringAlignmentNear);
      g.DrawString(L"W A V E F O R M", -1, &wlf, PointF(px+PP, MC_SECT_Y+FEAT_H+10.f), &sfWL, &wlb); }

    // ── Waveform grid ───────────────────────────────────────────────────────
    int activeSel = -1;
    for (int si=0; si<hidpp::SHOWN_WF_COUNT; si++)
        if (hidpp::SHOWN_WAVEFORMS[si]==m_wf[5]) { activeSel=si; break; }

    Font nameFont(L"Segoe UI", 7.f, FontStyleRegular);
    Font nameFontSel(L"Segoe UI", 7.f, FontStyleBold);
    StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentFar);
    sfC.SetTrimming(StringTrimmingEllipsisCharacter);

    for (int si=0; si<hidpp::SHOWN_WF_COUNT; si++) {
        int col=si%WF_COLS, row=si/WF_COLS;
        RectF r(px+PP+col*(WF_CELL_W+WF_CELL_GAP), MC_WF_GRID_Y+row*(WF_CELL_H+WF_CELL_GAP), WF_CELL_W, WF_CELL_H);
        bool sel=(si==activeSel), hov=(si==m_hoveredMcWf && !sel);
        int wfId=hidpp::SHOWN_WAVEFORMS[si];

        if (sel) {
            RectF gr(r.X-3,r.Y-3,r.Width+6,r.Height+6);
            SolidBrush gb(toGdi(ac,BYTE(18*alpha))); drawRoundRect(g,gr,12.f,gb);
            LinearGradientBrush fill(PointF(r.X,r.Y),PointF(r.X,r.Y+r.Height),
                                     toGdi(ac,BYTE(55*alpha)),toGdi(ac,BYTE(28*alpha)));
            drawRoundRect(g,r,9.f,fill);
            { auto* cl=makeRoundRectPath(r,9.f); Region cr(cl); g.SetClip(&cr); delete cl;
              LinearGradientBrush sh(PointF(r.X,r.Y),PointF(r.X,r.Y+r.Height*0.45f),
                                     Color(BYTE(28*alpha),255,255,255),Color(0,255,255,255));
              g.FillRectangle(&sh,r.X,r.Y,r.Width,r.Height*0.45f); g.ResetClip(); }
            Pen bp(toGdi(ac,BYTE(195*alpha)),1.f); drawRoundRectStroke(g,r,9.f,bp);
            RectF vizR(r.X+4,r.Y+4,r.Width-8,r.Height-18);
            drawWaveformViz(g,wfId,vizR,toGdi(ac,ba));
            RectF nameR(r.X,r.Y+r.Height-16.f,r.Width,14.f);
            SolidBrush tb(toGdi(ac,ba)); g.DrawString(hidpp::waveformName(wfId),-1,&nameFontSel,nameR,&sfC,&tb);
        } else if (hov) {
            SolidBrush fill(Color(BYTE(38*alpha),255,255,255)); drawRoundRect(g,r,9.f,fill);
            Pen bp(Color(BYTE(22*alpha),255,255,255),1.f); drawRoundRectStroke(g,r,9.f,bp);
            RectF vizR(r.X+4,r.Y+4,r.Width-8,r.Height-18);
            drawWaveformViz(g,wfId,vizR,Color(BYTE(180*alpha),210,210,220));
            RectF nameR(r.X,r.Y+r.Height-16.f,r.Width,14.f);
            SolidBrush tb(Color(BYTE(210*alpha),245,245,247)); g.DrawString(hidpp::waveformName(wfId),-1,&nameFont,nameR,&sfC,&tb);
        } else {
            SolidBrush fill(Color(BYTE(14*alpha),255,255,255)); drawRoundRect(g,r,9.f,fill);
            Pen bp(Color(BYTE(9*alpha),255,255,255),1.f); drawRoundRectStroke(g,r,9.f,bp);
            RectF vizR(r.X+4,r.Y+4,r.Width-8,r.Height-18);
            drawWaveformViz(g,wfId,vizR,Color(BYTE(70*alpha),140,138,155));
            RectF nameR(r.X,r.Y+r.Height-16.f,r.Width,14.f);
            SolidBrush tb(Color(BYTE(120*alpha),140,138,155)); g.DrawString(hidpp::waveformName(wfId),-1,&nameFont,nameR,&sfC,&tb);
        }
    }

    // ── Delay slider ────────────────────────────────────────────────────────
    float pct = clamp01(float(m_delayMs[5]) / float(DELAY_MAX_MS));
    float lw=54.f, vw=54.f;
    float tx=px+PP+lw, tw=float(PANEL_W)-2*PP-lw-vw, ty=MC_DLY_Y+DLY_H/2.f-2.f;
    RectF tr2(tx, ty, tw, 3.f);
    float kp=clamp01(float(m_delayMs[5])/float(DELAY_MAX_MS)); constexpr float KR=9.f;
    RectF kr(tr2.X+kp*tr2.Width-KR, tr2.Y+tr2.Height/2.f-KR, KR*2, KR*2);
    bool act = m_hovDelayMcSlider || m_draggingMcDelay;

    StringFormat sfL8; sfL8.SetAlignment(StringAlignmentNear); sfL8.SetLineAlignment(StringAlignmentCenter);
    StringFormat sfR4; sfR4.SetAlignment(StringAlignmentFar);  sfR4.SetLineAlignment(StringAlignmentCenter);

    { Font lf2(L"Segoe UI",7.f,FontStyleBold);
      SolidBrush lb2(Color(BYTE(100*alpha),245,245,247));
      g.DrawString(L"DELAY",-1,&lf2,RectF(px+PP,MC_DLY_Y,52.f,DLY_H),&sfL8,&lb2); }

    { RectF trE(tr2.X,tr2.Y-1.f,tr2.Width,tr2.Height+2.f);
      SolidBrush trbg(Color(BYTE(255*alpha),13,12,16)); drawRoundRect(g,trE,2.f,trbg);
      auto* cl=makeRoundRectPath(trE,2.f); Region cr2(cl); g.SetClip(&cr2); delete cl;
      LinearGradientBrush ts(PointF(tr2.X,tr2.Y-1.f),PointF(tr2.X,tr2.Y+4.f),
                              Color(BYTE(55*alpha),0,0,0),Color(0,0,0,0));
      g.FillRectangle(&ts,tr2.X,tr2.Y-1.f,tr2.Width,5.f); g.ResetClip();
      Pen tbp(Color(BYTE(18*alpha),255,255,255),1.f); drawRoundRectStroke(g,trE,2.f,tbp);
      if (pct>0.001f) {
          RectF filled(tr2.X,tr2.Y-1.f,tr2.Width*pct,tr2.Height+2.f);
          LinearGradientBrush fill(PointF(tr2.X,tr2.Y),PointF(tr2.X+tr2.Width*pct,tr2.Y),
                                   toGdi(ac,BYTE(90*alpha)),toGdi(ac,BYTE(220*alpha)));
          drawRoundRect(g,filled,2.f,fill);
      } }

    { float kx2=kr.X+kr.Width/2, ky2=kr.Y+kr.Height/2, kR2=kr.Width/2;
      for (int i=4;i>=1;i--) { float o=float(i)*0.7f; SolidBrush sh(Color(BYTE((18+i*12)*alpha),0,0,0));
          g.FillEllipse(&sh,RectF(kr.X+o,kr.Y+o,kr.Width,kr.Height)); }
      LinearGradientBrush kf(PointF(kx2-kR2,ky2-kR2),PointF(kx2-kR2,ky2+kR2),
                              Color(ba,250,249,255),Color(ba,220,218,230));
      g.FillEllipse(&kf,kr);
      LinearGradientBrush ks(PointF(kx2-kR2,ky2-kR2),PointF(kx2-kR2,ky2),Color(BYTE(50*alpha),255,255,255),Color(0,255,255,255));
      g.FillEllipse(&ks,kr);
      if (act) { Pen gr2(toGdi(ac,BYTE(180*alpha)),1.5f); g.DrawEllipse(&gr2,RectF(kr.X-1,kr.Y-1,kr.Width+2,kr.Height+2)); }
      else { Pen ep(Color(BYTE(20*alpha),255,255,255),1.f); g.DrawEllipse(&ep,kr); } }

    { wchar_t vs[16]; swprintf_s(vs,L"%ums",m_delayMs[5]);
      Font vf(L"Segoe UI",8.5f,FontStyleBold); SolidBrush vb(toGdi(ac,BYTE(225*alpha)));
      g.DrawString(vs,-1,&vf,RectF(tr2.X+tr2.Width+4.f,MC_DLY_Y,52.f,DLY_H),&sfR4,&vb); }
}

// ─── drawHoverControls ───────────────────────────────────────────────────────
void PopupWindow::drawHoverControls(Graphics& g, float px) {
    float alpha = clamp01(m_animT * 2.f);
    COLORREF ac = TYPE_COLORS[6];

    Font mf(L"Segoe UI", 8.f, FontStyleBold);
    StringFormat sfC3; sfC3.SetAlignment(StringAlignmentCenter); sfC3.SetLineAlignment(StringAlignmentCenter);

    // Mode buttons
    for (int mi = 0; mi < 3; mi++) {
        RectF mr = modeBtnRect(mi, px);
        bool  ms = (mi == int(m_hoverMode));
        bool  mh = (m_hoveredMode == mi && !ms);
        float r2 = mr.Height / 2.f;

        if (ms) {
            // Active: glow + green gradient + shimmer
            for (int k = 2; k >= 1; k--) {
                float ext = float(k) * 2.5f;
                RectF gr(mr.X-ext, mr.Y-ext, mr.Width+ext*2, mr.Height+ext*2);
                SolidBrush gb(Color(BYTE((16-k*6)*alpha), GetRValue(ac), GetGValue(ac), GetBValue(ac)));
                auto* p = makeRoundRectPath(gr, r2+ext); g.FillPath(&gb, p); delete p;
            }
            LinearGradientBrush mb(PointF(mr.X,mr.Y), PointF(mr.X,mr.Y+mr.Height),
                                   toGdi(ac,BYTE(95*alpha)), toGdi(ac,BYTE(60*alpha)));
            drawRoundRect(g, mr, r2, mb);
            GraphicsPath* cl = makeRoundRectPath(mr, r2); Region cr(cl); g.SetClip(&cr); delete cl;
            LinearGradientBrush sh(PointF(mr.X,mr.Y), PointF(mr.X,mr.Y+mr.Height*0.5f),
                                   Color(BYTE(32*alpha),255,255,255), Color(0,255,255,255));
            g.FillRectangle(&sh, mr.X, mr.Y, mr.Width, mr.Height*0.5f);
            g.ResetClip();
            Pen mbp(toGdi(ac, BYTE(200*alpha)), 1.2f); drawRoundRectStroke(g, mr, r2, mbp);
            SolidBrush mt(toGdi(ac, BYTE(235*alpha)));
            g.DrawString(HOVER_MODE_LABELS[mi], -1, &mf, RectF(mr.X, mr.Y + 1.0f, mr.Width, mr.Height), &sfC3, &mt);
        } else {
            SolidBrush mb(Color(BYTE((mh?30:16)*alpha), 255,255,255));
            drawRoundRect(g, mr, r2, mb);
            Pen mbp(Color(BYTE((mh?45:22)*alpha), 255,255,255), 1.f);
            drawRoundRectStroke(g, mr, r2, mbp);
            SolidBrush mt(Color(BYTE((mh?190:130)*alpha), 245,245,247));
            g.DrawString(HOVER_MODE_LABELS[mi], -1, &mf, RectF(mr.X, mr.Y + 1.0f, mr.Width, mr.Height), &sfC3, &mt);
        }
    }

    // Focus-only toggle button
    { RectF fr  = focusBtnRect(px);
      RectF btn(fr.X, fr.Y, fr.Width, 28.f);
      float br  = btn.Height / 2.f;
      bool  fh  = m_hoveredFocusBtn;
      bool  fon = m_hoverOnlyFocused;

      if (fon) {
          // Active: green glow + gradient fill + shimmer
          for (int k = 2; k >= 1; k--) {
              float ext = float(k) * 3.f;
              RectF gr(btn.X-ext, btn.Y-ext, btn.Width+ext*2, btn.Height+ext*2);
              SolidBrush gb(Color(BYTE((18-k*7)*alpha), GetRValue(ac), GetGValue(ac), GetBValue(ac)));
              auto* p = makeRoundRectPath(gr, br+ext); g.FillPath(&gb, p); delete p;
          }
          LinearGradientBrush fill(PointF(btn.X,btn.Y), PointF(btn.X,btn.Y+btn.Height),
                                   toGdi(ac,BYTE(90*alpha)), toGdi(ac,BYTE(55*alpha)));
          drawRoundRect(g, btn, br, fill);
          GraphicsPath* cl = makeRoundRectPath(btn, br); Region cr(cl); g.SetClip(&cr); delete cl;
          LinearGradientBrush sh(PointF(btn.X,btn.Y), PointF(btn.X,btn.Y+btn.Height*0.5f),
                                 Color(BYTE(30*alpha),255,255,255), Color(0,255,255,255));
          g.FillRectangle(&sh, btn.X, btn.Y, btn.Width, btn.Height*0.5f);
          g.ResetClip();
          Pen bp(toGdi(ac, BYTE(200*alpha)), 1.2f); drawRoundRectStroke(g, btn, br, bp);
      } else {
          // Inactive: dark fill + subtle border
          SolidBrush fill(Color(BYTE((fh?30:16)*alpha), 255,255,255));
          drawRoundRect(g, btn, br, fill);
          Pen bp(Color(BYTE((fh?45:22)*alpha),255,255,255), 1.f);
          drawRoundRectStroke(g, btn, br, bp);
      }

      // State dot
      float dotR  = 4.f;
      float dotCx = btn.X + br;
      float dotCy = btn.Y + btn.Height / 2.f;
      SolidBrush dotB(toGdi(ac, BYTE((fon?230:70)*alpha)));
      g.FillEllipse(&dotB, RectF(dotCx-dotR, dotCy-dotR, dotR*2, dotR*2));

      Font labf(L"Segoe UI", 8.f, FontStyleBold);
      BYTE ta = BYTE((fon ? 235 : (fh ? 160 : 110)) * alpha);
      SolidBrush lb2(fon ? toGdi(ac, ta) : Color(ta, 245,245,247));
      StringFormat sfL7; sfL7.SetAlignment(StringAlignmentCenter); sfL7.SetLineAlignment(StringAlignmentCenter);
      g.DrawString(L"ON APP FOCUS", -1, &labf,
                   RectF(btn.X + dotR*2 + 4.f, btn.Y + 1.0f, btn.Width - dotR*2 - 4.f, btn.Height), &sfL7, &lb2);
    }
}

void PopupWindow::updatePanelCache(int dt) {
    if (dt < 0 || dt >= NUM_TYPES) return;
    if (!m_panelCache[dt]) {
        m_panelCache[dt] = new Gdiplus::Bitmap(PANEL_W, POPUP_H, PixelFormat32bppARGB);
    }
    Graphics g(m_panelCache[dt]);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(Color(0, 0, 0, 0));

    // Save state
    float oldT = m_animT;
    int oldDt = m_displayType;
    UINT_PTR oldTimer = m_animTimer;
    m_animT = 1.0f; // force full alpha, panelX() returns 0.f
    m_displayType = dt;
    m_animTimer = 0; // force live draw bypass in drawPanel

    // Clear hovers to not bake them
    int hwf = m_hoveredWf; m_hoveredWf = -1;
    bool ht = m_hoveredToggle; m_hoveredToggle = false;
    bool hd = m_hovDelaySlider; m_hovDelaySlider = false;
    int hmod = m_hoveredMode; m_hoveredMode = -1;
    bool hfoc = m_hoveredFocusBtn; m_hoveredFocusBtn = false;
    int hmcw = m_hoveredMcWf; m_hoveredMcWf = -1;
    bool hmcd = m_hovDelayMcSlider; m_hovDelayMcSlider = false;
    bool hmct = m_hoveredMcToggle; m_hoveredMcToggle = false;

    // Draw
    drawPanel(g);

    // Restore state
    m_animT = oldT;
    m_displayType = oldDt;
    m_animTimer = oldTimer;
    m_hoveredWf = hwf;
    m_hoveredToggle = ht;
    m_hovDelaySlider = hd;
    m_hoveredMode = hmod;
    m_hoveredFocusBtn = hfoc;
    m_hoveredMcWf = hmcw;
    m_hovDelayMcSlider = hmcd;
    m_hoveredMcToggle = hmct;
}

} // namespace ui
