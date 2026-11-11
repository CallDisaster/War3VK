#pragma once
#include "win32_transport.h"
#include <cstdio>
#include <string>

namespace warvk::host::d9 {
// OS handles are encoded as their interoperable low 32 bits, never as a C++
// object pointer. The nonce-derived property distinguishes HWND reuse even in
// the same process/thread. It is a lifetime witness, NOT a security boundary.
inline std::wstring WindowMarker(ipc::Nonce nonce) {
  return L"WarVK-D9C1-" + lab::NonceText(nonce);
}
inline HWND DecodeWindow(uint32_t token) noexcept {
  return reinterpret_cast<HWND>(static_cast<INT_PTR>(static_cast<int32_t>(token)));
}
inline std::wstring ObjectName(HANDLE object) {
  wchar_t name[256]{}; DWORD needed=0;
  if(!object||!GetUserObjectInformationW(object,UOI_NAME,name,sizeof(name),&needed)
      ||needed<sizeof(wchar_t)||needed>sizeof(name))
    throw std::runtime_error("D3D9 desktop identity unavailable");
  return name;
}
inline std::wstring CurrentDesktop() {
  return ObjectName(GetThreadDesktop(GetCurrentThreadId()));
}
inline std::wstring InputDesktop() {
  HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
  if(!input)throw std::runtime_error("D3D9 input desktop identity unavailable");
  try { auto name=ObjectName(input);CloseDesktop(input);return name; }
  catch(...) { CloseDesktop(input);throw; }
}
inline std::wstring RequireIsolatedDesktop() {
  const auto station=ObjectName(GetProcessWindowStation()),desktop=CurrentDesktop();
  if(station!=L"WinSta0"||desktop==InputDesktop())
    throw std::runtime_error("D3D9 Presenter requires a non-input WinSta0 desktop");
  return station+L"\\"+desktop;
}
inline std::string NameJson(const std::wstring& value) {
  std::string result="\"";
  for(wchar_t c:value){char item[7];std::snprintf(item,sizeof(item),"\\u%04x",unsigned(uint16_t(c)));result+=item;}
  return result+"\"";
}
} // namespace warvk::host::d9
