#include "arb_engine.h"
#include "../intelligence/market_guard.h"
#include "../logger/logger.h"
#include "../config/price_utils.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// FIX #4: All price fields (DepthBook::Level::price, ArbSignal::buy_px/sell_px)
// are now int32_t paise (₹ × 100). See price_utils.h for conversion helpers.
//
// Key changes vs previous version:
//   1. cross_and_depth() signature: double buy_ask, double sell_bid
//                                → int32_t buy_ask, int32_t sell_bid
//      The >= comparison is now exact integer arithmetic. No IEEE 754 rounding.
//
//   2. Spread/bps calculation: was (sell - buy) / mid where all were double.
//      Now uses spread_bps(sell_px, buy_px) from price_utils.h.
//      The subtraction (sell - buy) is exact integer subtraction; only the
//      subsequent division to get bps needs floating point.
//
//   3. est_pnl: was (sell - buy) * qty in rupee doubles — imprecise.
//      Now uses gross_pnl_rs() and mid_rs() helpers from price_utils.h.
//
//   4. Log output: px_to_rs() converts paise back to rupees for display only.
// ─────────────────────────────────────────────────────────────────────────────

ArbEngine::ArbEngine(const AppConfig& cfg, SignalCb cb)
    : cfg_(cfg), cb_(std::move(cb))
{
    states_.reserve(cfg_.symbols.size());
    for (auto& s : cfg_.symbols) {
        size_t idx = states_.size();
        auto st = std::make_unique<SymbolState>();
        st->name      = s.name.c_str();
        st->trade_qty = s.trade_qty;
        states_.push_back(std::move(st));
        tok_idx_[s.nse_token] = idx;
        tok_idx_[s.bse_token] = idx;
    }

    MarketGuardCfg mgcfg;
    mgcfg.oir_min_threshold         = cfg_.intelligence.oir_min_threshold;
    mgcfg.oir_depth_levels          = cfg_.intelligence.oir_depth_levels;
    mgcfg.min_cross_ticks           = cfg_.intelligence.min_cross_ticks;
    mgcfg.spread_decay_max_bps      = cfg_.intelligence.spread_decay_max_bps;
    mgcfg.fill_prob_min             = cfg_.intelligence.fill_prob_min;
    mgcfg.fill_prob_scale_threshold = cfg_.intelligence.fill_prob_scale_threshold;
    mgcfg.min_qty_scale_factor      = cfg_.intelligence.min_qty_scale_factor;
    guard_ = std::make_unique<MarketGuard>(mgcfg);

    LOG_SYSTEM("ArbEngine v5 — integer paise prices + MarketGuard(OIR+age+prob)");
}

ArbEngine::~ArbEngine() = default;

void ArbEngine::on_depth(const std::string& token,
                          uint8_t exchange_type,
                          const DepthBook& book,
                          SteadyTp t0)
{
    auto it = tok_idx_.find(token);
    if (it == tok_idx_.end()) return;
    SymbolState& st = *states_[it->second];   // dereference unique_ptr
    if      (exchange_type == 1) st.nse.write(book);
    else if (exchange_type == 3) st.bse.write(book);
    else return;
    if (!book.valid) return;
    evaluate(st, t0);
}

