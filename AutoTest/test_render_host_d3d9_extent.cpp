// Real back-buffer extents through the public D3D9 COM API: identical calls on
// the original x86 renderer and on the x64 transport, with an independent
// per-pixel membership oracle over every pixel of every image. With --present
// (remote, diagnostic Presenter profile) the actual acquired swapchain image of
// each frame must equal the color target read just before that Present, byte
// for byte. With --reset the device then goes through D3D9 Reset to a second
// extent: refused parameters, refused live DEFAULT-pool resources (NotReset),
// a resized window ahead of the Reset, then the same images again, plus a
// MANAGED texture kept across the Reset. --format/--depth/--interval select the
// game's swap chain (X8R8G8B8, D16, IMMEDIATE). --ring replays Game.dll's
// buffer use from the S1 buffer census instead: two dynamic vertex rings and an
// index ring, NOOVERWRITE appends and DISCARD wraps, drawn with its two FVFs.
// --depth-bias adds Game.dll's DEPTHBIAS (render state 195). --resize resizes
// the window instead of a Reset and lets its WM_SIZE through: WarVK's window
// hook resets the swap chain itself, keeping every resource and state except
// RT0's viewport and scissor, and the application never sees the WM_SIZE.
// No game module, no input, nothing deployed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static_assert(sizeof(void*) == 4, "The D3D9 API caller must be an actual x86 process");

namespace {
constexpr DWORD Behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
constexpr uint64_t FnvOffset = 14695981039346656037ull, FnvPrime = 1099511628211ull;
constexpr wchar_t WindowClass[] = L"WarVK_D3D9_Extent_Fixture";
constexpr UINT PresentFrames = 3;

void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void Check(HRESULT hr, const char* operation) {
  if (hr == D3D_OK) return;
  char text[180];
  std::snprintf(text, sizeof(text), "%s: HRESULT 0x%08lx", operation, static_cast<unsigned long>(hr));
  throw std::runtime_error(text);
}
const char* JsonBool(bool value) { return value ? "true" : "false"; }
uint64_t Hash(const void* bytes, size_t size, uint64_t value) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) value = (value ^ data[i]) * FnvPrime;
  return value;
}

template<class T> class Com {
public:
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  T* get() const { return value; }
  T* operator->() const { return value; }
  T** put() { reset(); return &value; }
  T* detach() { T* result = value; value = nullptr; return result; }
  void reset() { if (value) { T* old = detach(); old->Release(); } }
private:
  T* value = nullptr;
};

struct Options {
  std::wstring dll;
  UINT width = 0, height = 0, resetWidth = 0, resetHeight = 0, resizeWidth = 0, resizeHeight = 0;
  bool reference = false, present = false, zeroExtent = false, resetZero = false, ring = false, depthBias = false;
  D3DFORMAT format = D3DFMT_A8R8G8B8, depth = D3DFMT_D32F_LOCKABLE;
  DWORD interval = D3DPRESENT_INTERVAL_ONE;
  // A second extent, by the application's Reset or by the window's own resize.
  bool secondExtent() const { return resetWidth || resizeWidth; }
};

void ParseExtent(const wchar_t* text, UINT& width, UINT& height, const char* reason) {
  unsigned w = 0, h = 0; wchar_t tail = 0;
  Require(swscanf(text, L"%ux%u%lc", &w, &h, &tail) == 2 && w && h && w <= 4096 && h <= 4096, reason);
  width = w; height = h;
}

Options ParseOptions(int argc, wchar_t** argv) {
  Options o;
  bool format = false, depth = false, interval = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    const bool value = i + 1 < argc;
    if (arg == L"--dll" && value && o.dll.empty()) o.dll = argv[++i];
    else if (arg == L"--extent" && value && !o.width) ParseExtent(argv[++i], o.width, o.height, "bad --extent");
    else if (arg == L"--reset" && value && !o.resetWidth) ParseExtent(argv[++i], o.resetWidth, o.resetHeight, "bad --reset");
    else if (arg == L"--resize" && value && !o.resizeWidth) ParseExtent(argv[++i], o.resizeWidth, o.resizeHeight, "bad --resize");
    else if (arg == L"--reference" && !o.reference) o.reference = true;
    else if (arg == L"--present" && !o.present) o.present = true;
    else if (arg == L"--zero-extent" && !o.zeroExtent) o.zeroExtent = true;
    else if (arg == L"--reset-zero" && !o.resetZero) o.resetZero = true;
    else if (arg == L"--ring" && !o.ring) o.ring = true;
    else if (arg == L"--depth-bias" && !o.depthBias) o.depthBias = true;
    else if (arg == L"--format" && value && !format && std::wstring(argv[i + 1]) == L"x8") { o.format = D3DFMT_X8R8G8B8; format = true; ++i; }
    else if (arg == L"--depth" && value && !depth && std::wstring(argv[i + 1]) == L"d16") { o.depth = D3DFMT_D16; depth = true; ++i; }
    else if (arg == L"--interval" && value && !interval) {
      const std::wstring name = argv[++i]; interval = true;
      Require(name == L"immediate" || name == L"default", "bad --interval");
      o.interval = name == L"immediate" ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_DEFAULT;
    } else throw std::runtime_error("usage: --dll ABS --extent WxH [--reference] [--present] [--zero-extent]"
      " [--reset WxH [--reset-zero]] [--resize WxH] [--ring] [--depth-bias] [--format x8] [--depth d16] [--interval immediate|default]");
  }
  Require(o.dll.size() > 3 && o.dll[1] == L':' && o.width, "an absolute DLL and an extent are required");
  Require(!(o.present && o.reference), "--present is remote-only");
  Require(!o.resetZero || o.resetWidth, "--reset-zero needs --reset");
  Require(!(o.ring || o.depthBias) || (!o.present && !o.secondExtent()), "--ring and --depth-bias run without --present, --reset or --resize");
  Require(!o.resizeWidth || !o.resetWidth, "--resize runs without --reset");
  return o;
}
const char* FormatName(D3DFORMAT f) { return f == D3DFMT_X8R8G8B8 ? "x8r8g8b8" : f == D3DFMT_D16 ? "d16" : f == D3DFMT_D32F_LOCKABLE ? "d32f" : "a8r8g8b8"; }
const char* IntervalName(DWORD i) { return i == D3DPRESENT_INTERVAL_IMMEDIATE ? "immediate" : i == D3DPRESENT_INTERVAL_DEFAULT ? "default" : "one"; }

// --resize lets the window's WM_SIZE through; the application's own count of it.
std::atomic<bool> SizeMessages{false};
std::atomic<unsigned> AppSizeMessages{0};

