#define NOMINMAX
#include "d3d9_war3_csm.h"
#include "d3d9_war3_debug.h"
#include "war3/render/war3_rts_shadow_stability_contract.h"

#include "../util/util_env.h"
#include "../util/util_math.h"

#include <cmath>
#include <limits>
#include <string>

namespace dxvk {

float War3CsmCalculator::dot3(const Vector4 &a, const Vector4 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector4 War3CsmCalculator::cross3(const Vector4 &a, const Vector4 &b) {
  return Vector4(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x, 0.0f);
}

Vector4 War3CsmCalculator::normalize3(Vector4 v) {
  const float len2 = dot3(v, v);
  if (len2 <= (std::numeric_limits<float>::min)())
    return v;
  const float invLen = 1.0f / std::sqrt(len2);
  v.x *= invLen;
  v.y *= invLen;
  v.z *= invLen;
  v.w = 0.0f;
  return v;
}

bool War3CsmCalculator::ExtractPerspectiveNearFar(const Matrix4 &proj,
                                                  float &outNear,
                                                  float &outFar) {
  // D3D9 perspective projection (row-major):
  // LH: m23 =  1, m33 = 0
  // RH: m23 = -1, m33 = 0
  const float m22 = proj[2][2];
  const float m23 = proj[2][3];
  const float m32 = proj[3][2];
  const float m33 = proj[3][3];

  // 仅支持透视投影（m23≈±1 且 m33≈0）
  // 注意：部分客户端会有轻微数值误差，这里使用略宽的阈值避免偶发“整帧无阴影”。
  constexpr float kEps = 1e-2f;
  if (std::abs(m33) > kEps)
    return false;

  if (std::abs(m22) <= (std::numeric_limits<float>::min)())
    return false;

  float zn = 0.0f;
  float zf = 0.0f;

  // LH
  if (std::abs(m23 - 1.0f) <= kEps) {
    // m22 = zf/(zf-zn), m32 = -zn*zf/(zf-zn)
    zn = -m32 / m22;
    const float denom = (m22 - 1.0f);
    if (std::abs(denom) <= (std::numeric_limits<float>::min)())
      return false;
    zf = (m22 * zn) / denom;
  }
  // RH
  else if (std::abs(m23 + 1.0f) <= kEps) {
    // m22 = zf/(zn-zf), m32 = zn*zf/(zn-zf)
    zn = m32 / m22;
    const float denom = (m22 + 1.0f);
    if (std::abs(denom) <= (std::numeric_limits<float>::min)())
      return false;
    zf = (m22 * zn) / denom;
  } else {
    return false;
  }

  if (!(zn > 0.0f) || !(zf > zn))
    return false;

  outNear = zn;
  outFar = zf;
  return true;
}

Matrix4 War3CsmCalculator::makeLookAtLH(const Vector4 &eye,
                                        const Vector4 &target,
                                        const Vector4 &up) {
  // Row-vector convention: p' = p * M
  // Columns contain basis vectors, last row contains translation.
  const Vector4 f = normalize3(
      Vector4(target.x - eye.x, target.y - eye.y, target.z - eye.z, 0.0f));
  Vector4 r = cross3(up, f);
  r = normalize3(r);
  Vector4 u = cross3(f, r);

  Matrix4 m;
  m[0] = Vector4(r.x, u.x, f.x, 0.0f);
  m[1] = Vector4(r.y, u.y, f.y, 0.0f);
  m[2] = Vector4(r.z, u.z, f.z, 0.0f);
  m[3] = Vector4(-dot3(eye, r), -dot3(eye, u), -dot3(eye, f), 1.0f);
  return m;
}

Matrix4 War3CsmCalculator::makeOrthoOffCenterLH(float left, float right,
                                                float bottom, float top,
                                                float nearZ, float farZ) {
  const float invW = 1.0f / (right - left);
  const float invH = 1.0f / (top - bottom);
  const float invD = 1.0f / (farZ - nearZ);

  Matrix4 m;
  m[0] = Vector4(2.0f * invW, 0.0f, 0.0f, 0.0f);
  m[1] = Vector4(0.0f, 2.0f * invH, 0.0f, 0.0f);
  m[2] = Vector4(0.0f, 0.0f, invD, 0.0f);
  m[3] = Vector4(-(right + left) * invW, -(top + bottom) * invH, -nearZ * invD,
                 1.0f);
  return m;
}

Vector4
War3CsmCalculator::selectStableWorldUp(const War3WorldCameraState &camera,
                                       const Vector4 &lightDir) const {
  // 通过 view 矩阵反推相机 up 向量在世界空间的朝向，在 Y-up 与 Z-up
  // 之间做选择。
  //
  // 关键约束（稳定优先）：
  // - worldUp 一旦在运行中“切换”，光空间 basis 会发生 roll 翻转，典型表现就是：
  //   - 阴影突然转 45°/90°，出现大面积方块抽搐；
  //   - 视角移动时阴影角度跟着变化（不应发生）。
  //
  // 因此这里采用“只初始化一次”的策略：
  // - 首次捕获到有效 world camera 时，根据得分选择 Y-up 或 Z-up；
  // - 后续整局固定，不再动态切换（避免抖动）。
  const Vector4 yUp = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
  const Vector4 zUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);

  Vector4 camUpWorld = yUp;
  {
    // view: world -> view；invView: view -> world
    const Matrix4 invView = inverse(camera.view);
    camUpWorld = invView * Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    camUpWorld = normalize3(camUpWorld);
  }

  const float yScore = std::abs(dot3(camUpWorld, yUp));
  const float zScore = std::abs(dot3(camUpWorld, zUp));

  // War3 world is typically Z-up.
  // Force Z-Up by default to match our new sun direction logic.
  // Values: "Z" (default), "Y", "AUTO".
  static int s_forcedUp = 1; // Default to Z (was -2)

  if (s_forcedUp == -2) {
    // ... logic removed for conciseness as we default to 1 now ...
    // But let's keep the env var check just in case but initialize to 1
    const std::string v = env::getEnvVar("DXVK_WAR3_SHADOW_WORLD_UP");
    if (!v.empty() && (v == "AUTO" || v == "auto"))
      s_forcedUp = -1;
    else if (!v.empty() && (v == "Y" || v == "y"))
      s_forcedUp = 0;
    else
      s_forcedUp = 1;
  }

  const bool preferY = (yScore >= zScore);
  Vector4 preferred = preferY ? yUp : zUp;

  // Enforce defaults
  if (s_forcedUp == 1)
    preferred = zUp;
  else if (s_forcedUp == 0)
    preferred = yUp;

  if (!m_worldUpInitialized) {
    m_worldUpInitialized = true;
    m_cachedWorldUp = preferred;
    WAR3_RENDER_LOG(
        "DXVK War3CSM: worldUp init=%s (yScore=%.3f zScore=%.3f forced=%d)\n",
        (std::abs(m_cachedWorldUp.y) > 0.5f) ? "Y" : "Z",
        static_cast<double>(yScore), static_cast<double>(zScore), s_forcedUp);
  }

  Vector4 up = m_cachedWorldUp;
  // 极端情况：光方向与 up 近共线时，cross(up, f) 会退化，给一个备选轴。
  if (std::abs(dot3(up, lightDir)) > 0.95f) {
    up = (std::abs(dot3(up, yUp)) > 0.5f) ? zUp : yUp;
  }
  return up;
}

War3CsmData War3CsmCalculator::Compute(const War3WorldCameraState &camera,
                                       const Vector4 &lightDirWorld,
                                       const War3CsmConfig &config) const {
  War3CsmData out;
  if (!camera.valid)
    return out;

  const uint32_t cascadeCount =
      std::min<uint32_t>(std::max<uint32_t>(config.cascadeCount, 1u), 4u);
  out.cascadeCount = cascadeCount;

  float camNear = 0.0f, camFar = 0.0f;
  if (!ExtractPerspectiveNearFar(camera.proj, camNear, camFar)) {
    // 非透视投影（或无法解析）时，不生成阴影数据
    out.cascadeCount = 0;
    return out;
  }

  const float maxFar =
      (std::min)(camFar, (std::max)(config.maxDistance, camNear + 1.0f));
  const float lambda = fclamp(config.splitLambda, 0.0f, 1.0f);

  std::array<float, 5> splits = {};
  splits[0] = camNear;
  for (uint32_t i = 1; i <= cascadeCount; i++) {
    const float p = float(i) / float(cascadeCount);
    const float logSplit = camNear * std::pow(maxFar / camNear, p);
    const float uniSplit = camNear + (maxFar - camNear) * p;
    splits[i] = uniSplit + (logSplit - uniSplit) * lambda;
  }

  const Matrix4 invProj = inverse(camera.proj);
  const Matrix4 invView = inverse(camera.view);

  // NDC corners for far plane (z=1). Y-up, X-right.
  const std::array<Vector4, 4> ndcCorners = {
      Vector4(-1.0f, -1.0f, 1.0f, 1.0f),
      Vector4(1.0f, -1.0f, 1.0f, 1.0f),
      Vector4(1.0f, 1.0f, 1.0f, 1.0f),
      Vector4(-1.0f, 1.0f, 1.0f, 1.0f),
  };

  // Light direction: ensure normalized and points "from sun to world".
  Vector4 lightDir0 = normalize3(lightDirWorld);
  if (dot3(lightDir0, lightDir0) <= (std::numeric_limits<float>::min)()) {
    // 容错：方向光为零时使用一个“俯视向下”的默认方向，避免出现极端长阴影。
    lightDir0 = Vector4(-0.3f, -0.2f, -1.0f, 0.0f);
    lightDir0 = normalize3(lightDir0);
  }

  // Stable up selection (cached, init-once) to avoid roll flipping.
  // 注意：up 的选择以相机矩阵为准（矩阵坐标系），而传入的 lightDirWorld 默认按
  // War3 Z-up 语义理解。
  const Vector4 up = selectStableWorldUp(camera, lightDir0);

  // 若传入方向与推断出来的 up 轴“几乎正交”，很可能是轴语义不一致（Y-up vs
  // Z-up）。 修正：日夜循环中低角度光照（日出日落）自然会导致 dot(light, up)
  // 很小， 这里的 axis-fix 会误判并强制交换轴，导致阴影方向跳变。 由于上游
  // (ShadowReceiver) 已经根据 worldUp 做了自适应输出，这里不再做猜测修正。
  Vector4 lightDir = lightDir0;
  /*
  {
      const Vector4 swapped = normalize3(Vector4(lightDir0.x, lightDir0.z,
  lightDir0.y, 0.0f)); const float s0 = std::abs(dot3(lightDir0, up)); const
  float s1 = std::abs(dot3(swapped, up)); if (s1 > s0 + 0.15f) { lightDir =
  swapped; static bool s_logged = false; if (!s_logged) { s_logged = true;
              WAR3_RENDER_LOG(
                  "DXVK War3CSM: sunDir axis-fix (swap Y/Z) up=%s
  old=(%.3f,%.3f,%.3f) new=(%.3f,%.3f,%.3f)\n", (std::abs(up.y) > 0.5f) ? "Y" :
  "Z", static_cast<double>(lightDir0.x), static_cast<double>(lightDir0.y),
                  static_cast<double>(lightDir0.z),
                  static_cast<double>(lightDir.x),
                  static_cast<double>(lightDir.y),
                  static_cast<double>(lightDir.z));
          }
      }
  }
  */

  // 统一方向：确保光线是“朝向地面”的（与 worldUp 夹角为钝角）。
  // 避免误把 direction 当成“从地面指向太阳”的向量时，阴影方向整体反转。
  if (dot3(lightDir, up) > 0.0f)
    lightDir = Vector4(-lightDir.x, -lightDir.y, -lightDir.z, 0.0f);

  out.lightDirection = lightDir;
  // selectStableWorldUp may temporarily choose an orthogonal helper when the
  // light is nearly collinear with the canonical axis. Height fog and other
  // consumers need the stable world axis, not that matrix-basis helper.
  out.worldUp = m_cachedWorldUp;

  // Precompute a stable light basis (world space)
  // 策略：
  // - 优先沿用上一帧 right 向量并投影到当前光方向的正交平面，保证跨帧连续；
  // - 退化时再用固定 helper axis 重建，避免“阈值切换”导致的 roll 抖动。
  const Vector4 f = lightDir;
  auto buildRightFromHelper = [&](const Vector4 &forward) {
    const float ax = std::abs(forward.x);
    const float ay = std::abs(forward.y);
    const float az = std::abs(forward.z);
    Vector4 helper = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    if (ax <= ay && ax <= az)
      helper = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    else if (ay <= az)
      helper = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    else
      helper = Vector4(0.0f, 0.0f, 1.0f, 0.0f);

    Vector4 right = normalize3(cross3(helper, forward));
    if (dot3(right, right) <= 1e-6f)
      right = normalize3(cross3(up, forward));
    return right;
  };

  Vector4 r = Vector4(0.0f);
  if (m_lightBasisInitialized) {
    const float proj = dot3(m_prevLightRight, f);
    r = Vector4(m_prevLightRight.x - f.x * proj,
                m_prevLightRight.y - f.y * proj,
                m_prevLightRight.z - f.z * proj, 0.0f);
    r = normalize3(r);
  }

  if (dot3(r, r) <= 1e-6f)
    r = buildRightFromHelper(f);

  Vector4 u = normalize3(cross3(f, r));
  if (dot3(u, u) <= 1e-6f) {
    r = buildRightFromHelper(f);
    u = normalize3(cross3(f, r));
  }

  // 防止基底在数值扰动下发生 180° 翻转（翻转会直接表现为阴影抖动/闪烁）。
  if (m_lightBasisInitialized && dot3(u, m_prevLightUp) < 0.0f) {
    r = Vector4(-r.x, -r.y, -r.z, 0.0f);
    u = Vector4(-u.x, -u.y, -u.z, 0.0f);
  }

  m_prevLightRight = r;
  m_prevLightUp = u;
  m_lightBasisInitialized = true;

  for (uint32_t c = 0; c < cascadeCount; c++) {
    const float splitNear = splits[c];
    const float splitFar = splits[c + 1];

    // Compute frustum corners in world space for this split
    std::array<Vector4, 8> viewCorners = {};
    std::array<Vector4, 8> worldCorners = {};
    for (uint32_t i = 0; i < 4; i++) {
      // Far corner in view space
      Vector4 v = invProj * ndcCorners[i];
      const float invW = (std::abs(v.w) > (std::numeric_limits<float>::min)())
                             ? 1.0f / v.w
                             : 1.0f;
      v.x *= invW;
      v.y *= invW;
      v.z *= invW;

      // Scale along ray to desired split depth
      // 注意：RH 视空间前方可能是 -Z，因此这里以 v.z 的符号决定 split 的符号。
      const float zFarSigned = v.z;
      const float zFarAbs = std::abs(zFarSigned);
      if (zFarAbs <= (std::numeric_limits<float>::min)()) {
        out.cascadeCount = 0;
        return out;
      }

      const float zSign = (zFarSigned >= 0.0f) ? 1.0f : -1.0f;
      const float sNear = splitNear / zFarAbs;
      const float sFar = splitFar / zFarAbs;

      Vector4 viewNear =
          Vector4(v.x * sNear, v.y * sNear, zSign * splitNear, 1.0f);
      Vector4 viewFar = Vector4(v.x * sFar, v.y * sFar, zSign * splitFar, 1.0f);

      viewCorners[i + 0] = viewNear;
      viewCorners[i + 4] = viewFar;
      worldCorners[i + 0] = invView * viewNear;
      worldCorners[i + 4] = invView * viewFar;
    }

    // 计算 frustum 中心与拟合范围：
    // - StableSphere：稳定优先，包围球拟合，旋转时更稳定但会浪费 XY 分辨率
    // - TightAabb：清晰优先，光空间 AABB
    // 拟合，能提升纹素密度但对旋转/缩放更敏感
    Vector4 centerAvg = Vector4(0.0f);
    for (const auto &p : worldCorners) {
      centerAvg.x += p.x;
      centerAvg.y += p.y;
      centerAvg.z += p.z;
    }
    centerAvg.x *= (1.0f / 8.0f);
    centerAvg.y *= (1.0f / 8.0f);
    centerAvg.z *= (1.0f / 8.0f);
    centerAvg.w = 1.0f;

    // StableSphere must not derive its radius from world-space corners. The
    // inverse-view transform adds camera translation before the subtraction
    // below, which loses low bits at large War3 world coordinates and makes a
    // theoretically constant radius breathe while the camera moves or rotates.
    // View-space corners depend only on the projection and cascade split, so
    // their sphere radius is invariant under camera motion.
    Vector4 centerView = Vector4(0.0f);
    for (const auto &p : viewCorners) {
      centerView.x += p.x;
      centerView.y += p.y;
      centerView.z += p.z;
    }
    centerView.x *= (1.0f / 8.0f);
    centerView.y *= (1.0f / 8.0f);
    centerView.z *= (1.0f / 8.0f);
    centerView.w = 1.0f;

    float radius3d = 0.0f;
    for (const auto &p : viewCorners) {
      const Vector4 d = Vector4(p.x - centerView.x, p.y - centerView.y,
                                p.z - centerView.z, 0.0f);
      radius3d =
          (std::max)(radius3d, std::sqrt((std::max)(dot3(d, d), 0.0f)));
    }
    radius3d = (std::max)(radius3d, 1.0f);

    // StableSphere should keep its center on the same precision path as its
    // radius. Averaging eight world-space corners first quantizes the camera
    // translation eight times; transforming the view-space center once is
    // mathematically equivalent but avoids that frame-dependent low-bit noise.
    Vector4 center = config.fitMode != War3CsmFitMode::TightAabb
                         ? invView * centerView
                         : centerAvg;
    center.w = 1.0f;
    float tightHalfExtent = 0.0f;
    if (config.fitMode == War3CsmFitMode::TightAabb) {
      float minX = (std::numeric_limits<float>::max)();
      float minY = (std::numeric_limits<float>::max)();
      float maxX = (std::numeric_limits<float>::lowest)();
      float maxY = (std::numeric_limits<float>::lowest)();
      for (const auto &p : worldCorners) {
        const float x = dot3(p, r);
        const float y = dot3(p, u);
        minX = (std::min)(minX, x);
        minY = (std::min)(minY, y);
        maxX = (std::max)(maxX, x);
        maxY = (std::max)(maxY, y);
      }

      const float cx = 0.5f * (minX + maxX);
      const float cy = 0.5f * (minY + maxY);
      const float halfX = 0.5f * (maxX - minX);
      const float halfY = 0.5f * (maxY - minY);
      tightHalfExtent = (std::max)(halfX, halfY);

      // Z 取平均中心在光方向上的投影，避免在切换 fitMode 时引入额外的 Z 漂移
      const float cz = dot3(centerAvg, f);
      center = Vector4(r.x * cx + u.x * cy + f.x * cz,
                       r.y * cx + u.y * cy + f.y * cz,
                       r.z * cx + u.z * cy + f.z * cz, 1.0f);
    }

    // XY 投影半径（对称正方形）
    // TightAabb 在低仰角时 tightHalfExtent 会因视锥 far 角极远而爆炸。
    // Guard：若 tightHalfExtent > radius3d * kTightAabbFallbackRatio，
    // 说明相机接近水平导致 AABB 退化，回退到包围球半径以避免矩阵爆炸。
    static constexpr float kTightAabbFallbackRatio = 3.0f;  // 可调；超过 3× sphere 即退化
    float radius = radius3d;
    if (config.fitMode == War3CsmFitMode::TightAabb) {
      if (tightHalfExtent <= radius3d * kTightAabbFallbackRatio) {
        radius = (std::max)(tightHalfExtent, 1.0f);
      }
      // else: 低仰角退化，保留 radius3d（包围球保底），不引入爆炸值
    }

    war3::render::War3RtsShadowReceiverBandFit rtsFit = {};
    if (config.fitMode == War3CsmFitMode::RtsReceiverBand) {
      war3::render::War3RtsShadowReceiverBandQuery query = {};
      for (uint32_t i = 0u; i < worldCorners.size(); i++) {
        query.frustumCorners[i] = {
            double(worldCorners[i].x), double(worldCorners[i].y),
            double(worldCorners[i].z)};
      }
      query.planeNormal = {double(out.worldUp.x), double(out.worldUp.y),
                           double(out.worldUp.z)};
      query.lightRight = {double(r.x), double(r.y), double(r.z)};
      query.lightUp = {double(u.x), double(u.y), double(u.z)};
      query.receiverPlaneHeight = double(config.rtsReceiverPlaneHeight);
      query.receiverBandHalfHeight =
          double(config.rtsReceiverBandHalfHeight);
      query.receiverPadding = double(config.rtsReceiverPadding);
      query.worldTexelSize = double(config.rtsBaseWorldTexelSize) *
          double(uint32_t(1u) << c);
      query.shadowResolution = (std::max)(config.shadowResolution, 1u);
      query.stableSnap = config.stableSnap > 0.5f;
      rtsFit = war3::render::War3FitRtsShadowReceiverBand(query);
      if (rtsFit.valid)
        radius = float(rtsFit.halfExtent);
    }

    const uint32_t shadowResU = (std::max)(config.shadowResolution, 1u);
    const float shadowRes = float(shadowResU);

    // Stable snapping: snap cascade center in light-rotated space to the texel
    // grid. Important: the snap grid must match the final orthographic extent,
    // otherwise the "snapped center" will still drift (visible as
    // shimmering/angle changes).

    // [Fix] 这里的 radiusPadded 量化逻辑：
    // 原逻辑是 radiusPadded = ceil(radiusPadded / texelSize) * texelSize，
    // 这是一个恒等式，因为 texelSize = 2*radiusPadded / shadowRes。
    // 真正的量化应该是为了让 texelSize 在相邻几帧内保持绝对一致。

    float radiusPadded = radius;
    if (rtsFit.valid) {
      // The clip radius and texel size are fixed by the development candidate.
      // The numeric contract already proves the snapped center still covers
      // the complete receiver band, so no breathing safety expansion is used.
      radiusPadded = radius;
    } else if (config.fitMode != War3CsmFitMode::TightAabb) {
      // 对于 StableSphere，我们通过量化半径来锁定 texelSize。
      // 步进值选为 16.0f (可以根据精度需求调整)，确保微小的旋转/移动不会导致
      // Radius 波动。
      const float step = 16.0f;
      radiusPadded = std::ceil(radiusPadded / step) * step;
    } else {
      // 对于 TightAabb，虽然它追求极致清晰，但过于敏感会导致闪烁。
      // 这里把步进从“接近连续”的 2.0 收粗到 8.0，避免墙体/竖直面在小幅
      // 俯仰变化时反复重拟合出不同的光空间范围，表现成阴影沿墙面波浪流动。
      const float step = 8.0f;
      radiusPadded = std::ceil(radiusPadded / step) * step;
    }

    float texelSize = (2.0f * radiusPadded) / shadowRes;
    const bool doSnap = (config.stableSnap > 0.5f) && (texelSize > 1e-6f);

    // Add a 1-texel safety margin to avoid edge clipping after snapping.
    if (doSnap && !rtsFit.valid) {
      radiusPadded = radiusPadded + texelSize;
      texelSize = (2.0f * radiusPadded) / shadowRes;
    }
    radius = radiusPadded;

    // Keep the snapped coordinates in light space all the way into the view
    // matrix. Reconstructing a world-space point and projecting it back with
    // float dot products loses the exact texel quantization at large world
    // coordinates.
    const auto projectCenter = [](const Vector4 &p, const Vector4 &axis) {
      return double(p.x) * double(axis.x) + double(p.y) * double(axis.y) +
             double(p.z) * double(axis.z);
    };
    double lightCenterX = rtsFit.valid
        ? rtsFit.centerLightX
        : projectCenter(center, r);
    double lightCenterY = rtsFit.valid
        ? rtsFit.centerLightY
        : projectCenter(center, u);
    const double lightCenterZ = projectCenter(center, f);
    if (doSnap) {
      // Round-to-nearest texel to minimize drift (handle negative coordinates
      // correctly)
      const double texelSizeD = double(texelSize);
      const double invTexel = 1.0 / texelSizeD;
      lightCenterX = std::round(lightCenterX * invTexel) * texelSizeD;
      lightCenterY = std::round(lightCenterY * invTexel) * texelSizeD;
    }

    // RenderEdge approach: place light camera at (center - lightDir *
    // depth_extent) Using -minExtents.z as the distance (matches RenderEdge's
    // calculation)
    //
    // 原先固定加大 Z 范围（+3000）会严重损失深度精度；逐帧紧拟合角点又会
    // 让深度尺度随相机旋转呼吸。下面改用稳定球加小 margin，在精度与稳定性间
    // 保持固定、保守的范围。
    const float depthMargin = (std::max)(config.depthRangeMargin, 0.0f);
    // Keep the light-space Z scale stable as well. A tight min/max fit of the
    // rotated frustum corners changes whenever the camera rotates, even though
    // the stable sphere itself has not changed. That alters normalized shadow
    // depth and receiver bias from frame to frame, showing up as fine edge
    // shimmer. A symmetric sphere bound is conservative and invariant.
    const float depthRadius = (std::max)(radius3d, radius);
    const float baseLightDistance = depthRadius + depthMargin;
    // Directional shadow casters can sit toward the sun, outside the receiver
    // frustum's symmetric Z sphere, while still projecting into it. C0/C1 are
    // intentionally unchanged; volume-enabled far cascades receive a fixed
    // one-sided allowance. Keeping it fixed avoids reintroducing per-frame
    // Z-scale breathing from caster-dependent fitting.
    // 体积光需要所有 cascade 都能包含上游 caster 的深度。
    // C0/C1 使用一半的扩展量，避免过度损失深度精度；
    // C2/C3 使用完整的 farCasterDepthExtension。
    const float farCasterDepthExtension = c >= 2u
        ? (std::max)(config.farCasterDepthExtension, 0.0f)
        : (std::max)(config.farCasterDepthExtension * 0.5f, 0.0f);
    const float lightDistance =
        baseLightDistance + farCasterDepthExtension;

    // Use the exact same stabilized basis that was used for texel snapping.
    // Write its light-space translation directly as well: this preserves the
    // snapped X/Y coordinates instead of round-tripping them through a large
    // world-space eye position.
    Matrix4 lightView;
    lightView[0] = Vector4(r.x, u.x, f.x, 0.0f);
    lightView[1] = Vector4(r.y, u.y, f.y, 0.0f);
    lightView[2] = Vector4(r.z, u.z, f.z, 0.0f);
    lightView[3] = Vector4(float(-lightCenterX), float(-lightCenterY),
                           float(double(lightDistance) - lightCenterZ), 1.0f);

    const float minZ = 0.0f;
    // Moving the eye toward the sun by E shifts both receiver bounds by E.
    // Adding E (rather than 2E) to the old symmetric far plane preserves the
    // far receiver boundary and opens only the upstream caster side.
    const float maxZ = (std::max)(
        2.0f * baseLightDistance + farCasterDepthExtension, 1.0f);

    // Symmetric XY bounds around the snapped center
    const float halfExtent = radius;
    const float left = -halfExtent;
    const float right = halfExtent;
    const float bottom = -halfExtent;
    const float top = halfExtent;
    const Matrix4 lightProj =
        makeOrthoOffCenterLH(left, right, bottom, top, minZ, maxZ);
    const Matrix4 lightViewProj = lightProj * lightView;

    out.cascades[c].lightViewProj = lightViewProj;
    out.cascades[c].splitNear = splitNear;
    out.cascades[c].splitFar = splitFar;
    out.cascades[c].snappedCenterLightSpace =
        Vector4(float(lightCenterX), float(lightCenterY),
                float(lightCenterZ), 1.0f);
    out.cascades[c].texelSize = texelSize;
  }

  return out;
}

War3VolumeSunOrtho ComputeVolumeSunOrtho(
    const Vector4& lightDirWorld,
    const Vector4& worldUpIn,
    const Vector4& centerWorld,
    float orthoRadius,
    float depthMargin,
    float depthExtension,
    uint32_t resolution,
    bool stableSnap) {
  War3VolumeSunOrtho out = {};
  if (!std::isfinite(orthoRadius) || orthoRadius <= 1.0f ||
      resolution == 0u || !std::isfinite(centerWorld.x) ||
      !std::isfinite(centerWorld.y) || !std::isfinite(centerWorld.z))
    return out;

  auto n3 = [](Vector4 v) {
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= (std::numeric_limits<float>::min)())
      return Vector4(0.0f, 0.0f, -1.0f, 0.0f);
    const float inv = 1.0f / std::sqrt(len2);
    return Vector4(v.x * inv, v.y * inv, v.z * inv, 0.0f);
  };
  auto d3 = [](const Vector4& a, const Vector4& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
  };
  auto x3 = [](const Vector4& a, const Vector4& b) {
    return Vector4(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                   a.x * b.y - a.y * b.x, 0.0f);
  };

