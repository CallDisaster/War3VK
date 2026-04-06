#include "war3_storm.h"
#include <iostream>
#include <cstdint>

namespace dxvk::war3 {

    War3Storm& War3Storm::get() {
        static War3Storm instance;
        return instance;
    }

    void War3Storm::init(HMODULE hStorm) {
        if (m_hStorm) return;
        m_hStorm = hStorm;
        
        if (!m_hStorm) return;
        
        // Load by ordinal
        m_pSFileLoadFile = (SFileLoadFile_t)GetProcAddress(m_hStorm, (LPCSTR)279);
        m_pSFileUnloadFile = (SFileUnloadFile_t)GetProcAddress(m_hStorm, (LPCSTR)280);
        m_pSFileExists = (SFileExists_t)GetProcAddress(m_hStorm, (LPCSTR)288); // 288 is SFileExists
    }

    bool War3Storm::exists(const std::string& path) {
        if (!m_pSFileExists) return false;
        return m_pSFileExists(path.c_str());
    }

    bool War3Storm::loadFile(const std::string& path, std::vector<uint8_t>& buffer) {
        if (!m_pSFileLoadFile || !m_pSFileUnloadFile) return false;

        void* ptr = nullptr;
        uint32_t size = 0;
        
        if (m_pSFileLoadFile(path.c_str(), &ptr, &size, 0, nullptr)) {
            if (ptr && size > 0) {
                buffer.resize(size);
                memcpy(buffer.data(), ptr, size);
                m_pSFileUnloadFile(ptr);
                return true;
            }
            if (ptr) m_pSFileUnloadFile(ptr);
        }
        return false;
    }

}
