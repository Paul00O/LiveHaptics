#pragma once
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <functional>
#include <string>
#include "haptics/HapticController.h"

namespace ui {

// ─── Palette (COLORREF = 0x00BBGGRR) ─────────────────────────────────────────
namespace Colors {
    constexpr COLORREF BG_DARK    = 0x100D0D;
    constexpr COLORREF BG_SURFACE = 0x1C1818;
    constexpr COLORREF TEXT_MAIN  = 0xF7F5F5;
    constexpr COLORREF TEXT_DIM   = 0x908C8C;
    constexpr COLORREF STATUS_OK     = 0x6FC926;
    constexpr COLORREF STATUS_WARN   = 0x0ADBFF;
    constexpr COLORREF STATUS_ERR    = 0x3A45FF;
    constexpr COLORREF STATUS_CHARGE = 0x07C1FF;  // amber: R=255 G=193 B=7
}

inline Gdiplus::Color toGdi(COLORREF cr, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(cr), GetGValue(cr), GetBValue(cr));
}

// ─── Layout ──────────────────────────────────────────────────────────────────
constexpr int POPUP_W   = 720;
constexpr int POPUP_H   = 800;
constexpr int NUM_TYPES = 7;
constexpr int PANEL_W   = 258;
constexpr int HEADER_H  = 58;

// ─── PopupWindow ─────────────────────────────────────────────────────────────
class PopupWindow {
public:
    using WaveformCallback         = std::function<void(hidpp::Waveform)>;
    using PreviewCallback          = std::function<void(hidpp::Waveform)>;
    using EnabledCallback          = std::function<void(bool)>;
    using ModeCallback             = std::function<void(haptics::HoverMode)>;
    using DelayCallback            = std::function<void(int type, uint32_t ms)>;
    using CloseCallback            = std::function<void()>;
    using HoverOnlyFocusedCallback = std::function<void(bool)>;

    PopupWindow();
    ~PopupWindow();

    bool create(HINSTANCE hInst);
    void show(POINT anchor);
    void hide();
    bool isVisible() const;
    HWND hwnd() const { return m_hwnd; }

    void setConnected(bool connected, const std::wstring& name);
    void setBattery(int pct, bool charging);
    void setHapticsSupported(bool s) { m_hapticsSupported = s; }

    // Setters — 0=Scroll,1=SideScroll,2=L.Click,3=R.Click,4=SideButtons,5=ScrollClick,6=Hover
    void setScrollWaveform      (hidpp::Waveform wf) { m_wf[0]=int(wf); invalidate(); }
    void setSideScrollWaveform  (hidpp::Waveform wf) { m_wf[1]=int(wf); invalidate(); }
    void setClickWaveform       (hidpp::Waveform wf) { m_wf[2]=int(wf); invalidate(); }
    void setRightClickWaveform  (hidpp::Waveform wf) { m_wf[3]=int(wf); invalidate(); }
    void setSideButtonWaveform  (hidpp::Waveform wf) { m_wf[4]=int(wf); invalidate(); }
    void setScrollClickWaveform (hidpp::Waveform wf) { m_wf[5]=int(wf); invalidate(); }
    void setHoverWaveform       (hidpp::Waveform wf) { m_wf[6]=int(wf); invalidate(); }
    void setScrollEnabled       (bool e)             { m_enabled[0]=e;  invalidate(); }
    void setSideScrollEnabled   (bool e)             { m_enabled[1]=e;  invalidate(); }
    void setClickEnabled        (bool e)             { m_enabled[2]=e;  invalidate(); }
    void setRightClickEnabled   (bool e)             { m_enabled[3]=e;  invalidate(); }
    void setSideButtonEnabled   (bool e)             { m_enabled[4]=e;  invalidate(); }
    void setScrollClickEnabled  (bool e)             { m_enabled[5]=e;  invalidate(); }
    void setHoverEnabled        (bool e)             { m_enabled[6]=e;  invalidate(); }
    void setHoverOnlyFocused    (bool e)             { m_hoverOnlyFocused=e; invalidate(); }
    void setHoverMode           (haptics::HoverMode m){ m_hoverMode=m;  invalidate(); }
    void setDelay(int type, uint32_t ms) {
        if (type >= 0 && type < NUM_TYPES) { m_delayMs[type]=ms; invalidate(); }
    }

