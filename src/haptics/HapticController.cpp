#include "HapticController.h"
#include <Shlobj.h>
#include <chrono>
#include <fstream>
#include <cstring>

namespace haptics {

HapticController::HapticController() = default;
HapticController::~HapticController() { stop(); }

void HapticController::start() {
    m_running = true;
    m_managerThread = std::thread(&HapticController::managerThreadFn, this);
    m_hapticThread  = std::thread(&HapticController::hapticThreadFn,  this);
    SetThreadPriority(m_hapticThread.native_handle(), THREAD_PRIORITY_HIGHEST);
}

void HapticController::stop() {
    m_running = false;
    m_cv.notify_all();
    if (m_hapticThread.joinable())  m_hapticThread.join();
    if (m_managerThread.joinable()) m_managerThread.join();
}

std::wstring HapticController::deviceName() const {
    std::lock_guard<std::mutex> lk(m_nameMutex);
    return m_deviceName;
}

void HapticController::play(Waveform wf) {
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        while (!m_queue.empty()) m_queue.pop();
        m_queue.push({wf});
    }
    m_cv.notify_one();
}

void HapticController::playScroll()       { m_scrollPending.store(true, std::memory_order_relaxed);      m_cv.notify_one(); }
void HapticController::playSideScroll()  { m_sideScrollPending.store(true, std::memory_order_relaxed);  m_cv.notify_one(); }
void HapticController::playLeftClick()   { m_leftClickPending.store(true, std::memory_order_relaxed);   m_cv.notify_one(); }
void HapticController::playRightClick()  { m_rightClickPending.store(true, std::memory_order_relaxed);  m_cv.notify_one(); }
void HapticController::playSideButton()  { m_sideButtonPending.store(true, std::memory_order_relaxed);  m_cv.notify_one(); }
void HapticController::playScrollClick() { m_scrollClickPending.store(true, std::memory_order_relaxed); m_cv.notify_one(); }

