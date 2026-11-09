#include "order_manager.h"
#include "../intelligence/dynamic_hedge.h"
#include "../logger/logger.h"
#include "../config/price_utils.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// This file incorporates all previous fixes PLUS FIX #4 (integer paise prices).
//
// FIX #3: Slippage double-count removed — paper_fill_market() fills at raw
//         current_px; slippage applied once only in close_trade() cost formula.
// FIX #4: Slippage is now computed in paise (integer), then converted to rupees
//         for cost formula. px_cache_ stores int32_t paise, not double.
// FIX #4: All fill_px fields are int32_t paise.
//         P&L computed via gross_pnl_rs() and mid_rs() from price_utils.h.
// FIX #4: Log/CSV price display uses px_to_rs() — paise → rupees for humans.
// FIX #5: open_trades_ map for O(1) lookup (unchanged from previous fix).
// FIX #4 (market fill): slippage deduction converted to paise for fill price.
//         slip_paise = sell_limit_px * market_slippage_bps / 10000 (rounded)
//         fill_px = current_px - slip_paise  (exact integer subtraction)
// ─────────────────────────────────────────────────────────────────────────────

OrderManager::OrderManager(const AppConfig& cfg,
                             asio::io_context& ioc,
                             TradeClosedCb on_closed)
    : cfg_(cfg), ioc_(ioc), on_closed_(std::move(on_closed)) {}

// ─── Execute — both legs dispatched simultaneously ────────────────────────────
int OrderManager::execute(const ArbSignal& sig) {
    std::lock_guard<std::mutex> lk(mtx_);

    int id = next_id_++;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();

    Trade t;
    t.id = id; t.symbol = sig.symbol; t.opened_ms = now_ms;
    t.timer.trade_id = id; t.timer.symbol = sig.symbol;
    t.timer.t0 = sig.t0; t.timer.mark_t1();

    // Buy leg — int32_t paise limit price
    t.buy_leg.exchange  = (sig.dir == ArbDir::BUY_NSE_SELL_BSE) ? "NSE" : "BSE";
    t.buy_leg.side      = "BUY";
    t.buy_leg.limit_px  = sig.buy_px;   // int32_t paise
    t.buy_leg.qty       = sig.qty;
    t.buy_leg.placed_ms = now_ms;
    paper_fill_limit(t.buy_leg);

    // Sell leg — PENDING, watched by DynamicHedge
    t.sell_leg.exchange = (sig.dir == ArbDir::BUY_NSE_SELL_BSE) ? "BSE" : "NSE";
    t.sell_leg.side     = "SELL";
    t.sell_leg.limit_px = sig.sell_px;  // int32_t paise
    t.sell_leg.qty      = sig.qty;
    t.sell_leg.placed_ms = now_ms;
    t.sell_leg.status   = LegStatus::PENDING;

    t.timer.mark_t2();
    trades_.push_back(std::move(t));
    open_trades_[id] = &trades_.back();

    // FIX #4: DynamicHedge now receives int32_t paise prices
    auto hedge = std::make_shared<DynamicHedge>(
        id,
        sig.dir,
        sig.sell_px,                              // int32_t paise sell limit
        sig.buy_px,                               // int32_t paise buy fill
        cfg_.intelligence.price_move_hedge_bps,
        cfg_.exec.hedge_deadline_ms,
        ioc_,
        [this](int trade_id, HedgeReason reason, double adverse_bps) {
            on_hedge_triggered(trade_id, reason, adverse_bps);
        }
    );
    hedges_[id] = hedge;

    char buf[180];
    // FIX #4: px_to_rs() for human-readable log — prices stored as paise
    std::snprintf(buf, sizeof(buf),
        "EXEC #%d %s | BUY %s @%.2f FILLED | SELL %s @%.2f PENDING | qty=%d",
        id, sig.symbol,
        trades_.back().buy_leg.exchange.c_str(),  px_to_rs(sig.buy_px),
        trades_.back().sell_leg.exchange.c_str(), px_to_rs(sig.sell_px),
        sig.qty);
    LOG_TRADE(buf);

    return id;
}

// ─── Called on every depth tick ───────────────────────────────────────────────
void OrderManager::on_market_tick(const std::string& symbol,
                                    const DepthBook& nse_book,
                                    const DepthBook& bse_book)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [trade_id, hedge] : hedges_) {
        auto ot = open_trades_.find(trade_id);
        if (ot == open_trades_.end()) continue;
        Trade* t = ot->second;
        if (t->closed || t->symbol != symbol) continue;
        if (!hedge->fired()) {
            hedge->on_tick(nse_book, bse_book);
        }
    }
}