  Vector4 lightDir = n3(lightDirWorld);
  Vector4 worldUp = n3(worldUpIn);
  if (d3(worldUp, worldUp) <= 1e-8f)
    worldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  // 与 CSM 一致：光方向朝向地面（与 up 夹角为钝角）
  if (d3(lightDir, worldUp) > 0.0f)
    lightDir = Vector4(-lightDir.x, -lightDir.y, -lightDir.z, 0.0f);

  const Vector4 f = lightDir;
  Vector4 helper = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
  {
    const float ax = std::abs(f.x);
    const float ay = std::abs(f.y);
    const float az = std::abs(f.z);
    if (ax <= ay && ax <= az)
      helper = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    else if (ay <= az)
      helper = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    else
      helper = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  }
  Vector4 r = n3(x3(helper, f));
  if (d3(r, r) <= 1e-8f)
    r = n3(x3(worldUp, f));
  Vector4 u = n3(x3(f, r));
  if (d3(u, u) <= 1e-8f) {
    r = n3(x3(worldUp, f));
    u = n3(x3(f, r));
  }

  // 固定半径：只量化，不随 pitch/FOV 膨胀。
  float radius = (std::max)(orthoRadius, 64.0f);
  constexpr float kRadiusStep = 64.0f;
  radius = std::ceil(radius / kRadiusStep) * kRadiusStep;

