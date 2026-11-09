#include "dynamic_hedge.h"
#include "../logger/logger.h"
#include "../config/price_utils.h"
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// FIX #4: sell_limit_px_ and buy_exec_px_ are int32_t paise.
//
// spread_closed():
//   OLD: bse.bids[0].price <= nse.asks[0].price   (double comparison)
//        bse.bids[0].price <  sell_limit_px_        (double comparison)
//   NEW: same logic but operands are int32_t — comparison is exact.
//   e.g. sell_limit_px_ = 10005 (₹100.05 exactly).
//        If bse bid = 10004 → 10004 < 10005 is unambiguously true.
//        With double: 100.05 stored as 100.04999... → threshold shifts by ~0.01p.
//
// price_moved_against():
//   OLD: double move_bps = (sell_limit_px_ - current_bid) / sell_limit_px_ * 10000
//        Both operands double → subtraction accumulates rounding error.
//   NEW: int32_t subtraction (exact), cast to double only for the bps division.
//        move_bps = static_cast<double>(sell_limit_px_ - current_bid)
//                   / sell_limit_px_ * 10000.0
//        The numerator is now an exact integer difference in paise.
// ─────────────────────────────────────────────────────────────────────────────

DynamicHedge::DynamicHedge(int          trade_id,
                              ArbDir       dir,
                              int32_t      sell_limit_px,
                              int32_t      buy_exec_px,
                              double       price_move_threshold_bps,
                              int          timeout_ms,
                              asio::io_context& ioc,
                              HedgeCb      cb)
    : trade_id_(trade_id),
      dir_(dir),
      sell_limit_px_(sell_limit_px),
      buy_exec_px_(buy_exec_px),
      price_move_threshold_bps_(price_move_threshold_bps),
      timeout_ms_(timeout_ms),
      cb_(std::move(cb))
{
    timeout_timer_ = std::make_shared<asio::steady_timer>(ioc);
    timeout_timer_->expires_after(std::chrono::milliseconds(timeout_ms_));
    timeout_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec) fire(HedgeReason::TIMEOUT, 0.0);
    });
}

DynamicHedge::~DynamicHedge() {
    if (timeout_timer_) {
        timeout_timer_->cancel();
    }
}

// ─── Called on every depth tick after trade opens ─────────────────────────────
void DynamicHedge::on_tick(const DepthBook& nse_book,
                             const DepthBook& bse_book)
{
    if (fired_.load(std::memory_order_relaxed)) return;

    if (spread_closed(nse_book, bse_book)) {
        fire(HedgeReason::SPREAD_CLOSED, 0.0);
        return;
    }

    double adverse_bps = 0.0;
    if (price_moved_against(nse_book, bse_book, adverse_bps)) {
        fire(HedgeReason::PRICE_MOVED_AGAINST, adverse_bps);
    }
}

// ─── Fire once, cancel timer, invoke callback ─────────────────────────────────
void DynamicHedge::fire(HedgeReason reason, double adverse_bps) {
    bool expected = false;
    if (!fired_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
        return;
    }

    if (timeout_timer_) {
        timeout_timer_->cancel();
    }

    const char* reason_str = "TIMEOUT";
    if (reason == HedgeReason::SPREAD_CLOSED)       reason_str = "SPREAD_CLOSED";
    if (reason == HedgeReason::PRICE_MOVED_AGAINST) reason_str = "PRICE_ADVERSE";

    char buf[140];
    std::snprintf(buf, sizeof(buf),
        "HEDGE FIRE #%d reason=%s adverse=%.2fbps",
        trade_id_, reason_str, adverse_bps);
    LOG_RISK(buf);

    if (cb_) cb_(trade_id_, reason, adverse_bps);
}

// ─── Condition A: Has the cross closed? ──────────────────────────────────────
// FIX #4: operands are int32_t paise — comparisons are exact.
// Previously as double: sell_limit_px_ = 100.05 → stored as 100.04999...
// This shifted the threshold unpredictably by up to 0.5 paise.
bool DynamicHedge::spread_closed(const DepthBook& nse,
                                   const DepthBook& bse) const
{
    if (dir_ == ArbDir::BUY_NSE_SELL_BSE) {
        // Cross was: NSE_ask < BSE_bid
        // Closed when BSE_bid <= NSE_ask  OR  BSE_bid < our sell limit
        return (bse.bids[0].price <= nse.asks[0].price)   // exact int comparison
            || (bse.bids[0].price <  sell_limit_px_);     // exact int comparison
    } else {
        // Cross was: BSE_ask < NSE_bid
        return (nse.bids[0].price <= bse.asks[0].price)
            || (nse.bids[0].price <  sell_limit_px_);
    }
}

// ─── Condition B: Has price moved > threshold bps against our sell? ───────────
// FIX #4: current_bid is int32_t paise. The subtraction
//   (sell_limit_px_ - current_bid) is exact integer arithmetic.
// Only the division to convert to bps uses floating point — and at that point
// the numerator is an exact integer, so no accumulated rounding error.
//
// OLD (double): 100.05 - 100.02 might compute as 0.029999... instead of 0.03,
//               shifting move_bps by ~0.03 bps — enough to suppress a hedge.
// NEW (int32_t): 10005 - 10002 = 3 exactly → move_bps = 3/10005 * 10000 = 2.999bps
bool DynamicHedge::price_moved_against(const DepthBook& nse,
                                         const DepthBook& bse,
                                         double& adverse_bps) const
{
    int32_t current_bid = (dir_ == ArbDir::BUY_NSE_SELL_BSE)
                          ? bse.bids[0].price
                          : nse.bids[0].price;

    if (current_bid <= 0 || sell_limit_px_ <= 0) return false;

    // FIX #4: integer subtraction → exact numerator, float division for bps only
    double move_bps = static_cast<double>(sell_limit_px_ - current_bid)
                      / sell_limit_px_ * 10000.0;

    if (move_bps > price_move_threshold_bps_) {
        adverse_bps = move_bps;
        return true;
    }
    return false;
}
