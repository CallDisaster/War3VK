// R1/Q1: actual x86 COM and raw C2 peers, isolated offscreen DXVK owner only.
// The two modes run in separate processes. Rendering expectations are derived
// from this scene, not from backend counters or from an image emitted by it.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../tools/render_host/d3d9_connection.h"
#include "../tools/render_host/d3d9_draw_wire.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static_assert(sizeof(void*) == 4, "Actual x86 peer required");
namespace d9 = warvk::host::d9;
namespace proxy = warvk::host::d3d9proxy;
namespace {
constexpr UINT Size = 64, Rounds = 96, QueryRepeats = 256;
constexpr DWORD ClearColor = 0xff13273b;
constexpr UINT PixelsPerOracle = 3328;
constexpr std::array<d9::Type, 6> Types{{d9::Type::VertexBuffer, d9::Type::IndexBuffer,
  d9::Type::Texture, d9::Type::VertexShader, d9::Type::PixelShader, d9::Type::Declaration}};
void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void Check(HRESULT hr, const char* reason) {
  if (hr == D3D_OK) return;
  char text[240]; std::snprintf(text, sizeof(text), "%s: HRESULT 0x%08lx", reason, DWORD(hr));
  throw std::runtime_error(text);
}
const char* Bool(bool value) { return value ? "true" : "false"; }
std::string Json(const char* value) {
  std::string result = "\"";
  for (size_t i = 0; value && value[i] && i < 1200; ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c < 32) { char s[7]; std::snprintf(s, sizeof(s), "\\u%04x", unsigned(c)); result += s; }
    else result += char(c);
  }
  return result + '"';
}
template<class T> struct Com {
  T* p = nullptr;
  Com() = default; Com(const Com&) = delete; Com& operator=(const Com&) = delete;
  ~Com() { if (p) p->Release(); }
  T* operator->() const { return p; }
  T** out() { Require(!p, "COM output must start empty"); return &p; }
  ULONG close() { T* old = p; p = nullptr; return old ? old->Release() : 0; }
};
struct Options { std::wstring mode, dll, desktop, input; };
Options Parse(int argc, wchar_t** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::wstring key = argv[i]; Require(i + 1 < argc, "missing option value");
    auto* target = key == L"--mode" ? &o.mode : key == L"--dll" ? &o.dll
      : key == L"--expect-desktop" ? &o.desktop : key == L"--input-desktop" ? &o.input : nullptr;
    Require(target && target->empty(), "unknown/duplicate option"); *target = argv[++i];
  }
  Require((o.mode == L"com" || o.mode == L"raw") && !o.desktop.empty() && !o.input.empty(), "mode/desktop required");
  Require(o.dll.size() > 3 && o.dll[1] == L':' && (o.dll[2] == L'/' || o.dll[2] == L'\\'), "absolute DLL required");
  return o;
}
void Desktop(const Options& o) {
  Require(d9::RequireIsolatedDesktop() == L"WinSta0\\" + o.desktop && d9::InputDesktop() == o.input,
    "actual non-input desktop/input identity changed");
  Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"vulkan-1.dll"), "x86 game/Vulkan module prohibited");
}
struct Vertex { float position[4], uv[2], color[4]; };
static_assert(sizeof(Vertex) == 40);
const std::array<Vertex, 4> Vertices{{
  {{-.75f, .75f, .5f, 1}, {0, 0}, {1, 1, 1, 1}},
  {{ .75f, .75f, .5f, 1}, {1, 0}, {1, 1, 1, 1}},
  {{ .75f,-.75f, .5f, 1}, {1, 1}, {1, 1, 1, 1}},
  {{-.75f,-.75f, .5f, 1}, {0, 1}, {1, 1, 1, 1}}
}};
const std::array<uint16_t, 6> Indices{{0, 1, 2, 0, 2, 3}};
const D3DVERTEXELEMENT9 Elements[] = {
  {0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
  {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
  {0, 24, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()
};
constexpr char VertexSource[] =
  "struct In { float4 p:POSITION0; float2 uv:TEXCOORD0; float4 c:COLOR0; };"
  "struct Out { float4 p:POSITION0; float2 uv:TEXCOORD0; float4 c:COLOR0; };"
  "Out main(In i){Out o;o.p=i.p;o.uv=i.uv;o.c=i.c;return o;}";
constexpr char PixelSource[] =
  "sampler2D t:register(s0);float4 main(float2 uv:TEXCOORD0,float4 c:COLOR0):COLOR0{return tex2D(t,uv)*c;}";
struct Shaders {
  std::vector<DWORD> vs, ps;
  Shaders() {
    HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Require(module != nullptr, "system D3D compiler unavailable");
    try {
      const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(module, "D3DCompile"));
      Require(compile != nullptr, "D3DCompile export unavailable");
      auto one = [&](const char* text, const char* profile, std::vector<DWORD>& target) {
        Com<ID3DBlob> bytes, errors;
        Check(compile(text, std::strlen(text), "resource-lifetime-original", nullptr, nullptr, "main", profile,
          D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, bytes.out(), errors.out()), "compile original SM3");
        Require(bytes.p && bytes->GetBufferSize() > 16 && bytes->GetBufferSize() <= 16384
          && bytes->GetBufferSize() % 4 == 0, "bounded original shader bytes");
        const auto* begin = static_cast<const DWORD*>(bytes->GetBufferPointer());
        target.assign(begin, begin + bytes->GetBufferSize() / 4);
      };
      one(VertexSource, "vs_3_0", vs); one(PixelSource, "ps_3_0", ps);
    } catch (...) { FreeLibrary(module); throw; }
    Require(FreeLibrary(module) != FALSE, "compiler unload");
  }
};
struct Evidence {
  uint32_t rounds = 0, draws = 0, readbacks = 0, pixels = 0, failures = 0, maxError = 0;
  uint32_t resourcesCreated = 0, resourcesReleased = 0, rejections = 0, pixelRecoveries = 0;
  uint32_t privateBoundDraws = 0, finalPrivateBoundResources = 0;
  uint32_t queryIssues = 0, queryPolls = 0, queryRecoveries = 0, sameSerialChecks = 0;
  uint64_t firstSerial = 0, lastSerial = 0;
  uint32_t typedSlotsFilled = 0, sameGenerationRetries = 0;
  bool cleanup = false;
};
void Pixels(Evidence& e, const void* data, UINT pitch, DWORD color) {
  Require(data && pitch >= Size * 4, "readback storage/pitch");
  uint32_t count = 0, failures = 0;
  for (UINT y = 0; y < Size; ++y) for (UINT x = 0; x < Size; ++x) {
    const bool clear = x <= 5 || x >= 58 || y <= 5 || y >= 58;
    const bool inner = x >= 10 && x <= 53 && y >= 10 && y <= 53;
    if (!clear && !inner) continue; // Independently fixed raster-edge guard strips.
    const DWORD expected = clear ? ClearColor : color;
    const auto* pixel = static_cast<const uint8_t*>(data) + size_t(y) * pitch + x * 4;
    bool bad = false;
    for (unsigned c = 0; c < 4; ++c) {
      const int delta = int(pixel[c]) - int((expected >> (c * 8)) & 255);
      const uint32_t error = uint32_t(delta < 0 ? -delta : delta);
      e.maxError = std::max(e.maxError, error); bad |= error > (c == 3 ? 0u : 1u);
    }
    if (bad && !failures) std::fprintf(stderr, "lifetime pixel mismatch x=%u y=%u expected=%08lx actual=%02x%02x%02x%02x\n",
      x, y, expected, pixel[3], pixel[2], pixel[1], pixel[0]);
    ++count; failures += bad;
  }
  Require(count == PixelsPerOracle, "independent pixel coverage");
  e.pixels += count; e.failures += failures; ++e.readbacks;
  Require(!failures, "independent clear/textured-quad pixels differ");
}
void Print(const Options& o, const Evidence& e) {
  std::printf("{\"schema\":\"warvk-d3d9-resource-lifetime/v1\",\"ok\":true,\"bits\":32,\"mode\":\"%s\","
    "\"publicD3D9Api\":%s,\"rawProtocolPeer\":%s,\"rounds\":%u,\"draws\":%u,\"readbacks\":%u,"
    "\"pixelsChecked\":%u,\"pixelFailures\":%u,\"maxChannelError\":%u,\"resourcesCreated\":%u,\"resourcesReleased\":%u,"
    "\"resourceReleaseScope\":\"%s\",\"privateBoundDraws\":%u,\"finalPrivateBoundResources\":%u,"
    "\"rejectionChecks\":%u,\"pixelRecoveries\":%u,\"queryIssues\":%u,\"queryPolls\":%u,\"queryRecoveries\":%u,"
    "\"sameSerialChecks\":%u,\"firstSerial\":%llu,\"lastSerial\":%llu,\"typedSlotsFilled\":%u,\"sameGenerationRetries\":%u,"
    "\"fixtureCleanupComplete\":%s,\"parentVulkanAbsent\":true,\"desktop\":%s,\"inputBefore\":%s,\"inputAfter\":%s,"
    "\"globalInputUsed\":false,\"desktopSwitched\":false,\"gameTouched\":false,\"productIntegrated\":false,"
    "\"memorySavingsProven\":false,\"windowPixelsVerified\":false,\"playerVisualAccepted\":false}\n",
    o.mode == L"com" ? "com" : "raw", Bool(o.mode == L"com"), Bool(o.mode == L"raw"),
    e.rounds, e.draws, e.readbacks, e.pixels, e.failures, e.maxError, e.resourcesCreated, e.resourcesReleased,
    o.mode == L"com" ? "public-COM-references-zero-not-host-destruction" : "confirmed-remote-destroy-replies",
    e.privateBoundDraws, e.finalPrivateBoundResources,
    e.rejections, e.pixelRecoveries, e.queryIssues, e.queryPolls, e.queryRecoveries, e.sameSerialChecks,
    static_cast<unsigned long long>(e.firstSerial), static_cast<unsigned long long>(e.lastSerial),
    e.typedSlotsFilled, e.sameGenerationRetries, Bool(e.cleanup), d9::NameJson(d9::CurrentDesktop()).c_str(),
    d9::NameJson(o.input).c_str(), d9::NameJson(d9::InputDesktop()).c_str());
}
struct ComOwner {
  HMODULE module = nullptr; HWND window = nullptr;
  Com<IDirect3D9> factory; Com<IDirect3DDevice9> device;
  Com<IDirect3DSurface9> target, readback; Com<IDirect3DQuery9> query;
  ~ComOwner() {
    query.close(); readback.close(); target.close(); device.close(); factory.close();
    if (module) FreeLibrary(module); if (window) DestroyWindow(window);
  }
  void close() {
    Require(query.close() == 0 && readback.close() == 0 && target.close() == 0, "surface/query leaked references");
    Require(device.close() == 0 && factory.close() == 0, "device/factory final references");
    Require(FreeLibrary(module) != FALSE, "proxy module unload"); module = nullptr;
    Require(DestroyWindow(window) != FALSE, "fixture window close"); window = nullptr;
  }
};
void ComMode(const Options& o, const Shaders& shaders, Evidence& e) {
  const auto progressStart = GetTickCount64();
  auto progress = [&](const char* phase) {
    std::fprintf(stderr, "R1Q1 COM progress phase=%s rounds=%u queryIssues=%u elapsedMs=%llu\n",
      phase, e.rounds, e.queryIssues, static_cast<unsigned long long>(GetTickCount64()-progressStart));
    std::fflush(stderr);
  };
  progress("begin");
  ComOwner owner;
  owner.window = CreateWindowExW(0, L"STATIC", L"R1Q1 isolated offscreen fixture", WS_POPUP,
    0, 0, Size, Size, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Require(owner.window != nullptr, "own offscreen HWND creation");
  owner.module = LoadLibraryW(o.dll.c_str()); Require(owner.module != nullptr, "explicit proxy DLL load");
  const auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(owner.module, "Direct3DCreate9"));
  Require(create != nullptr, "Direct3DCreate9 unavailable"); *owner.factory.out() = create(D3D_SDK_VERSION);
  Require(owner.factory.p != nullptr, "factory creation");
  D3DPRESENT_PARAMETERS p{}; p.BackBufferWidth = Size; p.BackBufferHeight = Size;
  p.BackBufferFormat = D3DFMT_A8R8G8B8; p.BackBufferCount = 1; p.SwapEffect = D3DSWAPEFFECT_DISCARD;
  p.hDeviceWindow = owner.window; p.Windowed = TRUE; p.EnableAutoDepthStencil = TRUE;
  p.AutoDepthStencilFormat = D3DFMT_D32F_LOCKABLE; p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  Check(owner.factory->CreateDevice(0, D3DDEVTYPE_HAL, owner.window,
    D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &p, owner.device.out()), "CreateDevice");
  progress("device-created");
  auto* d = owner.device.p;
  Check(d->GetRenderTarget(0, owner.target.out()), "GetRenderTarget");
  Check(d->CreateOffscreenPlainSurface(Size, Size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
    owner.readback.out(), nullptr), "CreateOffscreenPlainSurface");
  Check(d->CreateQuery(D3DQUERYTYPE_EVENT, owner.query.out()), "CreateQuery");
  const D3DVIEWPORT9 viewport{0, 0, Size, Size, 0, 1}; Check(d->SetViewport(&viewport), "SetViewport");
  Check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "cull none");
  Check(d->SetRenderState(D3DRS_ZENABLE, TRUE), "depth enable");
  Check(d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "depth write");
  Check(d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "depth compare");
  Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), "blend disabled");
  Check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15), "color mask");
  for (UINT round = 0; round < Rounds; ++round) {
    Com<IDirect3DVertexBuffer9> vb; Com<IDirect3DIndexBuffer9> ib; Com<IDirect3DTexture9> texture;
    Com<IDirect3DVertexShader9> vs; Com<IDirect3DPixelShader9> ps; Com<IDirect3DVertexDeclaration9> declaration;
    Check(d->CreateVertexBuffer(sizeof(Vertices), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, vb.out(), nullptr), "create VB");
    Check(d->CreateIndexBuffer(sizeof(Indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, ib.out(), nullptr), "create IB");
    Check(d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.out(), nullptr), "create texture");
    Check(d->CreateVertexShader(shaders.vs.data(), vs.out()), "create VS");
    Check(d->CreatePixelShader(shaders.ps.data(), ps.out()), "create PS");
    Check(d->CreateVertexDeclaration(Elements, declaration.out()), "create declaration"); e.resourcesCreated += 6;
    void* mapped = nullptr;
    Check(vb->Lock(0, sizeof(Vertices), &mapped, 0), "lock VB"); std::memcpy(mapped, Vertices.data(), sizeof(Vertices)); Check(vb->Unlock(), "upload VB");
    Check(ib->Lock(0, sizeof(Indices), &mapped, 0), "lock IB"); std::memcpy(mapped, Indices.data(), sizeof(Indices)); Check(ib->Unlock(), "upload IB");
    const DWORD color = 0xff2060a0u + round;
    D3DLOCKED_RECT locked{}; Check(texture->LockRect(0, &locked, nullptr, 0), "lock texture");
    Require(locked.pBits && locked.Pitch >= 4, "texture storage"); std::memcpy(locked.pBits, &color, 4);
    Check(texture->UnlockRect(0), "upload texture");
    Check(d->SetStreamSource(0, vb.p, 0, sizeof(Vertex)), "bind VB"); Check(d->SetIndices(ib.p), "bind IB");
    Check(d->SetTexture(0, texture.p), "bind texture"); Check(d->SetVertexShader(vs.p), "bind VS");
    Check(d->SetPixelShader(ps.p), "bind PS"); Check(d->SetVertexDeclaration(declaration.p), "bind declaration");
    // Release all public resource references BEFORE using the device's bound
    // state. Never dereference the released resource pointers. A public zero
    // is not remote destruction: the private bindings must keep data alive.
    Require(vb.close() == 0 && ib.close() == 0 && texture.close() == 0 && vs.close() == 0
      && ps.close() == 0 && declaration.close() == 0, "public resource references did not reach zero");
    e.resourcesReleased += 6;
    Check(d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, ClearColor, 1, 0), "clear");
    Check(d->BeginScene(), "BeginScene"); Check(d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2), "draw");
    Check(d->EndScene(), "EndScene"); ++e.draws; ++e.privateBoundDraws;
    Check(d->GetRenderTargetData(owner.target.p, owner.readback.p), "readback");
    Check(owner.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "readback lock"); Pixels(e, locked.pBits, UINT(locked.Pitch), color);
    Check(owner.readback->UnlockRect(), "readback unlock");
    if (round + 1 < Rounds) {
      Check(d->SetStreamSource(0, nullptr, 0, 0), "unbind VB"); Check(d->SetIndices(nullptr), "unbind IB");
      Check(d->SetTexture(0, nullptr), "unbind texture"); Check(d->SetVertexShader(nullptr), "unbind VS");
      Check(d->SetPixelShader(nullptr), "unbind PS"); Check(d->SetVertexDeclaration(nullptr), "unbind declaration");
    } else {
      // The final six private bindings survive Query reuse until the actual
      // device destructor. Host logical/GPU zero and destruction counts are
      // checked separately; this number is the pre-final-Release population.
      e.finalPrivateBoundResources = 6;
    }
    ++e.rounds;
    if ((e.rounds % 16) == 0) progress("resource-rounds");
  }
  // No GPU-producing calls between these issues. The raw peer additionally
  // checks the returned serial: public COM intentionally exposes no serial.
  for (UINT n = 0; n < QueryRepeats; ++n) {
    Check(owner.query->Issue(D3DISSUE_END), "repeat same query Issue"); ++e.queryIssues;
    struct { DWORD before; BOOL complete; DWORD after; } value{0x18395a7bu, FALSE, 0xc2e40628u};
    Check(owner.query->GetData(&value.complete, sizeof(BOOL), 0), "repeat same query GetData"); ++e.queryPolls;
    Require(value.complete == TRUE && value.before == 0x18395a7bu && value.after == 0xc2e40628u, "query BOOL/guards");
    if ((e.queryIssues % 64) == 0) progress("query-reissues");
  }
  progress("before-final-close");
  owner.close(); Desktop(o); e.cleanup = true;
  progress("closed");
}

