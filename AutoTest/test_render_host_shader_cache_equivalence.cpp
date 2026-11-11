// Public x86 D3D9 pixel regression for BOTH actual shader-cache adapters.
// Run --reference with each old/new shipping DLL in its own isolated process;
// omit it for the development proxy + x64 host. Never load multiple renderer
// DLLs in one process. This fixture does not observe/certify cache cookies:
// root's independent host cache observation and child-exit receipt do that.
//
// Reuse only the established public-API/window/COM foundation. No wire, backend
// or cache headers are included, and expected pixels below are scene constants.
#define wmain WarVKShaderEquivalenceUnusedParentMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include <algorithm>
#include <utility>

namespace {
constexpr UINT CacheCycles = 8, CachePhases = 9;
constexpr UINT CachePixelsPerFrame = 3328;
constexpr DWORD CacheClear = 0xff13273bu;
constexpr DWORD CacheRed = 0xffff0000u, CacheGreen = 0xff00ff00u;
constexpr ULONGLONG CacheExitLimitMs = 5000;
using CacheWords = std::vector<DWORD>;

struct CacheFixtureOptions {
  Options api;
  std::wstring expectedDesktop, expectedInput;
};

CacheFixtureOptions CacheParse(int argc, wchar_t** argv) {
  CacheFixtureOptions result;
  bool scenarioSeen = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring key = argv[i];
    if (key == L"--reference") {
      Require(!result.api.reference, "duplicate --reference");
      result.api.reference = true;
      continue;
    }
    Require(i + 1 < argc, "missing shader-equivalence option value");
    const std::wstring value = argv[++i];
    if (key == L"--scenario") {
      Require(!scenarioSeen && value == L"normal", "only scenario normal is supported");
      scenarioSeen = true;
      continue;
    }
    auto* target = key == L"--dll" ? &result.api.dll
      : key == L"--expect-desktop" ? &result.expectedDesktop
      : key == L"--input-desktop" ? &result.expectedInput : nullptr;
    Require(target && target->empty() && !value.empty(), "unknown/duplicate/empty shader-equivalence option");
    *target = value;
  }
  const auto& path = result.api.dll;
  Require(path.size() >= 3 && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\'),
    "shader equivalence requires an absolute drive DLL path");
  const DWORD attributes = GetFileAttributesW(path.c_str());
  Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY),
    "shader equivalence DLL is missing");
  Require(!result.expectedDesktop.empty() && !result.expectedInput.empty(),
    "explicit isolated/input desktop names are required");
  wchar_t profile[64]{};
  Require(!GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE", profile, 64),
    "shader equivalence is offscreen only; no profile override permitted");
  return result;
}

CacheWords CacheVertexWords() {
  // vs_3_0: dcl_position v0; dcl_position o0; mov o0,v0; end.
  return {0xfffe0300u, 0x0200001fu, 0x80000000u, 0x900f0000u,
    0x0200001fu, 0x80000000u, 0xe00f0000u,
    0x02000001u, 0xe00f0000u, 0x90e40000u, 0x0000ffffu};
}

CacheWords CachePixelWords(bool green) {
  // ps_3_0: def c0, {1,0,0,1} or {0,1,0,1}; mov oC0,c0; end.
  // Only two literal constant words differ; stage/layout/options are identical.
  // Immediate values, not generated output or implementation metadata, define
  // the independent red/green pixel oracle.
  return {0xffff0300u, 0x05000051u, 0xa00f0000u,
    green ? 0u : 0x3f800000u, green ? 0x3f800000u : 0u, 0u, 0x3f800000u,
    0x02000001u, 0x800f0800u, 0xa0e40000u, 0x0000ffffu};
}

struct CacheEvidence {
  UINT frames = 0, redFrames = 0, greenFrames = 0, pixelChecks = 0, pixelFailures = 0, maxError = 0;
  UINT vertexCreates = 0, pixelCreates = 0, vertexPublicZero = 0, pixelPublicZero = 0;
  UINT shaderRoundtrips = 0, simultaneousDistinctPairs = 0, allPublicShaderRefsReleased = 0;
  UINT queryIssues = 0, queryPolls = 0, desktopChecks = 0;
  uint64_t redPixelHash = 0, greenPixelHash = 0, vertexWordsHash = 0, redWordsHash = 0, greenWordsHash = 0;
  uint64_t deviceReleaseMs = 0;
};

