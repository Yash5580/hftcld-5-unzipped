#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// latency/latency_tracker.h
//
// LatencyTimer: stamps four points in a trade's lifecycle and emits a CSV row
// to latency.log.
//
//   t0 — signal detected (set from ArbSignal::t0, the feed tick timestamp)
//   t1 — OrderManager::execute() entered
//   t2 — both legs placed
//   t3 — trade closed (both legs filled or hedged)
//
// All intervals are in microseconds (µs).
// emit() is a synchronous file write — acceptable because it runs after the
// trade is already closed and is not on the critical hot path.
// ─────────────────────────────────────────────────────────────────────────────
#include <chrono>
#include <string>
#include <fstream>
#include <cstdio>

using SteadyClock = std::chrono::steady_clock;
using SteadyTp    = SteadyClock::time_point;

struct LatencyTimer {
    int         trade_id{0};
    std::string symbol;
    SteadyTp    t0;   // feed tick that triggered the signal
    SteadyTp    t1;   // execute() entered
    SteadyTp    t2;   // legs placed
    SteadyTp    t3;   // trade closed

    void mark_t1() { t1 = SteadyClock::now(); }
    void mark_t2() { t2 = SteadyClock::now(); }
    void mark_t3() { t3 = SteadyClock::now(); }

    // Write one CSV row to latency.log.
    // Format: trade_id, symbol, detect_to_place_µs, place_to_close_µs, total_µs
    void emit() const {
        auto us = [](SteadyTp a, SteadyTp b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };

        static std::ofstream f = [](){
            std::ofstream o("latency.log", std::ios::app);
            if (o.tellp() == 0)
                o << "trade_id,symbol,detect_us,execute_us,close_us,total_us\n";
            return o;
        }();

        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "%d,%s,%lld,%lld,%lld,%lld\n",
            trade_id, symbol.c_str(),
            (long long)us(t0, t1),
            (long long)us(t1, t2),
            (long long)us(t2, t3),
            (long long)us(t0, t3));
        f << buf;
        f.flush();
    }
};
