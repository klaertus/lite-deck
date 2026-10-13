#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include "core/state_store.h"
#include "fake_context.h"

namespace fs = std::filesystem;

TEST_CASE("StateStore persists values across instances") {
  fs::path dir = fs::temp_directory_path() / "ld_state";
  fs::remove_all(dir);
  {
    ld::StateStore s(dir / "state.json");
    CHECK(s.get("stopwatch:k9").is_null());
    s.set("stopwatch:k9", {{"running", true}, {"accumulated_ms", 1234}});
  }
  ld::StateStore again(dir / "state.json");
  CHECK(again.get("stopwatch:k9")["accumulated_ms"] == 1234);
  fs::remove_all(dir);
}

TEST_CASE("StateStore starts empty on a corrupt file") {
  fs::path dir = fs::temp_directory_path() / "ld_state_bad";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "state.json") << "{ nope"; }
  ld::StateStore s(dir / "state.json");
  CHECK(s.get("x").is_null());
  s.set("x", 1);
  CHECK(ld::StateStore(dir / "state.json").get("x") == 1);
  fs::remove_all(dir);
}

TEST_CASE("StateStore set() survives invalid UTF-8 and keeps saving") {
  fs::path dir = fs::temp_directory_path() / "ld_state_utf8";
  fs::remove_all(dir);
  {
    ld::StateStore s(dir / "state.json");
    CHECK_NOTHROW(s.set("bad", std::string("\xff\xfe")));
    CHECK_NOTHROW(s.set("ok", 1));
  }
  CHECK(ld::StateStore(dir / "state.json").get("ok") == 1);
  fs::remove_all(dir);
}

TEST_CASE("StateStore set() does not throw when the directory cannot be created") {
  fs::path dir = fs::temp_directory_path() / "ld_state_nodir";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "X") << "file"; }
  ld::StateStore s(dir / "X" / "state.json");
  CHECK_NOTHROW(s.set("a", 1));
  fs::remove_all(dir);
}

TEST_CASE("FakeContext stores state and http requests") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ctx.save_state({{"a", 1}});
  CHECK(ctx.load_state()["a"] == 1);
  int got = 0;
  ctx.http_get("https://x", [&](const ld::HttpResponse& r) { got = r.status; });
  ctx.respond(0, ld::HttpResponse{204, "", ""});
  CHECK(got == 204);
}

TEST_CASE("StateStore logs non-ANSI paths without throwing") {
  fs::path dir = fs::temp_directory_path() / L"ld_state_日本";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "state.json") << "{ corrupt"; }
  CHECK_NOTHROW(ld::StateStore(dir / "state.json"));
  fs::create_directories(dir / "X");
  { std::ofstream(dir / "X" / "Y") << "file"; }
  ld::StateStore s(dir / "X" / "Y" / "state.json");  // parent is a file: every save fails and logs
  CHECK_NOTHROW(s.set("a", 1));
  fs::remove_all(dir);
}

TEST_CASE("StateStore::rekey moves values, swaps safely and drops the rest") {
  fs::path dir = fs::temp_directory_path() / "ld_state_rekey";
  fs::remove_all(dir);
  {
    ld::StateStore s(dir / "state.json");
    s.set("a:k0", 1);
    s.set("b:k1", 2);
    s.set("ghost:k9", 3);
    s.rekey({{"a:k1", "a:k0"}, {"b:k0", "b:k1"}, {"c:k2", ""}, {"d:k3", "d:k3"}});
    CHECK(s.get("a:k1") == 1);
    CHECK(s.get("b:k0") == 2);
    CHECK(s.get("a:k0").is_null());
    CHECK(s.get("ghost:k9").is_null());
    CHECK(s.get("c:k2").is_null());
    CHECK(s.get("d:k3").is_null());
  }
  ld::StateStore again(dir / "state.json");
  CHECK(again.get("a:k1") == 1);
  CHECK(again.get("ghost:k9").is_null());
  fs::remove_all(dir);
}

TEST_CASE("StateStore starts empty on a deeply nested file and leaves no temp file after a save") {
  fs::path dir = fs::temp_directory_path() / "ld_state_deep";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "state.json", std::ios::binary) << "{\"a\":" << std::string(100000, '[') << std::string(100000, ']') << "}"; }
  ld::StateStore s(dir / "state.json");
  CHECK(s.get("a").is_null());
  s.set("b", 2);
  CHECK_FALSE(fs::exists(dir / "state.json.tmp"));
  CHECK(ld::StateStore(dir / "state.json").get("b") == 2);
  fs::remove_all(dir);
}
