#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// risk/risk_manager.h
// Pre-trade risk checks. check_signal() must return true for OrderManager
// to call execute(). update_pnl() is called by OrderManager after each close.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <string>
#include "../config/config.h"
#include "../strategy/arb_engine.h"
#include "../config/price_utils.h"

class RiskManager {
public:
    explicit RiskManager(const RiskCfg& cfg) : cfg_(cfg) {}

    // Returns true if the signal is within all risk limits.
    // Logs the specific breach if false.
    bool check_signal(const ArbSignal& sig) const;

    // Called by OrderManager after each trade closes.
    void update_pnl(double realised_pnl);

    bool is_halted() const { return halted_.load(); }
    double daily_pnl()    const { return daily_pnl_.load(); }
    int    trade_count()  const { return trade_count_.load(); }

private:
    const RiskCfg&         cfg_;
    std::atomic<double>    daily_pnl_{0.0};
    std::atomic<int>       trade_count_{0};
    std::atomic<bool>      halted_{false};
};
