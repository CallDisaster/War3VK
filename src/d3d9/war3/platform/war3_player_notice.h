// war3_player_notice.h - telling the player about a condition the game cannot
// continue from
//
// A report file under <game directory>\WarVK\Log, and a dialog on the player's
// desktop. Two places use it: a start that failed (war3_startup_error.cpp) and
// a render host that is gone (war3_proxy_host_lost.cpp).
#pragma once

#include <string>

namespace dxvk::war3::notice {

std::string WideToUtf8(const wchar_t* text);
std::wstring Utf8ToWide(const std::string& text);

// <game directory>\WarVK\Log, created if it is missing.
std::wstring EnsureLogDirectory();

// Replaces the file: UTF-8 with a byte order mark, line ends as CRLF.
bool WriteReportFile(const std::wstring& path, const std::string& text);

// Whether a dialog is shown. A dialog waits until someone answers it, so by
// default it is shown only on the Default desktop, the one the player looks
// at; AutoTest's isolated desktops have nobody to answer. The environment
// variable `control` overrides that: 0 / off / false never shows it,
// 1 / on / force shows it on any desktop.
struct DialogDecision {
  bool show = false;
  std::string desktop; // the name of this thread's desktop
};
DialogDecision DecideDialog(const char* control);

// A task-modal error dialog; the texts are UTF-8. Returns MessageBoxW's result.
int ShowErrorDialog(const std::string& title, const std::string& body);

} // namespace dxvk::war3::notice
