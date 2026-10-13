// src/modules/launcher/launcher.cpp
#include "modules/launcher/launcher.h"
#include <algorithm>
#include <cwctype>
#include "core/log.h"
#include "core/strings.h"
#include "modules/key_module.h"

namespace ld {

namespace {

std::wstring lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return s;
}

std::wstring file_name(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring stem(std::wstring path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();  // a folder
  std::wstring name = file_name(path);
  const std::size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) name.resize(dot);
  return name;
}

std::wstring base64(const std::uint8_t* data, std::size_t n) {
  static constexpr wchar_t kAlphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::wstring out;
  out.reserve((n + 2) / 3 * 4);
  for (std::size_t i = 0; i < n; i += 3) {
    const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (i + 1 < n ? std::uint32_t{data[i + 1]} << 8 : 0) |
                            (i + 2 < n ? std::uint32_t{data[i + 2]} : 0);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += i + 1 < n ? kAlphabet[(v >> 6) & 63] : L'=';
    out += i + 2 < n ? kAlphabet[v & 63] : L'=';
  }
  return out;
}

}  // namespace

std::wstring shell_command_line(Shell shell, const std::wstring& command) {
  if (shell == Shell::Cmd) return L"cmd.exe /d /s /c \"" + command + L"\"";
  std::vector<std::uint8_t> utf16le;
  utf16le.reserve(command.size() * 2);
  for (wchar_t c : command) {
    utf16le.push_back(static_cast<std::uint8_t>(c & 0xFF));
    utf16le.push_back(static_cast<std::uint8_t>((c >> 8) & 0xFF));
  }
  return L"powershell.exe -NoLogo -NoProfile -EncodedCommand " + base64(utf16le.data(), utf16le.size());
}

std::wstring program_command_line(const std::wstring& exe, const std::wstring& args) {
  std::wstring line = L"\"" + exe + L"\"";
  if (!args.empty()) line += L" " + args;
  return line;
}

std::wstring launch_label(const LaunchRequest& r) {
  if (r.kind == LaunchKind::Command) return r.shell == Shell::Cmd ? L"cmd" : L"PowerShell";
  if (r.kind == LaunchKind::Open && is_url(r.path)) {
    const std::size_t sep = r.path.find(L"://");
    if (sep == std::wstring::npos) return r.path.substr(0, r.path.find(L':'));  // mailto:...: its scheme
    std::wstring host = r.path.substr(sep + 3);
    host = host.substr(0, host.find_first_of(L"/?#"));
    if (const std::size_t at = host.find_last_of(L'@'); at != std::wstring::npos) host = host.substr(at + 1);
    if (host.empty() || host[0] != L'[') host = host.substr(0, host.find(L':'));  // the port; not an IPv6 literal
    if (lower(host).starts_with(L"www.")) host = host.substr(4);
    return host;
  }
  return stem(r.path);
}

bool runs_directly(const LaunchRequest& r) {
  if (r.kind == LaunchKind::Command || r.via_shell || is_url(r.path)) return false;
  return lower(r.path).ends_with(L".exe");
}

bool focus_existing_window(WindowSystem& ws, const std::wstring& exe) {
  if (exe.empty()) return false;
  const bool bare = exe.find_first_of(L"\\/") == std::wstring::npos;
  const std::wstring want = lower(exe);
  for (const WindowInfo& w : ws.windows()) {
    if (!w.visible || w.owned || !w.titled || w.exe.empty()) continue;
    const std::wstring have = lower(bare ? file_name(w.exe) : w.exe);
    if (have != want) continue;
    ws.activate(w.id, w.minimized);  // found: never a second instance, even if Windows refused the focus
    return true;
  }
  return false;
}

namespace {

constexpr Rgb kAzure{0x4d, 0x9d, 0xff};
constexpr auto kElements = action_elements(kAzure);

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool is_exe(const std::wstring& path) {
  const std::wstring l = lower(path);
  return l.ends_with(L".exe");
}

}  // namespace

