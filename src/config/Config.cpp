#include "Config.h"
#include <Windows.h>
#include <Shlobj.h>
#include <fstream>

namespace config {

Config::Config() { load(); }

std::wstring Config::configPath() const {
    wchar_t buf[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf);
    return std::wstring(buf) + L"\\LiveHaptics\\config.ini";
}

static void ensureDir(const std::wstring& filePath) {
    auto pos = filePath.rfind(L'\\');
    if (pos == std::wstring::npos) return;
    CreateDirectoryW(filePath.substr(0, pos).c_str(), nullptr);
}

void Config::load() {
    std::wifstream f(configPath());
    if (!f.is_open()) return;

    std::wstring line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == L'#') continue;
        auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);

        if      (key == L"scrollWaveform")       haptic.scrollWaveform        = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"scrollEnabled")        haptic.scrollEnabled         = (val == L"1");
        else if (key == L"sideScrollWaveform")   haptic.sideScrollWaveform    = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"sideScrollEnabled")    haptic.sideScrollEnabled     = (val == L"1");
        else if (key == L"clickWaveform")        haptic.clickWaveform         = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"clickEnabled")         haptic.clickEnabled        = (val == L"1");
        else if (key == L"rightClickWaveform")   haptic.rightClickWaveform  = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"rightClickEnabled")    haptic.rightClickEnabled   = (val == L"1");
        else if (key == L"hoverWaveform")        haptic.hoverWaveform       = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"hoverEnabled")         haptic.hoverEnabled        = (val == L"1");
        else if (key == L"hoverOnlyFocused")     haptic.hoverOnlyFocused    = (val == L"1");
        else if (key == L"hoverMode")            haptic.hoverMode           = static_cast<haptics::HoverMode>(std::stoi(val));
        else if (key == L"hoverCooldownMs")      haptic.hoverCooldownMs       = static_cast<uint32_t>(std::stoi(val));
        else if (key == L"scrollCooldownMs")     haptic.scrollCooldownMs      = static_cast<uint32_t>(std::stoi(val));
        else if (key == L"sideScrollCooldownMs") haptic.sideScrollCooldownMs  = static_cast<uint32_t>(std::stoi(val));
        else if (key == L"clickCooldownMs")      haptic.clickCooldownMs       = static_cast<uint32_t>(std::stoi(val));
        else if (key == L"rightClickCooldownMs") haptic.rightClickCooldownMs  = static_cast<uint32_t>(std::stoi(val));
        else if (key == L"sideButtonWaveform")    haptic.sideButtonWaveform    = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"sideButtonEnabled")     haptic.sideButtonEnabled     = (val == L"1");
        else if (key == L"sideButtonCooldownMs")  haptic.sideButtonCooldownMs  = static_cast<uint32_t>(std::stoi(val));
        else if (key == L"scrollClickWaveform")   haptic.scrollClickWaveform   = static_cast<hidpp::Waveform>(std::stoi(val));
        else if (key == L"scrollClickEnabled")    haptic.scrollClickEnabled    = (val == L"1");
        else if (key == L"scrollClickCooldownMs") haptic.scrollClickCooldownMs = static_cast<uint32_t>(std::stoi(val));
    }
}

void Config::save() const {
    std::wstring path = configPath();
    ensureDir(path);
    std::wofstream f(path);
    if (!f.is_open()) return;

    f << L"scrollWaveform="      << static_cast<int>(haptic.scrollWaveform)       << L"\n";
    f << L"scrollEnabled="       << (haptic.scrollEnabled    ? L"1" : L"0")      << L"\n";
    f << L"sideScrollWaveform="  << static_cast<int>(haptic.sideScrollWaveform)  << L"\n";
    f << L"sideScrollEnabled="   << (haptic.sideScrollEnabled ? L"1" : L"0")    << L"\n";
    f << L"clickWaveform="       << static_cast<int>(haptic.clickWaveform)       << L"\n";
    f << L"clickEnabled="        << (haptic.clickEnabled   ? L"1" : L"0")        << L"\n";
    f << L"rightClickWaveform="  << static_cast<int>(haptic.rightClickWaveform)  << L"\n";
    f << L"rightClickEnabled="   << (haptic.rightClickEnabled ? L"1" : L"0")    << L"\n";
    f << L"hoverWaveform="       << static_cast<int>(haptic.hoverWaveform)       << L"\n";
    f << L"hoverEnabled="        << (haptic.hoverEnabled   ? L"1" : L"0")        << L"\n";
    f << L"hoverOnlyFocused="    << (haptic.hoverOnlyFocused ? L"1" : L"0")      << L"\n";
    f << L"hoverMode="           << static_cast<int>(haptic.hoverMode)           << L"\n";
    f << L"scrollCooldownMs="     << static_cast<int>(haptic.scrollCooldownMs)     << L"\n";
    f << L"sideScrollCooldownMs="<< static_cast<int>(haptic.sideScrollCooldownMs) << L"\n";
    f << L"clickCooldownMs="     << static_cast<int>(haptic.clickCooldownMs)     << L"\n";
    f << L"rightClickCooldownMs="<< static_cast<int>(haptic.rightClickCooldownMs) << L"\n";
    f << L"hoverCooldownMs="     << static_cast<int>(haptic.hoverCooldownMs)     << L"\n";
    f << L"sideButtonWaveform="   << static_cast<int>(haptic.sideButtonWaveform)   << L"\n";
    f << L"sideButtonEnabled="    << (haptic.sideButtonEnabled  ? L"1" : L"0")    << L"\n";
    f << L"sideButtonCooldownMs=" << static_cast<int>(haptic.sideButtonCooldownMs) << L"\n";
    f << L"scrollClickWaveform="  << static_cast<int>(haptic.scrollClickWaveform)  << L"\n";
    f << L"scrollClickEnabled="   << (haptic.scrollClickEnabled  ? L"1" : L"0")   << L"\n";
    f << L"scrollClickCooldownMs="<< static_cast<int>(haptic.scrollClickCooldownMs)<< L"\n";
}

} // namespace config
