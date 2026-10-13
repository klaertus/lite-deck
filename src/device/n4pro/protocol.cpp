#include "device/n4pro/protocol.h"
#include <algorithm>
#include <string_view>

namespace ld::n4pro::proto {
using namespace std::string_view_literals;

static Report command(std::string_view body) {
  Report r{};
  constexpr std::string_view header = "CRT\0\0"sv;
  std::copy(header.begin(), header.end(), r.begin() + 1);
  std::copy(body.begin(), body.end(), r.begin() + 1 + header.size());
  return r;
}

static Report command_with(std::string_view tag, std::initializer_list<std::uint8_t> args) {
  Report r = command(tag);
  std::size_t at = 1 + 5 + tag.size();
  for (std::uint8_t a : args) r[at++] = a;
  return r;
}

Report dis() { return command("DIS"sv); }
Report stp() { return command("STP"sv); }
Report connect() { return command("CONNECT"sv); }
Report han() { return command("HAN"sv); }
Report disconnect() { return command("CLE\0\0DC"sv); }
Report brightness(std::uint8_t pct) { return command_with("LIG\0\0"sv, {pct}); }
Report clear(std::uint8_t index) { return command_with("CLE\0\0\0"sv, {index}); }
Report bg_clear(std::uint8_t pos) { return command_with("BGCLE"sv, {pos}); }
Report ring_brightness(std::uint8_t pct) { return command_with("LBLIG"sv, {pct}); }

Report ring_colors(const std::array<Rgb, 4>& c) {
  return command_with("SETLB"sv, {c[0].r, c[0].g, c[0].b, c[1].r, c[1].g, c[1].b,
                                  c[2].r, c[2].g, c[2].b, c[3].r, c[3].g, c[3].b});
}

static void append_chunks(std::vector<Report>& out, std::span<const std::uint8_t> data) {
  for (std::size_t off = 0; off < data.size(); off += kChunk) {
    Report r{};
    std::size_t n = std::min(kChunk, data.size() - off);
    std::copy_n(data.begin() + off, n, r.begin() + 1);
    out.push_back(r);
  }
}

static std::uint8_t byte(std::uint32_t v, int shift) { return static_cast<std::uint8_t>((v >> shift) & 0xFF); }

std::vector<Report> image_key(std::uint8_t index, std::span<const std::uint8_t> jpeg) {
  auto len = static_cast<std::uint32_t>(jpeg.size());
  std::vector<Report> out;
  out.push_back(command_with("BAT"sv, {byte(len, 24), byte(len, 16), byte(len, 8), byte(len, 0), index}));
  append_chunks(out, jpeg);
  return out;
}

std::vector<Report> image_bg(std::span<const std::uint8_t> jpeg, std::uint16_t x, std::uint16_t y,
                             std::uint16_t w, std::uint16_t h, std::uint8_t layer) {
  auto len = static_cast<std::uint32_t>(jpeg.size());
  std::vector<Report> out;
  out.push_back(command_with("BGPIC"sv, {byte(len, 24), byte(len, 16), byte(len, 8), byte(len, 0),
                                         byte(x, 8), byte(x, 0), byte(y, 8), byte(y, 0),
                                         byte(w, 8), byte(w, 0), byte(h, 8), byte(h, 0), 0, layer}));
  append_chunks(out, jpeg);
  return out;
}

std::uint8_t key_image_index(int slot) {
  return static_cast<std::uint8_t>(slot < 5 ? 11 + slot : 1 + slot);
}

static int be16(std::span<const std::uint8_t> r, std::size_t at) { return (r[at] << 8) | r[at + 1]; }

RawInput decode(std::span<const std::uint8_t> r) {
  if (r.size() < 15 || r[1] != 'A' || r[2] != 'C' || r[3] != 'K') return {};
  if (r[5] == 'A' && r[6] == 'R' && r[7] == 'X')
    return {RawKind::TouchMove, 0, 0, be16(r, 11), be16(r, 13)};
  if (r[6] != 'O' || r[7] != 'K') return {};

  const std::uint8_t code = r[10], state = r[11];
  if (code == 0xFF) return {RawKind::WriteAck};
  if (code >= 0x01 && code <= 0x0A) return {state ? RawKind::KeyDown : RawKind::KeyUp, code - 1};
  switch (code) {
    case 0xA0: return {RawKind::KnobTurn, 0, -1};
    case 0xA1: return {RawKind::KnobTurn, 0, +1};
    case 0x50: return {RawKind::KnobTurn, 1, -1};
    case 0x51: return {RawKind::KnobTurn, 1, +1};
    case 0x90: return {RawKind::KnobTurn, 2, -1};
    case 0x91: return {RawKind::KnobTurn, 2, +1};
    case 0x70: return {RawKind::KnobTurn, 3, -1};
    case 0x71: return {RawKind::KnobTurn, 3, +1};
    case 0x37: return {RawKind::KnobPress, 0};
    case 0x35: return {RawKind::KnobPress, 1};
    case 0x33: return {RawKind::KnobPress, 2};
    case 0x36: return {RawKind::KnobPress, 3};
    case 0x00: return state == 1 ? RawInput{RawKind::TouchDown, 0, 0, be16(r, 12)} : RawInput{};
    case 0x40: case 0x41: case 0x42: case 0x43:
      return {RawKind::TouchUp, 0, 0, be16(r, 12)};
    case 0x38: case 0x39:
      return {RawKind::TouchUp};
    default:
      return {};
  }
}

DeviceInput to_device_input(const RawInput& in) {
  InputKind k = InputKind::None;
  switch (in.kind) {
    case RawKind::KeyDown: k = InputKind::KeyDown; break;
    case RawKind::KeyUp: k = InputKind::KeyUp; break;
    case RawKind::KnobTurn: k = InputKind::KnobTurn; break;
    case RawKind::KnobPress: k = InputKind::KnobPress; break;
    case RawKind::TouchDown: k = InputKind::TouchDown; break;
    case RawKind::TouchMove: k = InputKind::TouchMove; break;
    case RawKind::TouchUp: k = InputKind::TouchUp; break;
    case RawKind::None:
    case RawKind::WriteAck: return {};
  }
  return DeviceInput{k, in.index, in.delta, in.x, in.y};
}

}  // namespace ld::n4pro::proto
