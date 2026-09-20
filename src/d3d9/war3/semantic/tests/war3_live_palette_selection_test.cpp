// M2-1 live palette selection 迁移：新模块的宿主机等价测试（differential）。
//
// 两部分组成：
//   (1) 逐条差分等价：把**迁移前** d3d9_device.cpp 的实现（legacy 参考，见
//       war3_live_palette_selection_legacy_reference.inc，由入库生成器
//       AutoTest/gen_war3_live_palette_selection_legacy_reference.py 从 pre-M2
//       工作树快照逐字节抽取）与迁出后的模块实现放在同一输入域上对比，要求
//       返回值 / 输出参数逐位相同。输入域 = 枚举小域 + 固定种子随机域：
//         - War3SemanticHashMatrixPalette：nullptr/0/上界 + 特殊浮点位模式 +
//           100,000 组随机 palette；
//         - War3DecodeRuntimePoseMatrix48：120,000 组随机 48 字节，逐 16 个分量
//           位级对照 + 解码后 palette 哈希往返对照；
//         - War3TryReadRuntimePoseArray：stub CModel 内存（+0x5C/+0x60）上的
//           系统网格（count 0/1/1024/1025/溢出 x 模型可读长度 x pose 数组可读
//           长度）+ 40,000 随机场景；
//         - War3ResolveLivePoseRuntimeAlias：stub runtimeModel 内存上 ±0xA0
//           三个候选位的有效/失效/多有效顺序 + nullptr/<0x10000/未注册 +
//           40,000 随机场景；
//         - 4 个 env getter：主运行在当前 env 下双侧对照；
//   M2-2 扩展（同一文件，加法式）：选择链本体
//   War3TryBuildLiveRuntimeGroupPalette 的逐条差分 —— legacy 参考见
//   war3_live_palette_selection_chain_legacy_reference.inc（生成器
//   --m2-2 模式产出，与 M2-1 .inc 共享 m2_legacy_reference 命名空间）：
//         - 55 个系统场景：早退/proven hint/合同 ON·OFF 的 owned hit·miss
//           fail-closed/slot 直读/part snapshot（hash=0/tag=0 变体）/global
//           （null 指针/不可读/无 Game.dll）/blended（超界/欠数/尺寸不符）/
//           4096 缓存 confirm·三类 reject/epoch 翻转/8192 lookup 碰撞/
//           published pose（直中/±0xA0/owner 反查/hash=0）/CModel alias
//           （deny/allow/count 越界/pose 不可读/模型不可读）/walk-through
//           （published+cmodel 与 5 类无效变体）/5 条 fallback 各自隔离；
//         - 20,000 轮固定种子随机世界（8 个 part 指针复用以驱动缓存
//           confirm/reject、epoch 偶发翻转、全表随机布点）；
//         - 比较：返回值/全部输出参数/palette 逐 float-word/timing.calls[]
//           逐相/两计数器增量/skin::Selection 全字段；QPC ticks 不比较；
//   (2b) 选择链的 --probe-chain 模式：10 个固定场景，由等价门禁在多组
//       env（含 DXVK_WAR3_SKIN_PALETTE_CONTRACT=1）下驱动，双侧对照 +
//       独立期望值表。
//   (2) 运行时配置 getter 的 --probe 模式：由
//       AutoTest/test_war3_live_palette_selection_equivalence_static.py 在不同
//       env 下驱动，同时对比模块实现与 legacy 参考实现（同一进程、同一 env），
//       并对照独立登记的期望值表。
//
// 链接说明：本可执行文件链接真实的 war3_live_palette_selection.cpp，并为
// 设备层原语提供**宿主机替身**（下面的 "host stubs"）：可读内存范围表
// （IsReadableRangeFast/IsReadableRange）、env 读取（dxvk::env::getEnvVar）。
// 另外 sem::War3GetEnvU32 由本文件提供与 M1 模块**逐字节相同**的替身定义：
// War3GetEnvU32 属 M1（不在本轮 8 个迁移符号内），不链接 M1 模块 .cpp（那会
// 拖入整套 M1 替身世界）；legacy 侧的 .inc 内嵌了从 M1 模块逐字节抽取的同一
// 定义，两侧调用目标文本相同，probe 场景再以独立期望值表端到端校验该链。
// legacy 参考与模块实现**共用同一份替身**，因此差分隔离出的正是 M2-1 搬动的
// 那部分逻辑。
//
// 边界（未覆盖，如实声明）：
//   - 替身不是生产实现：本测试证明"同一输入 + 同一替身世界下两个实现一致"，
//     不证明内存读取/env 原语本身正确；
//   - 函数内 static 的 env getter 在一进程内只读一次 env，因此单个进程只能覆盖
//     一组 env 分支；驱动脚本用多组 env 多次运行覆盖回退分支；
//   - 未覆盖 device.cpp 侧的调用点编排（taxonomy 发射留在 device.cpp）；
//   - 不覆盖 GPU、Vulkan、实机画面与性能。

#include "../war3_live_palette_selection.h"
#include "../war3_palette_taxonomy_emission.h"
#include "../war3_palette_submitted_aggregation.h"
#include "../war3_device_semantic_predicates.h"
#include "../../core/war3_memory.h"
#include "../../core/war3_game_structs.h"
// M2-2：选择链本体依赖的真实声明头（stub 只提供 12 个外部符号的定义；
// 类型/内联原语与生产同源）。宿主编译可行性已实测（语法探针通过）。
#include "../../model/war3_model_hook.h"
#include "../../model/war3_model_registry.h"
#include "../../model/war3_model_resource_cache.h"
#include "../../shadow/war3_shadow_renderer_core.h"
#include "../../render/war3_skin_palette_selection.h"
#include "../../render/war3_current_draw_contract.h"
#include "../../render/war3_visible_renderables.h"
#include "../../war3.h"
#include "../../../../util/util_env.h"
// M2-3：motion 诊断三函数的参数类型 War3ShadowCaptureStats 是**共享真实
// 契约类型**（与模块 .cpp 同一份 d3d9_war3_scene.h 定义），两侧不各自造
// 副本；只有模块 .h 里是前向声明。
#include "../../../d3d9_war3_scene.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <windows.h>

// 迁移前实现（verbatim，自动生成；见 .inc 头部 provenance）。
// 它自带全局 using-directive 与 namespace m2_legacy_reference。
#include "war3_live_palette_selection_legacy_reference.inc"

// M2-2 选择链本体的迁移前实现（verbatim，自动生成；见 .inc 头部
// provenance）。与上方 M2-1 .inc 共享 m2_legacy_reference 命名空间：
// 链内无限定的 M2-1 helper/env getter 调用按 .inc 头部改写规则解析到
// M2-1 legacy 副本；因此本 include 必须位于 M2-1 .inc 之后。
#include "war3_live_palette_selection_chain_legacy_reference.inc"

// M2-3 motion / churn 诊断三函数的迁移前实现（verbatim，自动生成；见 .inc
// 头部 provenance）。仍与上面两份 .inc 共享 m2_legacy_reference 命名空间：
// 三个函数内无限定的 War3SemanticPaletteDiagnosticsRuntime() 调用按 .inc
// 头部改写规则解析到 M2-1 legacy 副本；因此本 include 必须位于 M2-1 .inc
// 之后。
#include "war3_live_palette_selection_motion_legacy_reference.inc"
#include "war3_palette_taxonomy_emission_legacy_reference.inc"
#include "war3_live_palette_selection_m2_4_legacy_reference.inc"

// A9 死代码裁定的迁移前实现（verbatim，自动生成；见 .inc 头部 provenance）。
// 它**必须**位于 M2-4 .inc 之后：正文内无限定的 War3SemanticPaletteLooksModelLocal
// 调用（A4 compose-policy，调用链 = A4 -> A3/A6/A7/A8）按 .inc 头部改写规则解析到
// M2-4 legacy 副本，与 pre-A9 device.cpp 经 using-directive 的真实调用目标一致。
#include "war3_live_palette_selection_a9_legacy_reference.inc"

// M2-5B B4 聚合块的迁移前实现（verbatim，自动生成；见 .inc 头部 provenance）。
// 与上面各 .inc 共享 m2_legacy_reference 命名空间；块内无限定的 bit::fnv1a_iter
// 经 .inc 头部的 using namespace dxvk; 解析，与 pre-M2-5B device.cpp 一致。
#include "war3_palette_submitted_aggregation_legacy_reference.inc"

namespace sem = dxvk::war3::semantic;
namespace legacy = m2_legacy_reference;

namespace {

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;

void Check(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expr << "\n";
  }
}

void DiffU64(const char* what, uint64_t a, uint64_t b, int line) {
  ++g_checks;
  if (a != b) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what << ": module=" << a
              << " legacy=" << b << "\n";
  }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)
#define DIFF_U64(what, a, b) DiffU64(what, uint64_t(a), uint64_t(b), __LINE__)

// 固定种子 splitmix64：差分输入域确定性可复现。
uint64_t g_rngState = 0x9E3779B97F4A7C15ull;

uint32_t NextU32() {
  g_rngState += 0x9E3779B97F4A7C15ull;
  uint64_t z = g_rngState;
  z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
  z = z ^ (z >> 31u);
  return uint32_t(z >> 32u);
}

} // namespace

// ===========================================================================
// host stubs：被测模块在 device 内依赖的原语（宿主机替身）
// ===========================================================================
namespace {

struct ReadableBlock {
  const void* base = nullptr;
  size_t size = 0u;
};

std::vector<ReadableBlock> g_readable;

void ClearReadable() { g_readable.clear(); }

void RegisterReadable(const void* base, size_t size) {
  g_readable.push_back(ReadableBlock{base, size});
}

bool RangeIsReadable(const void* p, size_t size) {
  if (p == nullptr || size == 0u)
    return false;
  const uintptr_t address = reinterpret_cast<uintptr_t>(p);
  for (const auto& block : g_readable) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(block.base);
    if (address < base || size > block.size)
      continue;
    if (address - base <= block.size - size)
      return true;
  }
  return false;
}

} // namespace