void HapticController::checkHover() {
    if (!m_config.hoverEnabled) return;

    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING)) return;

    // Method 1: Cursor shape (links, web, hand pointer)
    if (!m_handCursor) m_handCursor = LoadCursorW(nullptr, IDC_HAND);
    bool cursorIsHand = (ci.hCursor == m_handCursor);

    // Method 2: IUIAutomation (primary – works on taskbar, modern apps, classic apps)
    bool onInteractive = false;
    RECT curElemRect   = {};
    if (m_pUIA) {
        IUIAutomationElement* pElem = nullptr;
        POINT pt = ci.ptScreenPos;
        if (SUCCEEDED(m_pUIA->ElementFromPoint(pt, &pElem)) && pElem) {
            CONTROLTYPEID ctrlType = 0;
            pElem->get_CurrentControlType(&ctrlType);
            onInteractive = (ctrlType == UIA_ButtonControlTypeId         ||
                             ctrlType == UIA_CheckBoxControlTypeId       ||
                             ctrlType == UIA_ComboBoxControlTypeId       ||
                             ctrlType == UIA_HyperlinkControlTypeId      ||
                             ctrlType == UIA_ListItemControlTypeId       ||
                             ctrlType == UIA_MenuItemControlTypeId       ||
                             ctrlType == UIA_RadioButtonControlTypeId    ||
                             ctrlType == UIA_TabItemControlTypeId        ||
                             ctrlType == UIA_TreeItemControlTypeId       ||
                             ctrlType == UIA_DataItemControlTypeId       ||
                             ctrlType == UIA_SplitButtonControlTypeId    ||
                             ctrlType == UIA_HeaderItemControlTypeId);
            if (onInteractive) {
                RECT r = {};
                if (SUCCEEDED(pElem->get_CurrentBoundingRectangle(&r)))
                    curElemRect = r;
            }
            pElem->Release();
        }
    }

    // Method 2b: MSAA fallback (for apps where UIA is unavailable)
    if (!onInteractive) {
        IAccessible* pAcc = nullptr;
        VARIANT varChild;
        VariantInit(&varChild);
        if (SUCCEEDED(AccessibleObjectFromPoint(ci.ptScreenPos, &pAcc, &varChild))) {
            VARIANT varRole; VariantInit(&varRole);
            if (SUCCEEDED(pAcc->get_accRole(varChild, &varRole)) && varRole.vt == VT_I4) {
                long role = varRole.lVal;
                onInteractive = (role == ROLE_SYSTEM_LINK          ||
                                 role == ROLE_SYSTEM_PUSHBUTTON    ||
                                 role == ROLE_SYSTEM_MENUITEM      ||
                                 role == ROLE_SYSTEM_LISTITEM      ||
                                 role == ROLE_SYSTEM_OUTLINEITEM   ||
                                 role == ROLE_SYSTEM_PAGETAB       ||
                                 role == ROLE_SYSTEM_CHECKBUTTON   ||
                                 role == ROLE_SYSTEM_RADIOBUTTON   ||
                                 role == ROLE_SYSTEM_COMBOBOX      ||
                                 role == ROLE_SYSTEM_COLUMNHEADER  ||
                                 role == ROLE_SYSTEM_CELL          ||
                                 role == ROLE_SYSTEM_BUTTONMENU    ||
                                 role == ROLE_SYSTEM_BUTTONDROPDOWN||
                                 role == ROLE_SYSTEM_SPLITBUTTON   ||
                                 role == ROLE_SYSTEM_TOOLBAR       ||
                                 role == ROLE_SYSTEM_DROPLIST      ||
                                 role == ROLE_SYSTEM_SPINBUTTON);
            }
            VariantClear(&varRole);
            if (onInteractive) {
                LONG x, y, w, h;
                if (SUCCEEDED(pAcc->accLocation(&x, &y, &w, &h, varChild)))
                    curElemRect = {x, y, x + w, y + h};
            }
            pAcc->Release();
        }
        VariantClear(&varChild);
    }

    // Method 3: NC hit-test for window chrome buttons (close/min/max)
    HWND hwndUnder = WindowFromPoint(ci.ptScreenPos);
    LRESULT hitTest = HTNOWHERE;
    if (hwndUnder) {
        DWORD_PTR result = 0;
        if (SendMessageTimeout(hwndUnder, WM_NCHITTEST, 0,
                               MAKELPARAM(ci.ptScreenPos.x, ci.ptScreenPos.y),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, 10, &result))
            hitTest = (LRESULT)result;
    }

    // Only fire if the window under the cursor belongs to the foreground app,
    // OR if it is the taskbar (always interactive regardless of focus).
    if (m_config.hoverOnlyFocused) {
        HWND rootUnder = hwndUnder ? GetAncestor(hwndUnder, GA_ROOT) : nullptr;
        HWND fg        = GetForegroundWindow();
        bool isTaskbar = false;
        if (rootUnder) {
            wchar_t cls[64] = {};
            GetClassNameW(rootUnder, cls, 64);
            isTaskbar = (wcscmp(cls, L"Shell_TrayWnd")          == 0 ||
                         wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0);
        }
        if (rootUnder != fg && !isTaskbar) {
            m_wasHovering = false;
            m_lastElemRect = {}; m_lastNcRegion = HTNOWHERE;
            return;
        }
    }
    bool isNcInteractive = (hitTest == HTCLOSE    || hitTest == HTMINBUTTON ||
                            hitTest == HTMAXBUTTON || hitTest == HTHELP     ||
                            hitTest == HTSYSMENU);

    bool isHovering = cursorIsHand || onInteractive || isNcInteractive;

    // Detect element change (to fire per-element, not just on enter/exit).
    // For UIA/MSAA elements we check whether the CURSOR has moved outside the
    // previously fired element's rect rather than comparing rects directly.
    // Animated elements (hover-highlight, scrolling lists, etc.) can shift their
    // reported bounds by 1-2 px between polls, causing false re-triggers that feel
    // like a "harder" or doubled haptic.
    // Element-change detection uses spatial methods (UIA/MSAA rect, NC region) as primary.
    // cursorIsHand acts as FALLBACK for elements that UIA/MSAA miss (some web <div> types).
    // It fires only on the not-hand→hand transition to prevent double haptics when both
    // spatial and cursor detection succeed on the same element.
    ULONGLONG now = GetTickCount64();

    bool elementChanged = false;
    if (onInteractive) {
        bool outsidePrev = (m_lastElemRect.right <= m_lastElemRect.left) ||
                           (ci.ptScreenPos.x <  m_lastElemRect.left)    ||
                           (ci.ptScreenPos.x >= m_lastElemRect.right)   ||
                           (ci.ptScreenPos.y <  m_lastElemRect.top)     ||
                           (ci.ptScreenPos.y >= m_lastElemRect.bottom);
        elementChanged = outsidePrev && (curElemRect.right > curElemRect.left);
    } else if (isNcInteractive) {
        elementChanged = (hitTest != m_lastNcRegion);
    } else if (cursorIsHand && !m_lastCursorWasHand) {
        // Fallback: cursor changed to hand but UIA/MSAA found nothing — fire once
        elementChanged = true;
    }

    // Spatial+temporal dedup across detection methods:
    // If a haptic already fired within 250ms AND the cursor hasn't moved more
    // than 20px, a different method (cursor-hand vs UIA/MSAA) is detecting the
    // same element — suppress the duplicate.
    if (elementChanged && (now - m_lastHoverFireTick < 250)) {
        int dx = ci.ptScreenPos.x - m_lastHoverFirePos.x;
        int dy = ci.ptScreenPos.y - m_lastHoverFirePos.y;
        if (dx * dx + dy * dy < 20 * 20)
            elementChanged = false;
    }

    bool      coolOk = (now - m_lastHoverFireTick >= m_config.hoverCooldownMs);

    auto fireHaptic = [&]() {
        m_device.playHaptic(m_config.hoverWaveform);
        m_lastHoverFireTick = now;
        m_lastHoverFirePos  = ci.ptScreenPos;
        m_lastNcRegion      = hitTest;
        if (curElemRect.right > curElemRect.left) {
            m_lastElemRect = curElemRect;
        } else {
            m_lastElemRect = { ci.ptScreenPos.x - 4, ci.ptScreenPos.y - 4,
                               ci.ptScreenPos.x + 4, ci.ptScreenPos.y + 4 };
        }
    };

    switch (m_config.hoverMode) {
    case HoverMode::Enter:
        if (isHovering && elementChanged && coolOk) fireHaptic();
        break;
    case HoverMode::Exit:
        if (m_wasHovering && !isHovering) {
            m_device.playHaptic(m_config.hoverWaveform);
            m_lastElemRect = {}; m_lastNcRegion = HTNOWHERE;
        }
        break;
    case HoverMode::Both:
        if (isHovering && elementChanged && coolOk) fireHaptic();
        else if (m_wasHovering && !isHovering) {
            m_device.playHaptic(m_config.hoverWaveform);
            m_lastElemRect = {}; m_lastNcRegion = HTNOWHERE;
        }
        break;
    }

    m_wasHovering = isHovering;
    m_lastCursorWasHand = cursorIsHand;
    if (!isHovering) { m_lastElemRect = {}; m_lastNcRegion = HTNOWHERE; }
}

