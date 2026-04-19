#pragma once
#include "haptics/HapticController.h"
#include <string>

namespace config {

class Config {
public:
    Config();

    void load();
    void save() const;

    haptics::HapticConfig haptic;

private:
    std::wstring configPath() const;
};

} // namespace config