namespace dxvk::env {

std::string getEnvVar(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

} // namespace dxvk::env

namespace dxvk::war3 {

bool IsReadableRangeFast(const void* p, size_t size) {
  return RangeIsReadable(p, size);
}

bool IsReadableRange(const void* p, size_t size) {
  return RangeIsReadable(p, size);
}

} // namespace dxvk::war3

namespace dxvk::war3::semantic {

// 与 M1 模块 war3_device_semantic_predicates.cpp 中的定义逐字节相同的替身
// （见文件头部"链接说明"）。
uint32_t War3GetEnvU32(const char *name, uint32_t fallback) {
  const std::string v = env::getEnvVar(name);
  if (v.empty())
    return fallback;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(v.c_str(), &end, 0);
  if (end == v.c_str())
    return fallback;
  return static_cast<uint32_t>(parsed);
}

} // namespace dxvk::war3::semantic

// ===========================================================================
// 差分输入构造
// ===========================================================================
namespace {

// 特殊浮点位模式：+0/-0/±Inf/qNaN/sNaN/最小与最大 denormal/最小正规格化/1.0。
const uint32_t kSpecialFloatBits[] = {
    0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0xFFA00000u, 0x00000001u, 0x007FFFFFu,
    0x00800000u, 0x7F7FFFFFu, 0x3F800000u, 0xBF800000u,
    0xFFFFFFFFu, 0x00000010u,
};

float BitsToFloat(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void FillMatrixRandom(dxvk::Matrix4& matrix, bool specialHeavy) {
  for (uint32_t r = 0u; r < 4u; ++r) {
    uint32_t bits[4];
    for (uint32_t c = 0u; c < 4u; ++c) {
      const uint32_t roll = NextU32();
      if (specialHeavy && (roll % 3u) == 0u)
        bits[c] = kSpecialFloatBits[(roll >> 8u) % 14u];
      else
        bits[c] = roll;
    }
    matrix[r].x = BitsToFloat(bits[0]);
    matrix[r].y = BitsToFloat(bits[1]);
    matrix[r].z = BitsToFloat(bits[2]);
    matrix[r].w = BitsToFloat(bits[3]);
  }
}

void DiffHashPair(const dxvk::Matrix4* matrices, uint32_t count, int line) {
  const uint64_t moduleHash = sem::War3SemanticHashMatrixPalette(matrices, count);
  const uint64_t legacyHash =
      legacy::War3SemanticHashMatrixPalette(matrices, count);
  DiffU64("HashMatrixPalette", moduleHash, legacyHash, line);
}

#define DIFF_HASH(matrices, count) DiffHashPair(matrices, count, __LINE__)

void DiffHashMatrixPalette() {
  // 空/上界形态（注意：matrices==nullptr 时 count 仍参与哈希）。
  DIFF_HASH(nullptr, 0u);
  DIFF_HASH(nullptr, 1u);
  DIFF_HASH(nullptr, 1024u);
  DIFF_HASH(nullptr, 0xFFFFFFFFu);

  std::array<dxvk::Matrix4, 16> palette = {};
  DIFF_HASH(palette.data(), 0u);

  // 特殊位模式小域：size 1..16，模式随位置旋转。
  for (uint32_t size = 1u; size <= 16u; ++size) {
    for (uint32_t i = 0u; i < size; ++i)
      FillMatrixRandom(palette[i], true);
    DIFF_HASH(palette.data(), size);
  }
  // 纯特殊位模式确定性网格：每个位置取第 (i+size) 个特殊值。
  for (uint32_t size = 1u; size <= 16u; ++size) {
    for (uint32_t i = 0u; i < size; ++i) {
      for (uint32_t r = 0u; r < 4u; ++r) {
        palette[i][r].x = BitsToFloat(kSpecialFloatBits[(i + r + size) % 14u]);
        palette[i][r].y = BitsToFloat(kSpecialFloatBits[(i * 3u + r) % 14u]);
        palette[i][r].z = BitsToFloat(kSpecialFloatBits[(i + r * 5u) % 14u]);
        palette[i][r].w = BitsToFloat(kSpecialFloatBits[(i * 7u + r) % 14u]);
      }
    }
    DIFF_HASH(palette.data(), size);
  }

  // 随机大域。
  for (uint32_t iter = 0u; iter < 100000u; ++iter) {
    const uint32_t size = 1u + NextU32() % 16u;
    for (uint32_t i = 0u; i < size; ++i)
      FillMatrixRandom(palette[i], (NextU32() % 10u) == 0u);
    DIFF_HASH(palette.data(), size);
  }
}

void DiffDecodePair(const uint8_t* poseBytes, int line) {
  const dxvk::Matrix4 moduleMatrix =
      sem::War3DecodeRuntimePoseMatrix48(poseBytes);
  const dxvk::Matrix4 legacyMatrix =
      legacy::War3DecodeRuntimePoseMatrix48(poseBytes);
  for (uint32_t r = 0u; r < 4u; ++r) {
    DiffU64("Decode48.x", FloatBits(moduleMatrix[r].x),
            FloatBits(legacyMatrix[r].x), line);
    DiffU64("Decode48.y", FloatBits(moduleMatrix[r].y),
            FloatBits(legacyMatrix[r].y), line);
    DiffU64("Decode48.z", FloatBits(moduleMatrix[r].z),
            FloatBits(legacyMatrix[r].z), line);
    DiffU64("Decode48.w", FloatBits(moduleMatrix[r].w),
            FloatBits(legacyMatrix[r].w), line);
  }
  // 解码->哈希往返：解码结果再经 palette 哈希，两侧交叉对照。
  const uint64_t moduleHash =
      sem::War3SemanticHashMatrixPalette(&moduleMatrix, 1u);
  const uint64_t legacyHash =
      legacy::War3SemanticHashMatrixPalette(&legacyMatrix, 1u);
  DiffU64("Decode48.roundtripHash", moduleHash, legacyHash, line);
}

#define DIFF_DECODE(bytes) DiffDecodePair(bytes, __LINE__)

void DiffDecodeRuntimePoseMatrix48() {
  std::array<uint8_t, 48> bytes = {};
  DIFF_DECODE(bytes.data());  // 全 0
  bytes.fill(0xFFu);
  DIFF_DECODE(bytes.data());  // 全 1
  for (uint32_t i = 0u; i < 48u; ++i)
    bytes[i] = uint8_t(i);
  DIFF_DECODE(bytes.data());  // 递增
  for (uint32_t i = 0u; i < 48u; ++i)
    bytes[i] = uint8_t(0x80u + i);
  DIFF_DECODE(bytes.data());

  for (uint32_t iter = 0u; iter < 120000u; ++iter) {
    for (uint32_t i = 0u; i < 48u; i += 4u) {
      const uint32_t roll = NextU32();
      bytes[i] = uint8_t(roll);
      bytes[i + 1u] = uint8_t(roll >> 8u);
      bytes[i + 2u] = uint8_t(roll >> 16u);
      bytes[i + 3u] = uint8_t(roll >> 24u);
    }
    DIFF_DECODE(bytes.data());
  }
}

// ---------------------------------------------------------------------------
// stub CModel / runtimeModel 世界
// ---------------------------------------------------------------------------
// 模型缓冲区：alias 解析以 P = g_modelBuf.data() + 0xA0 为 runtimeModel，
// 覆盖 [P-0xA0, P+0xA0+0x70)。
std::vector<uint8_t> g_modelBuf(0x400u, uint8_t(0u));
std::vector<uint8_t> g_poseBufA(1024u * 48u + 64u, uint8_t(0u));
std::vector<uint8_t> g_poseBufB(1024u * 48u + 64u, uint8_t(0u));
std::vector<uint8_t> g_poseBufC(1024u * 48u + 64u, uint8_t(0u));

void WriteCModelFields(void* model, uint32_t count, void* poseArrayPtr) {
  std::memcpy(static_cast<uint8_t*>(model) +
                  dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
              &count, sizeof(count));
  std::memcpy(static_cast<uint8_t*>(model) +
                  dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
              &poseArrayPtr, sizeof(poseArrayPtr));
}

void DiffPoseArrayPair(void* runtimeModelPtr, int line) {
  uint32_t moduleCount = 0xDEADBEEFu;
  uint32_t legacyCount = 0xDEADBEEFu;
  void* moduleArray = reinterpret_cast<void*>(uintptr_t(0x1u));
  void* legacyArray = reinterpret_cast<void*>(uintptr_t(0x1u));
  const bool moduleOk = sem::War3TryReadRuntimePoseArray(
      runtimeModelPtr, moduleCount, moduleArray);
  const bool legacyOk = legacy::War3TryReadRuntimePoseArray(
      runtimeModelPtr, legacyCount, legacyArray);
  DiffU64("TryReadPoseArray.ret", moduleOk ? 1u : 0u, legacyOk ? 1u : 0u,
          line);
  DiffU64("TryReadPoseArray.count", moduleCount, legacyCount, line);
  DiffU64("TryReadPoseArray.array",
          reinterpret_cast<uintptr_t>(moduleArray),
          reinterpret_cast<uintptr_t>(legacyArray), line);
}

#define DIFF_POSE_ARRAY(ptr) DiffPoseArrayPair(ptr, __LINE__)

void RunPoseArrayCase(bool registerModel, size_t modelReadableBytes,
                      uint32_t count, int poseMode, long long poseDelta) {
  // poseMode: 0=null, 1=registered(need+poseDelta 字节), 2=unregistered pointer
  ClearReadable();
  std::memset(g_modelBuf.data(), 0, g_modelBuf.size());
  void* model = g_modelBuf.data() + 0xA0;
  void* poseArray = nullptr;
  if (poseMode == 1) {
    poseArray = g_poseBufA.data();
    const long long need = static_cast<long long>(count) * 48ll;
    const long long readable = need + poseDelta;
    if (readable > 0) {
      size_t bytes = static_cast<size_t>(readable);
      if (bytes > g_poseBufA.size())
        bytes = g_poseBufA.size();  // 注册上限截断（被测代码从不解引用 pose 内容）
      RegisterReadable(poseArray, bytes);
    }
  } else if (poseMode == 2) {
    poseArray = g_poseBufA.data();  // 故意不注册
  }
  WriteCModelFields(model, count, poseArray);
  if (registerModel)
    RegisterReadable(model, modelReadableBytes);
  DIFF_POSE_ARRAY(model);
}

void DiffTryReadRuntimePoseArray() {
  DIFF_POSE_ARRAY(nullptr);

  const size_t modelBytes[] = {0u, 0x5Cu, 0x60u, 0x63u, 0x64u, 0x68u, 0x70u};
  const uint32_t counts[] = {0u,  1u,   2u,   8u,   255u,
                             256u, 1023u, 1024u, 1025u, 4096u,
                             0xFFFFFFFFu};
  // 系统网格：模型可读长度 x count x pose 形态。
  for (size_t mb : modelBytes) {
    for (uint32_t count : counts) {
      RunPoseArrayCase(mb > 0u, mb, count, 0, 0);  // null pose ptr
      RunPoseArrayCase(mb > 0u, mb, count, 1, 0);  // 精确可读
      RunPoseArrayCase(mb > 0u, mb, count, 2, 0);  // 未注册 pose
    }
  }
  // pose 可读长度围绕 count*48 的边界（短 1 / 短 8 / 精确 / 长 16）。
  for (uint32_t count : counts) {
    RunPoseArrayCase(true, 0x70u, count, 1, -1);
    RunPoseArrayCase(true, 0x70u, count, 1, -8);
    RunPoseArrayCase(true, 0x70u, count, 1, 16);
  }

  // 随机大域。
  for (uint32_t iter = 0u; iter < 40000u; ++iter) {
    const size_t mb = modelBytes[NextU32() % 7u];
    const uint32_t countRoll = NextU32();
    uint32_t count;
    if ((countRoll % 16u) == 0u)
      count = counts[(countRoll >> 4u) % 11u];
    else
      count = countRoll % 1100u;  // 跨 1024 边界
    const int poseMode = int(NextU32() % 3u);
    const long long deltas[] = {0, -1, -8, 16, 48};
    const long long delta = deltas[NextU32() % 5u];
    RunPoseArrayCase(mb > 0u, mb, count, poseMode, delta);
  }
}

void DiffAliasPair(void* runtimeModelPtr, int line) {
  uint32_t moduleCount = 0xDEADBEEFu;
  uint32_t legacyCount = 0xDEADBEEFu;
  void* moduleArray = reinterpret_cast<void*>(uintptr_t(1u));
  void* legacyArray = reinterpret_cast<void*>(uintptr_t(1u));
  void* moduleResolved = sem::War3ResolveLivePoseRuntimeAlias(
      runtimeModelPtr, moduleCount, moduleArray);
  void* legacyResolved = legacy::War3ResolveLivePoseRuntimeAlias(
      runtimeModelPtr, legacyCount, legacyArray);
  DiffU64("ResolveAlias.resolved",
          reinterpret_cast<uintptr_t>(moduleResolved),
          reinterpret_cast<uintptr_t>(legacyResolved), line);
  DiffU64("ResolveAlias.count", moduleCount, legacyCount, line);
  DiffU64("ResolveAlias.array",
          reinterpret_cast<uintptr_t>(moduleArray),
          reinterpret_cast<uintptr_t>(legacyArray), line);
}

#define DIFF_ALIAS(ptr) DiffAliasPair(ptr, __LINE__)

// 在候选位掩码 mask（bit0=P+0xA0, bit1=P, bit2=P-0xA0，即代码里的候选顺序）
// 写入有效 CModel 字段；poseArrayOk=false 时候选的 pose 数组不可读（候选失效）。
void RunAliasCase(bool registerModel, uint32_t validMask, uint32_t countBase,
                  bool poseArrayOk) {
  ClearReadable();
  std::memset(g_modelBuf.data(), 0, g_modelBuf.size());
  uint8_t* base = g_modelBuf.data();
  void* P = base + 0xA0;
  void* candidates[3] = {
      static_cast<void*>(base + 0xA0 + 0xA0),  // value + 0xA0
      P,                                       // value 本身
      static_cast<void*>(base)};               // value - 0xA0
  void* poseArrays[3] = {g_poseBufA.data(), g_poseBufB.data(),
                         g_poseBufC.data()};
  for (uint32_t i = 0u; i < 3u; ++i) {
    if ((validMask & (1u << i)) == 0u)
      continue;
    const uint32_t count = countBase + i;
    WriteCModelFields(candidates[i], count, poseArrays[i]);
    if (poseArrayOk) {
      size_t bytes = size_t(count) * 48u;
      if (bytes > g_poseBufA.size())
        bytes = g_poseBufA.size();  // 注册上限截断（被测代码从不解引用 pose 内容）
      RegisterReadable(poseArrays[i], bytes);
    }
  }
  if (registerModel)
    RegisterReadable(base, g_modelBuf.size());
  DIFF_ALIAS(P);
}

void DiffResolveLivePoseRuntimeAlias() {
  DIFF_ALIAS(nullptr);
  DIFF_ALIAS(reinterpret_cast<void*>(uintptr_t(0x8000u)));   // < 0x10000
  DIFF_ALIAS(reinterpret_cast<void*>(uintptr_t(0x10000u)));  // 未注册

  // 单候选有效：全部三个位置 x 若干 count。
  const uint32_t counts[] = {1u, 7u, 64u, 1023u, 1024u};
  for (uint32_t pos = 0u; pos < 3u; ++pos) {
    for (uint32_t count : counts)
      RunAliasCase(true, 1u << pos, count, true);
  }
  // 全无效 / 未注册模型。
  RunAliasCase(true, 0u, 1u, true);
  RunAliasCase(false, 7u, 1u, true);
  // 多候选有效：候选顺序（+0xA0 优先于自身，自身优先于 -0xA0）。
  RunAliasCase(true, 3u, 10u, true);   // +0xA0 与自身
  RunAliasCase(true, 6u, 20u, true);   // 自身与 -0xA0
  RunAliasCase(true, 7u, 30u, true);   // 三个全有效
  RunAliasCase(true, 5u, 40u, true);   // +0xA0 与 -0xA0
  // pose 数组不可读使高优先级候选失效，落到低优先级。
  RunAliasCase(true, 7u, 50u, false);
  RunAliasCase(true, 6u, 60u, false);
  RunAliasCase(true, 3u, 70u, false);

  // 随机大域。
  for (uint32_t iter = 0u; iter < 40000u; ++iter) {
    const uint32_t mask = NextU32() % 8u;
    const uint32_t countBase = 1u + NextU32() % 1024u;
    const bool poseOk = (NextU32() % 4u) != 0u;
    const bool registerModel = (NextU32() % 8u) != 0u;
    RunAliasCase(registerModel, mask, countBase, poseOk);
  }
}

void DiffEnvGettersCurrentEnv() {
  DIFF_U64("LivePaletteSafeCopyRuntime",
           sem::War3SemanticLivePaletteSafeCopyRuntime() ? 1u : 0u,
           legacy::War3SemanticLivePaletteSafeCopyRuntime() ? 1u : 0u);
  DIFF_U64("LivePaletteRefreshRuntime",
           sem::War3SemanticLivePaletteRefreshRuntime() ? 1u : 0u,
           legacy::War3SemanticLivePaletteRefreshRuntime() ? 1u : 0u);
  DIFF_U64("LivePaletteAllowCModelFallbackRuntime",
           sem::War3SemanticLivePaletteAllowCModelFallbackRuntime() ? 1u : 0u,
           legacy::War3SemanticLivePaletteAllowCModelFallbackRuntime()
               ? 1u
               : 0u);
  DIFF_U64("SemanticPaletteDiagnosticsRuntime",
           sem::War3SemanticPaletteDiagnosticsRuntime() ? 1u : 0u,
           legacy::War3SemanticPaletteDiagnosticsRuntime() ? 1u : 0u);
  DIFF_U64("SemanticPaletteInPlaceAppendRuntime",
           sem::War3SemanticPaletteInPlaceAppendRuntime() ? 1u : 0u,
           legacy::War3SemanticPaletteInPlaceAppendRuntime() ? 1u : 0u);
  DIFF_U64("SemanticDrawTimePoseRuntime",
           sem::War3SemanticDrawTimePoseRuntime() ? 1u : 0u,
           legacy::War3SemanticDrawTimePoseRuntime() ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// --probe：env getter 的模块实现 vs legacy 实现（同一进程、同一 env）
// ---------------------------------------------------------------------------
int RunProbe() {
  struct BoolProbe {
    const char* name;
    bool (*module)();
    bool (*legacy)();
  };
  const BoolProbe boolProbes[] = {
      {"War3SemanticLivePaletteSafeCopyRuntime",
       &sem::War3SemanticLivePaletteSafeCopyRuntime,
       &legacy::War3SemanticLivePaletteSafeCopyRuntime},
      {"War3SemanticLivePaletteRefreshRuntime",
       &sem::War3SemanticLivePaletteRefreshRuntime,
       &legacy::War3SemanticLivePaletteRefreshRuntime},
      {"War3SemanticLivePaletteAllowCModelFallbackRuntime",
       &sem::War3SemanticLivePaletteAllowCModelFallbackRuntime,
       &legacy::War3SemanticLivePaletteAllowCModelFallbackRuntime},
      {"War3SemanticPaletteDiagnosticsRuntime",
       &sem::War3SemanticPaletteDiagnosticsRuntime,
       &legacy::War3SemanticPaletteDiagnosticsRuntime},
      {"War3SemanticPaletteInPlaceAppendRuntime",
       &sem::War3SemanticPaletteInPlaceAppendRuntime,
       &legacy::War3SemanticPaletteInPlaceAppendRuntime},
      {"War3SemanticDrawTimePoseRuntime",
       &sem::War3SemanticDrawTimePoseRuntime,
       &legacy::War3SemanticDrawTimePoseRuntime},
  };
  for (const auto& probe : boolProbes) {
    std::cout << "probe " << probe.name << " module=" << (probe.module() ? 1 : 0)
              << " legacy=" << (probe.legacy() ? 1 : 0) << "\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
// M2-2：选择链本体差分 —— stub 世界 / 运行器 / 场景电池 / --probe-chain
// ---------------------------------------------------------------------------
// 口径（同文件头部"链接说明"）：模块侧与 legacy 侧共用同一份替身世界；
// 两侧函数内 static/thread_local（4096 项 slot 缓存、8192 项 lookup、
// cursor、globalPaletteSnapshot、s_posePalette）各自独立，但由**同一调用
// 序列**驱动，状态演化一致。差分比较返回值/全部输出参数/计数器增量/
// timing.calls[]；QPC ticks 不做数值比较。
// 12 个替身符号的定义在匿名命名空间之后（命名空间要求），状态全局量在此。

constexpr size_t kChainPhaseCount = sem::kWar3LivePaletteBuildPhaseCount;

// stub part 池（+0x08 = StagePresetSpanBaseIndex）。CollisionA/B 相距
// 131072 字节：(ptr>>4)&8191 相同 → 8192 项 lookup 直接映射碰撞，强制
// 回退到 4096 项线性扫描（两侧走法相同，结果必须一致）。
alignas(16) std::array<uint8_t, 0x40000u> g_chainPartArena = {};

void* ChainPart(uint32_t index) {
  return g_chainPartArena.data() + size_t(index) * 0x100u;
}
void* ChainPartCollisionA() { return g_chainPartArena.data() + 0x1000u; }
void* ChainPartCollisionB() {
  return g_chainPartArena.data() + 0x1000u + 131072u;
}

// stub CModel / FinalPose 缓冲（CModel 位于 buf+0xA0，字段 +0x5C/+0x60）。
std::vector<uint8_t> g_chainModelBufA(0x400u, uint8_t(0u));
std::vector<uint8_t> g_chainModelBufB(0x400u, uint8_t(0u));
std::vector<uint8_t> g_chainPoseBuf(1024u * 48u + 64u, uint8_t(0u));

void* ChainModelA() { return g_chainModelBufA.data() + 0xA0; }
void* ChainModelB() { return g_chainModelBufB.data() + 0xA0; }

// Game.dll 伪镜像：链只读 image+0xBC6BD0 处的全局 palette 指针。
// GetModuleHandleA("Game.dll") 在本测试进程确定性返回 NULL（未加载该 DLL）。
alignas(16) std::array<uint8_t, 0xBC6BD0u + 16u> g_stubGameDllImage = {};
alignas(16) std::array<uint8_t, 0x3A98u * 48u> g_stubGlobalPalette = {};

uintptr_t g_stubGameDllBase = 0u;
uint64_t g_stubMapEpoch = 1u;

// --- 12 个替身符号的可编程表 ---
struct StubPartBinding {
  bool queryOk = false;
  uint32_t slotIndex = 0xFFFFFFFFu;
  uint32_t groupCount = 0u;
  uint32_t frameTag = 0u;
};
std::map<void*, StubPartBinding> g_stubPartBindings;

struct StubPartSnapshot {
  bool queryOk = false;
  std::vector<dxvk::Matrix4> palette;
  uint64_t hash = 0u;
  uint32_t frameTag = 0u;
};
std::map<void*, StubPartSnapshot> g_stubPartSnapshots;

struct StubSlotPalette {
  bool queryOk = false;
  std::vector<dxvk::Matrix4> palette;
  uint32_t capturedCount = 0u;
};
std::map<uint32_t, StubSlotPalette> g_stubSlotPalettes;

struct StubFrameTagRange {
  bool queryOk = false;
  uint32_t minFrameTag = 0u;
  uint32_t maxFrameTag = 0u;
  uint32_t missingCount = 0u;
};
// key = (slotIndex << 32) | expectedCount
std::map<uint64_t, StubFrameTagRange> g_stubFrameTagRanges;

std::map<void*, dxvk::war3::model::PoseRecord> g_stubPoseRecords;

struct StubPartOwner {
  bool queryOk = false;
  void* ownerRuntimeModel = nullptr;
};
std::map<void*, StubPartOwner> g_stubPartOwners;

struct StubOwnedSnapshot {
  bool queryOk = false;
  std::vector<dxvk::Matrix4> palette;
  dxvk::war3::render::skin::Selection selection = {};
};
// key = (runtimeModel, renderablePart)
std::map<std::pair<void*, void*>, StubOwnedSnapshot> g_stubOwnedSnapshots;

void SetChainPartSlot(void* part, uint32_t slot) {
  std::memcpy(static_cast<uint8_t*>(part) +
                  dxvk::war3::RenderablePartFieldOffsets::StagePresetSpanBaseIndex,
              &slot, sizeof(slot));
}

// 轻量重置：只清随机电池会弄脏的字段（避免每轮 memset 大块静态缓冲）。
void ResetChainWorldLite() {
  ClearReadable();
  g_stubPartBindings.clear();
  g_stubPartSnapshots.clear();
  g_stubSlotPalettes.clear();
  g_stubFrameTagRanges.clear();
  g_stubPoseRecords.clear();
  g_stubPartOwners.clear();
  g_stubOwnedSnapshots.clear();
  g_stubGameDllBase = 0u;
  g_stubMapEpoch = 1u;
  const uint32_t invalid = 0xFFFFFFFFu;
  for (uint32_t i = 0u; i < 8u; ++i)
    SetChainPartSlot(ChainPart(i), invalid);
  SetChainPartSlot(ChainPartCollisionA(), invalid);
  SetChainPartSlot(ChainPartCollisionB(), invalid);
  std::memset(g_chainModelBufA.data() + 0xA0 + 0x5Cu, 0, 8u);
  std::memset(g_chainModelBufB.data() + 0xA0 + 0x5Cu, 0, 8u);
  std::memset(g_stubGameDllImage.data() + 0xBC6BD0u, 0, 8u);
}

// 完整重置：系统场景用（额外清零内容缓冲，确定性更直观）。
void ResetChainWorld() {
  ResetChainWorldLite();
  std::memset(g_chainPartArena.data(), 0, g_chainPartArena.size());
  std::memset(g_chainModelBufA.data(), 0, g_chainModelBufA.size());
  std::memset(g_chainModelBufB.data(), 0, g_chainModelBufB.size());
  std::memset(g_chainPoseBuf.data(), 0, g_chainPoseBuf.size());
  std::memset(g_stubGlobalPalette.data(), 0, g_stubGlobalPalette.size());
}

std::vector<uint8_t> ChainGroups(std::initializer_list<uint32_t> slots) {
  std::vector<uint8_t> out;
  for (uint32_t slot : slots)
    out.push_back(static_cast<uint8_t>(slot));
  return out;
}

std::vector<dxvk::Matrix4> MakeChainPalette(uint32_t count, uint32_t seed) {
  std::vector<dxvk::Matrix4> palette(count);
  for (uint32_t i = 0u; i < count; ++i) {
    for (uint32_t r = 0u; r < 4u; ++r) {
      palette[i][r].x = BitsToFloat(seed + i * 97u + r * 13u + 1u);
      palette[i][r].y = BitsToFloat(seed + i * 57u + r * 29u + 5u);
      palette[i][r].z = BitsToFloat(seed + i * 31u + r * 43u + 9u);
      palette[i][r].w = BitsToFloat(seed + i * 17u + r * 71u + 13u);
    }
  }
  return palette;
}

dxvk::war3::model::PoseRecord MakeChainPoseRecord(void* key, uint32_t count,
                                                  uint32_t seed,
                                                  uint64_t matrixHash) {
  dxvk::war3::model::PoseRecord record = {};
  record.runtimeModelPtr = key;
  record.matrixCount = count;
  record.matrixHash = matrixHash;
  record.matrixPalette = MakeChainPalette(count, seed);
  return record;
}

// 配置 Game.dll 伪镜像：image+0xBC6BD0 写入全局 palette 指针，并填充
// [slot*48, slot*48+required*48) 的确定字节；镜像与 palette 注册可读。
void SetupGlobalSlotPalette(uint32_t slot, uint32_t required, uint32_t seed) {
  g_stubGameDllBase =
      reinterpret_cast<uintptr_t>(g_stubGameDllImage.data());
  const void* palettePtr = g_stubGlobalPalette.data();
  std::memcpy(g_stubGameDllImage.data() + 0xBC6BD0u, &palettePtr,
              sizeof(palettePtr));
  for (uint32_t i = 0u; i < required * 48u; ++i)
    g_stubGlobalPalette[size_t(slot) * 48u + i] = uint8_t(seed + i);
  RegisterReadable(g_stubGameDllImage.data(), g_stubGameDllImage.size());
  RegisterReadable(g_stubGlobalPalette.data(), g_stubGlobalPalette.size());
}

// 配置 stub CModel：+0x5C=count、+0x60=pose 数组指针；模型缓冲注册可读，
// pose 数组按 registerPose 决定是否注册（alias 解析的可读性门）。
// 注意：count>1024 时 TryRead 在检查 pose 可读性之前即拒绝，因此写入与
// 注册都按缓冲容量截断（S39 的 count=2000 场景不会越界）。
void SetupChainCModel(std::vector<uint8_t>& modelBuf, uint32_t poseCount,
                      bool registerPose) {
  void* model = modelBuf.data() + 0xA0;
  WriteCModelFields(model, poseCount, g_chainPoseBuf.data());
  RegisterReadable(modelBuf.data(), modelBuf.size());
  if (registerPose) {
    const size_t capped =
        std::min(size_t(poseCount) * 48u, g_chainPoseBuf.size());
    RegisterReadable(g_chainPoseBuf.data(), capped);
    for (size_t i = 0u; i < capped; ++i)
      g_chainPoseBuf[i] = uint8_t(0x40u + i);
  }
}

// --- 差分运行器 ---
struct ChainInput {
  const std::vector<uint8_t>* vertexGroups = nullptr;
  const std::vector<uint32_t>* matrixGroupSizes = nullptr;
  const std::vector<uint32_t>* matrixIndices = nullptr;
  void* runtimeModel = nullptr;
  void* part = nullptr;
  uint64_t frameSerial = 1u;
  bool allowCModel = false;
  uint32_t provenMaxSlot = 0xFFFFFFFFu;
};

struct ChainOutcome {
  bool ok = false;
  uint32_t source = 0u;
  uint32_t maxSlot = 0u;
  uint64_t hash = 0u;
  uint64_t rawPoseHash = 0u;
  uintptr_t poseModel = 0u;
  uint32_t slotIndex = 0u;
  uint32_t minTag = 0u;
  uint32_t maxTag = 0u;
  std::vector<uint32_t> paletteBits;
  std::array<uint32_t, kChainPhaseCount> calls = {};
  uint64_t servedDelta = 0u;
  uint64_t rejectedDelta = 0u;
  // skin::Selection 全字段（非合同路径保持入口清零值）。
  uint32_t selSource = 0u;
  uint32_t selSpace = 0u;
  uint32_t selDomain = 0u;
  uintptr_t selModel = 0u;
  uintptr_t selPart = 0u;
  uintptr_t selMesh = 0u;
  uint64_t selOwnerEpoch = 0u;
  uint64_t selTicket = 0u;
  uint64_t selCapture = 0u;
  uint64_t selHash = 0u;
  uint64_t selSlotGen = 0u;
  uint32_t selSlot = 0u;
  uint32_t selGroups = 0u;
  uint32_t selTag = 0u;
};

void FlattenChainSelection(const dxvk::war3::render::skin::Selection& sel,
                           ChainOutcome& out) {
  out.selSource = static_cast<uint32_t>(sel.source);
  out.selSpace = static_cast<uint32_t>(sel.space);
  out.selDomain = static_cast<uint32_t>(sel.domain);
  out.selModel = sel.runtimeModel;
  out.selPart = sel.part;
  out.selMesh = sel.meshPayload;
  out.selOwnerEpoch = sel.ownerEpoch;
  out.selTicket = sel.publicationTicket;
  out.selCapture = sel.captureSerial;
  out.selHash = sel.hash;
  out.selSlotGen = sel.slotAllocationGeneration;
  out.selSlot = sel.slot;
  out.selGroups = sel.actualGroupCount;
  out.selTag = sel.frameTag;
}

void FlattenChainPalette(const std::vector<dxvk::Matrix4>& palette,
                         ChainOutcome& out) {
  out.paletteBits.reserve(palette.size() * 16u);
  for (const auto& matrix : palette) {
    for (uint32_t r = 0u; r < 4u; ++r) {
      out.paletteBits.push_back(FloatBits(matrix[r].x));
      out.paletteBits.push_back(FloatBits(matrix[r].y));
      out.paletteBits.push_back(FloatBits(matrix[r].z));
      out.paletteBits.push_back(FloatBits(matrix[r].w));
    }
  }
}

ChainOutcome RunChainModule(const ChainInput& in) {
  dxvk::war3::shadow::ShadowPacketResource resource;
  resource.vertexGroupIndices = in.vertexGroups;
  resource.matrixGroupSizes = in.matrixGroupSizes;
  resource.matrixIndices = in.matrixIndices;
  ChainOutcome out;
  std::vector<dxvk::Matrix4> palette;
  uint32_t maxSlot = 0u;
  uint64_t hash = 0u;
  uint64_t rawPoseHash = 0u;
  void* poseModel = nullptr;
  sem::War3SemanticPaletteSource source = sem::War3SemanticPaletteSource::None;
  uint32_t slotIndex = 0u;
  uint32_t minTag = 0u;
  uint32_t maxTag = 0u;
  sem::War3LivePaletteBuildTiming timing = {};
  dxvk::war3::render::skin::Selection selection;
  const uint64_t served0 =
      sem::g_devicePaletteSlotCacheServedAfterConfirmCount.load(
          std::memory_order_relaxed);
  const uint64_t rejected0 =
      sem::g_devicePaletteSlotCacheRejectedStaleCount.load(
          std::memory_order_relaxed);
  out.ok = sem::War3TryBuildLiveRuntimeGroupPalette(
      resource, in.runtimeModel, in.part, in.frameSerial, palette, maxSlot,
      hash, &rawPoseHash, &poseModel, in.allowCModel, &source, &slotIndex,
      &minTag, &maxTag, &timing, in.provenMaxSlot, &selection);
  out.source = static_cast<uint32_t>(source);
  out.maxSlot = maxSlot;
  out.hash = hash;
  out.rawPoseHash = rawPoseHash;
  out.poseModel = reinterpret_cast<uintptr_t>(poseModel);
  out.slotIndex = slotIndex;
  out.minTag = minTag;
  out.maxTag = maxTag;
  out.calls = timing.calls;
  out.servedDelta =
      sem::g_devicePaletteSlotCacheServedAfterConfirmCount.load(
          std::memory_order_relaxed) -
      served0;
  out.rejectedDelta =
      sem::g_devicePaletteSlotCacheRejectedStaleCount.load(
          std::memory_order_relaxed) -
      rejected0;
  FlattenChainSelection(selection, out);
  FlattenChainPalette(palette, out);
  return out;
}

ChainOutcome RunChainLegacy(const ChainInput& in) {
  dxvk::war3::shadow::ShadowPacketResource resource;
  resource.vertexGroupIndices = in.vertexGroups;
  resource.matrixGroupSizes = in.matrixGroupSizes;
  resource.matrixIndices = in.matrixIndices;
  ChainOutcome out;
  std::vector<dxvk::Matrix4> palette;
  uint32_t maxSlot = 0u;
  uint64_t hash = 0u;
  uint64_t rawPoseHash = 0u;
  void* poseModel = nullptr;
  legacy::War3SemanticPaletteSource source =
      legacy::War3SemanticPaletteSource::None;
  uint32_t slotIndex = 0u;
  uint32_t minTag = 0u;
  uint32_t maxTag = 0u;
  legacy::War3LivePaletteBuildTiming timing = {};
  dxvk::war3::render::skin::Selection selection;
  const uint64_t served0 =
      legacy::g_devicePaletteSlotCacheServedAfterConfirmCount.load(
          std::memory_order_relaxed);
  const uint64_t rejected0 =
      legacy::g_devicePaletteSlotCacheRejectedStaleCount.load(
          std::memory_order_relaxed);
  out.ok = legacy::War3TryBuildLiveRuntimeGroupPalette(
      resource, in.runtimeModel, in.part, in.frameSerial, palette, maxSlot,
      hash, &rawPoseHash, &poseModel, in.allowCModel, &source, &slotIndex,
      &minTag, &maxTag, &timing, in.provenMaxSlot, &selection);
  out.source = static_cast<uint32_t>(source);
  out.maxSlot = maxSlot;
  out.hash = hash;
  out.rawPoseHash = rawPoseHash;
  out.poseModel = reinterpret_cast<uintptr_t>(poseModel);
  out.slotIndex = slotIndex;
  out.minTag = minTag;
  out.maxTag = maxTag;
  out.calls = timing.calls;
  out.servedDelta =
      legacy::g_devicePaletteSlotCacheServedAfterConfirmCount.load(
          std::memory_order_relaxed) -
      served0;
  out.rejectedDelta =
      legacy::g_devicePaletteSlotCacheRejectedStaleCount.load(
          std::memory_order_relaxed) -
      rejected0;
  FlattenChainSelection(selection, out);
  FlattenChainPalette(palette, out);
  return out;
}

void DiffChainOutcomePair(const char* name, const ChainOutcome& moduleOutcome,
                          const ChainOutcome& legacyOutcome, int line) {
  const ChainOutcome& m = moduleOutcome;
  const ChainOutcome& l = legacyOutcome;
  DiffU64(name, m.ok ? 1u : 0u, l.ok ? 1u : 0u, line);
  DiffU64("chain.source", m.source, l.source, line);
  DiffU64("chain.maxSlot", m.maxSlot, l.maxSlot, line);
  DiffU64("chain.hash", m.hash, l.hash, line);
  DiffU64("chain.rawPoseHash", m.rawPoseHash, l.rawPoseHash, line);
  DiffU64("chain.poseModel", m.poseModel, l.poseModel, line);
  DiffU64("chain.slotIndex", m.slotIndex, l.slotIndex, line);
  DiffU64("chain.minTag", m.minTag, l.minTag, line);
  DiffU64("chain.maxTag", m.maxTag, l.maxTag, line);
  DiffU64("chain.servedDelta", m.servedDelta, l.servedDelta, line);
  DiffU64("chain.rejectedDelta", m.rejectedDelta, l.rejectedDelta, line);
  for (size_t i = 0u; i < kChainPhaseCount; ++i)
    DiffU64("chain.phaseCalls", m.calls[i], l.calls[i], line);
  DiffU64("chain.selSource", m.selSource, l.selSource, line);
  DiffU64("chain.selSpace", m.selSpace, l.selSpace, line);
  DiffU64("chain.selDomain", m.selDomain, l.selDomain, line);
  DiffU64("chain.selModel", m.selModel, l.selModel, line);
  DiffU64("chain.selPart", m.selPart, l.selPart, line);
  DiffU64("chain.selMesh", m.selMesh, l.selMesh, line);
  DiffU64("chain.selOwnerEpoch", m.selOwnerEpoch, l.selOwnerEpoch, line);
  DiffU64("chain.selTicket", m.selTicket, l.selTicket, line);
  DiffU64("chain.selCapture", m.selCapture, l.selCapture, line);
  DiffU64("chain.selHash", m.selHash, l.selHash, line);
  DiffU64("chain.selSlotGen", m.selSlotGen, l.selSlotGen, line);
  DiffU64("chain.selSlot", m.selSlot, l.selSlot, line);
  DiffU64("chain.selGroups", m.selGroups, l.selGroups, line);
  DiffU64("chain.selTag", m.selTag, l.selTag, line);
  DiffU64("chain.paletteWords", uint64_t(m.paletteBits.size()),
          uint64_t(l.paletteBits.size()), line);
  const size_t words = std::min(m.paletteBits.size(), l.paletteBits.size());
  for (size_t i = 0u; i < words; ++i)
    DiffU64("chain.paletteWord", m.paletteBits[i], l.paletteBits[i], line);
}

void RunDiffChain(const char* name, const ChainInput& in, int line) {
  const ChainOutcome moduleOutcome = RunChainModule(in);
  const ChainOutcome legacyOutcome = RunChainLegacy(in);
  DiffChainOutcomePair(name, moduleOutcome, legacyOutcome, line);
}

#define DIFF_CHAIN(name, input) RunDiffChain(name, input, __LINE__)

uint64_t g_chainFrameSerial = 100u;

// --- 系统场景电池 ---
void DiffChainSystematic() {
  std::vector<uint8_t> groups;
  std::vector<uint32_t> sizes;
  std::vector<uint32_t> indices;
  ChainInput in;
  in.vertexGroups = &groups;
  in.matrixGroupSizes = &sizes;
  in.matrixIndices = &indices;
  auto freshIn = [&]() {
    in.runtimeModel = nullptr;
    in.part = nullptr;
    in.allowCModel = false;
    in.provenMaxSlot = 0xFFFFFFFFu;
    in.frameSerial = ++g_chainFrameSerial;
  };

  // S1 早退：runtimeModel 与 part 均为 nullptr。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  sizes.clear();
  indices.clear();
  DIFF_CHAIN("S1-both-null", in);

  // S2 早退：vertexGroups 为空。
  ResetChainWorld();
  freshIn();
  groups.clear();
  in.runtimeModel = ChainModelA();
  DIFF_CHAIN("S2-empty-groups", in);

  // S3 proven hint <256：不进入 GroupScan；maxSlot=proven。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.provenMaxSlot = 5u;
  DIFF_CHAIN("S3-proven-hint", in);

  // S4 proven=0：required=1。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u});
  in.runtimeModel = ChainModelA();
  in.provenMaxSlot = 0u;
  DIFF_CHAIN("S4-proven-zero", in);

  // S5 owned snapshot 已发布：合同 ON 时命中 source=6；合同 OFF 时
  // owned 表被忽略，其余来源全 miss → false。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.part = ChainPart(3u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 6u);
  {
    StubOwnedSnapshot owned;
    owned.queryOk = true;
    owned.palette = MakeChainPalette(3u, 500u);
    auto& sel = owned.selection;
    sel.source = dxvk::war3::render::skin::Source::OwnedPartSnapshot;
    sel.space = dxvk::war3::render::skin::Space::World;
    sel.domain = dxvk::war3::render::skin::Domain::VertexGroups;
    sel.runtimeModel = reinterpret_cast<uintptr_t>(in.runtimeModel);
    sel.part = reinterpret_cast<uintptr_t>(in.part);
    sel.ownerEpoch = 7u;
    sel.publicationTicket = 9u;
    sel.captureSerial = 11u;
    sel.hash = 0x777u;
    sel.slot = 42u;
    sel.actualGroupCount = 3u;
    sel.frameTag = 77u;
    g_stubOwnedSnapshots[std::make_pair(in.runtimeModel, in.part)] = owned;
  }
  DIFF_CHAIN("S5-owned-hit", in);

  // S6 owned miss（合同 ON fail-closed，不得 fallthrough 到 slot 路径）：
  // 合同 OFF 时同一世界走 slot snapshot 命中。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.part = ChainPart(4u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 8u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 600u);
    snapshot.hash = 0x600u;
    snapshot.frameTag = 6u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S6-owned-miss-fail-closed", in);

  // S7 slot 直读 + part snapshot 命中（hash/tag 均非零）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 7u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 700u);
    snapshot.hash = 0xABCDu;
    snapshot.frameTag = 55u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S7-slot-direct", in);

  // S8 part snapshot 命中但 producer hash=0 → outHash 重算。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 7u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 800u);
    snapshot.hash = 0u;
    snapshot.frameTag = 55u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S8-snapshot-hash-zero", in);

  // S9 part snapshot 命中但 frameTag=0 → lazy FrameTagQuery（ready）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 7u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 900u);
    snapshot.hash = 0x900u;
    snapshot.frameTag = 0u;
    g_stubPartSnapshots[in.part] = snapshot;
    StubFrameTagRange range;
    range.queryOk = true;
    range.minFrameTag = 10u;
    range.maxFrameTag = 12u;
    g_stubFrameTagRanges[(uint64_t(7u) << 32u) | 3u] = range;
  }
  DIFF_CHAIN("S9-snapshot-tag-zero-range-ready", in);

  // S10 同 S9 但 frame tag range 未 ready → min/max 保持 0。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 7u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 1000u);
    snapshot.hash = 0x1000u;
    snapshot.frameTag = 0u;
    g_stubPartSnapshots[in.part] = snapshot;
    StubFrameTagRange range;
    range.queryOk = false;
    g_stubFrameTagRanges[(uint64_t(7u) << 32u) | 3u] = range;
  }
  DIFF_CHAIN("S10-snapshot-tag-zero-range-not-ready", in);

  // S11 part snapshot queryOk 但 palette 为空 → 落到 global。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 3u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;  // 空 palette：链检查 !outPalette.empty()
    g_stubPartSnapshots[in.part] = snapshot;
  }
  SetupGlobalSlotPalette(3u, 3u, 200u);
  DIFF_CHAIN("S11-snapshot-empty-to-global", in);

  // S12 part snapshot miss → global（SAFE_COPY 默认 ON；flipped env 走
  // GlobalRangeCheck，同一电池两侧仍一致）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 3u);
  SetupGlobalSlotPalette(3u, 3u, 210u);
  DIFF_CHAIN("S12-global", in);

  // S13 global 指针为 null → blended。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  g_stubGameDllBase =
      reinterpret_cast<uintptr_t>(g_stubGameDllImage.data());
  RegisterReadable(g_stubGameDllImage.data(), g_stubGameDllImage.size());
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 1300u);
    g_stubSlotPalettes[4u] = slotPalette;
  }
  DIFF_CHAIN("S13-global-null-ptr-to-blended", in);

  // S14 global palette 缓冲不可读（SafeCopy/RangeCheck 均失败）→ blended。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  g_stubGameDllBase =
      reinterpret_cast<uintptr_t>(g_stubGameDllImage.data());
  RegisterReadable(g_stubGameDllImage.data(), g_stubGameDllImage.size());
  {
    const void* palettePtr = g_stubGlobalPalette.data();
    std::memcpy(g_stubGameDllImage.data() + 0xBC6BD0u, &palettePtr,
                sizeof(palettePtr));
    // 故意不注册 g_stubGlobalPalette。
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 1400u);
    g_stubSlotPalettes[4u] = slotPalette;
  }
  DIFF_CHAIN("S14-global-unreadable-to-blended", in);

