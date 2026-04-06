// war3_handle_resolver.cpp - Handle 解析模块实现

#include "war3_handle_resolver.h"
#include "../core/war3_memory.h"
#include "../core/war3_game_structs.h"
#include "../debug/war3_debug.h"
#include <algorithm>
#include <atomic>

namespace dxvk::war3 {

namespace {

bool IsLikelyFourCC(uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t ch = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
        if (ch < 0x20 || ch > 0x7E)
            return false;
    }
    return true;
}

bool LooksLikeUnitPtr(void* ptr) {
    if (!ptr || !IsReadableRange(ptr, CUnitOffsets::Rawcode + sizeof(uint32_t)))
        return false;
    uint32_t raw = 0;
    if (!SafeReadU32(ptr, CUnitOffsets::Rawcode, raw))
        return false;
    return IsLikelyFourCC(raw);
}

} // namespace

HandleResolver& HandleResolver::instance() {
    static HandleResolver s_instance;
    return s_instance;
}

bool HandleResolver::initialize(uintptr_t gameDllBase) {
    if (m_initialized)
        return true;
    
    if (!gameDllBase)
        return false;
    
    m_gameWar3PtrAddr = gameDllBase + GameDllOffsets::GameWar3Ptr;
    
    // 验证指针可读
    if (!IsReadableRange(reinterpret_cast<void*>(m_gameWar3PtrAddr), sizeof(void*)))
        return false;
    
    m_initialized = true;
    return true;
}

uintptr_t HandleResolver::getGameStateBase() const {
    if (!m_initialized || !m_gameWar3PtrAddr)
        return 0;
    
    // CGameWar3* gameWar3 = *(CGameWar3**)(Game.dll + 0xBE4238)
    void* gameWar3 = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(m_gameWar3PtrAddr), 0, gameWar3) || !gameWar3)
        return 0;
    
    // CGameState* gameState = gameWar3->gameState (+0x1C)
    void* gameState = nullptr;
    if (!SafeReadPtr(gameWar3, GameDllOffsets::GameStateOff, gameState) || !gameState)
        return 0;
    
    return reinterpret_cast<uintptr_t>(gameState);
}

uintptr_t HandleResolver::getHandleTableBase() const {
    uintptr_t gameState = getGameStateBase();
    if (!gameState)
        return 0;
    
    // JassHandleTable* table = gameState + 0x194
    return gameState + GameDllOffsets::HandleTableOff;
}

bool HandleResolver::findHandleByUnitPtr(void* unitPtr, uint32_t* outHandleId, void** outAgent) {
    if (!unitPtr || !outHandleId)
        return false;
    
    *outHandleId = 0;
    if (outAgent) *outAgent = nullptr;
    
    uintptr_t tableBase = getHandleTableBase();
    if (!tableBase)
        return tryFallbackHandleId(unitPtr, outHandleId);
    
    // 读取 handle 数组
    void* handleArray = nullptr;
    uint32_t capacity = 0;
    uint32_t size = 0;
    
    if (!SafeReadPtr(reinterpret_cast<void*>(tableBase), HandleTableOffsets::HandleArray, handleArray))
        return tryFallbackHandleId(unitPtr, outHandleId);
    
    if (!SafeReadU32(reinterpret_cast<void*>(tableBase), HandleTableOffsets::Capacity, capacity))
        return tryFallbackHandleId(unitPtr, outHandleId);

    // 注意：1.27a 的 JassHandleNode 为 12 bytes（ref_count + object + pad）。
    // 之前误判为 8 bytes（state + object）会导致 resolveHandle/findHandle 永远失败。
    if (!SafeReadU32(reinterpret_cast<void*>(tableBase), HandleTableOffsets::Size, size))
        size = 0;

    const uint32_t arraySize = (std::max)(capacity, size);
    
    if (!handleArray || arraySize == 0 || arraySize > 0x100000)
        return tryFallbackHandleId(unitPtr, outHandleId);
    
    // 遍历查找（JassHandleNode: ref_count + object + pad）
    constexpr size_t entrySize = sizeof(::dxvk::war3::JassHandleNode);
    const size_t maxScan = std::min(arraySize, 50000u);
    
    uint32_t unitHandleCandidate = 0;
    for (size_t i = 1; i < maxScan; ++i) { // 从 1 开始，0 通常无效
        uintptr_t entryAddr = reinterpret_cast<uintptr_t>(handleArray) + i * entrySize;
        
        if (!IsReadableRange(reinterpret_cast<void*>(entryAddr), entrySize))
            continue;
        
        const ::dxvk::war3::JassHandleNode node =
            *reinterpret_cast<const ::dxvk::war3::JassHandleNode*>(entryAddr);
        if (node.ref_count == 0)
            continue;
        
        void* agentObj = node.object;
        if (!agentObj)
            continue;

        // 兼容：有些版本 handle 表直接存 CUnit 指针
        // 这里不直接返回，继续扫描以寻找真正的 CAgent 条目
        if (agentObj == unitPtr && LooksLikeUnitPtr(agentObj)) {
            if (unitHandleCandidate == 0)
                unitHandleCandidate = static_cast<uint32_t>(i);
            continue;
        }

        if (!IsReadableRange(agentObj, 0x58))
            continue;
        
        // 检查 agent->unitPtr (+0x54) 是否匹配
        void* agentUnitPtr = nullptr;
        if (SafeReadPtr(agentObj, CAgentOffsets::UnitPtr, agentUnitPtr) && agentUnitPtr == unitPtr) {
            *outHandleId = static_cast<uint32_t>(i);
            if (outAgent) *outAgent = agentObj;
            return true;
        }
    }

    if (unitHandleCandidate != 0) {
        *outHandleId = unitHandleCandidate;
        if (outAgent) *outAgent = nullptr;

        static std::atomic<uint32_t> s_unitEntryLog{0};
        if (s_unitEntryLog.fetch_add(1, std::memory_order_relaxed) < 8) {
            WAR3_LOG_DEBUG(
                "HandleResolver: handle[%u] points to CUnit (unitPtr=0x%p)\n",
                static_cast<unsigned>(unitHandleCandidate),
                unitPtr);
        }
        return true;
    }

    return tryFallbackHandleId(unitPtr, outHandleId);
}