  const float resF = float((std::max)(resolution, 1u));
  float radiusPadded = radius;
  float texelSize = (2.0f * radiusPadded) / resF;
  if (stableSnap && texelSize > 1e-6f) {
    radiusPadded = radiusPadded + texelSize;
    texelSize = (2.0f * radiusPadded) / resF;
  }

  auto project = [&](const Vector4& p, const Vector4& axis) {
    return double(p.x) * double(axis.x) + double(p.y) * double(axis.y) +
           double(p.z) * double(axis.z);
  };
  Vector4 center = centerWorld;
  center.w = 1.0f;
  double lightCenterX = project(center, r);
  double lightCenterY = project(center, u);
  const double lightCenterZ = project(center, f);
  if (stableSnap && texelSize > 1e-6f) {
    const double texelSizeD = double(texelSize);
    const double invTexel = 1.0 / texelSizeD;
    lightCenterX = std::round(lightCenterX * invTexel) * texelSizeD;
    lightCenterY = std::round(lightCenterY * invTexel) * texelSizeD;
  }

  const float margin = (std::max)(depthMargin, 0.0f);
  const float extension = (std::max)(depthExtension, 0.0f);
  const float baseLightDistance = radiusPadded + margin;
  const float lightDistance = baseLightDistance + extension;
  const float minZ = 0.0f;
  const float maxZ =
      (std::max)(2.0f * baseLightDistance + extension, 1.0f);

