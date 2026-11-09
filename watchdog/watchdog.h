#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// watchdog/watchdog.h
// Spawns arb_bot as a child process and restarts it if it exits unexpectedly.
// Max restarts and backoff delays come from WatchdogCfg.
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include "../config/config.h"

class Watchdog {
public:
    Watchdog(const WatchdogCfg& cfg, const std::string& bot_path);
    void run();  // blocks — restart loop

private:
    const WatchdogCfg& cfg_;
    std::string        bot_path_;
};
