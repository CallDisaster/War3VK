// Test-only companion object, relinked with the exact production DLL objects.
// Not part of the shipping target/export table. No copied renderer/backend.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../src/d3d9/d3d9_device.h"
#include "../src/d3d9/war3/ui/war3_imgui.h"
#include "../src/d3d9/war3/render/war3_post_process.h"
#include "../src/d3d9/war3/reimpl/war3_instance_buffer.h"
#include "../src/dxvk/dxvk_stats.h"
#include "imgui.h"

namespace {
// Versioned, scalar-only test ABI. Parent repeats these public field positions;
// no pointer, private ImGui backend layout, or C++ object crosses this boundary.
constexpr uint32_t FieldCount = 39;
enum Operation : uint32_t {
  Snapshot = 0, UiDraw = 1, CursorSmall = 2, CursorLarge = 3,
  PostFx = 4, WrongOwner = 5, PostFxPartialRetry = 6,
  CursorSmallUpdate = 7, InstanceBuffer = 8, InstanceBufferArm = 9,
};
enum Field : uint32_t {
  Version = 0, Op = 1, RefsBefore = 2, RefsAfter = 3, UiInitialized = 4,
  UiOwner = 5, ContextPresent = 6, Vertices = 7, Indices = 8, Commands = 9,
  AtlasLive = 10, AtlasPixels = 11, GpuDraws = 12, GpuCompleted = 13,
  PostFxReady = 14, PostFxApplied = 15, PartialRejected = 16,
  WrongOwnerPreserved = 17, LosableCheckEnabled = 18,
  CursorWidth = 19, CursorHeight = 20, PixelsChecked = 21, PixelFailures = 22,
  CursorCommands = 23, CursorPixelsChecked = 24, CursorPixelFailures = 25,
  CursorColor = 26, InstanceBufferReady = 27, InstanceBytesWritten = 28,
  InstanceStride = 29, InstanceBound = 30, InstanceTextureRetained = 31,
  InstanceTexturePixels = 32, InstanceTextureFailures = 33,
  InstanceOtherOwner = 34, InstanceTlsIsolation = 35, InstanceActive = 36,
  InstanceStableOwner = 37, Reserved = 38,
};

using InstanceOwner = dxvk::war3::reimpl::War3InstanceBuffer;
using Instance = dxvk::war3::reimpl::InstanceData;
static_assert(sizeof(Instance) == 96, "instance fixture requires the production stream layout");
// Test-only borrowed identities. The public parent fixture owns both devices;
// these pointers add no reference, never cross the scalar ABI, and are cleared
// before the primary's final Release (with a null-Snapshot cleanup backstop).
dxvk::D3D9DeviceEx* primaryInstanceDevice = nullptr;
InstanceOwner* primaryInstances = nullptr;

struct ActiveInstanceScope {
  InstanceOwner* saved = InstanceOwner::GetActive();
  explicit ActiveInstanceScope(InstanceOwner* owner) { InstanceOwner::SetActive(owner); }
  ~ActiveInstanceScope() { InstanceOwner::SetActive(saved); }
};

struct InstanceUnlockScope {
  InstanceOwner* owner;
  explicit InstanceUnlockScope(InstanceOwner* value) : owner(value) { }
  ~InstanceUnlockScope() { if (owner) owner->Unlock(); }
  void unlock() { owner->Unlock(); owner = nullptr; }
};

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

std::string Quote(const char* text) {
  std::string result = "\"";
  for (size_t i = 0; text && text[i] && i < 600; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch == '\\' || ch == '"') { result += '\\'; result += char(ch); }
    else if (ch >= 32 && ch < 127) result += char(ch);
    else result += '?';
  }
  return result + '"';
}

void Check(HRESULT hr, const char* operation) {
  if (hr == D3D_OK) return;
  char text[180];
  std::snprintf(text, sizeof(text), "%s HRESULT=0x%08lx", operation,
      static_cast<unsigned long>(hr));
  throw std::runtime_error(text);
}

ULONG PublicRefs(IDirect3DDevice9* device) {
  if (!device) return 0;
  device->AddRef();
  return device->Release();
}

