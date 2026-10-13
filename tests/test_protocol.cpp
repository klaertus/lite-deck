#include <doctest/doctest.h>
#include "device/n4pro/protocol.h"
#include "test_util.h"

using namespace ld::n4pro::proto;

TEST_CASE("command reports start with report id and CRT header") {
  Report r = stp();
  CHECK(r.size() == 1025);
  CHECK(r[0] == 0x00);
  CHECK(r[1] == 'C'); CHECK(r[2] == 'R'); CHECK(r[3] == 'T'); CHECK(r[4] == 0); CHECK(r[5] == 0);
  CHECK(report_tag(r) == "STP");
}

TEST_CASE("brightness, clear and LED commands put their argument at the right offset") {
  CHECK(brightness(60)[11] == 60);            // LIG\0\0 + pct
  CHECK(clear(0xFF)[12] == 0xFF);             // CLE\0\0\0 + index
  CHECK(report_tag(disconnect()) == "CLE");
  CHECK(disconnect()[11] == 'D'); CHECK(disconnect()[12] == 'C');
  CHECK(bg_clear(3)[11] == 3);                // BGCLE + pos
  CHECK(ring_brightness(0)[11] == 0);         // LBLIG + pct
  Report rc = ring_colors({ld::Rgb{1, 2, 3}, ld::Rgb{4, 5, 6}, ld::Rgb{7, 8, 9}, ld::Rgb{10, 11, 12}});
  CHECK(report_tag(rc) == "SETLB");
  for (int i = 0; i < 12; ++i) CHECK(rc[11 + i] == i + 1);
}

TEST_CASE("image_key sends BAT header with big-endian length then 1024-byte chunks") {
  std::vector<uint8_t> jpeg(2500);
  for (size_t i = 0; i < jpeg.size(); ++i) jpeg[i] = static_cast<uint8_t>(i);
  auto reps = image_key(13, jpeg);
  REQUIRE(reps.size() == 4);  // header + ceil(2500/1024)=3
  CHECK(report_tag(reps[0]) == "BAT");
  CHECK(reps[0][9] == 0x00); CHECK(reps[0][10] == 0x00); CHECK(reps[0][11] == 0x09); CHECK(reps[0][12] == 0xC4);
  CHECK(reps[0][13] == 13);
  CHECK(reps[1][0] == 0); CHECK(reps[1][1] == 0); CHECK(reps[1][1024] == static_cast<uint8_t>(1023));
  CHECK(reps[3][1] == static_cast<uint8_t>(2048));
  CHECK(reps[3][2500 - 2048 + 1] == 0);  // zero padding after data
}

TEST_CASE("image_bg sends BGPIC with length and rectangle") {
  std::vector<uint8_t> jpeg(10, 0xAB);
  auto reps = image_bg(jpeg, 0, 0, 800, 112);
  REQUIRE(reps.size() == 2);
  CHECK(report_tag(reps[0]) == "BGPIC");
  CHECK(reps[0][14] == 10);                               // length low byte (buf[11..14])
  CHECK(reps[0][19] == 0x03); CHECK(reps[0][20] == 0x20); // w = 800
  CHECK(reps[0][21] == 0x00); CHECK(reps[0][22] == 0x70); // h = 112
  CHECK(reps[0][24] == 0);                                // layer
  CHECK(reps[1][1] == 0xAB);
}

TEST_CASE("key_image_index maps slots to wire indices") {
  CHECK(key_image_index(0) == 11);
  CHECK(key_image_index(4) == 15);
  CHECK(key_image_index(5) == 6);
  CHECK(key_image_index(9) == 10);
}

TEST_CASE("decode keys, knobs and touch reports from real captures") {
  auto k = decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 0a 01"));
  CHECK(k.kind == RawKind::KeyDown); CHECK(k.index == 9);
  CHECK(decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 01 00")).kind == RawKind::KeyUp);

  auto ccw = decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 a0 00"));
  CHECK(ccw.kind == RawKind::KnobTurn); CHECK(ccw.index == 0); CHECK(ccw.delta == -1);
  auto cw = decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 71 00"));
  CHECK(cw.kind == RawKind::KnobTurn); CHECK(cw.index == 3); CHECK(cw.delta == +1);
  auto press = decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 33 01"));
  CHECK(press.kind == RawKind::KnobPress); CHECK(press.index == 2);

  auto down = decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 00 01 00 79"));
  CHECK(down.kind == RawKind::TouchDown); CHECK(down.x == 121);
  auto move = decode(report_from_hex("00 41 43 4b 00 41 52 58 00 00 00 00 79 00 2e"));
  CHECK(move.kind == RawKind::TouchMove); CHECK(move.x == 121); CHECK(move.y == 46);
  auto up = decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 41 00 01 68"));
  CHECK(up.kind == RawKind::TouchUp); CHECK(up.x == 360);
  CHECK(decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 38 00")).kind == RawKind::TouchUp);

  CHECK(decode(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 ff 00")).kind == RawKind::WriteAck);
  CHECK(decode(report_from_hex("00 00 00 00")).kind == RawKind::None);
  CHECK(decode(std::vector<uint8_t>(4, 0)).kind == RawKind::None);  // too short
}