  // S15 gameDllBase=0 → GetModuleHandleA 宿主 NULL → blended。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 1500u);
    g_stubSlotPalettes[4u] = slotPalette;
  }
  DIFF_CHAIN("S15-no-game-dll-to-blended", in);

  // S16 blended 命中 capturedCount>required 且 size>required → resize。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 5u;
    slotPalette.palette = MakeChainPalette(5u, 1600u);
    g_stubSlotPalettes[4u] = slotPalette;
    StubFrameTagRange range;
    range.queryOk = true;
    range.minFrameTag = 30u;
    range.maxFrameTag = 31u;
    g_stubFrameTagRanges[(uint64_t(4u) << 32u) | 3u] = range;
  }
  DIFF_CHAIN("S16-blended-oversize-resize", in);

  // S17 blended capturedCount<required → 拒绝 → PoseFallback(published)。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 2u;
    slotPalette.palette = MakeChainPalette(2u, 1700u);
    g_stubSlotPalettes[4u] = slotPalette;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 1701u, 0x1701u);
  }
  DIFF_CHAIN("S17-blended-undercount-to-pose", in);

  // S18 blended capturedCount>256 → 拒绝 → PoseFallback。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 300u;
    slotPalette.palette = MakeChainPalette(300u, 1800u);
    g_stubSlotPalettes[4u] = slotPalette;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 1801u, 0x1801u);
  }
  DIFF_CHAIN("S18-blended-over-256-to-pose", in);

  // S19 blended palette size<required（与 capturedCount 不一致）→ 拒绝。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 5u;
    slotPalette.palette = MakeChainPalette(2u, 1900u);
    g_stubSlotPalettes[4u] = slotPalette;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 1901u, 0x1901u);
  }
  DIFF_CHAIN("S19-blended-size-mismatch-to-pose", in);

  // S20 part 不可读（+0x08 读取失败）→ producer 绑定建条目并供槽。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = reinterpret_cast<void*>(uintptr_t(0x30000000u));
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 9u;
    binding.groupCount = 4u;
    binding.frameTag = 3u;
    g_stubPartBindings[in.part] = binding;
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 2000u);
    snapshot.hash = 0x2000u;
    snapshot.frameTag = 3u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S20-unreadable-part-producer-slot", in);

  // S21 part 不可读 + 绑定 slot=0xFFFFFFFF → 0xFFFFFFFF，跳过 slot 块。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = reinterpret_cast<void*>(uintptr_t(0x30000000u));
  in.runtimeModel = ChainModelA();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 0xFFFFFFFFu;
    g_stubPartBindings[in.part] = binding;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 2101u, 0x2101u);
  }
  DIFF_CHAIN("S21-producer-unbound-to-pose", in);

  // S22 part 不可读 + 绑定 slot>=0x3A98 → 绑定查询拒绝 → 0xFFFFFFFF。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = reinterpret_cast<void*>(uintptr_t(0x30000000u));
  in.runtimeModel = ChainModelA();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 0x3A98u + 5u;
    g_stubPartBindings[in.part] = binding;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 2201u, 0x2201u);
  }
  DIFF_CHAIN("S22-producer-slot-over-max-to-pose", in);

  // S23 +0x08=0x3A98+100（非法但未全 1）+ 绑定 miss → 返回原值并发布到
  // outPaletteSlotIndex，但 slot 块整体跳过。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 0x3A98u + 100u);
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 4u, 2301u, 0x2301u);
  DIFF_CHAIN("S23-slot-over-max-published", in);

  // S24 缓存 confirm：call1 直读建条目；call2 +0x08 失效，producer 复核
  // 通过（槽位一致 + groupCount 覆盖）→ ServedAfterConfirm+1。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 9u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 2400u);
    snapshot.hash = 0x2400u;
    snapshot.frameTag = 9u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S24-call1-insert", in);
  in.frameSerial = ++g_chainFrameSerial;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  g_stubPartSnapshots.clear();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 9u;
    binding.groupCount = 5u;
    binding.frameTag = 10u;
    g_stubPartBindings[in.part] = binding;
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 2402u);
    g_stubSlotPalettes[9u] = slotPalette;
  }
  DIFF_CHAIN("S24-confirm-served", in);

  // S25 缓存 reject（槽位不一致）：RejectedStale+1 → PoseFallback。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(1u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 5u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 2500u);
    snapshot.hash = 0x2500u;
    snapshot.frameTag = 5u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S25-call1-insert", in);
  in.frameSerial = ++g_chainFrameSerial;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  g_stubPartSnapshots.clear();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 8u;  // != 记忆槽位 5
    binding.groupCount = 5u;
    g_stubPartBindings[in.part] = binding;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 2501u, 0x2501u);
  }
  DIFF_CHAIN("S25-reject-slot-mismatch", in);

  // S26 缓存 reject（groupCount 不足）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(1u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 5u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 2600u);
    snapshot.hash = 0x2600u;
    snapshot.frameTag = 5u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S26-call1-insert", in);
  in.frameSerial = ++g_chainFrameSerial;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  g_stubPartSnapshots.clear();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 5u;
    binding.groupCount = 2u;  // < required 3
    g_stubPartBindings[in.part] = binding;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 2601u, 0x2601u);
  }
  DIFF_CHAIN("S26-reject-groupcount", in);

  // S27 缓存 reject（producer 查询本身失败）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(1u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 5u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 2700u);
    snapshot.hash = 0x2700u;
    snapshot.frameTag = 5u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S27-call1-insert", in);
  in.frameSerial = ++g_chainFrameSerial;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  g_stubPartSnapshots.clear();
  {
    StubPartBinding binding;
    binding.queryOk = false;
    g_stubPartBindings[in.part] = binding;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 2701u, 0x2701u);
  }
  DIFF_CHAIN("S27-reject-producer-miss", in);

  // S28 epoch 翻转：同 part 旧条目 epoch 不匹配 → 按新对象重新绑定。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(2u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 9u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 2800u);
    snapshot.hash = 0x2800u;
    snapshot.frameTag = 9u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  DIFF_CHAIN("S28-call1-epoch1", in);
  in.frameSerial = ++g_chainFrameSerial;
  g_stubMapEpoch = 2u;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 11u;
    binding.groupCount = 4u;
    g_stubPartBindings[in.part] = binding;
  }
  DIFF_CHAIN("S28-epoch-flip-rebind", in);

  // S29 lookup 碰撞：A 建条目 → B（同 lookup 槽）建条目覆盖 → A 再访问
  // 时 lookup 指向 B（身份不符）→ 4096 线性扫描找回 A → confirm。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u});
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  {
    StubPartSnapshot snapshotA;
    snapshotA.queryOk = true;
    snapshotA.palette = MakeChainPalette(2u, 2900u);
    snapshotA.hash = 0x2900u;
    snapshotA.frameTag = 3u;
    g_stubPartSnapshots[ChainPartCollisionA()] = snapshotA;
    StubPartSnapshot snapshotB;
    snapshotB.queryOk = true;
    snapshotB.palette = MakeChainPalette(2u, 2901u);
    snapshotB.hash = 0x2901u;
    snapshotB.frameTag = 4u;
    g_stubPartSnapshots[ChainPartCollisionB()] = snapshotB;
  }
  in.part = ChainPartCollisionA();
  SetChainPartSlot(in.part, 3u);
  DIFF_CHAIN("S29-callA-insert", in);
  in.frameSerial = ++g_chainFrameSerial;
  in.part = ChainPartCollisionB();
  SetChainPartSlot(in.part, 4u);
  DIFF_CHAIN("S29-callB-insert-collision", in);
  in.frameSerial = ++g_chainFrameSerial;
  in.part = ChainPartCollisionA();
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 3u;
    binding.groupCount = 3u;
    g_stubPartBindings[in.part] = binding;
  }
  DIFF_CHAIN("S29-collision-scan-confirm", in);

  // S30 published pose 直中（matrixHash 非零 → rawPoseHash 取之）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 4u, 3000u, 0x3000u);
  DIFF_CHAIN("S30-published-direct", in);

  // S31 published pose（matrixHash=0 → rawPoseHash 重算）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 4u, 3100u, 0u);
  DIFF_CHAIN("S31-published-hash-zero", in);

  // S32 published 经 +0xA0 候选命中。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[static_cast<uint8_t*>(in.runtimeModel) + 0xA0u] =
      MakeChainPoseRecord(static_cast<uint8_t*>(in.runtimeModel) + 0xA0u, 4u,
                          3200u, 0x3200u);
  DIFF_CHAIN("S32-published-plus-a0", in);

  // S33 published 经 -0xA0 候选命中。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[static_cast<uint8_t*>(in.runtimeModel) - 0xA0u] =
      MakeChainPoseRecord(static_cast<uint8_t*>(in.runtimeModel) - 0xA0u, 4u,
                          3300u, 0x3300u);
  DIFF_CHAIN("S33-published-minus-a0", in);

  // S34 published 经 owner 反查命中（Phase 7.51 路径）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.part = ChainPart(5u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  {
    StubPartOwner owner;
    owner.queryOk = true;
    owner.ownerRuntimeModel = ChainModelB();
    g_stubPartOwners[in.part] = owner;
    g_stubPoseRecords[ChainModelB()] =
        MakeChainPoseRecord(ChainModelB(), 4u, 3400u, 0x3400u);
  }
  DIFF_CHAIN("S34-published-via-owner", in);

  // S35 owner 查询失败 → alias（CModel deny）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.part = ChainPart(5u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  {
    StubPartOwner owner;
    owner.queryOk = false;
    g_stubPartOwners[in.part] = owner;
  }
  SetupChainCModel(g_chainModelBufA, 4u, true);
  DIFF_CHAIN("S35-owner-miss-cmodel-deny", in);

  // S36 owner == runtimeModelPtr → 守卫跳过 → alias。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.part = ChainPart(5u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  {
    StubPartOwner owner;
    owner.queryOk = true;
    owner.ownerRuntimeModel = in.runtimeModel;  // 与 caller 相同 → 跳过
    g_stubPartOwners[in.part] = owner;
  }
  SetupChainCModel(g_chainModelBufA, 4u, true);
  DIFF_CHAIN("S36-owner-same-as-caller-deny", in);

  // S37 alias 解析成功 + CModel deny（默认 env）→ false；
  // flipped ALLOW_CMODEL=1 env 下同一场景 decode 成功 source=5。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  SetupChainCModel(g_chainModelBufA, 4u, true);
  DIFF_CHAIN("S37-cmodel-env-gated", in);

  // S38 alias + allowCModelFallbackForCall=true → decode source=5。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.allowCModel = true;
  SetupChainCModel(g_chainModelBufA, 4u, true);
  DIFF_CHAIN("S38-cmodel-allow-param", in);

  // S39 CModel count>1024 → TryRead 拒绝 → alias 失败 → false。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.allowCModel = true;
  SetupChainCModel(g_chainModelBufA, 2000u, true);
  DIFF_CHAIN("S39-cmodel-count-over-1024", in);

  // S40 CModel pose 数组不可读 → alias 失败 → false。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.allowCModel = true;
  SetupChainCModel(g_chainModelBufA, 4u, false);
  DIFF_CHAIN("S40-cmodel-pose-unreadable", in);

  // S41 模型完全不可读（伪指针）→ alias 失败 → false。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = reinterpret_cast<void*>(uintptr_t(0x10000u));
  in.allowCModel = true;
  DIFF_CHAIN("S41-alias-unreadable-model", in);

  // S42 poseCount<required：directPose/sparsePose 失败 → uniform 兜底。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.allowCModel = true;
  SetupChainCModel(g_chainModelBufA, 2u, true);
  DIFF_CHAIN("S42-cmodel-uniform-fallback", in);

  // S43 walk-through（published）：groupCount=3、加权平均 FP 路径。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  sizes = {2u, 2u, 2u};
  indices = {0u, 1u, 2u, 3u, 4u, 5u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 6u, 4300u, 0x4300u);
  DIFF_CHAIN("S43-walkthrough-published", in);
  sizes.clear();
  indices.clear();

  // S44 walk-through（cmodel allow）：source=5。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  sizes = {2u, 2u, 2u};
  indices = {0u, 1u, 2u, 3u, 4u, 5u};
  in.runtimeModel = ChainModelA();
  in.allowCModel = true;
  SetupChainCModel(g_chainModelBufA, 6u, true);
  DIFF_CHAIN("S44-walkthrough-cmodel", in);
  sizes.clear();
  indices.clear();

  // S45 walk-through groupSize=0 → 无效 → fallback（directPose）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u});
  sizes = {0u, 2u};
  indices = {0u, 1u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 4u, 4500u, 0x4500u);
  DIFF_CHAIN("S45-walkthrough-zero-size", in);
  sizes.clear();
  indices.clear();

  // S46 walk-through running>matrixIndices.size() → 整体跳过 → fallback。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  sizes = {2u, 2u, 2u};
  indices = {0u, 1u, 2u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 4u, 4600u, 0x4600u);
  DIFF_CHAIN("S46-walkthrough-running-over", in);
  sizes.clear();
  indices.clear();

  // S47 walk-through decode 越界（matrixIndex>=poseCount）→ fallback。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u});
  sizes = {1u, 1u};
  indices = {0u, 9u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 3u, 4700u, 0x4700u);
  DIFF_CHAIN("S47-walkthrough-decode-overrun", in);
  sizes.clear();
  indices.clear();

  // S48 walk-through vertexGroup slot>=groupCount → 无效 → fallback。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 5u});
  sizes = {1u, 1u};
  indices = {0u, 1u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 8u, 4800u, 0x4800u);
  DIFF_CHAIN("S48-walkthrough-slot-over-groups", in);
  sizes.clear();
  indices.clear();

  // S49 groupCount>256 → 跳过 walk-through → fallback。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  sizes.assign(300u, 0u);
  indices.clear();
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 4u, 4900u, 0x4900u);
  DIFF_CHAIN("S49-groupcount-over-256", in);
  sizes.clear();

  // S50 directMatrixRemap（matrixIndices 非空、索引乱序）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  indices = {3u, 2u, 1u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 8u, 5000u, 0x5000u);
  DIFF_CHAIN("S50-direct-matrix-remap", in);
  indices.clear();

  // S51 sparseMatrixRemap（maxSlot>=indices.size() 使 direct 失败；
  // uniqueCount<=size 使 sparse 成功；未命中槽保持 0 矩阵）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({2u, 0u, 5u, 2u});
  indices = {7u, 6u, 5u};
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 8u, 5100u, 0x5100u);
  DIFF_CHAIN("S51-sparse-matrix-remap", in);
  indices.clear();

  // S52 sparsePose（maxSlot>=poseCount 使 directPose 失败）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({1u, 3u, 7u});
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 5u, 5200u, 0x5200u);
  DIFF_CHAIN("S52-sparse-pose", in);

  // S53 uniform（uniqueCount>poseCount）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u});
  in.runtimeModel = ChainModelA();
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 3u, 5300u, 0x5300u);
  DIFF_CHAIN("S53-uniform", in);

  // S54 proven hint 下 GroupScan 不进入（calls[GroupScan]=0），
  // directPose 成功 palette=proven+1。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.runtimeModel = ChainModelA();
  in.provenMaxSlot = 5u;
  g_stubPoseRecords[in.runtimeModel] =
      MakeChainPoseRecord(in.runtimeModel, 8u, 5400u, 0x5400u);
  DIFF_CHAIN("S54-proven-no-groupscan", in);

  // S55 slot 路径 publishSlotFrameTags（blended 分支 ready）。
  ResetChainWorld();
  freshIn();
  groups = ChainGroups({0u, 1u, 2u});
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 5500u);
    g_stubSlotPalettes[4u] = slotPalette;
    StubFrameTagRange range;
    range.queryOk = true;
    range.minFrameTag = 41u;
    range.maxFrameTag = 43u;
    g_stubFrameTagRanges[(uint64_t(4u) << 32u) | 3u] = range;
  }
  DIFF_CHAIN("S55-blended-frametags", in);
}

