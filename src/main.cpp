// ─────────────────────────────────────────────────────────────────────────────
// src/main.cpp — arb_bot entry point
//
// Boot sequence:
//   1. Load config.json
//   2. Create io_context (single-threaded — all async ops run here)
//   3. Create OrderManager  (paper-fills, DynamicHedge, P&L tracking)
//   4. Create RiskManager   (pre-trade checks, daily loss halt)
//   5. Create ArbEngine     (cross detection, MarketGuard)
//   6. Create SmartStream   (WebSocket feed, login, packet parsing)
//   7. Wire signal callback: ArbEngine → RiskManager → OrderManager
//   8. Wire depth callback:  SmartStream → ArbEngine AND OrderManager
//   9. Install SIGINT/SIGTERM handler for graceful shutdown
//  10. Start io_context on a background thread
//  11. Run SmartStream (blocking loop with reconnect logic)
//  12. On shutdown: save CSV, print summary, send Telegram alert
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>

#include "../config/config.h"
#include "../logger/logger.h"
#include "../strategy/arb_engine.h"
#include "../strategy/order_book.h"
#include "../execution/order_manager.h"
#include "../risk/risk_manager.h"
#include "../alerts/telegram.h"
#include "../feed/smart_stream.h"
#include "../config/price_utils.h"

namespace asio = boost::asio;

// ── Global stop flag — set by signal handler ──────────────────────────────────
static std::atomic<bool> g_stop{false};

