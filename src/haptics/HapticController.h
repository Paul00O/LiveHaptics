#pragma once
#include "hidpp/HidppDevice.h"
#include <Windows.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <cstdint>

namespace haptics {

using hidpp::Waveform;

enum class HoverMode : uint8_t { Enter = 0, Exit = 1, Both = 2 };

struct HapticConfig {
    // Vertical scroll
    Waveform  scrollWaveform       = Waveform::Wave;
    bool      scrollEnabled        = true;

    // Side / lateral scroll wheel
    Waveform  sideScrollWaveform   = Waveform::Wave;
    bool      sideScrollEnabled    = true;

    // Left click
    Waveform  clickWaveform        = Waveform::Pop;
    bool      clickEnabled         = true;

    // Right click
    Waveform  rightClickWaveform   = Waveform::Alert;
    bool      rightClickEnabled    = true;

    // Side buttons (forward / back)
    Waveform  sideButtonWaveform   = Waveform::Alert;
    bool      sideButtonEnabled    = true;

    // Scroll wheel click (middle button)
    Waveform  scrollClickWaveform  = Waveform::Impact;
    bool      scrollClickEnabled   = true;

    // Per-feature minimum delay between haptic fires (0 = every event)
    uint32_t  scrollCooldownMs      = 0;
    uint32_t  sideScrollCooldownMs  = 80;
    uint32_t  clickCooldownMs       = 0;
    uint32_t  rightClickCooldownMs  = 0;
    uint32_t  sideButtonCooldownMs  = 0;
    uint32_t  scrollClickCooldownMs = 0;

    // Hover
    Waveform  hoverWaveform        = Waveform::Chime;
    bool      hoverEnabled         = false;
    bool      hoverOnlyFocused     = true;
    HoverMode hoverMode            = HoverMode::Enter;
    uint32_t  hoverCooldownMs      = 0;
};

class HapticController {
public:
    HapticController();
    ~HapticController();

    using ConnectCallback = std::function<void(bool connected, const std::wstring& name)>;
    using BatteryCallback = std::function<void(int pct)>;

    void setConnectCallback(ConnectCallback cb) { m_onConnect = std::move(cb); }
    void setBatteryCallback(BatteryCallback cb) { m_onBattery = std::move(cb); }

    void start();
    void stop();

    bool         isConnected()        const { return m_connected.load(); }
    std::wstring deviceName()         const;
    int          batteryPct()         const { return m_batteryPct.load(); }
    bool         supportsHaptics()    const { return m_hapticsSupported.load(); }
    bool         supportsThumbWheel() const { return m_device.supportsThumbWheel(); }

    void play(Waveform wf);  // UI preview
    void playScroll();
    void playSideScroll();
    void playLeftClick();
    void playRightClick();
    void playSideButton();
    void playScrollClick();

    HapticConfig& config() { return m_config; }

private:
    struct PlayCommand { Waveform wf; };

    void hapticThreadFn();
    void managerThreadFn();
    void checkHover();

    hidpp::HidppDevice m_device;
    HapticConfig       m_config;

    std::thread        m_hapticThread;
    std::thread        m_managerThread;
    std::atomic<bool>  m_running{false};

    std::atomic<bool>  m_connected       {false};
    std::atomic<bool>  m_hapticsSupported{false};
    std::atomic<int>   m_batteryPct      {-1};

    mutable std::mutex       m_nameMutex;
    std::wstring             m_deviceName;

    std::mutex               m_queueMutex;
    std::queue<PlayCommand>  m_queue;

    std::condition_variable  m_cv;
    std::mutex               m_cvMutex;

    std::atomic<bool>  m_scrollPending      {false};
    std::atomic<bool>  m_sideScrollPending  {false};
    std::atomic<bool>  m_leftClickPending   {false};
    std::atomic<bool>  m_rightClickPending  {false};
    std::atomic<bool>  m_sideButtonPending  {false};
    std::atomic<bool>  m_scrollClickPending {false};

    ULONGLONG m_lastScrollFireTick      = 0;
    ULONGLONG m_lastSideScrollFireTick  = 0;
    ULONGLONG m_lastClickFireTick       = 0;
    ULONGLONG m_lastRightClickFireTick  = 0;
    ULONGLONG m_lastSideButtonFireTick  = 0;
    ULONGLONG m_lastScrollClickFireTick = 0;

    // Hover state – haptic thread private
    IUIAutomation* m_pUIA           = nullptr;  // UIAutomation (primary)
    HCURSOR        m_handCursor     = nullptr;
    bool           m_wasHovering    = false;
    int            m_hoverCounter   = 0;
    RECT           m_lastElemRect   = {};
    LRESULT        m_lastNcRegion   = HTNOWHERE;
    bool           m_lastCursorWasHand = false;
    ULONGLONG      m_lastHoverFireTick = 0;
    POINT          m_lastHoverFirePos  = {0, 0};   // dedup: cursor pos when last fired

    ConnectCallback m_onConnect;
    BatteryCallback m_onBattery;
};

} // namespace haptics
