// war3_native_structs.h - Native 结构定义桥接
// 目的：将 native 目录中的结构定义“转正”并对外暴露
// 说明：native 目录仅还原游戏本身结构与函数，不在此追加额外逻辑

#pragma once

#include "war3/native/war3_native_renderer.h"

namespace dxvk::war3::native {
using namespace ::war3::native;
}