void CompleteGpu(IDirect3DDevice9* device) {
  dxvk::Com<IDirect3DQuery9> query;
  Check(device->CreateQuery(D3DQUERYTYPE_EVENT, &query), "Create event query");
  Check(query->Issue(D3DISSUE_END), "Issue event query");
  const ULONGLONG deadline = GetTickCount64() + 15000;
  for (;;) {
    BOOL complete = FALSE;
    const HRESULT hr = query->GetData(&complete, sizeof(complete), D3DGETDATA_FLUSH);
    if (hr == S_OK) {
      Require(complete == TRUE, "completed event did not report TRUE");
      return;
    }
    Require(hr == S_FALSE, "event query failed");
    Require(GetTickCount64() < deadline, "event query timed out");
    Sleep(1);
  }
}

uint64_t DrawCount(dxvk::D3D9DeviceEx* device) {
  return device->GetDXVKDevice()->getStatCounters().getCtr(dxvk::DxvkStatCounter::CmdDrawCalls);
}

void SnapshotUi(dxvk::D3D9DeviceEx* device, uint64_t* out) {
  auto& ui = dxvk::war3::War3Imgui::get();
  out[UiInitialized] = ui.isInitialized();
  out[UiOwner] = device && ui.isOwner(device);
  out[ContextPresent] = ImGui::GetCurrentContext() != nullptr;
  out[AtlasLive] = out[AtlasPixels] = 0;
  if (ImGui::GetCurrentContext()) {
    for (const ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
      if (texture && texture->Status == ImTextureStatus_OK &&
          texture->TexID != ImTextureID_Invalid) {
        ++out[AtlasLive];
        out[AtlasPixels] += uint64_t(texture->Width) * uint64_t(texture->Height);
      }
    }
  }
  out[LosableCheckEnabled] = device && device->GetOptions()->countLosableResources;
  out[InstanceActive] = InstanceOwner::GetActive() != nullptr;
}

void RenderUi(dxvk::D3D9DeviceEx* device, uint64_t* out, bool cursor = false) {
  auto& ui = dxvk::war3::War3Imgui::get();
  Require(ui.isOwner(device), "UI is not owned by this device");
  Require(ImGui::GetCurrentContext(), "UI context missing");
  CompleteGpu(device);
  const uint64_t before = DrawCount(device);
  if (!ui.isVisible()) ui.toggleVisible();
  ui.newFrame();
  if (cursor) {
    // Local fixture input only, after NewFrame so the Win32 backend cannot
    // replace it. Never move/click the real cursor or activate our hidden HWND.
    ImGui::GetIO().WantCaptureMouse = true;
    ImGui::GetIO().MousePos = ImVec2(96, 80);
  }
  // Independent nonempty draw input inside the actual production UI frame.
  ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(8, 8), ImVec2(68, 58),
      IM_COL32(64, 180, 240, 255));
  ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 18), IM_COL32_WHITE, "owner test");
  ui.render(false); // Production wrapper performs BeginScene/EndScene.
  const ImDrawData* draw = ImGui::GetDrawData();
  Require(draw && draw->Valid, "no completed ImGui draw data");
  out[Vertices] = draw->TotalVtxCount;
  out[Indices] = draw->TotalIdxCount;
  for (const ImDrawList* list : draw->CmdLists)
    out[Commands] += list->CmdBuffer.Size;
  ui.endFrame();
  Require(out[Vertices] && out[Indices] && out[Commands], "empty UI geometry");
  CompleteGpu(device);
  out[GpuCompleted] = 1;
  const uint64_t after = DrawCount(device);
  Require(after > before, "UI produced no actual DXVK draw commands");
  out[GpuDraws] = after - before;
  SnapshotUi(device, out);
  Require(out[AtlasLive] && out[AtlasPixels], "backend created no live atlas texture");
}