void HapticController::hapticThreadFn() {
    using namespace std::chrono_literals;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Create IUIAutomation instance for hover detection
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IUIAutomation, reinterpret_cast<void**>(&m_pUIA));

    while (m_running) {
        {
            std::unique_lock<std::mutex> lock(m_cvMutex);
            m_cv.wait_for(lock, 20ms, [this] {
                return !m_running ||
                       m_scrollPending.load(std::memory_order_relaxed)      ||
                       m_sideScrollPending.load(std::memory_order_relaxed)  ||
                       m_leftClickPending.load(std::memory_order_relaxed)   ||
                       m_rightClickPending.load(std::memory_order_relaxed)  ||
                       m_sideButtonPending.load(std::memory_order_relaxed)  ||
                       m_scrollClickPending.load(std::memory_order_relaxed);
            });
        }
        if (!m_running) break;
        if (!m_connected.load(std::memory_order_acquire)) continue;

        // UI preview queue
        {
            std::queue<PlayCommand> local;
            { std::lock_guard<std::mutex> lk(m_queueMutex); std::swap(local, m_queue); }
            while (!local.empty()) {
                m_device.playHaptic(local.front().wf);
                local.pop();
            }
        }

        // Vertical scroll — optional cooldown
        if (m_scrollPending.exchange(false, std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            bool cdOk = (m_config.scrollCooldownMs == 0 ||
                         now - m_lastScrollFireTick >= m_config.scrollCooldownMs);
            if (m_config.scrollEnabled && cdOk) {
                m_device.playHaptic(m_config.scrollWaveform);
                m_lastScrollFireTick = now;
            }
        }

        // Side / lateral scroll wheel — own cooldown gate
        if (m_sideScrollPending.exchange(false, std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            bool cdOk = (m_config.sideScrollCooldownMs == 0 ||
                         now - m_lastSideScrollFireTick >= m_config.sideScrollCooldownMs);
            if (m_config.sideScrollEnabled && cdOk) {
                m_device.playHaptic(m_config.sideScrollWaveform);
                m_lastSideScrollFireTick = now;
            }
        }

        // Left click — with per-feature cooldown
        if (m_leftClickPending.exchange(false, std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            bool cdOk = (m_config.clickCooldownMs == 0 ||
                         now - m_lastClickFireTick >= m_config.clickCooldownMs);
            if (m_config.clickEnabled && cdOk) {
                m_device.playHaptic(m_config.clickWaveform);
                m_lastClickFireTick = now;
            }
        }

        // Right click — with per-feature cooldown
        if (m_rightClickPending.exchange(false, std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            bool cdOk = (m_config.rightClickCooldownMs == 0 ||
                         now - m_lastRightClickFireTick >= m_config.rightClickCooldownMs);
            if (m_config.rightClickEnabled && cdOk) {
                m_device.playHaptic(m_config.rightClickWaveform);
                m_lastRightClickFireTick = now;
            }
        }

        // Side buttons (forward / back) — with per-feature cooldown
        if (m_sideButtonPending.exchange(false, std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            bool cdOk = (m_config.sideButtonCooldownMs == 0 ||
                         now - m_lastSideButtonFireTick >= m_config.sideButtonCooldownMs);
            if (m_config.sideButtonEnabled && cdOk) {
                m_device.playHaptic(m_config.sideButtonWaveform);
                m_lastSideButtonFireTick = now;
            }
        }

        // Scroll click (middle mouse button) — with per-feature cooldown
        if (m_scrollClickPending.exchange(false, std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            bool cdOk = (m_config.scrollClickCooldownMs == 0 ||
                         now - m_lastScrollClickFireTick >= m_config.scrollClickCooldownMs);
            if (m_config.scrollClickEnabled && cdOk) {
                m_device.playHaptic(m_config.scrollClickWaveform);
                m_lastScrollClickFireTick = now;
            }
        }

        // Hover (polled every 3 iterations ≈ 60 ms).
        // checkHover() can block for 50-200 ms on slow UIA/MSAA calls.
        // After it returns, immediately re-process any scroll/sideScroll events
        // that arrived while the thread was blocked inside checkHover.
        if (++m_hoverCounter >= 3) {
            m_hoverCounter = 0;
            checkHover();

            // Re-dispatch: events that arrived while we were blocked inside checkHover
            if (m_scrollPending.exchange(false, std::memory_order_relaxed)) {
                ULONGLONG now = GetTickCount64();
                bool cdOk = (m_config.scrollCooldownMs == 0 ||
                             now - m_lastScrollFireTick >= m_config.scrollCooldownMs);
                if (m_config.scrollEnabled && cdOk) {
                    m_device.playHaptic(m_config.scrollWaveform);
                    m_lastScrollFireTick = now;
                }
            }
            if (m_sideScrollPending.exchange(false, std::memory_order_relaxed)) {
                ULONGLONG now = GetTickCount64();
                bool cdOk = (m_config.sideScrollCooldownMs == 0 ||
                             now - m_lastSideScrollFireTick >= m_config.sideScrollCooldownMs);
                if (m_config.sideScrollEnabled && cdOk) {
                    m_device.playHaptic(m_config.sideScrollWaveform);
                    m_lastSideScrollFireTick = now;
                }
            }
        }
    }

    if (m_pUIA) { m_pUIA->Release(); m_pUIA = nullptr; }
    CoUninitialize();
}

void HapticController::managerThreadFn() {
    using namespace std::chrono_literals;

    bool    wasConnected = false;
    int64_t lastBatTick  = static_cast<int64_t>(GetTickCount64()) - 60000LL;

    while (m_running) {
        if (!m_device.isConnected()) {
            m_connected.store(false, std::memory_order_release);
            bool ok = m_device.connect();
            if (ok) {
                m_hapticsSupported = m_device.hasHapticSupport();
                m_batteryPct       = m_device.batteryPct();
                { std::lock_guard<std::mutex> lk(m_nameMutex); m_deviceName = m_device.deviceName(); }
                m_connected.store(true, std::memory_order_release);
                if (!wasConnected) {
                    wasConnected = true;
                    // Wire thumb wheel → sideScroll haptic via HID++ feature 0x2150.
                    // This bypasses Logitech Options+ app-specific overrides and fires
                    // once per physical notch (no sub-delta spam).
                    m_device.setThumbWheelCallback([this](int /*delta*/) {
                        m_sideScrollPending.store(true, std::memory_order_relaxed);
                        m_cv.notify_one();
                    });
                    // Gesture button (CID 0x00C4) is a virtual side button on
                    // MX Master 4 — not detectable via WH_MOUSE_LL, only via
                    // HID++ REPROG_CONTROLS_V4 divert notifications.
                    m_device.setGestureButtonCallback([this](uint16_t /*cid*/) {
                        m_sideButtonPending.store(true, std::memory_order_relaxed);
                        m_cv.notify_one();
                    });
                    // Wire real-time battery notifications from the device so that
                    // plugging/unplugging the charger is reflected immediately.
                    m_device.setBatteryCallback([this](int pct, bool charging) {
                        m_batteryPct.store(pct);
                        m_isCharging.store(charging);
                        if (m_onBattery) m_onBattery(pct, charging);
                    });
                    m_device.startNotifications();
                    if (m_onConnect) m_onConnect(true, m_device.deviceName());
                    if (m_onBattery && m_device.batteryPct() >= 0)
                        m_onBattery(m_device.batteryPct(), m_device.isCharging());
                    m_device.playHaptic(Waveform::Knock);
                }
                lastBatTick = static_cast<int64_t>(GetTickCount64());
            } else {
                if (wasConnected) { wasConnected = false; if (m_onConnect) m_onConnect(false, L""); }
                for (int i = 0; i < 20 && m_running; i++) std::this_thread::sleep_for(100ms);
                continue;
            }
        }

        int64_t nowTick = static_cast<int64_t>(GetTickCount64());
        if (nowTick - lastBatTick >= 60000LL) {
            if (m_device.refreshBattery()) {
                m_batteryPct = m_device.batteryPct();
                m_isCharging.store(m_device.isCharging());
                if (m_onBattery) m_onBattery(m_batteryPct, m_device.isCharging());
            }
            lastBatTick = static_cast<int64_t>(GetTickCount64());
        }

        if (!m_device.isConnected()) {
            m_connected.store(false, std::memory_order_release);
            wasConnected = false;
            if (m_onConnect) m_onConnect(false, L"");
        }

        std::this_thread::sleep_for(200ms);
    }

    m_device.disconnect();
    m_connected.store(false, std::memory_order_release);
}

} // namespace haptics
