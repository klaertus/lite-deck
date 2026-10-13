#include <doctest/doctest.h>
#include <windows.h>
#include <string>
#include "sensors/sensor_block.h"
#include "sensors/shared_mapping.h"

using namespace ld::sensors;

static std::wstring test_name(const wchar_t* tag) {
  return L"Local\\LiteDeckSensorsTest-" + std::wstring(tag) + L"-" + std::to_wstring(GetCurrentProcessId());
}

TEST_CASE("shared mapping: the reader sees what the writer publishes") {
  const auto name = test_name(L"rw");
  auto w = BlockWriter::create(name.c_str(), kMappingSddl);
  REQUIRE(w);
  auto r = BlockReader::open(name.c_str());
  REQUIRE(r);
  write_block(w->block(), 61.25, 42, Status::Ok);
  auto s = read_block(r->block());
  REQUIRE(s);
  CHECK(s->cpu_temp_c == 61.25);
  CHECK(s->updated_unix_ms == 42);
}

TEST_CASE("shared mapping: opening a missing section returns nullptr") {
  CHECK_FALSE(BlockReader::open(test_name(L"missing").c_str()));
}

TEST_CASE("shared mapping: the mapping SDDL denies write access to interactive users") {
  const auto name = test_name(L"acl");
  auto w = BlockWriter::create(name.c_str(), kMappingSddl);
  REQUIRE(w);
  HANDLE h = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name.c_str());
  const DWORD open_error = GetLastError();
  CHECK(h == nullptr);
  CHECK(open_error == ERROR_ACCESS_DENIED);
  if (h) CloseHandle(h);
  HANDLE ro = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  CHECK(ro != nullptr);  // the test runs in an interactive session (IU)
  if (ro) CloseHandle(ro);
}

TEST_CASE("shared mapping: a second writer reopens the section when the test SDDL is writable") {
  // The mapping SDDL gives write access to SYSTEM only (the service), so a test process cannot reopen
  // it read-write; this test uses an SDDL that lets the interactive user write.
  const wchar_t* kWritableSddl = L"D:(A;;GA;;;SY)(A;;GA;;;IU)";
  const auto name = test_name(L"reopen");
  auto w1 = BlockWriter::create(name.c_str(), kWritableSddl);
  REQUIRE(w1);
  write_block(w1->block(), 50.0, 1, Status::Ok);
  auto w2 = BlockWriter::create(name.c_str(), kWritableSddl);
  REQUIRE(w2);
  CHECK(read_block(w2->block())->cpu_temp_c == 50.0);
}

TEST_CASE("shared mapping: an invalid SDDL is reported, not thrown") {
  unsigned long err = 0;
  CHECK_FALSE(BlockWriter::create(test_name(L"badsddl").c_str(), L"not an sddl", &err));
  CHECK(err != 0);
}

TEST_CASE("shared mapping: with the mapping SDDL a non-SYSTEM second writer is denied") {
  const auto name = test_name(L"denied");
  auto w1 = BlockWriter::create(name.c_str(), kMappingSddl);
  REQUIRE(w1);
  unsigned long err = 0;
  CHECK_FALSE(BlockWriter::create(name.c_str(), kMappingSddl, &err));
  CHECK(err == ERROR_ACCESS_DENIED);
}

TEST_CASE("shared mapping: an existing section owned by the current user is accepted") {
  const auto name = test_name(L"owner");
  auto w1 = BlockWriter::create(name.c_str(), L"D:(A;;GA;;;SY)(A;;GA;;;IU)");
  REQUIRE(w1);
  unsigned long err = 0;
  CHECK(BlockWriter::create(name.c_str(), L"D:(A;;GA;;;SY)(A;;GA;;;IU)", &err));
  CHECK(err == 0);
}
// A foreign-owner case (section created by another account) cannot be built without elevation, so the
// refusal branch of the owner check is not covered by a test.