bool relative_with_folder(const std::wstring& p) {
  if (p.find_first_of(L"\\/") == std::wstring::npos) return false;  // a bare name: the PATH
  if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return false;
  return !(p.starts_with(L"\\\\") || p.starts_with(L"//"));
}

LaunchRequest open_request(std::string_view target) {
  return LaunchRequest{.kind = LaunchKind::Open, .path = expand_env(unquote(widen(target)))};
}

namespace {

class LauncherModule final : public ActionKey {
public:
  LauncherModule(std::shared_ptr<Launcher> launcher, ActionDeps deps)
      : ActionKey(kElements, std::move(deps)), launcher_(launcher ? std::move(launcher) : system_launcher()) {}

  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    const std::string kind = s.at("kind").get<std::string>();
    Target t;
    if (kind == "open") {
      req_ = open_request(s.at("target").get<std::string>());
      t.program = req_.path;
      t.url = is_url(req_.path);
      t.log_name = kind_for_log(req_.path);
      if (req_.path.empty()) t.hint = L"Target?";
      else if (!t.url && relative_with_folder(req_.path)) t.hint = L"Full path?";
    } else if (kind == "command") {
      req_.kind = LaunchKind::Command;
      req_.command = widen(s.at("command_line").get<std::string>());
      req_.shell = s.at("shell").get<std::string>() == "powershell" ? Shell::PowerShell : Shell::Cmd;
      req_.hidden = s.at("window").get<std::string>() == "hidden";
      t.program = req_.shell == Shell::Cmd ? L"cmd.exe" : L"powershell.exe";
      if (trim(s.at("command_line").get<std::string>()).empty()) t.hint = L"Command?";
    } else {
      req_.kind = LaunchKind::Program;
      req_.path = expand_env(unquote(widen(s.at("path").get<std::string>())));
      req_.args = widen(s.at("args").get<std::string>());
      req_.working_dir = expand_env(unquote(widen(s.at("working_dir").get<std::string>())));
      focus_ = s.at("focus_existing").get<bool>() && is_exe(req_.path);
      t.program = req_.path;
      if (!is_exe(req_.path)) t.log_name = kind_for_log(req_.path);  // a document: its kind, not its name
      if (req_.path.empty()) t.hint = L"Path?";
      else if (relative_with_folder(req_.path)) t.hint = L"Full path?";
    }
    t.default_label = launch_label(req_);
    configured_ = t.hint.empty();
    start_action(ctx, s, std::move(t));
  }

  void on_event(const InputEvent& e) override {
    if (e.kind != EventKind::Press || !configured_) return;
    const std::optional<CancelToken> token = begin_action();
    if (!token) return;  // a launch in flight: this press is ignored
    runner_([launcher = launcher_, req = req_, focus = focus_, token = *token, p = ctx_->poster(), this] {
      if (token->load()) {  // given up while queued behind a hung launch: dropped, never a burst
        p.post([this, token] { end_action(token); });
        return;
      }
      std::optional<std::string> error;
      try {
        if (!(focus && launcher->focus_existing(req.path))) error = launcher->launch(req);
      } catch (const std::exception& ex) {
        error = std::string("failed: ") + ex.what();
      } catch (...) {
        error = "failed";
      }
      p.post([this, token, error] { on_done(token, error); });  // always: the key never stays busy
    });
  }

private:
  void on_done(const CancelToken& token, const std::optional<std::string>& error) {
    if (!end_action(token) || !error) return;  // late (timed out), or started
    // Never the arguments, the command line or a document's name: the kind, and a program's name.
    const std::string what = req_.kind == LaunchKind::Command ? std::string(req_.shell == Shell::Cmd ? "a cmd command"
                                                                                                    : "a PowerShell command")
                             : req_.kind == LaunchKind::Open ? "open " + kind_for_log(req_.path)
                             : is_exe(req_.path)             ? "program " + name_for_log(req_.path)
                                                             : "open " + kind_for_log(req_.path);
    log_warn("launcher: cannot start {}: {}", what, *error);
    failed();
  }

