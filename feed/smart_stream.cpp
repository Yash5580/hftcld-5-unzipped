#include "smart_stream.h"
#include "../logger/logger.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <thread>
#include <sstream>
#include <iomanip>
#include <stdexcept>

using json = nlohmann::json;

// ─── libcurl write callback ───────────────────────────────────────────────────
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// ─────────────────────────────────────────────────────────────────────────────
SmartStream::SmartStream(const AppConfig& cfg,
                          asio::io_context& ioc,
                          OnDepthCb on_depth_cb)
    : cfg_(cfg), ioc_(ioc), on_depth_cb_(std::move(on_depth_cb)) {}

SmartStream::~SmartStream() { stop(); }

void SmartStream::stop() { stop_.store(true); }

// ─── Entry point ─────────────────────────────────────────────────────────────
void SmartStream::start() {
    int attempt = 0;
    const auto& wd = cfg_.watchdog;

    while (!stop_.load()) {
        try {
            LOG_SYSTEM("SmartStream: logging in to Angel One...");
            AuthTokens tokens = login();
            LOG_SYSTEM("SmartStream: login OK, connecting WebSocket...");
            run_session(tokens);
        } catch (const std::exception& ex) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "SmartStream error: %s", ex.what());
            LOG_RISK(buf);
        }

        if (stop_.load()) break;

        int delay_ms = (attempt < 3)
                       ? wd.backoff_ms[attempt]
                       : wd.backoff_ms[2];
        ++attempt;

        char buf[80];
        std::snprintf(buf, sizeof(buf),
            "SmartStream: reconnecting in %d ms (attempt %d)...", delay_ms, attempt);
        LOG_SYSTEM(buf);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    LOG_SYSTEM("SmartStream: stopped.");
}

// ─── TOTP — RFC 6238 / HMAC-SHA1 ─────────────────────────────────────────────
// Decodes base32, computes HOTP(K, T) where T = epoch / 30.
std::string SmartStream::generate_totp(const std::string& b32) const {
    // ── Base32 decode ─────────────────────────────────────────────────────────
    static const char* B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string clean;
    for (char c : b32) {
        if (c != '=') clean += toupper(c);
    }

    std::vector<uint8_t> key;
    key.reserve(clean.size() * 5 / 8);
    uint32_t buf_bits = 0;
    int      bits     = 0;
    for (char c : clean) {
        const char* p = strchr(B32, c);
        if (!p) continue;
        buf_bits = (buf_bits << 5) | (uint32_t)(p - B32);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            key.push_back((uint8_t)(buf_bits >> bits));
        }
    }

    // ── T = current 30-second window ─────────────────────────────────────────
    uint64_t T = (uint64_t)std::time(nullptr) / 30;
    // Big-endian byte order (as required by RFC 4226)
    uint8_t msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = T & 0xFF; T >>= 8; }

    // ── HMAC-SHA1 ─────────────────────────────────────────────────────────────
    uint8_t  hmac[20];
    uint32_t hmac_len = 20;
    HMAC(EVP_sha1(), key.data(), (int)key.size(),
         msg, 8, hmac, &hmac_len);

    // ── Dynamic truncation ────────────────────────────────────────────────────
    int    offset = hmac[19] & 0x0F;
    uint32_t code = ((hmac[offset]     & 0x7F) << 24) |
                    ((hmac[offset + 1] & 0xFF) << 16) |
                    ((hmac[offset + 2] & 0xFF) <<  8) |
                     (hmac[offset + 3] & 0xFF);
    code %= 1'000'000;

    char out[8];
    std::snprintf(out, sizeof(out), "%06u", code);
    return std::string(out);
}

