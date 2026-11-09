#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// intelligence/market_guard.h
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include "../strategy/order_book.h"
#include "../strategy/arb_engine.h"  // ArbDir, ArbSignal

// ── Config ────────────────────────────────────────────────────────────────────
struct MarketGuardCfg {
    double oir_min_threshold{0.10};
    int    oir_depth_levels{5};
    int    min_cross_ticks{3};
    double spread_decay_max_bps{1.0};
    double fill_prob_min{0.60};
    double fill_prob_scale_threshold{0.85};
    double min_qty_scale_factor{0.50};
};

// ── Result returned to ArbEngine ──────────────────────────────────────────────
struct MarketGuardResult {
    bool        approved{false};
    int         approved_qty{0};
    const char* reject_reason{nullptr};
    double      sell_oir{0.0};
    int         cross_age_ticks{0};
    double      fill_prob{0.0};
};

// ── Per-symbol, per-direction cross state ─────────────────────────────────────
// FIX #6: two independent spread ring buffers — one per direction.
struct CrossState {
    int buy_nse_ticks{0};
    int buy_bse_ticks{0};

    static constexpr int SPREAD_BUF = 4;

    // BUY_NSE_SELL_BSE spread history
    double spread_buf_buy_nse[SPREAD_BUF]{};
    int    spread_idx_buy_nse{0};
    int    spread_count_buy_nse{0};

    // BUY_BSE_SELL_NSE spread history
    double spread_buf_buy_bse[SPREAD_BUF]{};
    int    spread_idx_buy_bse{0};
    int    spread_count_buy_bse{0};

    void push_spread_dir(ArbDir dir, double bps) {
        if (dir == ArbDir::BUY_NSE_SELL_BSE) {
            spread_buf_buy_nse[spread_idx_buy_nse % SPREAD_BUF] = bps;
            ++spread_idx_buy_nse;
            if (spread_count_buy_nse < SPREAD_BUF) ++spread_count_buy_nse;
        } else {
            spread_buf_buy_bse[spread_idx_buy_bse % SPREAD_BUF] = bps;
            ++spread_idx_buy_bse;
            if (spread_count_buy_bse < SPREAD_BUF) ++spread_count_buy_bse;
        }
    }

    // Returns true if the most recent spread sample is significantly below
    // the oldest sample in this direction's buffer (spread is shrinking fast).
    bool spread_decaying_dir(ArbDir dir, double max_decay_bps) const {
        const double* buf   = (dir == ArbDir::BUY_NSE_SELL_BSE)
                              ? spread_buf_buy_nse : spread_buf_buy_bse;
        int           count = (dir == ArbDir::BUY_NSE_SELL_BSE)
                              ? spread_count_buy_nse : spread_count_buy_bse;
        int           idx   = (dir == ArbDir::BUY_NSE_SELL_BSE)
                              ? spread_idx_buy_nse : spread_idx_buy_bse;
        if (count < 2) return false;

        // Newest sample
        double newest = buf[(idx - 1 + SPREAD_BUF) % SPREAD_BUF];
        // Oldest sample in the ring
        double oldest = buf[idx % SPREAD_BUF];
        return (oldest - newest) > max_decay_bps;
    }
};

// ── MarketGuard ───────────────────────────────────────────────────────────────
class MarketGuard {
public:
    explicit MarketGuard(const MarketGuardCfg& cfg);

    // Called BEFORE evaluate() on every depth tick.
    void on_depth_update(const std::string& symbol,
                          ArbDir candidate_dir,
                          bool cross_exists,
                          double net_spread_bps);

    // Pre-trade gate: returns approved (with possible qty scaling) or rejected.
    MarketGuardResult evaluate(const ArbSignal& sig,
                                const DepthBook& nse_book,
                                const DepthBook& bse_book) const;

private:
    // Order Imbalance Ratio: (bid_qty - ask_qty) / (bid_qty + ask_qty)
    // Positive = buyers dominate; we want OIR > threshold on the sell side
    // so our sell limit fills quickly.
    double compute_oir(const DepthBook& book, int levels) const {
        double bid = 0.0, ask = 0.0;
        for (int i = 0; i < levels && i < DepthBook::DEPTH; ++i) {
            bid += book.bids[i].qty;
            ask += book.asks[i].qty;
        }
        double total = bid + ask;
        return (total > 0.0) ? (bid - ask) / total : 0.0;
    }

    // Composite fill probability: [0, 1]
    //   OIR component:  normalised OIR score
    //   Age component:  how many ticks the cross has persisted
    //   Qty component:  available qty vs required qty ratio
    double compute_fill_prob(double oir, int age, double qty_ratio) const {
        double oir_score = std::min(1.0, oir / 0.5);
        double age_score = std::min(1.0, age / 5.0);
        double qty_score = std::min(1.0, qty_ratio);
        return 0.4 * oir_score + 0.3 * age_score + 0.3 * qty_score;
    }

    MarketGuardCfg                                  cfg_;
    mutable std::unordered_map<std::string, CrossState> states_;
};
