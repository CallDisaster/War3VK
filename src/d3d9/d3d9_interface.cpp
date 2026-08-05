#include "d3d9_interface.h"

#include "d3d9_monitor.h"
#include "d3d9_caps.h"
#include "d3d9_device.h"
#include "d3d9_bridge.h"
#include "d3d9_war3_hook.h"

#include "../util/util_singleton.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace dxvk {

  Singleton<DxvkInstance> g_dxvkInstance;

  namespace {

    std::atomic<uint32_t> g_d3d9InterfaceAcquireOrdinal{0u};
    std::atomic<uint32_t> g_d3d9InterfaceDestroyOrdinal{0u};
    std::atomic<uint32_t> g_d3d9CreateDeviceOrdinal{0u};
    thread_local uint32_t g_d3d9InterfaceCtorOrdinal = 0u;

    void TraceD3D9Interface(const char* phase, uint32_t ordinal,
                           const void* object = nullptr) {
      // 仅用于启动期接口生命周期取证；固定栈缓冲，不进入渲染热路径。
      if (ordinal == 0u || ordinal > 4u)
        return;
      char buffer[256] = { };
      std::snprintf(
        buffer, sizeof(buffer),
        "DXVK W3START pid=%lu tid=%lu tick=%llu comp=D3D9Interface ord=%u "
        "phase=%s object=%p hr=0x00000000\n",
        static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<unsigned long long>(::GetTickCount64()), ordinal,
        phase ? phase : "<null>", object);
      ::OutputDebugStringA(buffer);
    }

    Rc<DxvkInstance> AcquireD3D9Instance() {
      const uint32_t ordinal =
        g_d3d9InterfaceAcquireOrdinal.fetch_add(1u, std::memory_order_relaxed) + 1u;
      g_d3d9InterfaceCtorOrdinal = ordinal;
      TraceD3D9Interface("acquire-begin", ordinal);
      Rc<DxvkInstance> instance =
        g_dxvkInstance.acquire(DxvkInstanceFlag::ClientApiIsD3D9);
      TraceD3D9Interface("acquire-end", ordinal, instance.ptr());
      return instance;
    }

  }

  D3D9InterfaceEx::D3D9InterfaceEx(bool bExtended, const D3D9ON12_ARGS* pOverrideList, uint32_t OverrideCount)
    : m_instance    ( AcquireD3D9Instance() )
    , m_d3d8Bridge  ( this )
    , m_extended    ( bExtended ) 
    , m_d3d9Options ( nullptr, m_instance->config() )
    , m_d3d9Interop ( this )
    , m_d3d9ExtInterface( this ) {
    const uint32_t traceOrdinal = g_d3d9InterfaceCtorOrdinal;
    TraceD3D9Interface("ctor-body-begin", traceOrdinal, this);
    // D3D9 doesn't enumerate adapters like physical adapters...
    // only as connected displays.

    // Let's create some "adapters" for the amount of displays we have.
    // We'll go through and match up displays -> our adapters in order.
    // If we run out of adapters, then we'll just make repeats of the first one.
    // We can't match up by names on Linux/Wine as they don't match at all
    // like on Windows, so this is our best option.
#ifdef _WIN32
    if (m_d3d9Options.enumerateByDisplays) {
      DISPLAY_DEVICEA device = { };
      device.cb = sizeof(device);

      uint32_t adapterOrdinal = 0;
      uint32_t i = 0;
      while (::EnumDisplayDevicesA(nullptr, i++, &device, 0)) {
        // If we aren't attached, skip over.
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
          continue;

        // If we are a mirror, skip over this device.
        if (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
          continue;

        Rc<DxvkAdapter> adapter = adapterOrdinal >= m_instance->adapterCount()
          ? m_instance->enumAdapters(0)
          : m_instance->enumAdapters(adapterOrdinal);

        if (adapter != nullptr) {
          const auto* d3d9On12Args = Find9On12Args(adapter, pOverrideList, OverrideCount);
          m_adapters.emplace_back(this, d3d9On12Args, adapter, adapterOrdinal++, i - 1);
        }
      }
    }
    else
#endif
    {
      const uint32_t adapterCount = m_instance->adapterCount();
      m_adapters.reserve(adapterCount);

      for (uint32_t i = 0; i < adapterCount; i++) {
        const auto* d3d9On12Args = Find9On12Args(m_instance->enumAdapters(i), pOverrideList, OverrideCount);
        m_adapters.emplace_back(this, d3d9On12Args, m_instance->enumAdapters(i), i, 0);
      }
    }

#ifdef _WIN32
    if (m_d3d9Options.dpiAware) {
      Logger::info("Process set as DPI aware");
      SetProcessDPIAware();
    }
#endif

    if (unlikely(m_d3d9Options.shaderModel == 0))
      Logger::warn("D3D9InterfaceEx: WARNING! Fixed-function exclusive mode is enabled.");
    TraceD3D9Interface("ctor-body-end", traceOrdinal, this);
    g_d3d9InterfaceCtorOrdinal = 0u;
  }


  D3D9InterfaceEx::~D3D9InterfaceEx() {
    const uint32_t traceOrdinal =
      g_d3d9InterfaceDestroyOrdinal.fetch_add(1u, std::memory_order_relaxed) + 1u;
    TraceD3D9Interface("dtor-release-begin", traceOrdinal, this);
    g_dxvkInstance.release();
    TraceD3D9Interface("dtor-release-end", traceOrdinal, this);
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3D9)
     || (m_extended && riid == __uuidof(IDirect3D9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IDxvkD3D8InterfaceBridge)) {
      *ppvObject = ref(&m_d3d8Bridge);
      return S_OK;
    }

    if (riid == __uuidof(ID3D9VkInteropInterface)
     || riid == __uuidof(ID3D9VkInteropInterface1)) {
      *ppvObject = ref(&m_d3d9Interop);
      return S_OK;
    }

    if (riid == __uuidof(ID3D9VkExtInterface)) {
      *ppvObject = ref(&m_d3d9ExtInterface);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDirect3D9), riid)) {
      Logger::warn("D3D9InterfaceEx::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::RegisterSoftwareDevice(void* pInitializeFunction) {
    Logger::warn("D3D9InterfaceEx::RegisterSoftwareDevice: Stub");
    return D3D_OK;
  }


  UINT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterCount() {
    return UINT(m_adapters.size());
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterIdentifier(
          UINT                    Adapter,
          DWORD                   Flags,
          D3DADAPTER_IDENTIFIER9* pIdentifier) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetAdapterIdentifier(Flags, pIdentifier);

    return D3DERR_INVALIDCALL;
  }


  UINT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
    D3DDISPLAYMODEFILTER filter;
    filter.Size             = sizeof(D3DDISPLAYMODEFILTER);
    filter.Format           = Format;
    filter.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    
    return this->GetAdapterModeCountEx(Adapter, &filter);
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) {
    if (auto* adapter = GetAdapter(Adapter)) {
      D3DDISPLAYMODEEX modeEx = { };
      modeEx.Size = sizeof(D3DDISPLAYMODEEX);
      HRESULT hr = adapter->GetAdapterDisplayModeEx(&modeEx, nullptr);

      if (FAILED(hr))
        return hr;

      pMode->Width       = modeEx.Width;
      pMode->Height      = modeEx.Height;
      pMode->RefreshRate = modeEx.RefreshRate;
      pMode->Format      = modeEx.Format;

      return D3D_OK;
    }

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceType(
          UINT       Adapter,
          D3DDEVTYPE DevType,
          D3DFORMAT  AdapterFormat,
          D3DFORMAT  BackBufferFormat,
          BOOL       bWindowed) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDeviceType(
        DevType, EnumerateFormat(AdapterFormat),
        EnumerateFormat(BackBufferFormat), bWindowed);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceFormat(
          UINT            Adapter,
          D3DDEVTYPE      DeviceType,
          D3DFORMAT       AdapterFormat,
          DWORD           Usage,
          D3DRESOURCETYPE RType,
          D3DFORMAT       CheckFormat) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDeviceFormat(
        DeviceType, EnumerateFormat(AdapterFormat),
        Usage, RType,
        EnumerateFormat(CheckFormat));

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceMultiSampleType(
          UINT                Adapter,
          D3DDEVTYPE          DeviceType,
          D3DFORMAT           SurfaceFormat,
          BOOL                Windowed,
          D3DMULTISAMPLE_TYPE MultiSampleType,
          DWORD*              pQualityLevels) { 
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDeviceMultiSampleType(
        DeviceType, EnumerateFormat(SurfaceFormat),
        Windowed, MultiSampleType,
        pQualityLevels);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDepthStencilMatch(
          UINT       Adapter,
          D3DDEVTYPE DeviceType,
          D3DFORMAT  AdapterFormat,
          D3DFORMAT  RenderTargetFormat,
          D3DFORMAT  DepthStencilFormat) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDepthStencilMatch(
        DeviceType, EnumerateFormat(AdapterFormat),
        EnumerateFormat(RenderTargetFormat),
        EnumerateFormat(DepthStencilFormat));

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceFormatConversion(
          UINT       Adapter,
          D3DDEVTYPE DeviceType,
          D3DFORMAT  SourceFormat,
          D3DFORMAT  TargetFormat) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDeviceFormatConversion(
        DeviceType, EnumerateFormat(SourceFormat),
        EnumerateFormat(TargetFormat));

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetDeviceCaps(
          UINT       Adapter,
          D3DDEVTYPE DeviceType,
          D3DCAPS9*  pCaps) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetDeviceCaps(
        DeviceType, pCaps);

    return D3DERR_INVALIDCALL;
  }


  HMONITOR STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterMonitor(UINT Adapter) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetMonitor();

    return nullptr;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CreateDevice(
          UINT                   Adapter,
          D3DDEVTYPE             DeviceType,
          HWND                   hFocusWindow,
          DWORD                  BehaviorFlags,
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          IDirect3DDevice9**     ppReturnedDeviceInterface) {
    return this->CreateDeviceEx(
      Adapter,
      DeviceType,
      hFocusWindow,
      BehaviorFlags,
      pPresentationParameters,
      nullptr, // <-- pFullscreenDisplayMode
      reinterpret_cast<IDirect3DDevice9Ex**>(ppReturnedDeviceInterface));
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::EnumAdapterModes(
          UINT            Adapter,
          D3DFORMAT       Format,
          UINT            Mode,
          D3DDISPLAYMODE* pMode) {
    if (pMode == nullptr)
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEFILTER filter;
    filter.Format           = Format;
    filter.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    filter.Size             = sizeof(D3DDISPLAYMODEFILTER);

    D3DDISPLAYMODEEX modeEx = { };
    modeEx.Size = sizeof(D3DDISPLAYMODEEX);
    HRESULT hr = this->EnumAdapterModesEx(Adapter, &filter, Mode, &modeEx);

    if (FAILED(hr))
      return hr;

    pMode->Width       = modeEx.Width;
    pMode->Height      = modeEx.Height;
    pMode->RefreshRate = modeEx.RefreshRate;
    pMode->Format      = modeEx.Format;

    return D3D_OK;
  }


  // Ex Methods


  UINT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterModeCountEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER* pFilter) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetAdapterModeCountEx(pFilter);

    return 0;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::EnumAdapterModesEx(
          UINT                  Adapter,
    const D3DDISPLAYMODEFILTER* pFilter,
          UINT                  Mode,
          D3DDISPLAYMODEEX*     pMode) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->EnumAdapterModesEx(pFilter, Mode, pMode);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterDisplayModeEx(
          UINT                Adapter,
          D3DDISPLAYMODEEX*   pMode,
          D3DDISPLAYROTATION* pRotation) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetAdapterDisplayModeEx(pMode, pRotation);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CreateDeviceEx(
          UINT                   Adapter,
          D3DDEVTYPE             DeviceType,
          HWND                   hFocusWindow,
          DWORD                  BehaviorFlags,
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          IDirect3DDevice9Ex**   ppReturnedDeviceInterface) {
    const uint32_t traceOrdinal =
      g_d3d9CreateDeviceOrdinal.fetch_add(1u, std::memory_order_relaxed) + 1u;
    TraceD3D9Interface("create-device-entry", traceOrdinal, this);
    InitReturnPtr(ppReturnedDeviceInterface);

    if (unlikely(ppReturnedDeviceInterface  == nullptr
              || pPresentationParameters    == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(DeviceType == D3DDEVTYPE_SW))
      return D3DERR_INVALIDCALL;

    // Creating a device with D3DCREATE_PUREDEVICE only works in conjunction
    // with D3DCREATE_HARDWARE_VERTEXPROCESSING on native drivers.
    if (unlikely(BehaviorFlags & D3DCREATE_PUREDEVICE &&
               !(BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING)))
      return D3DERR_INVALIDCALL;

    // Neither D3DDEVTYPE_REF nor D3DDEVTYPE_NULLREF support HWVP, although they
    // will accept these flags as any regular D3DDEVTYPE_HAL device would.
    if (unlikely(DeviceType == D3DDEVTYPE_REF || DeviceType == D3DDEVTYPE_NULLREF)) {
      BehaviorFlags &= ~D3DCREATE_MIXED_VERTEXPROCESSING
                     & ~D3DCREATE_PUREDEVICE
                     & ~D3DCREATE_HARDWARE_VERTEXPROCESSING;
      BehaviorFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    }

    HRESULT hr;
    // Black Desert creates a D3DDEVTYPE_NULLREF device and
    // expects it be created despite passing invalid parameters.
    if (likely(DeviceType != D3DDEVTYPE_NULLREF)) {
      hr = ValidatePresentationParameters(pPresentationParameters);

      if (unlikely(FAILED(hr)))
        return hr;
    }

    auto* adapter = GetAdapter(Adapter);

    if (adapter == nullptr)
      return D3DERR_INVALIDCALL;

    auto dxvkAdapter = adapter->GetDXVKAdapter();

    try {
      TraceD3D9Interface("create-dxvk-device-begin", traceOrdinal, this);
      auto dxvkDevice = dxvkAdapter->createDevice();
      TraceD3D9Interface("create-dxvk-device-end", traceOrdinal, dxvkDevice.ptr());

      TraceD3D9Interface("device-ctor-begin", traceOrdinal, this);
      auto* device = new D3D9DeviceEx(
        this,
        adapter,
        DeviceType,
        hFocusWindow,
        BehaviorFlags,
        dxvkDevice);
      TraceD3D9Interface("device-ctor-end", traceOrdinal, device);

      TraceD3D9Interface("initial-reset-begin", traceOrdinal, device);
      hr = device->InitialReset(pPresentationParameters, pFullscreenDisplayMode);
      TraceD3D9Interface("initial-reset-end", traceOrdinal, device);

      if (unlikely(FAILED(hr))) {
        // D3D9DeviceEx 是 refCount==0 构造的 ComObject，此失败路径上尚未被任何
        // ref()/Com<> 持有；InitialReset 失败（交换链创建/显存不足等）若直接
        // return 会连同其 Rc<DxvkDevice>、adapter 引用整体泄漏。上游用
        // Com<D3D9DeviceEx> RAII 持有，本改写版丢了 RAII，这里显式补 delete。
        delete device;
        return hr;
      }

      *ppReturnedDeviceInterface = ref(device);
      
      // Install War3 Hooks immediately after device creation
      TraceD3D9Interface("install-hooks-begin", traceOrdinal, device);
      War3Hook::InstallHooks(device);
      TraceD3D9Interface("install-hooks-end", traceOrdinal, device);
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_NOTAVAILABLE;
    }

    TraceD3D9Interface("create-device-return", traceOrdinal, this);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterLUID(UINT Adapter, LUID* pLUID) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetAdapterLUID(pLUID);

    return D3DERR_INVALIDCALL;
  }


  HRESULT D3D9InterfaceEx::ValidatePresentationParametersEx(
    const D3DPRESENT_PARAMETERS* pPresentationParameters,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    // 空指针检查必须先于下方对 pPresentationParameters 的任何解引用。ResetEx
    // 直接把应用层传入的指针转发到这里；原顺序先解引用 ->Windowed，把这条判空
    // 变成永不可达的死代码，导致 NULL 实参直接崩溃而非返回 D3DERR_INVALIDCALL。
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    // pFullscreenDisplayMode must not be NULL in full screen mode.
    if (unlikely(!pPresentationParameters->Windowed && pFullscreenDisplayMode == nullptr))
      return D3DERR_INVALIDCALL;

    // pFullscreenDisplayMode must be NULL in windowed mode.
    if (unlikely(pPresentationParameters->Windowed && pFullscreenDisplayMode != nullptr))
      return D3DERR_INVALIDCALL;

    // On extended devices, the backbuffer dimensions
    // must match the display mode when in full screen mode.
    if (unlikely(!pPresentationParameters->Windowed &&
                  (pPresentationParameters->BackBufferWidth  != pFullscreenDisplayMode->Width
                || pPresentationParameters->BackBufferHeight != pFullscreenDisplayMode->Height)))
      return D3DERR_INVALIDCALL;

    return ValidatePresentationParameters(pPresentationParameters);
  }


  HRESULT D3D9InterfaceEx::ValidatePresentationParameters(
    const D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    if (m_extended) {
      // The swap effect value on a D3D9Ex device
      // can not be higher than D3DSWAPEFFECT_FLIPEX.
      if (unlikely(pPresentationParameters->SwapEffect > D3DSWAPEFFECT_FLIPEX))
        return D3DERR_INVALIDCALL;

      // 30 is the highest supported back buffer count for Ex devices.
      if (unlikely(pPresentationParameters->BackBufferCount > D3DPRESENT_BACK_BUFFERS_MAX_EX))
        return D3DERR_INVALIDCALL;
    } else {
      // The swap effect value on a non-Ex D3D9 device
      // can not be higher than D3DSWAPEFFECT_COPY.
      if (unlikely(pPresentationParameters->SwapEffect > D3DSWAPEFFECT_COPY))
        return D3DERR_INVALIDCALL;

      // 3 is the highest supported back buffer count for non-Ex devices.
      if (unlikely(pPresentationParameters->BackBufferCount > D3DPRESENT_BACK_BUFFERS_MAX))
        return D3DERR_INVALIDCALL;
    }

    // The swap effect value can not be 0.
    if (unlikely(!pPresentationParameters->SwapEffect))
      return D3DERR_INVALIDCALL;

    // D3DSWAPEFFECT_COPY can not be used with more than one back buffer.
    // Allow D3DSWAPEFFECT_COPY to bypass this restriction in D3D8 compatibility
    // mode, since it may be a remapping of D3DSWAPEFFECT_COPY_VSYNC and RC Cars
    // depends on it not being validated.
    if (unlikely(!IsD3D8Compatible()
              && pPresentationParameters->SwapEffect == D3DSWAPEFFECT_COPY
              && pPresentationParameters->BackBufferCount > 1))
      return D3DERR_INVALIDCALL;

    // Valid fullscreen presentation intervals must be known values.
    if (unlikely(!pPresentationParameters->Windowed
            && !(pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_ONE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_TWO
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_THREE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_FOUR
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)))
      return D3DERR_INVALIDCALL;

    // In windowed mode, only a subset of the presentation interval flags can be used.
    if (unlikely(pPresentationParameters->Windowed
            && !(pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_ONE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)))
      return D3DERR_INVALIDCALL;

    return D3D_OK;
  }


  const D3D9ON12_ARGS* D3D9InterfaceEx::Find9On12Args(
    const Rc<DxvkAdapter>& Adapter,
    const D3D9ON12_ARGS*   pOverrides,
          uint32_t         OverrideCount) {
    const D3D9ON12_ARGS* arg = nullptr;

#ifdef _WIN32
    for (uint32_t i = 0u; i < OverrideCount; i++) {
      if (pOverrides[i].pD3D12Device) {
        const auto& vk11 = Adapter->deviceProperties().vk11;

        if (vk11.deviceLUIDValid) {
          Com<ID3D12Device> device = nullptr;

          if (SUCCEEDED(pOverrides[i].pD3D12Device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) {
            LUID luid = device->GetAdapterLuid();

            if (!std::memcmp(&luid, vk11.deviceLUID, sizeof(vk11.deviceLUID)))
              arg = &pOverrides[i];
          }
        }
      } else if (!arg) {
        arg = &pOverrides[i];
      }
    }
#endif

    return arg;
  }

}
