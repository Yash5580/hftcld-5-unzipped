#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// config/config.h
// Single config header — parsed once at startup, const everywhere else.
// Hot-path code reads only primitive members (no string lookups).
//
// FIX #1: IntelligenceCfg moved ABOVE AppConfig (was defined after use →
//         hard compile error: incomplete type as value member).
// FIX #7: ExecCfg::hedge_trigger_ms removed — DynamicHedge replaced the
//         fixed 75ms timer; only hedge_deadline_ms is used anywhere.
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

// ── Angel One credentials ──────────────────────────────────────────────────────
struct AngelCfg {
    std::string api_key;
    std::string client_id;
    std::string password;
    std::string totp_secret;
};

// ── Per-symbol config ──────────────────────────────────────────────────────────
struct SymbolCfg {
    std::string name;
    std::string nse_token;   // e.g. "2885"
    std::string bse_token;   // e.g. "500325"
    int         trade_qty{10};
};

// ── Arb / depth thresholds ─────────────────────────────────────────────────────
struct ArbCfg {
    double min_net_spread_bps{5.0};    // fire signal only if net spread >= this
    double txn_cost_bps{3.0};          // brokerage + STT + exchange charges
    double slippage_bps{2.0};          // estimated market impact (limit orders)
    double market_slippage_bps{5.0};   // extra slippage for hedge market orders
    int    cooldown_ms{500};           // per-symbol cooldown after signal

    // Depth validation (3-layer)
    int    min_qty_best_level{100};    // Layer 2: min shares at best bid/ask
    int    top3_min_cumulative{200};   // Layer 3: sum of top-3 levels each side
};

// ── Execution ──────────────────────────────────────────────────────────────────
// FIX #7: hedge_trigger_ms removed — it was parsed but never read anywhere.
//         DynamicHedge uses hedge_deadline_ms as the hard 100ms fallback only.
struct ExecCfg {
    int hedge_deadline_ms{100}; // hard deadline — force close regardless
};

// ── Risk ───────────────────────────────────────────────────────────────────────
struct RiskCfg {
    double max_daily_loss{10000.0};    // ₹ — halt after this
    int    max_trades_per_day{200};
    int    max_qty_per_symbol{100};
    double max_notional_per_trade{500000.0};
};

// ── WebSocket feed ─────────────────────────────────────────────────────────────
struct FeedCfg {
    int heartbeat_ms{10000};
    int reconnect_ms{2000};
};

// ── Telegram ───────────────────────────────────────────────────────────────────
struct TelegramCfg {
    std::string bot_token;
    std::string chat_id;
    bool        enabled{false};
};

// ── Watchdog ───────────────────────────────────────────────────────────────────
struct WatchdogCfg {
    int max_restarts{3};
    std::array<int,3> backoff_ms{5000, 15000, 30000};
};

// ── Intelligence config ────────────────────────────────────────────────────────
// FIX #1: Defined HERE — must appear before AppConfig uses it as a value member.
struct IntelligenceCfg {
    // OIR
    double oir_min_threshold{0.10};
    int    oir_depth_levels{5};
    // Spread quality
    int    min_cross_ticks{3};
    double spread_decay_max_bps{1.0};
    // Fill probability
    double fill_prob_min{0.60};
    double fill_prob_scale_threshold{0.85};
    double min_qty_scale_factor{0.50};
    // Dynamic hedge
    double price_move_hedge_bps{2.0};
};

// ── Top-level ──────────────────────────────────────────────────────────────────
struct AppConfig {
    AngelCfg                 angel;
    std::vector<SymbolCfg>   symbols;
    ArbCfg                   arb;
    ExecCfg                  exec;
    RiskCfg                  risk;
    FeedCfg                  feed;
    TelegramCfg              telegram;
    WatchdogCfg              watchdog;
    IntelligenceCfg          intelligence; // FIX #1: now complete type above

    static AppConfig load(const std::string& path = "config.json") {
        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open config: " + path);
        nlohmann::json j;
        f >> j;

        AppConfig c;
        c.angel.api_key      = j["angel"]["api_key"];
        c.angel.client_id    = j["angel"]["client_id"];
        c.angel.password     = j["angel"]["password"];
        c.angel.totp_secret  = j["angel"]["totp_secret"];

        for (auto& s : j["symbols"])
            c.symbols.push_back({s["name"], s["nse_token"],
                                  s["bse_token"], s["trade_qty"].get<int>()});

        auto& a = j["arb"];
        c.arb.min_net_spread_bps   = a["min_net_spread_bps"];
        c.arb.txn_cost_bps         = a["txn_cost_bps"];
        c.arb.slippage_bps         = a["slippage_bps"];
        c.arb.market_slippage_bps  = a["market_slippage_bps"];
        c.arb.cooldown_ms          = a["cooldown_ms"];
        c.arb.min_qty_best_level   = a["min_qty_best_level"];
        c.arb.top3_min_cumulative  = a["top3_min_cumulative"];

        // FIX #7: hedge_trigger_ms no longer parsed — field removed from ExecCfg.
        c.exec.hedge_deadline_ms   = j["exec"]["hedge_deadline_ms"];

        auto& r = j["risk"];
        c.risk.max_daily_loss         = r["max_daily_loss"];
        c.risk.max_trades_per_day     = r["max_trades_per_day"];
        c.risk.max_qty_per_symbol     = r["max_qty_per_symbol"];
        c.risk.max_notional_per_trade = r["max_notional_per_trade"];

        c.feed.heartbeat_ms   = j["feed"]["heartbeat_ms"];
        c.feed.reconnect_ms   = j["feed"]["reconnect_ms"];

        c.telegram.bot_token  = j["telegram"]["bot_token"];
        c.telegram.chat_id    = j["telegram"]["chat_id"];
        c.telegram.enabled    = j["telegram"]["enabled"];

        c.watchdog.max_restarts = j["watchdog"]["max_restarts"];
        auto& bd = j["watchdog"]["backoff_ms"];
        c.watchdog.backoff_ms   = {bd[0].get<int>(),
                                    bd[1].get<int>(),
                                    bd[2].get<int>()};

        if (j.contains("intelligence")) {
            auto& i = j["intelligence"];
            c.intelligence.oir_min_threshold         = i.value("oir_min_threshold",         0.10);
            c.intelligence.oir_depth_levels          = i.value("oir_depth_levels",          5);
            c.intelligence.min_cross_ticks           = i.value("min_cross_ticks",           3);
            c.intelligence.spread_decay_max_bps      = i.value("spread_decay_max_bps",      1.0);
            c.intelligence.fill_prob_min             = i.value("fill_prob_min",             0.60);
            c.intelligence.fill_prob_scale_threshold = i.value("fill_prob_scale_threshold", 0.85);
            c.intelligence.min_qty_scale_factor      = i.value("min_qty_scale_factor",      0.50);
            c.intelligence.price_move_hedge_bps      = i.value("price_move_hedge_bps",      2.0);
        }
        return c;
    }
};
