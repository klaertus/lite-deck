#include "core/state_store.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include "core/files.h"
#include "core/log.h"
#include "core/strings.h"

namespace ld {

StateStore::StateStore(std::filesystem::path file) : file_(std::move(file)) {
  std::ifstream f(file_, std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  auto j = parse_json_bounded(ss.str());
  if (j.is_object()) data_ = std::move(j);
  else log_warn("state file {} unreadable; starting empty", narrow(file_.wstring()));
}

nlohmann::json StateStore::get(const std::string& key) const {
  auto it = data_.find(key);
  return it == data_.end() ? nlohmann::json() : *it;
}

void StateStore::set(const std::string& key, nlohmann::json value) {
  data_[key] = std::move(value);
  save();
}

void StateStore::rekey(const std::map<std::string, std::string>& sources) {
  nlohmann::json next = nlohmann::json::object();
  for (const auto& [key, from] : sources) {
    if (from.empty()) continue;
    auto it = data_.find(from);
    if (it != data_.end()) next[key] = *it;
  }
  if (next == data_) return;
  data_ = std::move(next);
  save();
}

void StateStore::save() {
  try {
    if (!write_file_atomic(file_, data_.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
      const DWORD err = GetLastError();
      log_error("cannot save state file {} ({})", narrow(file_.wstring()), err);
    }
  } catch (const std::exception& e) {
    log_error("cannot save state file {}: {}", narrow(file_.wstring()), e.what());
  }
}

}  // namespace ld