// The HWND owner is its own thread; the API caller never pumps it. Shown only
// for --present, on the non-input desktop the gate created.
struct Window {
  HANDLE ready = nullptr, thread = nullptr;
  std::atomic<HWND> hwnd{nullptr};
  std::atomic<DWORD> failure{0};
  UINT width = 0, height = 0;
  bool visible = false;

  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    // WarVK's window hook answers WM_SIZE with a swap chain reset of its own.
    // Without DefWindowProc here no WM_SIZE is sent, so both renderers keep
    // the old target until the application's Reset; --resize lets it through.
    if (message == WM_WINDOWPOSCHANGED && !SizeMessages.load()) return 0;
    if (message == WM_SIZE) AppSizeMessages.fetch_add(1);
    return DefWindowProcW(window, message, wp, lp);
  }
  static unsigned __stdcall Thread(void* parameter) noexcept {
    auto& self = *static_cast<Window*>(parameter);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance; cls.lpfnWndProc = Proc; cls.lpszClassName = WindowClass;
    if (!RegisterClassExW(&cls)) { self.failure.store(GetLastError()); SetEvent(self.ready); return 2; }
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WindowClass, L"WarVK extent fixture",
      WS_POPUP, 0, 0, int(self.width), int(self.height), nullptr, nullptr, instance, nullptr);
    if (!window) { self.failure.store(GetLastError()); UnregisterClassW(WindowClass, instance); SetEvent(self.ready); return 2; }
    if (self.visible) ShowWindow(window, SW_SHOWNOACTIVATE);
    self.hwnd.store(window);
    SetEvent(self.ready);
    MSG msg{}; BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (result == -1) self.failure.store(GetLastError());
    UnregisterClassW(WindowClass, instance);
    return result == -1 ? 2u : 0u;
  }
  void start(UINT w, UINT h, bool show) {
    width = w; height = h; visible = show;
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr); Require(ready != nullptr, "window readiness event");
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Thread, this, 0, nullptr));
    Require(thread != nullptr && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "window thread");
    RECT client{};
    Require(hwnd.load() && !failure.load() && GetClientRect(hwnd.load(), &client)
      && client.right == LONG(w) && client.bottom == LONG(h), "window client extent");
  }
  // The owner thread processes the synchronous size messages; a borderless
  // popup's client is its whole window.
  void resize(UINT w, UINT h) {
    Require(SetWindowPos(hwnd.load(), nullptr, 0, 0, int(w), int(h), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
      "window resize");
    RECT client{};
    Require(GetClientRect(hwnd.load(), &client) && client.right == LONG(w) && client.bottom == LONG(h), "resized client extent");
    width = w; height = h;
  }
  void stop() {
    Require(PostMessageW(hwnd.load(), WM_CLOSE, 0, 0) != FALSE, "window close post");
    Require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "window shutdown timeout");
    DWORD exit = STILL_ACTIVE;
    Require(GetExitCodeThread(thread, &exit) && exit == 0 && !failure.load(), "window thread failure");
    CloseHandle(thread); CloseHandle(ready); thread = ready = nullptr;
  }
};

using ReadPresentedImage = HRESULT (WINAPI*)(IDirect3DDevice9*, void*, UINT, UINT64*);

struct Screen { float x, y, z, w; DWORD color; };
struct Clip { float x, y, z; DWORD color; };
struct Textured { float x, y, z, w; DWORD color; float u, v; };
constexpr UINT TextureSize = 16;
DWORD Texel(UINT x, UINT y) { return 0xff000000u | (x * 16) << 16 | (y * 16) << 8 | ((x ^ y) * 16); }
// Pixel-aligned rectangle [x0,x1) x [y0,y1) in D3D9 screen space (integral
// pixel centers), as a two-triangle strip.
struct Rect { UINT x0, y0, x1, y1; };

struct Fixture {
  Options options;
  const char* stage = "arguments";
  Window window;
  HMODULE renderer = nullptr;
  Com<IDirect3D9> factory;
  Com<IDirect3DDevice9> device;
  Com<IDirect3DSurface9> back, readback;
  Com<IDirect3DVertexBuffer9> screenQuads, clipQuad, texturedQuad;
  Com<IDirect3DTexture9> texture; // MANAGED: kept across the Reset.
  Com<IDirect3DVertexBuffer9> ringPlain, ringColored, biasQuads;
  Com<IDirect3DIndexBuffer9> ringIndices;
  ReadPresentedImage readPresented = nullptr;
  D3DPRESENT_PARAMETERS params{};
  UINT w = 0, h = 0;
  bool opaque = false; // X8R8G8B8: alpha is undefined, so neither compared nor hashed.
  std::vector<uint32_t> expected;
  std::vector<uint8_t> target; // Tightly packed copy of the last checked image.
  unsigned images = 0, draws = 0, stateChecks = 0, presentedFrames = 0, resetChecks = 0, resizeChecks = 0;
  unsigned ringLocks = 0, ringDiscards = 0, ringWraps = 0;
  uint64_t ringBytes = 0, ringCarriedBytes = 0;
  uint64_t pixelChecks = 0, presentedChecks = 0, pixelHash = FnvOffset, presentedHash = FnvOffset;
  std::vector<Rect> quads;
  std::vector<DWORD> quadColors;
  std::vector<float> quadDepths;

  void fill(DWORD color) { std::fill(expected.begin(), expected.end(), color); }
  void rect(const Rect& r, DWORD color) {
    for (UINT y = r.y0; y < r.y1; ++y) for (UINT x = r.x0; x < r.x1; ++x) expected[size_t(y) * w + x] = color;
  }
  void checkImage() {
    if (!readback.get())
      Check(device->CreateOffscreenPlainSurface(w, h, options.format, D3DPOOL_SYSTEMMEM, readback.put(), nullptr), "readback surface");
    Check(device->GetRenderTargetData(back.get(), readback.get()), "GetRenderTargetData");
    D3DLOCKED_RECT lock{}; Check(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY), "readback lock");
    unsigned failures = 0;
    const DWORD mask = opaque ? 0xff000000u : 0;
    for (UINT y = 0; y < h; ++y) {
      const auto* source = static_cast<const uint8_t*>(lock.pBits) + size_t(y) * lock.Pitch;
      auto* row = target.data() + size_t(y) * w * 4;
      std::memcpy(row, source, size_t(w) * 4);
      if (opaque) for (UINT x = 0; x < w; ++x) row[x * 4 + 3] = 0xff;
      pixelHash = Hash(row, size_t(w) * 4, pixelHash);
      for (UINT x = 0; x < w; ++x) {
        DWORD actual = 0; std::memcpy(&actual, row + x * 4, 4);
        const DWORD want = expected[size_t(y) * w + x] | mask;
        pixelChecks += 4;
        if (actual != want) {
          if (failures < 4) std::fprintf(stderr, "extent mismatch image=%u x=%u y=%u actual=%08lx expected=%08lx\n",
            images, x, y, static_cast<unsigned long>(actual), static_cast<unsigned long>(want));
          ++failures;
        }
      }
    }
    Check(readback->UnlockRect(), "readback unlock");
    Require(!failures, "extent membership mismatch");
    ++images;
  }
  void drawQuad(size_t index) {
    Check(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, UINT(index * 4), 2), "screen quad"); ++draws;
  }
  void resizeImages() { expected.assign(size_t(w) * h, 0); target.assign(size_t(w) * h * 4, 0); }
};