static void signal_handler(int) {
    g_stop.store(true);
    LOG_SYSTEM("Shutdown signal received — stopping...");
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const std::string cfg_path = (argc > 1) ? argv[1] : "config.json";

    // ── 1. Load config ────────────────────────────────────────────────────────
    AppConfig cfg;
    try {
        cfg = AppConfig::load(cfg_path);
    } catch (const std::exception& ex) {
        std::cerr << "[FATAL] Config: " << ex.what() << "\n";
        return 1;
    }
    LOG_SYSTEM("arb_bot v5 starting — config loaded from " + cfg_path);
    LOG_SYSTEM("Symbols: " + std::to_string(cfg.symbols.size()) +
               " | min_net_spread: " +
               std::to_string(cfg.arb.min_net_spread_bps) + " bps");

    // ── 2. io_context ─────────────────────────────────────────────────────────
    // Single-threaded: all Boost.Asio callbacks (DynamicHedge timers,
    // heartbeats, WebSocket reads) run on this context.
    asio::io_context ioc;

    // Keep io_context alive even with no pending work
    auto work_guard = asio::make_work_guard(ioc);

    // ── 3. OrderManager ───────────────────────────────────────────────────────
    RiskManager risk(cfg.risk);

    OrderManager order_mgr(cfg, ioc,
        // TradeClosedCb: called after every trade closes
        [&risk](const Trade& t) {
            risk.update_pnl(t.realised_pnl);
        }
    );

    // ── 4. ArbEngine — with signal callback ───────────────────────────────────
    // When ArbEngine detects a valid cross:
    //   (a) RiskManager checks limits
    //   (b) If approved → OrderManager executes
    //   (c) Send Telegram alert
    TelegramAlerter tg(cfg.telegram);

    ArbEngine arb_engine(cfg,
        [&](const ArbSignal& sig) {
            // Pre-trade risk check
            if (!risk.check_signal(sig)) return;
            if (risk.is_halted()) {
                LOG_RISK("Bot halted — ignoring signal");
                return;
            }

            // Execute (paper trade)
            int trade_id = order_mgr.execute(sig);

            // Telegram alert (non-blocking — libcurl timeout = 5s)
            char msg[320];
            std::snprintf(msg, sizeof(msg),
                "🔔 <b>SIGNAL #%d</b> %s\n"
                "Dir: %s\n"
                "Buy: ₹%.2f | Sell: ₹%.2f\n"
                "Net: %.2f bps | Qty: %d\n"
                "Est P&amp;L: ₹%.2f",
                trade_id, sig.symbol,
                (sig.dir == ArbDir::BUY_NSE_SELL_BSE)
                    ? "BUY_NSE / SELL_BSE" : "BUY_BSE / SELL_NSE",
                px_to_rs(sig.buy_px), px_to_rs(sig.sell_px),
                sig.net_bps, sig.qty, sig.est_pnl);
            tg.send(msg);
        }
    );

    // ── 5. Feed callback — wired to BOTH ArbEngine and OrderManager ───────────
    // SmartStream fires on every depth tick (Mode 3, ~50-100 µs per symbol).
    //
    // on_depth:
    //   → ArbEngine::on_depth()     — cross detection
    //   → OrderManager::on_market_tick() — DynamicHedge condition checks
    //   → OrderManager::update_price()  — price cache for hedge market fills
    OnDepthCb on_depth = [&](const std::string& token,
                              uint8_t exchange_type,
                              const DepthBook& book,
                              std::chrono::steady_clock::time_point t0)
    {
        // Feed ArbEngine
        arb_engine.on_depth(token, exchange_type, book, t0);

        // Determine symbol name and exchange string for price cache update
        // We update price cache for both exchanges on every tick.
        // get best bid/ask mid as the current price reference.
        if (book.valid && book.bids[0].price > 0 && book.asks[0].price > 0) {
            // Find symbol name from token
            const char* exch = (exchange_type == 1) ? "NSE" : "BSE";
            for (auto& sym : cfg.symbols) {
                bool match = (exchange_type == 1 && sym.nse_token == token)
                          || (exchange_type == 3 && sym.bse_token == token);
                if (match) {
                    // Use best bid as current price reference for sell-side hedge
                    order_mgr.update_price(sym.name, exch, book.bids[0].price);
                    // Feed DynamicHedge condition monitors
                    // We need both books but SmartStream fires per-tick per-token.
                    // on_market_tick with the live book — uses cached opposite side.
                    // Since we only have one side here, we pass the book as both
                    // and rely on DynamicHedge to use the correct side for its dir.
                    // Full dual-book tick happens when both exchanges update.
                    DepthBook dummy;
                    if (exchange_type == 1)
                        order_mgr.on_market_tick(sym.name, book, dummy);
                    else
                        order_mgr.on_market_tick(sym.name, dummy, book);
                    break;
                }
            }
        }
    };

    // ── 6. SmartStream (feed) ─────────────────────────────────────────────────
    SmartStream feed(cfg, ioc, on_depth);

    // ── 7. Signal handlers ────────────────────────────────────────────────────
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 8. Run io_context on background thread ────────────────────────────────
    std::thread ioc_thread([&ioc]() {
        ioc.run();
    });

    // ── 9. Monitor g_stop and feed stop signal through ────────────────────────
    std::thread stop_monitor([&]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        feed.stop();
        work_guard.reset();
    });

    // ── 10. Startup Telegram alert ────────────────────────────────────────────
    tg.send("🚀 <b>arb_bot v5 started</b>\n"
            "Symbols: " + std::to_string(cfg.symbols.size()) + "\n"
            "Min spread: " + std::to_string(cfg.arb.min_net_spread_bps) + " bps");

    // ── 11. SmartStream — blocking (reconnect loop inside) ────────────────────
    LOG_SYSTEM("Starting feed...");
    try {
        feed.start();
    } catch (const std::exception& ex) {
        LOG_RISK(std::string("Feed fatal: ") + ex.what());
    }

    // ── 12. Shutdown ──────────────────────────────────────────────────────────
    g_stop.store(true);
    work_guard.reset();

    if (stop_monitor.joinable()) stop_monitor.join();
    if (ioc_thread.joinable())   ioc_thread.join();

    // Save trade log
    order_mgr.save_csv("trades.csv");
    order_mgr.print_summary();

    // Final P&L alert
    auto pnl = order_mgr.snapshot();
    char final_msg[300];
    std::snprintf(final_msg, sizeof(final_msg),
        "🛑 <b>arb_bot stopped</b>\n"
        "Realised P&amp;L: ₹%.2f\n"
        "Trades: %d (W:%d / L:%d)\n"
        "Hedges: %d",
        pnl.realised, pnl.total, pnl.wins, pnl.losses, pnl.hedge_fires);
    tg.send(final_msg);

    LOG_SYSTEM("arb_bot shutdown complete.");
    return 0;
}
