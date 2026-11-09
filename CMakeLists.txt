cmake_minimum_required(VERSION 3.16)
project(arb_bot VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Dependencies ───────────────────────────────────────────────────────────────
find_package(CURL     REQUIRED)
find_package(OpenSSL  REQUIRED)
find_package(Threads  REQUIRED)
find_package(Boost 1.74 REQUIRED COMPONENTS system)

# ── FIX #2: nlohmann/json ──────────────────────────────────────────────────────
# OLD (broken): downloaded json.hpp directly with DOWNLOAD_NO_EXTRACT TRUE,
#               producing a flat file at json_SOURCE_DIR/json.hpp.
#               #include <nlohmann/json.hpp> then fails — no nlohmann/ subdir.
#
# NEW: download the official include.zip release which contains the proper
#      nlohmann/ directory structure. Point COMMON_INCLUDES at its include/ root.
include(FetchContent)
FetchContent_Declare(json
  URL      https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip
  URL_HASH SHA256=a22461d13119ac5c78f205d3df1db13403e58ce1bb1794edc9313677313f4a9d
)
FetchContent_MakeAvailable(json)

# ── Common include dirs ────────────────────────────────────────────────────────
# FIX #2 (cont.): include path is now ${json_SOURCE_DIR}/include, not
#                  ${json_SOURCE_DIR} — this is where nlohmann/ lives.
set(COMMON_INCLUDES
  ${CMAKE_SOURCE_DIR}
  ${json_SOURCE_DIR}/include
)

set(COMMON_LIBS
  CURL::libcurl
  OpenSSL::SSL
  OpenSSL::Crypto
  Boost::system
  Threads::Threads
  ws2_32
)

# ── arb_bot binary ─────────────────────────────────────────────────────────────
add_executable(arb_bot
  src/main.cpp
  feed/smart_stream.cpp
  strategy/arb_engine.cpp
  execution/order_manager.cpp
  risk/risk_manager.cpp
  alerts/telegram.cpp
  latency/latency_tracker.cpp
  intelligence/market_guard.cpp
  intelligence/dynamic_hedge.cpp
)
target_include_directories(arb_bot PRIVATE ${COMMON_INCLUDES})
target_link_libraries(arb_bot PRIVATE ${COMMON_LIBS})

# ── arb_watchdog binary ────────────────────────────────────────────────────────
add_executable(arb_watchdog
  src/watchdog_main.cpp
  watchdog/watchdog.cpp
  alerts/telegram.cpp
)
target_include_directories(arb_watchdog PRIVATE ${COMMON_INCLUDES})
target_link_libraries(arb_watchdog PRIVATE ${COMMON_LIBS})