// Owned until successful normal teardown. Failure retains the entire owner
// until exact-process exit, preserving the established no-driver-unwind rule.
struct CacheFixture {
  Owner owner;
  CacheFixtureOptions options;
  DesktopWitness lastDesktop;
  CacheEvidence evidence;
  Com<IDirect3DVertexShader9> peerVs;
  Com<IDirect3DPixelShader9> peerPs, greenPs, greenPeerPs;
};

void CacheDesktop(CacheFixture& test) {
  const auto witness = NonInputDesktop();
  test.lastDesktop = witness;
  Require(_wcsicmp(witness.thread.c_str(), test.options.expectedDesktop.c_str()) == 0
    && _wcsicmp(witness.input.c_str(), test.options.expectedInput.c_str()) == 0,
    "shader-equivalence isolated/input desktop identity changed");
  const HWND window = test.owner.window.window.load(std::memory_order_acquire);
  if (window) {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    Require(process == GetCurrentProcessId() && thread != GetCurrentThreadId()
      && thread == test.owner.window.ownerThreadId.load(std::memory_order_acquire),
      "offscreen HWND owner identity changed");
    const auto windowDesktop = DesktopName(GetThreadDesktop(thread));
    Require(_wcsicmp(windowDesktop.c_str(), witness.thread.c_str()) == 0 && !IsWindowVisible(window),
      "offscreen HWND is visible or on a different Desktop");
  }
  CheckProcessBoundary(test.owner);
  ++test.evidence.desktopChecks;
}

template<class T> void CacheShaderBytes(CacheFixture& test, T* shader, const CacheWords& words) {
  UINT bytes = 0;
  Check(shader->GetFunction(nullptr, &bytes), "shader GetFunction size");
  Require(bytes == words.size() * sizeof(DWORD), "shader byte count differs from original literal tokens");
  std::vector<uint8_t> guarded(size_t(bytes) + 16, 0xa5);
  const UINT expectedBytes = bytes;
  Check(shader->GetFunction(guarded.data() + 8, &bytes), "shader GetFunction exact bytes");
  Require(bytes == expectedBytes && !std::memcmp(guarded.data() + 8, words.data(), bytes),
    "shader original token roundtrip differs");
  for (unsigned i = 0; i < 8; ++i)
    Require(guarded[i] == 0xa5 && guarded[bytes + 8 + i] == 0xa5,
      "shader GetFunction overwrote guards");
  CheckDeviceIdentity(test.owner, shader);
  ++test.evidence.shaderRoundtrips;
}

void CacheCreateVs(CacheFixture& test, Com<IDirect3DVertexShader9>& out, const CacheWords& words) {
  Require(!out.get(), "live VS replacement is not a destroy/recreate test");
  Check(test.owner.device->CreateVertexShader(words.data(), out.put()), "cache fixture CreateVertexShader");
  Require(out.get() != nullptr, "successful VS create returned null");
  ++test.evidence.vertexCreates;
  CacheShaderBytes(test, out.get(), words);
}

void CacheCreatePs(CacheFixture& test, Com<IDirect3DPixelShader9>& out, const CacheWords& words) {
  Require(!out.get(), "live PS replacement is not a destroy/recreate test");
  Check(test.owner.device->CreatePixelShader(words.data(), out.put()), "cache fixture CreatePixelShader");
  Require(out.get() != nullptr, "successful PS create returned null");
  ++test.evidence.pixelCreates;
  CacheShaderBytes(test, out.get(), words);
}

template<class T> void CacheRelease(Com<T>& resource, UINT& zeroCount) {
  if (!resource.get()) return;
  Require(resource.detach()->Release() == 0, "shader public references did not reach zero");
  ++zeroCount;
}

void CacheUnbindShaders(CacheFixture& test) {
  Check(test.owner.device->SetVertexShader(nullptr), "unbind VS before destruction");
  Check(test.owner.device->SetPixelShader(nullptr), "unbind PS before destruction");
}

