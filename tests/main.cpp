#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <windows.h>
#include <objbase.h>

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  doctest::Context ctx(argc, argv);
  int code = ctx.run();
  CoUninitialize();
  return code;
}
