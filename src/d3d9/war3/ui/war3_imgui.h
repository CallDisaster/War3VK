#pragma once

#include <windows.h>
#include <d3d9.h>
#include "imgui.h"
#include "war3_render_stats_view.h"
#include "../../d3d9_texture.h"
#include <atomic>
#include <cstdint>

namespace dxvk {
    class D3D9DeviceEx;
}

namespace dxvk::war3 {

    // The entry points of one generated DX9 renderer backend: war3_imgui_dx9.h
    // in process (war3_imgui_inprocess.cpp), war3_imgui_dx9_proxy.h in proxy
    // mode. Called through this table, the shared panel (war3_imgui.cpp) never
    // links against the in-process device's backend.
    struct ImGuiRenderer {
        bool (*init)(IDirect3DDevice9* device);
        void (*shutdown)();
        void (*newFrame)();
        void (*renderDrawData)(ImDrawData* drawData);
        bool (*createDeviceObjects)();
        void (*invalidateDeviceObjects)();
        void (*updateTexture)(ImTextureData* texture);
    };

    // Sections only the in-process device has (war3_imgui_inprocess.cpp sets
    // them in initialize). Through pointers, so the panel stays free of device
    // and effects code (the x86 data-layer cut); empty in proxy mode.
    struct DevicePanels {
        void (*passToggles)(D3D9DeviceEx* device) = nullptr;
        void (*shaderPack)() = nullptr;
    };

    class War3Imgui {
    public:
        static War3Imgui& get();

        // In process: the swap chain's device. initialize and setCursorBitmap
        // live in war3_imgui_inprocess.cpp.
        void initialize(HWND hwnd, D3D9DeviceEx* device);
        void beginDeviceTeardown(D3D9DeviceEx* owner);
        void shutdown(D3D9DeviceEx* owner);
        void invalidateDeviceObjects(D3D9DeviceEx* owner);
        void restoreDeviceObjects(D3D9DeviceEx* owner);
        bool isOwner(const D3D9DeviceEx* owner) const {
            return m_initialized && !m_retiring && m_device == owner;
        }
        bool ownsWindow(HWND hwnd) const {
            return m_initialized && !m_retiring && m_hwnd == hwnd;
        }

        // Proxy mode: the front end's device (war3/platform/war3_proxy_overlay.cpp),
        // on its owner thread. Created when the panel first opens and shut down
        // when it closes, so a hidden panel holds no font, atlas or buffer.
        bool initializeProxy(HWND hwnd, IDirect3DDevice9* device);
        void shutdownProxy(IDirect3DDevice9* device);
        // The game's Reset: every panel resource goes; the next frame makes them again.
        void invalidateProxy(IDirect3DDevice9* device);
        bool isProxyOwner(const IDirect3DDevice9* device) const {
            return m_initialized && !m_retiring && m_proxy && m_target == device;
        }

        void newFrame();
        void endFrame();
        void render(bool inScene = false);
        bool hasRenderedThisFrame() const { return m_hasRendered; }

        bool handleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        bool wantsCaptureMouse() const;
        bool wantsCaptureKeyboard() const;
        void toggleVisible();
        bool isVisible() const { return m_visible; }

        bool isInitialized() const { return m_initialized; }

        // What the panel has done, for the ui.panel_status test command.
        struct Status {
            bool initialized = false;
            bool visible = false;
            bool proxy = false;
            bool wantCaptureMouse = false;
            bool wantCaptureKeyboard = false;
            uint64_t framesDrawn = 0;      // RenderDrawData calls since initialization
            uint64_t framesSkipped = 0;    // proxy: frames whose atlas was not uploaded
            uint64_t textureFailures = 0;  // proxy: texture requests still pending after an upload
            uint32_t lastCommands = 0;     // draw commands of the last drawn frame
            uint32_t lastVertices = 0;
            uint32_t atlasWidth = 0;
            uint32_t atlasHeight = 0;
        };
        Status status() const;

        // Cursor Overlay Support
        void setCursorBitmap(int width, int height, const void* bgraData, int hotX, int hotY);

        // FPS 解锁控制
        static bool isFpsUnlocked() { return s_fpsUnlocked.load(std::memory_order_relaxed); }
        static void setFpsUnlocked(bool unlocked) { s_fpsUnlocked.store(unlocked, std::memory_order_relaxed); }

    private:
        War3Imgui() = default;
        ~War3Imgui() = default;

        // Shared by both modes: the context, the font, the platform and the
        // renderer backend. textureLimit 0 keeps ImGui's own atlas limit.
        void initializeContext(HWND hwnd, IDirect3DDevice9* target,
                               const ImGuiRenderer& renderer, int textureLimit);
        void retire();
        void destroyContext();
        bool uploadTextures(ImDrawData* drawData);

        void drawDebugWindow();
        void drawFrameRecorderPanel();
        void drawRenderStatsPanel();
        void drawCursorOverlay();

        bool m_initialized = false;
        bool m_retiring = false;
        ImGuiContext* m_context = nullptr;
        HWND m_hwnd = nullptr;
        bool m_hasRendered = false;
        bool m_frameStarted = false;
        bool m_visible = false;
        // In process only (null in proxy mode).
        D3D9DeviceEx* m_device = nullptr;
        // The device the panel draws through: the in-process device or the
        // front end's; never held (the owner keeps it alive).
        IDirect3DDevice9* m_target = nullptr;
        const ImGuiRenderer* m_renderer = nullptr;
        DevicePanels m_devicePanels;
        bool m_proxy = false;
        ui::RenderStatsRefresh m_renderStatsRefresh;
        Status m_counters;

        // Custom Cursor
        Com<D3D9Texture2D, false> m_cursorTexture;
        int m_cursorWidth = 0;
        int m_cursorHeight = 0;
        int m_cursorHotX = 0;
        int m_cursorHotY = 0;

        // FPS 解锁状态 (静态原子变量，全局共享)
        static std::atomic<bool> s_fpsUnlocked;
    };

}
