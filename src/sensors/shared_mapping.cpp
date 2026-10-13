// src/sensors/shared_mapping.cpp
#include "sensors/shared_mapping.h"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>

namespace ld::sensors {

namespace {

// True when the owner of an already existing section is trustworthy: LocalSystem, the Administrators
// group, or the user running this process (keeps tests and the console mode working).
bool owner_is_trusted(HANDLE section) {
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetSecurityInfo(section, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
          ERROR_SUCCESS ||
      !owner)
    return false;
  bool ok = IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
  if (!ok) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      DWORD len = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &len);
      std::vector<unsigned char> buf(len);
      if (len && GetTokenInformation(token, TokenUser, buf.data(), len, &len))
        ok = EqualSid(owner, reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid) != FALSE;
      CloseHandle(token);
    }
  }
  LocalFree(sd);
  return ok;
}

}  // namespace

std::unique_ptr<BlockWriter> BlockWriter::create(const wchar_t* name, const wchar_t* sddl, unsigned long* error) {
  auto fail = [error] {
    if (error) *error = GetLastError();
    return std::unique_ptr<BlockWriter>();
  };
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return fail();
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SensorBlock), name);
  const DWORD create_error = GetLastError();
  LocalFree(sd);
  if (!h) {
    SetLastError(create_error);
    return fail();
  }
  if (create_error == ERROR_ALREADY_EXISTS && !owner_is_trusted(h)) {
    // Someone else created the section first: refuse it rather than publish into (or trust) their object.
    OutputDebugStringW(L"lite-deck sensors: existing shared section has an untrusted owner, refusing it\n");
    CloseHandle(h);
    SetLastError(ERROR_ACCESS_DENIED);
    return fail();
  }
  void* view = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SensorBlock));
  if (!view) {
    const DWORD e = GetLastError();
    CloseHandle(h);
    SetLastError(e);
    return fail();
  }
  return std::unique_ptr<BlockWriter>(new BlockWriter(h, static_cast<SensorBlock*>(view)));
}

BlockWriter::~BlockWriter() {
  UnmapViewOfFile(block_);
  CloseHandle(mapping_);
}

std::unique_ptr<BlockReader> BlockReader::open(const wchar_t* name) {
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!h) return nullptr;
  const void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(SensorBlock));
  if (!view) {
    CloseHandle(h);
    return nullptr;
  }
  return std::unique_ptr<BlockReader>(new BlockReader(h, static_cast<const SensorBlock*>(view)));
}

BlockReader::~BlockReader() {
  UnmapViewOfFile(block_);
  CloseHandle(mapping_);
}

}  // namespace ld::sensors
