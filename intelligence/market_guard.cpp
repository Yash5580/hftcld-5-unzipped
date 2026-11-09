#include "market_guard.h"
#include "../logger/logger.h"
#include <cstring>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// FIX #6: Spread history separated per direction.
//
// OLD: both BUY_NSE_SELL_BSE and BUY_BSE_SELL_NSE called st.push_spread()
//      into the same CrossState ring buffer. If ticks alternated directions
//      (e.g., spread briefly flipped), spread_decaying() read mixed samples
//      from both directions, producing false decay signals.
//
// NEW: CrossState carries two independent spread histories:
//        spread_buf_buy_nse[] — samples for BUY_NSE_SELL_BSE only
//        spread_buf_buy_bse[] — samples for BUY_BSE_SELL_NSE only
//      push_spread_dir() and spread_decaying_dir() select the correct buffer.
//
//      REQUIRED header change in market_guard.h — update CrossState to:
//
//        struct CrossState {
//            int buy_nse_ticks{0};
//            int buy_bse_ticks{0};
//
//            // Per-direction spread ring buffers (FIX #6)
//            static constexpr int SPREAD_BUF = 4;
//            double spread_buf_buy_nse[SPREAD_BUF]{};
//            double spread_buf_buy_bse[SPREAD_BUF]{};
//            int    spread_idx_buy_nse{0};
//            int    spread_idx_buy_bse{0};
//            int    spread_count_buy_nse{0};
//            int    spread_count_buy_bse{0};
//
//            void push_spread_dir(ArbDir dir, double bps);
//            bool spread_decaying_dir(ArbDir dir, double max_decay_bps) const;
//        };
//
//      Remove the old push_spread() / spread_decaying() methods from CrossState.
// ─────────────────────────────────────────────────────────────────────────────

MarketGuard::MarketGuard(const MarketGuardCfg& cfg) : cfg_(cfg) {}

// ─── Track cross state on every depth update ──────────────────────────────────
// FIX #6: push_spread_dir() pushes into the correct per-direction buffer.
void MarketGuard::on_depth_update(const std::string& symbol,
                                   ArbDir candidate_dir,
                                   bool cross_exists,
                                   double net_spread_bps)
{
    auto& st = states_[symbol];

    if (candidate_dir == ArbDir::BUY_NSE_SELL_BSE) {
        if (cross_exists) {
            ++st.buy_nse_ticks;
            st.push_spread_dir(ArbDir::BUY_NSE_SELL_BSE, net_spread_bps); // FIX #6
        } else {
            st.buy_nse_ticks = 0;
        }
    } else {
        if (cross_exists) {
            ++st.buy_bse_ticks;
            st.push_spread_dir(ArbDir::BUY_BSE_SELL_NSE, net_spread_bps); // FIX #6
        } else {
            st.buy_bse_ticks = 0;
        }
    }
}

// ─── Pre-trade evaluation ─────────────────────────────────────────────────────
MarketGuardResult MarketGuard::evaluate(const ArbSignal& sig,
                                         const DepthBook& nse_book,
                                         const DepthBook& bse_book) const
{
    MarketGuardResult res;
    res.approved_qty = sig.qty;

    auto st_it = states_.find(sig.symbol);
    if (st_it == states_.end()) {
        res.reject_reason = "no_cross_history";
        return res;
    }
    const CrossState& st = st_it->second;

    // ── Identify sell-side book ───────────────────────────────────────────────
    const DepthBook& sell_book = (sig.dir == ArbDir::BUY_NSE_SELL_BSE)
                                  ? bse_book : nse_book;

    int cross_age = (sig.dir == ArbDir::BUY_NSE_SELL_BSE)
                     ? st.buy_nse_ticks : st.buy_bse_ticks;

    // ── CHECK A: OIR on sell side ─────────────────────────────────────────────
    double sell_oir = compute_oir(sell_book, cfg_.oir_depth_levels);
    res.sell_oir = sell_oir;

    if (sell_oir < cfg_.oir_min_threshold) {
        res.reject_reason = "oir_weak_sell_side";
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "GUARD REJECT %s dir=%d OIR_sell=%.3f < %.3f",
            sig.symbol, (int)sig.dir, sell_oir, cfg_.oir_min_threshold);
        LOG_TRADE(buf);
        return res;
    }

    // ── CHECK B: Spread quality — minimum cross age ───────────────────────────
    res.cross_age_ticks = cross_age;

    if (cross_age < cfg_.min_cross_ticks) {
        res.reject_reason = "cross_too_young";
        char buf[100];
        std::snprintf(buf, sizeof(buf),
            "GUARD REJECT %s cross_age=%d < min=%d",
            sig.symbol, cross_age, cfg_.min_cross_ticks);
        LOG_TRADE(buf);
        return res;
    }

    // ── CHECK B2: Spread decay — direction-isolated buffer ────────────────────
    // FIX #6: spread_decaying_dir() checks only samples from this direction,
    //         not a shared buffer that mixes BUY_NSE and BUY_BSE ticks.
    if (st.spread_decaying_dir(sig.dir, cfg_.spread_decay_max_bps)) {
        res.reject_reason = "spread_decaying";
        char buf[100];
        std::snprintf(buf, sizeof(buf),
            "GUARD REJECT %s spread decaying rapidly (dir=%d)",
            sig.symbol, (int)sig.dir);
        LOG_TRADE(buf);
        return res;
    }

    // ── CHECK C: Fill probability score ──────────────────────────────────────
    double qty_ratio = static_cast<double>(
                           std::min(sig.buy_avail_qty, sig.sell_avail_qty))
                       / std::max(1, sig.qty);

    double fill_prob = compute_fill_prob(sell_oir, cross_age, qty_ratio);
    res.fill_prob = fill_prob;

    if (fill_prob < cfg_.fill_prob_min) {
        res.reject_reason = "fill_prob_too_low";
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "GUARD REJECT %s fill_prob=%.3f < %.3f",
            sig.symbol, fill_prob, cfg_.fill_prob_min);
        LOG_TRADE(buf);
        return res;
    }

    // ── Qty scaling: borderline confidence → reduce size ─────────────────────
    if (fill_prob < cfg_.fill_prob_scale_threshold) {
        double scale = std::max(cfg_.min_qty_scale_factor,
                                fill_prob / cfg_.fill_prob_scale_threshold);
        res.approved_qty = std::max(1, static_cast<int>(sig.qty * scale));
    }

    // ── ALL CHECKS PASSED ─────────────────────────────────────────────────────
    res.approved = true;

    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "GUARD OK %s OIR_sell=%.3f age=%d prob=%.3f qty=%d→%d",
        sig.symbol, sell_oir, cross_age, fill_prob,
        sig.qty, res.approved_qty);
    LOG_TRADE(buf);

    return res;
}