// ─── DynamicHedge callback ────────────────────────────────────────────────────
void OrderManager::on_hedge_triggered(int trade_id,
                                        HedgeReason reason,
                                        double adverse_bps)
{
    std::lock_guard<std::mutex> lk(mtx_);

    auto ot = open_trades_.find(trade_id);
    if (ot == open_trades_.end()) return;
    Trade* it = ot->second;
    if (it->closed) return;
    if (it->sell_leg.status != LegStatus::PENDING) return;

    it->hedge_fired  = true;
    it->hedge_reason = reason;
    ++hedge_count_;

    int32_t mkt_px = get_market_price(it->symbol, it->sell_leg.exchange);
    if (mkt_px <= 0) mkt_px = it->sell_leg.limit_px;

    paper_fill_market(it->sell_leg, mkt_px);

    const char* reason_str =
        (reason == HedgeReason::SPREAD_CLOSED)       ? "SPREAD_CLOSED" :
        (reason == HedgeReason::PRICE_MOVED_AGAINST)  ? "PRICE_ADVERSE" :
                                                         "TIMEOUT";

    char buf[180];
    // FIX #4: px_to_rs() for display
    std::snprintf(buf, sizeof(buf),
        "HEDGE #%d %s | reason=%s adverse=%.2fbps | "
        "SELL %s MARKET @%.2f (limit=%.2f)",
        trade_id, it->symbol.c_str(), reason_str, adverse_bps,
        it->sell_leg.exchange.c_str(),
        px_to_rs(it->sell_leg.fill_px),
        px_to_rs(it->sell_leg.limit_px));
    LOG_RISK(buf);

    close_trade(*it);
    open_trades_.erase(trade_id);
    hedges_.erase(trade_id);
}

// ─── Paper fills ──────────────────────────────────────────────────────────────
void OrderManager::paper_fill_limit(Leg& leg) {
    leg.fill_px  = leg.limit_px;    // int32_t paise — exact copy, no rounding
    leg.status   = LegStatus::LIMIT_FILLED;
    leg.filled_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
}