void CreateDevice(Fixture& f) {
  f.stage = "window-and-device";
  Require(!GetModuleHandleW(L"Game.dll"), "game module present");
  if (f.options.present) {
    wchar_t profile[64]{};
    const DWORD count = GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE", profile, 64);
    Require(count && count < 64 && std::wcscmp(profile, L"presenter-readback-isolated-v1") == 0,
      "--present requires WARVK_D3D9_PROFILE=presenter-readback-isolated-v1");
  }
  SizeMessages.store(f.options.resizeWidth != 0);
  f.window.start(f.options.width, f.options.height, f.options.present);
  f.renderer = LoadLibraryW(f.options.dll.c_str()); Require(f.renderer != nullptr, "renderer load");
  if (f.options.present) {
    f.readPresented = reinterpret_cast<ReadPresentedImage>(GetProcAddress(f.renderer, "WarVKD3D9ReadPresentedImage"));
    Require(f.readPresented != nullptr, "presented-image diagnostic export");
  }
  const auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(f.renderer, "Direct3DCreate9"));
  Require(create != nullptr, "Direct3DCreate9 export"); *f.factory.put() = create(D3D_SDK_VERSION);
  Require(f.factory.get() != nullptr, "factory");
  auto& p = f.params;
  p.BackBufferWidth = f.options.zeroExtent ? 0 : f.options.width;
  p.BackBufferHeight = f.options.zeroExtent ? 0 : f.options.height;
  p.BackBufferFormat = f.options.format; p.BackBufferCount = 1; p.SwapEffect = D3DSWAPEFFECT_DISCARD;
  p.hDeviceWindow = f.window.hwnd.load(); p.Windowed = TRUE;
  p.EnableAutoDepthStencil = TRUE; p.AutoDepthStencilFormat = f.options.depth;
  p.PresentationInterval = f.options.interval;
  f.opaque = f.options.format == D3DFMT_X8R8G8B8;
  f.stage = "create-device";
  auto created = p;
  Check(f.factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, created.hDeviceWindow, Behavior, &created, f.device.put()), "CreateDevice");
  // A zero windowed axis takes the client extent and is written back.
  Require(created.BackBufferWidth == f.options.width && created.BackBufferHeight == f.options.height, "chosen back buffer extent");
  ++f.stateChecks;
  f.w = f.options.width; f.h = f.options.height;
  f.resizeImages();
  Check(f.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, f.back.put()), "GetBackBuffer");
  D3DSURFACE_DESC desc{}; Check(f.back->GetDesc(&desc), "back buffer desc");
  Require(desc.Width == f.w && desc.Height == f.h && desc.Format == f.options.format, "back buffer description"); ++f.stateChecks;
  D3DVIEWPORT9 viewport{}; Check(f.device->GetViewport(&viewport), "default viewport");
  Require(!viewport.X && !viewport.Y && viewport.Width == f.w && viewport.Height == f.h, "default viewport covers the target");
  RECT scissor{}; Check(f.device->GetScissorRect(&scissor), "default scissor");
  Require(!scissor.left && !scissor.top && scissor.right == LONG(f.w) && scissor.bottom == LONG(f.h), "default scissor covers the target");
  f.stateChecks += 2;
  // A viewport reaching past the target is refused by the remote profile; the
  // original stores it. Only the last valid pixel row/column is required here.
  const D3DVIEWPORT9 edge{f.w - 1, f.h - 1, 1, 1, 0, 1};
  Check(f.device->SetViewport(&edge), "edge viewport"); Check(f.device->SetViewport(&viewport), "restore viewport");
  ++f.stateChecks;
}

void Prepare(Fixture& f) {
  f.stage = "fixed-function-state";
  auto& d = *f.device.get();
  Check(d.SetRenderState(D3DRS_LIGHTING, FALSE), "unlit");
  Check(d.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "no cull");
  Check(d.SetRenderState(D3DRS_ZENABLE, FALSE), "no depth");
  Check(d.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), "no blend");
  Check(d.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "diffuse color op");
  Check(d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "diffuse color");
  Check(d.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), "diffuse alpha op");
  Check(d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "diffuse alpha");
  D3DMATRIX identity{}; for (unsigned i = 0; i < 4; ++i) identity.m[i][i] = 1;
  for (auto t : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION}) Check(d.SetTransform(t, &identity), "identity transform");
  const UINT w = f.w, h = f.h;
  // 0 full target; 1 lower-right region; 2 depth far; 3 depth near; 4.. one per presented frame.
  f.quads = {{0, 0, w, h}, {w * 3 / 4, h * 2 / 3, w - 1, h - 1}, {w / 4, h / 4, w * 3 / 4, h * 3 / 4}, {w / 4, h / 4, w * 3 / 4, h * 3 / 4}};
  f.quadColors = {0xff40c070, 0xff1080f0, 0xffff0000, 0xff00ff00};
  f.quadDepths = {.5f, .5f, .75f, .25f};
  for (UINT i = 0; i < PresentFrames; ++i) {
    f.quads.push_back({w - 1 - (i + 1) * w / 8, i * h / 5, w - 1 - i * w / 8, h - 1 - i * h / 7});
    f.quadColors.push_back(0xff000000u | (0x30u + i * 0x40u) << 16 | (0xe0u - i * 0x30u) << 8 | (0x20u + i * 0x50u));
    f.quadDepths.push_back(.5f);
  }
  std::vector<Screen> vertices;
  for (size_t i = 0; i < f.quads.size(); ++i) {
    const auto& r = f.quads[i]; const float z = f.quadDepths[i]; const DWORD c = f.quadColors[i];
    vertices.insert(vertices.end(), {{float(r.x0), float(r.y0), z, 1, c}, {float(r.x1), float(r.y0), z, 1, c},
      {float(r.x0), float(r.y1), z, 1, c}, {float(r.x1), float(r.y1), z, 1, c}});
  }
  void* data = nullptr;
  Check(d.CreateVertexBuffer(UINT(vertices.size() * sizeof(Screen)), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
    D3DPOOL_DEFAULT, f.screenQuads.put(), nullptr), "screen VB");
  Check(f.screenQuads->Lock(0, 0, &data, 0), "screen VB lock");
  std::memcpy(data, vertices.data(), vertices.size() * sizeof(Screen)); Check(f.screenQuads->Unlock(), "screen VB upload");
  // Clip-space quad for the offset viewport: covers exactly the viewport.
  const DWORD clipColor = 0xffe0d000;
  const Clip clip[] = {{-1, 1, .5f, clipColor}, {1, 1, .5f, clipColor}, {-1, -1, .5f, clipColor}, {1, -1, .5f, clipColor}};
  Check(d.CreateVertexBuffer(sizeof(clip), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, f.clipQuad.put(), nullptr), "clip VB");
  Check(f.clipQuad->Lock(0, 0, &data, 0), "clip VB lock"); std::memcpy(data, clip, sizeof(clip)); Check(f.clipQuad->Unlock(), "clip VB upload");
  if (!f.options.secondExtent()) return;
  // Reset and resize modes: a 1:1 textured quad centred on the target. Half-pixel edges put
  // every pixel centre on a texel centre, so point sampling is exact.
  const float x0 = float(w / 2 - TextureSize / 2) - .5f, y0 = float(h / 2 - TextureSize / 2) - .5f;
  const float x1 = x0 + TextureSize, y1 = y0 + TextureSize;
  const Textured textured[] = {{x0, y0, .5f, 1, 0xffffffff, 0, 0}, {x1, y0, .5f, 1, 0xffffffff, 1, 0},
    {x0, y1, .5f, 1, 0xffffffff, 0, 1}, {x1, y1, .5f, 1, 0xffffffff, 1, 1}};
  Check(d.CreateVertexBuffer(sizeof(textured), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
    D3DPOOL_DEFAULT, f.texturedQuad.put(), nullptr), "textured VB");
  Check(f.texturedQuad->Lock(0, 0, &data, 0), "textured VB lock"); std::memcpy(data, textured, sizeof(textured));
  Check(f.texturedQuad->Unlock(), "textured VB upload");
  if (f.texture.get()) return;
  Check(d.CreateTexture(TextureSize, TextureSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, f.texture.put(), nullptr), "managed texture");
  D3DLOCKED_RECT lock{}; Check(f.texture->LockRect(0, &lock, nullptr, 0), "texture lock");
  for (UINT y = 0; y < TextureSize; ++y) for (UINT x = 0; x < TextureSize; ++x) {
    const DWORD texel = Texel(x, y); std::memcpy(static_cast<uint8_t*>(lock.pBits) + size_t(y) * lock.Pitch + x * 4, &texel, 4);
  }
  Check(f.texture->UnlockRect(0), "texture upload");
}

void BindScreen(Fixture& f) {
  Check(f.device->SetStreamSource(0, f.screenQuads.get(), 0, sizeof(Screen)), "screen stream");
  Check(f.device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "screen FVF");
}

