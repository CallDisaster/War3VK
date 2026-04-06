#pragma once

#include "d3d9_include.h"
#include "../dxvk/dxvk_include.h"
#include "../util/util_vector.h"
#include <vector>
#include <mutex>

namespace dxvk {

    struct War3PointLight {
        Vector4 position; // xyz = world pos, w = range
        Vector4 color;    // rgb = color, w = intensity
        Vector4 params;   // x = shadow_intensity (0..1), yzw = unused
        
        // internal id for updates
        int32_t id = 0;
        bool active = true;
    };

    // Uniform buffer layout alignment
    struct War3PointLightParams {
        Vector4 position; // xyz = pos, w = range
        Vector4 color;    // rgb = color, w = intensity
        Vector4 params;   // x = shadow_factor
        Vector4 padding;  // Pad to 64 bytes if needed, or just 48
    };

    class War3LightManager {
    public:
        static War3LightManager& Instance() {
            static War3LightManager s_instance;
            return s_instance;
        }

        int32_t AddPointLight(float x, float y, float z, float range, float r, float g, float b, float intensity, float shadowIntensity = 0.0f) {
            std::lock_guard<std::mutex> lock(m_mutex);
            int32_t id = ++m_nextId;
            
            War3PointLight light;
            light.position = Vector4(x, y, z, range);
            light.color = Vector4(r, g, b, intensity);
            light.params = Vector4(shadowIntensity, 0.0f, 0.0f, 0.0f);
            light.id = id;
            light.active = true;
            
            m_lights.push_back(light);
            return id;
        }

        bool UpdatePointLight(int32_t id, float x, float y, float z, float range, float r, float g, float b, float intensity) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& light : m_lights) {
                if (light.id == id && light.active) {
                    light.position = Vector4(x, y, z, range);
                    light.color = Vector4(r, g, b, intensity);
                    return true;
                }
            }
            return false;
        }

        bool RemovePointLight(int32_t id) {
             std::lock_guard<std::mutex> lock(m_mutex);
             for (auto it = m_lights.begin(); it != m_lights.end(); ++it) {
                 if (it->id == id) {
                     m_lights.erase(it);
                     return true;
                 }
             }
             return false;
        }

        void ClearLights() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lights.clear();
            m_hasTestLight = false;
        }
        
        // Create the test light requested by user
        void InitTestLight() {
             std::lock_guard<std::mutex> lock(m_mutex);
             if (m_hasTestLight || !m_lights.empty()) return;
             m_hasTestLight = true;
             
             // 测试点光源：世界坐标 (0,0,400)，用于验证阴影与光照
             War3PointLight light;
             light.position = Vector4(0.0f, 0.0f, 400.0f, 2000.0f);
             light.color = Vector4(1.0f, 0.95f, 0.85f, 3.0f);
             light.params = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
             light.id = ++m_nextId;
             light.active = true;
             m_lights.push_back(light);
        }

        std::vector<War3PointLight> GetActiveLights() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_lights;
        }

        bool HasActiveLights() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return !m_lights.empty();
        }

        uint32_t GetLightCount() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return static_cast<uint32_t>(m_lights.size());
        }

    private:
        War3LightManager() = default;
        
        mutable std::mutex m_mutex;
        std::vector<War3PointLight> m_lights;
        int32_t m_nextId = 0;
        bool m_hasTestLight = false;
    };

}
