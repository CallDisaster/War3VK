// Link the real war3_startup_witness.cpp with -DWARVK_STARTUP_WITNESS_DEV.
// Root owns compilation/execution. Each mode is a fresh process; no game.
// --emit fresh-path gives a real mapping/wrapper fixture with printed owner ID.
// --off path proves missing opt-in does not create a file; --existing path
// proves CREATE_NEW does not replace an existing fixture. No files are deleted.
#include "../src/d3d9/war3/diagnostics/war3_startup_witness.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace dxvk::war3::diagnostics;
static unsigned checks = 0;
static void check(bool result, unsigned line) {
  ++checks;
  if (!result) { std::fprintf(stderr, "FAIL %u\n", line); std::exit(1); }
}
#define CHECK(x) check(bool(x), __LINE__)
int main(int argc, char** argv) {
  CHECK(argc == 3);
  const bool emit = std::strcmp(argv[1], "--emit") == 0;
  const bool off = std::strcmp(argv[1], "--off") == 0;
  const bool existing = std::strcmp(argv[1], "--existing") == 0;
  CHECK(emit || off || existing);
  CHECK(SetEnvironmentVariableA("DXVK_WAR3_STARTUP_WITNESS_FILE", argv[2]));
  CHECK(SetEnvironmentVariableA("DXVK_WAR3_STARTUP_WITNESS", off ? nullptr : "1"));
  const DWORD before = GetFileAttributesA(argv[2]);
  CHECK(existing ? before != INVALID_FILE_ATTRIBUTES : before == INVALID_FILE_ATTRIBUTES);
  SetLastError(0xABCDEF01);
  InitializeStartupWitness();
  CHECK(GetLastError() == 0xABCDEF01);
  const auto token = BeginStartupStage(StartupStage::MainRunnerOriginal, 12, 34);
  CHECK(GetLastError() == 0xABCDEF01);
  CHECK(emit ? token != 0 : token == 0);
  SetLastError(0x76543210);
  EndStartupStage(StartupStage::MainRunnerOriginal, token, 56, 78);
  CHECK(GetLastError() == 0x76543210);
  NoteStartupStage(StartupStage::RuntimeReady);
  CHECK(GetLastError() == 0x76543210);
  InitializeStartupWitness();
  CHECK(GetLastError() == 0x76543210);
  if (off) CHECK(GetFileAttributesA(argv[2]) == INVALID_FILE_ATTRIBUTES);
  FILETIME created{}, exit{}, kernel{}, user{};
  CHECK(GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user));
  const unsigned long long creation = (static_cast<unsigned long long>(created.dwHighDateTime) << 32)
                                   | created.dwLowDateTime;
  std::printf("startup runtime checks=%u failures=0 pid=%lu creation100ns=%llu mode=%s\n",
              checks, GetCurrentProcessId(), creation, argv[1]);
  return 0;
}
