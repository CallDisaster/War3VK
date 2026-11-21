#pragma once
#include "win32_transport.h"
#include <cstdarg>
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
// WARVK_D3D9_GAME names the only two ways this development proxy enters a
// process that has Game.dll:
//   sandbox-isolated-v1  the automated runs. ALSO requires a non-input WinSta0
//                        desktop: a player's game runs on the input desktop,
//                        so this value alone never admits it.
//   author-trial-v1      the author trying the proxy by hand, on the desktop
//                        they are looking at. Nothing automated sets it and no
//                        default leads to it. It admits the input desktop here,
//                        in the host's Initialize and in the host's surface
//                        target, and starts the host at the game's priority.
// Unset keeps the laboratory refusal; any other value is an error.
enum class GameProfile { None, Sandbox, AuthorTrial };
inline GameProfile ReadGameProfile() {
  wchar_t value[32]{};
  const DWORD size=GetEnvironmentVariableW(L"WARVK_D3D9_GAME",value,32);
  if(!size)return GameProfile::None;
  if(size<32&&std::wstring(value)==L"sandbox-isolated-v1")return GameProfile::Sandbox;
  if(size<32&&std::wstring(value)==L"author-trial-v1")return GameProfile::AuthorTrial;
  throw std::runtime_error("unknown D3D9 game profile");
}
inline bool AuthorTrialProfile() noexcept {
  try{return ReadGameProfile()==GameProfile::AuthorTrial;}catch(...){return false;}
}
// The Presenter's desktop: WinSta0, and not the input desktop unless this is
// the author's trial (the host inherits the variable from the game).
inline std::wstring RequireIsolatedDesktop() {
  const auto station=ObjectName(GetProcessWindowStation()),desktop=CurrentDesktop();
  if(station!=L"WinSta0"||(!AuthorTrialProfile()&&desktop==InputDesktop()))
    throw std::runtime_error("D3D9 Presenter requires a non-input WinSta0 desktop");
  return station+L"\\"+desktop;
}
// True when this proxy may enter a process that has Game.dll (see above).
inline bool GameSandboxProfile() {
  if(ReadGameProfile()==GameProfile::None)return false;
  RequireIsolatedDesktop();
  return true;
}
// Cold first-wiring trace on stderr. Only the game profile prints, so the
// laboratory fixtures' output is unchanged.
inline bool GameTraceEnabled() noexcept {
  static const bool enabled=[]{try{return GameSandboxProfile();}catch(...){return false;}}();
  return enabled;
}
inline void GameTrace(const char* format,...) noexcept {
  if(!GameTraceEnabled())return;
  va_list args;va_start(args,format);
  std::fputs("D3D9 game: ",stderr);std::vfprintf(stderr,format,args);std::fputc('\n',stderr);std::fflush(stderr);
  va_end(args);
}
inline std::string NameJson(const std::wstring& value) {
  std::string result="\"";
  for(wchar_t c:value){char item[7];std::snprintf(item,sizeof(item),"\\u%04x",unsigned(uint16_t(c)));result+=item;}
  return result+"\"";
}
} // namespace warvk::host::d9