// FIX #3 (unchanged): no slippage here — applied once in close_trade() only.
// FIX #4: current_px is int32_t paise — fill_px is exact integer assignment.
void OrderManager::paper_fill_market(Leg& leg, int32_t current_px) {
    leg.fill_px  = current_px;      // raw paise, no deduction
    leg.status   = LegStatus::MARKET_FILLED;
    leg.filled_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ─── Close trade — compute P&L ────────────────────────────────────────────────
// FIX #4: gross P&L and mid use price_utils helpers.
//   gross_pnl_rs(): converts paise diff to rupees via integer subtraction + /100.
//   mid_rs():       converts sum of two paise prices to rupee mid via /200.
//   sell_slip_bps applied once: slippage cost in rupees = mid * qty * bps/10000.
void OrderManager::close_trade(Trade& t) {
    // FIX #4: exact integer subtraction in paise, then /100 for rupees
    double gross = gross_pnl_rs(t.sell_leg.fill_px, t.buy_leg.fill_px, t.buy_leg.qty);
    double mid   = mid_rs(t.buy_leg.fill_px, t.sell_leg.fill_px);  // rupees

    double sell_slip_bps = t.hedge_fired ? cfg_.arb.market_slippage_bps
                                         : cfg_.arb.slippage_bps;

    // Total cost = txn fees + buy limit slippage + sell slippage (once each)
    double cost = mid * t.buy_leg.qty
                  * (cfg_.arb.txn_cost_bps + cfg_.arb.slippage_bps + sell_slip_bps)
                  / 10000.0;

    t.realised_pnl = gross - cost;
    t.closed       = true;
    t.closed_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();

    realised_pnl_.store(realised_pnl_.load() + t.realised_pnl);

    t.timer.mark_t3();
    t.timer.emit();

    char buf[220];
    // FIX #4: px_to_rs() for display
    std::snprintf(buf, sizeof(buf),
        "CLOSED #%d %s | pnl=%.2f | %s | buy@%.2f sell@%.2f | cum=%.2f",
        t.id, t.symbol.c_str(), t.realised_pnl,
        t.hedge_fired ? "HEDGE" : "NORMAL",
        px_to_rs(t.buy_leg.fill_px), px_to_rs(t.sell_leg.fill_px),
        realised_pnl_.load());
    LOG_TRADE(buf);

    if (on_closed_) on_closed_(t);
}

// ─── Price cache ──────────────────────────────────────────────────────────────
// FIX #4: px_cache_ stores int32_t paise (was double).
//   update_price() receives paise directly from the feed after rs_to_px().
void OrderManager::update_price(const std::string& symbol,
                                  const std::string& exchange, int32_t px) {
    std::lock_guard<std::mutex> lk(px_mtx_);
    px_cache_[symbol + ":" + exchange] = px;
}

int32_t OrderManager::get_market_price(const std::string& symbol,
                                         const std::string& exchange) const {
    std::unique_lock<std::mutex> lk(px_mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return 0;
    auto it = px_cache_.find(symbol + ":" + exchange);
    return (it != px_cache_.end()) ? it->second : 0;
}

// ─── Snapshot ─────────────────────────────────────────────────────────────────
PnL OrderManager::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    PnL p;
    p.realised    = realised_pnl_.load();
    p.total       = (int)trades_.size();
    p.hedge_fires = hedge_count_.load();
    for (auto& t : trades_) {
        if (t.closed) {
            if (t.realised_pnl >= 0) ++p.wins;
            else ++p.losses;
        }
    }
    return p;
}

// FIX #4 + FIX #4 (CSV): fill prices displayed as rupees via px_to_rs()
void OrderManager::save_csv(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::ofstream f(path);
    f << "id,symbol,buy_exch,buy_type,buy_limit,buy_fill,"
         "sell_exch,sell_type,sell_limit,sell_fill,"
         "qty,pnl,hedge,hedge_reason,closed,opened_ms,closed_ms\n";

    auto leg_type = [](LegStatus s){
        return (s == LegStatus::MARKET_FILLED) ? "MKT" : "LMT";
    };
    auto hedge_reason_str = [](bool fired, HedgeReason r) -> const char* {
        if (!fired) return "NONE";
        switch (r) {
            case HedgeReason::SPREAD_CLOSED:       return "SPREAD_CLOSED";
            case HedgeReason::PRICE_MOVED_AGAINST: return "PRICE_ADVERSE";
            case HedgeReason::TIMEOUT:             return "TIMEOUT";
            default:                               return "UNKNOWN";
        }
    };

    for (auto& t : trades_) {
        // FIX #4: px_to_rs() converts paise → rupees for CSV readability
        f << std::fixed << std::setprecision(2)
          << t.id << "," << t.symbol << ","
          << t.buy_leg.exchange  << "," << leg_type(t.buy_leg.status)  << ","
          << px_to_rs(t.buy_leg.limit_px)  << "," << px_to_rs(t.buy_leg.fill_px)  << ","
          << t.sell_leg.exchange << "," << leg_type(t.sell_leg.status) << ","
          << px_to_rs(t.sell_leg.limit_px) << "," << px_to_rs(t.sell_leg.fill_px) << ","
          << t.buy_leg.qty       << "," << t.realised_pnl << ","
          << (t.hedge_fired ? 1 : 0) << ","
          << hedge_reason_str(t.hedge_fired, t.hedge_reason) << ","
          << (t.closed ? 1 : 0)      << ","
          << t.opened_ms << "," << t.closed_ms << "\n";
    }
    LOG_SYSTEM("Trade log saved: " + path);
}

void OrderManager::print_summary() const {
    auto p = snapshot();
    char buf[400];
    std::snprintf(buf, sizeof(buf),
        "\n╔══════════════════════════════════════════╗\n"
        "║       PAPER TRADING FINAL SUMMARY        ║\n"
        "╠══════════════════════════════════════════╣\n"
        "║ Realised P&L       : ₹%10.2f           ║\n"
        "║ Total trades       : %6d                ║\n"
        "║ Winners / Losers   : %3d / %3d            ║\n"
        "║ Dynamic hedges     : %6d                ║\n"
        "║   SPREAD_CLOSED    : market fired        ║\n"
        "║   PRICE_ADVERSE    : market fired        ║\n"
        "║   TIMEOUT(100ms)   : fallback only       ║\n"
        "╚══════════════════════════════════════════╝",
        p.realised, p.total, p.wins, p.losses, p.hedge_fires);
    LOG_SYSTEM(buf);
}
