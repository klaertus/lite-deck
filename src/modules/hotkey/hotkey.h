// src/modules/hotkey/hotkey.h: a key that sends key combinations ("Ctrl+Shift+M, Enter") or types
// a text, to the window that has the focus.
#pragma once
#include <memory>
#include "modules/common/action_key.h"
#include "modules/hotkey/keys.h"
#include "modules/module.h"

namespace ld {

// Empty arguments: system_keyboard() and the system icons.
std::unique_ptr<Module> make_hotkey_module(std::shared_ptr<Keyboard> keyboard, ActionDeps deps);
ModuleInfo hotkey_module_info();

}  // namespace ld