void CacheCreateScene(CacheFixture& test) {
  auto& owner = test.owner;
  owner.stage = "shader-equivalence-geometry";
  std::array<Vertex, 4> vertices{};
  FillQuad(vertices.data(), .75f, .5f, 1.f);
  const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
  Check(owner.device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY, 0,
    D3DPOOL_DEFAULT, owner.vb.put(), nullptr), "cache fixture CreateVertexBuffer");
  Check(owner.device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
    D3DPOOL_DEFAULT, owner.ib.put(), nullptr), "cache fixture CreateIndexBuffer");
  void* mapped = nullptr;
  Check(owner.vb->Lock(0, sizeof(vertices), &mapped, 0), "cache fixture VB Lock");
  Require(mapped != nullptr, "VB Lock returned null");
  std::memcpy(mapped, vertices.data(), sizeof(vertices));
  Check(owner.vb->Unlock(), "cache fixture VB Unlock");
  Check(owner.ib->Lock(0, sizeof(indices), &mapped, 0), "cache fixture IB Lock");
  Require(mapped != nullptr, "IB Lock returned null");
  std::memcpy(mapped, indices, sizeof(indices));
  Check(owner.ib->Unlock(), "cache fixture IB Unlock");
  const D3DVERTEXELEMENT9 elements[] = {
    // Keep the complete existing finite draw profile, even though this VS
    // only consumes POSITION. A smaller declaration is not an accepted wire.
    {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  Check(owner.device->CreateVertexDeclaration(elements, owner.declaration.put()), "cache fixture declaration");
  // The finite host draw profile always carries an initialized texture even
  // though this particular PS does not sample it. Do not weaken that profile.
  Check(owner.device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
    owner.texture.put(), nullptr), "cache fixture texture");
  D3DLOCKED_RECT locked{};
  Check(owner.texture->LockRect(0, &locked, nullptr, 0), "cache fixture texture Lock");
  Require(locked.pBits && locked.Pitch >= 4, "texture Lock storage");
  const DWORD white = 0xffffffffu; std::memcpy(locked.pBits, &white, sizeof(white));
  Check(owner.texture->UnlockRect(0), "cache fixture texture Unlock");
  Check(owner.device->SetStreamSource(0, owner.vb.get(), 0, sizeof(Vertex)), "cache fixture bind VB");
  Check(owner.device->SetIndices(owner.ib.get()), "cache fixture bind IB");
  Check(owner.device->SetVertexDeclaration(owner.declaration.get()), "cache fixture bind declaration");
  Check(owner.device->SetTexture(0, owner.texture.get()), "cache fixture bind texture");
  const D3DVIEWPORT9 viewport {0, 0, Width, Height, 0.f, 1.f};
  Check(owner.device->SetViewport(&viewport), "cache fixture viewport");
  const std::pair<D3DRENDERSTATETYPE, DWORD> states[] = {
    {D3DRS_ZENABLE, TRUE}, {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
    {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_COLORWRITEENABLE, 15}};
  for (const auto& state : states)
    Check(owner.device->SetRenderState(state.first, state.second), "cache fixture render state");
  Check(owner.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), "cache sampler min");
  Check(owner.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT), "cache sampler mag");
  Check(owner.device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "cache sampler mip");
  Check(owner.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP), "cache sampler U");
  Check(owner.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP), "cache sampler V");
  Check(owner.device->CreateOffscreenPlainSurface(Width, Height, D3DFMT_A8R8G8B8,
    D3DPOOL_SYSTEMMEM, owner.readback.put(), nullptr), "cache fixture readback surface");
  Check(owner.device->CreateQuery(D3DQUERYTYPE_EVENT, owner.query.put()), "cache fixture EVENT query");
  Require(owner.query.get() && owner.query->GetType() == D3DQUERYTYPE_EVENT
    && owner.query->GetDataSize() == sizeof(BOOL), "cache fixture EVENT query shape");
  CheckDeviceIdentity(owner, owner.vb.get()); CheckDeviceIdentity(owner, owner.ib.get());
  CheckDeviceIdentity(owner, owner.texture.get()); CheckDeviceIdentity(owner, owner.declaration.get());
  CacheDesktop(test);
}

