// src/modules/common/tap_action.h: what a tap does on a key that shows something (cpu_load, the
// clock, the weather...): nothing, Task Manager, or open a URL, a file or a program like the
// launcher's open kind. The same flash and red "!" as the launcher, drawn over the key only while
// they last.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include "modules/common/action_key.h"
#include "modules/key_module.h"
#include "modules/launcher/launcher.h"

namespace ld {

enum class TapKind { None, TaskManager, Open };

// Schema fields of a tap action, in the page's "tap" group: tap_action (none | task_manager | open,
// `action` the default) and tap_target (`target` the default).
nlohmann::json tap_fields(const char* action, const char* target = "");

// What the tap actions launch with: tests pass a fake launcher and an inline or held runner.
struct TapDeps {
  std::shared_ptr<Launcher> launcher;  // null: system_launcher()
  // null: worker_runner(), one per key, started on its first tap. A hung launch blocks its own key
  // only, at the cost of one idle thread per tapped key.
  Runner runner;
};
// The deps of the tap actions started from now on (tests); {} gives the system's back.
void set_tap_deps(TapDeps deps);

// Task Manager as Windows starts it from the Start menu: the system directory's taskmgr.exe (never
// a PATH search), through the shell, which applies its manifest (an administrator may see it start
// elevated, or the UAC prompt).
LaunchRequest task_manager_request();

// A key module whose tap runs the tap action of its settings. Long presses are left to the module.
class TapKey : public KeyModule {
public:
  using KeyModule::KeyModule;
  void on_event(const InputEvent& e) override;  // a Tap: the tap action
  void render(Canvas& c) override;              // the key, then the flash and the "!" over it
  const ActionFeedback& tap_feedback() const { return feedback_; }

protected:
  // Reads tap_action and tap_target. Call from start().
  void start_tap(ModuleContext& ctx, const nlohmann::json& settings);

private:
  void on_done(const CancelToken& token, const std::optional<std::string>& error);
  std::string what_for_log() const;
  ModuleContext* tap_ctx_ = nullptr;
  TapKind kind_ = TapKind::None;
  std::optional<LaunchRequest> req_;  // nullopt with Open: no usable target
  bool needs_full_path_ = false;      // Open: "tools\app.exe", refused like the launcher
  std::shared_ptr<Launcher> launcher_;
  Runner runner_;
  ActionFeedback feedback_;
};

}  // namespace ld