void Exercise(Fixture& f) {
  auto& d = *f.device.get();
  const UINT w = f.w, h = f.h;
  constexpr DWORD base = 0xff203040;
  const D3DVIEWPORT9 full{0, 0, w, h, 0, 1};
  f.stage = "full-clear";
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, base, 1, 0), "full clear");
  f.fill(base); f.checkImage();

  f.stage = "edge-rectangle-clears";
  const DWORD edgeColor = 0xffc08010;
  const D3DRECT rects[] = {{LONG(w) - 100, LONG(h) - 50, LONG(w) + 20, LONG(h) + 20}, {0, LONG(h) - 1, LONG(w), LONG(h)},
    {LONG(w) - 1, 0, LONG(w), 1}, {-10, -10, 5, 7}};
  Check(d.Clear(4, rects, D3DCLEAR_TARGET, edgeColor, 1, 0), "edge rectangles");
  for (const auto& r : rects) {
    const LONG x0 = std::max<LONG>(0, r.x1), y0 = std::max<LONG>(0, r.y1);
    const LONG x1 = std::min<LONG>(LONG(w), r.x2), y1 = std::min<LONG>(LONG(h), r.y2);
    if (x1 > x0 && y1 > y0) f.rect({UINT(x0), UINT(y0), UINT(x1), UINT(y1)}, edgeColor);
  }
  f.checkImage();

  f.stage = "offset-viewport-clear";
  const D3DVIEWPORT9 offset{w / 2 + 3, h / 3, w / 4, h / 5, 0, 1};
  const DWORD viewportColor = 0xff7020a0;
  Check(d.SetViewport(&offset), "offset viewport");
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, viewportColor, 1, 0), "viewport clear");
  f.rect({offset.X, offset.Y, offset.X + offset.Width, offset.Y + offset.Height}, viewportColor);
  Check(d.SetViewport(&full), "full viewport"); f.checkImage();

  f.stage = "default-scissor";
  BindScreen(f);
  // Never set by the caller: the default rectangle must be the whole target.
  Check(d.SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE), "scissor on");
  Check(d.BeginScene(), "scene"); f.drawQuad(0); Check(d.EndScene(), "scene end");
  f.fill(f.quadColors[0]); f.checkImage();

  f.stage = "explicit-scissor";
  const RECT scissor{LONG(w / 3), LONG(h / 2), LONG(std::min(w, w / 3 + 257)), LONG(std::min(h, h / 2 + 129))};
  Check(d.SetScissorRect(&scissor), "scissor rectangle");
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, base, 1, 0), "clear under scissor");
  f.fill(f.quadColors[0]);
  f.rect({UINT(scissor.left), UINT(scissor.top), UINT(scissor.right), UINT(scissor.bottom)}, base);
  f.checkImage();  // Clear honours the scissor rectangle too.
  Check(d.SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE), "scissor off");
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, base, 1, 0), "clear");
  Check(d.SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE), "scissor on");
  Check(d.BeginScene(), "scene"); f.drawQuad(0); Check(d.EndScene(), "scene end");
  Check(d.SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE), "scissor off");
  f.fill(base); f.rect({UINT(scissor.left), UINT(scissor.top), UINT(scissor.right), UINT(scissor.bottom)}, f.quadColors[0]);
  f.checkImage();

  f.stage = "far-region-and-offset-viewport";
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, base, 1, 0), "clear");
  const D3DVIEWPORT9 view{w / 5, h / 4, w / 2, h / 2, 0, 1};
  Check(d.BeginScene(), "scene");
  f.drawQuad(1);
  Check(d.SetViewport(&view), "draw viewport");
  Check(d.SetStreamSource(0, f.clipQuad.get(), 0, sizeof(Clip)), "clip stream");
  Check(d.SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE), "clip FVF");
  Check(d.DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2), "viewport quad"); ++f.draws;
  Check(d.EndScene(), "scene end");
  Check(d.SetViewport(&full), "full viewport"); BindScreen(f);
  f.fill(base); f.rect(f.quads[1], f.quadColors[1]);
  f.rect({view.X, view.Y, view.X + view.Width, view.Y + view.Height}, 0xffe0d000);
  f.checkImage();

  f.stage = "depth";
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, base, .5f, 0), "depth clear");
  Check(d.SetRenderState(D3DRS_ZENABLE, TRUE), "depth on"); Check(d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS), "depth less");
  Check(d.SetRenderState(D3DRS_ZWRITEENABLE, FALSE), "depth read only");
  Check(d.BeginScene(), "scene"); f.drawQuad(2); f.drawQuad(3); Check(d.EndScene(), "scene end");
  Check(d.SetRenderState(D3DRS_ZENABLE, FALSE), "depth off"); Check(d.SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "depth writes");
  f.fill(base); f.rect(f.quads[3], f.quadColors[3]); f.checkImage();
}

// Reset and resize modes, both renderers: the MANAGED texture through stage 0, then an
// alpha-blended quad that reads destination alpha. A target cleared with alpha
// zero gives black on A8R8G8B8; X8R8G8B8 reads that alpha as one.
void ExerciseTextureAndAlpha(Fixture& f) {
  if (!f.options.secondExtent()) return;
  auto& d = *f.device.get();
  constexpr DWORD base = 0xff203040;
  f.stage = "managed-texture";
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, base, 1, 0), "texture clear");
  Check(d.SetTexture(0, f.texture.get()), "bind texture");
  Check(d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "texture color");
  Check(d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), "texture alpha");
  for (auto s : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER}) Check(d.SetSamplerState(0, s, D3DTEXF_POINT), "point filter");
  Check(d.SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "no mip filter");
  for (auto s : {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV}) Check(d.SetSamplerState(0, s, D3DTADDRESS_CLAMP), "clamp");
  Check(d.SetStreamSource(0, f.texturedQuad.get(), 0, sizeof(Textured)), "textured stream");
  Check(d.SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1), "textured FVF");
  Check(d.BeginScene(), "scene"); Check(d.DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2), "textured quad"); ++f.draws;
  Check(d.EndScene(), "scene end");
  Check(d.SetTexture(0, nullptr), "unbind texture");
  Check(d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "diffuse color");
  Check(d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "diffuse alpha");
  f.fill(base);
  const UINT x0 = f.w / 2 - TextureSize / 2, y0 = f.h / 2 - TextureSize / 2;
  for (UINT y = 0; y < TextureSize; ++y) for (UINT x = 0; x < TextureSize; ++x) f.expected[size_t(y0 + y) * f.w + x0 + x] = Texel(x, y);
  f.checkImage();

  f.stage = "destination-alpha";
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, 0x00336699, 1, 0), "alpha-zero clear");
  BindScreen(f);
  Check(d.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE), "blend on");
  Check(d.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTALPHA), "source by destination alpha");
  Check(d.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO), "drop destination");
  Check(d.BeginScene(), "scene"); f.drawQuad(0); Check(d.EndScene(), "scene end");
  Check(d.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), "blend off");
  Check(d.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE), "source one");
  f.fill(f.opaque ? f.quadColors[0] : 0); f.checkImage();
}

// Game.dll's buffers (S1 buffer census): a 512 KiB XYZ|NORMAL|TEX1 ring, a
// 576 KiB XYZ|NORMAL|DIFFUSE|TEX1 ring and a 96 KiB INDEX16 ring, all DEFAULT
// pool, DYNAMIC|WRITEONLY. Each Lock appends with NOOVERWRITE, or wraps to 0
// with DISCARD when it does not fit; vertex Locks carry NOSYSLOCK like the
// game's. Lock sizes are larger than the game's so that every ring wraps here.
struct PlainVertex { float x, y, z, nx, ny, nz, u, v; };
struct ColoredVertex { float x, y, z, nx, ny, nz; DWORD color; float u, v; };
static_assert(sizeof(PlainVertex) == 32 && sizeof(ColoredVertex) == 36, "game FVF strides");
constexpr DWORD PlainFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1, ColoredFvf = PlainFvf | D3DFVF_DIFFUSE;
constexpr DWORD RingUsage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
constexpr UINT PlainRingBytes = 524288, ColoredRingBytes = 589824, IndexRingBytes = 98304;
constexpr UINT RingFrames = 8, RingDraws = 8, RingQuads = 4;
// 160 KiB plain Locks: three fit in the ring and a frame takes four, so every
// frame wraps it and DISCARDs a range an earlier draw of that frame still reads.
constexpr UINT PlainLockVertices = 5120, ColoredLockVertices = 88, ColoredFillerVertices = 3500, IndexLock = 4096;

