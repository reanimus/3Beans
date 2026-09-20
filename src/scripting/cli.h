#pragma once
#include <map>
#include <string>
#include <vector>
#include "session.h"

struct LaunchOptions {
    bool headless = false, help = false;
    double timeout = 0;
    std::string configDir;
    std::vector<std::string> scripts;
    std::map<std::string, std::string> paths;
    void parse(int argc, char** argv);
    void apply(ScriptSession& session) const;
};
extern LaunchOptions launchOptions;
std::string defaultConfigDir();
int runHeadless(const LaunchOptions& options);
const char* commandHelp();
