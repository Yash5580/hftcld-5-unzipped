#include <iostream>
#include "../config/config.h"
#include "../watchdog/watchdog.h"
#include "../alerts/telegram.h"

int main(int argc, char* argv[]) {
    std::string cfg_path  = (argc > 1) ? argv[1] : "config.json";
    std::string bot_path  = (argc > 2) ? argv[2] : "./arb_bot";

    AppConfig cfg;
    try {
        cfg = AppConfig::load(cfg_path);
    } catch (const std::exception& ex) {
        std::cerr << "Config error: " << ex.what() << "\n";
        return 1;
    }

    TelegramAlerter tg(cfg.telegram);
    tg.send("🐶 <b>arb_watchdog started</b> — monitoring arb_bot");

    Watchdog wd(cfg.watchdog, bot_path);
    wd.run();

    tg.send("⚠️ <b>arb_watchdog stopped</b> — max restarts reached or clean exit");
    return 0;
}