struct RingCursor { UINT size = 0, offset = 0, written = 0; bool used = false; };

// The next Lock range and its flags: NOOVERWRITE while it fits, else DISCARD at 0.
UINT Reserve(Fixture& f, RingCursor& ring, UINT bytes, DWORD hint, DWORD& flags) {
  flags = hint | D3DLOCK_NOOVERWRITE;
  if (!ring.used || ring.offset + bytes > ring.size) {
    flags = hint | D3DLOCK_DISCARD;
    if (ring.used) ++f.ringWraps;
    ++f.ringDiscards;
    // The x64 host carries the old initialized bytes into the new storage.
    f.ringCarriedBytes += ring.written;
    ring.offset = 0;
  }
  ring.used = true;
  const UINT at = ring.offset;
  ring.offset += bytes;
  ring.written = std::max(ring.written, ring.offset);
  ++f.ringLocks; f.ringBytes += bytes;
  return at;
}

template<class Buffer> void WriteRing(Buffer* buffer, UINT at, const void* bytes, UINT size, DWORD flags) {
  void* data = nullptr;
  Check(buffer->Lock(at, size, &data, flags), "ring Lock");
  std::memcpy(data, bytes, size);
  Check(buffer->Unlock(), "ring Unlock");
}

void ExerciseRing(Fixture& f) {
  if (!f.options.ring) return;
  auto& d = *f.device.get();
  const UINT w = f.w, h = f.h;
  f.stage = "ring-buffers";
  Check(d.CreateVertexBuffer(PlainRingBytes, RingUsage, PlainFvf, D3DPOOL_DEFAULT, f.ringPlain.put(), nullptr), "plain ring");
  Check(d.CreateVertexBuffer(ColoredRingBytes, RingUsage, ColoredFvf, D3DPOOL_DEFAULT, f.ringColored.put(), nullptr), "colored ring");
  Check(d.CreateIndexBuffer(IndexRingBytes, RingUsage, D3DFMT_INDEX16, D3DPOOL_DEFAULT, f.ringIndices.put(), nullptr), "index ring");
  D3DVERTEXBUFFER_DESC vd{}; Check(f.ringColored->GetDesc(&vd), "colored ring desc");
  Require(vd.Size == ColoredRingBytes && vd.Usage == RingUsage && vd.Pool == D3DPOOL_DEFAULT && vd.FVF == ColoredFvf, "ring description");
  D3DINDEXBUFFER_DESC id{}; Check(f.ringIndices->GetDesc(&id), "index ring desc");
  Require(id.Size == IndexRingBytes && id.Usage == RingUsage && id.Format == D3DFMT_INDEX16, "index ring description");
  f.stateChecks += 2;
  Check(d.CreateTexture(TextureSize, TextureSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, f.texture.put(), nullptr), "ring texture");
  D3DLOCKED_RECT lock{}; Check(f.texture->LockRect(0, &lock, nullptr, 0), "ring texture lock");
  for (UINT y = 0; y < TextureSize; ++y) for (UINT x = 0; x < TextureSize; ++x) {
    const DWORD texel = Texel(x, y); std::memcpy(static_cast<uint8_t*>(lock.pBits) + size_t(y) * lock.Pitch + x * 4, &texel, 4);
  }
  Check(f.texture->UnlockRect(0), "ring texture upload");
  Check(d.SetTexture(0, f.texture.get()), "ring texture bind");
  for (auto s : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER}) Check(d.SetSamplerState(0, s, D3DTEXF_POINT), "point filter");
  Check(d.SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "no mip filter");
  Check(d.SetIndices(f.ringIndices.get()), "ring indices");

  // Cell i of an 8x4 grid, inset one pixel, in clip space with half-pixel edges.
  const UINT cw = w / 8, ch = h / 4;
  Require(cw >= 3 && ch >= 3, "ring extent too small");
  auto cell = [&](UINT i) { return Rect{(i % 8) * cw + 1, (i / 8) * ch + 1, (i % 8 + 1) * cw - 1, (i / 8 + 1) * ch - 1}; };
  auto clipX = [&](UINT x) { return 2.f * (float(x) - .5f) / float(w) - 1.f; };
  auto clipY = [&](UINT y) { return 1.f - 2.f * (float(y) - .5f) / float(h); };
  std::vector<uint16_t> indices(IndexLock, 0);
  for (UINT q = 0; q < RingQuads; ++q) {
    const uint16_t b = uint16_t(q * 4), quad[] = {b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2), uint16_t(b + 1), uint16_t(b + 3)};
    std::copy(std::begin(quad), std::end(quad), indices.begin() + q * 6);
  }
  std::vector<PlainVertex> plain(PlainLockVertices);
  std::vector<ColoredVertex> colored(ColoredLockVertices), filler(ColoredFillerVertices);
  std::memset(plain.data(), 0x7f, plain.size() * sizeof(PlainVertex));
  std::memset(colored.data(), 0x7f, colored.size() * sizeof(ColoredVertex));
  std::memset(filler.data(), 0x7f, filler.size() * sizeof(ColoredVertex));
  RingCursor plainRing{PlainRingBytes}, coloredRing{ColoredRingBytes}, indexRing{IndexRingBytes};
  for (UINT frame = 0; frame < RingFrames; ++frame) {
    f.stage = "ring-frame";
    const DWORD background = 0xff000000u | (0x08u * frame) << 16 | 0x0810u;
    Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, background, 1, 0), "ring clear");
    f.fill(background);
    Check(d.BeginScene(), "ring scene");
    for (UINT draw = 0; draw < RingDraws; ++draw) {
      const bool textured = !(draw & 1);
      DWORD flags = 0; UINT base = 0;
      for (UINT q = 0; q < RingQuads; ++q) {
        const Rect r = cell(draw * RingQuads + q);
        const float x0 = clipX(r.x0), x1 = clipX(r.x1), y0 = clipY(r.y0), y1 = clipY(r.y1);
        if (textured) {
          const UINT texel = ((frame * 4 + draw / 2) * RingQuads + q) % (TextureSize * TextureSize);
          const UINT tx = texel % TextureSize, ty = texel / TextureSize;
          const float u = (float(tx) + .5f) / TextureSize, v = (float(ty) + .5f) / TextureSize;
          const PlainVertex quad[] = {{x0, y0, .5f, 0, 0, 1, u, v}, {x1, y0, .5f, 0, 0, 1, u, v},
            {x0, y1, .5f, 0, 0, 1, u, v}, {x1, y1, .5f, 0, 0, 1, u, v}};
          std::copy(std::begin(quad), std::end(quad), plain.begin() + q * 4);
          f.rect(r, Texel(tx, ty));
        } else {
          const DWORD color = 0xff000000u | (0x20u + frame * 0x18u) << 16 | (0x40u + draw / 2 * 0x30u) << 8 | (0x30u + q * 0x30u);
          const ColoredVertex quad[] = {{x0, y0, .5f, 0, 0, 1, color, 0, 0}, {x1, y0, .5f, 0, 0, 1, color, 0, 0},
            {x0, y1, .5f, 0, 0, 1, color, 0, 0}, {x1, y1, .5f, 0, 0, 1, color, 0, 0}};
          std::copy(std::begin(quad), std::end(quad), colored.begin() + q * 4);
          f.rect(r, color);
        }
      }
      if (textured) {
        const UINT bytes = PlainLockVertices * sizeof(PlainVertex);
        const UINT at = Reserve(f, plainRing, bytes, D3DLOCK_NOSYSLOCK, flags);
        WriteRing(f.ringPlain.get(), at, plain.data(), bytes, flags);
        base = at / sizeof(PlainVertex);
        Check(d.SetStreamSource(0, f.ringPlain.get(), 0, sizeof(PlainVertex)), "plain stream");
        Check(d.SetFVF(PlainFvf), "plain FVF");
        Check(d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "texture color");
        Check(d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), "texture alpha");
      } else {
        const UINT bytes = ColoredLockVertices * sizeof(ColoredVertex);
        const UINT at = Reserve(f, coloredRing, bytes, D3DLOCK_NOSYSLOCK, flags);
        WriteRing(f.ringColored.get(), at, colored.data(), bytes, flags);
        base = at / sizeof(ColoredVertex);
        Check(d.SetStreamSource(0, f.ringColored.get(), 0, sizeof(ColoredVertex)), "colored stream");
        Check(d.SetFVF(ColoredFvf), "colored FVF");
        Check(d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "diffuse color");
        Check(d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "diffuse alpha");
      }
      const UINT indexAt = Reserve(f, indexRing, IndexLock * 2, 0, flags);
      WriteRing(f.ringIndices.get(), indexAt, indices.data(), IndexLock * 2, flags);
      Check(d.DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(base), 0, RingQuads * 4, indexAt / 2, RingQuads * 2), "ring draw");
      ++f.draws;
      // Filler appends move the colored ring towards its own wrap; never drawn.
      if (draw == 3) {
        const UINT bytes = ColoredFillerVertices * sizeof(ColoredVertex);
        const UINT at = Reserve(f, coloredRing, bytes, D3DLOCK_NOSYSLOCK, flags);
        WriteRing(f.ringColored.get(), at, filler.data(), bytes, flags);
      }
    }
    Check(d.EndScene(), "ring scene end");
    f.checkImage();
  }
  // Native keeps one declaration per FVF while the device lives (m_fvfTable).
  Com<IDirect3DVertexDeclaration9> first, again;
  Check(d.SetFVF(PlainFvf), "plain FVF again"); Check(d.GetVertexDeclaration(first.put()), "plain declaration");
  Check(d.SetFVF(ColoredFvf), "colored FVF again"); Check(d.SetFVF(PlainFvf), "plain FVF third");
  Check(d.GetVertexDeclaration(again.put()), "plain declaration again");
  Require(first.get() && first.get() == again.get(), "one declaration per FVF"); ++f.stateChecks;
  Check(d.SetTexture(0, nullptr), "ring texture unbind");
  Check(d.SetStreamSource(0, nullptr, 0, 0), "ring stream unbind");
  Check(d.SetIndices(nullptr), "ring indices unbind");
}

