#pragma once
#include <string>
#include "../config/config.h"

class TelegramAlerter {
public:
    explicit TelegramAlerter(const TelegramCfg& cfg) : cfg_(cfg) {}
    void send(const std::string& text) const;
private:
    const TelegramCfg& cfg_;
};