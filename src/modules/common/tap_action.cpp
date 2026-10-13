// src/modules/common/tap_action.cpp
#include "modules/common/tap_action.h"
#include <algorithm>
#include <cwctype>
#include <mutex>
#include "core/log.h"

namespace ld {

namespace {

std::mutex g_deps_mu;
TapDeps g_deps;

TapDeps tap_deps() {
  std::lock_guard lock(g_deps_mu);
  return g_deps;
}

bool is_exe(const std::wstring& path) {
  std::wstring l = path;
  std::transform(l.begin(), l.end(), l.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return l.ends_with(L".exe");
}

}  // namespace

void set_tap_deps(TapDeps deps) {
  std::lock_guard lock(g_deps_mu);
  g_deps = std::move(deps);
}

LaunchRequest task_manager_request() {
  return LaunchRequest{.kind = LaunchKind::Program, .path = system_directory() + L"\\taskmgr.exe", .via_shell = true};
}

nlohmann::json tap_fields(const char* action, const char* target) {
  using nlohmann::json;
  return json::array({
      json{{"key", "tap_action"}, {"type", "enum"}, {"default", action},
           {"options", json::array({json{{"value", "none"}, {"label", "Nothing"}},
                                    json{{"value", "task_manager"}, {"label", "Open Task Manager"}},
                                    json{{"value", "open"}, {"label", "Open a link, file or program"}}})},
           {"label", "On tap"}, {"group", "tap"}},
      json{{"key", "tap_target"}, {"type", "string"}, {"default", target}, {"label", "Link, file or program"}, {"group", "tap"},
           {"placeholder", "https://… or C:\\…\\app.exe"},
           {"hint", "A link, a file, a folder or a program (%VARIABLES% are expanded). Runs with your rights, never "
                    "as admin."},
           {"show_if", json{{"field", "tap_action"}, {"equals", "open"}}}},
  });
}

void TapKey::start_tap(ModuleContext& ctx, const nlohmann::json& s) {
  tap_ctx_ = &ctx;
  feedback_.attach(ctx, "tap action");
  const std::string action = s.value("tap_action", std::string("none"));
  kind_ = action == "task_manager" ? TapKind::TaskManager : action == "open" ? TapKind::Open : TapKind::None;
  req_.reset();
  needs_full_path_ = false;
  if (kind_ == TapKind::None) return;
  if (kind_ == TapKind::TaskManager) {
    req_ = task_manager_request();
  } else {
    LaunchRequest r = open_request(s.value("tap_target", std::string()));
    needs_full_path_ = !r.path.empty() && !is_url(r.path) && relative_with_folder(r.path);
    if (!r.path.empty() && !needs_full_path_) req_ = std::move(r);
  }
  const TapDeps deps = tap_deps();
  launcher_ = deps.launcher ? deps.launcher : system_launcher();
  runner_ = deps.runner ? deps.runner : worker_runner();  // its thread starts on the first tap
}

void TapKey::render(Canvas& c) {
  KeyModule::render(c);
  feedback_.draw(c);
}

void TapKey::on_event(const InputEvent& e) {
  if (e.kind != EventKind::Tap || kind_ == TapKind::None || !tap_ctx_) return;
  if (!req_) {  // open, without a target it can use: the key says so
    log_warn("tap action: {}", needs_full_path_ ? "the target needs a full path" : "no target to open");
    feedback_.pressed();
    feedback_.failed();
    return;
  }
  const std::optional<CancelToken> token = feedback_.begin();
  if (!token) return;  // a launch in flight: this tap is ignored
  runner_([launcher = launcher_, req = *req_, token = *token, p = tap_ctx_->poster(), this] {
    if (token->load()) {  // given up while queued behind a hung launch: dropped, never a burst
      p.post([this, token] { feedback_.end(token); });
      return;
    }
    std::optional<std::string> error;
    try {
      error = launcher->launch(req);
    } catch (const std::exception& ex) {
      error = std::string("failed: ") + ex.what();
    } catch (...) {
      error = "failed";
    }
    p.post([this, token, error] { on_done(token, error); });  // always: the key never stays busy
  });
}

std::string TapKey::what_for_log() const {
  if (kind_ == TapKind::TaskManager || !req_) return "Task Manager";
  if (!is_url(req_->path) && is_exe(req_->path)) return "program " + name_for_log(req_->path);
  return "open " + kind_for_log(req_->path);  // never a document's name or a URL
}

void TapKey::on_done(const CancelToken& token, const std::optional<std::string>& error) {
  if (!feedback_.end(token) || !error) return;  // late (timed out), or started
  log_warn("tap action: cannot start {}: {}", what_for_log(), *error);
  feedback_.failed();
}

}  // namespace ld
