#include "risk_manager.h"
#include "../logger/logger.h"
#include <cstdio>

bool RiskManager::check_signal(const ArbSignal& sig) const {
    if (halted_.load()) {
        LOG_RISK("RISK HALT: bot halted — rejecting signal");
        return false;
    }

    // Max trades per day
    if (trade_count_.load() >= cfg_.max_trades_per_day) {
        LOG_RISK("RISK REJECT: max_trades_per_day reached");
        return false;
    }

    // Max qty per symbol
    if (sig.qty > cfg_.max_qty_per_symbol) {
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "RISK REJECT: qty=%d > max_qty_per_symbol=%d",
            sig.qty, cfg_.max_qty_per_symbol);
        LOG_RISK(buf);
        return false;
    }

    // Max notional per trade (buy_px in paise → convert to rupees for check)
    double notional = px_to_rs(sig.buy_px) * sig.qty;
    if (notional > cfg_.max_notional_per_trade) {
        char buf[140];
        std::snprintf(buf, sizeof(buf),
            "RISK REJECT: notional=%.2f > max=%.2f",
            notional, cfg_.max_notional_per_trade);
        LOG_RISK(buf);
        return false;
    }

    return true;
}

void RiskManager::update_pnl(double realised_pnl) {
    double new_pnl = daily_pnl_.load() + realised_pnl;
    daily_pnl_.store(new_pnl);
    trade_count_.fetch_add(1);

    if (new_pnl < -cfg_.max_daily_loss) {
        halted_.store(true);
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "RISK HALT: daily_pnl=%.2f breached max_loss=%.2f — HALTING",
            new_pnl, cfg_.max_daily_loss);
        LOG_RISK(buf);
    }
}
