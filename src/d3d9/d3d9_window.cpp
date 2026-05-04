#include "d3d9_window.h"

#include "d3d9_swapchain.h"
#include "war3/hooks/war3_hook_lifecycle.h"
#include "war3/ui/war3_imgui.h"

namespace dxvk
{

#ifdef _WIN32
  struct D3D9WindowData {
    bool unicode;
    bool filter;
    bool activateProcessed;
    bool deactivateProcessed;
    WNDPROC proc;
    D3D9SwapChainEx* swapchain;
  };

  static dxvk::recursive_mutex g_windowProcMapMutex;
  static std::unordered_map<HWND, D3D9WindowData> g_windowProcMap;

  static bool IsImeMessage(UINT msg) {
    switch (msg) {
    case WM_IME_SETCONTEXT:
    case WM_IME_NOTIFY:
    case WM_IME_CONTROL:
    case WM_IME_COMPOSITIONFULL:
    case WM_IME_SELECT:
    case WM_IME_CHAR:
    case WM_IME_REQUEST:
    case WM_IME_KEYDOWN:
    case WM_IME_KEYUP:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_INPUTLANGCHANGE:
    case WM_INPUTLANGCHANGEREQUEST:
      return true;
    default:
      return false;
    }
  }

  D3D9WindowMessageFilter::D3D9WindowMessageFilter(HWND window, bool filter)
    : m_window(window) {
    std::lock_guard lock(g_windowProcMapMutex);
    auto it = g_windowProcMap.find(m_window);
    m_filter = std::exchange(it->second.filter, filter);
  }

  D3D9WindowMessageFilter::~D3D9WindowMessageFilter() {
    std::lock_guard lock(g_windowProcMapMutex);
    auto it = g_windowProcMap.find(m_window);
    it->second.filter = m_filter;
  }

  LRESULT CALLBACK D3D9WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    D3D9WindowData windowData = {};

    {
      std::lock_guard lock(g_windowProcMapMutex);

      auto it = g_windowProcMap.find(window);
      if (it != g_windowProcMap.end())
        windowData = it->second;
    }

    bool unicode = windowData.proc
      ? windowData.unicode
      : IsWindowUnicode(window);

    if (!windowData.proc || windowData.filter)
      return CallCharsetFunction(
        DefWindowProcW, DefWindowProcA, unicode,
          window, message, wparam, lparam);

    const bool isFullscreen =
        windowData.swapchain != nullptr &&
        !windowData.swapchain->GetPresentParams()->Windowed;

    if (!isFullscreen && message == WM_SYSCOMMAND) {
      switch (wparam & 0xFFF0) {
      case SC_MAXIMIZE:
      case SC_RESTORE:
        // War3 的 windowed syscommand 分支会在 maximize/restore 上直接退进程。
        // 这里交给 DefWindowProc 完成窗口状态切换，后续 WM_SIZE 仍会正常回给游戏。
        return CallCharsetFunction(
          DefWindowProcW, DefWindowProcA, unicode,
            window, message, wparam, lparam);
      default:
        break;
      }
    }

    if (message == WM_NCCALCSIZE && wparam == TRUE && isFullscreen)
      return 0;

    using namespace dxvk::war3;
    if (message == WM_KEYDOWN && wparam == VK_F1 &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
      if ((lparam & 0x40000000) == 0)
        War3Imgui::get().toggleVisible();
      return true;
    }

    if (IsImeMessage(message) || (message == WM_KEYDOWN && wparam == VK_PROCESSKEY)) {
      return CallCharsetFunction(
          CallWindowProcW, CallWindowProcA, unicode, windowData.proc, window,
          message, wparam, lparam);
    }

    const bool handled = War3Imgui::get().handleWndProc(window, message, wparam, lparam);
    const bool wantMouse = War3Imgui::get().wantsCaptureMouse();
    const bool wantKeyboard = War3Imgui::get().wantsCaptureKeyboard();

    const bool isMouseMsg =
        message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
        message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
        message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN ||
        message == WM_MBUTTONUP || message == WM_MBUTTONDBLCLK ||
        message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL ||
        message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
        message == WM_XBUTTONDBLCLK;

    const bool isKeyMsg =
        message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
        message == WM_CHAR || message == WM_SYSCHAR;

    if ((isMouseMsg && wantMouse) || (isKeyMsg && wantKeyboard) || handled)
      return true;

    
    D3D9DeviceEx* device = windowData.swapchain->GetParent();