void Cursor(dxvk::D3D9DeviceEx* device, uint32_t extent, uint32_t color, uint64_t* out) {
  auto& ui = dxvk::war3::War3Imgui::get();
  Require(ui.isOwner(device), "cursor request is not from UI owner");
  std::array<uint32_t, 17 * 17> pixels{};
  std::fill(pixels.begin(), pixels.end(), color);
  ui.setCursorBitmap(int(extent), int(extent), pixels.data(), 1, 1);
  RenderUi(device, out, true);
  // Read only the public foreground draw stream and actual bound D3D texture.
  // Font textures are explicitly excluded; no private backend/cursor layout is
  // duplicated. The second same-size operation has a different pixel payload.
  for (const ImDrawCmd& command : ImGui::GetForegroundDrawList()->CmdBuffer) {
    const ImTextureID id = command.GetTexID();
    if (!command.ElemCount || id == ImTextureID_Invalid) continue;
    bool atlas = false;
    for (const ImTextureData* texture : ImGui::GetPlatformIO().Textures)
      atlas |= texture && texture->TexID == id;
    if (atlas) continue;
    auto* texture = reinterpret_cast<IDirect3DTexture9*>(static_cast<uintptr_t>(id));
    D3DSURFACE_DESC desc{};
    Check(texture->GetLevelDesc(0, &desc), "Cursor bound texture description");
    Require(desc.Width == extent && desc.Height == extent && desc.Format == D3DFMT_A8R8G8B8 &&
        desc.Pool == D3DPOOL_MANAGED, "Cursor draw did not bind requested texture");
    out[CursorWidth] = desc.Width;
    out[CursorHeight] = desc.Height;
    out[CursorColor] = color;
    ++out[CursorCommands];
    D3DLOCKED_RECT locked{};
    Check(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY), "Cursor texture LockRect");
    for (UINT y = 0; y < desc.Height; ++y) {
      for (UINT x = 0; x < desc.Width; ++x) {
        uint32_t actual = 0;
        std::memcpy(&actual, static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
        ++out[CursorPixelsChecked];
        if (actual != color) ++out[CursorPixelFailures];
      }
    }
    Check(texture->UnlockRect(0), "Cursor texture UnlockRect");
  }
  Require(out[CursorCommands] == 1 && out[CursorPixelsChecked] == extent * extent &&
      !out[CursorPixelFailures], "Cursor texture update/draw evidence missing");
}

