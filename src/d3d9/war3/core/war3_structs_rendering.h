#pragma once
#include <cstdint>

namespace dxvk {
namespace war3 {

// 基于 IDA/War3Class.txt 与运行时调试确认的 VTable 地址 (1.27a)
// CGeoset    VTable: 0x6F96170C
// CModel     VTable: 0x6F96173C
// CSpriteUber VTable: 0x6F9647BC
// CSpriteMini VTable: 0x6F9646F4

// 前向声明
struct CModel;

struct CGeoset {
  void *vtable; // 0x00
                // 目前未知内部结构，为了安全起见暂时不定义具体的成员
                // 之前猜测 +0x14 是 CModel*，但验证失败。
                // 待后续通过 CE + thisPtr 分析填充
};

struct CModel {
  void *vtable; // 0x00
                // +0x10, +0x14, +0x18 等看起来有数据，但含义未知
                // 可能是 Hierarchy 节点指针
};

struct WorldObjectEntry {
  // 根据 SceneCollector 日志确认：
  // entry=... sceneNode(+0x20)=...
  uint8_t pad_00[0x20];
  CModel *sceneNode; // +0x20

  // 可能还有 unitPtr 等信息，待挖掘
};

} // namespace war3
} // namespace dxvk