void CacheWait(CacheFixture& test) {
  Check(test.owner.query->Issue(D3DISSUE_END), "cache fixture query Issue");
  ++test.evidence.queryIssues;
  const auto deadline = GetTickCount64() + CacheExitLimitMs;
  for (;;) {
    struct Guarded { DWORD before; BOOL value; DWORD after; } value {0x18395a7bu, FALSE, 0xc2e40628u};
    const HRESULT hr = test.owner.query->GetData(&value.value, sizeof(BOOL), D3DGETDATA_FLUSH);
    ++test.evidence.queryPolls;
    Require(value.before == 0x18395a7bu && value.after == 0xc2e40628u, "EVENT BOOL guards changed");
    if (hr == S_OK) { Require(value.value == TRUE, "EVENT S_OK without completion"); return; }
    Require(hr == S_FALSE && GetTickCount64() < deadline, "cache fixture query failed/timed out");
    Sleep(1); // Isolated caller only; no global input or window-owner blocking.
  }
}

void CacheDraw(CacheFixture& test, UINT cycle, const char* phase,
    IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps, bool green) {
  auto& owner = test.owner;
  owner.stage = phase;
  Check(owner.device->SetVertexShader(vs), "cache fixture bind VS");
  Check(owner.device->SetPixelShader(ps), "cache fixture bind PS");
  // A public c0 write must not overwrite this shader's DEF immediate.
  const float poison[4] = {0.f, 0.f, 1.f, 1.f};
  Check(owner.device->SetPixelShaderConstantF(0, poison, 1), "cache fixture external c0 poison");
  Check(owner.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CacheClear, 1.f, 0), "cache fixture Clear");
  Check(owner.device->BeginScene(), "cache fixture BeginScene");
  Check(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2), "cache fixture DrawIndexedPrimitive");
  Check(owner.device->EndScene(), "cache fixture EndScene");
  ++owner.draws; owner.primitives += 2;
  CacheWait(test);
  Check(owner.device->GetRenderTargetData(owner.target.get(), owner.readback.get()), "cache fixture actual GPU readback");
  D3DLOCKED_RECT locked{};
  Check(owner.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "cache fixture readback Lock");
  Require(locked.pBits && locked.Pitch >= int(Width * 4), "cache readback storage/pitch");
  UINT checked = 0, innerCount = 0, clearCount = 0, failures = 0, maxError = 0;
  uint64_t hash = FnvOffset;
  for (UINT y = 0; y < Height; ++y) for (UINT x = 0; x < Width; ++x) {
    const bool clear = x <= 5 || x >= 58 || y <= 5 || y >= 58;
    const bool inner = x >= 10 && x <= 53 && y >= 10 && y <= 53;
    if (!clear && !inner) continue; // Fixed geometric edge guard, not output-derived.
    const DWORD expected = clear ? CacheClear : green ? CacheGreen : CacheRed;
    const auto* actual = static_cast<const uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch + x * 4;
    bool bad = false;
    for (unsigned c = 0; c < 4; ++c) {
      const int delta = int(actual[c]) - int((expected >> (8 * c)) & 255);
      const UINT error = UINT(delta < 0 ? -delta : delta);
      maxError = std::max(maxError, error);
      bad |= error > (c == 3 ? 0u : 1u);
    }
    if (bad && !failures)
      std::fprintf(stderr, "cache pixel mismatch phase=%s x=%u y=%u expected=%08lx actual=%02x%02x%02x%02x\n",
        phase, x, y, expected, actual[3], actual[2], actual[1], actual[0]);
    hash = Hash(actual, 4, hash); ++checked; innerCount += inner; clearCount += clear; failures += bad;
  }
  Check(owner.readback->UnlockRect(), "cache fixture readback Unlock");
  Require(checked == CachePixelsPerFrame && innerCount == 1936 && clearCount == 1392,
    "cache independent oracle population changed");
  auto& e = test.evidence;
  ++e.frames; e.redFrames += !green; e.greenFrames += green;
  e.pixelChecks += checked; e.pixelFailures += failures; e.maxError = std::max(e.maxError, maxError);
  auto& previous = green ? e.greenPixelHash : e.redPixelHash;
  const bool sameColorStable = !previous || previous == hash;
  if (!previous) previous = hash;
  CacheDesktop(test);
  std::printf("{\"schema\":\"warvk-d3d9-shader-cache-equivalence-frame/v1\",\"cycle\":%u,\"phase\":%s,"
    "\"ordinal\":%u,\"expectedBGRA\":%lu,\"pixelsChecked\":%u,\"inner\":%u,\"clear\":%u,"
    "\"failures\":%u,\"maxChannelError\":%u,\"sampledPixelFnv\":\"%016llx\",\"sameColorStable\":%s}\n",
    cycle, JsonString(phase).c_str(), e.frames, green ? CacheGreen : CacheRed, checked, innerCount,
    clearCount, failures, maxError, static_cast<unsigned long long>(hash), JsonBool(sameColorStable));
  std::fflush(stdout);
  Require(!failures && sameColorStable, "cache shader pixels differ or failed to restore");
}