    // Callbacks
    void setScrollWaveformCallback     (WaveformCallback  cb) { m_onWaveform[0]=std::move(cb); }
    void setSideScrollWaveformCallback (WaveformCallback  cb) { m_onWaveform[1]=std::move(cb); }
    void setClickWaveformCallback      (WaveformCallback  cb) { m_onWaveform[2]=std::move(cb); }
    void setRightClickWaveformCallback (WaveformCallback  cb) { m_onWaveform[3]=std::move(cb); }
    void setSideButtonWaveformCallback (WaveformCallback  cb) { m_onWaveform[4]=std::move(cb); }
    void setScrollClickWaveformCallback(WaveformCallback  cb) { m_onWaveform[5]=std::move(cb); }
    void setHoverWaveformCallback      (WaveformCallback  cb) { m_onWaveform[6]=std::move(cb); }
    void setScrollEnabledCallback      (EnabledCallback   cb) { m_onEnabled[0]=std::move(cb); }
    void setSideScrollEnabledCallback  (EnabledCallback   cb) { m_onEnabled[1]=std::move(cb); }
    void setClickEnabledCallback       (EnabledCallback   cb) { m_onEnabled[2]=std::move(cb); }
    void setRightClickEnabledCallback  (EnabledCallback   cb) { m_onEnabled[3]=std::move(cb); }
    void setSideButtonEnabledCallback  (EnabledCallback   cb) { m_onEnabled[4]=std::move(cb); }
    void setScrollClickEnabledCallback (EnabledCallback   cb) { m_onEnabled[5]=std::move(cb); }
    void setHoverEnabledCallback       (EnabledCallback   cb) { m_onEnabled[6]=std::move(cb); }
    void setHoverOnlyFocusedCallback   (HoverOnlyFocusedCallback cb) { m_onHoverOnlyFocused=std::move(cb); }
    void setHoverModeCallback          (ModeCallback      cb) { m_onHoverMode=std::move(cb); }
    void setDelayCallback              (DelayCallback     cb) { m_onDelay=std::move(cb); }
    void setPreviewCallback            (PreviewCallback   cb) { m_onPreview=std::move(cb); }
    void setCloseCallback              (CloseCallback     cb) { m_onClose=std::move(cb); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM);