// --- 随机场景电池：世界状态全权重随机，20,000 轮 ---
void DiffChainRandom() {
  std::vector<uint8_t> groups;
  std::vector<uint32_t> sizes;
  std::vector<uint32_t> indices;
  void* const fakePartA = reinterpret_cast<void*>(uintptr_t(0x30000000u));
  void* const fakePartB = reinterpret_cast<void*>(uintptr_t(0x30001000u));
  void* const fakeModelA = reinterpret_cast<void*>(uintptr_t(0x10000u));
  void* const fakeModelB = reinterpret_cast<void*>(uintptr_t(0x50000000u));

  for (uint32_t iter = 0u; iter < 20000u; ++iter) {
    ResetChainWorldLite();
    ChainInput in;
    in.frameSerial = 1000u + iter;

    // part 选择（arena 指针可写 +0x08；伪指针必然不可读）。
    bool partOnArena = false;
    switch (NextU32() % 8u) {
      case 0u: in.part = nullptr; break;
      case 1u: in.part = ChainPart(0u); partOnArena = true; break;
      case 2u: in.part = ChainPart(1u); partOnArena = true; break;
      case 3u: in.part = ChainPart(2u); partOnArena = true; break;
      case 4u: in.part = fakePartA; break;
      case 5u: in.part = fakePartB; break;
      case 6u: in.part = ChainPartCollisionA(); partOnArena = true; break;
      default: in.part = ChainPartCollisionB(); partOnArena = true; break;
    }

    switch (NextU32() % 5u) {
      case 0u: in.runtimeModel = nullptr; break;
      case 1u: in.runtimeModel = ChainModelA(); break;
      case 2u: in.runtimeModel = ChainModelB(); break;
      case 3u: in.runtimeModel = fakeModelA; break;
      default: in.runtimeModel = fakeModelB; break;
    }

    // vertex groups：规模与槽位上限分档。
    const uint32_t groupRun = 1u + NextU32() % 8u;
    const uint32_t slotClass = NextU32() % 4u;
    const uint32_t slotCap =
        slotClass == 0u ? 2u : slotClass == 1u ? 7u : slotClass == 2u ? 31u
                                                                      : 255u;
    groups.clear();
    uint32_t actualMax = 0u;
    for (uint32_t i = 0u; i < groupRun; ++i) {
      const uint8_t slot = uint8_t(NextU32() % (slotCap + 1u));
      groups.push_back(slot);
      if (slot > actualMax)
        actualMax = slot;
    }
    const uint32_t required = actualMax + 1u;
    in.vertexGroups = &groups;

    sizes.clear();
    if ((NextU32() % 5u) < 2u) {
      const uint32_t n = 1u + NextU32() % 4u;
      for (uint32_t i = 0u; i < n; ++i)
        sizes.push_back(NextU32() % 4u);
    }
    indices.clear();
    if ((NextU32() % 2u) == 0u) {
      const uint32_t n = 1u + NextU32() % 8u;
      for (uint32_t i = 0u; i < n; ++i)
        indices.push_back(NextU32() % 10u);
    }
    in.matrixGroupSizes = &sizes;
    in.matrixIndices = &indices;

    const uint32_t provenRoll = NextU32() % 10u;
    // proven 必须遵守生产合同（封包时的组域最大值）：只能为 0xFFFFFFFF
    // （未知，走精确扫描）或 >= 实际最大值。更小的值在生产中不可能出现
    // （链信任该上界，sparse 重建会按唯一槽位下标写入）。
    in.provenMaxSlot = provenRoll < 7u
        ? 0xFFFFFFFFu
        : (provenRoll < 9u ? actualMax
                           : actualMax + NextU32() % (256u - actualMax));
    in.allowCModel = (NextU32() % 5u) == 0u;

    // part +0x08 与可读性。
    uint32_t slot08 = 0xFFFFFFFFu;
    if (partOnArena) {
      if ((NextU32() % 4u) != 0u)
        RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
      const uint32_t slotRoll = NextU32() % 10u;
      if (slotRoll < 4u)
        slot08 = NextU32() % 16u;
      else if (slotRoll < 7u)
        slot08 = 0xFFFFFFFFu;
      else if (slotRoll < 9u)
        slot08 = 0x3A98u + NextU32() % 8u;
      SetChainPartSlot(in.part, slot08);
    }

    // producer 绑定表。
    uint32_t bindingSlot = 0xFFFFFFFFu;
    bool hasBinding = false;
    if (in.part != nullptr && (NextU32() % 2u) == 0u) {
      StubPartBinding binding;
      binding.queryOk = (NextU32() % 10u) != 0u;
      const uint32_t bRoll = NextU32() % 10u;
      binding.slotIndex = bRoll < 6u
          ? NextU32() % 16u
          : (bRoll < 8u ? 0xFFFFFFFFu : 0x3A98u + NextU32() % 4u);
      binding.groupCount = NextU32() % (required + 2u);
      binding.frameTag = NextU32() % 3u;
      bindingSlot = binding.slotIndex;
      hasBinding = true;
      g_stubPartBindings[in.part] = binding;
    }

    // part snapshot。
    if (in.part != nullptr && (NextU32() % 10u) < 4u) {
      StubPartSnapshot snapshot;
      snapshot.queryOk = (NextU32() % 10u) != 0u;
      const uint32_t sizeRoll = NextU32() % 10u;
      const uint32_t size = sizeRoll < 6u
          ? required
          : (sizeRoll < 8u ? required + 2u
                           : (required > 1u ? required - 1u : 1u));
      snapshot.palette = MakeChainPalette(size, iter);
      snapshot.hash = (NextU32() % 3u) == 0u ? 0u : (0x1000u + iter);
      snapshot.frameTag =
          (NextU32() % 3u) == 0u ? 0u : (0x2000u + (iter % 97u));
      g_stubPartSnapshots[in.part] = snapshot;
    }

    // slot palette / frame tag range（slot 键全覆盖 0..15）。
    for (uint32_t slot = 0u; slot < 16u; ++slot) {
      if ((NextU32() % 4u) != 0u)
        continue;
      StubSlotPalette slotPalette;
      slotPalette.queryOk = (NextU32() % 10u) != 0u;
      const uint32_t cRoll = NextU32() % 10u;
      slotPalette.capturedCount =
          cRoll < 5u ? required
                     : (cRoll < 7u ? (required > 1u ? required - 1u : 0u)
                                   : (cRoll < 9u ? required + 2u : 300u));
      const uint32_t paletteSize =
          (NextU32() % 5u) == 0u
              ? (slotPalette.capturedCount > 2u ? slotPalette.capturedCount - 2u
                                                : 0u)
              : slotPalette.capturedCount;
      slotPalette.palette = MakeChainPalette(paletteSize, iter * 7u + slot);
      g_stubSlotPalettes[slot] = slotPalette;
      if ((NextU32() % 3u) == 0u) {
        StubFrameTagRange range;
        range.queryOk = (NextU32() % 4u) != 0u;
        range.minFrameTag = 0x3000u + slot;
        range.maxFrameTag = range.minFrameTag + NextU32() % 3u;
        g_stubFrameTagRanges[(uint64_t(slot) << 32u) | required] = range;
      }
    }

    // Game.dll / 全局 palette。
    if ((NextU32() % 2u) == 0u) {
      g_stubGameDllBase =
          reinterpret_cast<uintptr_t>(g_stubGameDllImage.data());
      RegisterReadable(g_stubGameDllImage.data(), g_stubGameDllImage.size());
      if ((NextU32() % 10u) < 7u) {
        const void* palettePtr = g_stubGlobalPalette.data();
        std::memcpy(g_stubGameDllImage.data() + 0xBC6BD0u, &palettePtr,
                    sizeof(palettePtr));
        if ((NextU32() % 10u) < 7u)
          RegisterReadable(g_stubGlobalPalette.data(),
                           g_stubGlobalPalette.size());
        auto fillSlot = [&](uint32_t slot) {
          if (slot < 16u) {
            for (uint32_t i = 0u; i < required * 48u; ++i)
              g_stubGlobalPalette[size_t(slot) * 48u + i] =
                  uint8_t(0x80u + slot * 31u + i);
          }
        };
        fillSlot(slot08);
        if (hasBinding)
          fillSlot(bindingSlot);
      }
    }

    // PoseRegistry 记录（真实模型缓冲的 ±0xA0 三键）。
    auto maybeRecord = [&](void* key) {
      if ((NextU32() % 4u) != 0u)
        return;
      const uint32_t count = 1u + NextU32() % 8u;
      g_stubPoseRecords[key] = MakeChainPoseRecord(
          key, count, iter, (NextU32() % 3u) == 0u ? 0u : (0x4000u + iter));
    };
    if (in.runtimeModel == ChainModelA() ||
        in.runtimeModel == ChainModelB()) {
      maybeRecord(in.runtimeModel);
      maybeRecord(static_cast<uint8_t*>(in.runtimeModel) + 0xA0u);
      maybeRecord(static_cast<uint8_t*>(in.runtimeModel) - 0xA0u);
    }
    // owner 反查也可能命中 ChainModelA/B 的记录（上面已随机布点）。

    // owner 表。
    if (in.part != nullptr && (NextU32() % 4u) == 0u) {
      StubPartOwner owner;
      owner.queryOk = (NextU32() % 4u) != 0u;
      owner.ownerRuntimeModel =
          (NextU32() % 2u) == 0u ? ChainModelA() : ChainModelB();
      g_stubPartOwners[in.part] = owner;
    }

    // owned snapshot（合同 ON 时唯一来源；OFF 时被忽略）。
    if ((NextU32() % 5u) == 0u) {
      StubOwnedSnapshot owned;
      owned.queryOk = true;
      owned.palette = MakeChainPalette(required, iter * 13u);
      auto& sel = owned.selection;
      sel.source = dxvk::war3::render::skin::Source::OwnedPartSnapshot;
      sel.space = dxvk::war3::render::skin::Space::World;
      sel.domain = dxvk::war3::render::skin::Domain::VertexGroups;
      sel.runtimeModel = reinterpret_cast<uintptr_t>(in.runtimeModel);
      sel.part = reinterpret_cast<uintptr_t>(in.part);
      sel.ownerEpoch = 5u;
      sel.publicationTicket = 6u;
      sel.captureSerial = 7u;
      sel.hash = 0x5000u + iter;
      sel.slot = 21u;
      sel.actualGroupCount = required;
      sel.frameTag = 77u;
      g_stubOwnedSnapshots[std::make_pair(in.runtimeModel, in.part)] = owned;
    }

    // CModel 字段（真实模型缓冲；count 1..8，pose 可读性随机）。
    if ((in.runtimeModel == ChainModelA() ||
         in.runtimeModel == ChainModelB()) &&
        (NextU32() % 10u) < 4u) {
      std::vector<uint8_t>& buf = in.runtimeModel == ChainModelA()
          ? g_chainModelBufA
          : g_chainModelBufB;
      const uint32_t count = 1u + NextU32() % 8u;
      WriteCModelFields(in.runtimeModel, count, g_chainPoseBuf.data());
      RegisterReadable(buf.data(), buf.size());
      if ((NextU32() % 4u) != 0u) {
        RegisterReadable(g_chainPoseBuf.data(), size_t(count) * 48u);
        for (uint32_t i = 0u; i < count * 48u; ++i)
          g_chainPoseBuf[i] = uint8_t(0x40u + i);
      }
    }

    // map epoch 偶发翻转（缓存条目按 epoch 隔离）。
    if ((NextU32() % 20u) == 0u)
      g_stubMapEpoch = 2u;

    RunDiffChain("random", in, __LINE__);
  }
}

// ---------------------------------------------------------------------------
// --probe-chain：8 个固定场景（合同 OFF 期望）+ 2 个 owned 场景（合同 ON
// 时期望翻转），由等价门禁在多组 env 下驱动并对照独立期望值表。
// ---------------------------------------------------------------------------
void PrintProbeChainOutcome(const char* name, const ChainOutcome& m,
                            const ChainOutcome& l) {
  std::cout << "probe-chain " << name << " module=" << m.ok << "," << m.source
            << "," << m.slotIndex << "," << m.maxSlot << ","
            << (m.paletteBits.size() / 16u) << "," << m.hash << "," << m.minTag
            << "," << m.maxTag << "," << m.servedDelta << "," << m.rejectedDelta
            << " legacy=" << l.ok << "," << l.source << "," << l.slotIndex
            << "," << l.maxSlot << "," << (l.paletteBits.size() / 16u) << ","
            << l.hash << "," << l.minTag << "," << l.maxTag << ","
            << l.servedDelta << "," << l.rejectedDelta << "\n";
}

int RunProbeChain() {
  std::vector<uint8_t> groups;
  std::vector<uint32_t> sizes;
  std::vector<uint32_t> indices;
  ChainInput in;
  in.vertexGroups = &groups;
  in.matrixGroupSizes = &sizes;
  in.matrixIndices = &indices;
  auto run = [&](const char* name) {
    const ChainOutcome m = RunChainModule(in);
    const ChainOutcome l = RunChainLegacy(in);
    PrintProbeChainOutcome(name, m, l);
  };

  // P1 slot-direct：+0x08=7，part snapshot 命中（hash/tag 非零）。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  sizes.clear();
  indices.clear();
  in.runtimeModel = nullptr;
  in.allowCModel = false;
  in.provenMaxSlot = 0xFFFFFFFFu;
  in.frameSerial = 1u;
  in.part = ChainPart(0u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 7u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 100u);
    snapshot.hash = 0xABCDu;
    snapshot.frameTag = 55u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  run("slot-direct");

  // P2 caches-confirm：call1 建条目；call2 +0x08 失效、producer 复核通过。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 10u;
  in.part = ChainPart(1u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 9u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 200u);
    snapshot.hash = 0x200u;
    snapshot.frameTag = 1u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  RunChainModule(in);
  RunChainLegacy(in);
  in.frameSerial = 11u;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  g_stubPartSnapshots.clear();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 9u;
    binding.groupCount = 4u;
    binding.frameTag = 2u;
    g_stubPartBindings[in.part] = binding;
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 201u);
    g_stubSlotPalettes[9u] = slotPalette;
  }
  run("caches-confirm");

  // P3 reject-then-published：复核槽位不一致 → RejectedStale → published。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 20u;
  in.part = ChainPart(2u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 5u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 300u);
    snapshot.hash = 0x300u;
    snapshot.frameTag = 1u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  RunChainModule(in);
  RunChainLegacy(in);
  in.frameSerial = 21u;
  SetChainPartSlot(in.part, 0xFFFFFFFFu);
  g_stubPartSnapshots.clear();
  {
    StubPartBinding binding;
    binding.queryOk = true;
    binding.slotIndex = 8u;
    binding.groupCount = 4u;
    g_stubPartBindings[in.part] = binding;
    g_stubPoseRecords[in.runtimeModel] =
        MakeChainPoseRecord(in.runtimeModel, 4u, 301u, 0x1234u);
  }
  run("reject-then-published");
  in.runtimeModel = nullptr;

  // P4 global：Game.dll 伪镜像 + 全局 palette（frame tag range ready）。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 30u;
  in.part = ChainPart(3u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 3u);
  SetupGlobalSlotPalette(3u, 3u, 200u);
  {
    StubFrameTagRange range;
    range.queryOk = true;
    range.minFrameTag = 20u;
    range.maxFrameTag = 21u;
    g_stubFrameTagRanges[(uint64_t(3u) << 32u) | 3u] = range;
  }
  run("global");

  // P5 blended：gameDllBase=0（宿主 GetModuleHandleA NULL）→ blended。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 40u;
  in.part = ChainPart(4u);
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 4u);
  {
    StubSlotPalette slotPalette;
    slotPalette.queryOk = true;
    slotPalette.capturedCount = 3u;
    slotPalette.palette = MakeChainPalette(3u, 500u);
    g_stubSlotPalettes[4u] = slotPalette;
    StubFrameTagRange range;
    range.queryOk = true;
    range.minFrameTag = 30u;
    range.maxFrameTag = 30u;
    g_stubFrameTagRanges[(uint64_t(4u) << 32u) | 3u] = range;
  }
  run("blended");

  // P6 cmodel-deny：alias 成功但默认 env 不允许 CModel → false。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 50u;
  in.part = nullptr;
  in.runtimeModel = ChainModelA();
  in.allowCModel = false;
  SetupChainCModel(g_chainModelBufA, 4u, true);
  run("cmodel-deny");

  // P7 cmodel-allow：同一世界 + allowCModelFallbackForCall → source=5。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 60u;
  in.runtimeModel = ChainModelA();
  in.allowCModel = true;
  SetupChainCModel(g_chainModelBufA, 4u, true);
  run("cmodel-allow");
  in.allowCModel = false;

  // P8 pose-none：模型伪指针不可读 → alias 失败 → false。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 70u;
  in.runtimeModel = reinterpret_cast<void*>(uintptr_t(0x10000u));
  run("pose-none");
  in.runtimeModel = nullptr;

  // P9 owned-hit：合同 ON 时 source=6/slot=42；OFF 时 owned 表被忽略，
  // 其余来源全 miss → false。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 80u;
  in.part = ChainPart(5u);
  in.runtimeModel = ChainModelA();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 6u);
  {
    StubOwnedSnapshot owned;
    owned.queryOk = true;
    owned.palette = MakeChainPalette(3u, 900u);
    auto& sel = owned.selection;
    sel.source = dxvk::war3::render::skin::Source::OwnedPartSnapshot;
    sel.space = dxvk::war3::render::skin::Space::World;
    sel.domain = dxvk::war3::render::skin::Domain::VertexGroups;
    sel.runtimeModel = reinterpret_cast<uintptr_t>(in.runtimeModel);
    sel.part = reinterpret_cast<uintptr_t>(in.part);
    sel.ownerEpoch = 7u;
    sel.publicationTicket = 9u;
    sel.captureSerial = 11u;
    sel.hash = 0x777u;
    sel.slot = 42u;
    sel.actualGroupCount = 3u;
    sel.frameTag = 77u;
    g_stubOwnedSnapshots[std::make_pair(in.runtimeModel, in.part)] = owned;
  }
  run("owned-hit");

  // P10 owned-miss：合同 ON 时 fail-closed（不得 fallthrough 到 slot
  // snapshot）；OFF 时 slot snapshot 命中 source=3。
  ResetChainWorld();
  groups = ChainGroups({0u, 1u, 2u});
  in.frameSerial = 90u;
  in.part = ChainPart(6u);
  in.runtimeModel = ChainModelB();
  RegisterReadable(g_chainPartArena.data(), g_chainPartArena.size());
  SetChainPartSlot(in.part, 8u);
  {
    StubPartSnapshot snapshot;
    snapshot.queryOk = true;
    snapshot.palette = MakeChainPalette(3u, 1000u);
    snapshot.hash = 0x1000u;
    snapshot.frameTag = 3u;
    g_stubPartSnapshots[in.part] = snapshot;
  }
  run("owned-miss");

  return 0;
}

// ===========================================================================
// M2-3：motion / churn 诊断三函数的差分（模块实现 vs legacy 参考）
// ===========================================================================
// 口径：
//   * 三个函数只读写 War3ShadowCaptureStats 的 motion 字段族（23 个字段，
//     见 kMotionFieldNames），并各自持有**函数内 static** 的 512 项指向表 +
//     替换游标；两侧独立编译、独立 static，由同一调用序列驱动，状态演化
//     必须逐点一致。
//   * 逐调用比较 23 个 motion 字段；每个场景段结束再对整个
//     War3ShadowCaptureStats 做 memcmp（结构只含 POD / std::array，平凡可
//     复制；两侧都从 {} 起步），以捕获"写到了 motion 之外的字段"。
//   * 三个函数**从不解引用** runtimeModelPtr（只做指针相等比较），因此测试
//     直接使用合成指针常量：本电池不需要为它们提供任何替身，隔离出的正是
//     M2-3 搬动的那部分文本。
//   * War3SemanticPaletteDiagnosticsRuntime() 是默认 OFF 的 env 门：默认 env
//     下两侧都早退（差分仍在比较，但只覆盖早退路径）。真正覆盖函数体的是
//     DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1 的 motion-on 矩阵。

constexpr size_t kMotionFieldCount = 23u;

const char* const kMotionFieldNames[kMotionFieldCount] = {
    "liveSample", "liveNewRuntime", "liveRawChanged", "liveRawStable",
    "liveGroupChanged", "liveGroupStable", "liveLastRuntimeModelPtr",
    "liveLastPrevRawHash", "liveLastRawHash", "liveLastPrevGroupHash",
    "liveLastGroupHash", "poseChanged", "poseStable",
    "poseLastRuntimeModelPtr", "poseLastPrevHash", "poseLastHash",
    "submittedSample", "submittedNewRuntime", "submittedChanged",
    "submittedStable", "submittedLastRuntimeModelPtr",
    "submittedLastPrevHash", "submittedLastHash",
};

// --probe-motion 打印时取**绝对值**而非增量的字段下标（"上一次/本次"快照）。
const size_t kMotionAbsoluteFields[] = {
    6u, 7u, 8u, 9u, 10u, 13u, 14u, 15u, 20u, 21u, 22u,
};

static_assert(std::is_trivially_copyable<dxvk::War3ShadowCaptureStats>::value,
              "M2-3 差分依赖整结构 memcmp：War3ShadowCaptureStats 必须平凡可复制");

void SnapshotMotionFields(const dxvk::War3ShadowCaptureStats& stats,
                          uint64_t (&out)[kMotionFieldCount]) {
  out[0] = stats.semanticSceneLivePaletteMotionSampleCount;
  out[1] = stats.semanticSceneLivePaletteMotionNewRuntimeCount;
  out[2] = stats.semanticSceneLivePaletteMotionRawChangedCount;
  out[3] = stats.semanticSceneLivePaletteMotionRawStableCount;
  out[4] = stats.semanticSceneLivePaletteMotionGroupChangedCount;
  out[5] = stats.semanticSceneLivePaletteMotionGroupStableCount;
  out[6] = stats.semanticSceneLivePaletteMotionLastRuntimeModelPtr;
  out[7] = stats.semanticSceneLivePaletteMotionLastPrevRawHash;
  out[8] = stats.semanticSceneLivePaletteMotionLastRawHash;
  out[9] = stats.semanticSceneLivePaletteMotionLastPrevGroupHash;
  out[10] = stats.semanticSceneLivePaletteMotionLastGroupHash;
  out[11] = stats.semanticSceneDrawTimePoseChangedCount;
  out[12] = stats.semanticSceneDrawTimePoseStableCount;
  out[13] = stats.semanticSceneDrawTimePoseLastRuntimeModelPtr;
  out[14] = stats.semanticSceneDrawTimePoseLastPrevHash;
  out[15] = stats.semanticSceneDrawTimePoseLastHash;
  out[16] = stats.semanticSceneSubmittedPaletteMotionSampleCount;
  out[17] = stats.semanticSceneSubmittedPaletteMotionNewRuntimeCount;
  out[18] = stats.semanticSceneSubmittedPaletteMotionChangedCount;
  out[19] = stats.semanticSceneSubmittedPaletteMotionStableCount;
  out[20] = stats.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr;
  out[21] = stats.semanticSceneSubmittedPaletteMotionLastPrevHash;
  out[22] = stats.semanticSceneSubmittedPaletteMotionLastHash;
}

