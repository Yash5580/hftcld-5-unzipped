#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// feed/smart_stream.h
//
// SmartStream: connects to Angel One SmartAPI WebSocket, authenticates,
// subscribes to Mode 3 (Snap Quote) depth packets for configured symbols,
// parses 340-byte binary packets, and fires on_depth callbacks.
//
// Flow:
//   1. login()       — POST to Angel One REST API → get JWT + feedToken
//   2. connect()     — open wss://smartapisocket.angelone.in/smart-stream
//   3. subscribe()   — send JSON subscription message
//   4. read loop     — parse binary packets → fire on_depth_cb_
//   5. heartbeat     — send ping every heartbeat_ms
//   6. reconnect     — on disconnect, wait reconnect_ms then restart
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include "../config/config.h"
#include "../strategy/order_book.h"

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace ssl   = boost::asio::ssl;
namespace ws    = beast::websocket;

using OnDepthCb = std::function<void(
    const std::string& token,
    uint8_t exchange_type,
    const DepthBook& book,
    std::chrono::steady_clock::time_point t0)>;

class SmartStream {
public:
    SmartStream(const AppConfig& cfg,
                asio::io_context& ioc,
                OnDepthCb on_depth_cb);
    ~SmartStream();

    // Authenticate and start feed. Blocks until stopped.
    void start();

    // Signal the feed loop to stop.
    void stop();

private:
    // ── Auth ─────────────────────────────────────────────────────────────────
    struct AuthTokens {
        std::string jwt;
        std::string feed_token;
    };
    AuthTokens login();
    std::string generate_totp(const std::string& base32_secret) const;

    // ── WebSocket session ─────────────────────────────────────────────────────
    void run_session(const AuthTokens& tokens);
    void subscribe(ws::stream<beast::ssl_stream<beast::tcp_stream>>& wss,
                   const AuthTokens& tokens);

    // ── Packet parsing ────────────────────────────────────────────────────────
    // Parse one 340-byte Mode 3 (Snap Quote) binary packet.
    // Prices from Angel One are already in paise — stored directly as int32_t.
    //
    // ⚠️  IMPORTANT: verify byte offsets against Angel One's official
    //     SmartAPI documentation before going live. The offsets below are
    //     based on the publicly documented Mode 3 packet layout.
    void parse_packet(const uint8_t* buf, size_t len);

    // Helper: safe unaligned read (little-endian, as Angel One sends)
    template<typename T>
    static T read_le(const uint8_t* buf, size_t offset) {
        T val;
        memcpy(&val, buf + offset, sizeof(T));
        return val;
    }

    // ── Heartbeat ─────────────────────────────────────────────────────────────
    void arm_heartbeat(ws::stream<beast::ssl_stream<beast::tcp_stream>>& wss);

    const AppConfig& cfg_;
    asio::io_context& ioc_;
    OnDepthCb        on_depth_cb_;
    std::atomic<bool> stop_{false};

    // ── Mode 3 Snap Quote packet layout constants ──────────────────────────────
    // All offsets in bytes from start of packet.
    static constexpr size_t PKT_SUBSCRIPTION_MODE =  0;  // 1 byte, uint8
    static constexpr size_t PKT_EXCHANGE_TYPE     =  1;  // 1 byte, uint8
    static constexpr size_t PKT_TOKEN             =  2;  // 25 bytes, ASCII string
    static constexpr size_t PKT_SEQUENCE_NUM      = 27;  // 8 bytes, int64
    static constexpr size_t PKT_EXCHANGE_TS       = 35;  // 8 bytes, int64 (ms)
    static constexpr size_t PKT_LTP               = 43;  // 8 bytes, int64 (paise)
    static constexpr size_t PKT_LTQ               = 51;  // 8 bytes, int64
    static constexpr size_t PKT_AVG_PRICE         = 59;  // 8 bytes, int64 (paise)
    static constexpr size_t PKT_VOLUME            = 67;  // 8 bytes, int64
    static constexpr size_t PKT_TOTAL_BUY_QTY    = 75;  // 8 bytes, double
    static constexpr size_t PKT_TOTAL_SELL_QTY   = 83;  // 8 bytes, double
    static constexpr size_t PKT_OPEN              = 91;  // 8 bytes, int64 (paise)
    static constexpr size_t PKT_HIGH              = 99;  // 8 bytes, int64 (paise)
    static constexpr size_t PKT_LOW               = 107; // 8 bytes, int64 (paise)
    static constexpr size_t PKT_CLOSE             = 115; // 8 bytes, int64 (paise)
    static constexpr size_t PKT_LT_TIMESTAMP      = 123; // 8 bytes, int64

    // Best 5 depth data starts at offset 131 (Mode 3 for equities).
    // Each depth entry: 2 bytes (num_orders) + 8 bytes (price) + 8 bytes (qty)
    //                 = 18 bytes per level × 10 levels (5 buy + 5 sell) = 180 bytes
    // Total: 131 + 180 = 311 bytes.
    // ⚠️ Angel One may include additional OI / feed-time fields before depth.
    //    Verify PKT_DEPTH_START against a hex dump of a live packet.
    static constexpr size_t PKT_DEPTH_START       = 131; // first depth entry
    static constexpr size_t PKT_DEPTH_ENTRY_SIZE  = 18;  // bytes per level
    static constexpr size_t MODE3_MIN_PACKET_SIZE = 311; // minimum valid size
};
