#pragma once
#include "HidDevice.h"
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <optional>
#include <thread>
#include <atomic>
#include <functional>

namespace hidpp {

// ─── HID++ 2.0 constants ────────────────────────────────────────────────────

constexpr uint8_t  REPORT_SHORT        = 0x10;  // 7  bytes total
constexpr uint8_t  REPORT_LONG         = 0x11;  // 20 bytes total
constexpr uint8_t  REPORT_VERY_LONG    = 0x12;  // 64 bytes total

constexpr uint8_t  DEVICE_INDEX_WIRED  = 0xFF;
constexpr uint8_t  DEVICE_INDEX_BOLT   = 0x01;  // First paired Bolt device
constexpr uint8_t  SW_ID               = 0x01;  // Software ID (1-15)

// ─── Feature IDs ────────────────────────────────────────────────────────────
namespace FeatureId {
    constexpr uint16_t ROOT               = 0x0000;
    constexpr uint16_t FEATURE_SET        = 0x0001;
    constexpr uint16_t FIRMWARE_INFO      = 0x0003;
    constexpr uint16_t DEVICE_NAME        = 0x0005;
    constexpr uint16_t BATTERY_UNIFIED    = 0x1004;
    constexpr uint16_t BATTERY_STATUS     = 0x1000;
    constexpr uint16_t SMART_SHIFT        = 0x2110;
    constexpr uint16_t SMART_SHIFT_ENAHANCED = 0x2111;
    constexpr uint16_t HI_RES_WHEEL       = 0x2121;
    constexpr uint16_t FORCE_SENSE        = 0x8123;  // Haptic – click force sense
    constexpr uint16_t HAPTIC_FEEDBACK    = 0x8134;  // Haptic – waveform engine
    constexpr uint16_t ONBOARD_PROFILES   = 0x8100;
    constexpr uint16_t REPROG_CONTROLS_V4 = 0x1B04;
    constexpr uint16_t THUMB_WHEEL        = 0x2150;  // Side/thumb scroll wheel
}

// ─── Waveform definitions ────────────────────────────────────────────────────
// Hardware ROM waveform IDs for the MX Master 4 (Solaar HapticWaveForms spec).
// 0x00-0x0E are contiguous; 0x1B (Whisper) is a non-contiguous bonus ID.
enum class Waveform : uint8_t {
    Alert      = 0x00,  // Sharp State Change
    Impact     = 0x01,  // Damp State Change
    Wave       = 0x02,  // Sharp Collision
    Pop        = 0x03,  // Damp Collision
    Chime      = 0x04,  // Subtle Collision
    Click      = 0x05,  // Happy Alert
    DoubleClick= 0x06,  // Angry Alert
    Knock      = 0x07,  // Completed
    Burst      = 0x08,  // Square
    Rumble     = 0x09,  // Wave
    Pulse      = 0x0A,  // Firework
    Snap       = 0x0B,  // Mad
    Buzz       = 0x0C,  // Knock
    Vibrate    = 0x0D,  // Jingle
    Heartbeat  = 0x0E,  // Ringing
    Whisper    = 0x1B,  // Whisper Collision (non-contiguous)
    COUNT      = 15     // Count of base (contiguous) waveforms
};

// Safe name lookup — handles the gap at 0x0F-0x1A
inline const wchar_t* waveformName(int id) {
    switch (id) {
    case 0x00: return L"S. Change";
    case 0x01: return L"D. Change";
    case 0x02: return L"Sharp Hit";
    case 0x03: return L"Damp Hit";
    case 0x04: return L"Subtle";
    case 0x05: return L"Happy";
    case 0x06: return L"Angry";
    case 0x07: return L"Done";
    case 0x08: return L"Square";
    case 0x09: return L"Wave";
    case 0x0A: return L"Firework";
    case 0x0B: return L"Mad";
    case 0x0C: return L"Knock";
    case 0x0D: return L"Jingle";
    case 0x0E: return L"Ringing";
    case 0x1B: return L"Whisper";
    default:   return L"";
    }
}

// Kept for any legacy references; prefer waveformName() for display
static constexpr const wchar_t* WAVEFORM_NAMES[] = {
    L"S. Change", L"D. Change", L"Sharp Hit", L"Damp Hit",  L"Subtle",
    L"Happy",     L"Angry",     L"Done",      L"Square",    L"Wave",
    L"Firework",  L"Mad",       L"Knock",     L"Jingle",    L"Ringing"
};

// 12 waveforms shown in the popup grid (4 × 3).
// Includes Whisper (0x1B) — the non-contiguous bonus waveform.
static constexpr int SHOWN_WF_COUNT  = 12;
static constexpr int SHOWN_WAVEFORMS[SHOWN_WF_COUNT] = {
    0x00, 0x01, 0x02, 0x03,   // S.Change, D.Change, SharpHit, DampHit
    0x04, 0x05, 0x06, 0x07,   // Subtle, Happy, Angry, Done
    0x08, 0x09, 0x0B, 0x1B    // Square, Wave, Mad, Whisper
};

struct Packet {
    std::vector<uint8_t> bytes;

