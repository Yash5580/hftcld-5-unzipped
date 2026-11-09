#include "telegram.h"
#include "../logger/logger.h"
#include <curl/curl.h>
#include <cstring>
#include <cstdio>

// Silence curl response body
static size_t discard_cb(void*, size_t sz, size_t nm, void*) { return sz * nm; }

void TelegramAlerter::send(const std::string& text) const {
    if (!cfg_.enabled || cfg_.bot_token.empty() || cfg_.chat_id.empty()) return;

    CURL* curl = curl_easy_init();
    if (!curl) return;

    char url[256];
    std::snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/sendMessage",
        cfg_.bot_token.c_str());

    // URL-encode message (basic: curl handles percent-encoding via POSTFIELDS)
    char post[1024];
    std::snprintf(post, sizeof(post),
        "chat_id=%s&text=%s&parse_mode=HTML",
        cfg_.chat_id.c_str(), text.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "Telegram send failed: %s", curl_easy_strerror(res));
        LOG_SYSTEM(buf);
    }
    curl_easy_cleanup(curl);
}