void CacheCycle(CacheFixture& test, UINT cycle, const CacheWords& vs, const CacheWords& red, const CacheWords& green) {
  auto& o = test.owner; auto& e = test.evidence;
  CacheUnbindShaders(test);
  CacheRelease(o.vs, e.vertexPublicZero); CacheRelease(o.ps, e.pixelPublicZero);
  CacheCreateVs(test, o.vs, vs); CacheCreatePs(test, o.ps, red);
  CacheCreateVs(test, test.peerVs, vs); CacheCreatePs(test, test.peerPs, red);
  Require(o.vs.get() != test.peerVs.get() && o.ps.get() != test.peerPs.get(),
    "same-token live creates did not have distinct public COM objects");
  e.simultaneousDistinctPairs += 2;
  CacheDraw(test, cycle, "original-a", o.vs.get(), o.ps.get(), false);
  CacheDraw(test, cycle, "same-token-distinct-live-objects", test.peerVs.get(), test.peerPs.get(), false);
  CacheDraw(test, cycle, "original-a-restored", o.vs.get(), o.ps.get(), false);
  CacheUnbindShaders(test);
  CacheRelease(o.vs, e.vertexPublicZero); CacheRelease(o.ps, e.pixelPublicZero);
  CacheCreateVs(test, o.vs, vs); CacheCreatePs(test, o.ps, red);
  CacheDraw(test, cycle, "recreated-while-peer-live", o.vs.get(), o.ps.get(), false);
  CacheUnbindShaders(test);
  CacheRelease(o.vs, e.vertexPublicZero); CacheRelease(o.ps, e.pixelPublicZero);
  CacheRelease(test.peerVs, e.vertexPublicZero); CacheRelease(test.peerPs, e.pixelPublicZero);
  ++e.allPublicShaderRefsReleased;
  CacheCreateVs(test, o.vs, vs); CacheCreatePs(test, o.ps, red);
  CacheDraw(test, cycle, "recreated-after-all-public-shaders-released", o.vs.get(), o.ps.get(), false);
  CacheCreatePs(test, test.greenPs, green);
  CacheDraw(test, cycle, "only-immediate-constant-bytes-changed", o.vs.get(), test.greenPs.get(), true);
  CacheDraw(test, cycle, "original-constant-bytes-restored", o.vs.get(), o.ps.get(), false);
  CacheCreatePs(test, test.greenPeerPs, green);
  Require(test.greenPeerPs.get() != test.greenPs.get(), "changed-token duplicate COM objects aliased");
  ++e.simultaneousDistinctPairs;
  CacheDraw(test, cycle, "changed-token-distinct-live-objects", o.vs.get(), test.greenPeerPs.get(), true);
  Check(o.device->SetPixelShader(nullptr), "unbind changed PS before destruction");
  CacheRelease(test.greenPs, e.pixelPublicZero); CacheRelease(test.greenPeerPs, e.pixelPublicZero);
  CacheRelease(o.ps, e.pixelPublicZero);
  CacheCreatePs(test, o.ps, red);
  CacheDraw(test, cycle, "original-recreated-after-changed-shaders-destroyed", o.vs.get(), o.ps.get(), false);
}