struct Raw {
  std::unique_ptr<proxy::Connection> connection = std::make_unique<proxy::Connection>();
  const Shaders& shaders; Evidence& evidence; std::array<d9::ResourceRef, 6> refs{};
  DWORD color = 0xff11bb66u;
  explicit Raw(const Shaders& s, Evidence& e) : shaders(s), evidence(e) {}
  static d9::Header Header(d9::Op op, d9::ResourceRef ref = {}) {
    d9::Header h; h.op = op; h.id = ref.id; h.generation = ref.generation; return h;
  }
  void call(d9::Header h, const void* bytes = nullptr, void* out = nullptr, UINT outBytes = 0) {
    Check(connection->call(h, bytes, out, outBytes), "raw command");
  }
  void destroy(d9::ResourceRef ref, d9::Type type) {
    auto h = Header(d9::Op::Destroy, ref); h.args[0] = uint32_t(type); call(h);
    if (type != d9::Type::EventQuery) ++evidence.resourcesReleased;
  }
  void buffer(d9::ResourceRef ref, UINT bytes, bool index = false) {
    auto h = Header(d9::Op::CreateBuffer, ref); h.args[0] = bytes; h.args[1] = index; call(h); ++evidence.resourcesCreated;
  }
  void createScene(uint32_t generation, DWORD value) {
    color = value; for (UINT i = 0; i < 6; ++i) refs[i] = {i + 1, generation};
    buffer(refs[0], sizeof(Vertices)); buffer(refs[1], sizeof(Indices), true);
    auto h = Header(d9::Op::CreateTexture, refs[2]); h.args[0] = h.args[1] = 1; call(h); ++evidence.resourcesCreated;
    for (UINT i = 0; i < 2; ++i) {
      const auto& words = i ? shaders.ps : shaders.vs; h = Header(d9::Op::CreateShader, refs[3 + i]);
      h.args[0] = i; h.payloadBytes = UINT(words.size() * 4); call(h, words.data()); ++evidence.resourcesCreated;
    }
    h = Header(d9::Op::CreateDeclaration, refs[5]); h.payloadBytes = 24; call(h, Elements); ++evidence.resourcesCreated;
    h = Header(d9::Op::UploadBuffer, refs[0]); h.payloadBytes = sizeof(Vertices); call(h, Vertices.data());
    h = Header(d9::Op::UploadBuffer, refs[1]); h.payloadBytes = sizeof(Indices); call(h, Indices.data());
    h = Header(d9::Op::UploadTexture, refs[2]); h.payloadBytes = 4; call(h, &color);
  }
  d9::DrawCommand draw() const {
    d9::DrawCommand command; command.resources = refs; auto& s = command.state;
    s.stride = sizeof(Vertex); s.viewport = {0, 0, Size, Size, 0, 1}; s.numVertices = 4; s.primitiveCount = 2;
    return command;
  }
  void recover() {
    auto h = Header(d9::Op::Clear); h.args[0] = ClearColor; h.args[1] = d9::FloatBits(1); h.args[2] = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER; call(h);
    const auto packet = d9::EncodeDraw(draw()); h = Header(d9::Op::Draw); h.payloadBytes = UINT(packet.size()); call(h, packet.data()); ++evidence.draws;
    std::array<uint8_t, Size * Size * 4> pixels{}; call(Header(d9::Op::Readback), nullptr, pixels.data(), UINT(pixels.size()));
    Pixels(evidence, pixels.data(), Size * 4, color);
  }
  void rejected(const char* kind, UINT slot, d9::Header h, const void* bytes = nullptr, HRESULT expected = D3DERR_INVALIDCALL,
      bool imageRecovery = true) {
    const auto actual = connection->call(h, bytes);
    Require(actual == expected && connection->healthy(), "raw negative must refuse without losing transport");
    if (imageRecovery) { recover(); ++evidence.pixelRecoveries; }
    ++evidence.rejections;
    std::printf("{\"schema\":\"warvk-d3d9-lifetime-rejection/v1\",\"index\":%u,\"kind\":\"%s\",\"slot\":%u,"
      "\"op\":%u,\"id\":%u,\"generation\":%u,\"expected\":%lu,\"actual\":%lu,\"transportHealthy\":true,\"pixelRecovery\":%s}\n",
      evidence.rejections, kind, slot, unsigned(h.op), h.id, h.generation, DWORD(expected), DWORD(actual), Bool(imageRecovery));
  }
  std::pair<uint64_t, uint64_t> issue(d9::ResourceRef ref, uint64_t revision) {
    auto h = Header(d9::Op::Issue, ref); h.args[0] = uint32_t(revision); h.args[1] = uint32_t(revision >> 32);
    std::array<uint8_t, 16> result{}; call(h, nullptr, result.data(), UINT(result.size())); ++evidence.queryIssues;
    const auto observedRevision = d9::U64(result.data()), serial = d9::U64(result.data() + 8);
    Require(observedRevision == revision && serial, "issued query revision/serial");
    if (!evidence.firstSerial) evidence.firstSerial = serial;
    Require(serial == evidence.firstSerial, "no GPU commands but event serial changed");
    evidence.lastSerial = serial; ++evidence.sameSerialChecks; return {revision, serial};
  }
  static d9::Header pollHeader(d9::ResourceRef ref, uint64_t revision, uint64_t serial) {
    auto h = Header(d9::Op::Poll, ref); h.args[0] = uint32_t(revision); h.args[1] = uint32_t(revision >> 32);
    h.args[2] = uint32_t(serial); h.args[3] = uint32_t(serial >> 32); return h;
  }
  void poll(d9::ResourceRef ref, uint64_t revision, uint64_t serial) {
    std::array<uint8_t, 4> result{}; call(pollHeader(ref, revision, serial), nullptr, result.data(), UINT(result.size()));
    Require(d9::U32(result.data()) == 1, "issued retired query not complete"); ++evidence.queryPolls;
  }
};
void RawMode(const Options& o, const Shaders& shaders, Evidence& e) {
  Raw r(shaders, e); auto h = Raw::Header(d9::Op::Initialize); h.args[0] = h.args[1] = Size; r.call(h);
  r.createScene(1, 0xffaa3311u); r.recover(); const auto old = r.refs;
  for (UINT i = 0; i < 6; ++i) r.destroy(old[i], Types[i]);
  r.createScene(2, 0xff11bb66u); r.recover();
  for (UINT i = 0; i < 3; ++i) {
    h = Raw::Header(i == 2 ? d9::Op::UploadTexture : d9::Op::UploadBuffer, old[i]); h.payloadBytes = 4;
    const DWORD poison = 0xffffffffu; r.rejected("stale-upload", i, h, &poison);
  }
  for (UINT i = 0; i < 6; ++i) { h = Raw::Header(d9::Op::Destroy, old[i]); h.args[0] = uint32_t(Types[i]); r.rejected("stale-destroy", i, h); }
  for (UINT kind = 0; kind < 3; ++kind) for (UINT i = 0; i < 6; ++i) {
    auto command = r.draw(); command.resources[i] = kind == 0 ? old[i] : kind == 1 ? r.refs[(i + 1) % 6] : d9::ResourceRef{i + 1, 3};
    const auto packet = d9::EncodeDraw(command); h = Raw::Header(d9::Op::Draw); h.payloadBytes = UINT(packet.size());
    r.rejected(kind == 0 ? "stale-draw" : kind == 1 ? "wrong-type-draw" : "future-draw", i, h, packet.data());
  }
  for (UINT i = 0; i < 3; ++i) {
    h = Raw::Header(i == 2 ? d9::Op::UploadTexture : d9::Op::UploadBuffer, {i + 1, 3}); h.payloadBytes = 4;
    const DWORD poison = 0xffffffffu; r.rejected("future-upload", i, h, &poison);
  }
  for (UINT i = 0; i < 6; ++i) { h = Raw::Header(d9::Op::Destroy, {i + 1, 3}); h.args[0] = uint32_t(Types[i]); r.rejected("future-destroy", i, h); }
  for (UINT i = 0; i < 6; ++i) { h = Raw::Header(d9::Op::Destroy, r.refs[i]); h.args[0] = uint32_t(Types[(i + 1) % 6]); r.rejected("wrong-type-destroy", i, h); }
  const DWORD poison = 0xffffffffu;
  h = Raw::Header(d9::Op::UploadTexture, r.refs[0]); h.payloadBytes = 4; r.rejected("wrong-type-upload", 0, h, &poison);
  h = Raw::Header(d9::Op::UploadBuffer, r.refs[2]); h.payloadBytes = 4; r.rejected("wrong-type-upload", 2, h, &poison);
  h = Raw::Header(d9::Op::CreateBuffer, {8, 1}); r.rejected("bad-create-retry", 0, h);
  r.buffer({8, 1}, 64); ++e.sameGenerationRetries; r.destroy({8, 1}, d9::Type::VertexBuffer);
  for (UINT id = 8; id < 14; ++id) r.buffer({id, id == 8 ? 2u : 1u}, 64);
  e.typedSlotsFilled = 8; // Six successful additional VB creations plus scene VB/IB.
  h = Raw::Header(d9::Op::CreateBuffer, {14, 1}); h.args[0] = 64; r.rejected("typed-slots-full", 0, h, nullptr, D3DERR_OUTOFVIDEOMEMORY);
  r.destroy({8, 2}, d9::Type::VertexBuffer); r.buffer({14, 1}, 64); ++e.sameGenerationRetries;
  for (UINT id = 9; id <= 14; ++id) r.destroy({id, 1}, d9::Type::VertexBuffer);
  h = Raw::Header(d9::Op::CreateBuffer, {14, 3}); h.args[0] = 64; r.rejected("future-create", 0, h);
  r.buffer({14, 2}, 64); r.destroy({14, 2}, d9::Type::VertexBuffer);
  h = Raw::Header(d9::Op::CreateBuffer, r.refs[0]); h.args[0] = 64; r.rejected("duplicate-live-create", 0, h);
  Require(e.rejections == 48 && e.pixelRecoveries == 48, "resource negative population");
  d9::ResourceRef query{7, 1}; r.call(Raw::Header(d9::Op::CreateQuery, query));
  for (UINT n = 1; n <= QueryRepeats; ++n) { const auto issued = r.issue(query, n); r.poll(query, issued.first, issued.second); }
  uint64_t currentRevision = QueryRepeats;
  auto queryReject = [&](const char* name, d9::Header bad) {
    r.rejected(name, 0, bad, nullptr, D3DERR_INVALIDCALL, false);
    r.poll(query, currentRevision, e.lastSerial); ++e.queryRecoveries;
  };
  queryReject("query-old-revision", Raw::pollHeader(query, 255, e.lastSerial));
  queryReject("query-future-revision", Raw::pollHeader(query, 257, e.lastSerial));
  queryReject("query-wrong-serial", Raw::pollHeader(query, 256, e.lastSerial + 1));
  h = Raw::Header(d9::Op::Issue, query); h.args[0] = 256; queryReject("query-duplicate-issue", h);
  h.args[0] = 258; queryReject("query-gap-issue", h);
  queryReject("query-wrong-type-poll", Raw::pollHeader(r.refs[0], 256, e.lastSerial));
  h = Raw::Header(d9::Op::Issue, r.refs[0]); h.args[0] = 1; queryReject("query-wrong-type-issue", h);
  h = Raw::Header(d9::Op::Destroy, query); h.args[0] = uint32_t(d9::Type::VertexBuffer); queryReject("query-wrong-type-destroy", h);
  r.destroy(query, d9::Type::EventQuery); query.generation = 2;
  r.call(Raw::Header(d9::Op::CreateQuery, query)); currentRevision = 1; r.issue(query, 1); r.poll(query, 1, e.lastSerial);
  queryReject("query-old-key-poll", Raw::pollHeader({7, 1}, 256, e.lastSerial));
  h = Raw::Header(d9::Op::Issue, {7, 1}); h.args[0] = 257; queryReject("query-old-key-issue", h);
  h = Raw::Header(d9::Op::Destroy, {7, 1}); h.args[0] = uint32_t(d9::Type::EventQuery); queryReject("query-old-key-destroy", h);
  h = Raw::Header(d9::Op::Issue, {7, 3}); h.args[0] = 1; queryReject("query-future-key-issue", h);
  queryReject("query-future-key-poll", Raw::pollHeader({7, 3}, 1, e.lastSerial));
  r.destroy(query, d9::Type::EventQuery); r.recover();
  for (UINT i = 0; i < 6; ++i) r.destroy(r.refs[i], Types[i]);
  r.call(Raw::Header(d9::Op::Finish)); r.connection.reset();
  Require(e.rejections == 61 && e.queryRecoveries == 13 && e.resourcesCreated == e.resourcesReleased, "raw final population");
  Desktop(o); e.cleanup = true;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    const auto options = Parse(argc, argv); Desktop(options);
    wchar_t profile[64]{}; const DWORD chars = GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE", profile, 64);
    Require(!chars, "offscreen-only lifetime fixture requires no profile override");
    Shaders shaders; Evidence evidence;
    if (options.mode == L"com") ComMode(options, shaders, evidence); else RawMode(options, shaders, evidence);
    Print(options, evidence); return 0;
  } catch (const std::exception& error) {
    std::printf("{\"schema\":\"warvk-d3d9-resource-lifetime-error/v1\",\"ok\":false,\"error\":%s}\n", Json(error.what()).c_str());
    return 1;
  }
}
