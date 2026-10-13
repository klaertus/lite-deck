// sensors-service/main.cpp - LiteDeckSensors: publishes the CPU temperature in Global\\LiteDeckSensors.
// No ports, no commands: the only controls accepted are STOP and SHUTDOWN.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include "core/log.h"
#include "core/strings.h"
#include "sensors/pawnio_source.h"
#include "sensors/publisher.h"
#include "sensors/shared_mapping.h"

namespace {

constexpr wchar_t kServiceName[] = L"LiteDeckSensors";

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_done = nullptr;  // set once run() has logged "stopped" (console mode waits for it)

std::filesystem::path exe_dir() {
  // Empty on failure (never a truncated path).
  std::wstring buf(MAX_PATH, L'\0');
  for (int i = 0; i < 6; ++i) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return std::filesystem::path(buf).parent_path();
    }
    buf.resize(buf.size() * 2);
  }
  return {};
}

// Called from service_main and the control handler threads: a local SERVICE_STATUS per call.
void report(DWORD state, DWORD exit_code = NO_ERROR) {
  static std::atomic<DWORD> checkpoint{1};
  SERVICE_STATUS st{};
  st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  st.dwCurrentState = state;
  st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
  st.dwWin32ExitCode = exit_code;
  const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
  st.dwWaitHint = pending ? 3000 : 0;
  st.dwCheckPoint = pending ? checkpoint++ : 0;
  if (g_status_handle) SetServiceStatus(g_status_handle, &st);
}

// The service loop. Returns a Win32 exit code.
DWORD run(bool console) {
  const auto dir = exe_dir();
  if (dir.empty()) {
    ld::log_error("sensors: cannot determine the executable folder");
    return ERROR_BAD_PATHNAME;
  }
  const auto module = dir / L"AMDFamily17.bin";
  std::wstring name = ld::sensors::kMappingName;
  unsigned long err = 0;
  auto writer = ld::sensors::BlockWriter::create(name.c_str(), ld::sensors::kMappingSddl, &err);
  if (!writer && console && err == ERROR_ACCESS_DENIED) {
    // Global\ needs SeCreateGlobalPrivilege. Still useful to see the status codes.
    name = L"Local\\LiteDeckSensors";
    std::wprintf(L"Cannot publish in Global\\LiteDeckSensors (not elevated, or the service owns it): publishing in %ls (the daemon will not see it)\n", name.c_str());
    std::fflush(stdout);
    writer = ld::sensors::BlockWriter::create(name.c_str(), ld::sensors::kMappingSddl, &err);
  }
  if (!writer) {
    ld::log_error("sensors: cannot create the shared section (error {})", err);
    return err ? err : ERROR_GEN_FAILURE;
  }
  ld::sensors::Publisher pub(writer->block(), [module] { return ld::sensors::PawnIoSource::open(module); });
  pub.on_status = [](ld::sensors::Status s, const std::string& detail) {
    ld::log_info("sensors: status {}{}{}", ld::sensors::status_name(s), detail.empty() ? "" : " - ", detail);
  };
  ld::log_info("sensors: started (module {})", ld::narrow(module.wstring()));
  for (;;) {
    pub.tick(ld::sensors::unix_now_ms());
    if (console) {
      auto s = ld::sensors::read_block(writer->block());
      if (s)
        std::printf("%-16s %6.2f C\n", std::string(ld::sensors::status_name(s->status)).c_str(), s->cpu_temp_c);
    }
    if (console) std::fflush(stdout);
    if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) break;
  }
  ld::log_info("sensors: stopped");
  if (g_done) SetEvent(g_done);
  return NO_ERROR;
}

DWORD WINAPI control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    report(SERVICE_STOP_PENDING);
    SetEvent(g_stop);
    return NO_ERROR;
  }
  if (control == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
  return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, &control_handler, nullptr);
  if (!g_status_handle) return;
  report(SERVICE_START_PENDING);
  report(SERVICE_RUNNING);
  const DWORD code = run(false);
  report(SERVICE_STOPPED, code);
}

BOOL WINAPI console_ctrl(DWORD) {
  SetEvent(g_stop);
  // On CTRL_CLOSE_EVENT the process is killed soon after this returns: give the worker up to 2 s to log "stopped".
  if (g_done) WaitForSingleObject(g_done, 2000);
  return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // First thing: no DLL is ever searched in the exe/current folder or PATH (this runs as SYSTEM).
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
  const bool console = argc > 1 && std::wstring(argv[1]) == L"--console";
  const std::filesystem::path dir = exe_dir();
  if (!dir.empty()) ld::log_init(dir / L"logs" / L"sensors.log", ld::LogLevel::Info);  // never a relative path
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stop || !g_done) return static_cast<int>(GetLastError());
  if (console) {
    SetConsoleCtrlHandler(&console_ctrl, TRUE);
    std::printf("lite-deck-sensors --console: Ctrl+C to stop\n");
    return static_cast<int>(run(true));
  }
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), &service_main}, {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(table)) {
    const DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
      std::printf("This is the LiteDeckSensors service. Use install.ps1, or run with --console to test.\n");
    return static_cast<int>(error);
  }
  return 0;
}