void CacheTeardown(CacheFixture& test) {
  auto& o = test.owner; auto& e = test.evidence;
  o.stage = "shader-cache-bound-resource-teardown";
  CacheDesktop(test);
  Require(!test.peerVs.get() && !test.peerPs.get() && !test.greenPs.get() && !test.greenPeerPs.get(),
    "extra shader survived its ordered phase teardown");
  o.query.reset(); o.readback.reset(); o.back.reset(); o.target.reset();
  o.vb.reset(); o.ib.reset();
  // As in the original fixture, final bound state retains its private use
  // after these public references reach zero. No cache/retirement bypass.
  CacheRelease(o.vs, e.vertexPublicZero); CacheRelease(o.ps, e.pixelPublicZero);
  o.declaration.reset();
  IDirect3DDevice9* original = o.device.detach();
  o.deviceRefsWhileResourceHeld = original->Release();
  Require(o.deviceRefsWhileResourceHeld > 0, "texture failed to retain its device");
  Check(o.texture->GetDevice(o.device.put()), "resource-held device GetDevice");
  SameIdentity(o, original, o.device.get()); o.resourceHeldDevice = true;
  o.texture.reset();
  const auto releaseStart = GetTickCount64();
  o.deviceFinalRefs = o.device.detach()->Release();
  e.deviceReleaseMs = GetTickCount64() - releaseStart;
  Require(o.deviceFinalRefs == 0 && e.deviceReleaseMs <= CacheExitLimitMs,
    "device final Release failed or exceeded unchanged five-second exit gate");
  o.factoryFinalRefs = o.factory.detach()->Release();
  Require(o.factoryFinalRefs == 0, "factory final references remain");
  Require(FreeLibrary(o.renderer) != FALSE, "renderer DLL unload failed"); o.renderer = nullptr;
  Require(o.compiler == nullptr, "literal shader fixture unexpectedly loaded a compiler");
  o.window.stop(); // Existing owner waits at most 5000 ms; never force-terminates.
  CacheDesktop(test);
  o.normalTeardown = true; o.stage = "complete";
}

void CachePrint(const CacheFixture& test, bool ok, const char* error) {
  const auto& e = test.evidence; const auto& o = test.owner;
  std::printf("{\"schema\":\"warvk-d3d9-shader-cache-equivalence/v1\",\"ok\":%s,\"bits\":32,"
    "\"reference\":%s,\"adapter\":\"%s\",\"stage\":%s,\"error\":%s,\"publicD3D9Api\":true,"
    "\"cycles\":%u,\"phasesPerCycle\":%u,\"frames\":%u,\"redFrames\":%u,\"greenFrames\":%u,"
    "\"pixelsChecked\":%u,\"pixelFailures\":%u,\"maxChannelError\":%u,\"draws\":%u,"
    "\"vertexCreates\":%u,\"pixelCreates\":%u,\"vertexPublicZero\":%u,\"pixelPublicZero\":%u,"
    "\"shaderByteRoundtrips\":%u,\"simultaneousDistinctCOMPairs\":%u,\"allPublicShaderRefsReleased\":%u,"
    "\"queryIssues\":%u,\"queryPolls\":%u,\"desktopChecks\":%u,"
    "\"vertexTokenFnv\":\"%016llx\",\"redTokenFnv\":\"%016llx\",\"greenTokenFnv\":\"%016llx\","
    "\"redSampledPixelFnv\":\"%016llx\",\"greenSampledPixelFnv\":\"%016llx\","
    "\"signatureConsumedByDraw\":%s,\"definedConstantOutputVerified\":%s,"
    "\"needsConstantCopiesCovered\":false,\"needsConstantCopiesReason\":\"relative-operands-not-admitted-by-current-host-profile\","
    "\"cacheReuseCertifiedByFixture\":false,\"cacheIdentityEvidence\":\"separate-host-cache-observation-required\","
    "\"normalTeardown\":%s,\"deviceFinalRefs\":%lu,\"factoryFinalRefs\":%lu,\"resourceHeldDevice\":%s,"
    "\"deviceReleaseMs\":%llu,\"deviceReleaseLimitMs\":5000,\"parentVulkanAbsent\":%s,"
    "\"remoteChildExitVerifiedByFixture\":false,\"windowOwnerClosed\":%s,\"windowClassUnregistered\":%s,"
    "\"desktop\":%s,\"inputBefore\":%s,\"inputAfter\":%s,"
    "\"globalInputUsed\":false,\"desktopSwitched\":false,\"gameTouched\":false,\"productIntegrated\":false,"
    "\"memorySavingsProven\":false,\"windowPixelsVerified\":false,\"playerVisualAccepted\":false}\n",
    JsonBool(ok), JsonBool(o.options.reference), o.options.reference ? "shipping-d3d9" : "x64-development-host",
    JsonString(o.stage).c_str(), JsonString(error).c_str(), CacheCycles, CachePhases, e.frames, e.redFrames, e.greenFrames,
    e.pixelChecks, e.pixelFailures, e.maxError, o.draws, e.vertexCreates, e.pixelCreates, e.vertexPublicZero,
    e.pixelPublicZero, e.shaderRoundtrips, e.simultaneousDistinctPairs, e.allPublicShaderRefsReleased,
    e.queryIssues, e.queryPolls, e.desktopChecks, static_cast<unsigned long long>(e.vertexWordsHash),
    static_cast<unsigned long long>(e.redWordsHash), static_cast<unsigned long long>(e.greenWordsHash),
    static_cast<unsigned long long>(e.redPixelHash), static_cast<unsigned long long>(e.greenPixelHash),
    JsonBool(ok), JsonBool(ok), JsonBool(o.normalTeardown), o.deviceFinalRefs, o.factoryFinalRefs,
    JsonBool(o.resourceHeldDevice), static_cast<unsigned long long>(e.deviceReleaseMs), JsonBool(o.parentVulkanAbsent),
    JsonBool(o.window.closed), JsonBool(o.window.classUnregistered.load(std::memory_order_acquire)),
    JsonString(Utf8(test.lastDesktop.thread).c_str()).c_str(),
    JsonString(Utf8(test.options.expectedInput).c_str()).c_str(),
    JsonString(Utf8(test.lastDesktop.input).c_str()).c_str());
  std::fflush(stdout);
}
}

