#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "core/types.h"
#include "device/device.h"

namespace ld::n4pro::proto {

constexpr std::size_t kReportOut = 1025;  // report id + 1024
constexpr std::size_t kReportIn = 513;    // report id + 512
constexpr std::size_t kChunk = kReportOut - 1;
using Report = std::array<std::uint8_t, kReportOut>;

Report dis();
Report stp();
Report connect();
Report han();
Report disconnect();                       // "CLE\0\0DC"
Report brightness(std::uint8_t pct);       // "LIG\0\0" + pct
Report clear(std::uint8_t index);          // "CLE\0\0\0" + index (0xFF = all)
Report bg_clear(std::uint8_t pos);         // "BGCLE" + pos (3 = all)
Report ring_brightness(std::uint8_t pct);  // "LBLIG" + pct
Report ring_colors(const std::array<Rgb, 4>& colors);  // "SETLB" + 4 x RGB

std::vector<Report> image_key(std::uint8_t index, std::span<const std::uint8_t> jpeg);
std::vector<Report> image_bg(std::span<const std::uint8_t> jpeg, std::uint16_t x, std::uint16_t y,
                             std::uint16_t w, std::uint16_t h, std::uint8_t layer = 0);

// Slot 0-4 = top row, 5-9 = bottom row, left to right.
std::uint8_t key_image_index(int slot);

enum class RawKind { None, WriteAck, KeyDown, KeyUp, KnobTurn, KnobPress, TouchDown, TouchMove, TouchUp };
struct RawInput {
  RawKind kind = RawKind::None;
  int index = 0;  // key slot 0-9 or knob 0-3
  int delta = 0;  // knob: -1 counter-clockwise, +1 clockwise
  int x = 0;      // touch x (0-800)
  int y = 0;      // touch y (TouchMove only)
};

// `report` includes the leading report id byte, as returned by ReadFile.
RawInput decode(std::span<const std::uint8_t> report);
// The generic form of a decoded report: WriteAck and None become InputKind::None.
DeviceInput to_device_input(const RawInput& in);

}  // namespace ld::n4pro::proto