void RunPostFx(dxvk::D3D9DeviceEx* device, bool rejectFirst, uint64_t* out) {
  constexpr UINT Extent = 32;
  constexpr uint32_t Color = 0xff406080u;
  CompleteGpu(device);
  const uint64_t beforeDraws = DrawCount(device);
  {
    dxvk::War3PostProcess post(device);
    if (rejectFirst) {
      // Compile/create shader and fullscreen resources, then reject a depth
      // format with RENDERTARGET usage in NormalizeTextureProperties, before
      // any invalid Vulkan view. This is partial Init, NOT allocator injection.
      post.Init(Extent, Extent, D3DFMT_D16);
      Require(!post.IsReady(), "depth-only PostFX color target format was accepted");
      Require(PublicRefs(device) == out[RefsBefore], "partial Init retained public device refs");
      out[PartialRejected] = 1;
    }
    post.Init(Extent, Extent, D3DFMT_A8R8G8B8);
    Require(post.IsReady(), "PostFX valid Init failed");
    out[PostFxReady] = 1;
    Require(PublicRefs(device) == out[RefsBefore], "ready PostFX retained public device refs");
    dxvk::Com<IDirect3DTexture9> source;
    dxvk::Com<IDirect3DSurface9> target, readback;
    Check(device->CreateTexture(Extent, Extent, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &source, nullptr), "PostFX source texture");
    D3DLOCKED_RECT locked{};
    Check(source->LockRect(0, &locked, nullptr, 0), "PostFX source LockRect");
    for (UINT y = 0; y < Extent; ++y)
      for (UINT x = 0; x < Extent; ++x)
        std::memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4, &Color, 4);
    Check(source->UnlockRect(0), "PostFX source UnlockRect");
    Check(device->CreateRenderTarget(Extent, Extent, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr), "PostFX output RT");
    Check(device->ColorFill(target.ptr(), nullptr, 0xffe010e0u), "PostFX sentinel fill");
    // Default threshold=1.18 and knee=.5; our max linear channel is about .22,
    // below threshold-knee=.68. Bloom is zero, ACES off, exposure 1, so the
    // shader's gamma decode/encode roundtrip preserves this uniform image.
    // Allow one 8-bit LSB for GPU pow/UNORM rounding; do not alter the algorithm.
    dxvk::War3PostFxSettings settings{};
    Check(device->BeginScene(), "PostFX BeginScene");
    const bool applied = post.Apply(source.ptr(), target.ptr(), settings);
    Check(device->EndScene(), "PostFX EndScene");
    Require(applied, "PostFX Apply failed");
    out[PostFxApplied] = 1;
    CompleteGpu(device);
    out[GpuCompleted] = 1;
    Check(device->CreateOffscreenPlainSurface(Extent, Extent, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr), "PostFX readback surface");
    Check(device->GetRenderTargetData(target.ptr(), readback.ptr()), "PostFX readback");
    Check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "PostFX readback LockRect");
    for (UINT y = 0; y < Extent; ++y) {
      for (UINT x = 0; x < Extent; ++x) {
        uint32_t actual = 0;
        std::memcpy(&actual, static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
        bool match = true;
        for (unsigned shift = 0; shift < 32; shift += 8) {
          const int difference = int((actual >> shift) & 255u) - int((Color >> shift) & 255u);
          match &= difference >= -1 && difference <= 1;
        }
        ++out[PixelsChecked];
        if (!match) ++out[PixelFailures];
      }
    }
    Check(readback->UnlockRect(), "PostFX readback UnlockRect");
    post.Shutdown();
    Require(!post.IsReady(), "PostFX Shutdown left ready state");
  }
  const uint64_t afterDraws = DrawCount(device);
  Require(afterDraws >= beforeDraws + 12, "PostFX did not record all twelve draws");
  out[GpuDraws] = afterDraws - beforeDraws;
  Require(out[PixelsChecked] == Extent * Extent && !out[PixelFailures], "PostFX output pixels differ");
}

