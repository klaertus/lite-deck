// src/modules/key_module.h: the base of key modules. A key module collects its data and
// says what to show; the key engine draws it. render() stays the Deck's entry point, so the Deck,
// the reference images and the "!" of a failing module work unchanged.
#pragma once
#include <span>
#include <vector>
#include "modules/module.h"
#include "render/key_renderer.h"

namespace ld {

class KeyModule : public Module {
public:
  // `elements` must outlive the module: a static array of the module's .cpp (its ModuleInfo copies it).
  explicit KeyModule(std::span<const ElementSpec> elements) : elements_(elements) {}
  // What the key shows now. Called only after invalidate(), like render(); an exception is a
  // render failure (the key shows "!").
  virtual std::vector<KeyElement> content() = 0;
  void render(Canvas& c) override { render_key(c, content(), elements_); }

private:
  std::span<const ElementSpec> elements_;
};

}  // namespace ld
