#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// price_utils.h
//
// FIX #4: All prices stored as int32_t paise (₹ × 100).
//
// WHY:
//   IEEE 754 double cannot represent all decimal fractions exactly.
//   ₹100.05 becomes 100.04999999999... internally.
//   Comparisons like (buy_ask >= sell_bid) then misfire silently — the cross
//   detection at the heart of every arb decision becomes unreliable.
//
// RULE:
//   STORE  → int32_t paise  (exact, no rounding, faster integer comparisons)
//   DISPLAY → px_to_rs()   (divide by 100.0 → double, only for logging/CSV)
//   ARITHMETIC (bps/P&L) → cast to double after subtraction, not before.
//     e.g.  spread_bps = static_cast<double>(sell_px - buy_px) / mid_px * 10000.0
//     This is safe because the subtraction of two int32_t paise values is exact,
//     and only the final division needs floating point.
//
// HEADER CHANGES REQUIRED (files not provided — update manually):
//
//   order_book.h — struct Level { int32_t price; int32_t qty; };
//                  (was: double price)
//
//   arb_engine.h — struct ArbSignal {
//                      int32_t buy_px;      // paise  (was double)
//                      int32_t sell_px;     // paise  (was double)
//                      double  gross_bps;   // stays double (bps, not price)
//                      double  net_bps;     // stays double
//                      double  est_pnl;     // stays double (₹, computed)
//                  };
//
//   order_manager.h — struct Leg {
//                         int32_t limit_px;  // paise (was double)
//                         int32_t fill_px;   // paise (was double)
//                     };
//                     Also: std::unordered_map<std::string, int32_t> px_cache_;
//                     And:  int32_t get_market_price(...) const;
//                           void    update_price(..., int32_t px);
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

// ── Conversion helpers ────────────────────────────────────────────────────────

// Convert paise → rupees (for display / logging only)
inline double px_to_rs(int32_t paise) {
    return paise / 100.0;
}

// Convert rupees (double, e.g. from feed) → paise (round to nearest paisa)
// Use at the feed boundary only — once inside the engine, stay in paise.
inline int32_t rs_to_px(double rupees) {
    return static_cast<int32_t>(rupees * 100.0 + 0.5);
}

// ── Spread / bps helpers ──────────────────────────────────────────────────────
// All inputs in paise. Returns basis points as double.
// Subtraction of int32_t is exact → only the division needs floating point.

inline double spread_bps(int32_t sell_px, int32_t buy_px) {
    int32_t mid = (sell_px + buy_px) / 2;
    if (mid <= 0) return 0.0;
    return static_cast<double>(sell_px - buy_px) / mid * 10000.0;
}

// ── P&L helper ────────────────────────────────────────────────────────────────
// gross P&L in rupees from two paise fill prices and a quantity.
inline double gross_pnl_rs(int32_t sell_fill, int32_t buy_fill, int qty) {
    return static_cast<double>(sell_fill - buy_fill) / 100.0 * qty;
}

// mid price in rupees from two paise prices
inline double mid_rs(int32_t px_a, int32_t px_b) {
    return static_cast<double>(px_a + px_b) / 200.0;
}
