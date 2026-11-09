#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// execution/order_manager.h
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include <memory>
#include <boost/asio.hpp>
#include "../strategy/arb_engine.h"
#include "../strategy/order_book.h"
#include "../intelligence/dynamic_hedge.h"   // HedgeReason
#include "../latency/latency_tracker.h"
#include "../config/config.h"

namespace asio = boost::asio;

// ── Leg fill status ───────────────────────────────────────────────────────────
enum class LegStatus : uint8_t {
    PENDING,
    LIMIT_FILLED,
    MARKET_FILLED
};

// ── One side of a trade ───────────────────────────────────────────────────────
struct Leg {
    std::string exchange;
    std::string side;        // "BUY" or "SELL"
    int32_t     limit_px{0}; // FIX #4: paise
    int32_t     fill_px{0};  // FIX #4: paise
    int         qty{0};
    int64_t     placed_ms{0};
    int64_t     filled_ms{0};
    LegStatus   status{LegStatus::PENDING};
};

// ── A complete arb trade (two legs) ───────────────────────────────────────────
struct Trade {
    int         id{0};
    std::string symbol;
    Leg         buy_leg;
    Leg         sell_leg;
    bool        closed{false};
    bool        hedge_fired{false};
    HedgeReason hedge_reason{HedgeReason::TIMEOUT}; // FIX #4
    double      realised_pnl{0.0};                  // ₹
    int64_t     opened_ms{0};
    int64_t     closed_ms{0};
    LatencyTimer timer;
};

// ── P&L snapshot ──────────────────────────────────────────────────────────────
struct PnL {
    double realised{0.0};
    int    total{0};
    int    wins{0};
    int    losses{0};
    int    hedge_fires{0};
};

using TradeClosedCb = std::function<void(const Trade&)>;

// ── Order manager ─────────────────────────────────────────────────────────────
class OrderManager {
public:
    OrderManager(const AppConfig& cfg,
                 asio::io_context& ioc,
                 TradeClosedCb on_closed = nullptr);

    // Execute a signal: paper-fills buy leg, arms DynamicHedge for sell leg.
    // Returns trade id.
    int execute(const ArbSignal& sig);

    // Feed every depth tick here — DynamicHedge checks conditions.
    void on_market_tick(const std::string& symbol,
                        const DepthBook& nse_book,
                        const DepthBook& bse_book);

    // Price cache — update from feed on every tick (used as hedge fallback price)
    // FIX #4: int32_t paise
    void    update_price(const std::string& symbol,
                         const std::string& exchange, int32_t px);

    PnL         snapshot()  const;
    void        save_csv(const std::string& path) const;
    void        print_summary() const;

    double realised_pnl() const { return realised_pnl_.load(); }

private:
    void on_hedge_triggered(int trade_id, HedgeReason reason, double adverse_bps);

    void paper_fill_limit(Leg& leg);
    void paper_fill_market(Leg& leg, int32_t current_px); // FIX #4: paise

    void close_trade(Trade& t);

    int32_t get_market_price(const std::string& symbol,
                              const std::string& exchange) const; // FIX #4: paise

    const AppConfig&       cfg_;
    asio::io_context&      ioc_;
    TradeClosedCb          on_closed_;

    mutable std::mutex     mtx_;
    int                    next_id_{1};
    std::vector<Trade>     trades_;

    // FIX #5: O(1) lookup of open trades by id
    std::unordered_map<int, Trade*>                         open_trades_;
    std::unordered_map<int, std::shared_ptr<DynamicHedge>>  hedges_;

    std::atomic<double>  realised_pnl_{0.0};
    std::atomic<int>     hedge_count_{0};

    // FIX #4: price cache stores paise
    mutable std::mutex                         px_mtx_;
    std::unordered_map<std::string, int32_t>   px_cache_;
};
