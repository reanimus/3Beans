#include "cli.h"
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <stdexcept>

LaunchOptions launchOptions;
const char* commandHelp() {
    return "Usage: 3beans [--headless] [--script file.lua] [--firm homebrew.firm] [--sd image] [--nand image]\n"
           "              [--boot9 file] [--boot11 file] [--timeout seconds] [--config-dir directory]\n"
           "Scripts run in order. Headless scripts explicitly start and advance emulation.\n"
           "--firm selects direct homebrew boot; desktop starts it automatically without scripts.\n"
           "Boot paths override saved settings for this process only, taking effect on start/reset.\n";
}
void LaunchOptions::parse(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            help = true;
            continue;
        }
        if (arg != "--script" && arg != "--sd" && arg != "--nand" && arg != "--boot9" && arg != "--boot11" &&
            arg != "--firm" && arg != "--timeout" && arg != "--config-dir")
            throw std::runtime_error("Unknown option: " + arg);
        if (++i == argc)
            throw std::runtime_error("Missing value for " + arg);
        std::string value = argv[i];
        if (arg == "--script")
            scripts.push_back(ScriptSession::absolutePath(value));
        else if (arg == "--config-dir")
            configDir = ScriptSession::absolutePath(value);
        else if (arg == "--timeout") {
            size_t used = 0;
            timeout = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(timeout) || timeout <= 0 || timeout > 86400)
                throw std::runtime_error("Timeout must be greater than zero and at most 86400 seconds");
        } else
            paths[arg.substr(2)] = ScriptSession::absolutePath(value);
    }
    if (headless && scripts.empty() && !help)
        throw std::runtime_error("--headless requires at least one --script");
}
void LaunchOptions::apply(ScriptSession& session) const {
    for (auto& p : paths)
        session.setPath(p.first, p.second);
    if (headless)
        session.setTimeout(timeout);
}
std::string defaultConfigDir() {
    if (FILE* f = fopen("3beans.ini", "r")) {
        fclose(f);
        return ".";
    }
    auto env = [](const char* key) {
        const char* value = getenv(key);
        return value ? std::string(value) : std::string();
    };
#ifdef WINDOWS
    return env("APPDATA") + "/3Beans";
#elif defined(MACOS)
    return env("HOME") + "/Library/Application Support/3Beans";
#else
    std::string base = env("XDG_CONFIG_HOME");
    if (base.empty())
        base = env("HOME") + "/.config";
    return base + "/3beans";
#endif
}
int runHeadless(const LaunchOptions& options) {
    try {
        Settings::load(options.configDir.empty() ? defaultConfigDir() : options.configDir, false);
        ScriptSession session(true);
        options.apply(session);
        for (auto& script : options.scripts)
            if (!session.runFile(script))
                return 1;
        session.stop();
        return session.failed() ? 1 : 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
