// war3_player_notice.cpp - see war3_player_notice.h

#include "war3_player_notice.h"

#include <windows.h>

#include <array>
#include <cstdlib>
#include <cstring>

namespace dxvk::war3::notice {

namespace {

std::wstring GameDirectory() {
  std::array<wchar_t, MAX_PATH * 2> path = {};
  const DWORD len = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (len == 0 || len >= path.size())
    return L".";
  std::wstring result(path.data(), len);
  const size_t slash = result.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : result.substr(0, slash);
}

std::string ToCrlf(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 64);
  for (char c : text) {
    if (c == '\n')
      out += '\r';
    out += c;
  }
  return out;
}

std::string ThreadDesktopName() {
  HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
  std::array<wchar_t, 256> name = {};
  DWORD needed = 0;
  if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name.data(), DWORD(sizeof(wchar_t) * name.size()), &needed))
    return {};
  return WideToUtf8(name.data());
}

enum class DialogMode { Auto, Never, Force };

DialogMode ReadDialogMode(const char* control) {
  const char* value = control ? std::getenv(control) : nullptr;
  if (!value || !*value)
    return DialogMode::Auto;
  if (std::strcmp(value, "0") == 0 || _stricmp(value, "off") == 0 || _stricmp(value, "false") == 0)
    return DialogMode::Never;
  if (_stricmp(value, "force") == 0 || std::strcmp(value, "1") == 0 || _stricmp(value, "on") == 0)
    return DialogMode::Force;
  return DialogMode::Auto;
}

} // namespace

std::string WideToUtf8(const wchar_t* text) {
  if (!text || !*text)
    return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1)
    return {};
  std::string out(size_t(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty())
    return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
  if (len <= 0)
    return {};
  std::wstring out(size_t(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), len);
  return out;
}

std::wstring EnsureLogDirectory() {
  std::wstring dir = GameDirectory() + L"\\WarVK";
  CreateDirectoryW(dir.c_str(), nullptr);
  dir += L"\\Log";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool WriteReportFile(const std::wstring& path, const std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  static const char bom[] = "\xEF\xBB\xBF";
  DWORD written = 0;
  bool ok = WriteFile(file, bom, 3, &written, nullptr) && written == 3;
  const std::string body = ToCrlf(text);
  ok = ok && WriteFile(file, body.data(), DWORD(body.size()), &written, nullptr) && written == body.size();
  CloseHandle(file);
  return ok;
}

DialogDecision DecideDialog(const char* control) {
  DialogDecision decision;
  decision.desktop = ThreadDesktopName();
  const DialogMode mode = ReadDialogMode(control);
  const bool interactive = decision.desktop.empty() || _stricmp(decision.desktop.c_str(), "Default") == 0;
  decision.show = mode == DialogMode::Force || (mode == DialogMode::Auto && interactive);
  return decision;
}

int ShowErrorDialog(const std::string& title, const std::string& body) {
  const std::wstring wideTitle = Utf8ToWide(title);
  const std::wstring wideBody = Utf8ToWide(body);
  return MessageBoxW(nullptr, wideBody.c_str(), wideTitle.c_str(),
                     MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND | MB_TASKMODAL);
}

} // namespace dxvk::war3::notice