  Matrix4 lightView;
  lightView[0] = Vector4(r.x, u.x, f.x, 0.0f);
  lightView[1] = Vector4(r.y, u.y, f.y, 0.0f);
  lightView[2] = Vector4(r.z, u.z, f.z, 0.0f);
  lightView[3] = Vector4(float(-lightCenterX), float(-lightCenterY),
                         float(double(lightDistance) - lightCenterZ), 1.0f);

  const float halfExtent = radiusPadded;
  // 复用 CSM 的 ortho 约定（row-vector）
  const float invW = 1.0f / (2.0f * halfExtent);
  const float invH = 1.0f / (2.0f * halfExtent);
  const float invD = 1.0f / (maxZ - minZ);
  Matrix4 lightProj;
  lightProj[0] = Vector4(2.0f * invW, 0.0f, 0.0f, 0.0f);
  lightProj[1] = Vector4(0.0f, 2.0f * invH, 0.0f, 0.0f);
  lightProj[2] = Vector4(0.0f, 0.0f, invD, 0.0f);
  lightProj[3] = Vector4(0.0f, 0.0f, -minZ * invD, 1.0f);

  out.lightViewProj = lightProj * lightView;
  out.lightDirection = lightDir;
  out.worldUp = worldUp;
  out.center = center;
  out.radius = radiusPadded;
  out.texelSize = texelSize;
  out.minZ = minZ;
  out.maxZ = maxZ;
  out.valid = true;
  return out;
}

} // namespace dxvk
