#include <doctest/doctest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include "device/registry.h"
#include "device/simulator.h"
#include "fake_device.h"
#include "fake_loop.h"
#include "image_util.h"

using namespace ld;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

TEST_CASE("parse_sim_script reads every command") {
  std::string err;
  auto steps = parse_sim_script("# demo\nkeydown 3\nwait 600\nkeyup 3\nturn 1 -1\npress 2\ntouchdown 400\ntouchmove 450 50\ntouchup\n", default_device().layout, &err);
  CHECK(err.empty());
  REQUIRE(steps.size() == 8);
  CHECK(steps[0].input.kind == InputKind::KeyDown);
  CHECK(steps[0].input.index == 3);
  CHECK(steps[1].wait == 600ms);
  CHECK(steps[3].input.kind == InputKind::KnobTurn);
  CHECK(steps[3].input.delta == -1);
  CHECK(steps[6].input.x == 450);
  CHECK(steps[6].input.y == 50);
  CHECK(steps[7].input.kind == InputKind::TouchUp);
}

TEST_CASE("parse_sim_script reports the faulty line") {
  std::string err;
  auto steps = parse_sim_script("keydown 3\nfly 2\n", default_device().layout, &err);
  CHECK(steps.empty());
  CHECK(err.find("2") != std::string::npos);
  parse_sim_script("keydown 12\n", default_device().layout, &err);
  CHECK_FALSE(err.empty());
}

TEST_CASE("SimulatorSink writes images as JPEG files and ring colors") {
  fs::path dir = fs::temp_directory_path() / "ld_sim_out";
  fs::remove_all(dir);
  SimulatorSink sink(dir);
  sink.key_image(4, SolidImage(112, 112, Rgb{255, 0, 0}).image());
  sink.strip_image(SolidImage(800, 112, Rgb{0, 0, 255}).image());
  const std::array<Rgb, 4> rings{Rgb{255, 0, 0}, Rgb{}, Rgb{}, Rgb{0, 0, 255}};
  sink.rings(rings, 80);
  CHECK(fs::file_size(dir / "key4.jpg") > 100);
  {
    std::ifstream f(dir / "key4.jpg", std::ios::binary);
    unsigned char head[2]{};
    f.read(reinterpret_cast<char*>(head), 2);
    CHECK(head[0] == 0xFF);
    CHECK(head[1] == 0xD8);
  }
  CHECK(fs::file_size(dir / "strip.jpg") > 100);
  {
    std::ifstream f(dir / "rings.txt");
    std::string line;
    std::getline(f, line);
    CHECK(line == "#ff0000 #000000 #000000 #0000ff 80");
  }
  fs::remove_all(dir);
}

TEST_CASE("SimInjector runs a dropped script with waits and deletes it") {
  fs::path dir = fs::temp_directory_path() / "ld_sim_in";
  fs::remove_all(dir);
  fs::create_directories(dir);
  FakeLoop loop;
  std::vector<DeviceInput> got;
  SimInjector inj(loop, dir / "inject.txt", default_device().layout, [&](const DeviceInput& in) { got.push_back(in); });
  { std::ofstream(dir / "inject.txt") << "keydown 1\nwait 600\nkeyup 1\n"; }
  loop.advance(300ms);
  CHECK(got.size() == 1);
  CHECK_FALSE(fs::exists(dir / "inject.txt"));
  loop.advance(700ms);
  REQUIRE(got.size() == 2);
  CHECK(got[1].kind == InputKind::KeyUp);
  fs::remove_all(dir);
}

TEST_CASE("parse_sim_script follows the device: 15 keys and 2 knobs on the Test Deck, no touch strip") {
  std::string err;
  const DeviceLayout& td = fake::test_deck_layout();
  auto steps = parse_sim_script("keydown 14\nturn 1 -1\npress 1\n", td, &err);
  CHECK(err.empty());
  REQUIRE(steps.size() == 3);
  CHECK(steps[0].input == DeviceInput{InputKind::KeyDown, 14});
  CHECK(steps[1].input == DeviceInput{InputKind::KnobTurn, 1, -1});
  parse_sim_script("keydown 15\n", td, &err);
  CHECK(err == "line 1: key slot 0-14 expected");
  parse_sim_script("turn 2 1\n", td, &err);
  CHECK(err == "line 1: turn <0-1> <+1|-1> expected");
  parse_sim_script("touchdown 10\n", td, &err);
  CHECK(err == "line 1: this device has no touch strip");
  parse_sim_script("keydown 10\n", default_device().layout, &err);
  CHECK(err == "line 1: key slot 0-9 expected");
  DeviceLayout knobless = td;
  knobless.knobs = 0;
  parse_sim_script("press 0\n", knobless, &err);
  CHECK(err == "line 1: this device has no knobs");
}
