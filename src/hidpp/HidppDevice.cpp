#include "HidppDevice.h"
#include <algorithm>
#include <cstring>
#include <Windows.h>

namespace hidpp {

// ─── Logitech device candidates (VID 0x046D) ────────────────────────────────
// Both Bolt (C548) and Unifying (C52B) receivers expose HID++ on:
//   Usage Page 0xFF00, Usage 0x0002  (long reports – what we need)
//   Usage Page 0xFF00, Usage 0x0001  (short reports)
static constexpr uint16_t LOGITECH_VID = 0x046D;

static constexpr uint16_t PID_BOLT_RECEIVER      = 0xC548;
static constexpr uint16_t PID_UNIFYING_RECEIVER   = 0xC52B;

static constexpr uint16_t HIDPP_USAGE_PAGE  = 0xFF00;
static constexpr uint16_t HIDPP_USAGE_LONG  = 0x0002;
static constexpr uint16_t HIDPP_USAGE_SHORT = 0x0001;

// ─── Packet helpers ──────────────────────────────────────────────────────────
Packet Packet::makeLong(uint8_t devIdx, uint8_t featIdx, uint8_t func,
                         std::initializer_list<uint8_t> params) {
    Packet p;
    p.bytes.resize(20, 0);
    p.bytes[0] = REPORT_LONG;
    p.bytes[1] = devIdx;
    p.bytes[2] = featIdx;
    p.bytes[3] = static_cast<uint8_t>((func << 4) | SW_ID);
    size_t i = 0;
    for (uint8_t b : params) { if (i >= 16) break; p.bytes[4 + i++] = b; }
    return p;
}

Packet Packet::makeShort(uint8_t devIdx, uint8_t featIdx, uint8_t func,
                          std::initializer_list<uint8_t> params) {
    Packet p;
    p.bytes.resize(7, 0);
    p.bytes[0] = REPORT_SHORT;
    p.bytes[1] = devIdx;
    p.bytes[2] = featIdx;
    p.bytes[3] = static_cast<uint8_t>((func << 4) | SW_ID);
    size_t i = 0;
    for (uint8_t b : params) { if (i >= 3) break; p.bytes[4 + i++] = b; }
    return p;
}

bool Packet::isError() const {
    return bytes.size() >= 4 &&
           (bytes[0] == REPORT_SHORT || bytes[0] == REPORT_LONG) &&
           bytes[2] == 0xFF;
}

// ─── HidppDevice ─────────────────────────────────────────────────────────────
HidppDevice::HidppDevice() = default;

bool HidppDevice::connect() {
    disconnect();

    auto devices = HidDevice::enumerate(LOGITECH_VID, 0);

    auto isHidppLong  = [](const DeviceInfo& d) {
        return d.usagePage == HIDPP_USAGE_PAGE && d.usage == HIDPP_USAGE_LONG;
    };
    auto isHidppShort = [](const DeviceInfo& d) {
        return d.usagePage == HIDPP_USAGE_PAGE && d.usage == HIDPP_USAGE_SHORT;
    };

    // Try Bolt (C548) and Unifying (C52B) on all possible device indices 1-6
    auto tryReceiver = [&](uint16_t pid) {
        // Pre-scan: find the short endpoint path (for haptic writes after connecting)
        std::string shortPath;
        for (auto& d : devices) {
            if (d.productId == pid && isHidppShort(d)) { shortPath = d.path; break; }
        }
        // Iterate all HID++ endpoints (short first, then long) – same as before.
        // The short endpoint passes fail gracefully; the long endpoint at the correct
        // device index will succeed. Pass shortPath only for long endpoints.
        for (auto& d : devices) {
            if (d.productId != pid) continue;
            if (!isHidppLong(d) && !isHidppShort(d)) continue;
            std::string sp = isHidppLong(d) ? shortPath : std::string{};
            for (uint8_t idx = 1; idx <= 6; idx++) {
                if (tryConnect(d, idx, sp)) {
                    return true;
                }
            }
        }
        return false;
    };

    if (tryReceiver(PID_BOLT_RECEIVER))     return true;
    if (tryReceiver(PID_UNIFYING_RECEIVER)) return true;

    return false;
}

bool HidppDevice::tryConnect(const DeviceInfo& info, uint8_t devIdx,
                              const std::string& shortPath) {
    if (!m_hid.open(info.path)) {
        return false;
    }

    // Ping: Root feature (index 0), function 0 = getFeature(ROOT=0x0000)
    auto resp = request(0x00, 0x00, {0x00, 0x00, 0x00}, devIdx);
    if (!resp) {
        m_hid.close();
        return false;
    }

    m_devIdx = devIdx;
    m_path   = info.path;

    // Open short endpoint for haptic writes (separate handle needed on Windows)
    if (!shortPath.empty()) {
        m_hidShort.open(shortPath);
    }

    discoverFeatures(devIdx);
    fetchDeviceName(devIdx);
    fetchBattery(devIdx);
    resolveHapticFeature(devIdx);

    m_connected = true;
    return true;
}

bool HidppDevice::discoverFeatures(uint8_t devIdx) {
    m_featureMap.clear();
    m_featureMap[FeatureId::ROOT] = 0;

    // Get feature set index: Root.getFeature(0x0001)
    auto r = request(0x00, 0x00,
                     {uint8_t(FeatureId::FEATURE_SET >> 8),
                      uint8_t(FeatureId::FEATURE_SET & 0xFF), 0},
                     devIdx);
    if (!r || r->param(0) == 0) return false;
    uint8_t fsIdx = r->param(0);
    m_featureMap[FeatureId::FEATURE_SET] = fsIdx;

    // Get count: FeatureSet.getCount()
    auto cnt = request(fsIdx, 0x00, {}, devIdx);
    if (!cnt) return false;
    uint8_t count = cnt->param(0);

    for (uint8_t i = 1; i <= count; i++) {
        // FeatureSet.getFeatureID(i)
        auto fr = request(fsIdx, 0x01, {i}, devIdx);
        if (!fr) continue;
        uint16_t fid = (uint16_t(fr->param(0)) << 8) | fr->param(1);
        if (fid) {
            m_featureMap[fid] = i;
        }
    }
    return true;
}

bool HidppDevice::fetchDeviceName(uint8_t devIdx) {
    auto it = m_featureMap.find(FeatureId::DEVICE_NAME);
    if (it == m_featureMap.end()) { m_deviceName = L"MX Master 4"; return false; }

    auto r = request(it->second, 0x00, {}, devIdx); // getNameLength
    if (!r) return false;
    uint8_t len = r->param(0);

    std::string name;
    uint8_t offset = 0;
    while (offset < len) {
        auto chunk = request(it->second, 0x01, {offset}, devIdx);
        if (!chunk) break;
        for (int i = 0; i < 16 && offset + i < len; i++) {
            char c = static_cast<char>(chunk->param(i));
            if (c == 0) break;
            name += c;
        }
        offset += 16;
    }
    m_deviceName = std::wstring(name.begin(), name.end());
    if (m_deviceName.empty()) m_deviceName = L"MX Master 4";
    return true;
}

bool HidppDevice::fetchBattery(uint8_t devIdx) {
    // BATTERY_UNIFIED (0x1004) — preferred, gives exact percentage + charging state
    //   func 0x01 = getStatus → param[0]=level(0-100), param[1]=nextLevel,
    //                            param[2]=batteryStatus, param[3]=externalPower
    //   batteryStatus: 0=discharging 1=recharging 2=almost_full 3=full 4=slow_charge
    //   externalPower: 0=not_present 1=present (cable plugged in)
    auto it = m_featureMap.find(FeatureId::BATTERY_UNIFIED);
    if (it != m_featureMap.end()) {
        auto r = request(it->second, 0x01, {}, devIdx);
        if (r && r->bytes.size() > 5 && r->param(0) <= 100) {
            m_batteryPct = r->param(0);
            uint8_t status   = r->param(2);  // battery charge status
            uint8_t external = r->param(3);  // 1 = charging cable present
            m_isCharging = (external == 1) || (status >= 1 && status <= 4);
            return true;
        }
    }
    // BATTERY_STATUS (0x1000) — legacy fallback
    //   func 0x00 = getBatteryLevelStatus → param[0]=level, param[2]=status
    //   status: 0=discharging 1=recharging 2=charge_complete
    it = m_featureMap.find(FeatureId::BATTERY_STATUS);
    if (it != m_featureMap.end()) {
        auto r = request(it->second, 0x00, {}, devIdx);
        if (r && r->bytes.size() > 5 && r->param(0) <= 100) {
            m_batteryPct = r->param(0);
            uint8_t status = r->param(2);
            m_isCharging = (status == 1 || status == 2);
            return true;
        }
    }
    return false;
}

void HidppDevice::resolveHapticFeature(uint8_t devIdx) {
    m_hapticFeatureIdx  = 0;
    m_hapticUsesShort   = false;

    // 0x19B0 is the confirmed haptic feature for MX Master 4.
    // Protocol: SHORT report, function 4, params [effectId, 0, 0].
    static constexpr struct { uint16_t id; bool shortReport; } CANDIDATES[] = {
        { 0x19B0, true  },  // MX Master 4 confirmed
        { 0x9401, false },  // older ForceControl
        { 0x9402, false },
        { 0x9300, false },
        { 0x8134, false },
        { 0x8123, false },
    };

    for (auto& c : CANDIDATES) {
        if (!hasFeature(c.id)) continue;
        uint8_t idx = featureIndex(c.id);
        m_hapticFeatureIdx = idx;
        m_hapticUsesShort  = c.shortReport;
        return;
    }

    (void)devIdx;
}

void HidppDevice::startNotifications() {
    if (m_notifyRunning) return;

    // ── Thumb wheel (feature 0x2150) ──────────────────────────────────────────
    m_thumbWheelFeatureIdx = 0;
    auto twIt = m_featureMap.find(FeatureId::THUMB_WHEEL);
    if (twIt != m_featureMap.end())
        m_thumbWheelFeatureIdx = twIt->second;

    // ── Gesture button via REPROG_CONTROLS_V4 (feature 0x1B04) ───────────────
    // The MX Master 4 gesture button (CID 0x00C4) is a "virtual" button that
    // generates no Windows mouse events.  Diverting it via HID++ makes the
    // firmware send spontaneous LONG-report notifications we can intercept.
    m_reprogCtrlFeatureIdx = 0;
    m_divertedCIDs.clear();
    auto rcIt = m_featureMap.find(FeatureId::REPROG_CONTROLS_V4);
    if (rcIt != m_featureMap.end()) {
        m_reprogCtrlFeatureIdx = rcIt->second;

        // Known gesture/smart-button CIDs for MX Master 4 / 3 / similar.
        // Diverting these makes them emit HID++ press events instead of (or in
        // addition to) any default action.  If a CID doesn't exist on this
        // device the request fails silently.
        static constexpr uint16_t GESTURE_CIDS[] = {
            0x00C4,   // Gesture button   (MX Master 3 / 4 confirmed)
            0x00C3,   // Smart button     (some variants)
        };
        for (uint16_t cid : GESTURE_CIDS) {
            uint8_t cidH = (cid >> 8) & 0xFF;
            uint8_t cidL = cid & 0xFF;
            // setKeyReporting (function 3): param[2] bit 0 = temporarily diverted
            auto r = request(m_reprogCtrlFeatureIdx, 0x03, {cidH, cidL, 0x01}, m_devIdx);
            if (r) m_divertedCIDs.push_back(cid);
        }
    }

    // ── Battery change notifications (BATTERY_UNIFIED 0x1004) ───────────────────
    // Device sends spontaneous HID++ events when battery level or charging state
    // changes (e.g., charger plugged in). SW_ID=0 identifies spontaneous packets.
    m_batteryNotifyFeatureIdx = 0;
    {
        auto batIt = m_featureMap.find(FeatureId::BATTERY_UNIFIED);
        if (batIt != m_featureMap.end())
            m_batteryNotifyFeatureIdx = batIt->second;
    }

    // Need at least one notification source to justify opening the handle
    if (m_thumbWheelFeatureIdx == 0 && m_reprogCtrlFeatureIdx == 0
            && m_batteryNotifyFeatureIdx == 0)
        return;

    // Open a second independent handle to the same long-report endpoint.
    // On Windows, each HID file handle has its own read queue — both handles
    // receive full copies of all incoming packets from the device.
    if (!m_hidNotify.open(m_path)) {
        m_thumbWheelFeatureIdx    = 0;
        m_reprogCtrlFeatureIdx    = 0;
        m_batteryNotifyFeatureIdx = 0;
        return;
    }

    m_notifyRunning = true;
    m_notifyThread  = std::thread(&HidppDevice::notificationThreadFn, this);
}

void HidppDevice::stopNotifications() {
    if (!m_notifyRunning) return;

    // Un-divert gesture buttons so the firmware restores their default behaviour
    if (m_reprogCtrlFeatureIdx != 0) {
        for (uint16_t cid : m_divertedCIDs) {
            uint8_t cidH = (cid >> 8) & 0xFF;
            uint8_t cidL = cid & 0xFF;
            request(m_reprogCtrlFeatureIdx, 0x03, {cidH, cidL, 0x00}, m_devIdx);
        }
        m_divertedCIDs.clear();
        m_reprogCtrlFeatureIdx = 0;
    }

    m_notifyRunning = false;
    m_hidNotify.close();   // interrupts the blocking read() in the thread
    if (m_notifyThread.joinable()) m_notifyThread.join();
    m_thumbWheelFeatureIdx = 0;
}

void HidppDevice::notificationThreadFn() {
    std::vector<uint8_t> buf;
    while (m_notifyRunning) {
        buf.clear();
        // 30 ms timeout keeps the loop responsive to stop requests
        if (!m_hidNotify.read(buf, 30)) continue;
        if (buf.size() < 6) continue;

        // Only HID++ reports from our paired device
        if (buf[0] != REPORT_LONG && buf[0] != REPORT_SHORT) continue;
        if (buf[1] != m_devIdx) continue;

        // ── Thumb wheel motion (feature 0x2150) ───────────────────────────────
        if (m_thumbWheelFeatureIdx != 0 && buf[2] == m_thumbWheelFeatureIdx) {
            // bytes[4-5]: signed 16-bit big-endian delta
            int16_t delta = (int16_t)(((uint16_t)buf[4] << 8) | buf[5]);
            if (delta != 0 && m_thumbWheelCb)
                m_thumbWheelCb(delta);
            continue;
        }

        // ── Gesture button press (feature 0x1B04) ─────────────────────────────
        // Packet layout for spontaneous key-divert notification:
        //   buf[3]  = 0x00 (SW_ID=0 means spontaneous — not a response to our request)
        //   buf[4]  = CID high byte
        //   buf[5]  = CID low byte
        //   buf[6]  = virtual key index (unused)
        //   buf[7]  = active flags  (bit 0 = 1 → pressed, 0 → released)
        if (m_reprogCtrlFeatureIdx != 0 && buf[2] == m_reprogCtrlFeatureIdx) {
            // SW_ID must be 0 (spontaneous notification, not a response echo)
            if ((buf[3] & 0x0F) == 0 && buf.size() >= 8) {
                bool pressed = (buf[7] & 0x01) != 0;
                if (pressed && m_gestureButtonCb) {
                    uint16_t cid = ((uint16_t)buf[4] << 8) | buf[5];
                    m_gestureButtonCb(cid);
                }
            }
            continue;
        }

        // ── Battery level / charge-state change (BATTERY_UNIFIED 0x1004) ──────
        // Device sends this spontaneously when battery level changes or when the
        // charging cable is plugged/unplugged. SW_ID=0 distinguishes these events
        // from responses to our own requests.
        //   buf[4] = level (0-100)
        //   buf[5] = nextLevel
        //   buf[6] = batteryStatus (0=discharging 1=recharging 2=almost_full 3=full)
        //   buf[7] = externalPower (0=no cable 1=cable present)
        if (m_batteryNotifyFeatureIdx != 0 && buf[2] == m_batteryNotifyFeatureIdx) {
            if ((buf[3] & 0x0F) == 0 && buf.size() >= 8) {
                uint8_t level    = buf[4];
                uint8_t status   = buf[6];
                uint8_t external = buf[7];
                if (level <= 100) {
                    m_batteryPct = level;
                    m_isCharging = (external == 1) || (status >= 1 && status <= 4);
                    if (m_batteryNotifyCb) m_batteryNotifyCb(m_batteryPct, m_isCharging);
                }
            }
            continue;
        }
    }
}

void HidppDevice::disconnect() {
    stopNotifications();
    m_hid.close();
    m_hidShort.close();
    m_connected               = false;
    m_featureMap.clear();
    m_batteryPct              = -1;
    m_isCharging              = false;
    m_batteryNotifyFeatureIdx = 0;
    m_hapticFeatureIdx        = 0;
}

bool HidppDevice::isConnected() const { return m_connected && m_hid.isOpen(); }
bool HidppDevice::hasFeature(uint16_t id) const { return m_featureMap.count(id) > 0; }
uint8_t HidppDevice::featureIndex(uint16_t id) const {
    auto it = m_featureMap.find(id);
    return it != m_featureMap.end() ? it->second : 0;
}

std::optional<Packet> HidppDevice::request(uint8_t featIdx, uint8_t func,
                                             std::initializer_list<uint8_t> params,
                                             uint8_t devIdx,
                                             int timeoutMs,
                                             int maxAttempts) {
    if (!m_hid.isOpen()) return std::nullopt;

    // Drain stale input reports that accumulated in m_hid's private read queue.
    // Windows delivers copies of every incoming HID++ packet to all open file
    // handles for the same endpoint — including m_hid even though the notification
    // thread already consumed the same packets from m_hidNotify. Without this flush
    // the read loop below reads old thumb-wheel / gesture events instead of our
    // response, causing the request to silently time out.
    {
        std::vector<uint8_t> stale;
        while (m_hid.read(stale, 0) && !stale.empty()) {}
    }

    auto pkt = Packet::makeLong(devIdx, featIdx, func, params);
    if (!m_hid.write(pkt.bytes)) return std::nullopt;

    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        std::vector<uint8_t> buf;
        if (!m_hid.read(buf, timeoutMs)) return std::nullopt;
        if (buf.size() < 4) continue;  // timeout (0 bytes) – keep retrying

        Packet resp;
        resp.bytes = std::move(buf);

        if (resp.isError()) {
            // bytes[4] = funcByte from request, bytes[5] = errCode
            uint8_t respFuncByte = resp.bytes.size()>4 ? resp.bytes[4] : 0;
            uint8_t ourFuncByte  = static_cast<uint8_t>((func << 4) | SW_ID);
            if (respFuncByte == ourFuncByte) {
                return std::nullopt;
            }
            continue;  // stale error from prior request – skip
        }

        if (resp.deviceIndex()  == devIdx &&
            resp.featureIndex() == featIdx) {
            return resp;
        }
    }
    return std::nullopt;
}

