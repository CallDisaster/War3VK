// CPU-only startup diagnostic: import KERNEL32 only, no CRT, GPU, or game.
// The external owner supplies a non-input Desktop, exact Job and output handles.
// This custom-entry probe has no C++/CRT destructors; ExitProcess is its normal
// entry-point contract, never an accepted workaround for renderer shutdown.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void line(const char* text) {
  DWORD count = 0, written = 0;
  while (text[count]) ++count;
  if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, count, &written, 0)
      || written != count) ExitProcess(10);
}

static void number(DWORD value) {
  char data[32];
  DWORD count = 0;
  do { data[count++] = (char)('0' + value % 10); value /= 10; } while (value);
  for (DWORD i = 0; i < count / 2; ++i) {
    char tmp = data[i]; data[i] = data[count - 1 - i]; data[count - 1 - i] = tmp;
  }
  data[count++] = '\n'; data[count] = 0; line(data);
}

void __cdecl ProbeEntry(void) {
  line("KERNEL_ENTRY\n");
  line("PID="); number(GetCurrentProcessId());
  line("USER32_PRELOADED="); number(GetModuleHandleW(L"user32.dll") != 0);
  SetLastError(0);
  HMODULE user = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
  DWORD error = GetLastError();
  line("USER32_LOADED="); number(user != 0);
  line("USER32_ERROR="); number(error);
  if (!user) ExitProcess(20);
  typedef HDESK (WINAPI *ThreadDesktop)(DWORD);
  typedef HWINSTA (WINAPI *ProcessStation)(void);
  typedef BOOL (WINAPI *ObjectInfo)(HANDLE, int, PVOID, DWORD, LPDWORD);
  ThreadDesktop getDesktop = (ThreadDesktop)GetProcAddress(user, "GetThreadDesktop");
  ProcessStation getStation = (ProcessStation)GetProcAddress(user, "GetProcessWindowStation");
  ObjectInfo getInfo = (ObjectInfo)GetProcAddress(user, "GetUserObjectInformationW");
  if (!getDesktop || !getStation || !getInfo) ExitProcess(21);
  HANDLE objects[2];
  objects[0] = getStation(); objects[1] = getDesktop(GetCurrentThreadId());
  for (DWORD i = 0; i < 2; ++i) {
    wchar_t name[256]; char utf8[1024]; DWORD bytes = 0;
    if (!getInfo(objects[i], UOI_NAME, name, sizeof(name), &bytes)) ExitProcess(22);
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1,
                                  utf8, sizeof(utf8), 0, 0);
    if (count <= 0 || count > (int)sizeof(utf8)) ExitProcess(23);
    line(i == 0 ? "STATION=" : "DESKTOP="); line(utf8); line("\n");
  }
  if (GetModuleHandleW(L"vulkan-1.dll") || GetModuleHandleW(L"d3d9.dll")
      || GetModuleHandleW(L"dxgi.dll")) ExitProcess(24);
  if (!FreeLibrary(user)) ExitProcess(25);
  line("CPU_PROBE_COMPLETE\n");
  ExitProcess(0);
}
