#pragma once

#include "d3d9_war3_scene.h"

#include "../util/util_vector.h"

#include <array>
#include <cstdint>

namespace dxvk {

    enum class War3CsmFitMode : uint8_t {
        StableSphere = 0, // 稳定优先：使用包围球拟合（更稳定但可能浪费分辨率）
        TightAabb    = 1, // 清晰优先：使用光空间 AABB 拟合（更清晰但旋转/缩放更敏感）
    };

    struct War3CsmConfig {
        uint32_t cascadeCount = 4;
        // Release clarity baseline. Runtime work must not bounce this between
        // 4096 and 2048; allocation/budget fallback is session-latched.
        uint32_t shadowResolution = 4096;
        float splitLambda = 0.5f;
        // Phase 7.31 Iteration D：从 8000 降到 4000。War3 的 RTS 相机俯角下
        // 远景很少超过 3000 个单位，8000 会让每个 cascade 覆盖太大范围、
        // 远景 texel 稀释造成"阴影边缘糊"。4000 配 splitLambda=0.5 近三级
        // 可以覆盖到 1500~1800，视觉上锐度显著提升。
        float maxDistance = 4000.0f;

        float stableSnap = 1.0f;        // 1 = 开启 texel snapping，0 = 关闭
        float depthRangeMargin = 50.0f; // 额外扩展 Z 范围，减少裁剪
        // Optional one-sided, toward-sun caster allowance for far cascades.
        // Unlike depthRangeMargin this does not also move the far receiver
        // boundary, so volume-enabled C2/C3 can retain upstream occluders with
        // the smallest possible normalized-depth precision cost.
        float farCasterDepthExtension = 0.0f;
        War3CsmFitMode fitMode = War3CsmFitMode::StableSphere;
    };

    struct War3CsmCascade {
        Matrix4 lightViewProj;
        float splitNear = 0.0f; // view-space z
        float splitFar = 0.0f;  // view-space z
        // Diagnostic contract for non-TAA jitter analysis. This is the exact
        // light-space center after optional texel snapping and the final texel
        // size used by the orthographic projection.
        Vector4 snappedCenterLightSpace =
            Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        float texelSize = 0.0f;
    };

    struct War3CsmData {
        uint32_t cascadeCount = 0;
        std::array<War3CsmCascade, 4> cascades = { };
        // Canonical basis actually used by Compute after normalization and the
        // toward-ground sign fix. Consumers must not independently reconstruct
        // these from mutable settings or a separate world-up heuristic.
        Vector4 lightDirection = Vector4(0.0f, 0.0f, -1.0f, 0.0f);
        Vector4 worldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    };

    /**
     * @brief 体积光专用太阳 ortho 投影（非相机视锥 cascade）。
     * @note 半径/中心不随 pitch 变形；用于体积柱遮挡，不进表面 receiver。
     */
    struct War3VolumeSunOrtho {
        Matrix4 lightViewProj = {};
        Vector4 lightDirection = Vector4(0.0f, 0.0f, -1.0f, 0.0f);
        Vector4 worldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
        Vector4 center = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        float radius = 0.0f;
        float texelSize = 0.0f;
        float minZ = 0.0f;
        float maxZ = 1.0f;
        bool valid = false;
    };

    /**
     * @brief 计算稳定的体积太阳 ortho 矩阵。
     * @param lightDirWorld 从太阳指向地面的方向（与 CSM 同源语义）
     * @param worldUp 稳定世界上方向
     * @param centerWorld 覆盖中心（通常为相机世界位置）
     * @param orthoRadius 水平半边长（世界单位）
     * @param depthMargin 对称深度 margin
     * @param depthExtension 仅向太阳侧的 caster 余量
     * @param resolution 阴影图分辨率
     * @param stableSnap 是否做光空间 texel snap
     */
    War3VolumeSunOrtho ComputeVolumeSunOrtho(
        const Vector4& lightDirWorld,
        const Vector4& worldUp,
        const Vector4& centerWorld,
        float orthoRadius,
        float depthMargin,
        float depthExtension,
        uint32_t resolution,
        bool stableSnap);

    class War3CsmCalculator {
    public:
        War3CsmData Compute(const War3WorldCameraState& camera,
                            const Vector4& lightDirWorld,
                            const War3CsmConfig& config) const;

        static bool ExtractPerspectiveNearFar(const Matrix4& proj, float& outNear, float& outFar);

    private:
        static Vector4 normalize3(Vector4 v);
        static Vector4 cross3(const Vector4& a, const Vector4& b);
        static float dot3(const Vector4& a, const Vector4& b);

        static Matrix4 makeLookAtLH(const Vector4& eye, const Vector4& target, const Vector4& up);
        static Matrix4 makeOrthoOffCenterLH(float left, float right, float bottom, float top, float nearZ, float farZ);

        Vector4 selectStableWorldUp(const War3WorldCameraState& camera, const Vector4& lightDir) const;

        // 说明（稳定优先）：
        // - War3 在“游戏逻辑坐标”里通常是 Z-up，但传入 D3D9 的矩阵不一定仍是 Z-up（可能已做轴变换）。
        // - 如果 worldUp 选择错误，会导致：
        //   - 阴影角度像日落（向下分量太小，阴影过长）
        //   - 视角移动/旋转时阴影出现 45° 方块/抽搐（光空间 roll 不稳定）
        //
        // 因此这里做“只初始化一次”的 worldUp 推断：
        // - 通过 view 矩阵反推相机 up 向量在世界空间的朝向，在 Y-up 与 Z-up 间取更可信的一项；
        // - 一旦初始化后整局固定，避免运行中切换导致的 roll 翻转与阴影抽搐；
        // - 缓存在 m_cachedWorldUp 中，用于后续所有帧的 CSM 计算。
        mutable bool    m_worldUpInitialized = false;
        mutable Vector4 m_cachedWorldUp      = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
        // 稳定光空间基底（跨帧连续化，避免太阳高角度附近的 roll 抖动/翻转）。
        mutable bool    m_lightBasisInitialized = false;
        mutable Vector4 m_prevLightRight        = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
        mutable Vector4 m_prevLightUp           = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    };

} // namespace dxvk