bool HidppDevice::playHaptic(Waveform wf) {
    if (!m_connected || m_hapticFeatureIdx == 0) return false;

    uint8_t effectId = static_cast<uint8_t>(wf);  // effect IDs 0-14 match Waveform enum
    uint8_t intensity = 100; // Default and only used intensity level

    if (m_hapticUsesShort) {
        // MX Master 4: feature 0x19B0, function 4, SHORT report
        // bytes[4]=effectId  bytes[5]=amplitude(0-100)  bytes[6]=0
        HidDevice& hid = m_hidShort.isOpen() ? m_hidShort : m_hid;
        auto pkt = Packet::makeShort(m_devIdx, m_hapticFeatureIdx, 4, {effectId, intensity, 0});
        if (!hid.write(pkt.bytes)) {
            return false;
        }
        // Fire-and-forget: no ack read — avoids blocking the haptic thread
        return true;
    }

    // Long-report path for legacy devices
    auto r = request(m_hapticFeatureIdx, 0x00,
                     {effectId, 0x00, 0x01}, m_devIdx, 200, 5);
    return r.has_value();
}

bool HidppDevice::refreshBattery() { return fetchBattery(m_devIdx); }

std::vector<std::pair<uint16_t, uint8_t>> HidppDevice::enumerateFeatures() {
    std::vector<std::pair<uint16_t, uint8_t>> out;
    for (auto& [k, v] : m_featureMap) out.emplace_back(k, v);
    return out;
}

} // namespace hidpp

