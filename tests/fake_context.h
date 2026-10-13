// tests/fake_context.h
#pragma once
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "fake_loop.h"
#include "modules/module.h"
#include "render/renderer.h"

// A ModuleContext for unit-testing one module without a Deck.
class FakeContext final : public ld::ModuleContext {
public:
  explicit FakeContext(FakeLoop& loop) : loop_(loop) {
    target_->run = [](const std::function<void()>& f) { f(); };
  }
  ~FakeContext() override {
    target_->alive.store(false);
    for (auto id : timers_) loop_.cancel(id);
  }

  int invalidations = 0;
  // Every request asked: its url (first) and its callback (second); `sent` holds the whole request.
  std::vector<std::pair<std::string, std::function<void(const ld::HttpResponse&)>>> requests;
  std::vector<ld::HttpRequest> sent;
  nlohmann::json state;
  std::optional<ld::Rgb> ring;

  // Completes request `i` (on the "loop": callbacks run immediately, like after a drain).
  void respond(std::size_t i, ld::HttpResponse r) {
    auto cb = requests.at(i).second;
    cb(r);
  }

  ld::TimePoint now() const override { return loop_.now(); }
  ld::TimerId at(ld::TimePoint when, std::function<void()> fn) override {
    auto id = loop_.at(when, std::move(fn));
    timers_.insert(id);
    return id;
  }
  ld::TimerId every(ld::Duration p, std::function<void()> fn) override {
    auto id = loop_.every(p, std::move(fn));
    timers_.insert(id);
    return id;
  }
  void cancel(ld::TimerId id) override {
    timers_.erase(id);
    loop_.cancel(id);
  }
  void post(std::function<void()> fn) override { poster().post(std::move(fn)); }
  ld::Poster poster() override { return ld::Poster(loop_, target_); }
  void invalidate() override { ++invalidations; }
  void set_ring_color(ld::Rgb c) override { ring = c; }
  void clear_ring_color() override { ring.reset(); }
  using ld::ModuleContext::http_get;
  void http_get(const ld::HttpRequest& r, std::function<void(const ld::HttpResponse&)> done) override {
    requests.emplace_back(r.url, std::move(done));
    sent.push_back(r);
  }
  nlohmann::json load_state() override { return state; }
  void save_state(const nlohmann::json& s) override { state = s; }

private:
  FakeLoop& loop_;
  std::set<ld::TimerId> timers_;
  std::shared_ptr<ld::PostTarget> target_ = std::make_shared<ld::PostTarget>();
};

// Renders a module on a black w x h surface; returns how many sampled pixels are not black.
inline int render_lit_pixels(ld::Module& m, ld::Renderer& r, int w = 112, int h = 112) {
  ld::Surface s(r, w, h);
  {
    ld::Canvas c = s.canvas();
    c.clear(ld::Rgb{0, 0, 0});
    m.render(c);
  }
  int lit = 0;
  for (int y = 0; y < h; y += 2)
    for (int x = 0; x < w; x += 2) {
      auto p = s.pixel(x, y);
      if (p.r + p.g + p.b > 90) ++lit;
    }
  return lit;
}
