#pragma once

#include <string>
#include <memory>
#include "settings.h"

class FirmImage;

// A boot owns a snapshot of its paths. Session overrides never enter Settings.
struct BootConfig {
    std::string sd, nand, boot9, boot11, stateDirectory;
    std::shared_ptr<const FirmImage> firm;
    bool headless = false;
    bool forceSoftware = false;
    bool audioPacing = true;
    BootConfig(): sd(Settings::sdPath), nand(Settings::nandPath),
        boot9(Settings::boot9Path), boot11(Settings::boot11Path), stateDirectory(Settings::basePath) {}
};