// S1 G2b: DEPTHBIAS. Strips at the depth already written: no bias fails LESS,
// a negative bias passes it, a positive one fails it and passes GREATER. The
// bias is far above D32F precision at 0.5 (D16 maps to D32F in both renderers).
void ExerciseDepthBias(Fixture& f) {
  if (!f.options.depthBias) return;
  auto& d = *f.device.get();
  const UINT w = f.w, h = f.h, sw = w / 4;
  f.stage = "depth-bias";
  const DWORD base = 0xffc04020u, strips[] = {0xff10f010u, 0xff20e0c0u, 0xffe02090u, 0xff4060f0u};
  const float biases[] = {0.f, -1e-4f, 1e-4f, 1e-4f};
  const DWORD funcs[] = {D3DCMP_LESS, D3DCMP_LESS, D3DCMP_LESS, D3DCMP_GREATER};
  std::vector<Screen> vertices;
  const auto quad = [&](const Rect& r, DWORD c) {
    vertices.insert(vertices.end(), {{float(r.x0), float(r.y0), .5f, 1, c}, {float(r.x1), float(r.y0), .5f, 1, c},
      {float(r.x0), float(r.y1), .5f, 1, c}, {float(r.x1), float(r.y1), .5f, 1, c}});
  };
  quad({0, 0, w, h}, base);
  for (UINT i = 0; i < 4; ++i) quad({i * sw, 0, (i + 1) * sw, h}, strips[i]);
  void* data = nullptr;
  Check(d.CreateVertexBuffer(UINT(vertices.size() * sizeof(Screen)), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
    D3DPOOL_DEFAULT, f.biasQuads.put(), nullptr), "bias VB");
  Check(f.biasQuads->Lock(0, 0, &data, 0), "bias VB lock");
  std::memcpy(data, vertices.data(), vertices.size() * sizeof(Screen)); Check(f.biasQuads->Unlock(), "bias VB upload");
  Check(d.SetStreamSource(0, f.biasQuads.get(), 0, sizeof(Screen)), "bias stream");
  Check(d.SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "bias FVF");
  Check(d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "bias color");
  Check(d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "bias alpha");
  Check(d.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "bias depth test");
  Check(d.SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "bias depth write");
  Check(d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "bias base compare");
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1, 0), "bias clear");
  Check(d.BeginScene(), "bias scene");
  Check(d.DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2), "bias base"); ++f.draws;
  Check(d.SetRenderState(D3DRS_ZWRITEENABLE, FALSE), "bias strips keep depth");
  DWORD last = 0;
  for (UINT i = 0; i < 4; ++i) {
    std::memcpy(&last, &biases[i], 4);
    Check(d.SetRenderState(D3DRS_ZFUNC, funcs[i]), "bias compare");
    Check(d.SetRenderState(D3DRS_DEPTHBIAS, last), "DEPTHBIAS");
    Check(d.DrawPrimitive(D3DPT_TRIANGLESTRIP, (i + 1) * 4, 2), "bias strip"); ++f.draws;
  }
  DWORD stored = 0; Check(d.GetRenderState(D3DRS_DEPTHBIAS, &stored), "DEPTHBIAS readback");
  Require(stored == last, "DEPTHBIAS stored"); ++f.stateChecks;
  Check(d.EndScene(), "bias scene end");
  f.fill(base);
  f.rect({sw, 0, 2 * sw, h}, strips[1]); f.rect({3 * sw, 0, 4 * sw, h}, strips[3]);
  f.checkImage();
  Check(d.SetRenderState(D3DRS_DEPTHBIAS, 0), "DEPTHBIAS restore");
  Check(d.SetRenderState(D3DRS_ZENABLE, FALSE), "depth restore");
  Check(d.SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "depth write restore");
  Check(d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "compare restore");
  Check(d.SetStreamSource(0, nullptr, 0, 0), "bias stream unbind");
}

void Present(Fixture& f, UINT64 frameBase = 0) {
  if (!f.options.present) return;
  f.stage = "presented-frames";
  auto& d = *f.device.get();
  std::vector<uint8_t> presented(size_t(f.w) * f.h * 4);
  UINT64 frame = 0;
  Require(f.readPresented(f.device.get(), presented.data(), UINT(presented.size()), &frame) == D3DERR_INVALIDCALL,
    "presented image before the first Present at this extent");
  for (UINT i = 0; i < PresentFrames; ++i) {
    const DWORD background = 0xff000000u | (0x10u + i * 0x30u) << 16 | 0x2040u;
    Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, background, 1, 0), "frame clear");
    Check(d.BeginScene(), "frame scene"); f.drawQuad(4 + i); Check(d.EndScene(), "frame scene end");
    f.fill(background); f.rect(f.quads[4 + i], f.quadColors[4 + i]); f.checkImage();
    Check(d.Present(nullptr, nullptr, nullptr, nullptr), "Present");
    Check(f.readPresented(f.device.get(), presented.data(), UINT(presented.size()), &frame), "presented image");
    Require(frame == frameBase + i + 1, "presented frame number");
    unsigned failures = 0;
    // The 1:1 blit into the BGRA8 swapchain image keeps every channel; an
    // X8R8G8B8 source is sampled with alpha one.
    for (size_t p = 0; p < presented.size(); p += 4) {
      if (std::memcmp(presented.data() + p, f.target.data() + p, f.opaque ? 3 : 4) || (f.opaque && presented[p + 3] != 0xff)) {
        if (failures < 4) std::fprintf(stderr, "presented mismatch frame=%u pixel=%zu\n", i + 1, p / 4);
        ++failures;
      }
      f.presentedChecks += 4;
    }
    Require(!failures, "presented image differs from the target it was blitted from");
    f.presentedHash = Hash(presented.data(), presented.size(), f.presentedHash);
    ++f.presentedFrames;
  }
}