void ArbEngine::evaluate(SymbolState& st, SteadyTp t0)
{
    auto now_mono = std::chrono::duration_cast<std::chrono::milliseconds>(
                        SteadyClock::now().time_since_epoch()).count();
    if (now_mono - st.last_signal_mono_ms.load(std::memory_order_relaxed)
            < cfg_.arb.cooldown_ms) return;

    DepthBook nse, bse;
    if (!st.nse.read(nse) || !st.bse.read(bse)) return;
    if (!nse.valid || !bse.valid) return;
    if (std::abs(nse.ts_ms - bse.ts_ms) > 2000) return;

    int32_t nse_cum3_ask = nse.cum_ask_qty(3);
    int32_t nse_cum3_bid = nse.cum_bid_qty(3);
    int32_t bse_cum3_bid = bse.cum_bid_qty(3);
    int32_t bse_cum3_ask = bse.cum_ask_qty(3);

    // ── CASE A: BUY NSE, SELL BSE ─────────────────────────────────────────────
    {
        // FIX #4: prices are int32_t paise — cross_and_depth now takes int32_t.
        // The < comparison is exact integer arithmetic: no float rounding risk.
        bool cross_a = cross_and_depth(nse.asks[0].price, nse.asks[0].qty, nse_cum3_ask,
                                        bse.bids[0].price, bse.bids[0].qty, bse_cum3_bid);
        double net_a = 0.0;
        if (cross_a) {
            // FIX #4: spread_bps() subtracts int32_t paise exactly, then divides.
            // Was: (bse.bids[0].price - nse.asks[0].price) / mid * 10000.0
            // where mid was a double — rounding error in mid contaminated bps.
            net_a = spread_bps(bse.bids[0].price, nse.asks[0].price) - net_cost_bps();
        }
        guard_->on_depth_update(st.name, ArbDir::BUY_NSE_SELL_BSE,
                                  cross_a && net_a >= cfg_.arb.min_net_spread_bps, net_a);

        if (cross_a && net_a >= cfg_.arb.min_net_spread_bps) {
            ArbSignal sig;
            sig.t0     = t0;
            sig.symbol = st.name;
            sig.dir    = ArbDir::BUY_NSE_SELL_BSE;

            // int32_t paise — exact storage, no representation loss
            sig.buy_px         = nse.asks[0].price;
            sig.sell_px        = bse.bids[0].price;
            sig.buy_avail_qty  = nse.asks[0].qty;
            sig.sell_avail_qty = bse.bids[0].qty;

            sig.gross_bps = spread_bps(sig.sell_px, sig.buy_px);
            sig.net_bps   = net_a;

            sig.qty = std::min({(int)st.trade_qty,
                                (int)nse.asks[0].qty,
                                (int)bse.bids[0].qty});

            // FIX #4: gross_pnl_rs() divides paise diff by 100 to get rupees.
            // Was: (sig.sell_px - sig.buy_px) * qty — double subtraction, lossy.
            sig.est_pnl = gross_pnl_rs(sig.sell_px, sig.buy_px, sig.qty)
                          - mid_rs(sig.buy_px, sig.sell_px) * sig.qty
                            * net_cost_bps() / 10000.0;
            sig.ts_ms = nse.ts_ms;

            MarketGuardResult g = guard_->evaluate(sig, nse, bse);
            if (!g.approved) return;

            sig.qty     = g.approved_qty;
            sig.est_pnl = gross_pnl_rs(sig.sell_px, sig.buy_px, sig.qty)
                          - mid_rs(sig.buy_px, sig.sell_px) * sig.qty
                            * net_cost_bps() / 10000.0;
            sig.id = sig_counter_.fetch_add(1, std::memory_order_relaxed);
            st.last_signal_mono_ms.store(now_mono, std::memory_order_relaxed);

            char buf[220];
            // FIX #4: px_to_rs() converts paise → rupees for display only
            std::snprintf(buf, sizeof(buf),
                "SIGNAL #%d %s BUY_NSE_SELL_BSE buy=%.2f sell=%.2f "
                "net=%.2fbps qty=%d oir_sell=%.3f age=%d prob=%.2f",
                sig.id, sig.symbol,
                px_to_rs(sig.buy_px), px_to_rs(sig.sell_px),
                sig.net_bps, sig.qty,
                g.sell_oir, g.cross_age_ticks, g.fill_prob);
            LOG_TRADE(buf);
            if (cb_) cb_(sig);
            return;
        }
    }

    // ── CASE B: BUY BSE, SELL NSE ─────────────────────────────────────────────
    {
        bool cross_b = cross_and_depth(bse.asks[0].price, bse.asks[0].qty, bse_cum3_ask,
                                        nse.bids[0].price, nse.bids[0].qty, nse_cum3_bid);
        double net_b = 0.0;
        if (cross_b) {
            net_b = spread_bps(nse.bids[0].price, bse.asks[0].price) - net_cost_bps();
        }
        guard_->on_depth_update(st.name, ArbDir::BUY_BSE_SELL_NSE,
                                  cross_b && net_b >= cfg_.arb.min_net_spread_bps, net_b);

        if (cross_b && net_b >= cfg_.arb.min_net_spread_bps) {
            ArbSignal sig;
            sig.t0     = t0;
            sig.symbol = st.name;
            sig.dir    = ArbDir::BUY_BSE_SELL_NSE;

            sig.buy_px         = bse.asks[0].price;
            sig.sell_px        = nse.bids[0].price;
            sig.buy_avail_qty  = bse.asks[0].qty;
            sig.sell_avail_qty = nse.bids[0].qty;

            sig.gross_bps = spread_bps(sig.sell_px, sig.buy_px);
            sig.net_bps   = net_b;

            sig.qty = std::min({(int)st.trade_qty,
                                (int)bse.asks[0].qty,
                                (int)nse.bids[0].qty});

            sig.est_pnl = gross_pnl_rs(sig.sell_px, sig.buy_px, sig.qty)
                          - mid_rs(sig.buy_px, sig.sell_px) * sig.qty
                            * net_cost_bps() / 10000.0;
            sig.ts_ms = bse.ts_ms;

            MarketGuardResult g = guard_->evaluate(sig, nse, bse);
            if (!g.approved) return;

            sig.qty     = g.approved_qty;
            sig.est_pnl = gross_pnl_rs(sig.sell_px, sig.buy_px, sig.qty)
                          - mid_rs(sig.buy_px, sig.sell_px) * sig.qty
                            * net_cost_bps() / 10000.0;
            sig.id = sig_counter_.fetch_add(1, std::memory_order_relaxed);
            st.last_signal_mono_ms.store(now_mono, std::memory_order_relaxed);

            char buf[220];
            std::snprintf(buf, sizeof(buf),
                "SIGNAL #%d %s BUY_BSE_SELL_NSE buy=%.2f sell=%.2f "
                "net=%.2fbps qty=%d oir_sell=%.3f age=%d prob=%.2f",
                sig.id, sig.symbol,
                px_to_rs(sig.buy_px), px_to_rs(sig.sell_px),
                sig.net_bps, sig.qty,
                g.sell_oir, g.cross_age_ticks, g.fill_prob);
            LOG_TRADE(buf);
            if (cb_) cb_(sig);
        }
    }
}

// ─── Cross + depth validation ─────────────────────────────────────────────────
// FIX #4: buy_ask and sell_bid are now int32_t paise.
// The critical buy_ask < sell_bid check is now exact integer comparison.
// Previously as double, ₹100.05 could be stored as 100.04999... making a valid
// cross silently fail or a non-cross silently pass.
bool ArbEngine::cross_and_depth(int32_t buy_ask,  int32_t buy_ask_qty,  int32_t buy_cum3,
                                  int32_t sell_bid, int32_t sell_bid_qty, int32_t sell_cum3) const
{
    if (buy_ask >= sell_bid) return false;          // exact integer comparison
    if (buy_ask_qty  < cfg_.arb.min_qty_best_level) return false;
    if (sell_bid_qty < cfg_.arb.min_qty_best_level) return false;
    if (buy_cum3     < cfg_.arb.top3_min_cumulative) return false;
    if (sell_cum3    < cfg_.arb.top3_min_cumulative) return false;
    return true;
}
