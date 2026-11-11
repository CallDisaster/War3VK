// WarVK 内置模块命令面单测。
//
// 只验证"协议层"行为：前缀识别、控制命令、参数校验、错误映射。
// 真实 DLL 的加载/ABI 校验属于集成测试，需要 32 位模块二进制，不放在这里。
//
// 构建：#include 与 meson 目标见 src/d3d9/meson.build 的
//       war3_module_protocol_test。

#include "../src/d3d9/war3/japi/war3_japi_v1.h"
#include "../src/d3d9/war3/japi/war3_module_loader.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool condition, const char* label) {
  if (!condition) {
    ++g_failures;
    std::printf("FAIL %s\n", label);
  }
}

void CheckEqual(int32_t actual, int32_t expected, const char* label) {
  if (actual != expected) {
    ++g_failures;
    std::printf("FAIL %s (actual=%d expected=%d)\n", label,
                static_cast<int>(actual), static_cast<int>(expected));
  }
}

}  // namespace

int main() {
  using namespace dxvk::war3::japi;
  namespace modules = dxvk::war3::japi::modules;

  // 1. 前缀识别：只认 module.* 与已登记模块前缀。
  Check(modules::IsModulePayload("warvk:v1;module.count"),
        "module.count 应被识别");
  Check(modules::IsModulePayload("warvk:v1;module.load;s:abc"),
        "module.load 应被识别");
  Check(!modules::IsModulePayload("warvk:v1;sun.setEnabled;b:1"),
        "内置命令不应被模块层接管");
  Check(!modules::IsModulePayload("warvk:v1;wvk_sample.add;i:1;i:2"),
        "未加载模块的命令不应被接管");
  Check(!modules::IsModulePayload("notwarvk:module.count"),
        "非 warvk 前缀应忽略");

  // 2. 初始状态：0 个模块。
  {
    Reply reply = modules::Dispatch(Carrier::Hotkey, "warvk:v1;module.count");
    Check(reply.ok(), "module.count 应成功");
    CheckEqual(reply.integer, 0, "初始模块数应为 0");
  }

  // 3. 加载不存在的模块必须失败，且被模块层消费（不再落到内置命令表）。
  {
    Reply reply =
        modules::Dispatch(Carrier::Preloader, "warvk:v1;module.load;s:nosuchmod");
    Check(reply.consumed, "失败也应被消费");
    Check(!reply.ok(), "不存在的模块必须加载失败");
    Check(reply.error == ErrorCode::BackendRejected, "错误码应为 BackendRejected");
  }

  // 4. 失败原因可读回。
  {
    Reply reply =
        modules::Dispatch(Carrier::LocalizedString, "warvk:v1;module.lastError");
    Check(reply.ok(), "module.lastError 应成功");
    Check(!reply.text.empty(), "失败后应留下可读错误信息");
  }

  // 5. 参数数量校验。
  {
    Reply reply =
        modules::Dispatch(Carrier::LocalizedString, "warvk:v1;module.load");
    Check(!reply.ok(), "缺参必须失败");
    Check(reply.error == ErrorCode::ArgumentCountMismatch, "错误码应为参数数量不符");
  }

  // 6. 越界索引。
  {
    Reply reply = modules::Dispatch(Carrier::LocalizedString,
                                    "warvk:v1;module.info;i:5");
    Check(!reply.ok(), "越界索引必须失败");
    Check(reply.error == ErrorCode::InvalidId, "错误码应为 InvalidId");
  }

  // 7. 未知 module.* 子命令。
  {
    Reply reply =
        modules::Dispatch(Carrier::Preloader, "warvk:v1;module.nonsense");
    Check(!reply.ok(), "未知子命令必须失败");
    Check(reply.error == ErrorCode::UnknownCommand, "错误码应为 UnknownCommand");
  }

  // 8. 非法载荷不得被吞掉：调用方应继续走原协议。
  {
    Reply reply = modules::Dispatch(Carrier::Preloader, "warvk:v1;sun.setEnabled");
    Check(!reply.consumed, "非模块载荷不应被消费");
  }

  // 9. Reset 幂等。
  modules::ResetAuthorState();
  modules::ResetAuthorState();
  CheckEqual(static_cast<int32_t>(modules::LoadedCount()), 0,
             "Reset 后模块数应为 0");

  if (g_failures == 0) {
    std::printf("war3_module_protocol: all checks passed\n");
    return 0;
  }
  std::printf("war3_module_protocol: %d failures\n", g_failures);
  return 1;
}
