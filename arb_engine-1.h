#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// strategy/arb_engine.h
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <memory>
#include <chrono>
#include "order_book.h"
#include "../config/config.h"

using SteadyClock = std::chrono::steady_clock;
using SteadyTp    = SteadyClock::time_point;

// Forward declaration
class MarketGuard;

// ── Arb direction ──────────────────────────────────────────────────────────────
enum class ArbDir : uint8_t {
    BUY_NSE_SELL_BSE,
    BUY_BSE_SELL_NSE
};

// ── Signal emitted when a valid arb opportunity is found ──────────────────────
struct ArbSignal {
    int           id{0};
    SteadyTp      t0;                   // feed tick time (for latency tracking)
    const char*   symbol{nullptr};
    ArbDir        dir{ArbDir::BUY_NSE_SELL_BSE};

    // FIX #4: prices in integer paise (₹ × 100)
    int32_t  buy_px{0};            // paise
    int32_t  sell_px{0};          // paise
    int32_t  buy_avail_qty{0};
    int32_t  sell_avail_qty{0};

    double   gross_bps{0.0};
    double   net_bps{0.0};
    int      qty{0};
    double   est_pnl{0.0};        // estimated P&L in ₹
    int64_t  ts_ms{0};            // exchange timestamp
};

using SignalCb = std::function<void(const ArbSignal&)>;

// ── Per-symbol state ───────────────────────────────────────────────────────────
struct SymbolState {
    const char*             name;
    int                     trade_qty;
    SeqLock<DepthBook>      nse;
    SeqLock<DepthBook>      bse;
    std::atomic<int64_t>    last_signal_mono_ms{0};
};

// ── Arb engine ────────────────────────────────────────────────────────────────
class ArbEngine {
public:
    ArbEngine(const AppConfig& cfg, SignalCb cb);
    ~ArbEngine();

    // Called by feed on every depth tick
    void on_depth(const std::string& token,
                  uint8_t exchange_type,
                  const DepthBook& book,
                  SteadyTp t0);

private:
    void evaluate(SymbolState& st, SteadyTp t0);
    // FIX #4: int32_t paise — exact integer comparison for cross detection
    bool cross_and_depth(int32_t buy_ask,  int32_t buy_ask_qty,  int32_t buy_cum3,
                          int32_t sell_bid, int32_t sell_bid_qty, int32_t sell_cum3) const;

    double net_cost_bps() const {
        return cfg_.arb.txn_cost_bps + cfg_.arb.slippage_bps;
    }

    const AppConfig&                                  cfg_;
    SignalCb                                          cb_;
    std::vector<std::unique_ptr<SymbolState>>         states_;  // unique_ptr avoids atomic copy
    std::unordered_map<std::string, size_t>           tok_idx_; // token → states_ index
    std::unique_ptr<MarketGuard>                      guard_;
    std::atomic<int>                                  sig_counter_{0};
};