int wmain(int argc, wchar_t** argv) {
  CacheFixture* test = nullptr;
  try {
    test = new CacheFixture;
    test->options = CacheParse(argc, argv); test->owner.options = test->options.api;
    CacheDesktop(*test);
    CreateDevice(test->owner);
    CacheDesktop(*test);
    CacheCreateScene(*test);
    const auto vs = CacheVertexWords(), red = CachePixelWords(false), green = CachePixelWords(true);
    Require(red.size() == green.size(), "changed shader altered token extent");
    UINT differentWords = 0;
    for (size_t i = 0; i < red.size(); ++i) {
      if (red[i] != green[i]) { Require(i == 3 || i == 4, "non-immediate shader token changed"); ++differentWords; }
    }
    Require(differentWords == 2, "expected exact two-word immediate change");
    auto& e = test->evidence;
    e.vertexWordsHash = Hash(vs.data(), vs.size() * 4);
    e.redWordsHash = Hash(red.data(), red.size() * 4); e.greenWordsHash = Hash(green.data(), green.size() * 4);
    Require(e.redWordsHash != e.greenWordsHash, "different original token identities required");
    for (UINT cycle = 1; cycle <= CacheCycles; ++cycle) CacheCycle(*test, cycle, vs, red, green);
    Require(e.frames == CacheCycles * CachePhases && e.redFrames == CacheCycles * 7 && e.greenFrames == CacheCycles * 2
      && e.pixelChecks == e.frames * CachePixelsPerFrame && !e.pixelFailures
      && e.redPixelHash != e.greenPixelHash, "incomplete/different-color pixel population");
    Require(e.vertexCreates == CacheCycles * 4 && e.pixelCreates == CacheCycles * 7
      && e.shaderRoundtrips == e.vertexCreates + e.pixelCreates
      && e.simultaneousDistinctPairs == CacheCycles * 3 && e.allPublicShaderRefsReleased == CacheCycles,
      "shader recreation/identity population changed");
    CacheTeardown(*test);
    Require(e.vertexPublicZero == e.vertexCreates && e.pixelPublicZero == e.pixelCreates,
      "public shader references did not all close");
    CachePrint(*test, true, ""); delete test; return 0;
  } catch (const std::exception& error) {
    if (test) CachePrint(*test, false, error.what());
    else { std::printf("{\"schema\":\"warvk-d3d9-shader-cache-equivalence/v1\",\"ok\":false,\"error\":\"owner allocation\"}\n"); std::fflush(stdout); }
  } catch (...) {
    if (test) CachePrint(*test, false, "unknown exception");
  }
  // Failed GPU/driver state is NOT normal teardown. The outer isolated runner
  // must still prove creator exit, empty owned Jobs and exact Desktop closure.
  ExitProcess(2);
}
