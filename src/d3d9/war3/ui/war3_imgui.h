#pragma once

#include <windows.h>
#include <d3d9.h>
#include "imgui.h"
#include "war3_render_stats_view.h"
#include "../../d3d9_texture.h"
#include <atomic>

namespace dxvk {
    class D3D9DeviceEx;
}

namespace dxvk::war3 {

    class War3Imgui {
    public:
        static War3Imgui& get();

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

        // Cursor Overlay Support
        void setCursorBitmap(int width, int height, const void* bgraData, int hotX, int hotY);

        // FPS 解锁控制
        static bool isFpsUnlocked() { return s_fpsUnlocked.load(std::memory_order_relaxed); }
        static void setFpsUnlocked(bool unlocked) { s_fpsUnlocked.store(unlocked, std::memory_order_relaxed); }

    private:
        War3Imgui() = default;
        ~War3Imgui() = default;

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
        D3D9DeviceEx* m_device = nullptr;
        ui::RenderStatsRefresh m_renderStatsRefresh;

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