void RunInstanceBuffer(dxvk::D3D9DeviceEx* device, uint64_t* out) {
  Require(!InstanceOwner::GetActive(), "instance fixture started with another active batch");
  InstanceOwner* buffer = nullptr;
  if (dxvk::war3::War3Imgui::get().isOwner(device)) {
    buffer = InstanceOwner::Get(device);
    Require(buffer, "primary instance owner missing");
    Require(!primaryInstances || (primaryInstanceDevice == device && primaryInstances == buffer),
        "primary instance owner changed across Reset");
    primaryInstanceDevice = device;
    primaryInstances = buffer;
  } else {
    Require(primaryInstanceDevice && primaryInstances && primaryInstanceDevice != device,
        "secondary instance fixture has no live primary");
    ActiveInstanceScope active(primaryInstances);
    // Get must not initialize a second device by clearing the first's TLS.
    buffer = InstanceOwner::Get(device);
    Require(buffer && buffer != primaryInstances, "instance owner shared across devices");
    Require(InstanceOwner::GetActive() == primaryInstances &&
        InstanceOwner::GetActive(primaryInstanceDevice) == primaryInstances &&
        !InstanceOwner::GetActive(device), "second owner construction contaminated active TLS");
    uint32_t offset = 0;
    Instance* witness = primaryInstances->Alloc(1, offset);
    Require(witness, "primary lock witness allocation failed");
    InstanceUnlockScope unlocked(primaryInstances);
    const Instance zero{};
    std::memcpy(witness, &zero, sizeof(zero));
    const float constants[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 5, 7, 1};
    Check(device->SetVertexShaderConstantF(14, constants, 4), "Secondary constant write");
    Require(std::memcmp(witness, &zero, sizeof(zero)) == 0,
        "secondary constants wrote primary instance allocation");
    Check(primaryInstanceDevice->SetVertexShaderConstantF(14, constants, 4), "Primary constant write");
    Require(std::memcmp(witness->worldMatrix, constants, sizeof(constants)) == 0,
        "owner-matched constant capture was disabled");
    buffer->OnLostDevice();
    buffer->OnResetDevice();
    Require(InstanceOwner::GetActive(primaryInstanceDevice) == primaryInstances &&
        !InstanceOwner::GetActive(device) &&
        std::memcmp(witness->worldMatrix, constants, sizeof(constants)) == 0,
        "secondary resource reset altered primary TLS/allocation");
    out[InstanceOtherOwner] = out[InstanceTlsIsolation] = 1;
  }
  Require(buffer == InstanceOwner::Get(device), "lazy device owner was not stable");
  Require(PublicRefs(device) == out[RefsBefore], "instance owner retained public device refs");
  out[InstanceBufferReady] = out[InstanceStableOwner] = 1;
  {
    dxvk::Com<IDirect3DVertexBuffer9> oldStream;
    UINT oldOffset = 0, oldStride = 0;
    Check(device->GetStreamSource(1, &oldStream, &oldOffset, &oldStride), "Save instance stream");
    dxvk::Com<IDirect3DBaseTexture9> oldTexture0, oldTexture1;
    Check(device->GetTexture(0, &oldTexture0), "Save instance texture zero");
    Check(device->GetTexture(1, &oldTexture1), "Save instance texture one");
    const ULONG baseline = PublicRefs(device);
    uint32_t offset = 0;
    Instance* mapped = buffer->Alloc(3, offset);
    Require(mapped, "instance storage allocation failed");
    InstanceUnlockScope unlocked(buffer);
    Instance expected[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t j = 0; j < 16; ++j) expected[i].worldMatrix[j] = float(i * 16 + j);
      for (uint32_t j = 0; j < 4; ++j) expected[i].color[j] = float(j) * .25f;
      expected[i].teamColorIndex = float(i);
    }
    std::memcpy(mapped, expected, sizeof(expected));
    Require(std::memcmp(mapped, expected, sizeof(expected)) == 0, "mapped instance bytes differ");
    uint32_t rejectedOffset = 0xfeedu;
    Require(!buffer->Alloc(1, rejectedOffset), "instance duplicate Lock was accepted");
    Require(std::memcmp(mapped, expected, sizeof(expected)) == 0, "duplicate Lock damaged live payload");
    // Size of this verified three-instance payload, not a performance counter
    // for all stores (cross-device, reuse and teardown witnesses are separate).
    out[InstanceBytesWritten] = sizeof(expected);
    unlocked.unlock();
    Require(!buffer->Alloc(0, rejectedOffset), "instance zero allocation was accepted");
    Require(!buffer->Alloc(UINT32_MAX, rejectedOffset), "instance oversized allocation was accepted");
    // A rejection cannot poison the next legal batch, and Unlock must make
    // successive allocations usable. This fixture remains far from ring wrap.
    uint32_t expectedOffset = offset + 3;
    for (uint32_t batch = 0; batch < 2; ++batch) {
      const uint32_t count = batch + 1;
      uint32_t nextOffset = 0;
      Instance* next = buffer->Alloc(count, nextOffset);
      Require(next && nextOffset == expectedOffset, "legal instance allocation did not recover after rejection/Unlock");
      InstanceUnlockScope nextUnlock(buffer);
      Instance nextExpected[2]{};
      for (uint32_t i = 0; i < count; ++i) {
        nextExpected[i].worldMatrix[0] = float(11 + batch);
        nextExpected[i].worldMatrix[15] = 1.f;
        nextExpected[i].color[3] = 1.f;
        nextExpected[i].teamColorIndex = float(i + batch);
      }
      std::memcpy(next, nextExpected, count * sizeof(Instance));
      Require(std::memcmp(next, nextExpected, count * sizeof(Instance)) == 0,
          "successive instance payload mismatch");
      nextUnlock.unlock();
      expectedOffset += count;
    }
    buffer->Bind(1, offset);
    {
      dxvk::Com<IDirect3DVertexBuffer9> bound;
      UINT boundOffset = 0, stride = 0;
      Check(device->GetStreamSource(1, &bound, &boundOffset, &stride), "Read bound instance stream");
      Require(bound.ptr() && stride == sizeof(Instance) && boundOffset == offset * sizeof(Instance),
          "instance buffer was not bound with actual allocation/stride");
      D3DVERTEXBUFFER_DESC desc{};
      Check(bound->GetDesc(&desc), "Instance buffer description");
      Require(desc.Pool == D3DPOOL_DEFAULT && (desc.Usage & D3DUSAGE_DYNAMIC) &&
          desc.Size >= boundOffset + sizeof(expected), "wrong native instance storage");
      dxvk::Com<IDirect3DDevice9> owner;
      Check(bound->GetDevice(&owner), "Instance buffer public owner");
      Require(owner.ptr() == device, "instance buffer belongs to another device");
      out[InstanceStride] = stride;
      out[InstanceBound] = 1;
    }
    Require(PublicRefs(device) == baseline, "instance binding retained public device refs");

    constexpr uint32_t TextureColor = 0xff305070u;
    dxvk::Com<IDirect3DTexture9> texture;
    Check(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr),
        "Instance capture texture create");
    D3DLOCKED_RECT locked{};
    Check(texture->LockRect(0, &locked, nullptr, 0), "Instance texture upload lock");
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        std::memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4, &TextureColor, 4);
    Check(texture->UnlockRect(0), "Instance texture upload unlock");
    IDirect3DBaseTexture9* identity = texture.ptr();
    {
      // Exercise the real SetTexture capture hook with an empty batch. No DIP
      // is suppressed or replayed; this remains storage/ownership acceptance.
      uint32_t captureOffset = 0;
      Require(buffer->Alloc(1, captureOffset), "texture capture lock fixture failed");
      InstanceUnlockScope captureUnlock(buffer);
      ActiveInstanceScope active(buffer);
      Check(device->SetTexture(0, identity), "Bind capture texture");
      Check(device->SetTexture(1, nullptr), "Clear second capture texture");
    }
    buffer->CaptureTextureState();
    Check(device->SetTexture(0, nullptr), "Unbind captured texture");
    texture = nullptr; // Only production private references now keep it alive.
    Require(PublicRefs(device) == baseline, "captured texture retained public device owner");
    Require(!buffer->CheckTextureState(), "texture comparison failed to detect changed state");
    buffer->RestoreTextureState();
    Require(buffer->CheckTextureState(), "captured texture failed to restore");
    {
      dxvk::Com<IDirect3DBaseTexture9> restored;
      Check(device->GetTexture(0, &restored), "Read restored captured texture");
      Require(restored.ptr() == identity, "private captured texture identity changed");
      auto* concrete = static_cast<IDirect3DTexture9*>(restored.ptr());
      Check(concrete->LockRect(0, &locked, nullptr, D3DLOCK_READONLY), "Retained texture read lock");
      for (UINT y = 0; y < 4; ++y) {
        for (UINT x = 0; x < 4; ++x) {
          uint32_t actual = 0;
          std::memcpy(&actual, static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
          ++out[InstanceTexturePixels];
          if (actual != TextureColor) ++out[InstanceTextureFailures];
        }
      }
      Check(concrete->UnlockRect(0), "Retained texture read unlock");
    }
    Require(out[InstanceTexturePixels] == 16 && !out[InstanceTextureFailures],
        "private captured texture pixels changed after public Release");
    out[InstanceTextureRetained] = 1;
    Check(device->SetTexture(0, oldTexture0.ptr()), "Restore texture zero");
    Check(device->SetTexture(1, oldTexture1.ptr()), "Restore texture one");
    Check(device->SetStreamSource(1, oldStream.ptr(), oldOffset, oldStride), "Restore instance stream");
    Require(PublicRefs(device) == baseline, "instance capture/restore retained public refs");
  }
}
} // namespace