// 两侧各自的 stats 实例（两侧函数的 static 表相互独立，stats 也必须独立，
// 否则第二次调用就会看到对方写入的字段）。
dxvk::War3ShadowCaptureStats g_motionModuleStats = {};
dxvk::War3ShadowCaptureStats g_motionLegacyStats = {};

void DiffMotionState(const char* what, int line) {
  uint64_t moduleFields[kMotionFieldCount];
  uint64_t legacyFields[kMotionFieldCount];
  SnapshotMotionFields(g_motionModuleStats, moduleFields);
  SnapshotMotionFields(g_motionLegacyStats, legacyFields);
  for (size_t i = 0u; i < kMotionFieldCount; ++i) {
    ++g_checks;
    if (moduleFields[i] != legacyFields[i]) {
      ++g_failures;
      std::cerr << "DIFF line " << line << " " << what << "."
                << kMotionFieldNames[i] << ": module=" << moduleFields[i]
                << " legacy=" << legacyFields[i] << "\n";
    }
  }
}

void DiffMotionStructBytes(const char* what, int line) {
  ++g_checks;
  if (std::memcmp(&g_motionModuleStats, &g_motionLegacyStats,
                  sizeof(dxvk::War3ShadowCaptureStats)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what
              << ": War3ShadowCaptureStats 整结构字节不一致\n";
  }
}

void MotionCallLive(void* runtimeModelPtr, uint64_t frameSerial,
                    uint64_t rawHash, uint64_t groupHash) {
  sem::War3NoteLivePaletteMotion(g_motionModuleStats, runtimeModelPtr,
                                 frameSerial, rawHash, groupHash);
  legacy::War3NoteLivePaletteMotion(g_motionLegacyStats, runtimeModelPtr,
                                    frameSerial, rawHash, groupHash);
}

void MotionCallPose(void* runtimeModelPtr, uint64_t frameSerial,
                    uint64_t hash) {
  sem::War3NoteDrawTimePoseMotion(g_motionModuleStats, runtimeModelPtr,
                                  frameSerial, hash);
  legacy::War3NoteDrawTimePoseMotion(g_motionLegacyStats, runtimeModelPtr,
                                     frameSerial, hash);
}

void MotionCallSubmitted(void* runtimeModelPtr, uint64_t frameSerial,
                         uint64_t hash) {
  sem::War3NoteSubmittedPaletteMotion(g_motionModuleStats, runtimeModelPtr,
                                      frameSerial, hash);
  legacy::War3NoteSubmittedPaletteMotion(g_motionLegacyStats, runtimeModelPtr,
                                         frameSerial, hash);
}

// 合成指针：函数只比较指针值，不读内存。同一 index 在所有调用点恒定；
// 差分/电池/probe 使用互不重叠的 index 区间。
void* MotionPtr(uint32_t index) {
  return reinterpret_cast<void*>(uintptr_t(0x100000u) +
                                 uintptr_t(index) * 0x100u);
}

void DiffMotionSystematic() {
  // 早退组合（nullptr / raw==0 / group==0 / hash==0）与非零组合的全网格。
  const uint64_t hashes[4] = {0u, 1u, 0xFFFFFFFFFFFFFFFFull,
                              0x123456789ABCDEF0ull};
  const uint64_t frames[3] = {0u, 1u, 0xFFFFFFFFFFFFFFFFull};

  for (uint32_t p = 0u; p < 5u; ++p) {
    void* ptr = p == 0u ? nullptr : MotionPtr(p);
    for (uint32_t i = 0u; i < 4u; ++i) {
      for (uint32_t j = 0u; j < 4u; ++j) {
        for (uint32_t f = 0u; f < 3u; ++f) {
          MotionCallLive(ptr, frames[f], hashes[i], hashes[j]);
          DiffMotionState("LiveGrid", __LINE__);
        }
      }
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
      for (uint32_t f = 0u; f < 3u; ++f) {
        MotionCallPose(ptr, frames[f], hashes[i]);
        DiffMotionState("PoseGrid", __LINE__);
        MotionCallSubmitted(ptr, frames[f], hashes[i]);
        DiffMotionState("SubmittedGrid", __LINE__);
      }
    }
  }
  DiffMotionStructBytes("systematic-grid", __LINE__);

  // 同一 runtimeModel 的 new→stable→raw-changed→group-changed 逐级序列。
  void* seq = MotionPtr(100u);
  MotionCallLive(seq, 1u, 0xAAu, 0xBBu);
  DiffMotionState("LiveSeq1", __LINE__);
  MotionCallLive(seq, 2u, 0xAAu, 0xBBu);
  DiffMotionState("LiveSeq2", __LINE__);
  MotionCallLive(seq, 3u, 0xCCu, 0xBBu);
  DiffMotionState("LiveSeq3", __LINE__);
  MotionCallLive(seq, 4u, 0xCCu, 0xDDu);
  DiffMotionState("LiveSeq4", __LINE__);
  MotionCallPose(seq, 5u, 0xEEu);
  DiffMotionState("PoseSeq1", __LINE__);
  MotionCallPose(seq, 6u, 0xEEu);
  DiffMotionState("PoseSeq2", __LINE__);
  MotionCallPose(seq, 7u, 0xFFu);
  DiffMotionState("PoseSeq3", __LINE__);
  MotionCallSubmitted(seq, 8u, 0x11u);
  DiffMotionState("SubmittedSeq1", __LINE__);
  MotionCallSubmitted(seq, 9u, 0x11u);
  DiffMotionState("SubmittedSeq2", __LINE__);
  MotionCallSubmitted(seq, 10u, 0x22u);
  DiffMotionState("SubmittedSeq3", __LINE__);
  DiffMotionStructBytes("systematic-seq", __LINE__);
}

void DiffMotionRandom() {
  // Phase A：1700 个互异指针（1000..1699 由本阶段使用）。顺序驱动 512 项
  // 指向表填满，并越过 s_replaceCursor 触发驱逐；两侧独立 static 由同一
  // 序列驱动，驱逐槽位必须一致。
  constexpr uint32_t kMotionPoolSize = 1700u;
  for (uint32_t i = 0u; i < 700u; ++i) {
    void* ptr = MotionPtr(1000u + i);
    const uint64_t hash = 0x1000u + uint64_t(i);
    MotionCallLive(ptr, i, hash, hash + 1u);
    DiffMotionState("LiveFill", __LINE__);
    MotionCallPose(ptr, i, hash);
    DiffMotionState("PoseFill", __LINE__);
    MotionCallSubmitted(ptr, i, hash);
    DiffMotionState("SubmittedFill", __LINE__);
  }
  DiffMotionStructBytes("random-fill", __LINE__);

  // Phase B：300,000 轮定种子随机世界（覆盖 0..1699 全部指针：既有条目命中
  // 与驱逐共存；hash 池含 0 以反复走早退分支）。
  const uint64_t hashPool[6] = {0u, 1u, 2u, 0xFFFFFFFFFFFFFFFFull,
                                0xDEADBEEFCAFEBABEull, 0x8000000000000000ull};
  for (uint32_t round = 0u; round < 300000u; ++round) {
    const uint32_t pick = NextU32();
    void* ptr = (pick % 23u) == 0u ? nullptr
                                   : MotionPtr(pick % kMotionPoolSize);
    const uint64_t frame = uint64_t(NextU32()) * 0x10001u;
    const uint64_t hashA = hashPool[NextU32() % 6u];
    const uint64_t hashB = hashPool[NextU32() % 6u];
    switch (pick % 3u) {
      case 0u:
        MotionCallLive(ptr, frame, hashA, hashB);
        DiffMotionState("LiveRandom", __LINE__);
        break;
      case 1u:
        MotionCallPose(ptr, frame, hashA);
        DiffMotionState("PoseRandom", __LINE__);
        break;
      default:
        MotionCallSubmitted(ptr, frame, hashA);
        DiffMotionState("SubmittedRandom", __LINE__);
        break;
    }
  }
  DiffMotionStructBytes("random-total", __LINE__);
}

// ---------------------------------------------------------------------------
// M2-3 --probe-motion：固定场景的 module/legacy 逐字段对照，供等价门禁按
// 其**独立期望值表**校验（诊断门 ON / OFF 两组 env；场景指针互异，保证每个
// 场景的"首次调用 = new runtime"语义不依赖场景顺序）。
// ---------------------------------------------------------------------------
struct MotionProbeCall {
  uint64_t frame;
  uint64_t a;
  uint64_t b;
};

void FillMotionPrintFields(const uint64_t (&before)[kMotionFieldCount],
                           const uint64_t (&after)[kMotionFieldCount],
                           uint64_t (&out)[kMotionFieldCount]) {
  for (size_t i = 0u; i < kMotionFieldCount; ++i)
    out[i] = after[i] - before[i];
  for (size_t index : kMotionAbsoluteFields)
    out[index] = after[index];
}

void PrintMotionProbeRow(const char* name, int function, void* ptr,
                         const MotionProbeCall* calls, size_t callCount) {
  // probe 场景彼此独立：先把两侧 stats 归零（stats 归测试所有；两侧实现
  // 的函数内 static 指向表**不**归零，靠场景指针互异保证 "首次调用 =
  // new runtime" 语义）。归零后 Last* 字段只在被写到时非零，早退场景因此
  // 输出全 0 —— 门禁的期望值表正是按这个语义登记的。
  g_motionModuleStats = dxvk::War3ShadowCaptureStats{};
  g_motionLegacyStats = dxvk::War3ShadowCaptureStats{};
  uint64_t beforeModule[kMotionFieldCount];
  uint64_t beforeLegacy[kMotionFieldCount];
  SnapshotMotionFields(g_motionModuleStats, beforeModule);
  SnapshotMotionFields(g_motionLegacyStats, beforeLegacy);
  for (size_t i = 0u; i < callCount; ++i) {
    switch (function) {
      case 0:
        MotionCallLive(ptr, calls[i].frame, calls[i].a, calls[i].b);
        break;
      case 1:
        MotionCallPose(ptr, calls[i].frame, calls[i].a);
        break;
      default:
        MotionCallSubmitted(ptr, calls[i].frame, calls[i].a);
        break;
    }
  }
  uint64_t afterModule[kMotionFieldCount];
  uint64_t afterLegacy[kMotionFieldCount];
  SnapshotMotionFields(g_motionModuleStats, afterModule);
  SnapshotMotionFields(g_motionLegacyStats, afterLegacy);
  uint64_t modulePrint[kMotionFieldCount];
  uint64_t legacyPrint[kMotionFieldCount];
  FillMotionPrintFields(beforeModule, afterModule, modulePrint);
  FillMotionPrintFields(beforeLegacy, afterLegacy, legacyPrint);
  std::cout << "probe-motion " << name << " module=";
  for (size_t i = 0u; i < kMotionFieldCount; ++i)
    std::cout << (i == 0u ? "" : ",") << modulePrint[i];
  std::cout << " legacy=";
  for (size_t i = 0u; i < kMotionFieldCount; ++i)
    std::cout << (i == 0u ? "" : ",") << legacyPrint[i];
  std::cout << "\n";
}

int RunProbeMotion() {
  // 场景指针 index 2000..2013（与本文件其他 index 区间不重叠）。
  static const MotionProbeCall kLiveNew[] = {{1u, 0x11u, 0x22u}};
  static const MotionProbeCall kLiveStable[] = {{1u, 0x11u, 0x22u},
                                                {2u, 0x11u, 0x22u}};
  static const MotionProbeCall kLiveRawChange[] = {{1u, 0x11u, 0x22u},
                                                   {2u, 0x33u, 0x22u}};
  static const MotionProbeCall kLiveGroupChange[] = {{1u, 0x11u, 0x22u},
                                                     {2u, 0x11u, 0x44u}};
  static const MotionProbeCall kLiveZeroRaw[] = {{1u, 0x0u, 0x22u}};
  static const MotionProbeCall kLiveZeroGroup[] = {{1u, 0x11u, 0x0u}};
  static const MotionProbeCall kPoseNew[] = {{1u, 0xAAu, 0u}};
  static const MotionProbeCall kPoseStable[] = {{1u, 0xAAu, 0u}, {2u, 0xAAu, 0u}};
  static const MotionProbeCall kPoseChange[] = {{1u, 0xAAu, 0u}, {2u, 0xBBu, 0u}};
  static const MotionProbeCall kPoseZero[] = {{1u, 0x0u, 0u}};
  static const MotionProbeCall kSubmittedNew[] = {{1u, 0xCCu, 0u}};
  static const MotionProbeCall kSubmittedStable[] = {{1u, 0xCCu, 0u},
                                                     {2u, 0xCCu, 0u}};
  static const MotionProbeCall kSubmittedChange[] = {{1u, 0xCCu, 0u},
                                                     {2u, 0xDDu, 0u}};
  static const MotionProbeCall kSubmittedZero[] = {{1u, 0x0u, 0u}};

  PrintMotionProbeRow("live-new", 0, MotionPtr(2000u), kLiveNew, 1u);
  PrintMotionProbeRow("live-stable", 0, MotionPtr(2001u), kLiveStable, 2u);
  PrintMotionProbeRow("live-raw-change", 0, MotionPtr(2002u), kLiveRawChange,
                      2u);
  PrintMotionProbeRow("live-group-change", 0, MotionPtr(2003u),
                      kLiveGroupChange, 2u);
  PrintMotionProbeRow("live-null-ptr", 0, nullptr, kLiveNew, 1u);
  PrintMotionProbeRow("live-zero-raw", 0, MotionPtr(2004u), kLiveZeroRaw, 1u);
  PrintMotionProbeRow("live-zero-group", 0, MotionPtr(2005u), kLiveZeroGroup,
                      1u);
  PrintMotionProbeRow("pose-new", 1, MotionPtr(2006u), kPoseNew, 1u);
  PrintMotionProbeRow("pose-stable", 1, MotionPtr(2007u), kPoseStable, 2u);
  PrintMotionProbeRow("pose-change", 1, MotionPtr(2008u), kPoseChange, 2u);
  PrintMotionProbeRow("pose-zero", 1, MotionPtr(2009u), kPoseZero, 1u);
  PrintMotionProbeRow("submitted-new", 2, MotionPtr(2010u), kSubmittedNew, 1u);
  PrintMotionProbeRow("submitted-stable", 2, MotionPtr(2011u),
                      kSubmittedStable, 2u);
  PrintMotionProbeRow("submitted-change", 2, MotionPtr(2012u),
                      kSubmittedChange, 2u);
  PrintMotionProbeRow("submitted-zero", 2, MotionPtr(2013u), kSubmittedZero,
                      1u);
  return 0;
}

} // namespace

// ===========================================================================
// M2-2 host stubs：选择链本体在 device 内依赖的 12 个外部符号的定义
// （命名空间要求放在匿名命名空间之外；状态全局量见上方 stub 世界）。
// ===========================================================================
namespace dxvk::war3 {

uintptr_t GetGameDllBase() { return g_stubGameDllBase; }

// 与生产语义同向的替身：null/empty/源不可读 fail-closed。
bool SafeCopy(void* destination, const void* source, size_t size) noexcept {
  if (destination == nullptr || source == nullptr || size == 0u)
    return false;
  if (!RangeIsReadable(source, size))
    return false;
  std::memcpy(destination, source, size);
  return true;
}

} // namespace dxvk::war3

namespace dxvk::war3::model {

bool QueryBlendedPaletteBySlotIndex(uint32_t slotIndex, void* outPaletteVec,
                                    uint32_t& outGroupCount) {
  auto* vec = static_cast<std::vector<Matrix4>*>(outPaletteVec);
  const auto it = g_stubSlotPalettes.find(slotIndex);
  if (it == g_stubSlotPalettes.end() || !it->second.queryOk)
    return false;
  *vec = it->second.palette;
  outGroupCount = it->second.capturedCount;
  return true;
}

bool QueryBlendedPaletteFrameTagRange(uint32_t slotIndex,
                                      uint32_t expectedCount,
                                      uint32_t& outMinFrameTag,
                                      uint32_t& outMaxFrameTag,
                                      uint32_t& outMissingCount) {
  const auto it = g_stubFrameTagRanges.find((uint64_t(slotIndex) << 32u) |
                                            expectedCount);
  if (it == g_stubFrameTagRanges.end() || !it->second.queryOk)
    return false;
  outMinFrameTag = it->second.minFrameTag;
  outMaxFrameTag = it->second.maxFrameTag;
  outMissingCount = it->second.missingCount;
  return true;
}

bool QueryRenderablePartPaletteSlot(void* renderablePart,
                                    uint32_t& outSlotIndex,
                                    uint32_t* outGroupCount,
                                    uint32_t* outFrameTag) {
  const auto it = g_stubPartBindings.find(renderablePart);
  if (it == g_stubPartBindings.end() || !it->second.queryOk)
    return false;
  outSlotIndex = it->second.slotIndex;
  if (outGroupCount != nullptr)
    *outGroupCount = it->second.groupCount;
  if (outFrameTag != nullptr)
    *outFrameTag = it->second.frameTag;
  return true;
}

bool QueryRenderablePartPaletteSnapshot(void* renderablePart,
                                        uint32_t expectedCount,
                                        void* outPaletteVec, uint64_t* outHash,
                                        uint32_t* outFrameTag) {
  (void)expectedCount;
  auto* vec = static_cast<std::vector<Matrix4>*>(outPaletteVec);
  const auto it = g_stubPartSnapshots.find(renderablePart);
  if (it == g_stubPartSnapshots.end() || !it->second.queryOk)
    return false;
  *vec = it->second.palette;
  if (outHash != nullptr)
    *outHash = it->second.hash;
  if (outFrameTag != nullptr)
    *outFrameTag = it->second.frameTag;
  return true;
}

bool QueryOwnedRenderablePartPaletteSnapshot(
    void* runtimeModel, void* part, uint32_t required, void* outPaletteVec,
    render::skin::Selection& selected) {
  (void)required;
  auto* vec = static_cast<std::vector<Matrix4>*>(outPaletteVec);
  const auto it =
      g_stubOwnedSnapshots.find(std::make_pair(runtimeModel, part));
  if (it == g_stubOwnedSnapshots.end() || !it->second.queryOk)
    return false;
  *vec = it->second.palette;
  selected = it->second.selection;
  return true;
}

bool QueryRenderablePartOwnerRuntimeModel(void* renderablePart,
                                          void** outRuntimeModelPtr) {
  const auto it = g_stubPartOwners.find(renderablePart);
  if (it == g_stubPartOwners.end() || !it->second.queryOk)
    return false;
  *outRuntimeModelPtr = it->second.ownerRuntimeModel;
  return true;
}

PoseRegistry& PoseRegistry::instance() {
  static PoseRegistry registry;
  return registry;
}

bool PoseRegistry::findByRuntimeModel(void* runtimeModelPtr,
                                      PoseRecord& out) const {
  const auto it = g_stubPoseRecords.find(runtimeModelPtr);
  if (it == g_stubPoseRecords.end())
    return false;
  out = it->second;
  return true;
}

ShadowModelResourceCache& ShadowModelResourceCache::instance() {
  static ShadowModelResourceCache cache;
  return cache;
}

uint64_t ShadowModelResourceCache::mapEpoch() const { return g_stubMapEpoch; }

} // namespace dxvk::war3::model


