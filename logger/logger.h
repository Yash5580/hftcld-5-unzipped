#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// logger/logger.h
// Thread-safe, timestamped logging.
// Three channels written to separate files + stdout:
//   LOG_SYSTEM → system.log   (boot, config, reconnects)
//   LOG_TRADE  → trade.log    (signals, fills, P&L)
//   LOG_RISK   → risk.log     (hedges, risk events)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <fstream>

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void write(const char* channel, FILE* console_stream,
               std::ofstream& file_stream, const char* msg) {
        std::lock_guard<std::mutex> lk(mtx_);

        // Timestamp
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm* tm_info = localtime(&ts.tv_sec);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_info);
        long ms = ts.tv_nsec / 1'000'000;

        char line[512];
        std::snprintf(line, sizeof(line), "[%s.%03ld][%s] %s\n",
                      tbuf, ms, channel, msg);

        fputs(line, console_stream);
        if (file_stream.is_open()) file_stream << line;
        fflush(console_stream);
    }

    std::ofstream sys_file{"system.log",  std::ios::app};
    std::ofstream trd_file{"trade.log",   std::ios::app};
    std::ofstream rsk_file{"risk.log",    std::ios::app};

private:
    Logger() = default;
    std::mutex mtx_;
};

// ── Convenience macros ────────────────────────────────────────────────────────
#define LOG_SYSTEM(msg) \
    Logger::instance().write("SYS", stdout, Logger::instance().sys_file, \
        (std::string(msg)).c_str())

#define LOG_TRADE(msg) \
    Logger::instance().write("TRD", stdout, Logger::instance().trd_file, \
        (std::string(msg)).c_str())

#define LOG_RISK(msg) \
    Logger::instance().write("RSK", stderr, Logger::instance().rsk_file, \
        (std::string(msg)).c_str())