extern "C" __declspec(dllexport) HRESULT __cdecl WarVKTestDeviceOwned(
    IDirect3DDevice9* opaqueDevice, uint32_t operation, uint64_t* out, uint32_t count) {
  if (!out || count != FieldCount || operation > InstanceBufferArm ||
      (!opaqueDevice && operation != Snapshot))
    return E_INVALIDARG;
  std::fill(out, out + FieldCount, uint64_t(0));
  out[Version] = 1;
  out[Op] = operation;
  // Only the companion DLL's own public D3D9 factory may supply this pointer.
  auto* device = static_cast<dxvk::D3D9DeviceEx*>(opaqueDevice);
  out[RefsBefore] = PublicRefs(opaqueDevice);
  try {
    Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"War3.exe"), "game process forbidden");
    switch (operation) {
      case Snapshot:
        if (!device) { primaryInstanceDevice = nullptr; primaryInstances = nullptr; }
        break;
      case UiDraw: RenderUi(device, out); break;
      case CursorSmall: Cursor(device, 8, 0xff80c040u, out); break;
      case CursorSmallUpdate: Cursor(device, 8, 0xffe05060u, out); break;
      case CursorLarge: Cursor(device, 17, 0xff40a0e0u, out); break;
      case PostFx: RunPostFx(device, false, out); break;
      case PostFxPartialRetry: RunPostFx(device, true, out); break;
      case InstanceBuffer: RunInstanceBuffer(device, out); break;
      case InstanceBufferArm: {
        Require(!InstanceOwner::GetActive(), "cannot arm over another active owner");
        InstanceOwner* buffer = InstanceOwner::Get(device);
        Require(buffer, "instance owner absent at final Release");
        uint32_t finalOffset = 0;
        Instance* finalMapped = buffer->Alloc(1, finalOffset);
        Require(finalMapped, "final Release lock witness allocation failed");
        Instance finalPayload{};
        for (uint32_t i = 0; i < 16; ++i) finalPayload.worldMatrix[i] = float(i + 21);
        finalPayload.color[0] = .25f;
        finalPayload.color[3] = 1.f;
        finalPayload.teamColorIndex = 7.f;
        std::memcpy(finalMapped, &finalPayload, sizeof(finalPayload));
        Require(std::memcmp(finalMapped, &finalPayload, sizeof(finalPayload)) == 0,
            "final Release lock witness payload mismatch");
        // Intentionally do not Unlock here. The immediately following public
        // device Release must close the mapping before Flush/CS/GPU drain,
        // while retaining actual resources until that drain has completed.
        if (device == primaryInstanceDevice) {
          Require(buffer == primaryInstances, "primary identity changed before final Release");
          primaryInstanceDevice = nullptr;
          primaryInstances = nullptr;
        }
        InstanceOwner::SetActive(buffer);
        Require(InstanceOwner::GetActive(device) == buffer, "owner arm failed");
        break;
      }
      case WrongOwner: {
        auto& ui = dxvk::war3::War3Imgui::get();
        Require(ui.isInitialized() && !ui.isOwner(device), "wrong-owner fixture is not a second device");
        ImGuiContext* context = ImGui::GetCurrentContext();
        uint64_t before[FieldCount]{};
        SnapshotUi(device, before);
        ui.invalidateDeviceObjects(device);
        ui.restoreDeviceObjects(device);
        ui.beginDeviceTeardown(device);
        ui.shutdown(device);
        SnapshotUi(device, out);
        Require(ui.isInitialized() && !ui.isOwner(device) && context == ImGui::GetCurrentContext() &&
            before[AtlasLive] == out[AtlasLive] && before[AtlasPixels] == out[AtlasPixels],
            "non-owner altered the active UI context/resources");
        out[WrongOwnerPreserved] = 1;
        break;
      }
    }
    SnapshotUi(device, out);
    out[RefsAfter] = PublicRefs(opaqueDevice);
    Require(out[RefsAfter] == out[RefsBefore], "operation leaked a public device reference");
    return S_OK;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "DEVICE_OWNER_PROBE_FAILURE {\"op\":%u,\"error\":%s}\n", operation, Quote(error.what()).c_str());
  } catch (...) {
    std::fprintf(stderr, "DEVICE_OWNER_PROBE_FAILURE {\"op\":%u,\"error\":\"unknown-exception\"}\n", operation);
  }
  out[RefsAfter] = PublicRefs(opaqueDevice);
  return E_FAIL;
}
