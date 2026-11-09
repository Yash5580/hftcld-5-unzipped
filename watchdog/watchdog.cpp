#include "watchdog.h"
#include "../logger/logger.h"
#include <cstdlib>
#include <thread>
#include <cstdio>

Watchdog::Watchdog(const WatchdogCfg& cfg, const std::string& bot_path)
    : cfg_(cfg), bot_path_(bot_path) {}

void Watchdog::run() {
    int restarts = 0;
    while (restarts <= cfg_.max_restarts) {
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "Watchdog: launching %s (attempt %d/%d)",
            bot_path_.c_str(), restarts + 1, cfg_.max_restarts + 1);
        LOG_SYSTEM(buf);

        int ret = std::system(bot_path_.c_str());

        if (ret == 0) {
            LOG_SYSTEM("Watchdog: arb_bot exited cleanly (code 0). Stopping watchdog.");
            break;
        }

        std::snprintf(buf, sizeof(buf),
            "Watchdog: arb_bot exited with code %d", ret);
        LOG_RISK(buf);

        if (restarts >= cfg_.max_restarts) {
            LOG_RISK("Watchdog: max restarts reached. Manual intervention required.");
            break;
        }

        int delay_ms = cfg_.backoff_ms[std::min(restarts, 2)];
        std::snprintf(buf, sizeof(buf),
            "Watchdog: restarting in %d ms...", delay_ms);
        LOG_SYSTEM(buf);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        ++restarts;
    }
}