  std::shared_ptr<Launcher> launcher_;
  LaunchRequest req_;
  bool focus_ = false;
  bool configured_ = false;
};

}  // namespace

std::unique_ptr<Module> make_launcher_module(std::shared_ptr<Launcher> launcher, ActionDeps deps) {
  return std::make_unique<LauncherModule>(std::move(launcher), std::move(deps));
}

ModuleInfo launcher_module_info() {
  using nlohmann::json;
  const json kProgram{{"field", "kind"}, {"equals", "program"}};
  const json kCommand{{"field", "kind"}, {"equals", "command"}};
  ModuleInfo m;
  m.id = "launcher";
  m.name = "Launcher";
  m.description = "Press to start a program, open a link or a file, or run a command.";
  m.category = "Actions";
  m.hue = "#4d9dff";
  m.slots = {SlotKind::Key};
  json fields = json::array({
      json{{"key", "kind"}, {"type", "enum"}, {"default", "program"}, {"label", "Action"},
           {"options", json::array({json{{"value", "program"}, {"label", "Start a program"}},
                                    json{{"value", "open"}, {"label", "Open a link, file or folder"}},
                                    json{{"value", "command"}, {"label", "Run a command"}}})}},
      json{{"key", "path"}, {"type", "string"}, {"default", ""}, {"label", "Program"},
           {"hint", "The full path of an exe or a file. %VARIABLES% are expanded. Runs with your rights; a program that "
                    "needs admin rights shows the Windows prompt."},
           {"placeholder", "%LOCALAPPDATA%\\Programs\\App\\app.exe"}, {"show_if", kProgram}},
      json{{"key", "args"}, {"type", "string"}, {"default", ""}, {"label", "Arguments"},
           {"hint", "Passed as they are, without a shell."}, {"show_if", kProgram}},
      json{{"key", "working_dir"}, {"type", "string"}, {"default", ""}, {"label", "Start in"},
           {"hint", "Empty: the program's folder."}, {"show_if", kProgram}},
      json{{"key", "focus_existing"}, {"type", "bool"}, {"default", false}, {"label", "Bring it to the front if running"},
           {"hint", "Instead of starting another copy."}, {"show_if", kProgram}},
      json{{"key", "target"}, {"type", "string"}, {"default", ""}, {"label", "Link, file or folder"},
           {"hint", "Opened with its default program."}, {"placeholder", "https://example.com"},
           {"show_if", json{{"field", "kind"}, {"equals", "open"}}}},
      json{{"key", "command_line"}, {"type", "string"}, {"default", ""}, {"label", "Command line"},
           {"hint", "Runs in a shell with your rights, as if typed in a terminal."},
           {"placeholder", "ipconfig /flushdns"}, {"show_if", kCommand}},
      json{{"key", "shell"}, {"type", "enum"}, {"default", "cmd"}, {"label", "Shell"}, {"show_if", kCommand},
           {"options", json::array({json{{"value", "cmd"}, {"label", "Command Prompt"}},
                                    json{{"value", "powershell"}, {"label", "PowerShell"}}})}},
      json{{"key", "window"}, {"type", "enum"}, {"default", "visible"}, {"label", "Command window"}, {"show_if", kCommand},
           {"options", json::array({json{{"value", "visible"}, {"label", "Shown"}}, json{{"value", "hidden"}, {"label", "Hidden"}}})}},
  });
  for (const json& f : action_fields({"program", "image", "glyph", "none"}, "\xF0\x9F\x9A\x80", kAzure,
                                     "Empty: the program's name or the URL's host."))
    fields.push_back(f);
  m.settings_schema = json{{"fields", fields}};
  m.elements.assign(kElements.begin(), kElements.end());
  m.create = [] { return make_launcher_module({}, {}); };
  return m;
}

}  // namespace ld