// D3D9 Reset to the second extent, in native order: refused parameters keep
// the state; live DEFAULT-pool resources refuse the Reset after the state was
// cleared (NotReset); once they are released the Reset succeeds.
void Reset(Fixture& f) {
  if (!f.options.resetWidth) return;
  auto& d = *f.device.get();
  auto& factory = *f.factory.get();
  f.stage = "reset-factory-queries";
  // The game's swap chain formats are offered by both renderers.
  Check(factory.CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.options.format, TRUE), "back buffer type");
  Check(factory.CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, f.options.format), "target format");
  Check(factory.CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, f.options.depth), "depth format");
  Check(factory.CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.options.format, f.options.depth), "depth match");
  f.resetChecks += 4;

  f.stage = "resized-window";
  const UINT nextWidth = f.options.resetWidth, nextHeight = f.options.resetHeight;
  f.window.resize(nextWidth, nextHeight);
  // The old target still renders at its own extent until the Reset.
  constexpr DWORD resized = 0xff5a3c1e;
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET, resized, 1, 0), "clear after resize");
  f.fill(resized); f.checkImage();
  if (f.options.present) {
    // The host drops this frame (native would stretch it): the last one stays.
    Check(d.Present(nullptr, nullptr, nullptr, nullptr), "Present after resize");
    std::vector<uint8_t> presented(size_t(f.w) * f.h * 4); UINT64 frame = 0;
    Check(f.readPresented(f.device.get(), presented.data(), UINT(presented.size()), &frame), "presented image after resize");
    Require(frame == PresentFrames, "a skipped Present published a frame"); ++f.resetChecks;
  }

  f.stage = "reset-refused-parameters";
  DWORD value = 0, keptFvf = 0; UINT keptOffset = 0, keptStride = 0;
  {Com<IDirect3DVertexBuffer9> bound; Check(d.GetStreamSource(0, bound.put(), &keptOffset, &keptStride), "stream before Reset");}
  Check(d.GetFVF(&keptFvf), "FVF before Reset");
  Check(d.SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER), "marker state");
  auto next = f.params;
  next.BackBufferWidth = f.options.resetZero ? 0 : nextWidth; next.BackBufferHeight = f.options.resetZero ? 0 : nextHeight;
  auto refused = next; refused.SwapEffect = D3DSWAPEFFECT(0);
  Require(d.Reset(&refused) == D3DERR_INVALIDCALL, "Reset with a zero swap effect");
  Require(d.TestCooperativeLevel() == D3D_OK, "refused parameters lost the device");
  Check(d.GetRenderState(D3DRS_ZFUNC, &value), "marker read");
  Require(value == D3DCMP_GREATER, "refused parameters reset the state"); f.resetChecks += 3;

  f.stage = "reset-live-default-pool";
  // Three DEFAULT-pool vertex buffers and the application's back buffer reference.
  auto attempt = next;
  Require(d.Reset(&attempt) == D3DERR_INVALIDCALL, "Reset with live DEFAULT-pool resources");
  Require(d.TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "refused Reset is not NotReset");
  Require(d.Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST, "Present while NotReset");
  Check(d.GetRenderState(D3DRS_ZFUNC, &value), "cleared state read");
  Require(value == D3DCMP_LESSEQUAL, "the refused Reset kept the old state"); f.resetChecks += 4;

  f.stage = "reset";
  f.screenQuads.reset(); f.clipQuad.reset(); f.texturedQuad.reset(); f.back.reset(); f.readback.reset();
  attempt = next;
  Check(d.Reset(&attempt), "Reset");
  Require(d.TestCooperativeLevel() == D3D_OK, "Reset device not restored");
  Require(attempt.BackBufferWidth == nextWidth && attempt.BackBufferHeight == nextHeight, "chosen reset extent");
  f.w = nextWidth; f.h = nextHeight; f.resizeImages();
  Check(d.GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, f.back.put()), "GetBackBuffer after Reset");
  D3DSURFACE_DESC desc{}; Check(f.back->GetDesc(&desc), "reset back buffer desc");
  Require(desc.Width == f.w && desc.Height == f.h && desc.Format == f.options.format, "reset back buffer description");
  D3DVIEWPORT9 viewport{}; Check(d.GetViewport(&viewport), "reset viewport");
  Require(!viewport.X && !viewport.Y && viewport.Width == f.w && viewport.Height == f.h, "reset viewport covers the target");
  RECT scissor{}; Check(d.GetScissorRect(&scissor), "reset scissor");
  Require(!scissor.left && !scissor.top && scissor.right == LONG(f.w) && scissor.bottom == LONG(f.h), "reset scissor covers the target");
  f.resetChecks += 6;
  // DXVK's ResetState: auto depth on, lighting on, CCW culling, no buffer or
  // texture bound. A null stream keeps its offset and stride, the FVF survives.
  const std::pair<D3DRENDERSTATETYPE, DWORD> defaults[] = {{D3DRS_ZENABLE, D3DZB_TRUE}, {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
    {D3DRS_LIGHTING, TRUE}, {D3DRS_CULLMODE, D3DCULL_CCW}, {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE}};
  for (const auto& [state, expected] : defaults) {
    Check(d.GetRenderState(state, &value), "reset render state"); Require(value == expected, "render state not reset"); ++f.resetChecks;
  }
  Check(d.GetTextureStageState(0, D3DTSS_COLOROP, &value), "reset stage state");
  Require(value == D3DTOP_MODULATE, "stage state not reset"); ++f.resetChecks;
  Com<IDirect3DVertexBuffer9> stream; UINT offset = 1, stride = 1;
  Check(d.GetStreamSource(0, stream.put(), &offset, &stride), "reset stream");
  Require(!stream.get() && offset == keptOffset && stride == keptStride && keptStride, "stream source after Reset");
  Com<IDirect3DBaseTexture9> bound; Check(d.GetTexture(0, bound.put()), "reset texture");
  Require(!bound.get(), "texture binding not reset");
  Check(d.GetFVF(&value), "reset FVF"); Require(value == keptFvf && keptFvf, "FVF after Reset"); f.resetChecks += 3;
}