// ===========================================================================
// M2-5：宿主机替身与 key 工具
// ===========================================================================
// VisibleRenderableRegistry::computeShadowManifestPartKey 是**纯静态哈希**（不读
// 注册表实例状态），但它的定义在 war3_visible_renderables.cpp —— 本宿主机测试
// 不链接那个翻译单元（M1「测试自己提供设备层原语替身」的同一先例）。替身只被
// 模块 .cpp 调用；两侧共用同一替身，因此差分不覆盖该哈希本身，只覆盖 M2-5 搬动
// 的发射块。
namespace dxvk::war3::render {

uint64_t VisibleRenderableRegistry::computeShadowManifestPartKey(
    const CurrentDrawContractRecord& record) {
  if (record.jHandle == 0u && record.unitPtr == nullptr)
    return 0u;
  uint64_t x = uint64_t(record.jHandle) + 0x9E3779B97F4A7C15ull;
  x ^= uint64_t(uintptr_t(record.unitPtr)) >> 4;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

} // namespace dxvk::war3::render

uint64_t TaxonomyKeyFor(uint64_t jHandle) {
  dxvk::war3::render::CurrentDrawContractRecord record = {};
  record.jHandle = uint32_t(jHandle);
  return dxvk::war3::render::VisibleRenderableRegistry::
      computeShadowManifestPartKey(record);
}

// ===========================================================================
// M2-5：skinned palette taxonomy 发射块的差分（模块纯函数 vs legacy 参考）
// ===========================================================================
// 口径：
//   * 模块侧 = src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp 的
//     War3EmitSemanticPaletteTaxonomy；legacy 侧 = 迁移前 d3d9_device.cpp
//     :21202-21580 的逐字节正文（war3_palette_taxonomy_emission_legacy_reference
//     .inc，由入库生成器 --m2-5 产出）。
//   * 两侧各持独立 stats 实例；块内三张 8192 项 thread_local 探针表也是两侧
//     各自编译的独立副本，由**同一调用序列**驱动 ⇒ 状态演化必须逐点一致。
//   * 逐调用比较 34 个 taxonomy 字段；每个场景段结束再对整个
//     War3ShadowCaptureStats 做 memcmp，以捕获"写到了这 34 个字段之外"。
//   * 本电池需要 VisibleRenderableRegistry::computeShadowManifestPartKey 的
//     宿主机替身（模块 .cpp 调用该纯静态哈希）。两侧共用同一替身，因此差分
//     隔离出的正是 M2-5 搬动的那段文本。
//   * 诊断门 OFF（默认 env）时两侧都早退、34 字段必须全 0 —— 本条由本电池
//     显式断言（不是靠双侧互证）。

constexpr size_t kTaxonomyFieldCount = 34u;

const char* const kTaxonomyFieldNames[kTaxonomyFieldCount] = {
    "sourceNone", "sourceDrawTimeCaptured", "sourceSubmitTimeGlobalSlot",
    "sourceSubmitTimeBlendedCache", "sourceSubmitTimePublishedRegistry",
    "sourceSubmitTimeCModelFallback", "sourceOwnedPartSnapshot", "sourceChurn",
    "provTrustedBlendedWriter", "provRawGlobalArena", "provProducerPartPacket",
    "provRangeCopyPoseRebuild", "provCModelFallback", "provUnknown",
    "stablePartSample", "hashChurn", "slotIndexChurn", "countChurn",
    "hashUniqueInWindowMax", "slotIndexUniqueInWindowMax",
    "firstMatrixSmall", "firstMatrixMedium", "firstMatrixLarge",
    "afterStaleRestoreLarge", "liveToLiveLarge", "staleRestoreSubmitted",
    "leaseKeyPayload11CMultiValue", "leaseKeyPaletteCountMultiValue",
    "strictSliceSample", "strictSliceHashChurn", "strictSliceCountChurn",
    "strictSliceFirstMatrixSmall", "strictSliceFirstMatrixMedium",
    "strictSliceFirstMatrixLarge",
};

static_assert(std::is_trivially_copyable<dxvk::War3ShadowCaptureStats>::value,
              "M2-5 差分依赖整结构 memcmp：War3ShadowCaptureStats 必须平凡可复制");

// --probe-taxonomy 打印时取**绝对值**而非增量的字段下标（两个 max 语义字段）。
const size_t kTaxonomyAbsoluteFields[] = {18u, 19u};

void SnapshotTaxonomyFields(const dxvk::War3ShadowCaptureStats& stats,
                            uint64_t (&out)[kTaxonomyFieldCount]) {
  out[0] = stats.semanticSceneSubmittedSkinnedPaletteSourceNoneCount;
  out[1] = stats.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount;
  out[2] = stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount;
  out[3] = stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount;
  out[4] = stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount;
  out[5] = stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount;
  out[6] = stats.semanticSceneSubmittedSkinnedPaletteSourceOwnedPartSnapshotCount;
  out[7] = stats.semanticSceneSubmittedSkinnedPaletteSourceChurnCount;
  out[8] = stats.semanticSceneSubmittedSkinnedPaletteProvenanceTrustedBlendedWriterCount;
  out[9] = stats.semanticSceneSubmittedSkinnedPaletteProvenanceRawGlobalArenaCount;
  out[10] = stats.semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount;
  out[11] = stats.semanticSceneSubmittedSkinnedPaletteProvenanceRangeCopyPoseRebuildCount;
  out[12] = stats.semanticSceneSubmittedSkinnedPaletteProvenanceCModelFallbackCount;
  out[13] = stats.semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount;
  out[14] = stats.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount;
  out[15] = stats.semanticSceneSubmittedSkinnedPaletteHashChurnCount;
  out[16] = stats.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount;
  out[17] = stats.semanticSceneSubmittedSkinnedPaletteCountChurnCount;
  out[18] = stats.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax;
  out[19] = stats.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax;
  out[20] = stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount;
  out[21] = stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount;
  out[22] = stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount;
  out[23] = stats.semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount;
  out[24] = stats.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount;
  out[25] = stats.semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount;
  out[26] = stats.semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount;
  out[27] = stats.semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount;
  out[28] = stats.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount;
  out[29] = stats.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount;
  out[30] = stats.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount;
  out[31] = stats.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount;
  out[32] = stats.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount;
  out[33] = stats.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount;
}

// 只允许这 34 个字段被写：本表与 SnapshotTaxonomyFields 一一对应，缺一即编译期差错位。
static_assert(kTaxonomyFieldCount == 34u, "M2-5 taxonomy 字段数必须是 34");

dxvk::War3ShadowCaptureStats g_taxModuleStats = {};
dxvk::War3ShadowCaptureStats g_taxLegacyStats = {};

struct TaxonomyInputs {
  bool skinned = true;
  int32_t paletteSource = 0;
  const dxvk::war3::render::CurrentDrawAuthoritativeSample* currentDrawSample = nullptr;
  dxvk::war3::render::PaletteProvenance provenance =
      dxvk::war3::render::PaletteProvenance::Unknown;
  const std::vector<dxvk::Matrix4>* effectiveCanonicalPalette = nullptr;
  uint32_t effectiveCanonicalPaletteCount = 0u;
  uint32_t paletteSlotIndexThisSubmit = 0xFFFFFFFFu;
  uint64_t submittedPaletteHash = 0u;
  bool fromStalePoseRestore = false;
  uint64_t frameSerial = 0u;
};

void TaxonomyCallModule(const TaxonomyInputs& in) {
  sem::War3EmitSemanticPaletteTaxonomy(
      g_taxModuleStats, in.skinned,
      static_cast<sem::War3SemanticPaletteSource>(in.paletteSource),
      in.currentDrawSample, in.provenance, in.effectiveCanonicalPalette,
      in.effectiveCanonicalPaletteCount, in.paletteSlotIndexThisSubmit,
      in.submittedPaletteHash, in.fromStalePoseRestore, in.frameSerial);
}

void TaxonomyCallLegacy(const TaxonomyInputs& in) {
  legacy::War3EmitSemanticPaletteTaxonomy(
      g_taxLegacyStats, in.skinned,
      static_cast<legacy::War3SemanticPaletteSource>(in.paletteSource),
      in.currentDrawSample, in.provenance, in.effectiveCanonicalPalette,
      in.effectiveCanonicalPaletteCount, in.paletteSlotIndexThisSubmit,
      in.submittedPaletteHash, in.fromStalePoseRestore, in.frameSerial);
}

void TaxonomyCall(const TaxonomyInputs& in) {
  TaxonomyCallModule(in);
  TaxonomyCallLegacy(in);
}

void DiffTaxonomyState(const char* what, int line) {
  uint64_t moduleFields[kTaxonomyFieldCount];
  uint64_t legacyFields[kTaxonomyFieldCount];
  SnapshotTaxonomyFields(g_taxModuleStats, moduleFields);
  SnapshotTaxonomyFields(g_taxLegacyStats, legacyFields);
  for (size_t i = 0u; i < kTaxonomyFieldCount; ++i) {
    ++g_checks;
    if (moduleFields[i] != legacyFields[i]) {
      ++g_failures;
      std::cerr << "DIFF line " << line << " " << what << "."
                << kTaxonomyFieldNames[i] << ": module=" << moduleFields[i]
                << " legacy=" << legacyFields[i] << "\n";
    }
  }
}

void DiffTaxonomyStructBytes(const char* what, int line) {
  ++g_checks;
  if (std::memcmp(&g_taxModuleStats, &g_taxLegacyStats,
                  sizeof(dxvk::War3ShadowCaptureStats)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what
              << ": War3ShadowCaptureStats 整结构字节不一致\n";
  }
}

bool TaxonomyStatsAllZero(const dxvk::War3ShadowCaptureStats& stats) {
  uint64_t fields[kTaxonomyFieldCount];
  SnapshotTaxonomyFields(stats, fields);
  for (size_t i = 0u; i < kTaxonomyFieldCount; ++i) {
    if (fields[i] != 0u)
      return false;
  }
  return true;
}

// ---- 电池夹具：一个可复用的 current-draw 样本 + 3 个 palette 向量 ----------
dxvk::war3::render::CurrentDrawAuthoritativeSample g_taxScratchSample = {};
std::vector<dxvk::Matrix4> g_taxPalettes[3];

void TaxonomyFixtureReset() {
  g_taxScratchSample = dxvk::war3::render::CurrentDrawAuthoritativeSample{};
  g_taxScratchSample.contract.known = true;
  g_taxScratchSample.contract.payloadWord11C = 0x11u;
  g_taxScratchSample.contract.capturedPaletteCount = 1u;
  // [0] 空；[1] 单矩阵（translation 0）；[2] 两矩阵（translation 递增）。
  g_taxPalettes[0].clear();
  g_taxPalettes[1].assign(1u, dxvk::Matrix4{});
  g_taxPalettes[2].assign(2u, dxvk::Matrix4{});
  g_taxPalettes[2][0][3][0] = 0.0f;
  g_taxPalettes[2][1][3][0] = 0.25f;
}

void TaxonomySetScratchKey(uint64_t jHandle, uint32_t payloadWord11C,
                           uint32_t capturedPaletteCount) {
  g_taxScratchSample.contract.jHandle = jHandle;
  g_taxScratchSample.contract.payloadWord11C = payloadWord11C;
  g_taxScratchSample.contract.capturedPaletteCount = capturedPaletteCount;
}

// 单次调用：给定 key/来源/palette/帧号，驱动两侧并逐字段对照。
void TaxonomyStep(uint64_t jHandle, int32_t source, uint64_t hash,
                  uint32_t slot, uint32_t count, const std::vector<dxvk::Matrix4>* pal,
                  float firstTx, bool stale, uint64_t frame, int line) {
  TaxonomySetScratchKey(jHandle, g_taxScratchSample.contract.payloadWord11C,
                        g_taxScratchSample.contract.capturedPaletteCount);
  TaxonomyInputs in;
  in.skinned = true;
  in.paletteSource = source;
  in.currentDrawSample = &g_taxScratchSample;
  in.provenance = dxvk::war3::render::PaletteProvenance::TrustedBlendedWriter;
  in.effectiveCanonicalPalette = pal;
  in.effectiveCanonicalPaletteCount = count;
  in.paletteSlotIndexThisSubmit = slot;
  in.submittedPaletteHash = hash;
  in.fromStalePoseRestore = stale;
  in.frameSerial = frame;
  if (pal != nullptr && !pal->empty()) {
    const_cast<std::vector<dxvk::Matrix4>*>(pal)->at(0)[3][0] = firstTx;
  }
  TaxonomyCall(in);
  DiffTaxonomyState("TaxSeq", line);
}

// ---- 1) 系统网格：来源 × provenance × 样本 × palette × count × hash × slot
//        × stale × frame × skinned ----------------
void DiffTaxonomySystematic() {
  const int32_t sources[] = {0, 1, 2, 3, 4, 5, 6};
  const uint32_t provs[] = {0u, 1u, 2u, 3u, 4u, 5u, 9u};
  const int32_t palIdx[] = {-1, 0, 1, 2};
  const uint32_t counts[] = {0u, 1u, 2u};
  const uint64_t hashes[] = {0u, 1u, 0x123456789ABCDEF0ull};
  const uint32_t slots[] = {0xFFFFFFFFu, 0u, 5u};
  const bool stales[] = {false, true};
  const uint64_t frames[] = {0u, 1u};
  const bool skinnedVals[] = {false, true};
  for (bool skinned : skinnedVals) {
    for (int32_t source : sources) {
      for (uint32_t prov : provs) {
        for (int32_t p : palIdx) {
          for (uint32_t count : counts) {
            for (uint64_t hash : hashes) {
              for (uint32_t slot : slots) {
                for (bool stale : stales) {
                  for (uint64_t frame : frames) {
                    for (int32_t sampleIdx = -1; sampleIdx < 2; ++sampleIdx) {
                      g_taxScratchSample.contract.jHandle =
                          sampleIdx < 0 ? 0u : uint64_t(0x200u + sampleIdx);
                      g_taxScratchSample.contract.payloadWord11C = 0x11u;
                      g_taxScratchSample.contract.capturedPaletteCount = 1u;
                      TaxonomyInputs in;
                      in.skinned = skinned;
                      in.paletteSource = source;
                      in.currentDrawSample =
                          sampleIdx < 0 ? nullptr : &g_taxScratchSample;
                      in.provenance =
                          static_cast<dxvk::war3::render::PaletteProvenance>(prov);
                      in.effectiveCanonicalPalette =
                          p < 0 ? nullptr : &g_taxPalettes[p];
                      in.effectiveCanonicalPaletteCount = count;
                      in.submittedPaletteHash = hash;
                      in.paletteSlotIndexThisSubmit = slot;
                      in.fromStalePoseRestore = stale;
                      in.frameSerial = frame;
                      TaxonomyCall(in);
                      DiffTaxonomyState("TaxGrid", __LINE__);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  DiffTaxonomyStructBytes("taxonomy-grid", __LINE__);
}

// ---- 2) 定长序列：同 key 连续帧（window 填满/回绕、churn、delta 三档、stale
//        归因）、lease key 多值、同槽异 key 替换、诊断门 OFF 全 0 --------
void DiffTaxonomySequences() {
  TaxonomyFixtureReset();
  // (1) 同一 stable part key 的 10 帧序列。
  const uint64_t key = 0x51u;
  struct SeqStep {
    uint64_t hash; uint32_t slot; uint32_t count; int32_t source;
    float tx; bool stale;
  };
  const SeqStep steps[] = {
      {0xAAu, 3u, 1u, 2, 0.00f, false},
      {0xAAu, 3u, 1u, 2, 0.00f, false},
      {0xBBu, 3u, 1u, 2, 0.05f, false},
      {0xCCu, 4u, 2u, 2, 0.30f, false},
      {0xDDu, 5u, 3u, 2, 1.50f, false},
      {0xEEu, 5u, 3u, 2, 4.50f, true},
      {0xFFu, 5u, 3u, 3, 7.50f, false},
      {0xFFu, 5u, 3u, 3, 7.55f, false},
      {0xFFu, 6u, 4u, 2, 7.60f, false},
      {0x01u, 6u, 4u, 2, 7.65f, false},
  };
  uint64_t frame = 1u;
  for (size_t i = 0u; i < sizeof(steps) / sizeof(steps[0]); ++i) {
    TaxonomyStep(key, steps[i].source, steps[i].hash, steps[i].slot,
                 steps[i].count, &g_taxPalettes[1], steps[i].tx,
                 steps[i].stale, frame++, __LINE__);
  }
  DiffTaxonomyStructBytes("taxonomy-seq-window", __LINE__);

  // (2) 同 key 同帧下 payload11C / capturedPaletteCount 多值归因。
  const uint64_t leaseKey = 0x52u;
  const uint64_t leaseFrame = 7u;
  const uint32_t payloads[5] = {1u, 1u, 2u, 3u, 3u};
  const uint32_t counts[5] = {5u, 5u, 5u, 6u, 6u};
  for (size_t i = 0u; i < 5u; ++i) {
    TaxonomySetScratchKey(leaseKey, payloads[i], counts[i]);
    TaxonomyStep(leaseKey, 2, 0xAAu, 3u, 1u, &g_taxPalettes[1], 0.0f, false,
                 leaseFrame, __LINE__);
  }
  DiffTaxonomyStructBytes("taxonomy-seq-lease", __LINE__);

  // (3) 两个不同 key 命中同一 8192 槽（同一探针槽位的替换路径）。
  std::map<uint64_t, uint64_t> slotOwner;
  uint64_t k1 = 0u;
  uint64_t k2 = 0u;
  for (uint64_t candidate = 1u; candidate <= 200000u; ++candidate) {
    const uint64_t candidateKey = TaxonomyKeyFor(candidate);
    if (candidateKey == 0u)
      continue;
    const uint64_t slotBits = candidateKey & 8191ull;
    const auto it = slotOwner.find(slotBits);
    if (it == slotOwner.end()) {
      slotOwner[slotBits] = candidate;
      continue;
    }
    if (TaxonomyKeyFor(it->second) == candidateKey)
      continue;
    k1 = it->second;
    k2 = candidate;
    break;
  }
  if (k1 != 0u && k2 != 0u) {
    TaxonomyStep(k1, 2, 0xAAu, 3u, 1u, &g_taxPalettes[1], 0.0f, false, 11u,
                 __LINE__);
    TaxonomyStep(k2, 2, 0xBBu, 3u, 1u, &g_taxPalettes[1], 0.0f, false, 12u,
                 __LINE__);
    TaxonomyStep(k1, 2, 0xAAu, 3u, 1u, &g_taxPalettes[1], 0.0f, false, 13u,
                 __LINE__);
    TaxonomyStep(k2, 2, 0xBBu, 3u, 1u, &g_taxPalettes[1], 0.0f, false, 14u,
                 __LINE__);
    DiffTaxonomyStructBytes("taxonomy-seq-slot-collision", __LINE__);
  }

  // (4) 诊断门 OFF 时两侧必须都保持全 0（第一道门本身，不靠双侧互证）。
  if (!sem::War3SemanticPaletteDiagnosticsRuntime()) {
    ++g_checks;
    if (!TaxonomyStatsAllZero(g_taxModuleStats) ||
        !TaxonomyStatsAllZero(g_taxLegacyStats)) {
      ++g_failures;
      std::cerr << "DIFF line " << __LINE__
                << " taxonomy-gate-off: 诊断门 OFF 时发射块仍写 stats" << "\n";
    }
  }
}

// ---- 3) 定种子随机世界：先用 9000 个互异 key 填满/越过 8192 槽表，再做
//        250,000 轮 splitmix64 随机（key 池 1700，覆盖 sameKey 与 churn）--
uint64_t g_taxRngState = 0x243F6A8885A308D3ull;

uint64_t TaxNextU64() {
  uint64_t z = (g_taxRngState += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint32_t TaxNextU32() {
  return uint32_t(TaxNextU64() >> 32);
}

void DiffTaxonomyRandom() {
  TaxonomyFixtureReset();
  constexpr uint32_t kFillKeys = 9000u;
  for (uint32_t i = 0u; i < kFillKeys; ++i) {
    TaxonomySetScratchKey(1000u + uint64_t(i), 0x11u, 1u);
    TaxonomyInputs in;
    in.skinned = true;
    in.paletteSource = 2;
    in.currentDrawSample = &g_taxScratchSample;
    in.provenance = dxvk::war3::render::PaletteProvenance::TrustedBlendedWriter;
    in.effectiveCanonicalPalette = &g_taxPalettes[1];
    in.effectiveCanonicalPaletteCount = 1u;
    in.paletteSlotIndexThisSubmit = 3u;
    in.submittedPaletteHash = 0xAAu + i;
    in.fromStalePoseRestore = (i & 1u) != 0u;
    in.frameSerial = i + 1u;
    TaxonomyCall(in);
    DiffTaxonomyState("TaxFill", __LINE__);
    if ((i & 63u) == 0u)
      DiffTaxonomyStructBytes("taxonomy-fill", __LINE__);
  }
  DiffTaxonomyStructBytes("taxonomy-fill-total", __LINE__);

  constexpr uint32_t kRounds = 250000u;
  constexpr uint32_t kKeyPool = 1700u;
  static const float kTx[] = {0.0f, 0.05f, 0.30f, 1.50f, -2.0f};
  for (uint32_t i = 0u; i < kRounds; ++i) {
    const uint64_t r0 = TaxNextU64();
    const uint64_t r1 = TaxNextU64();
    const uint64_t r2 = TaxNextU64();
    const uint32_t pick = uint32_t(r0 % kKeyPool);
    TaxonomySetScratchKey(5000u + uint64_t(pick), uint32_t(r1 & 0x3u),
                          uint32_t((r1 >> 8) & 0x3u));
    TaxonomyInputs in;
    in.skinned = true;
    in.paletteSource = int32_t(r2 % 7ull);
    in.currentDrawSample = (r0 & 0x8000u) != 0u ? nullptr : &g_taxScratchSample;
    in.provenance = static_cast<dxvk::war3::render::PaletteProvenance>(
        uint32_t((r1 >> 16) % 7ull));
    in.effectiveCanonicalPalette =
        (r1 & 0x100u) != 0u ? nullptr : &g_taxPalettes[(r0 >> 8) % 3u];
    in.effectiveCanonicalPaletteCount = uint32_t(r2 & 0x3u);
    in.paletteSlotIndexThisSubmit =
        (r2 & 0x400u) != 0u ? 0xFFFFFFFFu : uint32_t(r0 & 0x7u);
    in.submittedPaletteHash = (r2 & 0x800u) != 0u ? 0u : r1;
    in.fromStalePoseRestore = (r0 & 0x40u) != 0u;
    in.frameSerial = (r0 >> 16) % 4ull;
    if (in.effectiveCanonicalPalette != nullptr &&
        !in.effectiveCanonicalPalette->empty()) {
      const_cast<std::vector<dxvk::Matrix4>*>(in.effectiveCanonicalPalette)
          ->at(0)[3][0] = kTx[(r2 >> 4) % 5u];
    }
    TaxonomyCall(in);
    DiffTaxonomyState("TaxRandom", __LINE__);
    if ((i & 63u) == 0u)
      DiffTaxonomyStructBytes("taxonomy-random", __LINE__);
  }
  DiffTaxonomyStructBytes("taxonomy-random-total", __LINE__);
}

// ---- 4) --probe-taxonomy：固定场景的 module/legacy 逐字段对照（绝对值），
//        由独立门禁与**独立期望值表**核对；诊断门 OFF 时必须全 0 --------
struct TaxProbeCall {
  uint64_t jHandle;
  uint32_t payload11C;
  uint32_t capturedCount;
  int32_t source;
  int32_t prov;
  uint64_t hash;
  uint32_t slot;
  uint32_t count;
  int32_t palIdx;
  float tx;
  bool stale;
  uint64_t frame;
};

void PrintTaxProbeRow(const char* name, const TaxProbeCall* calls,
                      size_t callCount) {
  g_taxModuleStats = dxvk::War3ShadowCaptureStats{};
  g_taxLegacyStats = dxvk::War3ShadowCaptureStats{};
  for (size_t i = 0u; i < callCount; ++i) {
    const TaxProbeCall& c = calls[i];
    TaxonomySetScratchKey(c.jHandle, c.payload11C, c.capturedCount);
    TaxonomyInputs in;
    in.skinned = true;
    in.paletteSource = c.source;
    in.currentDrawSample = c.jHandle == 0u ? nullptr : &g_taxScratchSample;
    in.provenance =
        static_cast<dxvk::war3::render::PaletteProvenance>(uint32_t(c.prov));
    in.effectiveCanonicalPalette =
        c.palIdx < 0 ? nullptr : &g_taxPalettes[c.palIdx];
    in.effectiveCanonicalPaletteCount = c.count;
    in.paletteSlotIndexThisSubmit = c.slot;
    in.submittedPaletteHash = c.hash;
    in.fromStalePoseRestore = c.stale;
    in.frameSerial = c.frame;
    if (c.palIdx >= 0 && !g_taxPalettes[c.palIdx].empty())
      g_taxPalettes[c.palIdx][0][3][0] = c.tx;
    TaxonomyCall(in);
  }
  uint64_t moduleFields[kTaxonomyFieldCount];
  uint64_t legacyFields[kTaxonomyFieldCount];
  SnapshotTaxonomyFields(g_taxModuleStats, moduleFields);
  SnapshotTaxonomyFields(g_taxLegacyStats, legacyFields);
  for (size_t i = 0u; i < kTaxonomyFieldCount; ++i) {
    ++g_checks;
    if (moduleFields[i] != legacyFields[i]) {
      ++g_failures;
      std::cerr << "DIFF probe-taxonomy " << name << "."
                << kTaxonomyFieldNames[i] << ": module=" << moduleFields[i]
                << " legacy=" << legacyFields[i] << "\n";
    }
  }
  std::cout << "probe-taxonomy " << name << " module=";
  for (size_t i = 0u; i < kTaxonomyFieldCount; ++i)
    std::cout << (i == 0u ? "" : ",") << moduleFields[i];
  std::cout << " legacy=";
  for (size_t i = 0u; i < kTaxonomyFieldCount; ++i)
    std::cout << (i == 0u ? "" : ",") << legacyFields[i];
  std::cout << "\n";
}

TaxProbeCall TaxProbe(uint64_t jHandle, int32_t source, int32_t prov,
                      uint64_t hash, uint32_t slot, uint32_t count,
                      int32_t palIdx, float tx, bool stale, uint64_t frame) {
  TaxProbeCall c{};
  c.jHandle = jHandle;
  c.payload11C = 0x11u;
  c.capturedCount = 1u;
  c.source = source;
  c.prov = prov;
  c.hash = hash;
  c.slot = slot;
  c.count = count;
  c.palIdx = palIdx;
  c.tx = tx;
  c.stale = stale;
  c.frame = frame;
  return c;
}

int RunProbeTaxonomy() {
  TaxonomyFixtureReset();
  // 每行的 key 都是本进程内**首次**出现（9000000+ 区间），因此首调用必然走
  // "新 key → 重置 entry"分支，期望值是确定的绝对量。
  const TaxProbeCall rNone[] = {
      TaxProbe(0u, 0, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rDtcUnknown[] = {
      TaxProbe(9000001u, 1, 9, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rDtcTrusted[] = {
      TaxProbe(9000002u, 1, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rDtcNullSample[] = {
      TaxProbe(0u, 1, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rOwned[] = {
      TaxProbe(0u, 6, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rSources[] = {
      TaxProbe(0u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(0u, 3, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(0u, 4, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(0u, 5, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rStablePair[] = {
      TaxProbe(9000003u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000003u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 2u),
  };
  const TaxProbeCall rHashChurnLarge[] = {
      TaxProbe(9000004u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000004u, 2, 1, 0xBBu, 3u, 1u, 1, 2.0f, false, 2u),
  };
  TaxProbeCall leaseFirst =
      TaxProbe(9000005u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 7u);
  leaseFirst.payload11C = 1u;
  leaseFirst.capturedCount = 5u;
  TaxProbeCall leaseSecond = leaseFirst;
  leaseSecond.payload11C = 2u;
  leaseSecond.capturedCount = 6u;
  const TaxProbeCall rLeaseMulti[] = {leaseFirst, leaseSecond};
  const TaxProbeCall rStaleRestore[] = {
      TaxProbe(9000006u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, true, 1u),
  };
  const TaxProbeCall rWindowWrap[] = {
      TaxProbe(9000007u, 2, 1, 0x01u, 1u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000007u, 2, 1, 0x02u, 2u, 1u, 1, 0.0f, false, 2u),
      TaxProbe(9000007u, 2, 1, 0x03u, 3u, 1u, 1, 0.0f, false, 3u),
      TaxProbe(9000007u, 2, 1, 0x04u, 4u, 1u, 1, 0.0f, false, 4u),
      TaxProbe(9000007u, 2, 1, 0x05u, 5u, 1u, 1, 0.0f, false, 5u),
  };
  const TaxProbeCall rProv245[] = {
      TaxProbe(9000008u, 1, 2, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000009u, 1, 4, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000010u, 1, 5, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
  };
  const TaxProbeCall rSourceChurn[] = {
      TaxProbe(9000011u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000011u, 3, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 2u),
  };
  const TaxProbeCall rCountChurn[] = {
      TaxProbe(9000012u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000012u, 2, 1, 0xAAu, 3u, 2u, 1, 0.0f, false, 2u),
  };
  const TaxProbeCall rMediumDelta[] = {
      TaxProbe(9000013u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, false, 1u),
      TaxProbe(9000013u, 2, 1, 0xBBu, 3u, 1u, 1, 0.3f, false, 2u),
  };
  const TaxProbeCall rStaleThenLiveLarge[] = {
      TaxProbe(9000014u, 2, 1, 0xAAu, 3u, 1u, 1, 0.0f, true, 1u),
      TaxProbe(9000014u, 2, 1, 0xBBu, 3u, 1u, 1, 3.0f, false, 2u),
      TaxProbe(9000014u, 2, 1, 0xCCu, 3u, 1u, 1, 6.0f, false, 3u),
  };
  PrintTaxProbeRow("none", rNone, 1u);
  PrintTaxProbeRow("dtc-unknown-outofrange", rDtcUnknown, 1u);
  PrintTaxProbeRow("dtc-trusted", rDtcTrusted, 1u);
  PrintTaxProbeRow("dtc-null-sample", rDtcNullSample, 1u);
  PrintTaxProbeRow("owned-part-snapshot", rOwned, 1u);
  PrintTaxProbeRow("sources-slot-blended-published-cmodel", rSources, 4u);
  PrintTaxProbeRow("global-stable-pair", rStablePair, 2u);
  PrintTaxProbeRow("hash-churn-large-delta", rHashChurnLarge, 2u);
  PrintTaxProbeRow("lease-multi-both", rLeaseMulti, 2u);
  PrintTaxProbeRow("stale-restore", rStaleRestore, 1u);
  PrintTaxProbeRow("window-wrap", rWindowWrap, 5u);
  PrintTaxProbeRow("dtc-provenance-2-4-5", rProv245, 3u);
  PrintTaxProbeRow("source-churn", rSourceChurn, 2u);
  PrintTaxProbeRow("count-churn", rCountChurn, 2u);
  PrintTaxProbeRow("medium-delta", rMediumDelta, 2u);
  PrintTaxProbeRow("stale-then-live-large", rStaleThenLiveLarge, 3u);
  return g_failures == 0u ? 0 : 1;
}

// ===========================================================================
// M2-4：调色板 compose-policy 判定 / env getter 余项的差分
//   legacy 参考见 war3_live_palette_selection_m2_4_legacy_reference.inc
//   （生成器 --m2-4 产出；与前面四份 .inc 共享 m2_legacy_reference 命名空间）。
// 比较口径：每个输入调用一次，8 个结果字段逐字段整数 / 位模式精确相等，并对同一
// 调用的整个结果结构 memcmp；每个场景段结束再对两侧累计结构做一次 memcmp。
// A3 / A4 的 IsReadableRange 走本文件的可读区间替身（两侧共用同一替身世界）。
// ===========================================================================

// 4096 个 Matrix4 的 arena。pointer 重载一律读这个 arena（而不是 vector 的
// data()），因此 count 取到 4097 时循环最多读前 4 个元素也不会越界；count 超出
// arena 注册范围且 checkReadable=true 时 IsReadableRange 先失败、函数在读取前
// 早退。arena 前 8 个元素在 M24Prepare 里跟随当前 palette。
alignas(16) std::array<dxvk::Matrix4, 4096> g_m24Arena = {};

struct M24Result {
  uint64_t hash4;
  uint32_t radiusBits;
  uint32_t distBits;
  uint32_t finiteWorld;
  uint32_t finitePal0;
  uint32_t readable;
  uint32_t looksPtr;
  uint32_t looksVec;
};

M24Result g_m24ModuleAcc = {};
M24Result g_m24LegacyAcc = {};

dxvk::Matrix4 M24Matrix(float tx, float ty, float tz) {
  dxvk::Matrix4 m;
  m[3] = dxvk::Vector4(tx, ty, tz, 1.0f);
  return m;
}

void M24Accumulate(const M24Result& m, const M24Result& l) {
  g_m24ModuleAcc.hash4 += m.hash4;
  g_m24ModuleAcc.radiusBits += m.radiusBits;
  g_m24ModuleAcc.distBits += m.distBits;
  g_m24ModuleAcc.finiteWorld += m.finiteWorld;
  g_m24ModuleAcc.finitePal0 += m.finitePal0;
  g_m24ModuleAcc.readable += m.readable;
  g_m24ModuleAcc.looksPtr += m.looksPtr;
  g_m24ModuleAcc.looksVec += m.looksVec;
  g_m24LegacyAcc.hash4 += l.hash4;
  g_m24LegacyAcc.radiusBits += l.radiusBits;
  g_m24LegacyAcc.distBits += l.distBits;
  g_m24LegacyAcc.finiteWorld += l.finiteWorld;
  g_m24LegacyAcc.finitePal0 += l.finitePal0;
  g_m24LegacyAcc.readable += l.readable;
  g_m24LegacyAcc.looksPtr += l.looksPtr;
  g_m24LegacyAcc.looksVec += l.looksVec;
}

void M24ResetAccumulators() {
  g_m24ModuleAcc = M24Result{};
  g_m24LegacyAcc = M24Result{};
}

M24Result M24Run(bool useLegacy, const dxvk::Matrix4& paletteFirst,
                 const dxvk::Matrix4& worldTransform,
                 const std::vector<dxvk::Matrix4>& palette,
                 const dxvk::Matrix4* palettePtr, uint32_t paletteCount,
                 uint8_t objectKind, bool checkReadable) {
  M24Result out = {};
  if (useLegacy) {
    out.hash4 = legacy::War3SemanticHashMatrix4(worldTransform);
    out.radiusBits = FloatBits(
        legacy::War3SemanticBoundsRadiusForObjectKind(objectKind));
    out.distBits = FloatBits(
        legacy::War3SemanticTranslationDistanceSq(paletteFirst, worldTransform));
    out.finiteWorld =
        legacy::War3SemanticTranslationFinite(worldTransform) ? 1u : 0u;
    out.finitePal0 =
        legacy::War3SemanticTranslationFinite(paletteFirst) ? 1u : 0u;
    out.readable =
        legacy::War3SemanticPaletteStorageReadable(palette) ? 1u : 0u;
    out.looksPtr = legacy::War3SemanticPaletteLooksModelLocal(
                        palettePtr, paletteCount, worldTransform, objectKind,
                        checkReadable)
                        ? 1u
                        : 0u;
    out.looksVec = legacy::War3SemanticPaletteLooksModelLocal(
                       palette, worldTransform, objectKind)
                       ? 1u
                       : 0u;
  } else {
    out.hash4 = sem::War3SemanticHashMatrix4(worldTransform);
    out.radiusBits =
        FloatBits(sem::War3SemanticBoundsRadiusForObjectKind(objectKind));
    out.distBits = FloatBits(
        sem::War3SemanticTranslationDistanceSq(paletteFirst, worldTransform));
    out.finiteWorld =
        sem::War3SemanticTranslationFinite(worldTransform) ? 1u : 0u;
    out.finitePal0 =
        sem::War3SemanticTranslationFinite(paletteFirst) ? 1u : 0u;
    out.readable =
        sem::War3SemanticPaletteStorageReadable(palette) ? 1u : 0u;
    out.looksPtr = sem::War3SemanticPaletteLooksModelLocal(
                       palettePtr, paletteCount, worldTransform, objectKind,
                       checkReadable)
                       ? 1u
                       : 0u;
    out.looksVec = sem::War3SemanticPaletteLooksModelLocal(
                       palette, worldTransform, objectKind)
                       ? 1u
                       : 0u;
  }
  return out;
}

void DiffM24Case(const char* what, const dxvk::Matrix4& paletteFirst,
                 const dxvk::Matrix4& worldTransform,
                 const std::vector<dxvk::Matrix4>& palette,
                 const dxvk::Matrix4* palettePtr, uint32_t paletteCount,
                 uint8_t objectKind, bool checkReadable, int line) {
  const M24Result m = M24Run(false, paletteFirst, worldTransform, palette,
                             palettePtr, paletteCount, objectKind,
                             checkReadable);
  const M24Result l = M24Run(true, paletteFirst, worldTransform, palette,
                             palettePtr, paletteCount, objectKind,
                             checkReadable);
  ++g_checks;
  if (std::memcmp(&m, &l, sizeof(M24Result)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what
              << ": M24Result memcmp mismatch\n";
  }
  DiffU64("M24.hash4", m.hash4, l.hash4, line);
  DiffU64("M24.radiusBits", m.radiusBits, l.radiusBits, line);
  DiffU64("M24.distBits", m.distBits, l.distBits, line);
  DiffU64("M24.finiteWorld", m.finiteWorld, l.finiteWorld, line);
  DiffU64("M24.finitePal0", m.finitePal0, l.finitePal0, line);
  DiffU64("M24.readable", m.readable, l.readable, line);
  DiffU64("M24.looksPtr", m.looksPtr, l.looksPtr, line);
  DiffU64("M24.looksVec", m.looksVec, l.looksVec, line);
  M24Accumulate(m, l);
}

void DiffM24StructBytes(const char* what, int line) {
  ++g_checks;
  if (std::memcmp(&g_m24ModuleAcc, &g_m24LegacyAcc, sizeof(M24Result)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " M24-acc " << what << "\n";
  }
}

void M24Prepare(const std::vector<dxvk::Matrix4>& palette, bool readable) {
  ClearReadable();
  for (size_t i = 0u; i < 8u && i < palette.size(); ++i)
    g_m24Arena[i] = palette[i];
  if (!readable)
    return;
  RegisterReadable(g_m24Arena.data(),
                   g_m24Arena.size() * sizeof(dxvk::Matrix4));
  if (!palette.empty())
    RegisterReadable(palette.data(), palette.size() * sizeof(dxvk::Matrix4));
}

std::vector<dxvk::Matrix4> M24Palette(uint32_t variant) {
  std::vector<dxvk::Matrix4> palette;
  switch (variant) {
    case 0u:
      break;
    case 1u:
      palette.push_back(M24Matrix(64.0f, 0.0f, 0.0f));
      break;
    case 2u:
      palette.push_back(M24Matrix(64.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(96.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(48.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(80.0f, 0.0f, 0.0f));
      break;
    case 3u:
      palette.push_back(M24Matrix(64.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(96.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(BitsToFloat(0x7FC00000u), 0.0f, 0.0f));
      palette.push_back(M24Matrix(80.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(48.0f, 0.0f, 0.0f));
      break;
    case 4u:
      for (uint32_t i = 0u; i < 4u; ++i)
        palette.push_back(
            M24Matrix(1000000.0f + float(i) * 4096.0f, 0.0f, 0.0f));
      break;
    case 5u:
      palette.push_back(M24Matrix(64.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(BitsToFloat(0x7F800000u), 0.0f, 0.0f));
      palette.push_back(M24Matrix(48.0f, 0.0f, 0.0f));
      break;
    case 6u:
      // 平移 500/600/700：palette 自身仍在 localMagLimit(1024) 之内，但与
      // world=(5,0,0) 的距离平方 245025 已超过 Unit 阈值 152100 -> 判定成立。
      palette.push_back(M24Matrix(500.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(600.0f, 0.0f, 0.0f));
      palette.push_back(M24Matrix(700.0f, 0.0f, 0.0f));
      break;
    default:
      break;
  }
  return palette;
}

dxvk::Matrix4 M24World(uint32_t idx) {
  switch (idx) {
    case 0u:
      return M24Matrix(0.0f, 0.0f, 0.0f);
    case 1u:
      return M24Matrix(4.0001f, 0.0f, 0.0f);
    case 2u:
      return M24Matrix(4.0f, 0.0f, 0.0f);
    case 3u:
      return M24Matrix(1000.0f, 0.0f, 0.0f);
    case 4u:
      return M24Matrix(4096.0f, 0.0f, 0.0f);
    case 5u: {
      dxvk::Matrix4 m = M24Matrix(1000.0f, 0.0f, 0.0f);
      m[3].x = BitsToFloat(0x7FC00000u);
      return m;
    }
    case 6u: {
      dxvk::Matrix4 m = M24Matrix(1000.0f, 0.0f, 0.0f);
      m[3].z = BitsToFloat(0x7F800000u);
      return m;
    }
    case 7u:
      // worldMagSq = 25：落在 A4 "worldMagSq > 16" 门与更高门限之间，
      // 专门用来咬住该门限的常量漂移。
      return M24Matrix(5.0f, 0.0f, 0.0f);
    default:
      return M24Matrix(-1000.0f, 1.0f, 2.0f);
  }
}

void DiffM24Systematic() {
  M24ResetAccumulators();
  const uint32_t counts[] = {0u,  1u,   2u,   3u,   4u,   5u,
                             255u, 256u, 257u, 4096u, 4097u};
  const uint32_t countCount = uint32_t(sizeof(counts) / sizeof(counts[0]));
  for (uint32_t variant = 0u; variant < 7u; ++variant) {
    const std::vector<dxvk::Matrix4> palette = M24Palette(variant);
    for (uint32_t readable = 0u; readable < 2u; ++readable) {
      M24Prepare(palette, readable != 0u);
      const dxvk::Matrix4 first =
          palette.empty() ? M24Matrix(64.0f, 0.0f, 0.0f) : palette[0];
      for (uint32_t worldIdx = 0u; worldIdx < 8u; ++worldIdx) {
        const dxvk::Matrix4 world = M24World(worldIdx);
        for (uint32_t countIdx = 0u; countIdx < countCount; ++countIdx) {
          for (uint32_t kind = 0u; kind < 256u; ++kind) {
            for (uint32_t check = 0u; check < 2u; ++check) {
              DiffM24Case("M24Grid", first, world, palette,
                          g_m24Arena.data(), counts[countIdx],
                          uint8_t(kind), check != 0u, __LINE__);
            }
          }
        }
      }
    }
  }
  DiffM24StructBytes("systematic-grid", __LINE__);

  // pointer 重载的 nullptr 早退分支（各 count x checkReadable）。
  const std::vector<dxvk::Matrix4> emptyPalette;
  M24Prepare(emptyPalette, false);
  for (uint32_t countIdx = 0u; countIdx < countCount; ++countIdx) {
    for (uint32_t check = 0u; check < 2u; ++check) {
      DiffM24Case("M24NullPtr", M24Matrix(64.0f, 0.0f, 0.0f),
                  M24Matrix(1000.0f, 0.0f, 0.0f), emptyPalette, nullptr,
                  counts[countIdx], 2u, check != 0u, __LINE__);
    }
  }
  DiffM24StructBytes("systematic-nullptr", __LINE__);
}

void DiffM24Random() {
  M24ResetAccumulators();
  const uint32_t countPool[8] = {0u, 1u, 2u, 3u, 4u, 5u, 255u, 4097u};
  std::vector<dxvk::Matrix4> pool(8u);
  for (uint32_t round = 0u; round < 200000u; ++round) {
    const uint32_t roll = NextU32();
    for (auto& matrix : pool)
      FillMatrixRandom(matrix, (roll & 1u) != 0u);
    dxvk::Matrix4 world;
    FillMatrixRandom(world, (roll & 2u) != 0u);
    const uint32_t size = roll % 9u;
    const std::vector<dxvk::Matrix4> view(pool.begin(), pool.begin() + size);
    const uint32_t count = countPool[(roll >> 4u) % 8u];
    const uint8_t kind = uint8_t(NextU32() & 0xFFu);
    const bool check = (roll & 8u) != 0u;
    M24Prepare(view, (roll & 16u) != 0u);
    const dxvk::Matrix4 first =
        view.empty() ? M24Matrix(0.0f, 0.0f, 0.0f) : view[0];
    const dxvk::Matrix4* ptr =
        (roll & 32u) != 0u ? nullptr : g_m24Arena.data();
    DiffM24Case("M24Random", first, world, view, ptr, count, kind, check,
                __LINE__);
  }
  DiffM24StructBytes("random", __LINE__);
}

// ---- --probe-m2-4：固定场景的 module/legacy 逐字段对照（绝对值），由独立
//      门禁与独立期望值表核对；A1/A2 的 env 期望值按 env 矩阵登记 --------
struct M24ProbeSpec {
  const char* name;
  uint8_t kind;
  uint32_t count;
  bool checkReadable;
  bool registerReadable;
  bool nullPtr;
  uint32_t worldVariant;
  int32_t paletteVariant;
  uint32_t paletteCount;
};

void PrintM24ProbeRow(const M24ProbeSpec& spec) {
  std::vector<dxvk::Matrix4> palette;
  if (spec.paletteVariant >= 0) {
    switch (spec.paletteVariant) {
      case 0:
        palette.push_back(M24Matrix(64.0f, 0.0f, 0.0f));
        break;
      case 1:
        palette.push_back(M24Matrix(1000.0f, 0.0f, 0.0f));
        break;
      case 2:
        palette.push_back(M24Matrix(1000000.0f, 0.0f, 0.0f));
        break;
      case 3:
        palette.push_back(M24Matrix(BitsToFloat(0x7FC00000u), 0.0f, 0.0f));
        break;
      case 4:
        palette.push_back(M24Matrix(BitsToFloat(0x7F800000u), 0.0f, 0.0f));
        break;
      default:
        break;
    }
    for (uint32_t i = uint32_t(palette.size()); i < spec.paletteCount; ++i)
      palette.push_back(M24Matrix(64.0f + float(i) * 16.0f, 0.0f, 0.0f));
  }
  M24Prepare(palette, spec.registerReadable);
  const dxvk::Matrix4 world = M24World(spec.worldVariant);
  const dxvk::Matrix4 first =
      palette.empty() ? M24Matrix(64.0f, 0.0f, 0.0f) : palette[0];
  const dxvk::Matrix4* ptr = spec.nullPtr ? nullptr : g_m24Arena.data();
  const M24Result m = M24Run(false, first, world, palette, ptr, spec.count,
                             spec.kind, spec.checkReadable);
  const M24Result l = M24Run(true, first, world, palette, ptr, spec.count,
                             spec.kind, spec.checkReadable);
  const uint64_t moduleFields[10] = {
      m.hash4,
      m.radiusBits,
      m.distBits,
      m.finiteWorld,
      m.finitePal0,
      m.readable,
      m.looksPtr,
      m.looksVec,
      sem::War3SemanticPaletteInPlaceAppendRuntime() ? 1u : 0u,
      sem::War3SemanticDrawTimePoseRuntime() ? 1u : 0u,
  };
  const uint64_t legacyFields[10] = {
      l.hash4,
      l.radiusBits,
      l.distBits,
      l.finiteWorld,
      l.finitePal0,
      l.readable,
      l.looksPtr,
      l.looksVec,
      legacy::War3SemanticPaletteInPlaceAppendRuntime() ? 1u : 0u,
      legacy::War3SemanticDrawTimePoseRuntime() ? 1u : 0u,
  };
  ++g_checks;
  if (std::memcmp(moduleFields, legacyFields, sizeof(moduleFields)) != 0) {
    ++g_failures;
    std::cerr << "DIFF probe-m2-4 " << spec.name << ": module/legacy\n";
  }
  std::cout << "probe-m2-4 " << spec.name << " module=";
  for (uint32_t i = 0u; i < 10u; ++i)
    std::cout << (i == 0u ? "" : ",") << moduleFields[i];
  std::cout << " legacy=";
  for (uint32_t i = 0u; i < 10u; ++i)
    std::cout << (i == 0u ? "" : ",") << legacyFields[i];
  std::cout << "\n";
}

int RunProbeM24() {
  const M24ProbeSpec specs[] = {
      // kind 取 ObjectKind 底层值：0=Unknown 1=Unit 2=Building 3=Destructible
      // 4=Item 5=Effect；A4 场景统一用 1（Unit，radius 260 -> guard 390），
      // 使 world(1000) 与 palette(64) 的 distSq 876096 落在阈值 152100 之上，
      // 从而真正走到"model-local 成立"的分支。
      {"a4-local-world-far", 1u, 1u, true, true, false, 3u, 0, 1u},
      {"a4-world-palette-near", 1u, 1u, true, true, false, 3u, 1, 1u},
      {"a4-palette-too-far", 1u, 1u, true, true, false, 3u, 2, 1u},
      {"a4-palette-nan", 1u, 1u, true, true, false, 3u, 3, 1u},
      {"a4-palette-inf", 1u, 1u, true, true, false, 3u, 4, 1u},
      {"a4-empty-palette", 1u, 0u, true, true, false, 3u, -1, 0u},
      {"a4-count-257", 1u, 257u, true, true, false, 3u, 0, 257u},
      {"a4-unreadable", 1u, 1u, true, false, false, 3u, 0, 1u},
      {"a4-check-off-unreadable", 1u, 1u, false, false, false, 3u, 0, 1u},
      {"a4-null-ptr", 1u, 1u, false, false, true, 3u, 0, 1u},
      {"a4-world-nan", 1u, 1u, true, true, false, 5u, 0, 1u},
      {"a4-world-zero", 1u, 1u, true, true, false, 0u, 0, 1u},
      {"a8-kind-0-unknown", 0u, 1u, true, true, false, 3u, 0, 1u},
      {"a8-kind-1-unit", 1u, 1u, true, true, false, 3u, 0, 1u},
      {"a8-kind-2-building", 2u, 1u, true, true, false, 3u, 0, 1u},
      {"a8-kind-3-destructible", 3u, 1u, true, true, false, 3u, 0, 1u},
      {"a8-kind-4-item", 4u, 1u, true, true, false, 3u, 0, 1u},
      {"a8-kind-5-effect", 5u, 1u, true, true, false, 3u, 0, 1u},
      {"a8-kind-255", 255u, 1u, true, true, false, 3u, 0, 1u},
      {"a5-hash-identity", 1u, 0u, true, false, true, 0u, -1, 0u},
  };
  for (const M24ProbeSpec& spec : specs)
    PrintM24ProbeRow(spec);
  return g_failures == 0u ? 0 : 1;
}



// ===========================================================================
// A9（2026-09-18 死代码裁定）：War3SemanticBuildWorldPaletteIfNeeded
//   —— model-local palette → world-space 组合参考实现（原 device.cpp :7161-7175
//   的 [[maybe_unused]] 死代码，裁定为"迁移而非删除"，正文逐字节迁入
//   war3/semantic/war3_live_palette_selection.cpp）。
//   legacy 参考见 war3_live_palette_selection_a9_legacy_reference.inc（生成器
//   --a9 产出；include 在 M2-4 .inc 之后 ⇒ 正文内无限定的 A4 调用解析到 M2-4
//   legacy 副本，因此本电池覆盖 A9 本体 + **其调用链的 A4 合同语义**）。
// 比较口径：每个输入对同一 (palette, worldTransform, objectKind) 各调用一次，
//   比较输出 vector 的元素个数、逐字节内容（16 个 float 位模式/元素）与整个
//   结果结构的 memcmp；每个场景段结束再对两侧累计结构做一次 memcmp。容差 = 0。
// ===========================================================================

struct A9Result {
  uint64_t count;
  uint64_t hash;
};

A9Result g_a9ModuleAcc = {};
A9Result g_a9LegacyAcc = {};

uint64_t A9HashPalette(const std::vector<dxvk::Matrix4>& palette) {
  uint64_t hash = dxvk::bit::fnv1a_init();
  for (const dxvk::Matrix4& matrix : palette) {
    for (uint32_t r = 0u; r < 4u; ++r) {
      hash = dxvk::bit::fnv1a_iter(hash, FloatBits(matrix[r].x));
      hash = dxvk::bit::fnv1a_iter(hash, FloatBits(matrix[r].y));
      hash = dxvk::bit::fnv1a_iter(hash, FloatBits(matrix[r].z));
      hash = dxvk::bit::fnv1a_iter(hash, FloatBits(matrix[r].w));
    }
  }
  return hash;
}

A9Result A9Run(bool useLegacy, const std::vector<dxvk::Matrix4>& palette,
               const dxvk::Matrix4& worldTransform, uint8_t objectKind,
               std::vector<dxvk::Matrix4>& outPalette) {
  if (useLegacy)
    legacy::War3SemanticBuildWorldPaletteIfNeeded(
        palette, worldTransform, objectKind, outPalette);
  else
    sem::War3SemanticBuildWorldPaletteIfNeeded(
        palette, worldTransform, objectKind, outPalette);
  A9Result out = {};
  out.count = uint64_t(outPalette.size());
  out.hash = A9HashPalette(outPalette);
  return out;
}

void A9Accumulate(const A9Result& m, const A9Result& l) {
  g_a9ModuleAcc.count += m.count;
  g_a9ModuleAcc.hash += m.hash;
  g_a9LegacyAcc.count += l.count;
  g_a9LegacyAcc.hash += l.hash;
}

void A9ResetAccumulators() {
  g_a9ModuleAcc = A9Result{};
  g_a9LegacyAcc = A9Result{};
}

void DiffA9Case(const char* what, const std::vector<dxvk::Matrix4>& palette,
                const dxvk::Matrix4& worldTransform, uint8_t objectKind,
                int line) {
  std::vector<dxvk::Matrix4> moduleOut;
  std::vector<dxvk::Matrix4> legacyOut;
  const A9Result m =
      A9Run(false, palette, worldTransform, objectKind, moduleOut);
  const A9Result l =
      A9Run(true, palette, worldTransform, objectKind, legacyOut);
  ++g_checks;
  if (std::memcmp(&m, &l, sizeof(A9Result)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what
              << ": A9Result memcmp mismatch\n";
  }
  DiffU64("A9.count", m.count, l.count, line);
  DiffU64("A9.hash", m.hash, l.hash, line);
  ++g_checks;
  if (moduleOut.size() != legacyOut.size() ||
      (!moduleOut.empty() &&
       std::memcmp(moduleOut.data(), legacyOut.data(),
                   moduleOut.size() * sizeof(dxvk::Matrix4)) != 0)) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what
              << ": A9 outPalette bytes mismatch\n";
  }
  A9Accumulate(m, l);
}

void DiffA9StructBytes(const char* what, int line) {
  ++g_checks;
  if (std::memcmp(&g_a9ModuleAcc, &g_a9LegacyAcc, sizeof(A9Result)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " A9-acc " << what << "\n";
  }
}

void A9Prepare(const std::vector<dxvk::Matrix4>& palette, bool readable) {
  ClearReadable();
  if (readable && !palette.empty())
    RegisterReadable(palette.data(), palette.size() * sizeof(dxvk::Matrix4));
}

std::vector<dxvk::Matrix4> A9Palette(uint32_t variant) {
  if (variant <= 6u)
    return M24Palette(variant);
  std::vector<dxvk::Matrix4> palette;
  const uint32_t size = variant == 7u ? 256u : 257u;
  for (uint32_t i = 0u; i < size; ++i)
    palette.push_back(M24Matrix(64.0f + float(i), 0.0f, 0.0f));
  return palette;
}

void DiffA9Systematic() {
  A9ResetAccumulators();
  for (uint32_t variant = 0u; variant < 9u; ++variant) {
    const std::vector<dxvk::Matrix4> palette = A9Palette(variant);
    for (uint32_t readable = 0u; readable < 2u; ++readable) {
      A9Prepare(palette, readable != 0u);
      for (uint32_t worldIdx = 0u; worldIdx < 8u; ++worldIdx) {
        const dxvk::Matrix4 world = M24World(worldIdx);
        for (uint32_t kind = 0u; kind < 256u; ++kind) {
          DiffA9Case("A9Grid", palette, world, uint8_t(kind), __LINE__);
        }
      }
    }
  }
  DiffA9StructBytes("systematic-grid", __LINE__);
}

void DiffA9Random() {
  A9ResetAccumulators();
  std::vector<dxvk::Matrix4> pool(12u);
  std::vector<dxvk::Matrix4> wide;
  wide.reserve(257u);
  for (uint32_t i = 0u; i < 257u; ++i)
    wide.push_back(M24Matrix(64.0f + float(i), 0.0f, 0.0f));
  for (uint32_t round = 0u; round < 200000u; ++round) {
    const uint32_t roll = NextU32();
    for (auto& matrix : pool)
      FillMatrixRandom(matrix, (roll & 1u) != 0u);
    dxvk::Matrix4 randomWorld;
    FillMatrixRandom(randomWorld, (roll & 2u) != 0u);
    const dxvk::Matrix4 world =
        (roll & 4u) != 0u ? M24World(roll % 9u) : randomWorld;
    const uint32_t size = roll % 13u;
    const std::vector<dxvk::Matrix4> view(pool.begin(), pool.begin() + size);
    const std::vector<dxvk::Matrix4>& palette =
        (roll & 64u) != 0u ? wide : view;
    A9Prepare(palette, (roll & 8u) != 0u);
    DiffA9Case("A9Random", palette, world, uint8_t(NextU32() & 0xFFu),
               __LINE__);
  }
  DiffA9StructBytes("random", __LINE__);
}

// ---- --probe-a9：固定场景的 module/legacy 逐字段对照（绝对值），由独立门禁
//      与门禁内**独立实现**的 A4/A9 语义推导核对 -------------------------
struct A9ProbeSpec {
  const char* name;
  uint32_t worldVariant;
  uint32_t paletteVariant;
  uint8_t kind;
  bool registerReadable;
};

void PrintA9ProbeRow(const A9ProbeSpec& spec) {
  const std::vector<dxvk::Matrix4> palette = A9Palette(spec.paletteVariant);
  A9Prepare(palette, spec.registerReadable);
  const dxvk::Matrix4 world = M24World(spec.worldVariant);
  std::vector<dxvk::Matrix4> moduleOut;
  std::vector<dxvk::Matrix4> legacyOut;
  const A9Result m = A9Run(false, palette, world, spec.kind, moduleOut);
  const A9Result l = A9Run(true, palette, world, spec.kind, legacyOut);
  ++g_checks;
  if (std::memcmp(&m, &l, sizeof(A9Result)) != 0) {
    ++g_failures;
    std::cerr << "DIFF probe-a9 " << spec.name << ": module/legacy\n";
  }
  std::cout << "probe-a9 " << spec.name << " module=" << m.count << ","
            << m.hash << " legacy=" << l.count << "," << l.hash << "\n";
}

int RunProbeA9() {
  const A9ProbeSpec specs[] = {
      {"a9-local-world-far", 3u, 1u, 1u, true},
      {"a9-palette-too-far", 3u, 4u, 1u, true},
      {"a9-palette-nan", 3u, 3u, 1u, true},
      {"a9-palette-inf", 3u, 5u, 1u, true},
      {"a9-world-zero", 0u, 1u, 1u, true},
      {"a9-world-mag-16-0008", 1u, 1u, 1u, true},
      {"a9-world-mag-exactly-16", 2u, 1u, 1u, true},
      {"a9-world-nan", 5u, 1u, 1u, true},
      {"a9-world-inf", 6u, 1u, 1u, true},
      {"a9-world-5-palette-500", 7u, 6u, 1u, true},
      {"a9-four-entries", 3u, 2u, 1u, true},
      {"a9-kind-0-unknown", 3u, 1u, 0u, true},
      {"a9-kind-2-building", 3u, 1u, 2u, true},
      {"a9-empty-palette", 3u, 0u, 1u, true},
      {"a9-count-256", 3u, 7u, 1u, true},
      {"a9-count-257", 3u, 8u, 1u, true},
      {"a9-unreadable", 3u, 1u, 1u, false},
  };
  for (const A9ProbeSpec& spec : specs)
    PrintA9ProbeRow(spec);
  return g_failures == 0u ? 0 : 1;
}

// ===========================================================================
// M2-5B（2026-09-18，M2-5 余项 B4）：per-frame submitted skinned palette 聚合。
//
// 被测函数 = dxvk::war3::semantic::War3AggregateSubmittedSkinnedPalette(
//     dxvk::War3ShadowCaptureStats& st, bool skinned)
// （定义在 war3/semantic/war3_palette_submitted_aggregation.cpp，逐字节迁自
// pre-M2-5B device.cpp :22200-22244）。
//
// 输出面 = 7 个 stats 字段；**每个用例**除逐字段整数精确相等外还对整个
// War3ShadowCaptureStats 做一次 memcmp（捕获「写到了这 7 个字段之外」）。
// 两侧各持独立结构，容差 = 0。
// ===========================================================================
namespace {

static_assert(std::is_trivially_copyable<dxvk::War3ShadowCaptureStats>::value,
              "M2-5B battery requires a trivially copyable stats struct");

struct M25bResult {
  uint64_t zero;
  uint64_t first;
  uint64_t combined;
  uint64_t distinct;
  uint64_t runLast;
  uint64_t run;
  uint64_t max;
};

M25bResult M25bExtract(const dxvk::War3ShadowCaptureStats& st) {
  M25bResult out;
  out.zero = st.semanticSceneSubmittedSkinnedPaletteZeroHashCount;
  out.first = st.semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash;
  out.combined = st.semanticSceneSubmittedSkinnedPaletteCombinedHash;
  out.distinct = st.semanticSceneSubmittedSkinnedPaletteDistinctSampleCount;
  out.runLast = st.semanticSceneSubmittedSkinnedPaletteRunningLastHash;
  out.run = st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun;
  out.max = st.semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax;
  return out;
}

struct M25bSeed {
  uint64_t lastSubmitted;
  uint64_t first;
  uint64_t combined;
  uint64_t runLast;
  uint32_t run;
  uint32_t distinct;
  uint32_t max;
  uint32_t zero;
};

void M25bFill(dxvk::War3ShadowCaptureStats& st, const M25bSeed& seed) {
  st = dxvk::War3ShadowCaptureStats{};
  st.semanticSceneDirectLastSubmittedPaletteHash = seed.lastSubmitted;
  st.semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash = seed.first;
  st.semanticSceneSubmittedSkinnedPaletteCombinedHash = seed.combined;
  st.semanticSceneSubmittedSkinnedPaletteRunningLastHash = seed.runLast;
  st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun = seed.run;
  st.semanticSceneSubmittedSkinnedPaletteDistinctSampleCount = seed.distinct;
  st.semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax = seed.max;
  st.semanticSceneSubmittedSkinnedPaletteZeroHashCount = seed.zero;
}

void DiffM25bStruct(const char* what, const dxvk::War3ShadowCaptureStats& m,
                    const dxvk::War3ShadowCaptureStats& l, int line) {
  ++g_checks;
  if (std::memcmp(&m, &l, sizeof(dxvk::War3ShadowCaptureStats)) != 0) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what
              << ": M2-5B stats memcmp mismatch\n";
  }
}

void DiffM25bFields(const char* what, const dxvk::War3ShadowCaptureStats& m,
                    const dxvk::War3ShadowCaptureStats& l, int line) {
  const M25bResult mr = M25bExtract(m);
  const M25bResult lr = M25bExtract(l);
  DiffU64("M25B.ZeroHash", mr.zero, lr.zero, line);
  DiffU64("M25B.FirstSubmittedHash", mr.first, lr.first, line);
  DiffU64("M25B.CombinedHash", mr.combined, lr.combined, line);
  DiffU64("M25B.DistinctSampleCount", mr.distinct, lr.distinct, line);
  DiffU64("M25B.RunningLastHash", mr.runLast, lr.runLast, line);
  DiffU64("M25B.RunningSameHashRun", mr.run, lr.run, line);
  DiffU64("M25B.ConsecutiveSameHashCountMax", mr.max, lr.max, line);
}

void DiffM25bCase(const char* what, const M25bSeed& seed, bool skinned,
                  int line) {
  dxvk::War3ShadowCaptureStats m;
  dxvk::War3ShadowCaptureStats l;
  M25bFill(m, seed);
  M25bFill(l, seed);
  sem::War3AggregateSubmittedSkinnedPalette(m, skinned);
  legacy::War3AggregateSubmittedSkinnedPalette(l, skinned);
  DiffM25bStruct(what, m, l, line);
  DiffM25bFields(what, m, l, line);
}

void DiffM25bSystematic() {
  static const uint64_t kHashes[] = {
      0u,
      1u,
      0xFFFFFFFFFFFFFFFFull,
      0x9E3779B97F4A7C15ull,
      0x8000000000000000ull,
      0x00000000FFFFFFFFull,
      0xFFFFFFFF00000000ull,
      0x0123456789ABCDEFull};
  static const uint64_t kFirstSeeds[] = {0u, 0x2222ull};
  static const uint64_t kPrev[] = {0u, 0x1111ull, 0x9E3779B97F4A7C15ull};
  static const uint32_t kRunSeeds[] = {0u, 1u, 2u, 9u};
  static const uint32_t kDistinctSeeds[] = {0u, 3u};
  static const uint32_t kMaxSeeds[] = {0u, 1u, 4u, 9u};
  static const uint32_t kZeroSeeds[] = {0u, 5u};
  static const bool kSkinned[] = {false, true};
  for (uint64_t last : kHashes)
    for (uint64_t first : kFirstSeeds)
      for (uint64_t combined : kPrev)
        for (uint64_t runLast : kPrev)
          for (uint32_t run : kRunSeeds)
            for (uint32_t distinct : kDistinctSeeds)
              for (uint32_t max : kMaxSeeds)
                for (uint32_t zero : kZeroSeeds)
                  for (bool skinned : kSkinned) {
                    const M25bSeed seed{last, first, combined, runLast, run,
                                        distinct, max, zero};
                    DiffM25bCase("M25BGrid", seed, skinned, __LINE__);
                  }
}

// 定长序列：同 key 连续帧 / 交替 key / 零 key 注入 / run 增长后切换 /
// 以及诊断门内的“整帧所有 caster 同一套 palette”滚动合并路径。
void DiffM25bSequences() {
  static const uint64_t kSequence[] = {
      0x1000u, 0x1000u, 0x1000u, 0x1000u, 0x1000u, 0x1000u, 0x1000u,
      0x1000u, 0x1000u, 0x1000u, 0x2000u, 0x1000u, 0x2000u, 0x1000u,
      0x2000u, 0u,       0u,       0x3000u, 0x3000u, 0x3000u, 0x3000u,
      0xFFFFFFFFFFFFFFFFull, 0x3000u, 0x9E3779B97F4A7C15ull,
      0x9E3779B97F4A7C15ull, 0x9E3779B97F4A7C15ull, 0u, 0x4000u,
      0x4000u, 0x4000u, 0x4000u, 0x4000u, 0x4000u, 0x4000u, 0x4000u,
      0x4000u, 0x4000u, 0x4000u, 0x4000u, 0x4000u, 0x4000u, 0x4000u,
      0x4000u, 0x4000u};
  dxvk::War3ShadowCaptureStats m;
  dxvk::War3ShadowCaptureStats l;
  const M25bSeed fresh{0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
  M25bFill(m, fresh);
  M25bFill(l, fresh);
  for (uint64_t hash : kSequence) {
    m.semanticSceneDirectLastSubmittedPaletteHash = hash;
    l.semanticSceneDirectLastSubmittedPaletteHash = hash;
    sem::War3AggregateSubmittedSkinnedPalette(m, true);
    legacy::War3AggregateSubmittedSkinnedPalette(l, true);
    DiffM25bStruct("M25BSeq", m, l, __LINE__);
    DiffM25bFields("M25BSeq", m, l, __LINE__);
  }
}

constexpr uint32_t kM25bRandomSteps = 150000u;

void DiffM25bRandom() {
  static const uint64_t kPool[] = {
      0u,
      1u,
      0x9E3779B97F4A7C15ull,
      0xFFFFFFFFFFFFFFFFull,
      0x0123456789ABCDEFull,
      0x8000000000000000ull,
      0x0000000100000000ull,
      0x0000000000000001ull};
  dxvk::War3ShadowCaptureStats m;
  dxvk::War3ShadowCaptureStats l;
  const M25bSeed fresh{0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
  M25bFill(m, fresh);
  M25bFill(l, fresh);
  for (uint32_t step = 0u; step < kM25bRandomSteps; ++step) {
    uint64_t hash = (uint64_t(NextU32()) << 32u) | uint64_t(NextU32());
    const uint32_t roll = NextU32() % 13u;
    if (roll == 0u)
      hash = 0u;
    else if (roll <= 4u)
      hash = kPool[NextU32() % (sizeof(kPool) / sizeof(kPool[0]))];
    const bool skinned = (NextU32() & 1u) != 0u;
    m.semanticSceneDirectLastSubmittedPaletteHash = hash;
    l.semanticSceneDirectLastSubmittedPaletteHash = hash;
    sem::War3AggregateSubmittedSkinnedPalette(m, skinned);
    legacy::War3AggregateSubmittedSkinnedPalette(l, skinned);
    DiffM25bStruct("M25BRand", m, l, __LINE__);
    DiffM25bFields("M25BRand", m, l, __LINE__);
  }
}

// ---- --probe-m2-5b：固定场景的绝对量打印（module vs legacy 全部 7 字段），
//      由独立等价门禁按自己独立实现的 FNV-1a 与场景推导逐行核对。
struct M25bProbeSpec {
  const char* name;
  M25bSeed seed;
  bool skinned;
};

const M25bProbeSpec kM25bProbes[] = {
    {"probe-none", {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}, false},
    {"probe-fresh-zero", {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}, true},
    {"probe-fresh-nonzero", {0x1111u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}, true},
    {"probe-seeded-repeat",
     {0x1111u, 0x1111u, 0x9999u, 0x1111u, 1u, 1u, 1u, 0u}, true},
    {"probe-seeded-change",
     {0x2222u, 0x1111u, 0x9999u, 0x1111u, 3u, 5u, 7u, 2u}, true},
    {"probe-max-growth", {0x1111u, 0x1111u, 0x9999u, 0x1111u, 3u, 3u, 3u, 0u},
     true},
    {"probe-max-hold", {0x1111u, 0x1111u, 0x9999u, 0x1111u, 2u, 2u, 7u, 0u},
     true},
    {"probe-first-set", {0x1111u, 0x3333u, 0u, 0x1111u, 0u, 0u, 0u, 0u}, true},
    {"probe-zero-seeded",
     {0u, 0x1111u, 0x9999u, 0x1111u, 3u, 4u, 6u, 1u}, true},
    {"probe-fnv-lo-hi",
     {0x0000000100000000ull, 0x1111u, 0xCBF29CE484222325ull,
      0x0000000000000001ull, 2u, 5u, 5u, 0u},
     true},
    {"probe-huge-hash",
     {0xFFFFFFFFFFFFFFFFull, 0x1111u, 0x9E3779B97F4A7C15ull,
      0xFFFFFFFFFFFFFFFFull, 4u, 9u, 12u, 3u},
     true},
    {"probe-skinned-off",
     {0x1111u, 0x1111u, 0x9999u, 0x1111u, 3u, 5u, 7u, 2u}, false},
};

int RunProbeM25b() {
  for (const M25bProbeSpec& spec : kM25bProbes) {
    dxvk::War3ShadowCaptureStats m;
    dxvk::War3ShadowCaptureStats l;
    M25bFill(m, spec.seed);
    M25bFill(l, spec.seed);
    sem::War3AggregateSubmittedSkinnedPalette(m, spec.skinned);
    legacy::War3AggregateSubmittedSkinnedPalette(l, spec.skinned);
    const M25bResult mr = M25bExtract(m);
    const M25bResult lr = M25bExtract(l);
    std::cout << "probe-m2-5b " << spec.name << " module=" << mr.zero << ","
              << mr.first << "," << mr.combined << "," << mr.distinct << ","
              << mr.runLast << "," << mr.run << "," << mr.max << " legacy="
              << lr.zero << "," << lr.first << "," << lr.combined << ","
              << lr.distinct << "," << lr.runLast << "," << lr.run << ","
              << lr.max << "\n";
  }
  return 0;
}

} // namespace


int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--probe")
    return RunProbe();
  if (argc >= 2 && std::string(argv[1]) == "--probe-chain")
    return RunProbeChain();
  if (argc >= 2 && std::string(argv[1]) == "--probe-motion")
    return RunProbeMotion();
  if (argc >= 2 && std::string(argv[1]) == "--probe-taxonomy")
    return RunProbeTaxonomy();
  if (argc >= 2 && std::string(argv[1]) == "--probe-m2-4")
    return RunProbeM24();
  if (argc >= 2 && std::string(argv[1]) == "--probe-a9")
    return RunProbeA9();
  if (argc >= 2 && std::string(argv[1]) == "--probe-m2-5b")
    return RunProbeM25b();

  DiffHashMatrixPalette();
  DiffDecodeRuntimePoseMatrix48();
  DiffTryReadRuntimePoseArray();
  DiffResolveLivePoseRuntimeAlias();
  DiffEnvGettersCurrentEnv();
  DiffChainSystematic();
  DiffChainRandom();
  DiffMotionSystematic();
  DiffMotionRandom();
  DiffTaxonomySystematic();
  DiffTaxonomySequences();
  DiffTaxonomyRandom();

  // M2-4 电池的独立检查数（差分被删成空壳时该数字会掉下来；门禁按登记下限
  // 咬住）。env getter 的两条 DIFF_U64 在 DiffEnvGettersCurrentEnv 内，不计入
  // 这里。
  const uint32_t checksBeforeM24 = g_checks;
  const uint32_t failuresBeforeM24 = g_failures;
  DiffM24Systematic();
  DiffM24Random();
  std::cout << "M2-4 battery: " << (g_checks - checksBeforeM24) << " checks, "
            << (g_failures - failuresBeforeM24) << " failures\n";

  // A9（2026-09-18 死代码裁定）电池的独立检查数（差分被删成空壳时该数字会
  // 掉下来；门禁按登记精确值 + 下限双重咬合）。
  const uint32_t checksBeforeA9 = g_checks;
  const uint32_t failuresBeforeA9 = g_failures;
  DiffA9Systematic();
  DiffA9Random();
  std::cout << "A9 battery: " << (g_checks - checksBeforeA9) << " checks, "
            << (g_failures - failuresBeforeA9) << " failures\n";

  // M2-5B（清册表 B 的 B4）电池的独立检查数（差分被删成空壳时该数字会掉下来；
  // 门禁按登记精确值 + 下限双重咬合）。
  const uint32_t checksBeforeM25b = g_checks;
  const uint32_t failuresBeforeM25b = g_failures;
  DiffM25bSystematic();
  DiffM25bSequences();
  DiffM25bRandom();
  std::cout << "M2-5B battery: " << (g_checks - checksBeforeM25b) << " checks, "
            << (g_failures - failuresBeforeM25b) << " failures\n";

  std::cout << "war3 live palette selection equivalence: "
            << (g_checks - g_failures) << "/" << g_checks
            << " checks passed\n";
  return g_failures == 0u ? 0 : 1;
}
