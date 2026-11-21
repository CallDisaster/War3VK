// war3_d3d9_observer.h - what the D3D9 front end tells the x86 data layer
//
// The data layer (Game.dll hooks, JASS, control plane; src/d3d9/war3) drives
// the front end through its WarVKD3D9* exports. This is the other direction:
// the four moments at which the in-process device used to call into the data
// layer. Plain C layout, shared by both modules (the front end is
// tools/render_host/d3d9_proxy.cpp); no ownership crosses it.
//
// Every callback runs on the device's owner thread (the game's D3D thread).
// A callback may use the device: Get* calls answer from the front end's local
// state, and D3D9 calls or WarVKD3D9* exports issued from inside a callback are
// forwarded normally. Draws issued from inside a callback are not observed,
// and no window resize is taken inside one: the Present that follows takes it
// and sends `resized` from there (a callback may meet the old back buffer).
#pragma once
#include <cstdint>

struct IDirect3DDevice9;
struct IUnknown;

extern "C" {

struct WarVKD3D9DrawFacts {
  uint32_t size;            // sizeof(WarVKD3D9DrawFacts)
  uint32_t indexed;         // 1 DrawIndexedPrimitive, 0 DrawPrimitive
  uint32_t primitiveType;   // D3DPRIMITIVETYPE
  int32_t  baseVertexIndex;
  uint32_t minVertexIndex;
  uint32_t numVertices;
  uint32_t startIndex;      // start vertex for a non-indexed draw
  uint32_t primitiveCount;
};

// Why the session with the host ended, and where it stood. The strings live
// as long as the device.
struct WarVKD3D9TerminalFacts {
  uint32_t size;            // sizeof(WarVKD3D9TerminalFacts)
  uint32_t result;          // the cause's own code (an HRESULT or a ring fault result), 0 if none
  const char* kind;         // Connection::TerminalName
  const char* detail;
  // The host process's exit code, once it has exited and been reaped.
  uint32_t hostExitKnown;
  uint32_t hostExit;
  // The command stream: the last sequence number this process published, and
  // the last one the host reported as applied.
  uint64_t published;
  uint64_t applied;
  const wchar_t* hostLog;   // the host's log file
};

struct WarVKD3D9Observer {
  uint32_t size;            // sizeof(WarVKD3D9Observer)
  void* user;
  // Before a draw is validated and sent. Annotations made here apply to it.
  void (__stdcall* draw)(void* user, IDirect3DDevice9* device, const WarVKD3D9DrawFacts* facts);
  // Before a Present is sent: the last point at which this frame's records,
  // markers and draws can still be added.
  void (__stdcall* present)(void* user, IDirect3DDevice9* device);
  // After the front end reset the swap chain to a new window client extent.
  void (__stdcall* resized)(void* user, IDirect3DDevice9* device,
      uint32_t previousWidth, uint32_t previousHeight, uint32_t width, uint32_t height);
  // Once, at the first Present after the connection became terminal (the host
  // exited, hung or rejected the session). Every later call answers
  // D3DERR_DEVICELOST. The callback may end the process.
  void (__stdcall* terminal)(void* user, IDirect3DDevice9* device,
      const WarVKD3D9TerminalFacts* facts);
  // The device is being destroyed: withdraw everything published about it.
  void (__stdcall* released)(void* user, IDirect3DDevice9* device);
  // The application called Reset: release the data layer's own DEFAULT-pool
  // resources (the settings panel's buffers), as the in-process device's Reset
  // has its panel do. A Reset fails while any DEFAULT-pool buffer is alive.
  void (__stdcall* reset)(void* user, IDirect3DDevice9* device);
};

// Replaces the device's observer (null removes it). Owner thread only. Any
// member may be null. D3DERR_INVALIDCALL for a device of another module or a
// struct of another size; the earlier size, which ends before `reset`, is
// still taken.
long __stdcall WarVKD3D9SetObserver(IDirect3DDevice9* device, const WarVKD3D9Observer* observer);

// The data layer's own resources on the device (the settings panel's buffers
// and atlas): RetainPrivate trades the caller's one public reference to a
// vertex buffer, index buffer or texture of this device for a private one, so
// the resource no longer keeps the device alive; ReleasePrivate drops that
// private reference. Release them no later than the `released` callback.
// Owner thread only; D3DERR_INVALIDCALL for anything else.
long __stdcall WarVKD3D9RetainPrivate(IDirect3DDevice9* device, IUnknown* resource);
long __stdcall WarVKD3D9ReleasePrivate(IDirect3DDevice9* device, IUnknown* resource);

// The fixed-function lighting of the game's world scene, as the data layer
// resolved it (sun, day/night ambient). While `active`, the front end sends
// each fixed-function draw with these values in place of the game's own; the
// game's state is untouched and Get* keeps answering with it. Values only.
struct WarVKD3D9WorldLighting {
  uint32_t size;                 // sizeof(WarVKD3D9WorldLighting)
  uint32_t active;               // inside the world RenderScene
  uint32_t materialAmbientWhite; // the material's ambient becomes (1,1,1,1)
  // Light 0, when the game has it enabled:
  //   0  the game's own
  //   1  the sun is switched off: a directional light keeps only its ambient
  //   2  replaced: directional, the direction and colours below
  uint32_t sunMode;
  float sunDirection[3];
  float sunColor[3];             // diffuse and specular, intensity applied
  float sunAmbient[3];
  // D3DRS_AMBIENT becomes ambientColor and the ambient material source the
  // material, so the white ambient above takes it.
  uint32_t ambientOverride;
  uint32_t ambientColor;         // D3DCOLOR
};

// Replaces the world lighting; null or active == 0 ends it. Owner thread.
// D3DERR_INVALIDCALL for a struct of another size or a value that is not
// finite; the previous lighting then stays.
long __stdcall WarVKD3D9SetWorldLighting(IDirect3DDevice9* device, const WarVKD3D9WorldLighting* lighting);

}
