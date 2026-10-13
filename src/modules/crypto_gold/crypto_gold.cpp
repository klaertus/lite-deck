// src/modules/crypto_gold/crypto_gold.cpp
#include "modules/crypto_gold/crypto_gold.h"
#include <cmath>
#include "core/http.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/key_module.h"

namespace ld {
using nlohmann::json;

std::optional<Prices> parse_prices(std::string_view body) {
  json j = json::parse(body, nullptr, false);
  try {
    if (!j.is_object()) return std::nullopt;
    return Prices{j.at("bitcoin").at("eur").get<double>(), j.at("tether-gold").at("eur").get<double>()};
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

namespace {

constexpr Rgb kOrange{0xf7, 0x93, 0x1a};  // bitcoin orange, the only data amber
constexpr ElementSpec kElements[] = {
    {.key = "status", .label = "Status", .size = 26, .weight = 700, .min_size = 10, .max_size = 40, .row = Row::Middle,
     .color = kOrange, .role = TextRole::Secondary},
    {.key = "symbol1", .label = "Bitcoin symbol", .size = 17, .weight = 600, .min_size = 9, .max_size = 40,
     .row = Row::Line1, .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Symbol},
    {.key = "line1", .label = "Bitcoin", .size = 26, .weight = 700, .min_size = 9, .max_size = 40, .row = Row::Line1,
     .align = HAlign::Right, .color = kOrange, .role = TextRole::TwoLine},
    {.key = "symbol2", .label = "Gold symbol", .size = 17, .weight = 600, .min_size = 9, .max_size = 40,
     .row = Row::Line2, .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Symbol},
    {.key = "line2", .label = "Gold", .size = 26, .weight = 700, .min_size = 9, .max_size = 40, .row = Row::Line2,
     .align = HAlign::Right, .color = kOrange, .role = TextRole::TwoLine},
};

class CryptoGoldModule final : public KeyModule {
public:
  CryptoGoldModule() : KeyModule(kElements) {}
  void start(ModuleContext& ctx, const nlohmann::json&) override {
    ctx_ = &ctx;
    fetch();
    ctx.every(kPricesPeriod, [this] { fetch(); });
  }
  void on_event(const InputEvent& e) override {
    if (e.kind == EventKind::Tap) fetch();
  }
  std::vector<KeyElement> content() override {
    if (!prices_) return {KeyElement{.key = "status", .text = kDash, .state = ElementState::Idle}};
    const ElementState st = stale_ ? ElementState::Stale : ElementState::Normal;
    return {KeyElement{.key = "symbol1", .text = L"₿", .state = st},
            KeyElement{.key = "line1", .text = format_grouped(std::llround(prices_->btc)), .state = st},
            KeyElement{.key = "symbol2", .text = L"Au", .state = st},
            KeyElement{.key = "line2", .text = format_grouped(std::llround(prices_->gold)), .state = st}};
  }

private:
  void fetch() {
    if (busy_) return;
    busy_ = true;
    ctx_->http_get(kPricesUrl, [this](const HttpResponse& r) {
      busy_ = false;
      auto p = r.ok() ? parse_prices(r.body) : std::nullopt;
      if (p) {
        bool changed = !prices_ || stale_ || std::llround(p->btc) != std::llround(prices_->btc) ||
                       std::llround(p->gold) != std::llround(prices_->gold);
        prices_ = p;
        stale_ = false;
        if (changed) ctx_->invalidate();
      } else if (prices_ && !stale_) {
        stale_ = true;
        ctx_->invalidate();
      }
    });
  }
  ModuleContext* ctx_ = nullptr;
  std::optional<Prices> prices_;
  bool stale_ = false;
  bool busy_ = false;
};

}  // namespace

ModuleInfo crypto_gold_module_info() {
  ModuleInfo m;
  m.id = "crypto_gold";
  m.name = "Bitcoin & gold";
  m.description = "Bitcoin and gold prices in euros. Tap to refresh.";
  m.category = "Online data";
  m.hue = "#f7931a";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"({"fields":[]})");
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return std::make_unique<CryptoGoldModule>(); };
  return m;
}

}  // namespace ld
