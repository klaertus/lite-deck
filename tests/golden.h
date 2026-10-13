// tests/golden.h
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "modules/module.h"
#include "render/renderer.h"

// PNG files of 32-bit BGRA pixels, lossless: the reference images and the documentation pictures.
void write_png(ld::Renderer& r, const std::filesystem::path& file, int w, int h, const std::vector<std::uint8_t>& bgra);
// nullopt = missing or unreadable; an empty vector = readable but of another size.
std::optional<std::vector<std::uint8_t>> read_png(ld::Renderer& r, const std::filesystem::path& file, int w, int h);

// Renders `m` on a black w x h surface, as the Deck does.
std::unique_ptr<ld::Surface> draw(ld::Module& m, ld::Renderer& r, int w = 112, int h = 112);

// The same with the slot appearance `look` set on the canvas, as Deck::render_slot does.
std::unique_ptr<ld::Surface> draw(ld::Module& m, ld::Renderer& r, const ld::SlotLook& look, int w = 112, int h = 112);

// Compares `s` pixel for pixel with tests/golden/<name>.png. With LD_UPDATE_GOLDEN=1 in the
// environment, writes that file instead. On a difference, saves the actual image as
// %TEMP%\ld_golden_actual\<name>.png and fails with the number of differing pixels.
void check_golden(ld::Renderer& r, ld::Surface& s, const std::string& name);

// Lit pixels (r + g + b > 90) in [x0, x1) x [y0, y1); x1 or y1 = -1: up to the edge.
struct Ink {
  int count = 0;
  int left = -1, top = -1, right = -1, bottom = -1;  // bounding box; -1 when count == 0
  int height() const { return count ? bottom - top + 1 : 0; }
};
Ink ink(ld::Surface& s, int x0 = 0, int y0 = 0, int x1 = -1, int y1 = -1);
// Pixels whose channels spread by more than 80 (a colour, not a grey) in [x0, x1) x [y0, y1).
int saturated(ld::Surface& s, int x0, int y0, int x1, int y1);
// True if rows [y0, y1) of two surfaces of the same size are byte for byte identical.
bool same_rows(ld::Surface& a, ld::Surface& b, int y0, int y1);
