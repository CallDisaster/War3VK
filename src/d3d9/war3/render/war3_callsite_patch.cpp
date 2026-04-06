#include "war3_callsite_patch.h"
#include "../debug/war3_debug.h"
#include <cstring>

namespace dxvk {
namespace war3 {
namespace render {

CallsitePatch::CallsitePatch() {
    m_backup[0].addr = 0;
    m_backup[1].addr = 0;
    m_applied = false;
}

bool CallsitePatch::ApplyPatch(uintptr_t renderQueueFlushAddr, 
                             uintptr_t trampolineDispatchCommon, 
                             uintptr_t trampolineDispatchSpecial) {
    if (m_applied) {
        WAR3_LOG_DEBUG("CallsitePatch: Already applied\n");
        return false;
    }
    
    // RenderQueue_FlushSortedItems中的两个call指令偏移
    // 这些偏移基于1.27a版本
    constexpr uintptr_t kOffsetCallCommon = 0x17B;  // call Dispatch_Common
    constexpr uintptr_t kOffsetCallSpecial = 0x170; // call Dispatch_Special
    
    const uintptr_t callCommonAddr = renderQueueFlushAddr + kOffsetCallCommon;
    const uintptr_t callSpecialAddr = renderQueueFlushAddr + kOffsetCallSpecial;
    
    WAR3_LOG_DEBUG("CallsitePatch: Patching at 0x%p (Common) and 0x%p (Special)\n",
                   (void*)callCommonAddr, (void*)callSpecialAddr);
    
    // 1. 备份原始指令
    uint8_t opcodeCommon, opcodeSpecial;
    uint32_t offsetCommon, offsetSpecial;
    
    if (!BackupOriginalCall(callCommonAddr, opcodeCommon, offsetCommon)) {
        WAR3_LOG_DEBUG("CallsitePatch: Failed to backup Common call\n");
        return false;
    }
    
    if (!BackupOriginalCall(callSpecialAddr, opcodeSpecial, offsetSpecial)) {
        WAR3_LOG_DEBUG("CallsitePatch: Failed to backup Special call\n");
        return false;
    }
    
    // 2. 验证原始指令确实是call（E8）
    if (opcodeCommon != 0xE8 || opcodeSpecial != 0xE8) {
        WAR3_LOG_DEBUG("CallsitePatch: Invalid opcodes (Common=0x%X, Special=0x%X)\n",
                       opcodeCommon, opcodeSpecial);
        return false;
    }
    
    // 3. 修改call指令指向trampoline
    if (!PatchCallInstruction(callCommonAddr, trampolineDispatchCommon)) {
        WAR3_LOG_DEBUG("CallsitePatch: Failed to patch Common call\n");
        return false;
    }
    
    if (!PatchCallInstruction(callSpecialAddr, trampolineDispatchSpecial)) {
        WAR3_LOG_DEBUG("CallsitePatch: Failed to patch Special call\n");
        // 回滚Common patch
        RemovePatch(renderQueueFlushAddr);
        return false;
    }
    
    m_applied = true;
    WAR3_LOG_DEBUG("CallsitePatch: Successfully applied\n");
    return true;
}

bool CallsitePatch::RemovePatch(uintptr_t renderQueueFlushAddr) {
    if (!m_applied)
        return true; // 已经移除或从未应用
    
    constexpr uintptr_t kOffsetCallCommon = 0x17B;
    constexpr uintptr_t kOffsetCallSpecial = 0x170;
    
    const uintptr_t callCommonAddr = renderQueueFlushAddr + kOffsetCallCommon;
    const uintptr_t callSpecialAddr = renderQueueFlushAddr + kOffsetCallSpecial;
    
    // 恢复原始指令
    DWORD oldProtect;
    
    // 恢复Common call
    VirtualProtect((void*)callCommonAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(uint8_t*)callCommonAddr = m_backup[0].opcode;
    *(uint32_t*)(callCommonAddr + 1) = m_backup[0].offset;
    VirtualProtect((void*)callCommonAddr, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)callCommonAddr, 5);
    
    // 恢复Special call
    VirtualProtect((void*)callSpecialAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(uint8_t*)callSpecialAddr = m_backup[1].opcode;
    *(uint32_t*)(callSpecialAddr + 1) = m_backup[1].offset;
    VirtualProtect((void*)callSpecialAddr, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)callSpecialAddr, 5);
    
    m_applied = false;
    WAR3_LOG_DEBUG("CallsitePatch: Removed\n");
    return true;
}

bool CallsitePatch::PatchCallInstruction(uintptr_t callAddr, uintptr_t targetAddr) {
    // 修改内存保护为可读写
    DWORD oldProtect;
    if (!VirtualProtect((void*)callAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        WAR3_LOG_DEBUG("CallsitePatch: VirtualProtect failed at 0x%p\n", (void*)callAddr);
        return false;
    }
    
    // 计算相对偏移：target - (callAddr + 5)
    // +5是因为call指令本身占5字节（E8 + offset32）
    const int32_t relOffset = CalculateRelOffset(callAddr, targetAddr);
    
    // 写入call指令：E8 [relative_offset]
    uint8_t* ptr = (uint8_t*)callAddr;
    ptr[0] = 0xE8; // call opcode
    *(int32_t*)(ptr + 1) = relOffset;
    
    // 恢复内存保护
    DWORD dummy;
    VirtualProtect((void*)callAddr, 5, oldProtect, &dummy);
    
    // 刷新指令缓存
    FlushInstructionCache(GetCurrentProcess(), (void*)callAddr, 5);
    
    WAR3_LOG_DEBUG("CallsitePatch: Patched call at 0x%p -> 0x%p (offset=0x%X)\n",
                   (void*)callAddr, (void*)targetAddr, (uint32_t)relOffset);
    
    return true;
}

bool CallsitePatch::BackupOriginalCall(uintptr_t callAddr, uint8_t& opcode, 
                                         uint32_t& offset) {
    // 验证地址可读
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)callAddr, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) {
        // NOACCESS或纯EXECUTE不可读
        return false;
    }
    
    // 读取5字节call指令
    uint8_t* ptr = (uint8_t*)callAddr;
    opcode = ptr[0];
    offset = *(uint32_t*)(ptr + 1);
    
    // 保存备份
    int backupIdx = (m_backup[0].addr == 0) ? 0 : 1;
    m_backup[backupIdx].addr = callAddr;
    m_backup[backupIdx].opcode = opcode;
    m_backup[backupIdx].offset = offset;
    
    return true;
}

int32_t CallsitePatch::CalculateRelOffset(uintptr_t from, uintptr_t to) const {
    // 相对跳转：target - (from + instruction_length)
    // call指令长度为5字节
    return static_cast<int32_t>(to - from - 5);
}

} // namespace render
} // namespace war3
} // namespace dxvk