// ─── Login via Angel One REST API ────────────────────────────────────────────
SmartStream::AuthTokens SmartStream::login() {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string response;
    std::string totp = generate_totp(cfg_.angel.totp_secret);

    // Build JSON body
    json body = {
        {"clientcode", cfg_.angel.client_id},
        {"password",   cfg_.angel.password},
        {"totp",       totp}
    };
    std::string body_str = body.dump();

    // Build custom headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "X-UserType: USER");
    headers = curl_slist_append(headers, "X-SourceID: WEB");

    // PrivateKey header = api_key
    std::string pk_hdr = "X-PrivateKey: " + cfg_.angel.api_key;
    headers = curl_slist_append(headers, pk_hdr.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,
        "https://apiconnect.angelone.in/rest/auth/angelbroking/user/v1/loginByPassword");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw std::runtime_error(std::string("Login curl error: ") + curl_easy_strerror(res));

    json resp = json::parse(response);
    if (!resp.value("status", false))
        throw std::runtime_error("Login failed: " + resp.dump());

    AuthTokens tokens;
    tokens.jwt        = resp["data"]["jwtToken"].get<std::string>();
    tokens.feed_token = resp["data"]["feedToken"].get<std::string>();

    LOG_SYSTEM("SmartStream: authenticated successfully");
    return tokens;
}

// ─── Build JSON subscription message ─────────────────────────────────────────
void SmartStream::subscribe(ws::stream<beast::ssl_stream<beast::tcp_stream>>& wss,
                              const AuthTokens& /*tokens*/)
{
    // Group tokens by exchange
    std::vector<std::string> nse_tokens, bse_tokens;
    for (auto& s : cfg_.symbols) {
        nse_tokens.push_back(s.nse_token);
        bse_tokens.push_back(s.bse_token);
    }

    json sub = {
        {"correlationID", "arb_bot_v5"},
        {"action", 1},   // 1 = subscribe
        {"params", {
            {"mode", 3},  // Mode 3 = Snap Quote (full depth)
            {"tokenList", json::array({
                {{"exchangeType", 1}, {"tokens", nse_tokens}},  // 1 = NSE
                {{"exchangeType", 3}, {"tokens", bse_tokens}}   // 3 = BSE
            })}
        }}
    };

    wss.write(asio::buffer(sub.dump()));
    LOG_SYSTEM("SmartStream: subscribed to " +
               std::to_string(cfg_.symbols.size()) + " symbols (Mode 3)");
}

