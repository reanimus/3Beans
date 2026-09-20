#pragma once

#include <string>
#include "settings.h"

// A boot owns a snapshot of its paths. Session overrides never enter Settings.
struct BootConfig {
    std::string sd, nand, boot9, boot11, stateDirectory;
    bool headless = false;
    bool forceSoftware = false;
    bool audioPacing = true;
    BootConfig(): sd(Settings::sdPath), nand(Settings::nandPath),
        boot9(Settings::boot9Path), boot11(Settings::boot11Path), stateDirectory(Settings::basePath) {}
};
