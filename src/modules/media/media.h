#pragma once
#include <functional>
#include <memory>
#include "modules/module.h"

namespace ld {
using KeySender = std::function<void(unsigned short vk)>;
void send_media_key(unsigned short vk);
std::unique_ptr<Module> make_media_module(KeySender sender);
ModuleInfo media_module_info();
}  // namespace ld
