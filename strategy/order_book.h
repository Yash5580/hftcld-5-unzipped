#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// strategy/order_book.h
//
// DepthBook: 5-level order book snapshot.
// Level::price is int32_t PAISE (₹ × 100) — FIX #4.
// SeqLock: wait-free single-writer / multi-reader snapshot.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <atomic>

// ── One price level ────────────────────────────────────────────────────────────
struct Level {
    int32_t price{0};   // paise (₹ × 100) — exact integer, no IEEE 754 drift
    int32_t qty{0};
};

// ── 5-level depth snapshot ─────────────────────────────────────────────────────
struct DepthBook {
    static constexpr int DEPTH = 5;

    Level   bids[DEPTH]{};   // bids[0] = best bid (highest price)
    Level   asks[DEPTH]{};   // asks[0] = best ask (lowest price)
    bool    valid{false};
    int64_t ts_ms{0};        // exchange timestamp (milliseconds)

    // Cumulative bid qty across top-n levels
    int32_t cum_bid_qty(int n) const {
        int32_t sum = 0;
        for (int i = 0; i < n && i < DEPTH; ++i) sum += bids[i].qty;
        return sum;
    }

    // Cumulative ask qty across top-n levels
    int32_t cum_ask_qty(int n) const {
        int32_t sum = 0;
        for (int i = 0; i < n && i < DEPTH; ++i) sum += asks[i].qty;
        return sum;
    }
};

// ── SeqLock — wait-free, single-writer, multi-reader ──────────────────────────
// Writer increments seq before and after the write (odd = writing).
// Reader retries if seq changes during read or is odd (write in progress).
// Guarantees a consistent snapshot without a mutex on the hot path.
template<typename T>
class SeqLock {
public:
    // Called by the feed thread (single writer)
    void write(const T& val) {
        uint64_t s = seq_.load(std::memory_order_relaxed) + 1;
        seq_.store(s, std::memory_order_release);          // mark write start (odd)
        std::atomic_thread_fence(std::memory_order_release);
        data_ = val;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 1, std::memory_order_release);      // mark write done (even)
    }

    // Called by strategy threads (multiple readers)
    // Returns true if at least one write has occurred.
    bool read(T& out) const {
        uint64_t s1, s2;
        do {
            s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) continue;                         // write in progress
            std::atomic_thread_fence(std::memory_order_acquire);
            out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = seq_.load(std::memory_order_relaxed);
        } while (s1 != s2);
        return s1 > 0;
    }

private:
    alignas(64) std::atomic<uint64_t> seq_{0};  // on its own cache line
    T data_{};
};