bool HandleResolver::tryFallbackHandleId(void* unitPtr, uint32_t* outHandleId) {
    if (!unitPtr || !outHandleId || !IsReadableRange(unitPtr, 0x14))
        return false;
    
    // 使用 CUnit+0x0C 作为 fallback
    uint32_t hash0C = 0, hash10 = 0;
    if (!SafeReadU32(unitPtr, CUnitOffsets::HashId0C, hash0C))
        return false;
    if (!SafeReadU32(unitPtr, CUnitOffsets::HashId10, hash10))
        return false;
    
    // 验证两个 hash 一致且在合理范围内
    if (hash0C == hash10 && hash0C > 0 && hash0C < 0x100000) {
        *outHandleId = hash0C;
        return true;
    }
    
    return false;
}

bool HandleResolver::resolveHandle(uint32_t handleId, uint32_t typeHash, void** outObj) {
    if (!outObj)
        return false;
    
    *outObj = nullptr;
    
    uintptr_t tableBase = getHandleTableBase();
    if (!tableBase)
        return false;
    
    void* handleArray = nullptr;
    uint32_t capacity = 0;
    uint32_t size = 0;
    
    if (!SafeReadPtr(reinterpret_cast<void*>(tableBase), HandleTableOffsets::HandleArray, handleArray))
        return false;
    if (!SafeReadU32(reinterpret_cast<void*>(tableBase), HandleTableOffsets::Capacity, capacity))
        return false;
    if (!SafeReadU32(reinterpret_cast<void*>(tableBase), HandleTableOffsets::Size, size))
        size = 0;

    const uint32_t arraySize = (std::max)(capacity, size);
    
    if (!handleArray || handleId >= arraySize)
        return false;
    
    constexpr size_t entrySize = sizeof(::dxvk::war3::JassHandleNode);
    uintptr_t entryAddr = reinterpret_cast<uintptr_t>(handleArray) + handleId * entrySize;
    
    if (!IsReadableRange(reinterpret_cast<void*>(entryAddr), entrySize))
        return false;
    
    const ::dxvk::war3::JassHandleNode node =
        *reinterpret_cast<const ::dxvk::war3::JassHandleNode*>(entryAddr);
    if (node.ref_count == 0)
        return false;
    
    void* agentObj = node.object;
    if (!agentObj || !IsReadableRange(agentObj, 0x10))
        return false;
    
    // 类型检查（可选）
    if (typeHash != 0) {
        uint32_t objType = 0;
        if (SafeReadU32(agentObj, CAgentOffsets::TypeFourCC, objType) && objType != typeHash)
            return false;
    }
    
    *outObj = agentObj;
    return true;
}

// ============================================================================
// 便捷函数实现
// ============================================================================

bool IsUnitBuilding(void* unitPtr) {
    if (!unitPtr || !IsReadableRange(unitPtr, 0x60))
        return false;
    
    uint32_t flags = 0;
    if (!SafeReadU32(unitPtr, CUnitOffsets::Flags5C, flags))
        return false;
    
    return (flags & UnitFlags5C::Building) != 0;
}

uint32_t GetUnitRawcode(void* unitPtr) {
    if (!unitPtr || !IsReadableRange(unitPtr, 0x34))
        return 0;
    
    uint32_t rawcode = 0;
    SafeReadU32(unitPtr, CUnitOffsets::Rawcode, rawcode);
    return rawcode;
}

const char* GetObjectKindName(void* unitPtr, uint32_t agentTypeHash) {
    if (!unitPtr)
        return "Unknown";
    
    if (!IsReadableRange(unitPtr, 0x60))
        return "NonUnit";
    
    // 检查 agent 类型
    if (agentTypeHash == AgentTypeFourCC::Destructible)
        return "Destructible";
    if (agentTypeHash == AgentTypeFourCC::Item)
        return "Item";
    
    // 检查建筑标志
    uint32_t flags = 0;
    if (SafeReadU32(unitPtr, CUnitOffsets::Flags5C, flags)) {
        if (flags & UnitFlags5C::Building)
            return "Building";
    }
    
    return "Unit";
}

} // namespace dxvk::war3
