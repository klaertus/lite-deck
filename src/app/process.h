#pragma once
#include <string_view>
namespace ld {
bool process_running(std::wstring_view exe_name);  // case-insensitive match on the image name
}
