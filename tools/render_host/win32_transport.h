#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "protocol.h"
#include <stdexcept>
#include <string>

// CPU LAB ONLY. Deadline cancellation drains actual I/O completion and may
// block; an external process watchdog is mandatory. Never call from game code.
namespace warvk::host::lab {
class Handle {
public:
  explicit Handle(HANDLE value=nullptr) noexcept : m_value(value) {}
  ~Handle() { reset(); }
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other) noexcept : m_value(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if(this!=&other)reset(other.release());
    return *this;
  }
  HANDLE get() const noexcept { return m_value; }
  explicit operator bool() const noexcept { return m_value&&m_value!=INVALID_HANDLE_VALUE; }
  HANDLE release() noexcept { HANDLE v=m_value;m_value=nullptr;return v; }
  void reset(HANDLE v=nullptr) noexcept { if(*this)CloseHandle(m_value);m_value=v; }
private:
  HANDLE m_value;
};
enum class Stage { Setup, Connect, Header, Body, Write, SlotMutex, SlotCopy };
enum class Fault { System, Timeout, PeerExited, PeerIdentity, HeaderInvalid, EndOfStream, Abandoned, SlotLease };
enum class Channel { Echo0, Slots1 };
struct Failure : std::runtime_error {
  Failure(Fault f,Stage s,DWORD e=0,ipc::Error p=ipc::Error::None)
    :std::runtime_error("RH0 lab transport failure"),fault(f),stage(s),win32(e),protocol(p) {}
  Fault fault;Stage stage;DWORD win32;ipc::Error protocol;
};
const char* StageName(Stage s) noexcept;
const char* FaultName(Fault f) noexcept;
struct IoStats {
  uint64_t reads=0,writes=0,shortReads=0,shortWrites=0;
  uint64_t cancelRequests=0,cancelCompletions=0;
};
using Packet=std::array<uint8_t,ipc::MaxMessageBytes>;
inline constexpr DWORD DeadlineMs=5000;
uint64_t CreationTime(HANDLE process);
ipc::Nonce RandomNonce();
std::wstring NonceText(ipc::Nonce nonce);
ipc::Nonce ParseNonce(const std::wstring& text);
std::wstring PipeName(ipc::Nonce nonce, Channel channel = Channel::Echo0);
Handle OpenPeer(DWORD pid,uint64_t creation);
Handle CreateServer(ipc::Nonce nonce, Channel channel = Channel::Echo0);
void Accept(HANDLE pipe,HANDLE peer,IoStats& stats);
Handle Connect(ipc::Nonce nonce,HANDLE peer,DWORD expectedPid,Channel channel = Channel::Echo0);
void VerifyPeer(HANDLE pipe,bool server,DWORD expectedPid);
size_t ReadPacket(HANDLE pipe,HANDLE peer,Packet& packet,IoStats& stats);
void WriteBytes(HANDLE pipe,HANDLE peer,const uint8_t* data,size_t bytes,IoStats& stats,
                size_t fragmentBytes=ipc::MaxMessageBytes);
class ReadWindow {
public:
  ReadWindow() noexcept : m_until(GetTickCount64() + DeadlineMs) {}
private:
  friend void ReadExact(HANDLE, HANDLE, uint8_t*, size_t, const ReadWindow&, Stage, IoStats&);
  ULONGLONG m_until;
};
// A framing owner uses the SAME window for its header and body. This function
// does not select a protocol or authorize bytes as RH0/RH1 messages.
void ReadExact(HANDLE pipe,HANDLE peer,uint8_t* bytes,size_t count,const ReadWindow& window,
               Stage stage,IoStats& stats);
} // namespace warvk::host::lab