    void onPaint();
    void onMouseMove(int x, int y);
    void onLButtonDown(int x, int y);
    void onLButtonUp(int x, int y);
    void onTimer();
    void invalidate() { if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); }

    // ── Drawing ──────────────────────────────────────────────────────────────
    void drawBackground(Gdiplus::Graphics& g);
    void drawHeader(Gdiplus::Graphics& g);
    void drawMouseSection(Gdiplus::Graphics& g);
    void drawHotspots(Gdiplus::Graphics& g, Gdiplus::RectF imgRect);
    void drawHoverPill(Gdiplus::Graphics& g);
    void drawPanel(Gdiplus::Graphics& g);
    void drawWaveformGrid(Gdiplus::Graphics& g, float px);
    void drawDelaySlider(Gdiplus::Graphics& g, float px);
    void drawHoverControls(Gdiplus::Graphics& g, float px);
    void drawMidClickSection(Gdiplus::Graphics& g, float px);
    void drawPremiumToggle(Gdiplus::Graphics& g, Gdiplus::RectF r,
                           bool on, bool hov, COLORREF accent);
    void drawRoundRect(Gdiplus::Graphics& g, Gdiplus::RectF r,
                       float radius, const Gdiplus::Brush& fill);
    void drawRoundRectStroke(Gdiplus::Graphics& g, Gdiplus::RectF r,
                             float radius, const Gdiplus::Pen& pen);
    void drawWaveformViz(Gdiplus::Graphics& g, int wfId,
                         Gdiplus::RectF area, Gdiplus::Color col);

    // ── Geometry ─────────────────────────────────────────────────────────────
    Gdiplus::RectF mouseImageRect() const;
    Gdiplus::RectF hoverPillRect() const;
    float          panelX() const;    // animated x origin of panel
    Gdiplus::RectF wfCellRect(int si, float px) const;
    Gdiplus::RectF delayTrackRect(float px) const;
    Gdiplus::RectF delayKnobRect(float px) const;
    Gdiplus::RectF modeBtnRect(int mi, float px) const;
    Gdiplus::RectF focusBtnRect(float px) const;
    Gdiplus::RectF enableToggleRect(float px) const;

    // ── Hit testing ──────────────────────────────────────────────────────────
    int  hotspotAt(int x, int y) const;
    bool hoverPillAt(int x, int y) const;
    int  wfAt(int x, int y, float px) const;
    bool delaySliderAt(int x, int y, float px) const;
    int  modeAt(int x, int y, float px) const;
    bool focusBtnAt(int x, int y, float px) const;
    bool enableToggleAt(int x, int y, float px) const;

    void updateDelayFromX(int x, float px);
    void selectFeature(int idx);
    void startAnim(float target);
    float easedT() const;

    // ── State ────────────────────────────────────────────────────────────────
    HWND         m_hwnd             = nullptr;
    HINSTANCE    m_hInst            = nullptr;
    bool         m_connected        = false;
    bool         m_hapticsSupported = false;
    std::wstring m_deviceName;
    int          m_batteryPct       = -1;
    bool         m_isCharging       = false;

    int                m_activeType   = -1; // -1 = none
    int                m_displayType  =  0; // shown in panel while animating closed
    int                m_wf[NUM_TYPES]      = {2,2,3,0,0,1,4};
    bool               m_enabled[NUM_TYPES] = {true,true,true,true,true,true,false};
    haptics::HoverMode m_hoverMode          = haptics::HoverMode::Enter;
    bool               m_hoverOnlyFocused   = true;
    uint32_t           m_delayMs[NUM_TYPES] = {0,80,0,0,0,0,0};

    // Hover / interaction state
    int  m_hoveredHotspot   = -1;
    bool m_hoveredHoverPill = false;
    int  m_hoveredWf        = -1;
    int  m_hoveredMode      = -1;
    bool m_hoveredFocusBtn  = false;
    bool m_hoveredToggle    = false;
    bool m_hovDelaySlider   = false;
    bool m_draggingDelay    = false;
    int  m_hoveredMcWf      = -1;
    bool m_hovDelayMcSlider = false;
    bool m_hoveredMcToggle  = false;
    bool m_draggingMcDelay  = false;

    // Animation
    float    m_animT      = 0.f;
    float    m_animTarget = 0.f;
    UINT_PTR m_animTimer  = 0;
    ULONGLONG m_animLastTick = 0;
    
    // Rendering Cache
    Gdiplus::Bitmap* m_panelCache[NUM_TYPES] = {nullptr};
    Gdiplus::Bitmap* m_bgCache = nullptr;
    float m_lastBgAnimT = -1.f;
    void updatePanelCache(int dt);

    // Mouse image (loaded from embedded resource)
    Gdiplus::Image* m_mouseImg     = nullptr;
    ULONG_PTR       m_gdiplusToken = 0;

    WaveformCallback         m_onWaveform[NUM_TYPES];
    PreviewCallback          m_onPreview;
    EnabledCallback          m_onEnabled[NUM_TYPES];
    ModeCallback             m_onHoverMode;
    HoverOnlyFocusedCallback m_onHoverOnlyFocused;
    DelayCallback            m_onDelay;
    CloseCallback            m_onClose;
};

} // namespace ui
