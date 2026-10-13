// tests/n4pro_reference.h: the N4 Pro wire format and JPEG pipeline as they were on 2026-10-13,
// written independently of src/device so that moving the driver cannot move the reference.
// Never change these to make a test pass: they define "identical HID traffic".
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>
#include "render/renderer.h"

namespace ref {

using Bytes = std::vector<std::uint8_t>;

// "CRT\0\0" + tag + args in a 1025-byte report (report id 0 first).
Bytes command(std::string_view tag, std::initializer_list<std::uint8_t> args = {});
// BAT header (JPEG length big-endian, wire index), then the JPEG in 1024-byte chunks.
std::vector<Bytes> image_key(std::uint8_t index, const Bytes& jpeg);
// BGPIC header (length, x, y, w, h big-endian, 0, layer 0), then the JPEG in 1024-byte chunks.
std::vector<Bytes> image_bg(const Bytes& jpeg, std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h);
// Slot 0-4 = top row (wire 11-15), 5-9 = bottom row (wire 6-10).
std::uint8_t key_index(int slot);
// The surface's pixels encoded as Surface::encode_jpeg did on 2026-10-13: 32bppPBGRA -> optional
// 180° rotation -> 24bppBGR -> WIC JPEG at `quality` / 100.
Bytes jpeg(ld::Renderer& r, ld::Surface& s, int quality, bool rotate180);

}  // namespace ref
