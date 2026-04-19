#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include "ui/TrayApp.h"

// Single-instance mutex
static HANDLE g_mutex = nullptr;

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Single instance check
    g_mutex = CreateMutexW(nullptr, TRUE, L"LiveHapticsApp_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"Live Haptics is already running.\n"
                    L"Check the system tray.",
                    L"Live Haptics",
                    MB_ICONINFORMATION | MB_OK);
        return 1;
    }

    // Common controls (needed for some Win32 drawing)
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    // DPI awareness (Windows 10+)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ui::TrayApp app;
    if (!app.init(hInst)) {
        MessageBoxW(nullptr,
                    L"Failed to initialize Live Haptics.\n"
                    L"Please make sure you have the required permissions.",
                    L"Live Haptics",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    int ret = app.run();

    if (g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
    }
    return ret;
}