    if (message == WM_DESTROY)
      ResetWindowProc(window);
    else if (message == WM_ACTIVATEAPP) {
      if ((wparam && !windowData.activateProcessed)
        || (!wparam && !windowData.deactivateProcessed)) {
        device->NotifyWindowActivated(window, wparam);
      }

      SetActivateProcessed(window, !!wparam);
    }
    else if (!isFullscreen && message == WM_SIZE) {
      // War3 自己的 windowed WM_SIZE 路径会直接带崩进程。
      // 这里继续走 DXVK 侧安全 reset，并额外同步一份已识别出的
      // War3 内部分辨率字段，避免只放大窗口外框导致黑块填充。
      if (wparam != SIZE_MINIMIZED) {
        RECT clientRect = { };
        if (::GetClientRect(window, &clientRect)) {
          const UINT clientWidth = clientRect.right - clientRect.left;
          const UINT clientHeight = clientRect.bottom - clientRect.top;

          if (clientWidth > 0 && clientHeight > 0) {
            const D3DPRESENT_PARAMETERS* currentParams =
              windowData.swapchain->GetPresentParams();
            const UINT previousWidth = currentParams->BackBufferWidth;
            const UINT previousHeight = currentParams->BackBufferHeight;
            if (previousWidth != clientWidth
             || previousHeight != clientHeight) {
              D3DPRESENT_PARAMETERS resetParams = *currentParams;
              resetParams.hDeviceWindow = window;
              resetParams.Windowed = TRUE;
              resetParams.BackBufferWidth = clientWidth;
              resetParams.BackBufferHeight = clientHeight;

              D3D9WindowMessageFilter filter(window);
              const HRESULT hr = device->ResetSwapChain(&resetParams, nullptr);

              if (SUCCEEDED(hr)) {
                windowData.swapchain->SetWindowedLogicalSourceExtent(
                  VkExtent2D{clientWidth, clientHeight}, false);
                dxvk::war3::hooks::TryOverrideWindowedClientSize(
                  previousWidth, previousHeight,
                  clientWidth, clientHeight);
                dxvk::war3::hooks::TryNotifyWindowedUiSizeChanged(
                  window, clientWidth, clientHeight);
              } else {
                Logger::warn(str::format(
                  "D3D9WindowProc: windowed WM_SIZE reset failed hr=0x",
                  std::hex, uint32_t(hr), std::dec,
                  " size=", clientWidth, "x", clientHeight));
              }
            } else {
              windowData.swapchain->SetWindowedLogicalSourceExtent(
                VkExtent2D{clientWidth, clientHeight}, false);
              dxvk::war3::hooks::TryOverrideWindowedClientSize(
                previousWidth, previousHeight,
                clientWidth, clientHeight);
              dxvk::war3::hooks::TryNotifyWindowedUiSizeChanged(
                window, clientWidth, clientHeight);
            }
          }
        }
      }

      return CallCharsetFunction(
        DefWindowProcW, DefWindowProcA, unicode,
          window, message, wparam, lparam);
    }
    return CallCharsetFunction(
      CallWindowProcW, CallWindowProcA, unicode,
        windowData.proc, window, message, wparam, lparam);
  }

  void ResetWindowProc(HWND window) {
    std::lock_guard lock(g_windowProcMapMutex);

    auto it = g_windowProcMap.find(window);
    if (it == g_windowProcMap.end())
      return;

    auto proc = reinterpret_cast<WNDPROC>(
      CallCharsetFunction(
      GetWindowLongPtrW, GetWindowLongPtrA, it->second.unicode,
        window, GWLP_WNDPROC));


    if (proc == D3D9WindowProc)
      CallCharsetFunction(
        SetWindowLongPtrW, SetWindowLongPtrA, it->second.unicode,
          window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.proc));

    g_windowProcMap.erase(window);
  }


  void HookWindowProc(HWND window, D3D9SwapChainEx* swapchain) {
    std::lock_guard lock(g_windowProcMapMutex);

    ResetWindowProc(window);

    D3D9WindowData windowData;
    windowData.unicode = IsWindowUnicode(window);
    windowData.filter  = false;
    windowData.activateProcessed = false;
    windowData.deactivateProcessed = false;
    windowData.proc = reinterpret_cast<WNDPROC>(
      CallCharsetFunction(
      SetWindowLongPtrW, SetWindowLongPtrA, windowData.unicode,
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(D3D9WindowProc)));
    windowData.swapchain = swapchain;

    g_windowProcMap[window] = std::move(windowData);
  }

  void SetActivateProcessed(HWND window, bool processed)
  {
      std::lock_guard lock(g_windowProcMapMutex);
      auto it = g_windowProcMap.find(window);
      if (it != g_windowProcMap.end()) {
        it->second.activateProcessed = processed;
        it->second.deactivateProcessed = !processed;
      }
  }
#else
  D3D9WindowMessageFilter::D3D9WindowMessageFilter(HWND window, bool filter) {

  }

  D3D9WindowMessageFilter::~D3D9WindowMessageFilter() {

  }

  void ResetWindowProc(HWND window) {

  }

  void HookWindowProc(HWND window, D3D9SwapChainEx* swapchain) {

  }

  void SetActivateProcessed(HWND window, bool processed) {
  }
#endif

}
