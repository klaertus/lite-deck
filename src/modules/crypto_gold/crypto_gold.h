// src/modules/crypto_gold/crypto_gold.h
#pragma once
#include <optional>
#include <string_view>
#include "modules/module.h"

namespace ld {
struct Prices {
  double btc = 0, gold = 0;
};
inline constexpr const char* kPricesUrl =
    "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,tether-gold&vs_currencies=eur";
inline constexpr Duration kPricesPeriod{5 * 60 * 1000};
std::optional<Prices> parse_prices(std::string_view body);
ModuleInfo crypto_gold_module_info();
}  // namespace ld