    static Packet makeLong(uint8_t devIdx, uint8_t featIdx, uint8_t func,
                           std::initializer_list<uint8_t> params = {});
    static Packet makeShort(uint8_t devIdx, uint8_t featIdx, uint8_t func,
                            std::initializer_list<uint8_t> params = {});

    bool isError() const;
    uint8_t  reportId()     const { return bytes.size() > 0 ? bytes[0] : 0; }
    uint8_t  deviceIndex()  const { return bytes.size() > 1 ? bytes[1] : 0; }
    uint8_t  featureIndex() const { return bytes.size() > 2 ? bytes[2] : 0; }
    uint8_t  funcSwId()     const { return bytes.size() > 3 ? bytes[3] : 0; }
    uint8_t  function()     const { return funcSwId() >> 4; }
    uint8_t  param(size_t i) const {
        return bytes.size() > 4 + i ? bytes[4 + i] : 0;
    }
};

// ─── HidppDevice ─────────────────────────────────────────────────────────────
class HidppDevice {
public:
    HidppDevice();
    ~HidppDevice() = default;

    // Connect – tries Bolt receiver first, then direct BT/USB
    bool connect();
    void disconnect();
    bool isConnected() const;

    std::wstring deviceName() const { return m_deviceName; }
    int          batteryPct()  const { return m_batteryPct; }

    // Feature discovery
    bool     hasFeature(uint16_t featureId) const;
    uint8_t  featureIndex(uint16_t featureId) const;
    bool     hasHapticSupport() const { return m_hapticFeatureIdx != 0; }

    // Send request and wait for matching response
    std::optional<Packet> request(uint8_t featIdx, uint8_t func,
                                  std::initializer_list<uint8_t> params = {},
                                  uint8_t devIdx = DEVICE_INDEX_WIRED,
                                  int timeoutMs = 150,
                                  int maxAttempts = 3);

    // Trigger a haptic waveform (returns false if haptic not supported)
    bool playHaptic(Waveform wf);

    // Refresh battery
    bool refreshBattery();

    // Enumerate all features (for debugging)
    std::vector<std::pair<uint16_t, uint8_t>> enumerateFeatures();

    const std::string& connectedPath() const { return m_path; }

    // ── Thumb-wheel HID++ notifications (feature 0x2150) ─────────────────────
    // Fires one callback per physical notch, regardless of Logitech driver overrides.
    using ThumbWheelCallback = std::function<void(int delta)>;
    void setThumbWheelCallback(ThumbWheelCallback cb) { m_thumbWheelCb = std::move(cb); }
    bool supportsThumbWheel() const { return m_thumbWheelFeatureIdx != 0; }

    // ── Gesture button via REPROG_CONTROLS_V4 (feature 0x1B04) ───────────────
    // The gesture button (CID 0x00C4 on MX Master 4) is a "virtual" side button
    // that never appears as a standard Windows mouse event.  We divert it via
    // HID++ so the notification thread can catch its press events.
    using GestureButtonCallback = std::function<void(uint16_t cid)>;
    void setGestureButtonCallback(GestureButtonCallback cb) { m_gestureButtonCb = std::move(cb); }

    void startNotifications();
    void stopNotifications();

private:
    void notificationThreadFn();
    bool tryConnect(const DeviceInfo& info, uint8_t devIdx,
                    const std::string& shortPath = {});
    bool discoverFeatures(uint8_t devIdx);
    bool fetchDeviceName(uint8_t devIdx);
    bool fetchBattery(uint8_t devIdx);
    void resolveHapticFeature(uint8_t devIdx);

    HidDevice                   m_hid;          // long-report endpoint (discovery/control)
    HidDevice                   m_hidShort;     // short-report endpoint (haptic)
    HidDevice                   m_hidNotify;    // second long-report handle (notifications)
    ThumbWheelCallback          m_thumbWheelCb;
    GestureButtonCallback       m_gestureButtonCb;
    std::thread                 m_notifyThread;
    std::atomic<bool>           m_notifyRunning{false};
    uint8_t                     m_thumbWheelFeatureIdx = 0;
    uint8_t                     m_reprogCtrlFeatureIdx = 0;
    std::vector<uint16_t>       m_divertedCIDs;     // CIDs diverted for gesture detection
    std::string                 m_path;
    std::wstring                m_deviceName;
    int                         m_batteryPct        = -1;
    std::map<uint16_t, uint8_t> m_featureMap;       // featureId -> index
    uint8_t                     m_devIdx            = DEVICE_INDEX_WIRED;
    uint8_t                     m_hapticFeatureIdx  = 0; // resolved at connect
    bool                        m_hapticUsesShort   = false; // true = SHORT report protocol
    bool                        m_connected         = false;
};

} // namespace hidpp