// The window's own resize (S1 G7), no Reset: WarVK's window hook answers the
// WM_SIZE with a swap chain reset to the new client extent. The back buffer
// and auto depth are new and RT0's viewport and scissor cover them; every
// other state and binding is kept, and so are the DEFAULT-pool buffers.
void ResizeWindow(Fixture& f) {
  if (!f.options.resizeWidth) return;
  auto& d = *f.device.get();
  const UINT oldWidth = f.w, oldHeight = f.h, nextWidth = f.options.resizeWidth, nextHeight = f.options.resizeHeight;
  f.stage = "resize-kept-state";
  // State the resize keeps, and a viewport and scissor rectangle it replaces.
  constexpr DWORD clipFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
  Check(d.SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER), "marker state");
  Check(d.SetTexture(0, f.texture.get()), "kept texture");
  Check(d.SetStreamSource(0, f.clipQuad.get(), 0, sizeof(Clip)), "kept stream");
  Check(d.SetFVF(clipFvf), "kept FVF");
  const D3DVIEWPORT9 offset{oldWidth / 4, oldHeight / 4, oldWidth / 2, oldHeight / 2, 0, 1};
  Check(d.SetViewport(&offset), "replaced viewport");
  const RECT scissor{1, 2, LONG(oldWidth / 2), LONG(oldHeight / 2)};
  Check(d.SetScissorRect(&scissor), "replaced scissor");
  // The application keeps its old back buffer reference across the resize.
  Com<IDirect3DSurface9> old; *old.put() = f.back.detach(); f.readback.reset();
  const unsigned appSizeMessages = AppSizeMessages.load();

  f.stage = "resize-window";
  f.window.resize(nextWidth, nextHeight);
  // Whatever the size messages queued on the window thread is handled by now.
  DWORD_PTR none = 0;
  Require(SendMessageTimeoutW(f.window.hwnd.load(), WM_NULL, 0, 0, SMTO_BLOCK, 5000, &none) != 0, "window thread round trip");

  f.stage = "resize-checks";
  Require(AppSizeMessages.load() == appSizeMessages, "WM_SIZE reached the application"); ++f.resizeChecks;
  Check(d.TestCooperativeLevel(), "device after the resize"); ++f.resizeChecks;
  Check(d.GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, f.back.put()), "GetBackBuffer after the resize");
  D3DSURFACE_DESC desc{}; Check(f.back->GetDesc(&desc), "resized back buffer desc");
  Require(desc.Width == nextWidth && desc.Height == nextHeight && desc.Format == f.options.format, "resized back buffer description");
  Require(f.back.get() != old.get(), "the resize made a new back buffer"); f.resizeChecks += 2;
  Check(old->GetDesc(&desc), "old back buffer desc");
  Require(desc.Width == oldWidth && desc.Height == oldHeight, "the old back buffer keeps its extent"); ++f.resizeChecks;
  old.reset();
  D3DVIEWPORT9 viewport{}; Check(d.GetViewport(&viewport), "resized viewport");
  Require(!viewport.X && !viewport.Y && viewport.Width == nextWidth && viewport.Height == nextHeight
    && viewport.MinZ == 0 && viewport.MaxZ == 1, "the viewport covers the new target");
  RECT rect{}; Check(d.GetScissorRect(&rect), "resized scissor");
  Require(!rect.left && !rect.top && rect.right == LONG(nextWidth) && rect.bottom == LONG(nextHeight), "the scissor covers the new target");
  f.resizeChecks += 2;
  DWORD value = 0;
  Check(d.GetRenderState(D3DRS_ZFUNC, &value), "kept render state"); Require(value == D3DCMP_GREATER, "render state kept");
  Com<IDirect3DVertexBuffer9> stream; UINT streamOffset = 1, stride = 0;
  Check(d.GetStreamSource(0, stream.put(), &streamOffset, &stride), "kept stream source");
  Require(stream.get() == f.clipQuad.get() && !streamOffset && stride == sizeof(Clip), "stream source kept");
  Check(d.GetFVF(&value), "kept FVF read"); Require(value == clipFvf, "FVF kept");
  Com<IDirect3DBaseTexture9> bound; Check(d.GetTexture(0, bound.put()), "kept texture read");
  Require(bound.get() == f.texture.get(), "texture binding kept");
  Check(d.GetSamplerState(0, D3DSAMP_MINFILTER, &value), "kept sampler state"); Require(value == D3DTEXF_POINT, "sampler state kept");
  Check(d.GetTextureStageState(0, D3DTSS_COLORARG1, &value), "kept stage state"); Require(value == D3DTA_DIFFUSE, "stage state kept");
  f.resizeChecks += 6;

  f.stage = "resize-kept-buffer";
  Check(d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "marker restore");
  Check(d.SetTexture(0, nullptr), "unbind texture");
  f.w = nextWidth; f.h = nextHeight; f.resizeImages();
  // A fresh target and depth: cleared, then the clip quad made before the
  // resize fills the new full viewport.
  constexpr DWORD base = 0xff306090;
  Check(d.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, base, 1, 0), "clear after the resize");
  f.fill(base); f.checkImage();
  Check(d.BeginScene(), "scene"); Check(d.DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2), "kept clip quad"); ++f.draws;
  Check(d.EndScene(), "scene end");
  f.fill(0xffe0d000); f.checkImage();
}

void Teardown(Fixture& f) {
  f.stage = "teardown";
  f.screenQuads.reset(); f.clipQuad.reset(); f.texturedQuad.reset(); f.texture.reset(); f.readback.reset(); f.back.reset();
  f.ringPlain.reset(); f.ringColored.reset(); f.ringIndices.reset(); f.biasQuads.reset();
  Require(f.device.detach()->Release() == 0, "device final reference");
  Require(f.factory.detach()->Release() == 0, "factory final reference");
  Require(FreeLibrary(f.renderer) != FALSE, "renderer release"); f.renderer = nullptr;
  f.window.stop();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  Fixture* f = nullptr;
  try {
    f = new Fixture;
    f->options = ParseOptions(argc, argv);
    CreateDevice(*f); Prepare(*f);
    if (f->options.ring || f->options.depthBias) { ExerciseRing(*f); ExerciseDepthBias(*f); }
    else { Exercise(*f); ExerciseTextureAndAlpha(*f); Present(*f); }
    if (f->options.secondExtent()) {
      Reset(*f); ResizeWindow(*f); Prepare(*f); Exercise(*f); ExerciseTextureAndAlpha(*f); Present(*f, PresentFrames);
    }
    const auto images = f->images; const auto pixelChecks = f->pixelChecks;
    // While the renderer is still loaded: the original one brings Vulkan in.
    const bool vulkanAbsent = GetModuleHandleW(L"vulkan-1.dll") == nullptr;
    Teardown(*f);
    // width/height: the creation extent; resetWidth/resetHeight 0 without --reset,
    // resizeWidth/resizeHeight 0 without --resize.
    std::printf("{\"schema\":\"warvk-d3d9-extent-fixture/v1\",\"ok\":true,\"width\":%u,\"height\":%u,\"zeroExtent\":%s,"
      "\"reference\":%s,\"present\":%s,\"images\":%u,\"draws\":%u,\"stateChecks\":%u,\"pixelChecks\":%llu,\"pixelHash\":\"%016llx\","
      "\"presentedFrames\":%u,\"presentedChecks\":%llu,\"presentedHash\":\"%016llx\",\"x86VulkanAbsent\":%s,"
      "\"format\":\"%s\",\"depth\":\"%s\",\"interval\":\"%s\",\"resetWidth\":%u,\"resetHeight\":%u,\"resetZero\":%s,"
      "\"resetChecks\":%u,\"resizeWidth\":%u,\"resizeHeight\":%u,\"resizeChecks\":%u,\"ring\":%s,\"ringLocks\":%u,\"ringDiscards\":%u,\"ringWraps\":%u,\"ringBytes\":%llu,"
      "\"ringCarriedBytes\":%llu,\"depthBias\":%s,\"normalTeardown\":true,\"gameTouched\":false}\n",
      f->options.width, f->options.height, JsonBool(f->options.zeroExtent), JsonBool(f->options.reference), JsonBool(f->options.present),
      images, f->draws, f->stateChecks, static_cast<unsigned long long>(pixelChecks), static_cast<unsigned long long>(f->pixelHash),
      f->presentedFrames, static_cast<unsigned long long>(f->presentedChecks), static_cast<unsigned long long>(f->presentedHash),
      JsonBool(vulkanAbsent), FormatName(f->options.format), FormatName(f->options.depth), IntervalName(f->options.interval),
      f->options.resetWidth, f->options.resetHeight, JsonBool(f->options.resetZero), f->resetChecks,
      f->options.resizeWidth, f->options.resizeHeight, f->resizeChecks,
      JsonBool(f->options.ring), f->ringLocks, f->ringDiscards, f->ringWraps,
      static_cast<unsigned long long>(f->ringBytes), static_cast<unsigned long long>(f->ringCarriedBytes),
      JsonBool(f->options.depthBias));
    delete f; return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "extent fixture failed stage=%s reason=%s\n", f ? f->stage : "allocate", e.what());
  }
  ExitProcess(2);
}