// ─── WebSocket session ────────────────────────────────────────────────────────
void SmartStream::run_session(const AuthTokens& tokens) {
    // ── Resolve host ──────────────────────────────────────────────────────────
    asio::ip::tcp::resolver resolver(ioc_);
    auto const results = resolver.resolve(
        "smartapisocket.angelone.in", "443");

    // ── SSL context ───────────────────────────────────────────────────────────
    ssl::context ssl_ctx(ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    // ── Beast WebSocket over SSL ──────────────────────────────────────────────
    ws::stream<beast::ssl_stream<beast::tcp_stream>> wss(ioc_, ssl_ctx);

    // Connect TCP
    beast::get_lowest_layer(wss).connect(results);
    beast::get_lowest_layer(wss).expires_after(std::chrono::seconds(30));

    // TLS handshake
    SSL_set_tlsext_host_name(wss.next_layer().native_handle(),
                              "smartapisocket.angelone.in");
    wss.next_layer().handshake(ssl::stream_base::client);

    // WebSocket handshake with Angel One auth headers
    wss.set_option(ws::stream_base::decorator(
        [&](ws::request_type& req) {
            req.set("Authorization", "Bearer " + tokens.jwt);
            req.set("x-api-key",    cfg_.angel.api_key);
            req.set("x-client-code",cfg_.angel.client_id);
            req.set("x-feed-token", tokens.feed_token);
        }
    ));
    wss.handshake("smartapisocket.angelone.in", "/smart-stream");

    LOG_SYSTEM("SmartStream: WebSocket connected");

    // Subscribe
    subscribe(wss, tokens);

    // ── Heartbeat timer ───────────────────────────────────────────────────────
    asio::steady_timer hb_timer(ioc_);
    std::function<void()> arm_hb = [&]() {
        hb_timer.expires_after(
            std::chrono::milliseconds(cfg_.feed.heartbeat_ms));
        hb_timer.async_wait([&](const boost::system::error_code& ec) {
            if (ec || stop_.load()) return;
            try {
                wss.ping({});
                LOG_SYSTEM("SmartStream: heartbeat sent");
            } catch (...) {}
            arm_hb();
        });
    };
    arm_hb();

    // ── Read loop ─────────────────────────────────────────────────────────────
    beast::flat_buffer rx_buf;
    while (!stop_.load()) {
        boost::system::error_code ec;
        wss.read(rx_buf, ec);
        if (ec) break;

        auto t0 = std::chrono::steady_clock::now();
        const uint8_t* data = static_cast<const uint8_t*>(rx_buf.data().data());
        size_t len = rx_buf.data().size();

        // Text frames are control messages (pong, subscription ack) — skip
        if (!wss.got_binary()) {
            rx_buf.consume(len);
            continue;
        }

        if (len >= MODE3_MIN_PACKET_SIZE) {
            parse_packet(data, len);
        }
        rx_buf.consume(len);
    }

    hb_timer.cancel();
    boost::system::error_code ec;
    wss.close(ws::close_code::normal, ec);
}

// ─── Parse one Mode 3 (Snap Quote) binary packet ─────────────────────────────
void SmartStream::parse_packet(const uint8_t* buf, size_t len) {
    if (len < MODE3_MIN_PACKET_SIZE) return;

    uint8_t sub_mode     = read_le<uint8_t>(buf, PKT_SUBSCRIPTION_MODE);
    uint8_t exchange_type = read_le<uint8_t>(buf, PKT_EXCHANGE_TYPE);

    // Extract null-terminated token string (25 bytes)
    char token_str[26]{};
    memcpy(token_str, buf + PKT_TOKEN, 25);
    token_str[25] = '\0';
    std::string token(token_str);

    int64_t exchange_ts_ms = read_le<int64_t>(buf, PKT_EXCHANGE_TS);

    // ── Build DepthBook ───────────────────────────────────────────────────────
    // Angel One sends depth as: 5 buy entries then 5 sell entries.
    // Each entry: 2 bytes num_orders + 8 bytes price (paise int64)
    //             + 8 bytes qty (int64) = 18 bytes.
    //
    // Prices are already in paise from Angel One — stored directly as int32_t.
    // (Angel One uses int64 in the packet; cast to int32_t is safe for
    //  Indian equity prices which are well within int32_t range in paise.)
    //
    // ⚠️  Verify PKT_DEPTH_START (131) against actual live packets.

    DepthBook book;
    book.valid = true;
    book.ts_ms = exchange_ts_ms;

    size_t depth_off = PKT_DEPTH_START;

    // Best 5 buy (bids)
    for (int i = 0; i < DepthBook::DEPTH; ++i, depth_off += PKT_DEPTH_ENTRY_SIZE) {
        if (depth_off + PKT_DEPTH_ENTRY_SIZE > len) { book.valid = false; break; }
        // num_orders at depth_off+0 (2 bytes) — not needed for arb logic
        int64_t price = read_le<int64_t>(buf, depth_off + 2);
        int64_t qty   = read_le<int64_t>(buf, depth_off + 10);
        book.bids[i].price = static_cast<int32_t>(price);  // paise — FIX #4
        book.bids[i].qty   = static_cast<int32_t>(qty);
    }

    // Best 5 sell (asks)
    for (int i = 0; i < DepthBook::DEPTH; ++i, depth_off += PKT_DEPTH_ENTRY_SIZE) {
        if (depth_off + PKT_DEPTH_ENTRY_SIZE > len) { book.valid = false; break; }
        int64_t price = read_le<int64_t>(buf, depth_off + 2);
        int64_t qty   = read_le<int64_t>(buf, depth_off + 10);
        book.asks[i].price = static_cast<int32_t>(price);  // paise — FIX #4
        book.asks[i].qty   = static_cast<int32_t>(qty);
    }

    if (book.valid && on_depth_cb_) {
        on_depth_cb_(token, exchange_type, book,
                     std::chrono::steady_clock::now());
    }
    (void)sub_mode;
}
