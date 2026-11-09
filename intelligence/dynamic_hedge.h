#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// intelligence/dynamic_hedge.h
//
// Replaces the fixed 75ms → market order timer.
//
// After both legs are dispatched, DynamicHedge monitors the sell side on
// every subsequent depth tick and fires a hedge if market conditions deteriorate.
//
// HEDGE fires immediately if ANY of:
//   Condition A — Spread closed:
//     The original cross no longer exists (buy_ask >= sell_bid again).
//     Means the arb opportunity has vanished → our sell limit likely won't fill.
//     Action: market-order the sell leg NOW.
//
//   Condition B — Price moved against us > 2 bps:
//     Sell-side best bid has dropped > 2 bps below our sell limit price.
//     We are now selling at a worse price than expected.
//     Action: market-order immediately to stop P&L from eroding further.
//
// FALLBACK timer (last resort only):
//   100ms hard deadline — if neither A nor B fired, force close.
//   This should almost never trigger in liquid markets.
//
// FIX #4: sell_limit_px_ and buy_exec_px_ changed from double to int32_t paise.
//   Previously double fields caused:
//     - spread_closed(): bse.bids[0].price < sell_limit_px_ could misfire
//       because ₹100.05 stored as 100.04999... shifted the threshold.
//     - price_moved_against(): (sell_limit_px_ - current_bid) / sell_limit_px_
//       was a double subtraction — the numerator accumulated rounding error,
//       producing slightly wrong move_bps values that could suppress/trigger
//       hedges at the wrong moment.
//   With int32_t paise: subtraction is exact integer arithmetic.
//   The cast to double happens only for the final bps division.
//
// Design:
//   - No thread. Runs on io_context via async callbacks.
//   - Called from on_depth() — same thread as feed.
//   - Each open trade has one DynamicHedge instance.
//   - Once hedge fires, instance self-disarms (idempotent).
// ─────────────────────────────────────────────────────────────────────────────
#include <functional>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdint>
#include <boost/asio.hpp>
#include "../strategy/order_book.h"
#include "../strategy/arb_engine.h"   // ArbDir

namespace asio = boost::asio;

enum class HedgeReason : uint8_t {
    SPREAD_CLOSED,        // cross no longer exists
    PRICE_MOVED_AGAINST,  // sell bid dropped > 2 bps below our limit
    TIMEOUT               // 100ms fallback
};

struct HedgeDecision {
    bool        should_hedge{false};
    HedgeReason reason{HedgeReason::TIMEOUT};
    double      adverse_move_bps{0.0};
};

using HedgeCb = std::function<void(int trade_id, HedgeReason reason,
                                    double adverse_bps)>;

class DynamicHedge {
public:
    DynamicHedge(int          trade_id,
                  ArbDir       dir,
                  int32_t      sell_limit_px,   // FIX #4: paise (was double)
                  int32_t      buy_exec_px,     // FIX #4: paise (was double)
                  double       price_move_threshold_bps,
                  int          timeout_ms,
                  asio::io_context& ioc,
                  HedgeCb      cb);

    ~DynamicHedge();

    void on_tick(const DepthBook& nse_book,
                  const DepthBook& bse_book);

    bool fired() const { return fired_.load(std::memory_order_relaxed); }

private:
    int     trade_id_;
    ArbDir  dir_;
    int32_t sell_limit_px_;              // FIX #4: paise
    int32_t buy_exec_px_;               // FIX #4: paise
    double  price_move_threshold_bps_;
    int     timeout_ms_;
    HedgeCb cb_;

    std::atomic<bool> fired_{false};
    std::shared_ptr<asio::steady_timer> timeout_timer_;

    void fire(HedgeReason reason, double adverse_bps);

    // Condition A: cross closed — now uses exact integer comparison
    bool spread_closed(const DepthBook& nse, const DepthBook& bse) const;

    // Condition B: price moved > threshold bps — subtraction is exact int32_t,
    // only the final division to bps is floating point
    bool price_moved_against(const DepthBook& nse,
                               const DepthBook& bse,
                               double& adverse_bps) const;
};
